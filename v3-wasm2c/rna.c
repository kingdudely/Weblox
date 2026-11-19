/* impl/rna.c - the engine's transport (RNA) over WebTransport.
 *
 * The web client has no socket; Chrome hands it a WebTransport and the engine
 * speaks its "RNA" datagram/stream API to the host:
 *
 *   host -> engine: on_connect_success / on_receive_ring_slot /
 *                   on_stream_accepted / on_stream_fin / on_connection_closed
 *   engine -> host: connect_js / send_datagram_js / open_stream_js /
 *                   write_stream_js / close_js ...
 *
 * Those callbacks are embind functions registered under those names, so the
 * host calls them through the embind registry.  The wire is a real
 * WebTransport (QUIC/h3, ALPN h3, CONNECT +protocol webtransport) done by
 * `tools/wt_bridge.py` over a pipe; there is no RUPP here - the browser path
 * does not use it.  The engine hands over the finished URL, the receive ring
 * geometry and the certificate hashes.
 */
#define _GNU_SOURCE
#include "../host/host.h"
#include "../host/plat.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern void wc_defer_embind(const char* name, int nargs, ...);

#define CONN_MAX 8

typedef struct conn {
  int used;
  u32 id;
  pid_t pid;
  int to;                       /* bridge stdin */
  FILE* from;                   /* bridge stdout */
  volatile int stop;
  pthread_t th;
  pthread_mutex_t wmtx;         /* serialises writes to the bridge */
  u32 data, head, tail;         /* receive ring, in wasm memory */
  u32 slot, nslots;
  u32 dropped;
} conn_t;

static conn_t g_conns[CONN_MAX];
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

static conn_t* conn_get(u32 id) {
  for (int i = 0; i < CONN_MAX; i++)
    if (g_conns[i].used && g_conns[i].id == id) return &g_conns[i];
  return NULL;
}

static void conn_free(conn_t* c) {
  pthread_mutex_lock(&g_mtx);
  c->used = 0;
  pthread_mutex_unlock(&g_mtx);
}

/* write a command line + optional payload to the bridge */
static void cmd(conn_t* c, const char* line, const void* data, size_t n) {
  pthread_mutex_lock(&c->wmtx);
  size_t l = strlen(line);
  if (write(c->to, line, l) != (ssize_t)l) goto out;
  if (n && write(c->to, data, n) != (ssize_t)n) goto out;
out:
  pthread_mutex_unlock(&c->wmtx);
}

static int ring_put(conn_t* c, u32 kind, u32 sid, const u8* buf, u32 len) {
  u32 head = __atomic_load_n(&c->head, __ATOMIC_ACQUIRE);
  u32 tail = __atomic_load_n(&c->tail, __ATOMIC_ACQUIRE);
  if (head - tail >= c->nslots) { c->dropped++; return 0; }
  u32 idx = head & (c->nslots - 1);
  u32 n = len > c->slot ? c->slot : len;
  memcpy(wc_ptr(c->data + idx * c->slot), buf, n);
  __atomic_store_n(&c->head, head + 1, __ATOMIC_RELEASE);
  wc_defer_embind("rbx_wasm_rna_on_receive_ring_slot", 5, c->id, kind, sid, idx, n);
  return 1;
}

static void* reader(void* p) {
  conn_t* c = p;
  char line[256];
  u8* buf = malloc(1 << 16);
  while (!c->stop && fgets(line, sizeof line, c->from)) {
    size_t l = strlen(line);
    while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
    if (!*line) continue;
    char tag = line[0];
    if (tag == 'R') {
      wc_defer_embind("rbx_wasm_rna_on_connect_success", 2, c->id, 1200u);
    } else if (tag == 'E') {
      fprintf(stderr, "[rna] connect error: %s\n", line + 2);
      wc_defer_embind("rbx_wasm_rna_on_connect_error", 1, c->id);
      break;
    } else if (tag == 'X') {
      unsigned code = 0;
      sscanf(line + 2, "%u", &code);
      wc_defer_embind("rbx_wasm_rna_on_connection_closed", 4, c->id, code, 0u, 0u);
      break;
    } else if (tag == 'A') {
      unsigned sid = 0;
      sscanf(line + 2, "%u", &sid);
      wc_defer_embind("rbx_wasm_rna_on_stream_accepted", 2, c->id, sid);
    } else if (tag == 'F') {
      unsigned sid = 0;
      sscanf(line + 2, "%u", &sid);
      wc_defer_embind("rbx_wasm_rna_on_stream_fin", 2, c->id, sid);
    } else if (tag == 'D' || tag == 'S') {
      unsigned sid = 0, n = 0;
      if (tag == 'D') sscanf(line + 2, "%u", &n);
      else sscanf(line + 2, "%u %u", &sid, &n);
      if (n > (1u << 20)) continue;
      size_t got = fread(buf, 1, n, c->from);
      if (got != n) break;
      ring_put(c, tag == 'D' ? 1u : 0u, sid, buf, n);
    }
  }
  free(buf);
  if (!c->stop)
    wc_defer_embind("rbx_wasm_rna_on_connection_closed", 4, c->id, 0u, 0u, 0u);
  return NULL;
}

u32 wc_impl_rbx_wasm_rna_is_supported_js(struct w2c_env* a0) {
  (void)a0;
  return 1;
}

/* (id, url, certHashes[], count, recvData, recvHead, recvTail, slotSize,
 *  slotCount, fixConnOwnership, a, b, c) */
void wc_impl_rbx_wasm_rna_connect_js(struct w2c_env* a0, u32 id, u32 urlp,
                                     u32 hashesp, u32 nhash, u32 data, u32 head,
                                     u32 tail, u32 slot, u32 nslots, u32 fa,
                                     u32 fb, u32 fc, u32 fd) {
  (void)a0; (void)hashesp; (void)nhash; (void)fa; (void)fb; (void)fc; (void)fd;
  const char* url = wc_str(urlp);
  fprintf(stderr, "[rna] connect id=%u url=%.120s\n", id, url ? url : "?");

  conn_t* c = NULL;
  pthread_mutex_lock(&g_mtx);
  for (int i = 0; i < CONN_MAX; i++) if (!g_conns[i].used) { c = &g_conns[i]; break; }
  if (c) {
    memset(c, 0, sizeof *c);
    pthread_mutex_init(&c->wmtx, NULL);
    c->used = 1; c->id = id; c->to = -1;
  }
  pthread_mutex_unlock(&g_mtx);
  if (!c) { wc_defer_embind("rbx_wasm_rna_on_connect_error", 1, id); return; }
  c->data = data; c->head = head; c->tail = tail;
  c->slot = slot ? slot : 1500;
  c->nslots = nslots ? nslots : 1;

  /* port from the URL (https://host:port/wt?...) */
  int port = 0;
  const char* hp = strstr(url ? url : "", "://");
  const char* slash = hp ? strchr(hp + 3, '/') : NULL;
  const char* colon = slash ? memchr(hp + 3, ':', (size_t)(slash - (hp + 3))) : NULL;
  if (colon) port = atoi(colon + 1);

  int in[2], out[2];
  if (pipe(in) || pipe(out)) { conn_free(c); wc_defer_embind("rbx_wasm_rna_on_connect_error", 1, id); return; }
  const char* py = pl_getenv("WC_PYTHON");
  const char* br = pl_getenv("WC_BRIDGE");
  if (!br || !*br) br = "tools/wt_bridge.py";

  pid_t pid = fork();
  if (pid == 0) {
    dup2(in[0], 0); dup2(out[1], 1);
    close(in[0]); close(in[1]); close(out[0]); close(out[1]);
    execlp(py && *py ? py : "python3", "python3", "-u", br, (char*)NULL);
    _exit(127);
  }
  close(in[0]); close(out[1]);
  if (pid < 0) { close(in[1]); close(out[0]); conn_free(c);
    wc_defer_embind("rbx_wasm_rna_on_connect_error", 1, id); return; }
  c->pid = pid;
  c->to = in[1];
  c->from = fdopen(out[0], "r");
  if (!c->from) { close(in[1]); conn_free(c); wc_defer_embind("rbx_wasm_rna_on_connect_error", 1, id); return; }
  setvbuf(c->from, NULL, _IONBF, 0);

  char line[1200];
  snprintf(line, sizeof line, "C %d %s\n", port, url ? url : "");
  cmd(c, line, NULL, 0);

  if (pthread_create(&c->th, NULL, reader, c)) {
    close(in[1]); fclose(c->from); conn_free(c);
    wc_defer_embind("rbx_wasm_rna_on_connect_error", 1, id);
    return;
  }
}

void wc_impl_rbx_wasm_rna_send_datagram_js(struct w2c_env* a0, u32 id, u32 ptr,
                                           u32 len, u32 hi, u32 lo) {
  (void)a0;
  conn_t* c = conn_get(id);
  if (!c || c->to < 0) { wc_defer_embind("rbx_wasm_rna_on_datagram_lost", 3, id, hi, lo); return; }
  char line[64];
  snprintf(line, sizeof line, "D %u\n", len);
  cmd(c, line, wc_ptr(ptr), len);
  wc_defer_embind("rbx_wasm_rna_on_datagram_acked", 3, id, hi, lo);
}

void wc_impl_rbx_wasm_rna_open_stream_js(struct w2c_env* a0, u32 id, u32 sid) {
  (void)a0;
  conn_t* c = conn_get(id);
  if (!c) return;
  char line[64];
  snprintf(line, sizeof line, "S %u\n", sid);
  cmd(c, line, NULL, 0);
}

void wc_impl_rbx_wasm_rna_write_stream_js(struct w2c_env* a0, u32 id, u32 sid,
                                          u32 ptr, u32 len) {
  (void)a0;
  conn_t* c = conn_get(id);
  if (!c) return;
  char line[64];
  snprintf(line, sizeof line, "W %u %u\n", sid, len);
  cmd(c, line, wc_ptr(ptr), len);
}

void wc_impl_rbx_wasm_rna_shutdown_stream_write_js(struct w2c_env* a0, u32 id, u32 sid) {
  (void)a0;
  conn_t* c = conn_get(id);
  if (!c) return;
  char line[64];
  snprintf(line, sizeof line, "F %u\n", sid);
  cmd(c, line, NULL, 0);
}

/* read-side controls are flow-control hints; the bridge reads eagerly */
void wc_impl_rbx_wasm_rna_pause_stream_read_js(struct w2c_env* a0, u32 id, u32 sid) {
  (void)a0; (void)id; (void)sid;
}
void wc_impl_rbx_wasm_rna_resume_stream_read_js(struct w2c_env* a0, u32 id, u32 sid) {
  (void)a0; (void)id; (void)sid;
}
void wc_impl_rbx_wasm_rna_shutdown_stream_read_js(struct w2c_env* a0, u32 id, u32 sid) {
  (void)a0; (void)id; (void)sid;
}

static void conn_close(conn_t* c, int completed) {
  if (!c) return;
  c->stop = 1;
  if (c->to >= 0) { cmd(c, "X\n", NULL, 0); shutdown(c->to, SHUT_WR); }
  if (c->pid > 0) { int st; waitpid(c->pid, &st, WNOHANG); }
  if (c->from) { fclose(c->from); c->from = NULL; }
  if (c->to >= 0) { close(c->to); c->to = -1; }
  if (c->th) { pthread_join(c->th, NULL); c->th = 0; }
  if (completed) wc_defer_embind("rbx_wasm_rna_on_close_complete", 1, c->id);
  conn_free(c);
}

void wc_impl_rbx_wasm_rna_close_js(struct w2c_env* a0, u32 id) {
  (void)a0;
  conn_close(conn_get(id), 1);
}

void wc_impl_rbx_wasm_rna_send_connection_close_js(struct w2c_env* a0, u32 id) {
  (void)a0;
  conn_t* c = conn_get(id);
  if (c && c->to >= 0) cmd(c, "X\n", NULL, 0);
}
