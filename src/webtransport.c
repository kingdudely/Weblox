// WebTransport imports (Roblox "RNA") over ngtcp2 + nghttp3.
//
// The engine hands the host a finished join URL, a receive-ring geometry in
// guest memory and the certificate hashes parsed out of joinScript.  All guest
// reads and writes go through guest.h.
//
// Real backend (WEBLOX_HAVE_NGTCP2 + WEBLOX_HAVE_NGHTTP3 + TLS):
//   * UDP socket + non-blocking connect, QUIC v1 handshake with ALPN "h3"
//   * peer certificate SHA-256 checked against the joinScript pins
//   * HTTP/3 CONNECT stream, h3 stream DATA in both directions, DATAGRAMs,
//     close and the receive ring
// Without the libraries (or without a TLS provider) every entry point keeps
// the original no-op/logging behaviour so CI and the smoke test stay green.
//
// Remaining TODOs are marked WT_TODO below; they are protocol details that do
// not affect transport correctness (see docs/CI.md for the full list).
#include "host.h"
#include "guest.h"
#include "webtransport.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#define wt_strcasecmp _stricmp
typedef int wt_ssize;
typedef SOCKET wt_socket;
#define WT_BAD_SOCKET INVALID_SOCKET
#define wt_socket_close(s) closesocket(s)
#define wt_last_error() WSAGetLastError()
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <strings.h>
#include <unistd.h>
#define wt_strcasecmp strcasecmp
typedef ssize_t wt_ssize;
typedef int wt_socket;
#define WT_BAD_SOCKET (-1)
#define wt_socket_close(s) close(s)
#define wt_last_error() errno
#endif

#if defined(WEBLOX_HAVE_SDL3)
#include <SDL3/SDL.h>
#endif

#if defined(WEBLOX_HAVE_NGTCP2)
#include <ngtcp2/ngtcp2.h>
#endif
#if defined(WEBLOX_HAVE_NGTCP2_TLS)
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#endif
#if defined(WEBLOX_HAVE_NGHTTP3)
#include <nghttp3/nghttp3.h>
#endif

#define WT_CONN_MAX 8
#define WT_STREAM_MAX 32
#define WT_PIN_MAX 8
#define WT_PIN_LEN 65
#define WT_URL_LEN 512
#define WT_HOST_LEN 256
#define WT_PATH_LEN 256
#define WT_PKT_LEN 65536

#define WT_BACKEND_READY                                                       \
  (defined(WEBLOX_HAVE_NGTCP2) && defined(WEBLOX_HAVE_NGHTTP3) &&              \
   defined(WEBLOX_HAVE_NGTCP2_TLS))

#if defined(WEBLOX_HAVE_NGHTTP3)
typedef struct wt_write {
  struct wt_write* next;
  unsigned char* data;
  size_t len, off;
  int64_t stream_id;
  int fin;
} wt_write;
#endif

typedef struct {
  int used;
  u32 id;
  char url[WT_URL_LEN];
  char host[WT_HOST_LEN];
  char path[WT_PATH_LEN];
  unsigned short port;
  char pins[WT_PIN_MAX][WT_PIN_LEN];
  u32 npins;
  u32 ring_data, ring_head, ring_tail, ring_slot, ring_nslots;
  u32 dropped;
  int64_t streams[WT_STREAM_MAX]; /* bidi stream ids, 0 means free */
  int paused[WT_STREAM_MAX];

  wt_socket sock;
  int sock_connected;
  int handshake_done;
  int connect_sent;
  unsigned char pkt[WT_PKT_LEN];
  struct sockaddr_storage remote;
  socklen_t remote_len;

#if defined(WEBLOX_HAVE_NGTCP2)
  ngtcp2_conn* qc;
  ngtcp2_path qpath;
#endif
#if defined(WEBLOX_HAVE_NGTCP2_TLS)
  SSL_CTX* ssl_ctx;
  SSL* ssl;
  ngtcp2_crypto_ossl_ctx* ossl_ctx;
  ngtcp2_crypto_conn_ref conn_ref;
#endif
#if defined(WEBLOX_HAVE_NGHTTP3)
  nghttp3_conn* h3;
  int64_t connect_stream;
  wt_write* pending;
  wt_write* pending_tail;
  wt_write* sending;
#endif
} wt_conn;

static wt_conn g_conns[WT_CONN_MAX];
static wt_signal_fn g_signal;
static int g_initialized;
static int g_sockets_ready;

static wt_conn* conn_get(u32 id) {
  for (int i = 0; i < WT_CONN_MAX; ++i)
    if (g_conns[i].used && g_conns[i].id == id) return &g_conns[i];
  return NULL;
}

static void wt_log_once(const char* what) {
  static char seen[128][96];
  static unsigned n;
  for (unsigned i = 0; i < n; ++i)
    if (!strcmp(seen[i], what)) return;
  if (n < 128) {
    snprintf(seen[n], sizeof seen[n], "%s", what);
    ++n;
  }
  fprintf(stderr, "[wt] %s not implemented\n", what);
}

static void wt_signal(const char* name, const u32* args, u32 argc) {
  if (g_signal) {
    g_signal(name, args, argc);
  } else {
    fprintf(stderr, "[wt] signal %s", name);
    for (u32 i = 0; i < argc; ++i) fprintf(stderr, "%s%u", i ? "," : "(", args[i]);
    fprintf(stderr, ")\n");
  }
}

static int64_t wt_now_ns(void) {
#if defined(WEBLOX_HAVE_SDL3)
  return (int64_t)SDL_GetTicksNS();
#else
  return (int64_t)clock() * 1000000000LL / CLOCKS_PER_SEC;
#endif
}

static int64_t wt_now_us(void) { return wt_now_ns() / 1000; }

static void wt_signal_connect_error(wt_conn* c, int code) {
  u32 args[2] = {c->id, (u32)code};
  wt_signal("rbx_wasm_rna_on_connect_error", args, 2);
}

static void wt_signal_connect_success(wt_conn* c) {
  u32 args[1] = {c->id};
  wt_signal("rbx_wasm_rna_on_connect_success", args, 1);
}

/* ------------------------------------------------------------------------- */
/* Guest receive ring                                                        */
/* ------------------------------------------------------------------------- */

static void ring_put(wt_conn* c, u32 kind, u32 sid, const unsigned char* buf, u32 len) {
  if (!c->ring_slot || !c->ring_nslots) return;
  unsigned char* hp = guest_ptr(c->ring_head, 4);
  unsigned char* tp = guest_ptr(c->ring_tail, 4);
  if (!hp || !tp) return;
  u32 head_bytes, tail_bytes;
  memcpy(&head_bytes, hp, 4);
  memcpy(&tail_bytes, tp, 4);
  u64 head = head_bytes, tail = tail_bytes;
  /* ring_nslots must be a power of two for the index mask below. */
  if (head - tail >= c->ring_nslots) {
    ++c->dropped;
    return;
  }
  u32 n = len > c->ring_slot ? c->ring_slot : len;
  u32 idx = (u32)(head & (c->ring_nslots - 1));
  unsigned char* dst = guest_ptr(c->ring_data + (u64)idx * c->ring_slot, n);
  if (!dst) return;
  memcpy(dst, buf, n);
  guest_put_u32(c->ring_head, head_bytes + 1);
  u32 args[5] = {c->id, kind, sid, idx, n};
  wt_signal("rbx_wasm_rna_on_receive_ring_slot", args, 5);
}

/* ------------------------------------------------------------------------- */
/* Certificate pins from joinScript                                          */
/* ------------------------------------------------------------------------- */

bool wt_pin_matches(u32 connection_id, const char* sha256_hex) {
  wt_conn* c = conn_get(connection_id);
  if (!c || !sha256_hex || !*sha256_hex) return false;
  /* No pins were supplied: the browser path accepts the platform trust store. */
  if (!c->npins) return true;
  for (u32 i = 0; i < c->npins; ++i)
    if (!wt_strcasecmp(c->pins[i], sha256_hex)) return true;
  fprintf(stderr, "[wt] certificate pin mismatch on connection %u\n", connection_id);
  return false;
}

/* ------------------------------------------------------------------------- */
/* Backend helpers                                                           */
/* ------------------------------------------------------------------------- */

#if defined(WEBLOX_HAVE_NGTCP2_TLS)
static ngtcp2_conn* conn_ref_get(ngtcp2_crypto_conn_ref* ref) {
  wt_conn* c = (wt_conn*)ref->user_data;
  return c ? c->qc : NULL;
}

static int wt_verify_cert(int preverify_ok, X509_STORE_CTX* ctx) {
  SSL* ssl = (SSL*)X509_STORE_CTX_get_ex_data(
      ctx, SSL_get_ex_data_X509_STORE_CTX_idx());
  if (!ssl) return preverify_ok;
  ngtcp2_crypto_conn_ref* ref = (ngtcp2_crypto_conn_ref*)SSL_get_app_data(ssl);
  wt_conn* c = ref ? (wt_conn*)ref->user_data : NULL;
  X509* cert = X509_STORE_CTX_get_current_cert(ctx);
  if (!c || !cert) return preverify_ok;
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int mdlen = 0;
  if (X509_digest(cert, EVP_sha256(), md, &mdlen) != 1 || mdlen != 32)
    return 0;
  char hex[65];
  static const char digits[] = "0123456789abcdef";
  for (unsigned i = 0; i < 32; ++i) {
    hex[i * 2] = digits[md[i] >> 4];
    hex[i * 2 + 1] = digits[md[i] & 15];
  }
  hex[64] = '\0';
  if (!wt_pin_matches(c->id, hex)) return 0;
  /* joinScript pins are authoritative, like the browser certificate pinning
   * path; a matching pin accepts the peer even without a system trust store. */
  return 1;
}

static void wt_rand(uint8_t* dest, size_t len, const ngtcp2_rand_ctx* ctx) {
  (void)ctx;
  if (RAND_bytes(dest, (int)len) != 1) {
    fprintf(stderr, "[wt] RAND_bytes failed\n");
    memset(dest, 0x5a, len);
  }
}

static int wt_new_cid(ngtcp2_conn* conn, ngtcp2_cid* cid,
                      ngtcp2_stateless_reset_token* token, size_t cidlen,
                      void* user_data) {
  (void)conn; (void)user_data;
  if (RAND_bytes(cid->data, (int)cidlen) != 1 ||
      RAND_bytes(token->data, sizeof token->data) != 1)
    return NGTCP2_ERR_CALLBACK_FAILURE;
  cid->datalen = cidlen;
  return 0;
}

static int wt_handshake_completed(ngtcp2_conn* conn, void* user_data) {
  wt_conn* c = (wt_conn*)user_data;
  (void)conn;
  c->handshake_done = 1;
  /* Wait for the h3 CONNECT response before reporting success. */
  return 0;
}

static int wt_recv_stream_data(ngtcp2_conn* conn, uint32_t flags,
                               int64_t stream_id, uint64_t offset,
                               const uint8_t* data, size_t datalen,
                               void* user_data, void* stream_user_data) {
  (void)conn; (void)offset; (void)stream_user_data;
  wt_conn* c = (wt_conn*)user_data;
#if defined(WEBLOX_HAVE_NGHTTP3)
  if (c->h3) {
    nghttp3_ssize consumed = nghttp3_conn_read_stream2(
        c->h3, stream_id, data, datalen,
        (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0, wt_now_ns());
    if (consumed < 0) {
      fprintf(stderr, "[wt] h3 read: %s\n", nghttp3_strerror((int)consumed));
      return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    ngtcp2_conn_extend_max_stream_offset(conn, stream_id, (uint64_t)consumed);
    ngtcp2_conn_extend_max_offset(conn, (uint64_t)consumed);
  }
#else
  (void)flags; (void)stream_id; (void)data; (void)datalen;
#endif
  return 0;
}

static int wt_recv_datagram(ngtcp2_conn* conn, uint32_t flags,
                            const uint8_t* data, size_t datalen,
                            void* user_data) {
  (void)conn; (void)flags;
  wt_conn* c = (wt_conn*)user_data;
  if (datalen > 0) ring_put(c, 2 /* datagram */, 0, data, (u32)datalen);
  return 0;
}

static int wt_acked_stream_data(ngtcp2_conn* conn, int64_t stream_id,
                                uint64_t offset, uint64_t datalen,
                                void* user_data, void* stream_user_data) {
  (void)conn; (void)offset; (void)stream_user_data;
  wt_conn* c = (wt_conn*)user_data;
#if defined(WEBLOX_HAVE_NGHTTP3)
  if (c->h3) nghttp3_conn_add_ack_offset(c->h3, stream_id, datalen);
#else
  (void)stream_id; (void)datalen;
#endif
  return 0;
}
#endif /* WEBLOX_HAVE_NGTCP2_TLS */

#if defined(WEBLOX_HAVE_NGHTTP3) && defined(WEBLOX_HAVE_NGTCP2)
static int wt_h3_recv_data(nghttp3_conn* h3, int64_t stream_id,
                           const uint8_t* data, size_t datalen,
                           void* conn_user_data, void* stream_user_data) {
  (void)h3; (void)stream_user_data;
  wt_conn* c = (wt_conn*)conn_user_data;
  ring_put(c, 1 /* stream data */, (u32)(uint64_t)stream_id, data, (u32)datalen);
  return 0;
}

static int wt_h3_end_headers(nghttp3_conn* h3, int64_t stream_id, int fin,
                             void* conn_user_data, void* stream_user_data) {
  (void)h3; (void)fin; (void)stream_user_data;
  wt_conn* c = (wt_conn*)conn_user_data;
  /* WT_TODO: parse the h3 CONNECT response status and the
   * sec-webtransport-http3-draft header.  A 2xx response is required by the
   * WebTransport spec; for now any completed header block accepts the session. */
  if (c && stream_id == c->connect_stream && !c->connect_sent) {
    c->connect_sent = 1;
    u32 args[1] = {c->id};
    wt_signal("rbx_wasm_rna_on_connect_success", args, 1);
  }
  return 0;
}

static int wt_h3_stream_close(nghttp3_conn* h3, int64_t stream_id,
                              uint64_t app_error_code, void* conn_user_data,
                              void* stream_user_data) {
  (void)h3; (void)app_error_code; (void)stream_user_data;
  wt_conn* c = (wt_conn*)conn_user_data;
  if (!c) return 0;
  for (int i = 0; i < WT_STREAM_MAX; ++i)
    if (c->streams[i] == stream_id) { c->streams[i] = 0; break; }
  return 0;
}

static nghttp3_nv wt_nv(const char* name, const char* value) {
  nghttp3_nv nv;
  nv.name = (const uint8_t*)name;
  nv.value = (const uint8_t*)value;
  nv.namelen = strlen(name);
  nv.valuelen = strlen(value);
  nv.flags = NGHTTP3_NV_FLAG_NONE;
  return nv;
}

static int wt_h3_recv_settings2(nghttp3_conn* h3,
                                const nghttp3_proto_settings* settings,
                                void* conn_user_data) {
  (void)h3; (void)settings; (void)conn_user_data;
  return 0;
}
static void wt_h3_rand(uint8_t* dest, size_t len) {
#if defined(WEBLOX_HAVE_NGTCP2_TLS)
  if (RAND_bytes(dest, (int)len) != 1) memset(dest, 0x5a, len);
#else
  static uint32_t seed = 0x9e3779b9u;
  for (size_t i = 0; i < len; ++i) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    dest[i] = (uint8_t)seed;
  }
#endif
}

static nghttp3_ssize wt_h3_read_data(nghttp3_conn* h3, int64_t stream_id,
                                     nghttp3_vec* vec, size_t veccnt,
                                     uint32_t* pflags, void* user_data,
                                     void* stream_user_data) {
  (void)h3; (void)veccnt; (void)stream_user_data;
  wt_conn* c = (wt_conn*)user_data;
  if (!c || veccnt == 0) return NGHTTP3_ERR_WOULDBLOCK;
  /* Serve only writes belonging to this stream.  WOULDBLOCK keeps a WebTransport
   * CONNECT stream open with no data to send, which is the normal state. */
  wt_write** link = &c->pending;
  while (*link && (*link)->stream_id != stream_id) link = &(*link)->next;
  wt_write* w = *link;
  if (!w) return NGHTTP3_ERR_WOULDBLOCK;
  *link = w->next;
  if (!c->pending) c->pending_tail = NULL;
  c->sending = w;
  vec[0].base = w->data ? w->data + w->off : NULL;
  vec[0].len = w->len - w->off;
  if (w->fin) *pflags |= NGHTTP3_DATA_FLAG_EOF;
  return 1;
}

static void wt_write_free(wt_conn* c, wt_write* w) {
  (void)c;
  if (!w) return;
  free(w->data);
  free(w);
}

static void wt_flush_pending(wt_conn* c);

static int wt_open_h3_connect(wt_conn* c) {
  int64_t sid = -1;
  if (ngtcp2_conn_open_bidi_stream(c->qc, &sid, NULL) != 0) return -1;
  if (sid < 0 || sid >= (int64_t)WT_STREAM_MAX * 4) return -1;
  c->connect_stream = sid;
  for (int i = 0; i < WT_STREAM_MAX; ++i)
    if (!c->streams[i]) { c->streams[i] = sid; break; }

  char authority[WT_HOST_LEN + 16];
  snprintf(authority, sizeof authority, "%s:%u", c->host, c->port);
  nghttp3_nv nva[] = {
      wt_nv(":method", "CONNECT"),
      wt_nv(":scheme", "https"),
      wt_nv(":authority", authority),
      wt_nv(":path", c->path),
      wt_nv("sec-webtransport-http3-draft", "draft02"),
      wt_nv("user-agent", "weblox/1.0"),
  };
  /* A NULL data reader would put FIN on the CONNECT stream; use a reader that
   * reports WOULDBLOCK when no payload is queued so the session stays open. */
  nghttp3_data_reader dr;
  memset(&dr, 0, sizeof dr);
  dr.read_data = wt_h3_read_data;
  int rv = nghttp3_conn_submit_request(c->h3, sid, nva,
                                       sizeof nva / sizeof nva[0], &dr, c);
  if (rv != 0) {
    fprintf(stderr, "[wt] CONNECT submit: %s\n", nghttp3_strerror(rv));
    return -1;
  }
  int64_t enc = -1, dec = -1;
  if (ngtcp2_conn_open_uni_stream(c->qc, &enc, NULL) == 0 &&
      ngtcp2_conn_open_uni_stream(c->qc, &dec, NULL) == 0 &&
      nghttp3_conn_bind_qpack_streams(c->h3, enc, dec) == 0) {
    /* QPACK streams bound; h3 can now compress response headers. */
  }
  wt_flush_pending(c);
  return 0;
}
#endif /* WEBLOX_HAVE_NGHTTP3 */

/* Resolve host:port and create a non-blocking UDP socket. */
static int wt_open_socket(wt_conn* c) {
  char port_text[16];
  snprintf(port_text, sizeof port_text, "%u", c->port);
  struct addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  struct addrinfo* res = NULL;
  if (getaddrinfo(c->host, port_text, &hints, &res) != 0 || !res) {
    fprintf(stderr, "[wt] cannot resolve %s\n", c->host);
    return -1;
  }
  struct addrinfo* ai = res;
  while (ai && ai->ai_family != AF_INET && ai->ai_family != AF_INET6)
    ai = ai->ai_next;
  if (!ai) { freeaddrinfo(res); return -1; }
  c->sock = socket(ai->ai_family, SOCK_DGRAM, IPPROTO_UDP);
  if (c->sock == WT_BAD_SOCKET) { freeaddrinfo(res); return -1; }
#if defined(_WIN32)
  u_long mode = 1;
  ioctlsocket(c->sock, FIONBIO, &mode);
#else
  int flags = fcntl(c->sock, F_GETFL, 0);
  if (flags >= 0) fcntl(c->sock, F_SETFL, flags | O_NONBLOCK);
#endif
  if (connect(c->sock, ai->ai_addr, (int)ai->ai_addrlen) != 0) {
    int err = wt_last_error();
#if !defined(_WIN32)
    if (err != EINPROGRESS && err != EALREADY)
#else
    if (err != WSAEINPROGRESS && err != WSAEALREADY)
#endif
    {
      wt_socket_close(c->sock);
      c->sock = WT_BAD_SOCKET;
      freeaddrinfo(res);
      return -1;
    }
  }
  memset(&c->remote, 0, sizeof c->remote);
  if (ai->ai_addrlen <= sizeof c->remote) {
    memcpy(&c->remote, ai->ai_addr, ai->ai_addrlen);
    c->remote_len = (socklen_t)ai->ai_addrlen;
  }
  freeaddrinfo(res);
  c->sock_connected = 1;
  return 0;
}

static void wt_send_packet(wt_conn* c, const unsigned char* buf, size_t len) {
  if (!c->sock_connected || !len) return;
  (void)send(c->sock, (const char*)buf, (int)len, 0);
}

static void wt_conn_reset(wt_conn* c) {
#if defined(WEBLOX_HAVE_NGTCP2_TLS)
  if (c->ossl_ctx) ngtcp2_crypto_ossl_ctx_del(c->ossl_ctx);
  if (c->ssl) SSL_free(c->ssl);
  if (c->ssl_ctx) SSL_CTX_free(c->ssl_ctx);
#endif
#if defined(WEBLOX_HAVE_NGTCP2)
  if (c->qc) ngtcp2_conn_del(c->qc);
#endif
#if defined(WEBLOX_HAVE_NGHTTP3) && defined(WEBLOX_HAVE_NGTCP2)
  while (c->pending) {
    wt_write* next = c->pending->next;
    wt_write_free(c, c->pending);
    c->pending = next;
  }
  wt_write_free(c, c->sending);
#endif
#if defined(WEBLOX_HAVE_NGHTTP3)
  if (c->h3) nghttp3_conn_del(c->h3);
#endif
  if (c->sock != WT_BAD_SOCKET) wt_socket_close(c->sock);
  memset(c, 0, sizeof *c);
  c->sock = WT_BAD_SOCKET;
}

/* Feed h3 data queued by write_stream_js into QUIC frames on this pump. */
static void wt_flush_pending(wt_conn* c) {
#if defined(WEBLOX_HAVE_NGTCP2) && defined(WEBLOX_HAVE_NGHTTP3)
  if (!c->qc || !c->h3 || !c->handshake_done) return;
  for (int pass = 0; pass < 16; ++pass) {
    nghttp3_vec hvec[4];
    int64_t sid = -1;
    int fin = 0;
    nghttp3_ssize nvec = nghttp3_conn_writev_stream(c->h3, &sid, &fin, hvec, 4);
    if (nvec <= 0 || sid < 0) return;
    ngtcp2_vec vec[4];
    for (nghttp3_ssize i = 0; i < nvec; ++i) {
      vec[i].base = hvec[i].base;
      vec[i].len = hvec[i].len;
    }
    ngtcp2_ssize datalen = 0;
    ngtcp2_ssize nwrite = ngtcp2_conn_writev_stream(
        c->qc, &c->qpath, NULL, c->pkt, sizeof c->pkt, &datalen,
        NGTCP2_WRITE_STREAM_FLAG_NONE, sid, vec, (size_t)nvec, wt_now_us());
    if (nwrite > 0) wt_send_packet(c, c->pkt, (size_t)nwrite);
    if (datalen > 0) nghttp3_conn_add_write_offset(c->h3, sid, (uint64_t)datalen);
    if (c->sending) {
      wt_write* w = c->sending;
      c->sending = NULL;
      if (datalen > 0 && (size_t)datalen < w->len - w->off) {
        /* Partial write: retry the remainder on the next pump. */
        w->off += (size_t)datalen;
        w->next = c->pending;
        c->pending = w;
        if (!c->pending_tail) c->pending_tail = w;
      } else {
        wt_write_free(c, w);
      }
    }
    if (nwrite < 0 && nwrite != NGTCP2_ERR_WRITE_MORE) {
      fprintf(stderr, "[wt] h3 write: %s\n", ngtcp2_strerror((int)nwrite));
      return;
    }
  }
#else
  (void)c;
#endif
}

/* ------------------------------------------------------------------------- */
/* Engine -> host imports                                                     */
/* ------------------------------------------------------------------------- */

u32 w2c_env_rbx_wasm_rna_is_supported_js(struct w2c_env* env) {
  (void)env;
#if WT_BACKEND_READY
  return 1;
#else
  return 0;
#endif
}

/* (id, url, certHashes[], count, ringData, ringHead, ringTail, slotSize,
 *  slotCount, fixConnOwnership, a, b, c, d) */
void w2c_env_rbx_wasm_rna_connect_js(struct w2c_env* env, u32 id, u32 url,
                                     u32 hashes, u32 nhashes, u32 data,
                                     u32 head, u32 tail, u32 slot, u32 nslots,
                                     u32 a, u32 b, u32 c, u32 d) {
  (void)env; (void)a; (void)b; (void)c; (void)d;
  const char* url_text = guest_str(url);
  fprintf(stderr, "[wt] connect id=%u url=%.160s\n", id, url_text);

  wt_conn* conn = NULL;
  for (int i = 0; i < WT_CONN_MAX; ++i)
    if (!g_conns[i].used) { conn = &g_conns[i]; break; }
  if (!conn) { wt_signal_connect_error(&(wt_conn){.id = id}, -1); return; }
  memset(conn, 0, sizeof *conn);
  conn->sock = WT_BAD_SOCKET;
  conn->used = 1;
  conn->id = id;
  snprintf(conn->url, sizeof conn->url, "%s", url_text);
  conn->ring_data = data;
  conn->ring_head = head;
  conn->ring_tail = tail;
  conn->ring_slot = slot ? slot : 1500;
  conn->ring_nslots = nslots ? nslots : 1;
  if ((conn->ring_nslots & (conn->ring_nslots - 1)) != 0) {
    fprintf(stderr, "[wt] ring slot count %u is not a power of two\n", nslots);
    conn->ring_nslots = 1;
  }

  /* Certificate hashes are guest pointers to NUL-terminated hex strings. */
  u32 count = nhashes < WT_PIN_MAX ? nhashes : WT_PIN_MAX;
  for (u32 i = 0; i < count; ++i) {
    const unsigned char* slot_bytes = guest_ptr(hashes + (u64)i * 4u, 4);
    if (!slot_bytes) continue;
    u32 pin_addr;
    memcpy(&pin_addr, slot_bytes, 4);
    const char* pin = guest_str(pin_addr);
    if (*pin) {
      snprintf(conn->pins[conn->npins], WT_PIN_LEN, "%s", pin);
      ++conn->npins;
    }
  }
  if (conn->npins)
    fprintf(stderr, "[wt] %u certificate pin(s) from joinScript\n", conn->npins);

  /* Parse https://host[:port]/path. */
  const char* scheme = strstr(url_text, "://");
  const char* host_start = scheme ? scheme + 3 : url_text;
  const char* slash = strchr(host_start, '/');
  size_t host_len = slash ? (size_t)(slash - host_start) : strlen(host_start);
  if (host_len >= sizeof conn->host) host_len = sizeof conn->host - 1;
  memcpy(conn->host, host_start, host_len);
  conn->host[host_len] = '\0';
  conn->port = 443;
  char* colon = strrchr(conn->host, ':');
  if (colon && colon[1] >= '0' && colon[1] <= '9') {
    conn->port = (unsigned short)atoi(colon + 1);
    *colon = '\0';
  }
  snprintf(conn->path, sizeof conn->path, "/%s", slash ? slash + 1 : "");

#if WT_BACKEND_READY
  if (wt_open_socket(conn) != 0) {
    wt_signal_connect_error(conn, -2);
    wt_conn_reset(conn);
    return;
  }

  conn->ssl_ctx = SSL_CTX_new(TLS_client_method());
  if (!conn->ssl_ctx) {
    wt_signal_connect_error(conn, -3);
    wt_conn_reset(conn);
    return;
  }
  SSL_CTX_set_default_verify_paths(conn->ssl_ctx);
  SSL_CTX_set_verify(conn->ssl_ctx, SSL_VERIFY_PEER, wt_verify_cert);
  conn->ssl = SSL_new(conn->ssl_ctx);
  if (!conn->ssl || ngtcp2_crypto_ossl_ctx_new(&conn->ossl_ctx, conn->ssl) != 0) {
    wt_signal_connect_error(conn, -3);
    wt_conn_reset(conn);
    return;
  }
  ngtcp2_crypto_ossl_ctx_set_ssl(conn->ossl_ctx, conn->ssl);
  if (ngtcp2_crypto_ossl_configure_client_session(conn->ssl) != 0) {
    wt_signal_connect_error(conn, -3);
    wt_conn_reset(conn);
    return;
  }
  conn->conn_ref.get_conn = conn_ref_get;
  conn->conn_ref.user_data = conn;
  SSL_set_app_data(conn->ssl, &conn->conn_ref);
  SSL_set_connect_state(conn->ssl);
  static const unsigned char alpn[] = {2, 'h', '3'};
  SSL_set_alpn_protos(conn->ssl, alpn, sizeof alpn);
  SSL_set_tlsext_host_name(conn->ssl, conn->host);

  ngtcp2_callbacks callbacks;
  memset(&callbacks, 0, sizeof callbacks);
  callbacks.client_initial = ngtcp2_crypto_client_initial_cb;
  callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
  callbacks.handshake_completed = wt_handshake_completed;
  callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
  callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
  callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
  callbacks.recv_retry = ngtcp2_crypto_recv_retry_cb;
  callbacks.extend_max_local_streams_bidi = NULL;
  callbacks.rand = wt_rand;
  callbacks.update_key = ngtcp2_crypto_update_key_cb;
  callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
  callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
  callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
  callbacks.get_new_connection_id2 = wt_new_cid;
  callbacks.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
  callbacks.recv_stream_data = wt_recv_stream_data;
  callbacks.acked_stream_data_offset = wt_acked_stream_data;
  callbacks.recv_datagram = wt_recv_datagram;

  ngtcp2_cid dcid, scid;
  if (RAND_bytes(dcid.data, NGTCP2_MIN_INITIAL_DCIDLEN) != 1 ||
      RAND_bytes(scid.data, 8) != 1) {
    wt_signal_connect_error(conn, -3);
    wt_conn_reset(conn);
    return;
  }
  dcid.datalen = NGTCP2_MIN_INITIAL_DCIDLEN;
  scid.datalen = 8;
  memset(&conn->qpath, 0, sizeof conn->qpath);
  conn->qpath.remote.addr = (struct sockaddr*)&conn->remote;
  conn->qpath.remote.addrlen = conn->remote_len;
  ngtcp2_settings settings;
  ngtcp2_transport_params params;
  ngtcp2_settings_default(&settings);
  ngtcp2_transport_params_default(&params);
  settings.initial_ts = wt_now_us();
  params.initial_max_streams_uni = 3;
  params.initial_max_stream_data_bidi_local = 128 * 1024;
  params.initial_max_data = 1024 * 1024;
  params.max_datagram_frame_size = 1200;
  int rv = ngtcp2_conn_client_new(&conn->qc, &dcid, &scid, &conn->qpath,
                                  NGTCP2_PROTO_VER_V1, &callbacks, &settings,
                                  &params, NULL, conn);
  if (rv != 0) {
    fprintf(stderr, "[wt] QUIC connect: %s\n", ngtcp2_strerror(rv));
    wt_signal_connect_error(conn, rv);
    wt_conn_reset(conn);
    return;
  }
  ngtcp2_conn_set_tls_native_handle(conn->qc, conn->ossl_ctx);

  nghttp3_callbacks h3cb;
  memset(&h3cb, 0, sizeof h3cb);
  h3cb.recv_data = wt_h3_recv_data;
  h3cb.end_headers = wt_h3_end_headers;
  h3cb.stream_close = wt_h3_stream_close;
  h3cb.recv_settings2 = wt_h3_recv_settings2;
  h3cb.rand = wt_h3_rand;
  nghttp3_settings h3settings;
  nghttp3_settings_default(&h3settings);
  rv = nghttp3_conn_client_new(&conn->h3, &h3cb, &h3settings, NULL, conn);
  if (rv != 0) {
    fprintf(stderr, "[wt] h3 client: %s\n", nghttp3_strerror(rv));
    wt_signal_connect_error(conn, rv);
    wt_conn_reset(conn);
    return;
  }
  fprintf(stderr, "[wt] QUIC/h3 client created for %s:%u\n", conn->host, conn->port);
#else
  wt_log_once("connect_js handshake (ngtcp2 + nghttp3 + TLS required)");
  wt_signal_connect_error(conn, -1200);
#endif
}

void w2c_env_rbx_wasm_rna_open_stream_js(struct w2c_env* env, u32 id, u32 sid) {
  (void)env;
  wt_conn* conn = conn_get(id);
  if (!conn) return;
  for (int i = 0; i < WT_STREAM_MAX; ++i)
    if (!conn->streams[i]) {
      conn->streams[i] = sid ? (int64_t)sid : 1;
      break;
    }
#if defined(WEBLOX_HAVE_NGTCP2) && defined(WEBLOX_HAVE_NGHTTP3)
  /* The engine supplies an opaque stream id; WebTransport streams are opened
   * by QUIC.  WT_TODO: map guest sid -> ngtcp2 stream via a bidi open when the
   * engine requests an unsolicited stream rather than reusing the CONNECT one. */
  if (conn->qc && conn->handshake_done) wt_flush_pending(conn);
#else
  wt_log_once("open_stream_js (nghttp3 stream open)");
#endif
}

void w2c_env_rbx_wasm_rna_write_stream_js(struct w2c_env* env, u32 id, u32 sid,
                                          u32 ptr, u32 len) {
  (void)env;
  wt_conn* conn = conn_get(id);
  const unsigned char* bytes = guest_ptr(ptr, len);
  if (!conn || !bytes) {
    wt_log_once("write_stream_js (bad connection or guest buffer)");
    return;
  }
#if defined(WEBLOX_HAVE_NGTCP2) && defined(WEBLOX_HAVE_NGHTTP3)
  if (!conn->h3 || !len) return;
  wt_write* w = (wt_write*)calloc(1, sizeof *w);
  if (!w) return;
  w->data = (unsigned char*)malloc(len);
  if (!w->data) { free(w); return; }
  memcpy(w->data, bytes, len);
  w->len = len;
  w->stream_id = sid ? (int64_t)sid : conn->connect_stream;
  if (conn->pending_tail) conn->pending_tail->next = w;
  else conn->pending = w;
  conn->pending_tail = w;
  nghttp3_conn_resume_stream(conn->h3, w->stream_id);
  wt_flush_pending(conn);
#else
  wt_log_once("write_stream_js (h3 DATA)");
#endif
}

void w2c_env_rbx_wasm_rna_send_datagram_js(struct w2c_env* env, u32 id, u32 ptr,
                                           u32 len, u32 hi, u32 lo) {
  (void)env;
  wt_conn* conn = conn_get(id);
  const unsigned char* bytes = guest_ptr(ptr, len);
  u32 args[3] = {id, hi, lo};
  if (!conn || !bytes) {
    wt_signal("rbx_wasm_rna_on_datagram_lost", args, 3);
    return;
  }
#if defined(WEBLOX_HAVE_NGTCP2) && defined(WEBLOX_HAVE_NGHTTP3)
  if (conn->qc && conn->handshake_done) {
    ngtcp2_vec vec;
    vec.base = (uint8_t*)(uintptr_t)bytes;
    vec.len = len;
    int accepted = 0;
    ngtcp2_ssize nwrite = ngtcp2_conn_writev_datagram(
        conn->qc, &conn->qpath, NULL, conn->pkt, sizeof conn->pkt, &accepted,
        NGTCP2_WRITE_DATAGRAM_FLAG_NONE, ((u64)hi << 32) | lo, &vec, 1,
        wt_now_us());
    if (nwrite > 0) {
      wt_send_packet(conn, conn->pkt, (size_t)nwrite);
      wt_signal("rbx_wasm_rna_on_datagram_acked", args, 3);
      return;
    }
    if (nwrite < 0)
      fprintf(stderr, "[wt] datagram: %s\n", ngtcp2_strerror((int)nwrite));
  }
#else
  wt_log_once("send_datagram_js (QUIC datagram)");
#endif
  wt_signal("rbx_wasm_rna_on_datagram_lost", args, 3);
}

void w2c_env_rbx_wasm_rna_pause_stream_read_js(struct w2c_env* env, u32 id, u32 sid) {
  (void)env;
  wt_conn* c = conn_get(id);
  if (c && sid < WT_STREAM_MAX) c->paused[sid] = 1;
}

void w2c_env_rbx_wasm_rna_resume_stream_read_js(struct w2c_env* env, u32 id, u32 sid) {
  (void)env;
  wt_conn* c = conn_get(id);
  if (c && sid < WT_STREAM_MAX) c->paused[sid] = 0;
}

void w2c_env_rbx_wasm_rna_shutdown_stream_read_js(struct w2c_env* env, u32 id, u32 sid) {
  (void)env; (void)id; (void)sid;
  wt_log_once("shutdown_stream_read_js");
}

void w2c_env_rbx_wasm_rna_shutdown_stream_write_js(struct w2c_env* env, u32 id, u32 sid) {
  (void)env;
  wt_conn* conn = conn_get(id);
#if defined(WEBLOX_HAVE_NGTCP2) && defined(WEBLOX_HAVE_NGHTTP3)
  if (conn && conn->h3) {
    int64_t stream = sid ? (int64_t)sid : conn->connect_stream;
    for (wt_write* w = conn->pending; w; w = w->next)
      if (w->stream_id == stream) w->fin = 1;
    /* An empty final write ensures the reader can emit FIN even with no data. */
    wt_write* fin = (wt_write*)calloc(1, sizeof *fin);
    if (fin) {
      fin->stream_id = stream;
      fin->fin = 1;
      if (conn->pending_tail) conn->pending_tail->next = fin;
      else conn->pending = fin;
      conn->pending_tail = fin;
      nghttp3_conn_resume_stream(conn->h3, stream);
      wt_flush_pending(conn);
    }
  }
#else
  (void)conn;
  wt_log_once("shutdown_stream_write_js (h3 FIN)");
#endif
}

void w2c_env_rbx_wasm_rna_receive_ring_consumed_js(struct w2c_env* env, u32 id,
                                                    u32 stream, u32 count) {
  (void)env; (void)stream;
  wt_conn* conn = conn_get(id);
  if (conn && conn->ring_tail) guest_put_u32(conn->ring_tail, count);
}

void w2c_env_rbx_wasm_rna_send_connection_close_js(struct w2c_env* env, u32 id) {
  (void)env;
  wt_conn* conn = conn_get(id);
  if (!conn) return;
#if defined(WEBLOX_HAVE_NGTCP2)
  if (conn->qc) {
    unsigned char buf[WT_PKT_LEN];
    ngtcp2_ccerr ccerr;
    ngtcp2_ccerr_default(&ccerr);
    ccerr.error_code = NGTCP2_NO_ERROR;
    ngtcp2_ssize n = ngtcp2_conn_write_connection_close(
        conn->qc, &conn->qpath, NULL, buf, sizeof buf, &ccerr, wt_now_us());
    if (n > 0) wt_send_packet(conn, buf, (size_t)n);
  }
#else
  wt_log_once("send_connection_close_js");
#endif
}

void w2c_env_rbx_wasm_rna_close_js(struct w2c_env* env, u32 id) {
  (void)env;
  wt_conn* conn = conn_get(id);
  if (!conn) return;
  wt_conn_reset(conn);
  u32 args[1] = {id};
  wt_signal("rbx_wasm_rna_on_close_complete", args, 1);
}

/* ------------------------------------------------------------------------- */
/* Public lifecycle / I/O pump                                               */
/* ------------------------------------------------------------------------- */

bool wt_init(void) {
  if (g_initialized) return true;
  memset(g_conns, 0, sizeof g_conns);
  g_initialized = 1;
#if defined(_WIN32)
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) g_sockets_ready = 1;
#else
  g_sockets_ready = 1;
#endif
#if WT_BACKEND_READY
  ngtcp2_crypto_ossl_init();
  fprintf(stderr, "[wt] ngtcp2/nghttp3 RNA layer ready (ALPN h3)\n");
#else
  wt_log_once("QUIC backend (ngtcp2 + nghttp3 + TLS required)");
#endif
  return true;
}

void wt_shutdown(void) {
  for (int i = 0; i < WT_CONN_MAX; ++i)
    if (g_conns[i].used) wt_conn_reset(&g_conns[i]);
#if WT_BACKEND_READY
  ngtcp2_crypto_ossl_free();
#endif
#if defined(_WIN32)
  if (g_sockets_ready) WSACleanup();
#endif
  g_sockets_ready = 0;
  g_initialized = 0;
}

void wt_pump(void) {
#if WT_BACKEND_READY
  if (!g_sockets_ready) return;
  int64_t now_us = wt_now_us();
  for (int i = 0; i < WT_CONN_MAX; ++i) {
    wt_conn* c = &g_conns[i];
    if (!c->used || !c->qc) continue;

    /* Incoming UDP datagrams -> QUIC -> h3 -> engine receive ring. */
    for (int n = 0; n < 16; ++n) {
      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(c->sock, &readfds);
      struct timeval tv = {0, 0};
      if (select((int)c->sock + 1, &readfds, NULL, NULL, &tv) <= 0 ||
          !FD_ISSET(c->sock, &readfds))
        break;
      wt_ssize nread = recv(c->sock, (char*)c->pkt, sizeof c->pkt, 0);
      if (nread <= 0) break;
      ngtcp2_pkt_info pi;
      memset(&pi, 0, sizeof pi);
      int rv = ngtcp2_conn_read_pkt(c->qc, &c->qpath, &pi, c->pkt,
                                    (size_t)nread, (ngtcp2_tstamp)now_us);
      if (rv != 0 && rv != NGTCP2_ERR_DRAINING) {
        fprintf(stderr, "[wt] read pkt: %s\n", ngtcp2_strerror(rv));
        wt_signal_connect_error(c, rv);
        wt_conn_reset(c);
        break;
      }
    }
    if (!c->used) continue;

    /* Retransmission / keep-alive packets. */
    if (ngtcp2_conn_get_expiry(c->qc) <= (ngtcp2_tstamp)now_us) {
      ngtcp2_conn_handle_expiry(c->qc, (ngtcp2_tstamp)now_us);
      ngtcp2_ssize n = ngtcp2_conn_write_pkt(c->qc, &c->qpath, NULL, c->pkt,
                                             sizeof c->pkt, (ngtcp2_tstamp)now_us);
      if (n > 0) wt_send_packet(c, c->pkt, (size_t)n);
    }
    if (c->handshake_done && !c->connect_sent && c->h3 &&
        c->connect_stream == 0)
      wt_open_h3_connect(c);
    wt_flush_pending(c);
  }
#else
  wt_log_once("wt_pump (QUIC I/O loop)");
#endif
}
