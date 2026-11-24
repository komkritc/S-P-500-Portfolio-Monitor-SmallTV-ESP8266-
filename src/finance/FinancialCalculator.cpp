#include "FinancialCalculator.h"
#include "../core/MarketClock.h"
#include <string.h>

double safeDiv(double a, double b) {
  if (!isNum(a) || !isNum(b) || b == 0.0) return NAN;
  return a / b;
}

const SymQuote *findQuote(const QuoteBook &b, const char *symbol) {
  for (uint8_t i = 0; i < b.n; i++)
    if (strcmp(b.q[i].symbol, symbol) == 0) return &b.q[i];
  return nullptr;
}

bool positionValid(const Position &p) {
  return p.symbol[0] && isNum(p.qty) && p.qty > 0 && isNum(p.buy) && p.buy > 0;
}

static bool isStale(const SymQuote &q, uint32_t nowMs, uint32_t staleAfterMs) {
  if (q.failed || q.fromCache) return true;
  return (uint32_t)(nowMs - q.okMs) > staleAfterMs;
}

void computePosition(const Position &p, const SymQuote *q, uint32_t nowMs, uint32_t staleAfterMs, PositionResult &r) {
  r.price = r.cost = r.nav = r.pl = r.retPct = NAN;
  r.quoteEpoch = 0;
  if (!positionValid(p)) { r.state = Q_INVALID; return; }
  r.cost = p.qty * p.buy;
  if (!q || !q->valid || !isNum(q->price) || q->price <= 0) { r.state = Q_MISSING; return; }
  r.price = q->price;
  r.nav = p.qty * q->price;
  r.pl = r.nav - r.cost;
  r.retPct = safeDiv(r.pl, r.cost) * 100.0;
  r.quoteEpoch = q->marketTime;
  r.state = isStale(*q, nowMs, staleAfterMs) ? Q_STALE : Q_FRESH;
}

void computePortfolio(const Position *pos, uint8_t n, const QuoteBook &book, uint32_t nowMs, uint32_t staleAfterMs,
                      PositionResult *results, PortfolioSummary &s) {
  memset(&s, 0, sizeof s);
  s.count = n;
  double invested = 0, nav = 0;
  PositionResult local;
  for (uint8_t i = 0; i < n; i++) {
    PositionResult &r = results ? results[i] : local;
    computePosition(pos[i], findQuote(book, pos[i].symbol), nowMs, staleAfterMs, r);
    if (r.state == Q_INVALID) { s.invalid++; continue; }
    invested += r.cost;
    if (r.state == Q_MISSING) continue;
    nav += r.nav;
    s.priced++;
    if (r.state == Q_STALE) s.stale = true;
  }
  const uint8_t valid = (uint8_t)(n - s.invalid);
  s.invested = invested;
  s.complete = valid > 0 && s.priced == valid;
  s.nav = s.complete ? nav : NAN;
  s.pl = s.complete ? nav - invested : NAN;
  s.retPct = s.complete ? safeDiv(s.pl, invested) * 100.0 : NAN;
}

void computeQuoteView(const SP500Quote &q, uint32_t nowEpoch, uint32_t nowMs, uint32_t staleAfterMs, QuoteView &v) {
  memset(&v, 0, sizeof v);
  v.last = v.prevClose = v.change = v.changePct = v.open = v.high = v.low = NAN;
  v.status = marketStatusAt(nowEpoch, q.valid ? q.marketTime : 0);
  if (!q.valid || !isNum(q.last)) return;
  v.valid = true;
  v.last = q.last;
  v.prevClose = isNum(q.prevClose) && q.prevClose > 0 ? q.prevClose : NAN;
  v.change = isNum(v.prevClose) ? v.last - v.prevClose : NAN;
  v.changePct = safeDiv(v.change, v.prevClose) * 100.0;
  v.open = q.open; v.high = q.high; v.low = q.low;
  v.marketTime = q.marketTime;
  v.fetchedEpoch = q.okEpoch;
  formatSessionDate(v.sessionDate, sizeof v.sessionDate, q.marketTime);
  strncpy(v.tz, q.tz, sizeof v.tz - 1);
  v.stale = q.failed || q.fromCache || (uint32_t)(nowMs - q.okMs) > staleAfterMs;
}

void computeChartStats(const Chart &c, ChartStats &s) {
  memset(&s, 0, sizeof s);
  s.first = s.last = s.base = s.change = s.changePct = s.minV = s.maxV = NAN;
  if (!c.valid || c.n < 2) return;
  s.valid = true;
  s.first = c.v[0];
  s.last = c.v[c.n - 1];
  s.minV = s.maxV = c.v[0];
  for (uint16_t i = 1; i < c.n; i++) {
    if (c.v[i] < s.minV) s.minV = c.v[i];
    if (c.v[i] > s.maxV) s.maxV = c.v[i];
  }
  s.base = (isNum(c.base) && c.base > 0) ? c.base : s.first;
  s.change = s.last - s.base;
  s.changePct = safeDiv(s.change, s.base) * 100.0;
}
