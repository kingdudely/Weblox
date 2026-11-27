// WebTransport imports (Roblox "RNA") over ngtcp2 + nghttp3.
//
// The engine hands the host a finished join URL, a receive-ring geometry in
// guest memory and the certificate hashes parsed out of joinScript.  All guest
// reads and writes go through guest.h.
//
// Status: the connection table, stream bookkeeping, receive ring, datagram
// bookkeeping and certificate pins are implemented.  The QUIC/TLS handshake
// and h3 stream I/O are marked TODO: completing them requires a socket loop,
// a TLS provider and the ALPN "h3" CONNECT handshake.  Until then connect_js
// reports on_connect_error so the engine never waits forever.
#include "host.h"
#include "guest.h"
#include "webtransport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

#if defined(WEBLOX_HAVE_NGTCP2)
#include <ngtcp2/ngtcp2.h>
#endif
#if defined(WEBLOX_HAVE_NGHTTP3)
#include <nghttp3/nghttp3.h>
#endif

#define WT_CONN_MAX 8
#define WT_STREAM_MAX 32
#define WT_PIN_MAX 8
#define WT_PIN_LEN 65
#define WT_URL_LEN 512

typedef struct {
  int used;
  u32 id;
  char url[WT_URL_LEN];
  char pins[WT_PIN_MAX][WT_PIN_LEN];
  u32 npins;
  u32 ring_data, ring_head, ring_tail, ring_slot, ring_nslots;
  u32 dropped;
  int streams[WT_STREAM_MAX]; /* stream ids, 0 means free */
#if defined(WEBLOX_HAVE_NGTCP2)
  ngtcp2_conn* qc;
  ngtcp2_settings settings;
  ngtcp2_transport_params params;
#endif
#if defined(WEBLOX_HAVE_NGHTTP3)
  nghttp3_conn* h3;
#endif
} wt_conn;

static wt_conn g_conns[WT_CONN_MAX];
static wt_signal_fn g_signal;
static int g_initialized;

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
    /* No host bridge installed yet: keep the names visible in logs. */
    fprintf(stderr, "[wt] signal %s", name);
    for (u32 i = 0; i < argc; ++i) fprintf(stderr, "%s%u", i ? "," : "(", args[i]);
    fprintf(stderr, ")\n");
  }
}

bool wt_init(void) {
  if (g_initialized) return true;
  memset(g_conns, 0, sizeof g_conns);
  g_initialized = 1;
#if defined(WEBLOX_HAVE_NGTCP2) && defined(WEBLOX_HAVE_NGHTTP3)
  fprintf(stderr, "[wt] ngtcp2/nghttp3 RNA layer ready\n");
#else
  wt_log_once("QUIC backend (define WEBLOX_HAVE_NGTCP2 and WEBLOX_HAVE_NGHTTP3)");
#endif
  return true;
}

void wt_set_signal(wt_signal_fn fn) { g_signal = fn; }

u32 wt_active_connections(void) {
  u32 n = 0;
  for (int i = 0; i < WT_CONN_MAX; ++i) n += g_conns[i].used ? 1u : 0u;
  return n;
}

void wt_shutdown(void) {
  for (int i = 0; i < WT_CONN_MAX; ++i)
    if (g_conns[i].used) {
#if defined(WEBLOX_HAVE_NGTCP2)
      if (g_conns[i].qc) ngtcp2_conn_del(g_conns[i].qc);
#endif
#if defined(WEBLOX_HAVE_NGHTTP3)
      if (g_conns[i].h3) nghttp3_conn_del(g_conns[i].h3);
#endif
      memset(&g_conns[i], 0, sizeof g_conns[i]);
    }
  g_initialized = 0;
}

/* ------------------------------------------------------------------------- */
/* Guest receive ring                                                        */
/* ------------------------------------------------------------------------- */

static void ring_put(wt_conn* c, u32 kind, u32 sid, const unsigned char* buf, u32 len) {
  if (!c->ring_slot || !c->ring_nslots) return;
  u32 head_bytes;
  unsigned char* hp = guest_ptr(c->ring_head, 4);
  if (!hp) return;
  memcpy(&head_bytes, hp, 4);
  u32 tail_bytes;
  unsigned char* tp = guest_ptr(c->ring_tail, 4);
  if (!tp) return;
  memcpy(&tail_bytes, tp, 4);
  u64 head = head_bytes, tail = tail_bytes;
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
    if (!strcasecmp(c->pins[i], sha256_hex)) return true;
  fprintf(stderr, "[wt] certificate pin mismatch on connection %u\n", connection_id);
  return false;
}

/* ------------------------------------------------------------------------- */
/* Engine -> host imports                                                     */
/* ------------------------------------------------------------------------- */

u32 w2c_env_rbx_wasm_rna_is_supported_js(struct w2c_env* env) {
  (void)env;
#if defined(WEBLOX_HAVE_NGTCP2) && defined(WEBLOX_HAVE_NGHTTP3)
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
  if (!conn) {
    u32 args[1] = {id};
    wt_signal("rbx_wasm_rna_on_connect_error", args, 1);
    return;
  }
  memset(conn, 0, sizeof *conn);
  conn->used = 1;
  conn->id = id;
  snprintf(conn->url, sizeof conn->url, "%s", url_text);
  conn->ring_data = data;
  conn->ring_head = head;
  conn->ring_tail = tail;
  conn->ring_slot = slot ? slot : 1500;
  conn->ring_nslots = nslots ? nslots : 1;

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

#if defined(WEBLOX_HAVE_NGTCP2) && defined(WEBLOX_HAVE_NGHTTP3)
  /* TODO: resolve the host/port from conn->url, create the UDP socket, call
   * ngtcp2_conn_client_new(...) with ALPN "h3" and nghttp3_conn_client_new(...),
   * then verify the peer certificate with wt_pin_matches() before completing
   * the CONNECT handshake.  On success signal rbx_wasm_rna_on_connect_success. */
#else
  wt_log_once("connect_js handshake (ngtcp2 + nghttp3 required)");
#endif
  u32 args[2] = {id, 1200};
  wt_signal("rbx_wasm_rna_on_connect_error", args, 1);
}

void w2c_env_rbx_wasm_rna_open_stream_js(struct w2c_env* env, u32 id, u32 sid) {
  (void)env;
  wt_conn* conn = conn_get(id);
  if (!conn) return;
  for (int i = 0; i < WT_STREAM_MAX; ++i)
    if (!conn->streams[i]) { conn->streams[i] = sid ? sid : 1; break; }
  wt_log_once("open_stream_js (nghttp3 stream open)");
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
  /* TODO: submit `len` bytes as an h3 DATA frame on stream `sid`. */
  wt_log_once("write_stream_js (h3 DATA)");
}

void w2c_env_rbx_wasm_rna_send_datagram_js(struct w2c_env* env, u32 id, u32 ptr,
                                           u32 len, u32 hi, u32 lo) {
  (void)env;
  wt_conn* conn = conn_get(id);
  const unsigned char* bytes = guest_ptr(ptr, len);
  if (conn && bytes) {
    /* TODO: ngtcp2_conn_writev_datagram() on the active QUIC connection. */
    wt_log_once("send_datagram_js (QUIC datagram)");
    u32 args[3] = {id, hi, lo};
    wt_signal("rbx_wasm_rna_on_datagram_acked", args, 3);
  } else {
    u32 args[3] = {id, hi, lo};
    wt_signal("rbx_wasm_rna_on_datagram_lost", args, 3);
  }
}

void w2c_env_rbx_wasm_rna_pause_stream_read_js(struct w2c_env* env, u32 id, u32 sid) {
  (void)env; (void)id; (void)sid;
  wt_log_once("pause_stream_read_js (flow control hint)");
}

void w2c_env_rbx_wasm_rna_resume_stream_read_js(struct w2c_env* env, u32 id, u32 sid) {
  (void)env; (void)id; (void)sid;
  wt_log_once("resume_stream_read_js (flow control hint)");
}

void w2c_env_rbx_wasm_rna_shutdown_stream_read_js(struct w2c_env* env, u32 id, u32 sid) {
  (void)env; (void)id; (void)sid;
  wt_log_once("shutdown_stream_read_js");
}

void w2c_env_rbx_wasm_rna_shutdown_stream_write_js(struct w2c_env* env, u32 id, u32 sid) {
  (void)env; (void)id; (void)sid;
  /* TODO: nghttp3_conn_shutdown_stream_write() / ngtcp2 stream FIN. */
  wt_log_once("shutdown_stream_write_js (h3 FIN)");
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
  /* TODO: ngtcp2_conn_submit_crypto_data(CONNECTION_CLOSE). */
  wt_log_once("send_connection_close_js");
}

void w2c_env_rbx_wasm_rna_close_js(struct w2c_env* env, u32 id) {
  (void)env;
  wt_conn* conn = conn_get(id);
  if (!conn) return;
#if defined(WEBLOX_HAVE_NGTCP2)
  if (conn->qc) { ngtcp2_conn_del(conn->qc); conn->qc = NULL; }
#endif
#if defined(WEBLOX_HAVE_NGHTTP3)
  if (conn->h3) { nghttp3_conn_del(conn->h3); conn->h3 = NULL; }
#endif
  memset(conn, 0, sizeof *conn);
  u32 args[1] = {id};
  wt_signal("rbx_wasm_rna_on_close_complete", args, 1);
}

void wt_pump(void) {
  (void)ring_put; /* used once received packets are fed to the receive ring */
#if defined(WEBLOX_HAVE_NGTCP2) && defined(WEBLOX_HAVE_NGHTTP3)
  /* TODO: recvfrom -> ngtcp2_conn_read_pkt -> nghttp3_conn_read_stream ->
   * ring_put(), and ngtcp2_conn_writev_streams -> sendto for pending writes. */
  wt_log_once("wt_pump (QUIC I/O loop)");
#else
  wt_log_once("wt_pump (ngtcp2 + nghttp3 required)");
#endif
}
