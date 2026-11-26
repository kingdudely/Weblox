/* impl/http.c - the engine's HTTP: emscripten_fetch over curl.
 *
 * The web client runs on a browser XHR/fetch.  The engine builds an
 * emscripten_fetch_t, and the glue's `fetchXHR` performs it.  Here curl does,
 * with two deliberate shape rules taken from the working Web Play extension:
 *
 *   - joins are posted with `rbx-user-agent: Roblox/Darwin` (native identity),
 *     otherwise gamejoin answers status 12 "Playtesters Only" for any place the
 *     account does not own;
 *   - an approved join that has no `gameFqdn` gets one derived from its own
 *     network fields (see `patch_join`), because the WebTransport endpoint only
 *     exists in the neighbouring UDMUX address.
 *
 * Struct layout is the one the glue uses (fetchXHR): url at +8, the attribute
 * block at +108 (method[32], onsuccess +36, onerror +40, onprogress +44,
 * onreadystatechange +48, attributes +52, timeout +56, withCredentials +60,
 * userName +68, password +72, headers +76, mime +80, body +84, size +88);
 * results go to data +12, numBytes +16, readyState +40, status +42,
 * statusText +44.
 */
#include "../host/host.h"
#include "../host/plat.h"

#include <ctype.h>
#include <pthread.h>
#include <strings.h>
#include <unistd.h>

#define FETCH_ATTR 108
#define MAX_BODY (256u << 20)

/* response headers are needed by emscripten_fetch_get_response_headers */
typedef struct {
  u32 fetch;
  char* text;
} hdr_t;
static hdr_t g_hdr[64];
static pthread_mutex_t g_hdr_mtx = PTHREAD_MUTEX_INITIALIZER;

static const char* cookie_value(void) {
  static char buf[4096];
  static int loaded;
  if (loaded) return buf;
  loaded = 1;
  const char* e = pl_getenv("WC_COOKIE");
  if (e && *e) { snprintf(buf, sizeof buf, "%s", e); return buf; }
  const char* p = pl_getenv("WC_COOKIE_FILE");
  if (!p || !*p) p = "king.txt";
  pl_file* f = pl_open_read(p);
  if (!f) return buf;
  size_t n = pl_read(f, buf, sizeof buf - 1);
  pl_close(f);
  buf[n] = 0;
  while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
  return buf;
}

static int host_is(const char* url, const char* want) {
  const char* p = strstr(url, "://");
  p = p ? p + 3 : url;
  size_t n = strcspn(p, "/:?#");
  return n == strlen(want) && !strncasecmp(p, want, n);
}

static void hdr_set(u32 fetch, const char* text) {
  pthread_mutex_lock(&g_hdr_mtx);
  for (int i = 0; i < 64; i++) if (g_hdr[i].fetch == fetch) { free(g_hdr[i].text); g_hdr[i].text = NULL; break; }
  for (int i = 0; i < 64; i++) if (!g_hdr[i].text) {
    if (text) g_hdr[i].text = strdup(text);
    g_hdr[i].fetch = fetch;
    break;
  }
  pthread_mutex_unlock(&g_hdr_mtx);
}

static const char* hdr_get(u32 fetch) {
  const char* r = NULL;
  pthread_mutex_lock(&g_hdr_mtx);
  for (int i = 0; i < 64; i++) if (g_hdr[i].fetch == fetch && g_hdr[i].text) r = g_hdr[i].text;
  pthread_mutex_unlock(&g_hdr_mtx);
  return r;
}

/* ---------------------------------------------------------------- fqdn ---- */

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int b64_16(const char* s) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  unsigned char bin[32];
  int n = 0, bits = 0, acc = 0;
  for (; *s && *s != '"'; s++) {
    if (*s == '=') break;
    const char* q = strchr(t, *s);
    if (!q) return -1;
    acc = (acc << 6) | (int)(q - t);
    bits += 6;
    if (bits >= 8) { bits -= 8; if (n < (int)sizeof bin) bin[n++] = (acc >> bits) & 0xff; }
  }
  return n == 16 ? (memcpy((void*)(uintptr_t)0, bin, 0), n) : -1;
}

/* find "key": <value> in the JSON; returns pointer to the value */
static const char* json_val(const char* js, const char* key) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\":", key);
  const char* p = strstr(js, pat);
  if (!p) return NULL;
  p += strlen(pat);
  while (*p == ' ') p++;
  return p;
}

/* base64 token as 32 lowercase hex chars; 0 on failure */
static int tok_hex(const char* v, char out[33]) {
  if (*v == '"') {
    if (strlen(v) >= 32 && v[32] == '"' && !v[33]) { /* already hex? */ }
    v++;
  }
  if (strlen(v) >= 32 && !strncmp(v + 32, "\"", 1)) {
    int ok = 1;
    for (int i = 0; i < 32; i++) if (hexval(v[i]) < 0) { ok = 0; break; }
    if (ok) { for (int i = 0; i < 32; i++) out[i] = (char)tolower(v[i]); out[32] = 0; return 1; }
  }
  if (*v == '"') {
    v++;
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned char bin[32];
    int n = 0, bits = 0, acc = 0;
    for (; *v && *v != '"'; v++) {
      if (*v == '=') break;
      const char* q = strchr(t, *v);
      if (!q) return 0;
      acc = (acc << 6) | (int)(q - t);
      bits += 6;
      if (bits >= 8) { bits -= 8; if (n < 32) bin[n++] = (acc >> bits) & 0xff; }
    }
    if (n != 16) return 0;
    for (int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", bin[i]);
    out[32] = 0;
    return 1;
  }
  return 0;
}

static int ipv4_hex(const char* v, char out[9], int* last) {
  int o[4];
  if (sscanf(v, "%d.%d.%d.%d", &o[0], &o[1], &o[2], &o[3]) != 4) return 0;
  for (int i = 0; i < 4; i++) if (o[i] < 0 || o[i] > 255) return 0;
  snprintf(out, 9, "%02x%02x%02x%02x", o[0], o[1], o[2], o[3]);
  *last = o[3];
  return 1;
}

static char g_fqdn[160];
static int g_fqdn_ok;

/* Derive `gameFqdn` into g_fqdn.  Same transform as the extension:
 *   token-hex - rcc-ip-hex - netstack-port-hex . (udmux-ip last octet + 1) . qdmux
 * The browser endpoint lives at the odd/even neighbour of the native UDMUX
 * address; only odd, non-.255 sources have one. */
static int derive_fqdn(const char* js) {
  const char* tv = json_val(js, "NetStackTokenValue");
  if (!tv) tv = json_val(js, "TokenValue");
  const char* ma = json_val(js, "MachineAddress");
  if (ma && *ma == '"') ma++;
  const char* np = json_val(js, "NetStackPort");
  const char* ud = strstr(js, "\"UdmuxEndpoints\"");
  if (ud) { ud = strstr(ud, "\"Address\":"); }
  if (!tv || !ma || !np || !ud) return 0;

  char tok[33], rh[9], vh[9];
  int rlast, vlast;
  if (!tok_hex(tv, tok)) return 0;
  if (!ipv4_hex(ma, rh, &rlast)) return 0;
  ud += strlen("\"Address\":");
  while (*ud == ' ') ud++;
  if (*ud != '"') return 0;
  ud++;
  if (!ipv4_hex(ud, vh, &vlast)) return 0;
  if ((vlast & 1) == 0 || vlast == 255) return 0;

  long port = strtol(np, NULL, 10);
  if (port < 1 || port > 65535) return 0;
  vh[6] = "0123456789abcdef"[(vlast + 1) >> 4];
  vh[7] = "0123456789abcdef"[(vlast + 1) & 15];

  snprintf(g_fqdn, sizeof g_fqdn, "%s-%s-%04lx.%s.qdmux.roblox.com", tok, rh, port, vh);
  g_fqdn_ok = 1;
  return 1;
}

/* Rewrite a join response: put the derived host into joinScript. */
static char* patch_join(const char* body, size_t len, size_t* outlen) {
  if (!body || !strstr(body, "\"joinScript\"")) return NULL;
  const char* js = strstr(body, "\"joinScript\"");
  js = strchr(js, '{');
  if (!js) return NULL;
  if (strstr(js, "\"gameFqdn\":\"http")) return NULL;   /* already present */
  if (!derive_fqdn(js)) return NULL;

  const char* fld = strstr(js, "\"gameFqdn\"");
  char* out = malloc(len + sizeof g_fqdn + 48);
  if (!out) return NULL;
  size_t off;
  if (fld) {
    const char* v = strchr(fld, ':');
    const char* end = v ? v + 1 : NULL;
    while (end && *end == ' ') end++;
    if (end && !strncmp(end, "null", 4)) {
      off = (size_t)(end - body);
      memcpy(out, body, off);
      off += (size_t)snprintf(out + off, sizeof g_fqdn + 32, "\"%s\"", g_fqdn);
      memcpy(out + off, end + 4, len - (size_t)(end + 4 - body));
      off += len - (size_t)(end + 4 - body);
      *outlen = off;
      return out;
    }
  }
  off = (size_t)(js - body) + 1;              /* just inside the object */
  memcpy(out, body, off);
  off += (size_t)snprintf(out + off, sizeof g_fqdn + 32, "\"gameFqdn\":\"%s\",", g_fqdn);
  memcpy(out + off, js + 1, len - (size_t)(js + 1 - body));
  off += len - (size_t)(js + 1 - body);
  *outlen = off;
  return out;
}

/* --------------------------------------------------------------- request -- */

static void call_cb(u32 fetch, u32 idx) {
  if (!idx || idx >= g_inst->w2c_0x5F_indirect_function_table.size) return;
  wasm_rt_funcref_t* e = &g_inst->w2c_0x5F_indirect_function_table.data[idx];
  if (e->func) ((void (*)(void*, u32))e->func)(e->module_instance, fetch);
}

static char* read_file(const char* p, size_t* n) {
  pl_file* f = pl_open_read(p);
  if (!f) return NULL;
  int64_t sz = pl_size(f);
  if (sz < 0 || (uint64_t)sz > MAX_BODY) { pl_close(f); return NULL; }
  char* b = malloc((size_t)sz + 1);
  if (!b) { pl_close(f); return NULL; }
  size_t got = pl_read(f, b, (size_t)sz);
  pl_close(f);
  b[got] = 0;
  *n = got;
  return b;
}

void wc_impl_emscripten_start_fetch(struct w2c_env* a0, u32 fetch) {
  (void)a0;
  u32 urlp = *(u32*)wc_ptr(fetch + 8);
  u32 attr = fetch + FETCH_ATTR;
  const char* method = wc_str(attr);
  const char* url = wc_str(urlp);
  u32 cbs = *(u32*)wc_ptr(attr + 48);       /* onreadystatechange */
  u32 ok = *(u32*)wc_ptr(attr + 36);
  u32 err = *(u32*)wc_ptr(attr + 40);

  if (!url || !*url) { hdr_set(fetch, NULL); call_cb(fetch, err); return; }
  if (!method || !*method) method = "GET";
  fprintf(stderr, "[http] %s %s\n", method, url);

  int is_join = host_is(url, "gamejoin.roblox.com") && strstr(url, "/join");
  int roblox = host_is(url, "roblox.com") || strstr(url, ".roblox.com");

  /* header pairs: [name, value] x N, NULL terminated */
  u32 hp = *(u32*)wc_ptr(attr + 76);
  const char* argv[192];
  char* hdr_store[48];
  int ai = 0, hi = 0;
  static const char* base[] = { "curl", "-sS", "-L", "--max-time", "120" };
  for (unsigned i = 0; i < sizeof base / sizeof *base; i++) argv[ai++] = base[i];

  char hdrtmp[] = "/tmp/opencode/hdr.XXXXXX";
  int hfd = mkstemp(hdrtmp);
  if (hfd >= 0) close(hfd);
  (void)hdr_store;
  argv[ai++] = "-D"; argv[ai++] = hdrtmp;

  argv[ai++] = "-X"; argv[ai++] = method;

  char* bodyin = NULL;
  u32 rp = *(u32*)wc_ptr(attr + 84);
  u32 rl = *(u32*)wc_ptr(attr + 88);
  if (rp && rl) {
    char btmp[] = "/tmp/opencode/body.XXXXXX";
    int fd = mkstemp(btmp);
    if (fd >= 0) {
      write(fd, wc_ptr(rp), rl);
      close(fd);
      bodyin = strdup(btmp);
      argv[ai++] = "--data-binary"; argv[ai++] = bodyin;
    }
  }

  char* cookie_hdr = NULL;
  if (roblox && *cookie_value()) {
    size_t n = strlen(cookie_value()) + 32;
    cookie_hdr = malloc(n);
    snprintf(cookie_hdr, n, "Cookie: .ROBLOSECURITY=%s", cookie_value());
    argv[ai++] = "-H"; argv[ai++] = cookie_hdr;
  }
  if (is_join) {
    argv[ai++] = "-H"; argv[ai++] = "rbx-user-agent: Roblox/Darwin";
    argv[ai++] = "-H"; argv[ai++] = "Origin: https://play.roblox.com";
    argv[ai++] = "-H"; argv[ai++] = "Referer: https://play.roblox.com/";
  }

  int have_ua = 0, have_cookie = !roblox;
  for (u32 p = hp; p; p += 8) {
    u32 n = *(u32*)wc_ptr(p), v = *(u32*)wc_ptr(p + 4);
    if (!n) break;
    const char* hn = wc_str(n);
    const char* hv = v ? wc_str(v) : "";
    if (!hn || !*hn) break;
    if (!strcasecmp(hn, "rbx-user-agent")) {
      if (is_join) continue;                 /* ours wins */
      have_ua = 1;
    }
    if (!strcasecmp(hn, "cookie")) {
      if (roblox) continue;                  /* ours wins */
      have_cookie = 1;
    }
    size_t l = strlen(hn) + strlen(hv) + 3;
    char* b = malloc(l);
    snprintf(b, l, "%s: %s", hn, hv);
    if (hi < 48) hdr_store[hi++] = b;
    argv[ai++] = "-H"; argv[ai++] = b;
  }
  if (!have_ua) { argv[ai++] = "-A"; argv[ai++] =
      "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/142.0.0.0 Safari/537.36"; }
  (void)have_cookie;

  char outtmp[] = "/tmp/opencode/out.XXXXXX";
  int ofd = mkstemp(outtmp);
  if (ofd >= 0) close(ofd);
  argv[ai++] = "-o"; argv[ai++] = outtmp;
  argv[ai++] = "-w"; argv[ai++] = "%{http_code}";
  argv[ai++] = url;
  argv[ai] = NULL;

  char codetmp[] = "/tmp/opencode/code.XXXXXX";
  int cfd = mkstemp(codetmp);
  if (cfd >= 0) close(cfd);

  int rc = pl_spawn_capture(argv, codetmp);

  long code = 0;
  size_t cn = 0;
  char* cb = read_file(codetmp, &cn);
  if (cb) { code = strtol(cb, NULL, 10); free(cb); }

  size_t blen = 0;
  char* body = read_file(outtmp, &blen);
  if (rc != 0 && !body) code = 0;

  if (body && code >= 200 && code < 300 && is_join) {
    size_t plen = 0;
    char* patched = patch_join(body, blen, &plen);
    if (patched) { free(body); body = patched; blen = plen;
      fprintf(stderr, "[http] injected gameFqdn %s\n", g_fqdn); }
  }

  size_t hn = 0;
  char* hb = read_file(hdrtmp, &hn);
  hdr_set(fetch, hb);
  free(hb);

  /* fill the fetch object the way fetchXHR's completion does */
  u32 data = 0;
  if (body && blen) {
    data = w2c_WebClient_malloc(g_inst, (u32)blen);
    if (data) memcpy(wc_ptr(data), body, blen);
  }
  *(u32*)wc_ptr(fetch + 12) = data;
  *(u64*)wc_ptr(fetch + 16) = body ? blen : 0;     /* numBytes */
  *(u64*)wc_ptr(fetch + 24) = 0;                   /* dataOffset */
  *(u64*)wc_ptr(fetch + 32) = body ? blen : 0;     /* total */
  *(u16*)wc_ptr(fetch + 40) = 4;                   /* readyState DONE */
  *(u16*)wc_ptr(fetch + 42) = (u16)code;
  { const char* st = code == 200 ? "OK" : code ? "Error" : "NetworkError";
    snprintf(wc_ptr(fetch + 44), 64, "%s", st); }
  fprintf(stderr, "[http] -> %ld %zu bytes\n", code, blen);

  call_cb(fetch, cbs);
  if (code >= 200 && code < 300) call_cb(fetch, ok);
  else call_cb(fetch, err);

  free(body); free(cookie_hdr);
  for (int i = 0; i < hi; i++) free(hdr_store[i]);
  pl_remove(outtmp); pl_remove(codetmp); pl_remove(hdrtmp);
  if (bodyin) pl_remove(bodyin);          /* before free: pl_remove reads it */
  free(bodyin);
}

void wc_impl_emscripten_fetch_free(struct w2c_env* a0, u32 a1) {
  (void)a0; (void)a1;
  hdr_set(a1, NULL);
}

u32 wc_impl_0x5Femscripten_fetch_get_response_headers_length(struct w2c_env* a0, u32 a1) {
  (void)a0;
  const char* h = hdr_get(a1);
  return h ? (u32)strlen(h) : 0;
}

u32 wc_impl_0x5Femscripten_fetch_get_response_headers(struct w2c_env* a0, u32 a1, u32 a2,
                                                      u32 a3) {
  (void)a0;
  const char* h = hdr_get(a1);
  if (!h) return 0;
  size_t n = strlen(h);
  if (n > a3) n = a3;
  memcpy(wc_ptr(a2), h, n);
  return (u32)n;
}
