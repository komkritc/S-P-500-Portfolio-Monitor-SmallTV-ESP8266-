#include "StorageManager.h"
#include <LittleFS.h>
#include "../config/AppConfig.h"
#include "../finance/Portfolio.h"

namespace storage {
static bool s_ok = false;
static uint8_t s_busy = 0;

struct BusyGuard { BusyGuard() { s_busy++; } ~BusyGuard() { s_busy--; } };

bool begin() {
  LittleFS.setConfig(LittleFSConfig(true));      // auto-format when the mount fails (first boot / corrupt FS)
  s_ok = LittleFS.begin();
  if (!s_ok) Serial.println(F("[fs] LittleFS mount FAILED"));
  return s_ok;
}
bool ok() { return s_ok; }
bool busy() { return s_busy != 0; }
void busyBegin() { s_busy++; }
void busyEnd() { if (s_busy) s_busy--; }

void fsInfo(uint32_t &total, uint32_t &used) {
  FSInfo i;
  if (s_ok && LittleFS.info(i)) { total = i.totalBytes; used = i.usedBytes; } else { total = used = 0; }
}

// Whole-file read into a heap buffer sized to the file (config files are tiny). Caller frees.
static char *readAll(const char *path, size_t &len, size_t maxLen = 4096) {
  len = 0;
  if (!s_ok) return nullptr;
  File f = LittleFS.open(path, "r");
  if (!f) return nullptr;
  size_t sz = f.size();
  if (sz == 0 || sz > maxLen) { f.close(); return nullptr; }
  char *buf = (char *)malloc(sz + 1);
  if (!buf) { f.close(); return nullptr; }
  len = f.readBytes(buf, sz);
  f.close();
  buf[len] = 0;
  return buf;
}

bool writeAtomic(const char *path, const uint8_t *data, size_t len) {
  if (!s_ok) return false;
  BusyGuard g;
  String tmp = String(path) + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  size_t w = f.write(data, len);
  f.close();
  if (w != len) { LittleFS.remove(tmp); return false; }
  LittleFS.remove(path);
  if (!LittleFS.rename(tmp, path)) { LittleFS.remove(tmp); return false; }
  return true;
}

struct StrSink { String *s; };
static void sinkToString(void *c, const char *d, size_t n) { static_cast<String *>(c)->concat(d, n); }

void loadSettings(Settings &s) {
  size_t n; char *b = readAll("/settings.json", n);
  if (!b) return;
  settingsLoadJson(s, b, n);
  free(b);
}

bool saveSettings(const Settings &s) {
  String out; out.reserve(320);
  { JsonWriter w(sinkToString, &out); settingsWrite(w, s, true, false); }
  return writeAtomic("/settings.json", (const uint8_t *)out.c_str(), out.length());
}

uint8_t loadPortfolio(AppState &a) {
  size_t n; char *b = readAll("/portfolio.json", n, 6144);
  if (!b) return 0;
  uint8_t c = portfolioLoad(a.positions, b, n, a.pmeta);
  free(b);
  return c;
}

bool savePortfolio(const AppState &a) {
  String out; out.reserve(80 + (size_t)a.posCount * 110);
  { JsonWriter w(sinkToString, &out); portfolioWrite(w, a.positions, a.posCount, a.pmeta); }
  return writeAtomic("/portfolio.json", (const uint8_t *)out.c_str(), out.length());
}

bool loadWifi(WifiCreds &w) {
  memset(&w, 0, sizeof w);
  size_t n; char *b = readAll("/wifi.json", n, 512);
  if (!b) return false;
  FlatJson j;
  bool ok = j.parse(b, n) && j.isStr("ssid") && strlen(j.str("ssid")) > 0 && strlen(j.str("ssid")) <= 32 && strlen(j.str("pass")) <= 64;
  if (ok) { strcpy(w.ssid, j.str("ssid")); strcpy(w.pass, j.str("pass")); }
  free(b);
  return ok;
}

bool saveWifi(const char *ssid, const char *pass) {
  String out; out.reserve(160);
  { JsonWriter w(sinkToString, &out); w.beginObj().kStr("ssid", ssid).kStr("pass", pass).endObj(); }
  return writeAtomic("/wifi.json", (const uint8_t *)out.c_str(), out.length());
}

bool eraseWifi() { return s_ok && (LittleFS.remove("/wifi.json") || !LittleFS.exists("/wifi.json")); }

// ---------------------------------------------------------------- cache.bin
// Layout: header | SymQuote[nq] | SP500Quote | for each range {n, base, okEpoch, t[n], v[n]} | crc32
static const uint32_t CACHE_MAGIC = 0x53503543;   // "SP5C"
struct CacheHdr { uint32_t magic; uint16_t version, nq, sizeofQuote, sizeofSp; };

static uint32_t crc32(const uint8_t *p, size_t n, uint32_t c = 0xFFFFFFFF) {
  while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
  return c;
}

struct BufW { uint8_t *p; size_t n, cap; bool bad;
  void put(const void *d, size_t l) { if (n + l > cap) { bad = true; return; } memcpy(p + n, d, l); n += l; } };

bool saveCache(const AppState &a) {
  size_t cap = sizeof(CacheHdr) + sizeof(SymQuote) * MAX_SYMBOLS + sizeof(SP500Quote) + RANGE_COUNT * 16 + CHART_POOL * 8 + 4;
  uint8_t *buf = (uint8_t *)malloc(cap);
  if (!buf) return false;
  BufW w{buf, 0, cap, false};
  CacheHdr h{CACHE_MAGIC, 3, a.book.n, (uint16_t)sizeof(SymQuote), (uint16_t)sizeof(SP500Quote)};
  w.put(&h, sizeof h);
  w.put(a.book.q, sizeof(SymQuote) * a.book.n);
  w.put(&a.sp, sizeof a.sp);
  for (uint8_t r = 0; r < RANGE_COUNT; r++) {
    const Chart &c = a.charts[r];
    uint16_t n = c.valid ? c.n : 0;
    w.put(&n, 2); w.put(&c.base, 4); w.put(&c.okEpoch, 4);
    w.put(c.t, (size_t)n * 4); w.put(c.v, (size_t)n * 4);
  }
  uint32_t crc = crc32(buf, w.n) ^ 0xFFFFFFFF;
  w.put(&crc, 4);
  bool ok = !w.bad && writeAtomic("/cache.bin", buf, w.n);
  free(buf);
  return ok;
}

bool loadCache(AppState &a) {
  size_t n; char *b = readAll("/cache.bin", n, 8192);
  if (!b) return false;
  const uint8_t *p = (const uint8_t *)b;
  bool ok = false;
  do {
    if (n < sizeof(CacheHdr) + 4) break;
    uint32_t crc; memcpy(&crc, p + n - 4, 4);
    if ((crc32(p, n - 4) ^ 0xFFFFFFFF) != crc) break;
    CacheHdr h; memcpy(&h, p, sizeof h);
    if (h.magic != CACHE_MAGIC || h.version != 3 || h.sizeofQuote != sizeof(SymQuote) || h.sizeofSp != sizeof(SP500Quote) || h.nq > MAX_SYMBOLS) break;
    size_t o = sizeof h, end = n - 4;
    if (o + (size_t)h.nq * sizeof(SymQuote) + sizeof(SP500Quote) > end) break;
    SymQuote q[MAX_SYMBOLS]; memcpy(q, p + o, sizeof(SymQuote) * h.nq); o += sizeof(SymQuote) * h.nq;
    SP500Quote sp; memcpy(&sp, p + o, sizeof sp); o += sizeof sp;
    // charts: validate everything before touching the live state
    uint16_t cn[RANGE_COUNT]; float cb[RANGE_COUNT]; uint32_t ce[RANGE_COUNT]; size_t toff[RANGE_COUNT];
    bool good = true;
    for (uint8_t r = 0; r < RANGE_COUNT && good; r++) {
      if (o + 10 > end) { good = false; break; }
      memcpy(&cn[r], p + o, 2); memcpy(&cb[r], p + o + 2, 4); memcpy(&ce[r], p + o + 6, 4); o += 10;
      if (cn[r] > RANGE_CAP[r] || o + (size_t)cn[r] * 8 > end) { good = false; break; }
      toff[r] = o; o += (size_t)cn[r] * 8;
    }
    if (!good) break;
    // apply: everything loaded from flash is marked fromCache -> stale until refreshed this boot
    for (uint8_t i = 0; i < h.nq; i++) {
      q[i].fromCache = true; q[i].failed = false; q[i].okMs = 0;
      SymQuote *dst = nullptr;
      for (uint8_t j = 0; j < a.book.n; j++) if (!strcmp(a.book.q[j].symbol, q[i].symbol)) dst = &a.book.q[j];
      if (dst && q[i].valid && isNum(q[i].price)) *dst = q[i];
    }
    if (sp.valid && isNum(sp.last)) { a.sp = sp; a.sp.fromCache = true; a.sp.failed = false; a.sp.okMs = 0; }
    for (uint8_t r = 0; r < RANGE_COUNT; r++) {
      Chart &c = a.charts[r];
      if (cn[r] < 2) continue;
      memcpy(c.t, p + toff[r], (size_t)cn[r] * 4);
      memcpy(c.v, p + toff[r] + (size_t)cn[r] * 4, (size_t)cn[r] * 4);
      c.n = cn[r]; c.base = cb[r]; c.okEpoch = ce[r]; c.valid = true; c.failed = false; c.fromCache = true; c.okMs = 0;
    }
    ok = true;
  } while (0);
  free(b);
  return ok;
}

bool eraseCache() { return s_ok && (LittleFS.remove("/cache.bin") || !LittleFS.exists("/cache.bin")); }

void eraseAll(bool keepWifi, bool keepPortfolio) {
  if (!s_ok) return;
  BusyGuard g;
  Dir d = LittleFS.openDir("/");
  while (d.next()) {
    String n = d.fileName();
    if (n.length() && n[0] != '/') n = "/" + n;
    if (keepWifi && n == "/wifi.json") continue;
    // the ledger and its in-flight journal describe the open positions: keep or drop them together
    if (keepPortfolio && (n == "/portfolio.json" || n == "/sells.jsonl" || n == "/txn.pending")) continue;
    LittleFS.remove(n);
  }
}
}  // namespace storage
