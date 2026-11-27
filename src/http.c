// Port of v3-wasm2c/http.c to the clean C11 host.  libcurl replaces the
// spawned curl process; yyjson replaces the hand-rolled join-patch scanner.
//
// The request is still synchronous (curl_easy_perform), but completion
// callbacks are *queued* here and delivered by weblox_http_pump(), matching the
// asynchronous XHR shape the guest expects.
#include "http.h"
#include "guest.h"
#include "fs.h"

#include <curl/curl.h>
#include <yyjson.h>

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===========================================================================
 * Guest struct layout.  Every emscripten_fetch_t / attribute offset used by
 * this file lives in this one block, copied from the fetchXHR layout comment
 * in v3-wasm2c/http.c.  Do not scatter numeric offsets through the code.
 * ========================================================================= */
enum weblox_fetch_layout {
  FETCH_URL              = 8,   /* char* url */
  FETCH_DATA             = 12,  /* char* data */
  FETCH_NUM_BYTES        = 16,  /* u64 numBytes */
  FETCH_DATA_OFFSET      = 24,  /* u64 dataOffset */
  FETCH_TOTAL            = 32,  /* u64 total */
  FETCH_READY_STATE      = 40,  /* u16 readyState (4 == DONE) */
  FETCH_STATUS           = 42,  /* u16 status */
  FETCH_STATUS_TEXT      = 44,  /* char statusText[64] */
  FETCH_ATTR             = 108, /* EmscriptenFetchCallArg attributes */
  ATTR_METHOD            = 0,   /* char method[32] */
  ATTR_ONSUCCESS         = 36,
  ATTR_ONERROR           = 40,
  ATTR_ONPROGRESS        = 44,
  ATTR_ONREADYSTATECHANGE = 48,
  ATTR_ATTRIBUTES        = 52,
  ATTR_TIMEOUT           = 56,
  ATTR_WITHCREDENTIALS   = 60,
  ATTR_USERNAME          = 68,
  ATTR_PASSWORD          = 72,
  ATTR_HEADERS           = 76,
  ATTR_MIME              = 80,
  ATTR_BODY              = 84,
  ATTR_SIZE              = 88,
  HEADER_PAIR_STRIDE     = 8,   /* [name_ptr, value_ptr] pairs */
  STATUS_TEXT_CAP        = 64
};
/* ========================================================================= */

#define MAX_BODY     (256u << 20)
#define MAX_HEADERS  64
#define MAX_CBS      64
#define MAX_GHDRS    128
#define DEFAULT_UA \
  "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) " \
  "Chrome/142.0.0.0 Safari/537.36"

// All guest reads/writes funnel through guest.h's guest_ptr().  These small
// wrappers only add the width conversions the ABI needs.
static int gread(u32 addr, void* dst, size_t n) {
  const unsigned char* p = guest_ptr(addr, n);
  if (!p) return 0;
  memcpy(dst, p, n);
  return 1;
}
static u32 gu32(u32 addr) {
  u32 v = 0;
  (void)gread(addr, &v, 4);
  return v;
}
static void put_u16(u32 addr, u16 v) {
  unsigned char* p = guest_ptr(addr, 2);
  if (p) memcpy(p, &v, 2);
}

// One C11 spinlock guards both tables; the reference used pthread mutexes.
static atomic_flag g_lock = ATOMIC_FLAG_INIT;
static void lock_acquire(void) {
  while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire)) {
  }
}
static void lock_release(void) {
  atomic_flag_clear_explicit(&g_lock, memory_order_release);
}

static char* dup_str(const char* s) {
  size_t n = strlen(s) + 1;
  char* p = (char*)malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

// --------------------------------------------------------- response headers --
typedef struct {
  u32 fetch;
  char* text;
} hdr_t;
static hdr_t g_hdr[MAX_HEADERS];

static void hdr_set(u32 fetch, const char* text) {
  lock_acquire();
  for (int i = 0; i < MAX_HEADERS; ++i) {
    if (g_hdr[i].fetch == fetch && g_hdr[i].text) {
      free(g_hdr[i].text);
      g_hdr[i].text = NULL;
      break;
    }
  }
  if (text) {
    for (int i = 0; i < MAX_HEADERS; ++i) {
      if (!g_hdr[i].text) {
        g_hdr[i].fetch = fetch;
        g_hdr[i].text = dup_str(text);
        break;
      }
    }
  }
  lock_release();
}

static const char* hdr_get(u32 fetch) {
  lock_acquire();
  const char* result = NULL;
  for (int i = 0; i < MAX_HEADERS; ++i)
    if (g_hdr[i].fetch == fetch && g_hdr[i].text) result = g_hdr[i].text;
  lock_release();
  return result;
}

// ------------------------------------------------------- completion queue ---
typedef struct {
  u32 fetch;
  u32 index;  // indirect function table index from the guest attribute block
} cb_t;
static cb_t g_cb[MAX_CBS];
static unsigned g_cb_head;
static unsigned g_cb_count;

static void cb_push(u32 fetch, u32 index) {
  if (!index) return;
  lock_acquire();
  if (g_cb_count >= MAX_CBS) {
    fprintf(stderr, "[http] completion queue full, dropping fetch=%u\n", fetch);
  } else {
    unsigned slot = (g_cb_head + g_cb_count) % MAX_CBS;
    g_cb[slot].fetch = fetch;
    g_cb[slot].index = index;
    ++g_cb_count;
  }
  lock_release();
}

static int cb_pop(cb_t* out) {
  lock_acquire();
  int ok = 0;
  if (g_cb_count) {
    *out = g_cb[g_cb_head];
    g_cb_head = (g_cb_head + 1) % MAX_CBS;
    --g_cb_count;
    ok = 1;
  }
  lock_release();
  return ok;
}

void weblox_http_drop(u32 fetch) {
  lock_acquire();
  unsigned i = 0;
  while (i < g_cb_count) {
    unsigned slot = (g_cb_head + i) % MAX_CBS;
    if (g_cb[slot].fetch == fetch) {
      for (unsigned j = i; j + 1 < g_cb_count; ++j) {
        unsigned dst = (g_cb_head + j) % MAX_CBS;
        unsigned src = (g_cb_head + j + 1) % MAX_CBS;
        g_cb[dst] = g_cb[src];
      }
      --g_cb_count;
    } else {
      ++i;
    }
  }
  lock_release();
}

static int call_table_index(w2c_WebClient* inst, u32 fetch, u32 index) {
  if (!inst || !index) return 0;
  wasm_rt_funcref_table_t* table = &inst->w2c_0x5F_indirect_function_table;
  if (!table->data || index >= table->size) {
    fprintf(stderr, "[http] callback index %u out of table bounds (%u)\n",
            index, table->size);
    return 0;
  }
  wasm_rt_funcref_t* entry = &table->data[index];
  if (!entry->func) return 0;
  ((void (*)(void*, u32))entry->func)(entry->module_instance, fetch);
  return 1;
}

int weblox_http_pump(w2c_WebClient* inst) {
  cb_t item;
  while (cb_pop(&item)) {
    if (!guest_ptr(item.fetch, FETCH_STATUS_TEXT + STATUS_TEXT_CAP)) {
      fprintf(stderr, "[http] dropping callback for stale fetch %u\n", item.fetch);
      continue;
    }
    if (call_table_index(inst, item.fetch, item.index)) return 1;
  }
  return 0;
}

// ------------------------------------------------------------ curl buffers --
typedef struct {
  char* data;
  size_t len;
  size_t cap;
  size_t limit;
  int overflow;
} buf_t;

static int buf_append(buf_t* b, const char* src, size_t n) {
  if (b->overflow) return 0;
  if (n > b->limit - b->len) {
    b->overflow = 1;
    return 0;
  }
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + n + 1) {
      if (cap > b->limit / 2) {
        cap = b->limit + 1;
        break;
      }
      cap *= 2;
    }
    char* grown = (char*)realloc(b->data, cap);
    if (!grown) {
      b->overflow = 1;
      return 0;
    }
    b->data = grown;
    b->cap = cap;
  }
  memcpy(b->data + b->len, src, n);
  b->len += n;
  b->data[b->len] = 0;
  return 1;
}

static size_t curl_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
  buf_t* b = (buf_t*)userdata;
  size_t n = size * nmemb;
  return buf_append(b, ptr, n) ? n : 0;
}

// ------------------------------------------------------------------ cookie --
static void clean_cookie(char* s) {
  size_t r = 0, w = 0;
  for (; s[r]; ++r) {
    unsigned char c = (unsigned char)s[r];
    if (c == '\r' || c == '\n') continue;  // never inject a second header
    s[w++] = (char)c;
  }
  s[w] = 0;
  while (w && isspace((unsigned char)s[w - 1])) s[--w] = 0;
  size_t start = 0;
  while (s[start] && isspace((unsigned char)s[start])) ++start;
  if (start) memmove(s, s + start, w - start + 1);
}

static const char* cookie_value(void) {
  static char buf[4096];
  static int loaded;
  if (loaded) return buf;
  loaded = 1;
  const char* e = getenv("WC_COOKIE");
  if (e && *e) {
    snprintf(buf, sizeof buf, "%s", e);
  } else {
    const char* p = getenv("WC_COOKIE_FILE");
    if (!p || !*p) p = "king.txt";
    long n = weblox_fs_read_file(p, buf, sizeof buf - 1);
    if (n <= 0) {
      buf[0] = 0;
      return buf;
    }
    buf[n] = 0;
  }
  clean_cookie(buf);
  return buf;
}

// ------------------------------------------------------------------- URLs ---
static int char_equal_ci(char a, char b) {
  return tolower((unsigned char)a) == tolower((unsigned char)b);
}

static int str_equal_ci(const char* a, const char* b) {
  while (*a && *b && char_equal_ci(*a, *b)) { ++a; ++b; }
  return !*a && !*b;
}

static int host_is(const char* url, const char* want) {
  const char* p = strstr(url, "://");
  p = p ? p + 3 : url;
  size_t n = strcspn(p, "/:?#");
  if (n != strlen(want)) return 0;
  for (size_t i = 0; i < n; ++i)
    if (!char_equal_ci(p[i], want[i])) return 0;
  return 1;
}

// ------------------------------------------------------- join FQDN patching --
static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Token is either 32 hex chars or 16 raw bytes in base64.  yyjson already
// unquoted the string, so there are no JSON quotes to skip here.
static int token_hex(const char* s, char out[33]) {
  if (!s || !*s) return 0;
  if (strlen(s) >= 32) {
    int ok = 1;
    for (int i = 0; i < 32; ++i)
      if (hexval((unsigned char)s[i]) < 0) { ok = 0; break; }
    if (ok) {
      for (int i = 0; i < 32; ++i)
        out[i] = (char)tolower((unsigned char)s[i]);
      out[32] = 0;
      return 1;
    }
  }
  static const char* table =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  unsigned char bin[16];
  int count = 0, bits = 0, acc = 0;
  for (; *s && *s != '=' && *s != ',' && *s != '}' && !isspace((unsigned char)*s); ++s) {
    const char* q = strchr(table, *s);
    if (!q) return 0;
    acc = (acc << 6) | (int)(q - table);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (count < 16) bin[count++] = (unsigned char)((acc >> bits) & 0xff);
    }
  }
  if (count != 16) return 0;
  for (int i = 0; i < 16; ++i)
    snprintf(out + i * 2, 3, "%02x", bin[i]);
  out[32] = 0;
  return 1;
}

static int ipv4_hex(const char* s, char out[9], int* last) {
  if (!s) return 0;
  unsigned o[4];
  if (sscanf(s, "%u.%u.%u.%u", &o[0], &o[1], &o[2], &o[3]) != 4) return 0;
  for (int i = 0; i < 4; ++i)
    if (o[i] > 255) return 0;
  snprintf(out, 9, "%02x%02x%02x%02x", o[0], o[1], o[2], o[3]);
  *last = (int)o[3];
  return 1;
}

static const char* udmux_address(const yyjson_val* join) {
  const yyjson_val* endpoints = yyjson_obj_get(join, "UdmuxEndpoints");
  if (!endpoints) return NULL;
  const yyjson_val* first = yyjson_is_arr(endpoints)
                                ? yyjson_arr_get(endpoints, 0)
                                : endpoints;
  return yyjson_get_str(yyjson_obj_get(first, "Address"));
}

static int netstack_port(const yyjson_val* join) {
  const yyjson_val* v = yyjson_obj_get(join, "NetStackPort");
  if (!v) return 0;
  if (yyjson_is_int(v)) {
    u64 port = yyjson_get_uint(v);
    return (port > 0 && port <= 65535) ? (int)port : 0;
  }
  const char* s = yyjson_get_str(v);
  if (!s) return 0;
  long port = strtol(s, NULL, 10);
  return (port > 0 && port <= 65535) ? (int)port : 0;
}

// Same transform as the extension:
//   token-hex - rcc-ip-hex - netstack-port-hex . (udmux last octet + 1) . qdmux
static int derive_fqdn(const yyjson_val* join, char* out, size_t cap) {
  const char* token = yyjson_get_str(yyjson_obj_get(join, "NetStackTokenValue"));
  if (!token) token = yyjson_get_str(yyjson_obj_get(join, "TokenValue"));
  const char* machine = yyjson_get_str(yyjson_obj_get(join, "MachineAddress"));
  const char* udmux = udmux_address(join);
  if (!token || !machine || !udmux) return 0;
  int port = netstack_port(join);
  if (!port) return 0;

  char tok[33], rh[9], vh[9];
  int rlast = 0, vlast = 0;
  (void)rlast;
  if (!token_hex(token, tok)) return 0;
  if (!ipv4_hex(machine, rh, &rlast)) return 0;
  if (!ipv4_hex(udmux, vh, &vlast)) return 0;
  if ((vlast & 1) == 0 || vlast == 255) return 0;

  static const char* hex = "0123456789abcdef";
  int next = vlast + 1;
  vh[6] = hex[(next >> 4) & 15];
  vh[7] = hex[next & 15];
  snprintf(out, cap, "%s-%s-%04x.%s.qdmux.roblox.com", tok, rh, port, vh);
  return 1;
}

static char g_last_fqdn[160];

// Parse the join response with yyjson and inject gameFqdn into joinScript.
// Returns a malloc'd minified document, or NULL when no patch was needed.
static char* patch_join(const char* body, size_t len, size_t* outlen) {
  yyjson_doc* doc = yyjson_read(body, len, 0);
  if (!doc) {
    fprintf(stderr, "[http] join reply is not JSON, skipping gameFqdn patch\n");
    return NULL;
  }
  yyjson_val* root = yyjson_doc_get_root(doc);
  yyjson_val* join = yyjson_obj_get(root, "joinScript");
  if (!yyjson_is_obj(join)) {
    yyjson_doc_free(doc);
    return NULL;
  }
  const char* existing = yyjson_get_str(yyjson_obj_get(join, "gameFqdn"));
  if (existing && !strncmp(existing, "http", 4)) {
    yyjson_doc_free(doc);
    return NULL;
  }
  char fqdn[sizeof g_last_fqdn];
  if (!derive_fqdn(join, fqdn, sizeof fqdn)) {
    yyjson_doc_free(doc);
    return NULL;
  }

  yyjson_mut_doc* mdoc = yyjson_doc_mut_copy(doc, NULL);
  yyjson_doc_free(doc);
  if (!mdoc) return NULL;
  yyjson_mut_val* mjoin = yyjson_mut_obj_get(yyjson_mut_doc_get_root(mdoc),
                                             "joinScript");
  if (!yyjson_mut_is_obj(mjoin)) {
    yyjson_mut_doc_free(mdoc);
    return NULL;
  }
  yyjson_mut_obj_put(mjoin, yyjson_mut_strcpy(mdoc, "gameFqdn"),
                     yyjson_mut_strcpy(mdoc, fqdn));
  size_t written = 0;
  char* out = yyjson_mut_write(mdoc, 0, &written);
  yyjson_mut_doc_free(mdoc);
  if (!out) return NULL;
  snprintf(g_last_fqdn, sizeof g_last_fqdn, "%s", fqdn);
  *outlen = written;
  return out;
}

// ------------------------------------------------------------ shared GET ---
int weblox_http_get(const char* url, size_t max_bytes, char** out,
                    size_t* out_len) {
  *out = NULL;
  *out_len = 0;
  if (!url || !*url) return -1;
  CURL* curl = curl_easy_init();
  if (!curl) return -1;
  buf_t body = {NULL, 0, 0, max_bytes ? max_bytes : MAX_BODY, 0};
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  CURLcode rc = curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  if (rc != CURLE_OK || body.overflow || !body.data) {
    fprintf(stderr, "[http] GET %s failed (%d)\n", url, (int)rc);
    free(body.data);
    return -1;
  }
  *out = body.data;
  *out_len = body.len;
  return 0;
}

// ------------------------------------------------------------ header list --
static int add_header(struct curl_slist** list, char* text, char** owned,
                      int* owned_count) {
  struct curl_slist* next = curl_slist_append(*list, text);
  if (!next) {
    free(text);
    return 0;
  }
  *list = next;
  owned[*owned_count] = text;
  ++*owned_count;
  return 1;
}

// --------------------------------------------------------------- imports ---
void w2c_env_emscripten_start_fetch(struct w2c_env* env, u32 fetch) {
  (void)env;
  u32 attr = fetch + FETCH_ATTR;
  if (!guest_ptr(fetch, FETCH_ATTR + ATTR_SIZE + 4)) {
    fprintf(stderr, "[http] invalid fetch object %u\n", fetch);
    return;
  }
  const char* url = guest_str(gu32(fetch + FETCH_URL));
  const char* method = guest_str(attr + ATTR_METHOD);
  if (!method || !*method) method = "GET";
  u32 cb_ready = gu32(attr + ATTR_ONREADYSTATECHANGE);
  u32 cb_ok = gu32(attr + ATTR_ONSUCCESS);
  u32 cb_err = gu32(attr + ATTR_ONERROR);

  if (!*url) {
    fprintf(stderr, "[http] empty URL for fetch %u\n", fetch);
    hdr_set(fetch, NULL);
    cb_push(fetch, cb_err);
    return;
  }
  fprintf(stderr, "[http] %s %s\n", method, url);

  int is_join = host_is(url, "gamejoin.roblox.com") && strstr(url, "/join");
  int roblox = host_is(url, "roblox.com") || strstr(url, ".roblox.com");

  CURL* curl = curl_easy_init();
  if (!curl) {
    fprintf(stderr, "[http] curl init failed\n");
    hdr_set(fetch, NULL);
    cb_push(fetch, cb_err);
    return;
  }

  struct curl_slist* list = NULL;
  char* owned[MAX_GHDRS + 8];
  int owned_count = 0;
  int have_ua = 0;
  const char* cookie = cookie_value();

#define ADD_OWNED(TEXT) \
  do { \
    char* owned_text_ = dup_str(TEXT); \
    if (owned_text_ && owned_count < (int)(sizeof owned / sizeof *owned)) \
      add_header(&list, owned_text_, owned, &owned_count); \
    else free(owned_text_); \
  } while (0)

  if (roblox && *cookie) {
    size_t n = strlen(cookie) + 40;
    char* header = (char*)malloc(n);
    if (header) {
      if (!strncmp(cookie, ".ROBLOSECURITY=", 15))
        snprintf(header, n, "Cookie: %s", cookie);
      else
        snprintf(header, n, "Cookie: .ROBLOSECURITY=%s", cookie);
      if (owned_count < (int)(sizeof owned / sizeof *owned))
        add_header(&list, header, owned, &owned_count);
      else
        free(header);
    }
  }
  if (is_join) {
    // Native identity is required or gamejoin returns "Playtesters Only".
    ADD_OWNED("rbx-user-agent: Roblox/Darwin");
    ADD_OWNED("Origin: https://play.roblox.com");
    ADD_OWNED("Referer: https://play.roblox.com/");
  }

  u32 hp = gu32(attr + ATTR_HEADERS);
  for (u32 p = hp, count = 0; p && count < MAX_GHDRS;
       p += HEADER_PAIR_STRIDE, ++count) {
    if (!guest_ptr(p, 8)) break;
    u32 namep = gu32(p);
    u32 valp = gu32(p + 4);
    if (!namep) break;
    const char* name = guest_str(namep);
    const char* value = valp ? guest_str(valp) : "";
    if (!*name) break;
    if (is_join && str_equal_ci(name, "rbx-user-agent")) continue;
    if (roblox && str_equal_ci(name, "cookie")) continue;
    if (str_equal_ci(name, "rbx-user-agent")) have_ua = 1;
    size_t n = strlen(name) + strlen(value) + 3;
    char* header = (char*)malloc(n);
    if (!header) break;
    snprintf(header, n, "%s: %s", name, value);
    if (owned_count < (int)(sizeof owned / sizeof *owned)) {
      add_header(&list, header, owned, &owned_count);
    } else {
      free(header);
      break;
    }
  }
#undef ADD_OWNED

  buf_t body = {NULL, 0, 0, MAX_BODY, 0};
  buf_t headers = {NULL, 0, 0, MAX_BODY, 0};
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_write);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headers);
  if (list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
  if (!have_ua) curl_easy_setopt(curl, CURLOPT_USERAGENT, DEFAULT_UA);

  u32 bodyp = gu32(attr + ATTR_BODY);
  u32 bodylen = gu32(attr + ATTR_SIZE);
  if (bodyp && bodylen) {
    const unsigned char* bytes = guest_ptr(bodyp, bodylen);
    if (bytes) {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bytes);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)bodylen);
    } else {
      fprintf(stderr, "[http] invalid request body %u/%u\n", bodyp, bodylen);
    }
  }

  CURLcode rc = curl_easy_perform(curl);
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(curl);
  if (list) curl_slist_free_all(list);
  for (int i = 0; i < owned_count; ++i) free(owned[i]);

  if (rc != CURLE_OK && !body.data) code = 0;
  if (body.data && code >= 200 && code < 300 && is_join) {
    size_t patched_len = 0;
    char* patched = patch_join(body.data, body.len, &patched_len);
    if (patched) {
      free(body.data);
      body.data = patched;
      body.len = patched_len;
      fprintf(stderr, "[http] injected gameFqdn %s\n", g_last_fqdn);
    }
  }

  u32 data = 0;
  if (body.data && body.len && weblox_instance) {
    data = w2c_WebClient_malloc(weblox_instance, (u32)body.len);
    if (data) {
      unsigned char* dst = guest_ptr(data, body.len);
      if (dst) memcpy(dst, body.data, body.len);
      else data = 0;
    }
  }
  guest_put_u32(fetch + FETCH_DATA, data);
  guest_put_u64(fetch + FETCH_NUM_BYTES, body.data ? body.len : 0);
  guest_put_u64(fetch + FETCH_DATA_OFFSET, 0);
  guest_put_u64(fetch + FETCH_TOTAL, body.data ? body.len : 0);
  put_u16(fetch + FETCH_READY_STATE, 4);
  put_u16(fetch + FETCH_STATUS,
                (u16)((code > 0 && code <= 65535) ? code : 0));
  unsigned char* status_text = guest_ptr(fetch + FETCH_STATUS_TEXT,
                                         STATUS_TEXT_CAP);
  if (status_text)
    snprintf((char*)status_text, STATUS_TEXT_CAP, "%s",
             code == 200 ? "OK" : code ? "Error" : "NetworkError");
  fprintf(stderr, "[http] -> %ld %zu bytes\n", code, body.len);

  hdr_set(fetch, headers.data);
  free(headers.data);
  free(body.data);

  // XHR never invokes its handlers inline; the host event loop pumps them.
  cb_push(fetch, cb_ready);
  cb_push(fetch, (code >= 200 && code < 300) ? cb_ok : cb_err);
}

void w2c_env_emscripten_fetch_free(struct w2c_env* env, u32 fetch) {
  (void)env;
  hdr_set(fetch, NULL);
  weblox_http_drop(fetch);
}

u32 w2c_env_0x5Femscripten_fetch_get_response_headers_length(struct w2c_env* env,
                                                             u32 fetch) {
  (void)env;
  const char* text = hdr_get(fetch);
  return text ? (u32)strlen(text) : 0;
}

u32 w2c_env_0x5Femscripten_fetch_get_response_headers(struct w2c_env* env,
                                                      u32 fetch, u32 dst,
                                                      u32 cap) {
  (void)env;
  const char* text = hdr_get(fetch);
  if (!text) return 0;
  size_t n = strlen(text);
  if (n > cap) n = cap;
  unsigned char* out = guest_ptr(dst, n);
  if (!out) return 0;
  memcpy(out, text, n);
  return (u32)n;
}

u32 w2c_env_0x5F_syscall_getegid32(struct w2c_env* env) {
  (void)env;
  return 0;
}
u32 w2c_env_0x5F_syscall_geteuid32(struct w2c_env* env) {
  (void)env;
  return 0;
}
u32 w2c_env_0x5F_syscall_getgid32(struct w2c_env* env) {
  (void)env;
  return 0;
}
u32 w2c_env_0x5F_syscall_getuid32(struct w2c_env* env) {
  (void)env;
  return 0;
}
