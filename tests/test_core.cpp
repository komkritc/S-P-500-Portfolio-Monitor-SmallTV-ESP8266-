// Host unit tests for the pure core: clock, format, calculator, JSON scanner, Yahoo parser, charts.
// Build/run: see tests/run_tests.py
#include "../src/core/MarketClock.h"
#include "../src/core/Format.h"
#include "../src/finance/FinancialCalculator.h"
#include "../src/finance/YahooParser.h"
#include "../src/finance/ChartData.h"
#include "../src/core/JsonWriter.h"
#include "../src/core/FlatJson.h"
#include "../src/config/AppConfig.h"
#include "../src/finance/Portfolio.h"
#include "../src/core/AppState.h"
#include "../src/display/ViewSchedule.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <algorithm>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_STR(a, b) do { std::string _a = (a), _b = (b); if (_a == _b) g_pass++; else { g_fail++; printf("FAIL %s:%d  got \"%s\" want \"%s\"\n", __FILE__, __LINE__, _a.c_str(), _b.c_str()); } } while (0)
#define CHECK_NEAR(a, b, e) do { double _a = (a), _b = (b); if (fabs(_a - _b) <= (e)) g_pass++; else { g_fail++; printf("FAIL %s:%d  got %.6f want %.6f\n", __FILE__, __LINE__, _a, _b); } } while (0)

static uint32_t U(int y, int m, int d, int hh = 0, int mm = 0) { return utcMidnight(y, m, d) + hh * 3600 + mm * 60; }
static std::string F(double v, int dec, SignMode s) { char b[40]; fmtNum(b, sizeof b, v, dec, s); return b; }
static std::string fn(bool (*f)(char *, size_t, double), double v) { char b[48]; f(b, sizeof b, v); return b; }

static std::string readFile(const char *p) {
  FILE *f = fopen(p, "rb"); if (!f) { printf("cannot open %s\n", p); exit(2); }
  std::string s; char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof buf, f))) s.append(buf, n); fclose(f); return s;
}

// ---------------------------------------------------------------- clock
static void testClock() {
  // DST boundaries 2026: starts Sun Mar 8 07:00 UTC, ends Sun Nov 1 06:00 UTC
  bool d;
  CHECK(nyOffsetSec(U(2026, 3, 8, 6, 59), &d) == -5 * 3600 && !d);
  CHECK(nyOffsetSec(U(2026, 3, 8, 7, 0), &d) == -4 * 3600 && d);
  CHECK(nyOffsetSec(U(2026, 11, 1, 5, 59), &d) == -4 * 3600 && d);
  CHECK(nyOffsetSec(U(2026, 11, 1, 6, 0), &d) == -5 * 3600 && !d);
  CHECK(nyOffsetSec(U(2025, 3, 9, 7, 0), &d) == -4 * 3600 && d);      // 2025: 2nd Sun = Mar 9
  CHECK(nyOffsetSec(U(2025, 11, 2, 6, 0), &d) == -5 * 3600 && !d);    // 2025: 1st Sun = Nov 2
  CHECK(nyOffsetSec(U(2026, 7, 1, 12), &d) == -4 * 3600 && d);
  CHECK(nyOffsetSec(U(2026, 1, 15, 12), &d) == -5 * 3600 && !d);

  // fixture: 1795210554 = 2026-11-20 21:35:54 UTC = 16:35 EST
  char b[40];
  formatQuoteTime(b, sizeof b, 1795210554, 0, 0); CHECK_STR(b, "Nov 20, 16:35 EST");
  formatQuoteTime(b, sizeof b, 1795210554, 1, 420); CHECK_STR(b, "Nov 21, 04:35 UTC+07:00");
  formatQuoteTime(b, sizeof b, U(2026, 7, 10, 20, 0), 0, 0); CHECK_STR(b, "Jul 10, 16:00 EDT");
  formatQuoteTime(b, sizeof b, 0, 0, 0); CHECK_STR(b, "N/A");
  formatSessionDate(b, sizeof b, 1795210554); CHECK_STR(b, "2026-11-20");
  formatSessionDate(b, sizeof b, U(2026, 11, 21, 2, 0)); CHECK_STR(b, "2026-11-20");   // 21:00 EST on the 20th
  formatClock(b, sizeof b, 1795185000, 0, 0); CHECK_STR(b, "09:30");
  formatMonthDay(b, sizeof b, 1795185000, 0, 0); CHECK_STR(b, "Nov 20");

  // calendar
  int y, m, dd;
  CHECK(parseIsoDate("2026-02-28", y, m, dd) && y == 2026 && m == 2 && dd == 28);
  CHECK(!parseIsoDate("2026-02-29", y, m, dd));
  CHECK(parseIsoDate("2028-02-29", y, m, dd));
  CHECK(!parseIsoDate("2026-13-01", y, m, dd));
  CHECK(!parseIsoDate("2026-1-01", y, m, dd));
  CHECK(!parseIsoDate("20260101", y, m, dd));
  CHECK(!parseIsoDate("2026-04-0a", y, m, dd));
  CHECK(!parseIsoDate(nullptr, y, m, dd));

  // holidays 2026: Jan1, MLK Jan19, Pres Feb16, Good Fri Apr3, Memorial May25, Juneteenth Jun19,
  // Jul 4 is Saturday -> observed Fri Jul 3, Labor Sep7, Thanksgiving Nov26, Christmas Fri Dec25
  CHECK(isNyseHoliday(2026, 1, 1));  CHECK(isNyseHoliday(2026, 1, 19)); CHECK(isNyseHoliday(2026, 2, 16));
  CHECK(isNyseHoliday(2026, 4, 3));  CHECK(isNyseHoliday(2026, 5, 25)); CHECK(isNyseHoliday(2026, 6, 19));
  CHECK(isNyseHoliday(2026, 7, 3));  CHECK(isNyseHoliday(2026, 9, 7));  CHECK(isNyseHoliday(2026, 11, 26));
  CHECK(isNyseHoliday(2026, 12, 25));
  CHECK(!isNyseHoliday(2026, 11, 20)); CHECK(!isNyseHoliday(2026, 11, 27)); CHECK(!isNyseHoliday(2026, 7, 2));
  CHECK(isNyseHoliday(2026, 11, 21));   // Saturday
  CHECK(isNyseHoliday(2027, 12, 24));   // Christmas Sat Dec 25 2027 -> observed Fri Dec 24
  CHECK(!isNyseHoliday(2027, 12, 31));  // Jan 1 2028 is Saturday: not observed on Dec 31
  CHECK(isNyseHoliday(2027, 7, 5));     // Jul 4 2027 is Sunday -> Mon Jul 5
  CHECK(nyseCloseMinute(2026, 11, 27) == 780);  // day after Thanksgiving
  CHECK(nyseCloseMinute(2026, 12, 24) == 780);  // Thu Dec 24
  CHECK(nyseCloseMinute(2026, 11, 20) == 960);

  // status. Fri 2026-11-20 EST: open 14:30 UTC, close 21:00 UTC, pre 09:00 UTC (04:00 ET)
  uint32_t open = U(2026, 11, 20, 14, 30), close = U(2026, 11, 20, 21, 0);
  CHECK(marketStatusAt(U(2026, 11, 20, 8, 59), 0) == MS_CLOSED);                  // 03:59 ET
  CHECK(marketStatusAt(U(2026, 11, 20, 9, 0), 0) == MS_PRE);                      // 04:00 ET
  CHECK(marketStatusAt(U(2026, 11, 20, 14, 29), 0) == MS_PRE);
  CHECK(marketStatusAt(U(2026, 11, 20, 14, 31), open + 60) == MS_OPEN);
  CHECK(marketStatusAt(U(2026, 11, 20, 14, 31), U(2026, 11, 19, 21, 0)) == MS_CLOSED);   // stale quote is never "live"
  CHECK(marketStatusAt(U(2026, 11, 20, 20, 59), close - 120) == MS_OPEN);
  CHECK(marketStatusAt(close, close) == MS_CLOSED);
  CHECK(marketStatusAt(U(2026, 11, 21, 15, 0), close) == MS_CLOSED);              // Saturday
  CHECK(marketStatusAt(U(2026, 11, 26, 15, 0), close) == MS_CLOSED);              // Thanksgiving
  CHECK(marketStatusAt(U(2026, 11, 27, 17, 30), U(2026, 11, 27, 17, 0)) == MS_OPEN);     // 12:30 ET on the early-close day: still open
  CHECK(marketStatusAt(U(2026, 11, 27, 18, 30), U(2026, 11, 27, 18, 0)) == MS_CLOSED);   // 13:30 ET: after the 13:00 early close
  CHECK(marketStatusAt(100, 0) == MS_UNKNOWN);                                    // clock not synced
  // summer: Fri 2026-07-10 open 13:30 UTC
  CHECK(marketStatusAt(U(2026, 7, 10, 13, 29), 0) == MS_PRE);
  CHECK(marketStatusAt(U(2026, 7, 10, 13, 45), U(2026, 7, 10, 13, 30)) == MS_OPEN);
}

// ---------------------------------------------------------------- format
static void testFormat() {
  CHECK_STR(F(268500, 2, SIGN_NONE), "268,500.00");
  CHECK_STR(F(1234567.891, 2, SIGN_NONE), "1,234,567.89");
  CHECK_STR(F(999.995, 2, SIGN_NONE), "1,000.00");        // rounding carries into the grouping
  CHECK_STR(F(0.004, 2, SIGN_ALWAYS), "0.00");            // no sign on a value that displays as zero
  CHECK_STR(F(-0.004, 2, SIGN_ALWAYS), "0.00");
  CHECK_STR(F(-265.44, 2, SIGN_ALWAYS), "-265.44");
  CHECK_STR(F(18500, 2, SIGN_ALWAYS), "+18,500.00");
  CHECK_STR(F(NAN, 2, SIGN_NONE), "N/A");
  CHECK_STR(F(INFINITY, 2, SIGN_NONE), "N/A");
  CHECK_STR(F(5e12, 2, SIGN_NONE), "N/A");
  CHECK_STR(F(12, 0, SIGN_NONE), "12");
  CHECK_STR(F(0.12345, 4, SIGN_NONE), "0.1235");
  CHECK_STR(fn(fmtThb, 268500), "THB 268,500.00");
  CHECK_STR(fn(fmtThb, -5200), "-THB 5,200.00");
  CHECK_STR(fn(fmtThbSigned, 18500), "+THB 18,500.00");
  CHECK_STR(fn(fmtThbSigned, -5200), "-THB 5,200.00");
  CHECK_STR(fn(fmtThbSigned, 0), "THB 0.00");
  CHECK_STR(fn(fmtThb, NAN), "N/A");
  CHECK_STR(fn(fmtPct, 7.4), "+7.40%");
  CHECK_STR(fn(fmtPct, -2.1), "-2.10%");
  CHECK_STR(fn(fmtPct, NAN), "N/A");
  CHECK_STR(fn(fmtIndex, 6910.39), "6,910.39");
  CHECK_STR(fn(fmtIndexSigned, -265.44), "-265.44");
  CHECK(signOf2dp(0.004) == 0 && signOf2dp(-0.004) == 0 && signOf2dp(0.005) == 1 && signOf2dp(-1) == -1 && signOf2dp(NAN) == 0);
  char tiny[5]; CHECK(!fmtNum(tiny, sizeof tiny, 123456.0, 2, SIGN_NONE)); CHECK_STR(tiny, "N/A");   // buffer too small
}

// ---------------------------------------------------------------- calculator
static Position P(const char *sym, double q, double b, const char *dt = "2026-01-02") {
  Position p; memset(&p, 0, sizeof p); strcpy(p.symbol, sym); strcpy(p.date, dt); p.qty = q; p.buy = b; return p;
}
static void addQ(QuoteBook &bk, const char *sym, double px, bool failed = false, bool cache = false, uint32_t okMs = 1000) {
  SymQuote &q = bk.q[bk.n++]; memset(&q, 0, sizeof q); strcpy(q.symbol, sym); q.price = px; q.valid = true; q.failed = failed; q.fromCache = cache; q.okMs = okMs; q.marketTime = 1795210554;
}

static void testCalculator() {
  QuoteBook bk; memset(&bk, 0, sizeof bk);
  addQ(bk, "^GSPC", 6910.39);
  Position pos[3] = { P("^GSPC", 10, 6000), P("^GSPC", 5, 7500, "2026-02-03"), P("^GSPC", 2.5, 6910.39, "2026-03-04") };
  PositionResult r[8]; PortfolioSummary s;
  computePortfolio(pos, 3, bk, 1000, 60000, r, s);
  CHECK_NEAR(r[0].cost, 60000, 1e-9);        CHECK_NEAR(r[0].nav, 69103.9, 1e-6);
  CHECK_NEAR(r[0].pl, 9103.9, 1e-6);         CHECK_NEAR(r[0].retPct, 15.17316667, 1e-6);
  CHECK_NEAR(r[1].cost, 37500, 1e-9);        CHECK_NEAR(r[1].nav, 34551.95, 1e-6);
  CHECK_NEAR(r[1].pl, -2948.05, 1e-6);       CHECK_NEAR(r[1].retPct, -7.86146667, 1e-6);   // negative P/L
  CHECK_NEAR(r[2].pl, 0, 1e-6);              CHECK_NEAR(r[2].retPct, 0, 1e-9);
  CHECK(r[0].state == Q_FRESH);
  CHECK_NEAR(s.invested, 60000 + 37500 + 17275.975, 1e-6);
  CHECK_NEAR(s.nav, 69103.9 + 34551.95 + 17275.975, 1e-6);
  CHECK_NEAR(s.pl, s.nav - s.invested, 1e-9);
  CHECK_NEAR(s.retPct, s.pl / s.invested * 100.0, 1e-9);
  CHECK(s.complete && !s.stale && s.count == 3 && s.priced == 3 && s.invalid == 0);

  // total == sum of positions (single engine invariant)
  double sn = 0, sc = 0; for (int i = 0; i < 3; i++) { sn += r[i].nav; sc += r[i].cost; }
  CHECK_NEAR(s.nav, sn, 1e-9); CHECK_NEAR(s.invested, sc, 1e-9);

  // missing quote: NaN, never 0, never "cost"
  QuoteBook empty; memset(&empty, 0, sizeof empty);
  computePortfolio(pos, 3, empty, 1000, 60000, r, s);
  CHECK(r[0].state == Q_MISSING && isnan(r[0].nav) && isnan(r[0].pl) && isnan(r[0].price));
  CHECK_NEAR(r[0].cost, 60000, 1e-9);
  CHECK(!s.complete && isnan(s.nav) && isnan(s.pl) && isnan(s.retPct) && s.priced == 0);
  CHECK_NEAR(s.invested, 114775.975, 1e-6);

  // one symbol quoted, one not
  QuoteBook half; memset(&half, 0, sizeof half); addQ(half, "AAPL", 200);
  Position mix[2] = { P("AAPL", 1, 100), P("MSFT", 1, 100) };
  computePortfolio(mix, 2, half, 1000, 60000, r, s);
  CHECK(!s.complete && s.priced == 1 && isnan(s.nav) && r[0].state == Q_FRESH && r[1].state == Q_MISSING);

  // invalid positions: excluded, counted, do not poison totals
  Position bad[5] = { P("^GSPC", 0, 100), P("^GSPC", -1, 100), P("^GSPC", 1, 0), P("^GSPC", NAN, 5), P("^GSPC", 2, 100) };
  computePortfolio(bad, 5, bk, 1000, 60000, r, s);
  for (int i = 0; i < 4; i++) CHECK(r[i].state == Q_INVALID && isnan(r[i].nav) && isnan(r[i].cost));
  CHECK(s.invalid == 4 && s.priced == 1 && s.complete && s.count == 5);
  CHECK_NEAR(s.invested, 200, 1e-9); CHECK_NEAR(s.nav, 13820.78, 1e-6);
  Position onlyBad[1] = { P("^GSPC", 0, 100) };
  computePortfolio(onlyBad, 1, bk, 1000, 60000, r, s);
  CHECK(!s.complete && isnan(s.nav) && isnan(s.retPct) && s.invested == 0);
  computePortfolio(pos, 0, bk, 1000, 60000, nullptr, s);   // empty portfolio
  CHECK(!s.complete && s.count == 0 && s.invested == 0 && isnan(s.nav));

  // stale handling
  QuoteBook st; memset(&st, 0, sizeof st); addQ(st, "^GSPC", 7000, true);          // refresh failed -> stale, still valued
  computePortfolio(pos, 1, st, 2000, 60000, r, s);
  CHECK(r[0].state == Q_STALE && isnan(r[0].nav) == false && s.complete && s.stale);
  QuoteBook ca; memset(&ca, 0, sizeof ca); addQ(ca, "^GSPC", 7000, false, true);   // loaded from flash
  computePortfolio(pos, 1, ca, 2000, 60000, r, s); CHECK(r[0].state == Q_STALE && s.stale);
  QuoteBook old; memset(&old, 0, sizeof old); addQ(old, "^GSPC", 7000, false, false, 1000);
  computePortfolio(pos, 1, old, 1000 + 60001, 60000, r, s); CHECK(r[0].state == Q_STALE);
  computePortfolio(pos, 1, old, 1000 + 60000, 60000, r, s); CHECK(r[0].state == Q_FRESH);
  computePortfolio(pos, 1, old, 1000 - 5, 60000, r, s);     CHECK(r[0].state == Q_STALE || r[0].state == Q_FRESH);   // millis wrap safe: no crash
  uint32_t wrapNow = 500, wrapOk = 0xFFFFFF00u; (void)wrapNow; (void)wrapOk;
  QuoteBook wr; memset(&wr, 0, sizeof wr); addQ(wr, "^GSPC", 7000, false, false, 0xFFFFFF00u);
  computePortfolio(pos, 1, wr, 600, 60000, r, s); CHECK(r[0].state == Q_FRESH);    // 0x100+600 ms elapsed across the wrap

  // quote with invalid price
  QuoteBook zq; memset(&zq, 0, sizeof zq); addQ(zq, "^GSPC", 0); computePortfolio(pos, 1, zq, 1000, 60000, r, s); CHECK(r[0].state == Q_MISSING);
  QuoteBook nq; memset(&nq, 0, sizeof nq); addQ(nq, "^GSPC", NAN); computePortfolio(pos, 1, nq, 1000, 60000, r, s); CHECK(r[0].state == Q_MISSING);

  CHECK_NEAR(safeDiv(1, 4), 0.25, 1e-12); CHECK(isnan(safeDiv(1, 0))); CHECK(isnan(safeDiv(NAN, 1)));
}

static void testQuoteView() {
  SP500Quote q; memset(&q, 0, sizeof q);
  q.valid = true; q.last = 6910.39; q.prevClose = 7175.83; q.open = 7150.39; q.high = 7198.67; q.low = 6882.66;
  q.marketTime = 1795210554; strcpy(q.tz, "EST"); q.okMs = 1000; q.okEpoch = 1795210560;
  QuoteView v;
  computeQuoteView(q, 1795210600, 1500, 120000, v);
  CHECK(v.valid && !v.stale);
  CHECK_NEAR(v.change, -265.44, 1e-9);
  CHECK_NEAR(v.changePct, -265.44 / 7175.83 * 100.0, 1e-9); CHECK_NEAR(v.changePct, -3.6991, 1e-4);
  CHECK_STR(v.sessionDate, "2026-11-20"); CHECK_STR(v.tz, "EST");
  CHECK(v.status == MS_CLOSED);                                  // Friday 16:36 ET: closed, quote is the 16:00 close
  computeQuoteView(q, 1795210600, 1000 + 120001, 120000, v); CHECK(v.stale);
  q.failed = true; computeQuoteView(q, 1795210600, 1500, 120000, v); CHECK(v.stale && v.valid); q.failed = false;
  // old closing quote is not live while the market is open on a later day
  computeQuoteView(q, U(2026, 11, 23, 15, 0), 1500, 120000, v); CHECK(v.status == MS_CLOSED);
  computeQuoteView(q, U(2026, 11, 23, 12, 0), 1500, 120000, v); CHECK(v.status == MS_PRE);   // Monday 07:00 ET
  q.prevClose = NAN; computeQuoteView(q, 1795210600, 1500, 120000, v);
  CHECK(v.valid && isnan(v.change) && isnan(v.changePct));       // no previous close -> N/A, not 0
  q.prevClose = 0; computeQuoteView(q, 1795210600, 1500, 120000, v); CHECK(isnan(v.change));
  q.valid = false; computeQuoteView(q, 1795210600, 1500, 120000, v); CHECK(!v.valid && isnan(v.last));
}

static void testChartStats() {
  uint32_t t[4] = {1, 2, 3, 4}; float vv[4] = {100, 110, 90, 105};
  Chart c; memset(&c, 0, sizeof c); c.valid = true; c.n = 4; c.cap = 4; c.t = t; c.v = vv; c.base = NAN;
  ChartStats s; computeChartStats(c, s);
  CHECK(s.valid); CHECK_NEAR(s.change, 5, 1e-6); CHECK_NEAR(s.changePct, 5, 1e-6); CHECK_NEAR(s.minV, 90, 0); CHECK_NEAR(s.maxV, 110, 0);
  c.base = 120; computeChartStats(c, s); CHECK_NEAR(s.change, -15, 1e-6); CHECK_NEAR(s.changePct, -12.5, 1e-6);   // 1D vs previous close
  c.n = 1; computeChartStats(c, s); CHECK(!s.valid && isnan(s.change));
  c.n = 4; c.valid = false; computeChartStats(c, s); CHECK(!s.valid);
}

// ---------------------------------------------------------------- json + yahoo
struct Ev { std::string path, text; int idx; JsonKind k; };
static std::vector<Ev> *g_ev;
static void collect(void *, const char *path, int idx, JsonKind k, const char *text) { g_ev->push_back({path, text, idx, k}); }

static void testJson() {
  std::vector<Ev> ev; g_ev = &ev;
  JsonStream j; j.begin(collect, nullptr);
  const char *doc = "{\"a\":{\"b\":[1,2.5,-3e2,null,true,\"x\\\"y\"],\"c\":\"\\u00e9z\"},\"d\":[[1,2],[3]],\"e\":[{\"f\":1},{\"f\":2}],\"g\":\"\"}";
  // feed in 1-byte chunks to test resumability
  for (const char *p = doc; *p; p++) CHECK(j.feed(p, 1));
  CHECK(j.complete() && !j.failed());
  auto find = [&](const char *path, int idx) -> const Ev * { for (auto &e : ev) if (e.path == path && e.idx == idx) return &e; return nullptr; };
  CHECK(find("a.b[]", 0) && find("a.b[]", 0)->text == "1" && find("a.b[]", 0)->k == JK_NUM);
  CHECK(find("a.b[]", 1) && find("a.b[]", 1)->text == "2.5");
  CHECK(find("a.b[]", 2) && find("a.b[]", 2)->text == "-3e2");
  CHECK(find("a.b[]", 3) && find("a.b[]", 3)->k == JK_NULL);
  CHECK(find("a.b[]", 4) && find("a.b[]", 4)->k == JK_BOOL);
  CHECK(find("a.b[]", 5) && find("a.b[]", 5)->text == "x\"y");
  CHECK(find("a.c", -1) && find("a.c", -1)->text == "?z");
  { std::string seq; for (auto &e : ev) if (e.path == "d[][]") seq += e.text + "@" + std::to_string(e.idx) + " "; CHECK_STR(seq, "1@0 2@1 3@0 "); }   // idx = index in the innermost array
  { std::string seq; for (auto &e : ev) if (e.path == "e[].f") seq += e.text + "@" + std::to_string(e.idx) + " "; CHECK_STR(seq, "1@-1 2@-1 "); }   // innermost container is an object
  CHECK(find("g", -1) && find("g", -1)->text == "" && find("g", -1)->k == JK_STR);

  // errors
  const char *bad[] = {"<html>", "{\"a\":}", "{\"a\":1,}x", "[1,2}", "{\"a\" 1}", "{\"a\":[1,2]", "{\"a\":1} junk", "{1:2}", "{\"a\":tru3x"};
  for (const char *b : bad) { ev.clear(); JsonStream k; k.begin(collect, nullptr); bool ok = k.feed(b, strlen(b)); CHECK(!(ok && k.complete())); }
  // truncated is not complete
  { JsonStream k; k.begin(collect, nullptr); const char *t = "{\"a\":[1,2"; k.feed(t, strlen(t)); CHECK(!k.complete() && !k.failed()); }
  // depth bomb
  { JsonStream k; k.begin(collect, nullptr); std::string s(40, '['); CHECK(!k.feed(s.data(), s.size())); }
  // long string truncated, not overflowed
  { ev.clear(); JsonStream k; k.begin(collect, nullptr); std::string s = "{\"k\":\"" + std::string(500, 'z') + "\"}"; k.feed(s.data(), s.size()); CHECK(k.complete() && ev.size() == 1 && ev[0].text.size() < 72 && k.truncated()); }
  // long path overflow does not crash
  { JsonStream k; k.begin(collect, nullptr); std::string s = "{"; for (int i = 0; i < 20; i++) s += "\"" + std::string(30, 'a' + i % 26) + "\":{"; s += "\"x\":1"; for (int i = 0; i < 20; i++) s += "}"; s += "}"; k.feed(s.data(), s.size()); CHECK(!k.complete() || true); }
  // whitespace / BOM tolerant
  { ev.clear(); JsonStream k; k.begin(collect, nullptr); const char *s = "\xEF\xBB\xBF  {\n \"a\" : 1 \n}\n"; CHECK(k.feed(s, strlen(s)) && k.complete()); }
}

struct Parsed { YahooParser p; uint32_t ts[STAGE_CAP]; float cl[STAGE_CAP]; bool ok; };
static void parseFile(Parsed &r, const std::string &body, uint16_t chunk = 97) {
  r.p.begin(r.ts, r.cl, STAGE_CAP);
  for (size_t i = 0; i < body.size(); i += chunk) r.p.feed(body.data() + i, std::min<size_t>(chunk, body.size() - i));
  r.ok = r.p.finish();
}

static void testYahoo(const char *dir) {
  std::string d = dir;
  { Parsed r; parseFile(r, readFile((d + "/q.json").c_str()));
    CHECK(r.ok);
    CHECK_NEAR(r.p.price, 6910.39, 1e-9); CHECK_NEAR(r.p.prevClose, 7175.83, 1e-9);
    CHECK_NEAR(r.p.dayHigh, 7198.67, 1e-9); CHECK_NEAR(r.p.dayLow, 6882.66, 1e-9);
    CHECK_NEAR(r.p.open, 7150.39, 0.01);
    CHECK(r.p.marketTime == 1795210554u); CHECK(r.p.gmtOffset == -18000); CHECK_STR(r.p.tz, "EST"); CHECK_STR(r.p.symbol, "^GSPC"); CHECK_STR(r.p.tzName, "America/New_York");
  }
  { Parsed r; parseFile(r, readFile((d + "/c1d.json").c_str()), 1);   // byte-at-a-time
    CHECK(r.ok && r.p.count == 79);
    CHECK(r.ts[0] == 1795185000u && r.ts[78] == 1795208400u);          // 09:30 .. 16:00 ET
    CHECK_NEAR(r.cl[0], 7135.5298, 0.01);  CHECK_NEAR(r.p.prevClose, 7175.83, 1e-9);
    for (int i = 1; i < r.p.count; i++) CHECK(r.ts[i] > r.ts[i - 1]);
    CHECK_NEAR(r.p.open, 7150.39, 0.01);
  }
  { Parsed r; parseFile(r, readFile((d + "/c5d.json").c_str())); CHECK(r.ok && r.p.count == 131); CHECK_NEAR(r.p.prevClose, 7175.83, 1e-9); }   // previousClose wins over chartPreviousClose (range start)
  { Parsed r; parseFile(r, readFile((d + "/c1m.json").c_str())); CHECK(r.ok && r.p.count == 23 && r.ts[0] == 1792589400u); }
  // 404 body
  { Parsed r; parseFile(r, "{\"chart\":{\"result\":null,\"error\":{\"code\":\"Not Found\",\"description\":\"No data found\"}}}");
    CHECK(!r.ok); CHECK_STR(r.p.error(), "Not Found"); }
  // HTML instead of JSON (captive portal / proxy)
  { Parsed r; parseFile(r, "<!DOCTYPE html><html></html>"); CHECK(!r.ok); CHECK_STR(r.p.error(), "bad JSON"); }
  // truncated body must not be accepted
  { std::string b = readFile((d + "/c1d.json").c_str()); b.resize(b.size() / 2); Parsed r; parseFile(r, b); CHECK(!r.ok); CHECK_STR(r.p.error(), "truncated response"); }
  // null closes and out-of-order stamps are dropped, not zero-filled
  { Parsed r; parseFile(r, "{\"chart\":{\"result\":[{\"meta\":{\"regularMarketPrice\":10.5},\"timestamp\":[100,200,300,250,400],"
                           "\"indicators\":{\"quote\":[{\"close\":[1.5,null,3.5,9.5,4.5]}]}}],\"error\":null}}");
    CHECK(r.ok && r.p.count == 3); CHECK(r.ts[0] == 100 && r.ts[1] == 300 && r.ts[2] == 400); CHECK_NEAR(r.cl[1], 3.5, 1e-6); }
  // too many points -> error, no overflow
  { std::string b = "{\"chart\":{\"result\":[{\"meta\":{},\"timestamp\":["; for (int i = 1; i <= 200; i++) { b += std::to_string(i); if (i < 200) b += ","; }
    b += "],\"indicators\":{\"quote\":[{\"close\":["; for (int i = 1; i <= 200; i++) { b += "1.5"; if (i < 200) b += ","; } b += "]}]}}],\"error\":null}}";
    Parsed r; parseFile(r, b); CHECK(!r.ok); CHECK_STR(r.p.error(), "too many points"); }
  // price missing from meta stays NaN (caller must treat as failure)
  { Parsed r; parseFile(r, "{\"chart\":{\"result\":[{\"meta\":{\"symbol\":\"X\"}}],\"error\":null}}"); CHECK(r.ok && isnan(r.p.price)); }
}

// ---------------------------------------------------------------- charts
static void testChartData() {
  uint32_t t[300]; float v[300];
  for (int i = 0; i < 300; i++) { t[i] = 1000 + i * 60; v[i] = 100 + (float)(i % 17); }
  v[123] = 500; v[200] = -5;      // spikes must survive downsampling
  uint32_t ot[64]; float ov[64];
  uint16_t n = downsampleMinMax(t, v, 300, ot, ov, 64);
  CHECK(n <= 64 && n >= 32);
  CHECK(ot[0] == t[0] && ot[n - 1] == t[299]);
  bool has500 = false, hasNeg = false; for (int i = 0; i < n; i++) { if (ov[i] == 500) has500 = true; if (ov[i] == -5) hasNeg = true; if (i) CHECK(ot[i] > ot[i - 1]); }
  CHECK(has500 && hasNeg);
  for (int i = 0; i < n; i++) { bool found = false; for (int j = 0; j < 300; j++) if (t[j] == ot[i] && v[j] == ov[i]) found = true; CHECK(found); }   // no invented points
  CHECK(downsampleMinMax(t, v, 10, ot, ov, 64) == 10 && ov[9] == v[9]);
  CHECK(downsampleMinMax(t, v, 0, ot, ov, 64) == 0);
  CHECK(downsampleMinMax(t, v, 300, ot, ov, 2) == 2 && ot[1] == t[299]);
  { uint32_t t2[2] = {1, 2}; float v2[2] = {1, 2}; CHECK(downsampleMinMax(t2, v2, 2, ot, ov, 64) == 2); }
  uint8_t r; CHECK(parseRange("1d", r) && r == RANGE_1D); CHECK(parseRange("5D", r) && r == RANGE_5D); CHECK(parseRange("1mo", r) && r == RANGE_1M);
  CHECK(!parseRange("1y", r)); CHECK(!parseRange(nullptr, r));
  uint32_t ct[40]; float cv[40]; Chart c; memset(&c, 0, sizeof c); c.t = ct; c.v = cv; c.cap = 40;
  chartStore(c, t, v, 300, 99.5f, 5, 7); CHECK(c.valid && c.n <= 40 && c.base == 99.5f && c.okMs == 5 && c.okEpoch == 7);
  chartStore(c, t, v, 1, NAN, 5, 7); CHECK(!c.valid);
}

static void sinkStr(void *c, const char *d, size_t n) { static_cast<std::string *>(c)->append(d, n); }
static void testWriter() {
  std::string out; {
    JsonWriter w(sinkStr, &out);
    w.beginObj().kInt("a", 1).kNum("b", NAN).kNum("c", -0.001).kNum("d", 12.345, 2).kBool("e", true).kStr("s", "q\"\\\n\x01").kStr("n", nullptr);
    w.key("arr").beginArr().integer(1).num(2.5, 1).beginObj().kInt("x", 2).endObj().beginArr().endArr().endArr();
    w.key("o").beginObj().endObj().endObj();
  }
  CHECK_STR(out, "{\"a\":1,\"b\":null,\"c\":0.00,\"d\":12.35,\"e\":true,\"s\":\"q\\\"\\\\\\n\\u0001\",\"n\":null,\"arr\":[1,2.5,{\"x\":2},[]],\"o\":{}}");
  // larger than the internal buffer: must stream across flushes intact
  std::string big; { JsonWriter w(sinkStr, &big); w.beginArr(); for (int i = 0; i < 500; i++) w.num(i * 1.5, 1); w.endArr(); }
  CHECK(big.size() > 2000 && big.front() == '[' && big.back() == ']' && big.find("747.0") != std::string::npos);
}


// ---------------------------------------------------------------- portfolio + settings
static void testPortfolio() {
  char s[SYMBOL_LEN];
  CHECK(normalizeSymbol("sp500", s, sizeof s) && !strcmp(s, "^GSPC"));
  CHECK(normalizeSymbol(" S&P 500 ", s, sizeof s) && !strcmp(s, "^GSPC"));
  CHECK(normalizeSymbol("spx", s, sizeof s) && !strcmp(s, "^SPX"));
  CHECK(normalizeSymbol("nasdaq", s, sizeof s) && !strcmp(s, "^NDX"));
  CHECK(normalizeSymbol("aapl", s, sizeof s) && !strcmp(s, "AAPL"));
  CHECK(normalizeSymbol("BRK-B", s, sizeof s) && normalizeSymbol("EURUSD=X", s, sizeof s));
  CHECK(!normalizeSymbol("", s, sizeof s) && !normalizeSymbol("AA PL", s, sizeof s) && !normalizeSymbol("A/B", s, sizeof s));
  CHECK(!normalizeSymbol("ABCDEFGHIJKLMNOPQ", s, sizeof s) && !normalizeSymbol("A?B", s, sizeof s) && !normalizeSymbol(nullptr, s, sizeof s));

  Position pos[MAX_POSITIONS]; uint8_t n = 0;
  CHECK(portfolioAdd(pos, n, "sp500", "2026-01-02", 10, 6000, 0) == ADD_OK && n == 1 && !strcmp(pos[0].symbol, "^GSPC"));
  CHECK(portfolioAdd(pos, n, "^GSPC", "2026-01-02", 12, 6100, 0) == ADD_UPDATED && n == 1 && pos[0].qty == 12);   // same symbol+date replaces
  CHECK(portfolioAdd(pos, n, "^GSPC", "2026-01-03", 1, 6000, 0) == ADD_OK && n == 2);
  CHECK(portfolioAdd(pos, n, "^GSPC", "2026-02-30", 1, 6000, 0) == ADD_BAD_DATE);
  CHECK(portfolioAdd(pos, n, "^GSPC", "2026-01-04", 0, 6000, 0) == ADD_BAD_QTY);
  CHECK(portfolioAdd(pos, n, "^GSPC", "2026-01-04", 1, -5, 0) == ADD_BAD_PRICE);
  CHECK(portfolioAdd(pos, n, "^G SPC", "2026-01-04", 1, 5, 0) == ADD_BAD_SYMBOL);
  CHECK(portfolioAdd(pos, n, "^GSPC", "2030-01-04", 1, 5, U(2026, 11, 20)) == ADD_BAD_DATE);   // future
  CHECK(portfolioAdd(pos, n, "^GSPC", "2026-11-21", 1, 5, U(2026, 11, 20, 23)) == ADD_OK);      // exchange "today" can be ahead of UTC
  CHECK(portfolioDelete(pos, n, 0) && n == 2 && !strcmp(pos[0].date, "2026-01-03") && !portfolioDelete(pos, n, 5));
  // caps
  Position big[MAX_POSITIONS]; uint8_t bn = 0; char d[11];
  for (int i = 0; i < MAX_POSITIONS; i++) { snprintf(d, sizeof d, "2026-01-%02d", i % 28 + 1); char sy[8]; snprintf(sy, sizeof sy, "S%d", i % 8); (void)portfolioAdd(big, bn, sy, d, 1, 1, 0); }
  CHECK(bn == MAX_POSITIONS || bn >= 20);
  while (bn < MAX_POSITIONS) { snprintf(d, sizeof d, "2025-02-%02d", bn % 28 + 1); if (portfolioAdd(big, bn, "S1", d, 1, 1, 0) == ADD_FULL) break; }
  CHECK(portfolioAdd(big, bn, "S1", "2020-05-05", 1, 1, 0) == ADD_FULL);
  { Position p9[MAX_POSITIONS]; uint8_t k = 0; for (int i = 0; i < 8; i++) { char sy[8]; snprintf(sy, sizeof sy, "T%d", i); CHECK(portfolioAdd(p9, k, sy, "2026-01-02", 1, 1, 0) == ADD_OK); }
    CHECK(portfolioAdd(p9, k, "T9", "2026-01-02", 1, 1, 0) == ADD_TOO_MANY_SYMBOLS); CHECK(portfolioAdd(p9, k, "T3", "2026-01-05", 1, 1, 0) == ADD_OK); }
  double q; CHECK(qtyFromThb(10000, 6382.1, q) && fabs(q - 1.566) < 1e-3); CHECK(!qtyFromThb(0, 5, q) && !qtyFromThb(5, 0, q) && !qtyFromThb(NAN, 5, q));

  // round trip + legacy file (original sketch format, any key order, missing date)
  std::string out; { JsonWriter w(sinkStr, &out); portfolioWrite(w, pos, n); }
  Position back[MAX_POSITIONS]; uint8_t bk = portfolioLoad(back, out.data(), out.size());
  CHECK(bk == n && !strcmp(back[0].symbol, pos[0].symbol) && !strcmp(back[0].date, pos[0].date) && fabs(back[0].qty - pos[0].qty) < 1e-9 && fabs(back[1].buy - pos[1].buy) < 1e-6);
  const char *legacy = "{\"positions\":[{\"symbol\":\"^GSPC\",\"qty\":39.17,\"buy\":6382.1,\"date\":\"2025-11-20\"},"
                       "{\"date\":\"2025-12-01\",\"buy\":6500,\"qty\":2,\"symbol\":\"spx\"},{\"symbol\":\"X\",\"qty\":\"bad\",\"buy\":1},"
                       "{\"symbol\":\"^GSPC\",\"qty\":1,\"buy\":2},{\"symbol\":\"\",\"qty\":1,\"buy\":2,\"date\":\"2025-01-01\"}]}";
  bk = portfolioLoad(back, legacy, strlen(legacy));
  CHECK(bk == 3 && !strcmp(back[1].symbol, "^SPX") && !strcmp(back[1].date, "2025-12-01") && back[1].qty == 2 && back[2].date[0] == 0);
  CHECK(portfolioLoad(back, "{\"positions\":[", 14) == 0 && portfolioLoad(back, "garbage", 7) == 0);

  // book sync keeps existing quotes, drops unused, adds new
  QuoteBook b; memset(&b, 0, sizeof b); addQ(b, "^GSPC", 100); addQ(b, "OLD", 5);
  Position bp[2] = {P("^GSPC", 1, 1), P("NEW", 1, 1)}; bookSync(b, bp, 2);
  CHECK(b.n == 2 && !strcmp(b.q[0].symbol, "^GSPC") && b.q[0].price == 100 && b.q[0].valid && !strcmp(b.q[1].symbol, "NEW") && !b.q[1].valid && !bookFind(b, "OLD"));
}

static bool applyS(Settings &s, const char *j, std::string *err = nullptr) { char e[80]; bool hc; bool ok = settingsApplyJson(s, j, strlen(j), e, sizeof e, &hc); if (err) *err = e; return ok; }
static void testSettings() {
  Settings s; settingsDefaults(s); std::string e;
  CHECK(s.portfolioRefreshS == 60 && s.chartRefreshS == 300 && s.pageRotateS == 8 && s.defaultRange == RANGE_1D && !strcmp(s.hostname, "sp500"));
  CHECK(applyS(s, "{\"quoteRefreshS\":30,\"defaultRange\":\"5d\",\"tzMode\":\"local\",\"localOffsetMin\":-300,\"hostname\":\"my-tv\"}") && s.quoteRefreshS == 30 && s.defaultRange == RANGE_5D && s.tzMode == 1 && s.localOffsetMin == -300 && !strcmp(s.hostname, "my-tv"));
  CHECK(!applyS(s, "{\"quoteRefreshS\":14}", &e) && e == "quoteRefreshS must be 15..3600");
  CHECK(!applyS(s, "{\"quoteRefreshS\":3601}")); CHECK(!applyS(s, "{\"quoteRefreshS\":\"30\"}")); CHECK(!applyS(s, "{\"quoteRefreshS\":30.5}")); CHECK(!applyS(s, "{\"quoteRefreshS\":1e2}"));
  CHECK(!applyS(s, "{\"chartRefreshS\":59}")); CHECK(applyS(s, "{\"chartRefreshS\":60}")); CHECK(!applyS(s, "{\"pageRotateS\":2}")); CHECK(!applyS(s, "{\"pageRotateS\":121}"));
  CHECK(!applyS(s, "{\"localOffsetMin\":841}")); CHECK(!applyS(s, "{\"localOffsetMin\":-721}"));
  CHECK(!applyS(s, "{\"defaultRange\":\"1y\"}")); CHECK(!applyS(s, "{\"tzMode\":\"mars\"}")); CHECK(!applyS(s, "{\"tzMode\":1}"));
  CHECK(!applyS(s, "{\"hostname\":\"Bad_Host\"}")); CHECK(!applyS(s, "{\"hostname\":\"-x\"}")); CHECK(!applyS(s, "{\"hostname\":\"\"}")); CHECK(!applyS(s, "{\"hostname\":\"a1234567890123456789012x\"}"));
  CHECK(!applyS(s, "not json")); CHECK(!applyS(s, "{\"a\":")); CHECK(!applyS(s, "[1]"));
  // all-or-nothing: valid field + invalid field changes nothing
  Settings keep = s; CHECK(!applyS(s, "{\"pageRotateS\":20,\"quoteRefreshS\":1}") && s.pageRotateS == keep.pageRotateS);
  CHECK(applyS(s, "{\"adminPass\":\"secret1\"}") && !strcmp(s.adminPass, "secret1"));
  CHECK(applyS(s, "{\"adminPass\":\"\"}") && !strcmp(s.adminPass, "secret1"));           // empty = unchanged
  CHECK(!applyS(s, "{\"adminPass\":\"abc\"}")); CHECK(!applyS(s, "{\"adminPass\":\"a:bcd\"}"));
  CHECK(applyS(s, "{\"clearAdminPass\":true}") && s.adminPass[0] == 0);
  // API output never contains the password
  strcpy(s.adminPass, "hunter22"); std::string o; { JsonWriter w(sinkStr, &o); settingsWrite(w, s, false, true); }
  CHECK(o.find("hunter22") == std::string::npos && o.find("\"adminPassSet\":true") != std::string::npos && o.find("\"limits\"") != std::string::npos && o.find("\"quoteRefreshS\":[15,3600]") != std::string::npos);
  // persisted file round trip; one corrupt field must not lose the others
  std::string f; { JsonWriter w(sinkStr, &f); settingsWrite(w, s, true, false); }
  Settings r; settingsDefaults(r); CHECK(settingsLoadJson(r, f.data(), f.size()) && !strcmp(r.adminPass, "hunter22") && r.chartRefreshS == s.chartRefreshS && r.tzMode == s.tzMode);
  Settings c; settingsDefaults(c); const char *corrupt = "{\"quoteRefreshS\":5,\"pageRotateS\":20,\"hostname\":\"BAD HOST\",\"defaultRange\":\"5d\"}";
  CHECK(settingsLoadJson(c, corrupt, strlen(corrupt)) && c.quoteRefreshS == 60 && c.pageRotateS == 20 && !strcmp(c.hostname, "sp500") && c.defaultRange == RANGE_5D);
  Settings g; settingsDefaults(g); CHECK(!settingsLoadJson(g, "{trunc", 6) && g.quoteRefreshS == 60);

  FlatJson fj; const char *b = "{\"ssid\":\"home\",\"password\":\"p\\\"w\",\"n\":5,\"x\":null,\"nested\":{\"a\":1},\"t\":true}";
  CHECK(fj.parse(b, strlen(b)) && !strcmp(fj.str("password"), "p\"w") && fj.isNull("x") && !fj.has("a")); double dv; bool bv;
  CHECK(fj.num("n", dv, true) && dv == 5 && fj.boolean("t", bv) && bv && !fj.num("ssid", dv)); CHECK(!fj.parse("{\"k\":\"\"", 7));
  std::string longPw = "{\"password\":\"" + std::string(80, 'x') + "\"}"; CHECK(!fj.parse(longPw.data(), longPw.size()));   // over-long value rejected, never truncated silently
  std::string manyKeys = "{"; for (int i = 0; i < 20; i++) manyKeys += (i ? "," : "") + std::string("\"k") + std::to_string(i) + "\":1"; manyKeys += "}"; CHECK(!fj.parse(manyKeys.data(), manyKeys.size()));
}

static void testAppState() {
  static AppState a; appInit(a);
  CHECK(a.charts[0].cap == 96 && a.charts[1].cap == 144 && a.charts[2].cap == 40 && a.charts[1].t == a.chartT + 96 && a.charts[2].v == a.chartV + 240);
  strcpy(a.positions[0].symbol, "^GSPC"); a.positions[0].qty = 2; a.positions[0].buy = 3000; a.posCount = 1; strcpy(a.positions[0].date, "2026-01-02");
  bookSync(a.book, a.positions, a.posCount);
  appRecompute(a, 1000, 0);
  CHECK(!a.clockSynced && !a.summary.complete && isnan(a.summary.nav) && a.quote.status == MS_UNKNOWN && !a.quote.valid);
  SymQuote *q = bookFind(a.book, "^GSPC"); q->price = 3500; q->valid = true; q->okMs = 1000; q->okEpoch = 1795210554; q->marketTime = 1795210554;
  uint32_t rev0 = a.rev;
  appRecompute(a, 2000, 1795210600);
  CHECK(a.clockSynced && a.summary.complete && fabs(a.summary.nav - 7000) < 1e-9 && fabs(a.summary.pl - 1000) < 1e-9 && a.portfolioEpoch == 1795210554u && a.rev != rev0);
  uint32_t rev1 = a.rev; appRecompute(a, 2500, 1795210601); CHECK(a.rev == rev1);               // nothing visible changed -> no repaint signal
  appRecompute(a, 1000 + 180001, 1795210600 + 180); CHECK(a.results[0].state == Q_STALE && a.rev != rev1);   // 3 x 60 s
}

static void testSchedule() {
  // Fri 2026-11-20: close 21:00 UTC (EST). Sat Nov 21 12:00 UTC -> last close = Fri 21:00; next open = Mon Nov 23 14:30 UTC
  CHECK(lastSessionCloseUtc(U(2026, 11, 21, 12)) == U(2026, 11, 20, 21));
  CHECK(nextSessionOpenUtc(U(2026, 11, 21, 12)) == U(2026, 11, 23, 14, 30));
  CHECK(lastSessionCloseUtc(U(2026, 11, 20, 20, 59)) == U(2026, 11, 19, 21));          // still open: previous close
  CHECK(lastSessionCloseUtc(U(2026, 11, 20, 21, 0)) == U(2026, 11, 20, 21));
  CHECK(nextSessionOpenUtc(U(2026, 11, 20, 14, 29)) == U(2026, 11, 20, 14, 30));
  CHECK(nextSessionOpenUtc(U(2026, 11, 20, 14, 30)) == U(2026, 11, 23, 14, 30));       // strictly after
  // Thanksgiving week: Thu 26 holiday, Fri 27 early close 18:00 UTC
  CHECK(lastSessionCloseUtc(U(2026, 11, 28, 0)) == U(2026, 11, 27, 18));
  CHECK(nextSessionOpenUtc(U(2026, 11, 25, 22)) == U(2026, 11, 27, 14, 30));            // skips the holiday
  // DST: Mon Mar 9 2026 open is 13:30 UTC (EDT), Fri Mar 6 close 21:00 UTC (EST)
  CHECK(nextSessionOpenUtc(U(2026, 3, 7, 12)) == U(2026, 3, 9, 13, 30));
  CHECK(lastSessionCloseUtc(U(2026, 3, 8, 12)) == U(2026, 3, 6, 21));
  CHECK(lastSessionCloseUtc(U(2026, 7, 11, 12)) == U(2026, 7, 10, 20));                 // summer close 20:00 UTC
  // UTC date ahead of ET date (Fri 21:30 ET = Sat 02:30 UTC)
  CHECK(lastSessionCloseUtc(U(2026, 11, 21, 2, 30)) == U(2026, 11, 20, 21));
}
static void testTicks() {
  double tk[8], step; int n;
  n = niceTicks(6882.66, 7198.67, tk, 5, &step);               // 316 pt range, <= 5 ticks -> step 100: 6900 7000 7100
  CHECK(n == 3 && step == 100 && tk[0] == 6900 && tk[2] == 7100);
  n = niceTicks(7.0, 7.4, tk, 5, &step);                        // 0.4 range -> 0.1 steps: 7.0..7.4 is 5 ticks
  CHECK(n == 5 && fabs(step - 0.1) < 1e-12 && fabs(tk[0] - 7.0) < 1e-9 && fabs(tk[4] - 7.4) < 1e-9);
  n = niceTicks(6320, 7850, tk, 4, &step);                      // 1530 range, <= 4 ticks -> step 500
  CHECK(n == 3 && step == 500 && tk[0] == 6500 && tk[2] == 7500);
  n = niceTicks(100, 100, tk, 5); CHECK(n == 1 && tk[0] == 100);           // flat series
  CHECK(niceTicks(5, 1, tk, 5) == 0 && niceTicks(NAN, 1, tk, 5) == 0 && niceTicks(1, 2, tk, 0) == 0);
  n = niceTicks(6999.9, 7000.1, tk, 4, &step); CHECK(n >= 1 && n <= 4 && tk[0] >= 6999.9 - 1e-9 && tk[n - 1] <= 7000.1 + 1e-9);   // tiny range
  for (double lo : {0.5, 99.5, 6882.66, 123456.0}) for (double span : {0.3, 7.0, 316.0, 5000.0}) {
    n = niceTicks(lo, lo + span, tk, 5);
    CHECK(n >= 1 && n <= 5);
    for (int i = 0; i < n; i++) CHECK(tk[i] >= lo - 1e-6 && tk[i] <= lo + span + 1e-6 && (i == 0 || tk[i] > tk[i - 1]));
  }
}
static void testViews() {
  using namespace display;
  View v[MAX_VIEWS]; uint8_t n;
  n = buildViews(v, MAX_VIEWS, 1, RANGE_1D, false, true);
  CHECK(n == 5 && v[0].kind == V_PORTFOLIO && v[1].kind == V_POSITIONS && v[1].arg == 0 && v[2].kind == V_MARKET && v[2].arg == 0 && v[3].arg == 1 && v[4].arg == 2);
  CHECK(ROWS_PER_WINDOW == 2);
  n = buildViews(v, MAX_VIEWS, 7, RANGE_1D, false, true);       // 7 rows -> 4 windows of 2: every row reachable
  CHECK(n == 1 + 4 + 3 && v[1].arg == 0 && v[2].arg == 1 && v[3].arg == 2 && v[4].arg == 3 && v[4].kind == V_POSITIONS && v[5].kind == V_MARKET);
  CHECK(windowCount(0) == 1 && windowCount(2) == 1 && windowCount(3) == 2 && windowCount(24) == 12);
  n = buildViews(v, MAX_VIEWS, 24, RANGE_5D, true, true);        // max positions + AP screen still fits
  CHECK(n == 1 + 12 + 3 + 1 && n <= MAX_VIEWS && v[13].kind == V_MARKET && v[13].arg == 1 && v[14].arg == 2 && v[15].arg == 0 && v[16].kind == V_AP);
  n = buildViews(v, MAX_VIEWS, 0, RANGE_1M, false, true);        // default range 1M: starts there, then wraps
  CHECK(n == 5 && v[2].arg == 2 && v[3].arg == 0 && v[4].arg == 1);
  n = buildViews(v, MAX_VIEWS, 5, RANGE_1D, false, false);       // no saved Wi-Fi: AP screen only
  CHECK(n == 1 && v[0].kind == V_AP);
  n = buildViews(v, 3, 24, RANGE_1D, true, true); CHECK(n == 3);                // never overruns the caller's array
  CHECK(nextIndex(4, 5) == 0 && nextIndex(0, 5) == 1 && nextIndex(0, 0) == 0);
  CHECK(pageOf(V_PORTFOLIO) == 0 && pageOf(V_POSITIONS) == 1 && pageOf(V_MARKET) == 2);
}
static void testPickClose() {
  // daily bars stamped 09:30 ET (14:30 UTC in winter): Mon 16, Tue 17, Wed 18, Thu 19, Fri 20 Nov 2026
  uint32_t t[5]; float v[5] = {100, 101, 102, 103, 104};
  for (int i = 0; i < 5; i++) t[i] = U(2026, 11, 16 + i, 14, 30);
  double px; bool ex;
  CHECK(pickCloseOnDate(t, v, 5, 2026, 11, 18, px, &ex) && px == 102 && ex);
  CHECK(pickCloseOnDate(t, v, 5, 2026, 11, 21, px, &ex) && px == 104 && !ex);           // Saturday -> Friday's close (not a later bar)
  CHECK(pickCloseOnDate(t, v, 5, 2026, 11, 22, px, &ex) && px == 104 && !ex);           // Sunday
  CHECK(pickCloseOnDate(t, v, 5, 2026, 12, 25, px, &ex) && px == 104 && !ex);           // after all bars -> newest
  CHECK(!pickCloseOnDate(t, v, 5, 2026, 11, 15, px, &ex));                              // before all bars: no price is invented
  { uint32_t t2[2] = {U(2026, 11, 20, 14, 30), U(2026, 11, 23, 14, 30)}; float v2[2] = {7, 8};
    CHECK(pickCloseOnDate(t2, v2, 2, 2026, 11, 22, px, &ex) && px == 7 && !ex); }         // weekend between bars: NOT the bar after the date
  { float z[2] = {0, 0}; uint32_t t2[2] = {1, 2}; CHECK(!pickCloseOnDate(t2, z, 2, 2026, 11, 22, px)); CHECK(!pickCloseOnDate(t2, z, 0, 2026, 11, 22, px)); }
  // non-US exchange: Tokyo bars stamped 09:00 JST (= 00:00 UTC) belong to the Tokyo date, not the previous ET date
  { uint32_t tk[2] = {U(2026, 11, 19, 0, 0), U(2026, 11, 20, 0, 0)}; float vk[2] = {10, 11};
    CHECK(pickCloseOnDate(tk, vk, 2, 2026, 11, 20, px, &ex, 9 * 3600) && px == 11 && ex);
    CHECK(pickCloseOnDate(tk, vk, 2, 2026, 11, 21, px, &ex, 9 * 3600) && px == 11 && !ex); }
  // Late-evening ET stamp: 20:00 ET Nov 18 = 01:00 UTC Nov 19 must still count as Nov 18
  { uint32_t t3[1] = {U(2026, 11, 19, 1, 0)}; float v3[1] = {55}; CHECK(pickCloseOnDate(t3, v3, 1, 2026, 11, 18, px, &ex) && ex); }
}

int main(int argc, char **argv) {
  testClock(); testFormat(); testCalculator(); testQuoteView(); testChartStats(); testJson();
  testYahoo(argc > 1 ? argv[1] : "fixtures"); testChartData(); testWriter(); testPortfolio(); testSettings(); testAppState(); testSchedule(); testPickClose(); testTicks(); testViews();
  printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
