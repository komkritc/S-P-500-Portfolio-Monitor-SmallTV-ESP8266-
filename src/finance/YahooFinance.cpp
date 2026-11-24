#include "YahooFinance.h"
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <new>

namespace yahoo {

static const char HOST[] = "query1.finance.yahoo.com";
static const uint32_t MIN_BLOCK = 14000;       // measured: a handshake takes ~10 KB; below this the Wi-Fi driver starves and the chip resets (Exception 29)
static const uint32_t MIN_FREE_AFTER_TLS = 6000;
static const uint32_t CONNECT_TIMEOUT_MS = 8000;
static const uint32_t IDLE_TIMEOUT_MS = 10000;  // no byte for this long
static const uint32_t TOTAL_TIMEOUT_MS = 25000;
static const uint16_t SLICE = 384;              // max bytes consumed per poll()
static const uint8_t RECV_BUF_KB5 = 5;          // Yahoo's TLS Certificate record is 4610 B (measured): 4 KB buffers fail the handshake

enum Stage : uint8_t { S_NONE, S_HEADERS, S_BODY };

static BearSSL::WiFiClientSecure *s_cl = nullptr;
static YahooParser *s_parser = nullptr;
static Result s_res = {IDLE, 0, "", false, false};
static Stage s_stage = S_NONE;
static uint32_t s_t0 = 0, s_lastByte = 0;
static char s_line[64];
static uint8_t s_lineLen = 0;
static bool s_inHeaderEnd = false;
static uint32_t s_bodyBytes = 0;
static bool s_errBody = false;      // non-200: still parse the body, it carries Yahoo's error code

static void finishFail(const char *msg) {
  strlcpy(s_res.error, msg, sizeof s_res.error);
  s_res.phase = FAILED;
  if (s_cl) { s_cl->stop(); delete s_cl; s_cl = nullptr; }
  s_stage = S_NONE;
}

static void finishDone() {
  if (s_cl) { s_cl->stop(); delete s_cl; s_cl = nullptr; }
  s_stage = S_NONE;
  if (s_res.http == 200 || s_errBody) {
    if (s_parser->finish()) { s_res.phase = DONE; return; }
    s_res.notFound = strcmp(s_parser->error(), "Not Found") == 0;
    if (s_res.http != 200 && !s_res.notFound) { snprintf(s_res.error, sizeof s_res.error, "HTTP %d", s_res.http); }
    else strlcpy(s_res.error, s_res.notFound ? "unknown symbol" : s_parser->error(), sizeof s_res.error);
    s_res.phase = FAILED;
    return;
  }
  snprintf(s_res.error, sizeof s_res.error, "HTTP %d", s_res.http);
  s_res.phase = FAILED;
}

bool busy() { return s_res.phase == RUNNING; }
const Result &result() { return s_res; }

void abort() {
  if (s_cl) { s_cl->stop(); delete s_cl; s_cl = nullptr; }
  s_stage = S_NONE;
  if (s_res.phase == RUNNING) { strlcpy(s_res.error, "aborted", sizeof s_res.error); s_res.phase = FAILED; }
}

bool start(const char *path, YahooParser &parser) {
  if (s_res.phase == RUNNING) return false;
  memset(&s_res, 0, sizeof s_res);
  if (WiFi.status() != WL_CONNECTED) { strlcpy(s_res.error, "no Wi-Fi", sizeof s_res.error); s_res.phase = FAILED; return false; }
  if (ESP.getMaxFreeBlockSize() < MIN_BLOCK) {
    snprintf(s_res.error, sizeof s_res.error, "low memory (%u)", (unsigned)ESP.getMaxFreeBlockSize());
    s_res.phase = FAILED;
    return false;
  }
  s_cl = new (std::nothrow) BearSSL::WiFiClientSecure();
  if (!s_cl) { strlcpy(s_res.error, "out of memory", sizeof s_res.error); s_res.phase = FAILED; return false; }
  s_cl->setInsecure();
  s_cl->setBufferSizes(RECV_BUF_KB5 * 1024, 512);
  s_cl->setTimeout(CONNECT_TIMEOUT_MS);
  s_parser = &parser;
  s_t0 = millis();
  // Blocking part: DNS + TCP + TLS handshake.
  uint32_t freeBefore = ESP.getFreeHeap(), blockBefore = ESP.getMaxFreeBlockSize();
  if (!s_cl->connect(HOST, 443)) {
    char tls[40] = "";
    int e = s_cl->getLastSSLError(tls, sizeof tls);
    snprintf(s_res.error, sizeof s_res.error, e ? "TLS error %d" : "connect failed", e);
    Serial.printf("[yahoo] connect failed: ssl=%d heap %u/%u\n", e, (unsigned)freeBefore, (unsigned)blockBefore);
    finishFail(s_res.error);
    return false;
  }
  Serial.printf("[yahoo] TLS up in %lu ms, heap %u/%u -> %u/%u (free/block)\n", (unsigned long)(millis() - s_t0), (unsigned)freeBefore, (unsigned)blockBefore, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize());
  // The Wi-Fi driver allocates from the same heap while the response streams in. When too little is left it writes through
  // a NULL pointer and the chip resets (Exception 29), so give up cleanly instead.
  if (ESP.getFreeHeap() < MIN_FREE_AFTER_TLS) {
    snprintf(s_res.error, sizeof s_res.error, "low memory after TLS (%u)", (unsigned)ESP.getFreeHeap());
    finishFail(s_res.error);
    return false;
  }
  s_cl->print(F("GET "));
  s_cl->print(path);
  s_cl->print(F(" HTTP/1.0\r\nHost: "));
  s_cl->print(HOST);
  s_cl->print(F("\r\nUser-Agent: Mozilla/5.0\r\nAccept: application/json\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n"));
  s_stage = S_HEADERS;
  s_lineLen = 0; s_bodyBytes = 0; s_errBody = false; s_inHeaderEnd = false;
  s_lastByte = millis();
  s_res.phase = RUNNING;
  return true;
}

static void handleHeaderLine() {
  s_line[s_lineLen] = 0;
  if (s_lineLen == 0) { s_stage = S_BODY; return; }              // blank line: headers done
  if (s_res.http == 0 && strncmp(s_line, "HTTP/", 5) == 0) {
    const char *sp = strchr(s_line, ' ');
    s_res.http = sp ? atoi(sp + 1) : 0;
    s_res.rateLimited = s_res.http == 429 || s_res.http == 403;
    s_errBody = s_res.http != 200;
  }
}

void poll() {
  if (s_res.phase != RUNNING || !s_cl) return;
  uint32_t now = millis();
  if (now - s_t0 > TOTAL_TIMEOUT_MS) { finishFail("timeout"); return; }
  if (now - s_lastByte > IDLE_TIMEOUT_MS) { finishFail("timeout (no data)"); return; }

  uint16_t budget = SLICE;
  if (s_stage == S_HEADERS) {
    while (budget && s_cl->available() > 0 && s_stage == S_HEADERS) {
      int c = s_cl->read();
      if (c < 0) break;
      budget--; s_lastByte = now;
      if (c == '\r') continue;
      if (c == '\n') { handleHeaderLine(); s_lineLen = 0; continue; }
      if (s_lineLen < sizeof s_line - 1) s_line[s_lineLen++] = (char)c;      // long header lines are cut, only the status line matters
    }
  }
  if (s_stage == S_BODY) {
    uint8_t buf[256];
    while (budget) {
      int avail = s_cl->available();
      if (avail <= 0) break;
      int want = avail < (int)sizeof buf ? avail : (int)sizeof buf;
      if (want > budget) want = budget;
      int n = s_cl->read(buf, want);
      if (n <= 0) break;
      budget -= (uint16_t)n; s_bodyBytes += (uint32_t)n; s_lastByte = now;
      if (!s_parser->feed((const char *)buf, (size_t)n) && s_res.http == 200) { finishDone(); return; }   // syntax error: stop reading, finish() reports it
    }
  }
  // connection closed by the server and everything consumed -> the response is complete
  if (!s_cl->connected() && s_cl->available() <= 0) {
    if (s_stage != S_BODY && s_res.http == 0) { finishFail("connection closed"); return; }
    finishDone();
  }
}

static bool encodeSymbol(char *out, size_t n, const char *sym) {
  size_t o = 0;
  for (; *sym; sym++) {
    if (*sym == '^') { if (o + 3 >= n) return false; out[o++] = '%'; out[o++] = '5'; out[o++] = 'E'; }
    else { if (o + 1 >= n) return false; out[o++] = *sym; }
  }
  out[o] = 0;
  return true;
}

bool buildRangePath(char *out, size_t n, const char *symbol, const char *range, const char *interval) {
  char enc[40];
  if (!encodeSymbol(enc, sizeof enc, symbol)) return false;
  int k = snprintf(out, n, "/v8/finance/chart/%s?range=%s&interval=%s", enc, range, interval);
  return k > 0 && (size_t)k < n;
}

bool buildPeriodPath(char *out, size_t n, const char *symbol, uint32_t p1, uint32_t p2) {
  char enc[40];
  if (!encodeSymbol(enc, sizeof enc, symbol)) return false;
  int k = snprintf(out, n, "/v8/finance/chart/%s?period1=%lu&period2=%lu&interval=1d", enc, (unsigned long)p1, (unsigned long)p2);
  return k > 0 && (size_t)k < n;
}

}  // namespace yahoo
