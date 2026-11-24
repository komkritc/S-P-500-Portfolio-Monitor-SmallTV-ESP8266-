// Renders the real LCD screens (display/*.cpp + the real Inter font renderer) on the host for a set of
// scenarios, with data from the recorded Yahoo fixtures. Checks: no out-of-bounds pixels, no overlapping
// text slots, every text inside the screen. Writes <outdir>/<name>.raw (RGB565 LE) for raw2png.py.
#define UI_TEST_HOOKS 1
#include <Arduino_GFX_Library.h>
#include "../../src/core/AppState.h"
#include "../../src/core/MarketClock.h"
#include "../../src/finance/Portfolio.h"
#include "../../src/finance/ChartData.h"
#include "../../src/finance/YahooParser.h"
#include "../../src/display/UiKit.h"
#include "../../src/display/ViewSchedule.h"
#include "../../src/display/Widgets.h"
#include "../../src/display/PortfolioScreen.h"
#include "../../src/display/PositionsScreen.h"
#include "../../src/display/RealisedScreen.h"
#include "../../src/display/MarketScreen.h"
#include "../../src/display/ChartRenderer.h"
#include "../../src/finance/SaleEngine.h"
#include <string>
#include <vector>

static Arduino_GFX G;
static ui::Ui U;
static int g_problems = 0, g_frames = 0;
static std::string g_dir;
static const uint32_t NOW_CLOSED = 1795210600;   // Fri 2026-11-20 16:36:40 ET: market closed, quote = the 16:00 close

static std::string readFile(const std::string &p) {
  FILE *f = fopen(p.c_str(), "rb"); if (!f) { printf("cannot open %s\n", p.c_str()); exit(2); }
  std::string s; char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f))) s.append(b, n); fclose(f); return s;
}

struct Fx { SP500Quote sp; double h; };

static void loadCharts(AppState &a, uint32_t nowMs, uint32_t epoch) {
  static const char *files[3] = {"c1d", "c5d", "c1m"};
  for (int r = 0; r < 3; r++) {
    std::string body = readFile(g_dir + "/fixtures/" + files[r] + ".json");
    static uint32_t ts[STAGE_CAP]; static float cl[STAGE_CAP];
    YahooParser p; p.begin(ts, cl, STAGE_CAP); p.feed(body.data(), body.size()); p.finish();
    float base = r == RANGE_1D ? (float)p.prevClose : NAN;
    chartStore(a.charts[r], ts, cl, p.count, base, nowMs, epoch);
  }
}

static void loadQuote(AppState &a, uint32_t nowMs, uint32_t epoch) {
  std::string body = readFile(g_dir + "/fixtures/q.json");
  YahooParser p; p.begin(nullptr, nullptr, 0); p.feed(body.data(), body.size()); p.finish();
  SP500Quote &q = a.sp; memset(&q, 0, sizeof q);
  q.valid = true; q.last = p.price; q.prevClose = p.prevClose; q.open = p.open; q.high = p.dayHigh; q.low = p.dayLow;
  q.marketTime = p.marketTime; strcpy(q.tz, p.tz); q.okMs = nowMs; q.okEpoch = epoch;
  SymQuote *b = bookFind(a.book, SP500_SYMBOL);
  if (b) { b->valid = true; b->price = p.price; b->marketTime = p.marketTime; b->okMs = nowMs; b->okEpoch = epoch; }
}

static void addPos(AppState &a, const char *sym, const char *date, double qty, double buy) { portfolioAdd(a.positions, a.posCount, a.pmeta, sym, date, qty, buy, 0); }
static void setPrice(AppState &a, const char *sym, double px, uint32_t nowMs) {
  SymQuote *q = bookFind(a.book, sym); if (!q) return;
  q->valid = true; q->price = px; q->marketTime = 1795210554; q->okMs = nowMs; q->okEpoch = 1795210560; q->failed = false; q->fromCache = false;
}

enum Scn { S_NORMAL, S_LOSS, S_MANY, S_MISSING, S_STALE, S_EMPTY, S_NODATA, S_BIG, S_REALISED, S_REALBIG };

// A synthetic ledger line (the engine is the same one the device runs on /sells.jsonl).
static void addSale(AppState &a, const char *id, const char *date, double realised) {
  SaleRecord r; memset(&r, 0, sizeof r);
  r.seq = a.realised.records + 1; r.ts = NOW_CLOSED; r.type = REC_SALE;
  strncpy(r.id, id, sizeof r.id - 1); strncpy(r.symbol, "^GSPC", sizeof r.symbol - 1);
  strncpy(r.date, date, sizeof r.date - 1);
  r.realised = realised; r.nAlloc = 1; r.alloc[0].lot = 1;
  sale::summaryAdd(a.realised, r);
}

// Undo = an appended reversal record (never an edit); the summary must net back out.
static void addReverse(AppState &a, const char *id, const char *refId, const char *date, double realised) {
  SaleRecord r; memset(&r, 0, sizeof r);
  r.seq = a.realised.records + 1; r.ts = NOW_CLOSED; r.type = REC_REVERSAL;
  strncpy(r.id, id, sizeof r.id - 1); strncpy(r.refId, refId, sizeof r.refId - 1);
  strncpy(r.symbol, "^GSPC", sizeof r.symbol - 1); strncpy(r.date, date, sizeof r.date - 1);
  r.realised = -realised; r.nAlloc = 1; r.alloc[0].lot = 1;
  sale::summaryAdd(a.realised, r);
}

static void build(AppState &a, Scn s, uint32_t epoch) {
  appInit(a);
  a.net.mode = NetInfo::NET_STA; a.net.connected = s != S_NODATA; a.net.rssi = -58; strcpy(a.net.ip, "192.168.1.50"); strcpy(a.net.ssid, "home");
  const uint32_t nowMs = 100000;
  if (s == S_EMPTY) { /* no positions */ }
  else if (s == S_MANY) {
    addPos(a, "^GSPC", "2025-11-20", 39.17, 6382.10); addPos(a, "^GSPC", "2026-01-05", 12.5, 7010.5); addPos(a, "^GSPC", "2026-02-02", 3.25, 7300);
    addPos(a, "^GSPC", "2026-03-02", 1.1, 6100); addPos(a, "^GSPC", "2026-04-01", 100.123456, 6900); addPos(a, "^GSPC", "2026-05-01", 0.5, 7800); addPos(a, "^GSPC", "2026-06-01", 2, 6500);
  } else if (s == S_REALISED) {
    addPos(a, "^GSPC", "2025-11-20", 39.17, 6382.10); addPos(a, "^GSPC", "2026-01-05", 12.5, 7010.5); addPos(a, "^GSPC", "2026-02-02", 3.25, 6200);
    sale::summaryReset(a.realised);
    addSale(a, "TX-000001", "2026-10-15", 2450.75);
    addSale(a, "TX-000002", "2025-06-01", -800.25);
    addReverse(a, "TX-000003", "TX-000002", "2025-06-01", -800.25);
    a.realised.hasLast = true; strcpy(a.realised.lastId, "TX-000001"); strcpy(a.realised.lastSymbol, "^GSPC");
    strcpy(a.realised.lastDate, "2026-10-15"); a.realised.lastRealised = 2450.75; a.realised.lastTs = NOW_CLOSED;
  } else if (s == S_REALBIG) {
    addPos(a, "^GSPC", "2025-11-20", 39.17, 6382.10);
    sale::summaryReset(a.realised);
    addSale(a, "TX-000001", "2026-11-19", -1234567.89);
    a.realised.hasLast = true; strcpy(a.realised.lastId, "TX-000001"); strcpy(a.realised.lastSymbol, "^GSPC");
    strcpy(a.realised.lastDate, "2026-11-19"); a.realised.lastRealised = -1234567.89; a.realised.lastTs = NOW_CLOSED;
  } else if (s == S_BIG) { addPos(a, "^GSPC", "2025-11-20", 1234567.891, 6382.10); }
  else if (s == S_LOSS) { addPos(a, "^GSPC", "2026-02-02", 20, 7300); addPos(a, "^GSPC", "2026-03-03", 5, 7500); }
  else { addPos(a, "^GSPC", "2025-11-20", 39.17, 6382.10); addPos(a, "^GSPC", "2026-01-05", 12.5, 7010.5); addPos(a, "^GSPC", "2026-02-02", 3.25, 6200); }
  bookSync(a.book, a.positions, a.posCount);
  if (!bookFind(a.book, SP500_SYMBOL)) { SymQuote &q = a.book.q[a.book.n++]; memset(&q, 0, sizeof q); strcpy(q.symbol, SP500_SYMBOL); q.price = NAN; }
  if (s == S_NODATA) { a.sp.last = a.sp.prevClose = a.sp.open = a.sp.high = a.sp.low = NAN; }
  else {
    loadQuote(a, nowMs, epoch);
    if (s != S_MISSING) { /* book price comes from the quote */ }
    if (s == S_MISSING) { SymQuote *q = bookFind(a.book, SP500_SYMBOL); q->valid = false; q->price = NAN; }
    loadCharts(a, nowMs, epoch);
  }
  if (s == S_STALE) { a.sp.failed = true; for (int i = 0; i < a.book.n; i++) a.book.q[i].failed = true; for (auto &c : a.charts) c.failed = true; }
  a.net.mode = NetInfo::NET_STA;
  appRecompute(a, nowMs, epoch);
}

static void checkOverlap(const char *name) {
  std::vector<const ui::Slot *> v;
  for (int i = 0; i < ui::MAX_SLOTS; i++) { const ui::Slot *s = U.slotAt(i); if (s->used) v.push_back(s); }
  for (size_t i = 0; i < v.size(); i++) {
    const ui::Slot *a = v[i];
    if (a->x < 0 || a->y < 0 || a->x + a->w > 240 || a->y + a->h > 240) { printf("  PROBLEM %s: text \"%s\" outside screen (%d,%d %dx%d)\n", name, a->s, a->x, a->y, a->w, a->h); g_problems++; }
    for (size_t j = i + 1; j < v.size(); j++) {
      const ui::Slot *b = v[j];
      bool ov = a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
      if (ov) { printf("  PROBLEM %s: \"%s\" (%d,%d %dx%d) overlaps \"%s\" (%d,%d %dx%d)\n", name, a->s, a->x, a->y, a->w, a->h, b->s, b->x, b->y, b->w, b->h); g_problems++; }
    }
  }
}

static void dump(const char *name) {
  std::string p = g_dir + "/preview/out/" + name + ".raw";
  FILE *f = fopen(p.c_str(), "wb"); fwrite(G.fb, 2, 240 * 240, f); fclose(f);
  printf("%-24s oob=%ld", name, G.oob); if (G.oob) printf("  first at (%d,%d)", G.oobX, G.oobY); printf("\n");
  if (G.oob) { g_problems++; }
  checkOverlap(name);
  g_frames++;
}

static void fresh() { U.clear(); display::resetWidgetCache(); G.oob = 0; }

int main(int argc, char **argv) {
  g_dir = argc > 1 ? argv[1] : ".";
  U.begin(&G);
  static AppState a;
  struct { Scn s; const char *n; } scn[] = {{S_NORMAL, "normal"}, {S_LOSS, "loss"}, {S_MANY, "many"}, {S_MISSING, "missing"}, {S_STALE, "stale"}, {S_EMPTY, "empty"}, {S_NODATA, "nodata"}, {S_BIG, "bignum"}, {S_REALISED, "realised"}, {S_REALBIG, "realbig"}};
  for (auto &sc : scn) {
    build(a, sc.s, NOW_CLOSED);
    char n[64];
    fresh(); display::drawPortfolio(U, a, true); snprintf(n, sizeof n, "%s_p1_portfolio", sc.n); dump(n);
    for (uint8_t w = 0; w < display::windowCount(a.posCount); w++) { fresh(); display::drawPositions(U, a, w, true); snprintf(n, sizeof n, "%s_p2_positions_w%u", sc.n, w); dump(n); }
    fresh(); display::drawRealised(U, a, true); snprintf(n, sizeof n, "%s_p4_realised", sc.n); dump(n);      // page 4 (also the "no sales yet" branch)
    for (uint8_t r = 0; r < 3; r++) {
      fresh(); display::ChartParams cp = {r, 0, 0, a.chartStale[r]}; (void)cp;
      display::drawMarket(U, a, r, true, true); snprintf(n, sizeof n, "%s_p3_market_%s", sc.n, rangeLabel(r)); dump(n);
    }
  }
  // incremental path: change a value without a full repaint and compare with a fresh render
  build(a, S_NORMAL, NOW_CLOSED);
  fresh(); display::drawPortfolio(U, a, true);
  SymQuote *q = bookFind(a.book, SP500_SYMBOL); q->price = 6000; a.sp.last = 6000; appRecompute(a, 100000, NOW_CLOSED);
  G.oob = 0; display::drawPortfolio(U, a, false);
  static uint16_t inc[240 * 240]; memcpy(inc, G.fb, sizeof inc);
  fresh(); display::drawPortfolio(U, a, true);
  long diff = 0; for (int i = 0; i < 240 * 240; i++) if (inc[i] != G.fb[i]) diff++;
  printf("incremental vs full repaint: %ld differing pixels\n", diff);
  { int x0 = 999, y0 = 999, x1 = -1, y1 = -1; for (int i = 0; i < 240 * 240; i++) if (inc[i] != G.fb[i]) { int x = i % 240, y = i / 240; x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); } if (diff) printf("  diff bbox (%d,%d)-(%d,%d)\n", x0, y0, x1, y1); int shown = 0; for (int i = 0; i < 240 * 240 && shown < 12; i++) if (inc[i] != G.fb[i]) { printf("   (%d,%d) inc=%04x full=%04x\n", i % 240, i / 240, inc[i], G.fb[i]); shown++; } }
  if (diff) g_problems++;
  // open-market status label
  build(a, S_NORMAL, 1795210600 - 6 * 3600 - 20 * 60);   // 10:16 ET Friday, quote older than the open -> not live
  a.sp.marketTime = 1795210554 - 86400;                    // Thursday's close
  appRecompute(a, 100000, 1795210600 - 6 * 3600 - 20 * 60);
  printf("status 10:16 ET with Friday-close quote = %s\n", a.quote.status == MS_CLOSED ? "CLOSED (correct: old quote is not live)" : "WRONG");
  if (a.quote.status != MS_CLOSED) g_problems++;
  printf("%d frames, %d problems\n", g_frames, g_problems);
  return g_problems ? 1 : 0;
}
