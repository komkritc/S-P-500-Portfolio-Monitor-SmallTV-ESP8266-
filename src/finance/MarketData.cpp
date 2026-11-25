#include "MarketData.h"
#include <time.h>
#include <ESP8266WiFi.h>
#include "YahooFinance.h"
#include "ChartData.h"
#include "Portfolio.h"
#include "../core/MarketClock.h"
#include "../storage/StorageManager.h"

namespace market {

static const uint32_t MIN_GAP_MS = 1500;          // spacing between Yahoo requests
static const uint32_t FIRST_DELAY_MS = 3000;
static const uint32_t CACHE_SAVE_MS = 15UL * 60 * 1000;
static const uint16_t CLOSED_QUOTE_S = 900;
static const uint16_t CLOSED_CHART_S = 3600;

enum JobType : uint8_t { J_NONE, J_SP_QUOTE, J_SYM_QUOTE, J_CHART, J_BUY_PRICE, J_SALE_PRICE };

struct Due { uint32_t at; uint8_t fails; bool force; };   // at: millis() deadline

static Due s_spQ, s_chart[RANGE_COUNT], s_sym[MAX_SYMBOLS];
static YahooParser s_parser;
static uint32_t s_stageT[STAGE_CAP];
static float s_stageV[STAGE_CAP];
// Packed bar staging for the candle range only (opens, highs, lows - see AppTypes.h). Raw bars
// beyond OHLC_STAGE simply come back without a candle; the closes are unaffected.
static int16_t s_stageW[3 * OHLC_STAGE];
static JobType s_job = J_NONE;
static uint8_t s_jobArg = 0;
static uint32_t s_nextStart = 0;
static uint8_t s_viewRange = RANGE_1D;
static bool s_started = false, s_cacheDirty = false, s_cacheLoaded = false;
static uint32_t s_lastCacheSave = 0;
static AddJob s_add = {JOB_IDLE, "", "", "", false, false, NAN, NAN};
static bool s_addQueued = false, s_addPreview = false;
static char s_addSym[SYMBOL_LEN], s_addDate[11];
static double s_addThb = 0;
static SalePriceJob s_salePrice = {JOB_IDLE, "", "", "", NAN};
static bool s_saleQueued = false;
static char s_saleSym[SYMBOL_LEN], s_saleDate[11];

static bool wifiUp() { return WiFi.status() == WL_CONNECTED; }
static uint32_t nowEpoch() { time_t t = time(nullptr); return t > 1600000000L ? (uint32_t)t : 0; }
static bool due(const Due &d, uint32_t now) { return d.force || (int32_t)(now - d.at) >= 0; }

static bool onlySp500Positions() {
  for (uint8_t i = 0; i < g_app.posCount; i++) if (strcmp(g_app.positions[i].symbol, SP500_SYMBOL) != 0) return false;
  return true;
}

static void setError(const char *what, const char *why) {
  snprintf(g_app.lastError, sizeof g_app.lastError, "%s: %s", what, why);
  g_app.lastErrorEpoch = nowEpoch();
  Serial.printf("[yahoo] %s\n", g_app.lastError);
}

// Interval actually used, stretched while the market is closed and the cache already holds the close.
static uint16_t effectiveInterval(uint16_t base, uint16_t closedMin, uint32_t dataEpoch, bool fromCache, uint32_t *untilOpenMs) {
  uint32_t ep = nowEpoch();
  if (untilOpenMs) *untilOpenMs = 0xFFFFFFFFu;
  if (!ep || fromCache || !dataEpoch) return base;
  if (marketStatusAt(ep, 0) == MS_OPEN) return base;
  uint32_t lastClose = lastSessionCloseUtc(ep);
  if (!lastClose || dataEpoch < lastClose + 60) return base;           // final close not captured yet: keep polling at the base rate
  uint32_t open = nextSessionOpenUtc(ep);
  if (open > ep && untilOpenMs) *untilOpenMs = (open - ep + 10) * 1000UL;   // wake right after the next open
  return base > closedMin ? base : closedMin;
}

static void schedule(Due &d, uint32_t now, uint32_t intervalS, uint32_t cap = 0xFFFFFFFFu) {
  uint32_t ms = intervalS * 1000UL;
  if (ms > cap) ms = cap;
  d.at = now + ms; d.force = false; d.fails = 0;
}

static void backoff(Due &d, uint32_t now, bool rateLimited, uint32_t baseS) {
  if (d.fails < 250) d.fails++;
  uint32_t s = rateLimited ? 600 : (20u << (d.fails > 4 ? 4 : d.fails - 1));    // 20,40,80,160,320 s
  if (s > 600) s = 600;
  if (!rateLimited && s < baseS && baseS < 300) s = baseS;
  d.at = now + s * 1000UL; d.force = false;
}

static void markAll(bool failed) {
  for (uint8_t i = 0; i < g_app.book.n; i++) g_app.book.q[i].failed = failed && g_app.book.q[i].valid;
}

void begin() {
  uint32_t now = millis();
  s_started = false;
  memset(s_sym, 0, sizeof s_sym); memset(s_chart, 0, sizeof s_chart); memset(&s_spQ, 0, sizeof s_spQ);
  s_viewRange = g_app.settings.defaultRange;
  s_lastCacheSave = now;
  s_nextStart = now + FIRST_DELAY_MS;
  s_cacheLoaded = true;
}

static void dueAllNow(uint32_t now) {
  s_spQ.at = now; s_spQ.force = false; s_spQ.fails = 0;
  for (auto &d : s_sym) { d.at = now; d.force = false; d.fails = 0; }
  for (auto &d : s_chart) { d.at = now; d.force = false; d.fails = 0; }
}

void requestRefresh(Refresh what) {
  if (what == REF_ALL || what == REF_QUOTE) { s_spQ.force = true; }
  if (what == REF_ALL || what == REF_PORTFOLIO) for (auto &d : s_sym) d.force = true;
  if (what == REF_ALL || what == REF_CHARTS) for (auto &d : s_chart) d.force = true;
  s_nextStart = millis();
}

void positionsChanged() {
  bookSync(g_app.book, g_app.positions, g_app.posCount);
  for (auto &d : s_sym) { d.at = millis(); d.force = false; d.fails = 0; }
}

void setViewRange(uint8_t r) { if (r < RANGE_COUNT) s_viewRange = r; }

bool requestAddByThb(const char *symbol, const char *date, double thb, bool preview) {
  if (s_add.state == JOB_PENDING) return false;
  char sym[SYMBOL_LEN];
  memset(&s_add, 0, sizeof s_add);
  if (!normalizeSymbol(symbol, sym, sizeof sym)) { s_add.state = JOB_ERROR; strlcpy(s_add.error, addResultText(ADD_BAD_SYMBOL), sizeof s_add.error); return true; }
  int y, m, d;
  if (!parseIsoDate(date, y, m, d)) { s_add.state = JOB_ERROR; strlcpy(s_add.error, addResultText(ADD_BAD_DATE), sizeof s_add.error); return true; }
  if (!isNum(thb) || thb <= 0) { s_add.state = JOB_ERROR; strlcpy(s_add.error, "THB amount must be > 0", sizeof s_add.error); return true; }
  uint32_t ep = nowEpoch();
  if (ep && utcMidnight(y, m, d) > ep + 86400) { s_add.state = JOB_ERROR; strlcpy(s_add.error, addResultText(ADD_BAD_DATE), sizeof s_add.error); return true; }
  strcpy(s_addSym, sym); strcpy(s_addDate, date); s_addThb = thb; s_addPreview = preview;
  strcpy(s_add.symbol, sym);
  s_add.state = JOB_PENDING;
  s_addQueued = true;
  return true;
}

const AddJob &addJob() { return s_add; }
void consumeAddJob() { if (s_add.state == JOB_OK || s_add.state == JOB_ERROR) memset(&s_add, 0, sizeof s_add); }

bool requestSalePrice(const char *symbol, const char *date) {
  if (s_salePrice.state == JOB_PENDING) return false;
  char sym[SYMBOL_LEN];
  memset(&s_salePrice, 0, sizeof s_salePrice);
  s_salePrice.price = NAN;
  if (!normalizeSymbol(symbol, sym, sizeof sym)) { s_salePrice.state = JOB_ERROR; strlcpy(s_salePrice.error, "invalid symbol", sizeof s_salePrice.error); return true; }
  int y, m, d;
  if (!parseIsoDate(date, y, m, d)) { s_salePrice.state = JOB_ERROR; strlcpy(s_salePrice.error, "invalid date (YYYY-MM-DD)", sizeof s_salePrice.error); return true; }
  uint32_t ep = nowEpoch();
  if (ep && utcMidnight(y, m, d) > ep + 86400UL) { s_salePrice.state = JOB_ERROR; strlcpy(s_salePrice.error, "date is in the future", sizeof s_salePrice.error); return true; }
  strcpy(s_saleSym, sym);
  strcpy(s_saleDate, date);
  strlcpy(s_salePrice.symbol, sym, sizeof s_salePrice.symbol);
  strlcpy(s_salePrice.date, date, sizeof s_salePrice.date);
  s_salePrice.state = JOB_PENDING;
  s_saleQueued = true;
  return true;
}

const SalePriceJob &salePriceJob() { return s_salePrice; }
void consumeSalePriceJob() {
  if (s_salePrice.state == JOB_OK || s_salePrice.state == JOB_ERROR) {
    memset(&s_salePrice, 0, sizeof s_salePrice);
    s_salePrice.price = NAN;
  }
}

void clearCache() {
  memset(&g_app.sp, 0, sizeof g_app.sp);
  g_app.sp.last = g_app.sp.prevClose = g_app.sp.open = g_app.sp.high = g_app.sp.low = NAN;
  for (uint8_t i = 0; i < g_app.book.n; i++) { SymQuote &q = g_app.book.q[i]; char s[SYMBOL_LEN]; strcpy(s, q.symbol); memset(&q, 0, sizeof q); strcpy(q.symbol, s); q.price = NAN; }
  for (uint8_t r = 0; r < RANGE_COUNT; r++) { g_app.charts[r].valid = false; g_app.charts[r].n = 0; g_app.charts[r].failed = false; g_app.charts[r].fromCache = false; g_app.charts[r].base = NAN; }
  storage::eraseCache();
  s_cacheDirty = false;
  requestRefresh(REF_ALL);
}

void flushCache() {
  if (s_cacheDirty && storage::saveCache(g_app)) { s_cacheDirty = false; s_lastCacheSave = millis(); }
}

bool busy() { return yahoo::busy(); }
void abortRequests() { yahoo::abort(); s_job = J_NONE; }

// ---------------------------------------------------------------- job start
static bool startRequest(const char *path) {
  bool ok = yahoo::start(path, s_parser);
  return ok;
}

static void failJob(uint32_t now, const yahoo::Result &r) {
  bool rl = r.rateLimited;
  const char *what = s_job == J_SP_QUOTE ? "^GSPC quote" : s_job == J_SYM_QUOTE ? g_app.book.q[s_jobArg].symbol : s_job == J_CHART ? rangeLabel(s_jobArg) : s_job == J_SALE_PRICE ? "sale price" : "buy price";
  setError(what, r.error);
  switch (s_job) {
    case J_SP_QUOTE: g_app.sp.failed = true; markAll(false); { SymQuote *q = bookFind(g_app.book, SP500_SYMBOL); if (q && q->valid) q->failed = true; } backoff(s_spQ, now, rl, g_app.settings.quoteRefreshS); break;
    case J_SYM_QUOTE: g_app.book.q[s_jobArg].failed = g_app.book.q[s_jobArg].valid; backoff(s_sym[s_jobArg], now, rl, g_app.settings.portfolioRefreshS); break;
    case J_CHART: g_app.charts[s_jobArg].failed = g_app.charts[s_jobArg].valid; backoff(s_chart[s_jobArg], now, rl, g_app.settings.chartRefreshS); break;
    case J_BUY_PRICE:
      s_add.state = JOB_ERROR;
      strlcpy(s_add.error, r.notFound ? "unknown symbol (Yahoo has no data)" : r.error, sizeof s_add.error);
      break;
    case J_SALE_PRICE:
      s_salePrice.state = JOB_ERROR;
      strlcpy(s_salePrice.error, r.notFound ? "unknown symbol (Yahoo has no data)" : r.error, sizeof s_salePrice.error);
      break;
    default: break;
  }
}

static void logOk(const char *what, double a, double b) {
  Serial.printf("[yahoo] ok %s %.2f %.2f heap=%u block=%u\n", what, a, b, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize());
}

static void commitJob(uint32_t now) {
  uint32_t ep = nowEpoch();
  g_app.lastError[0] = 0;
  g_app.lastMarketOkEpoch = ep;
  switch (s_job) {
    case J_SP_QUOTE: {
      if (!isNum(s_parser.price) || s_parser.price <= 0) { yahoo::Result r = {yahoo::FAILED, 200, "no price", false, false}; failJob(now, r); return; }
      SP500Quote &q = g_app.sp;
      q.valid = true; q.failed = false; q.fromCache = false;
      q.last = s_parser.price;
      q.prevClose = s_parser.prevClose;
      q.open = s_parser.open; q.high = s_parser.dayHigh; q.low = s_parser.dayLow;
      q.marketTime = s_parser.marketTime;
      q.gmtOffset = s_parser.gmtOffset;
      strlcpy(q.tz, s_parser.tz, sizeof q.tz);
      q.okMs = now; q.okEpoch = ep;
      SymQuote *b = bookFind(g_app.book, SP500_SYMBOL);
      if (b) { b->valid = true; b->failed = false; b->fromCache = false; b->price = s_parser.price; b->marketTime = s_parser.marketTime; b->okMs = now; b->okEpoch = ep; }
      uint32_t untilOpen;
      uint16_t eff = onlySp500Positions() ? effectiveInterval(g_app.settings.quoteRefreshS, CLOSED_QUOTE_S, ep, false, &untilOpen) : g_app.settings.quoteRefreshS;
      if (!onlySp500Positions()) untilOpen = 0xFFFFFFFFu;
      g_app.effQuoteS = eff;
      g_app.effPortfolioS = onlySp500Positions() ? (eff > g_app.settings.portfolioRefreshS ? eff : g_app.settings.portfolioRefreshS) : g_app.settings.portfolioRefreshS;
      schedule(s_spQ, now, eff, untilOpen);
      s_cacheDirty = true;
      logOk("quote ^GSPC last/prev", q.last, q.prevClose);
      break;
    }
    case J_SYM_QUOTE: {
      SymQuote &q = g_app.book.q[s_jobArg];
      if (!isNum(s_parser.price) || s_parser.price <= 0) { yahoo::Result r = {yahoo::FAILED, 200, "no price", false, false}; failJob(now, r); return; }
      q.valid = true; q.failed = false; q.fromCache = false;
      q.price = s_parser.price; q.marketTime = s_parser.marketTime; q.okMs = now; q.okEpoch = ep;
      schedule(s_sym[s_jobArg], now, g_app.settings.portfolioRefreshS);
      s_cacheDirty = true;
      logOk(q.symbol, q.price, 0);
      break;
    }
    case J_CHART: {
      Chart &c = g_app.charts[s_jobArg];
      if (s_parser.count < 2) { yahoo::Result r = {yahoo::FAILED, 200, "no chart points", false, false}; failJob(now, r); return; }
      float base = NAN;
      if (s_jobArg == RANGE_1D && isNum(s_parser.prevClose) && s_parser.prevClose > 0) base = (float)s_parser.prevClose;
      chartStore(c, s_stageT, s_stageV, s_parser.count, base, now, ep, c.w ? s_stageW : nullptr, c.w ? OHLC_STAGE : 0);
      uint32_t untilOpen;
      uint16_t eff = effectiveInterval(g_app.settings.chartRefreshS, CLOSED_CHART_S, ep, false, &untilOpen);
      g_app.effChartS = eff;
      schedule(s_chart[s_jobArg], now, eff, untilOpen);
      s_cacheDirty = true;
      logOk(rangeLabel(s_jobArg), c.n, s_parser.count);
      break;
    }
    case J_BUY_PRICE: {
      int y, m, d;
      double buy = NAN;
      bool exact = false;
      uint32_t barTs = 0;
      parseIsoDate(s_addDate, y, m, d);
      int32_t off = (s_parser.tzName[0] && strcmp(s_parser.tzName, "America/New_York") != 0) ? s_parser.gmtOffset : EXCHANGE_NY;
      if (!pickCloseOnDate(s_stageT, s_stageV, s_parser.count, y, m, d, buy, &exact, off, &barTs)) {
        s_add.state = JOB_ERROR; strlcpy(s_add.error, "no close price on or before that date", sizeof s_add.error); return;
      }
      // The session the price really came from: a weekend or holiday purchase date silently borrows the
      // close of the previous trading day, so the UI has to say so and ask before writing the row.
      LocalTime bl = toLocal(barTs, off == EXCHANGE_NY ? nyOffsetSec(barTs) : off);
      snprintf(s_add.priceDate, sizeof s_add.priceDate, "%04d-%02d-%02d", bl.y, bl.m, bl.d);
      s_add.exact = exact; s_add.price = buy;
      double qty;
      if (!qtyFromThb(s_addThb, buy, qty)) { s_add.state = JOB_ERROR; strlcpy(s_add.error, "invalid amount / price", sizeof s_add.error); return; }
      s_add.qty = qty;
      if (s_addPreview) {
        s_add.state = JOB_OK;
        Serial.printf("[add] preview %s %s thb=%.2f buy=%.6f qty=%.6f exact=%d from=%s\n", s_addSym, s_addDate, s_addThb, buy, qty, (int)exact, s_add.priceDate);
        break;
      }
      AddResult r = portfolioAdd(g_app.positions, g_app.posCount, g_app.pmeta, s_addSym, s_addDate, qty, buy, ep);
      if (r != ADD_OK && r != ADD_UPDATED) { s_add.state = JOB_ERROR; strlcpy(s_add.error, addResultText(r), sizeof s_add.error); return; }
      bool saved = storage::savePortfolio(g_app);
      positionsChanged();
      s_add.state = JOB_OK; s_add.updated = r == ADD_UPDATED;
      Serial.printf("[add] %s %s thb=%.2f buy=%.2f qty=%.6f exact=%d\n", s_addSym, s_addDate, s_addThb, buy, qty, (int)exact);
      if (!saved) strlcpy(s_add.error, "added, but saving to flash failed", sizeof s_add.error);
      break;
    }
    case J_SALE_PRICE: {
      int y, m, d;
      double px = NAN;
      bool exact = false;
      parseIsoDate(s_saleDate, y, m, d);
      int32_t off = (s_parser.tzName[0] && strcmp(s_parser.tzName, "America/New_York") != 0) ? s_parser.gmtOffset : EXCHANGE_NY;
      if (!pickCloseOnDate(s_stageT, s_stageV, s_parser.count, y, m, d, px, &exact, off)) {
        s_salePrice.state = JOB_ERROR;
        strlcpy(s_salePrice.error, "no close price on or before that date", sizeof s_salePrice.error);
        return;
      }
      s_salePrice.price = px;
      s_salePrice.state = JOB_OK;
      Serial.printf("[sale] %s %s close=%.6f exact=%d\n", s_saleSym, s_saleDate, px, (int)exact);
      break;
    }
    default: break;
  }
}

static bool pickAndStart(uint32_t now) {
  char path[112];
  if (s_addQueued) {
    s_addQueued = false;
    uint32_t ep = nowEpoch();
    int y, m, d; parseIsoDate(s_addDate, y, m, d);
    uint32_t mid = utcMidnight(y, m, d);
    // window: 9 days before .. 2 days after the buy date, daily bars (long enough to span holiday weekends)
    if (!yahoo::buildPeriodPath(path, sizeof path, s_addSym, mid - 9 * 86400, mid + 2 * 86400)) { s_add.state = JOB_ERROR; strlcpy(s_add.error, "bad symbol", sizeof s_add.error); return false; }
    (void)ep;
    s_parser.begin(s_stageT, s_stageV, STAGE_CAP);
    s_job = J_BUY_PRICE;
    if (!startRequest(path)) { const yahoo::Result &r = yahoo::result(); s_add.state = JOB_ERROR; strlcpy(s_add.error, r.error, sizeof s_add.error); s_job = J_NONE; return true; }
    return true;
  }
  if (s_saleQueued) {
    s_saleQueued = false;
    int y, m, d;
    parseIsoDate(s_saleDate, y, m, d);
    uint32_t mid = utcMidnight(y, m, d);
    // same window as the buy-price job: 9 days before .. 2 days after, daily bars
    if (!yahoo::buildPeriodPath(path, sizeof path, s_saleSym, mid - 9 * 86400, mid + 2 * 86400)) {
      s_salePrice.state = JOB_ERROR;
      strlcpy(s_salePrice.error, "bad symbol", sizeof s_salePrice.error);
      return false;
    }
    s_parser.begin(s_stageT, s_stageV, STAGE_CAP);
    s_job = J_SALE_PRICE;
    if (!startRequest(path)) {
      const yahoo::Result &r = yahoo::result();
      s_salePrice.state = JOB_ERROR;
      strlcpy(s_salePrice.error, r.error, sizeof s_salePrice.error);
      s_job = J_NONE;
      return true;
    }
    return true;
  }
  // ^GSPC quote
  if (due(s_spQ, now)) {
    if (!yahoo::buildRangePath(path, sizeof path, SP500_SYMBOL, "1d", "1d")) return false;
    s_parser.begin(nullptr, nullptr, 0);
    s_job = J_SP_QUOTE;
    if (!startRequest(path)) { failJob(now, yahoo::result()); s_job = J_NONE; }
    return true;
  }
  // other portfolio symbols
  for (uint8_t i = 0; i < g_app.book.n; i++) {
    if (!strcmp(g_app.book.q[i].symbol, SP500_SYMBOL) || !due(s_sym[i], now)) continue;
    if (!yahoo::buildRangePath(path, sizeof path, g_app.book.q[i].symbol, "1d", "1d")) continue;
    s_parser.begin(nullptr, nullptr, 0);
    s_job = J_SYM_QUOTE; s_jobArg = i;
    if (!startRequest(path)) { failJob(now, yahoo::result()); s_job = J_NONE; }
    return true;
  }
  // charts: the one on screen first
  for (uint8_t k = 0; k < RANGE_COUNT; k++) {
    uint8_t r = (s_viewRange + k) % RANGE_COUNT;
    if (!due(s_chart[r], now)) continue;
    if (!yahoo::buildRangePath(path, sizeof path, SP500_SYMBOL, rangeName(r), rangeInterval(r))) continue;
    // Only this range asks for candles (charts[r].w is null for 1D/5D), so the other two fetches
    // parse exactly as before.
    s_parser.begin(s_stageT, s_stageV, STAGE_CAP, g_app.charts[r].w ? s_stageW : nullptr, OHLC_STAGE);
    s_job = J_CHART; s_jobArg = r;
    if (!startRequest(path)) { failJob(now, yahoo::result()); s_job = J_NONE; }
    return true;
  }
  return false;
}

void loop() {
  uint32_t now = millis();
  if (s_cacheDirty && (now - s_lastCacheSave) >= CACHE_SAVE_MS && !yahoo::busy()) flushCache();

  if (s_job != J_NONE) {
    yahoo::poll();
    const yahoo::Result &r = yahoo::result();
    if (r.phase == yahoo::DONE) { commitJob(now); s_job = J_NONE; s_nextStart = now + MIN_GAP_MS; }
    else if (r.phase == yahoo::FAILED) { failJob(now, r); s_job = J_NONE; s_nextStart = now + MIN_GAP_MS; }
    return;
  }
  if (!wifiUp() ) return;
  if (!s_started) { s_started = true; dueAllNow(now); }
  if ((int32_t)(now - s_nextStart) < 0) return;
  if (pickAndStart(now)) {
    // a start that failed synchronously (low memory...) must not retry in a tight loop
    if (s_job == J_NONE) s_nextStart = now + 10000;
  }
}

}  // namespace market
