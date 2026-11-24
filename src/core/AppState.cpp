#include "AppState.h"
#include <string.h>

AppState g_app;

void settingsDefaults(Settings &s) {
  memset(&s, 0, sizeof s);
  s.portfolioRefreshS = 60;
  s.quoteRefreshS = 60;
  s.chartRefreshS = 300;
  s.pageRotateS = 8;
  s.defaultRange = RANGE_1D;
  s.tzMode = 0;
  s.localOffsetMin = 420;       // UTC+7 (the original sketch's GMT_OFFSET_SEC = 7 h)
  strcpy(s.hostname, "sp500");
}

void appInit(AppState &a) {
  memset(&a, 0, sizeof a);
  settingsDefaults(a.settings);
  uint16_t off = 0;
  for (uint8_t r = 0; r < RANGE_COUNT; r++) {
    a.charts[r].t = a.chartT + off;
    a.charts[r].v = a.chartV + off;
    a.charts[r].cap = RANGE_CAP[r];
    off += RANGE_CAP[r];
    a.charts[r].base = NAN;
  }
  a.sp.last = a.sp.prevClose = a.sp.open = a.sp.high = a.sp.low = NAN;
  a.summary.realisedPl = a.summary.lifetimePl = NAN;
  a.effPortfolioS = a.settings.portfolioRefreshS;
  a.effQuoteS = a.settings.quoteRefreshS;
  a.effChartS = a.settings.chartRefreshS;
}

static uint32_t staleMs(uint16_t intervalS) { return (uint32_t)intervalS * 3000UL; }

void appRecompute(AppState &a, uint32_t nowMs, uint32_t nowEpoch) {
  a.nowMs = nowMs;
  a.nowEpoch = nowEpoch;
  a.clockSynced = nowEpoch > 1600000000UL;

  computePortfolio(a.positions, a.posCount, a.book, nowMs, staleMs(a.effPortfolioS), a.results, a.summary);
  computeQuoteView(a.sp, a.clockSynced ? nowEpoch : 0, nowMs, staleMs(a.effQuoteS), a.quote);
  for (uint8_t r = 0; r < RANGE_COUNT; r++) {
    const Chart &c = a.charts[r];
    computeChartStats(c, a.chartStats[r]);
    a.chartStale[r] = c.valid && (c.failed || c.fromCache || (uint32_t)(nowMs - c.okMs) > staleMs(a.effChartS));
  }
  uint32_t newest = 0;
  for (uint8_t i = 0; i < a.book.n; i++) if (a.book.q[i].valid && a.book.q[i].okEpoch > newest) newest = a.book.q[i].okEpoch;
  a.portfolioEpoch = newest;

  // Lifetime = open (unrealised) + realised. N/A (NaN) while either side is unknown: never 0.
  a.summary.realisedPl = a.realised.valid ? a.realised.pl : NAN;
  a.summary.lifetimePl = (isNum(a.summary.pl) && isNum(a.summary.realisedPl)) ? a.summary.pl + a.summary.realisedPl : NAN;

  // Visible-change signature: the LCD repaints from this instead of diffing the whole state.
  uint32_t sig = a.posCount * 31u + a.summary.priced + (a.summary.stale ? 7u : 0) + (a.summary.complete ? 13u : 0)
               + (uint32_t)a.quote.status * 17u + (a.quote.valid ? 3u : 0) + (a.quote.stale ? 5u : 0);
  sig = sig * 2654435761u + (uint32_t)(int64_t)(isNum(a.summary.nav) ? a.summary.nav * 100 : 0);
  sig = sig * 2654435761u + (uint32_t)(int64_t)(isNum(a.quote.last) ? a.quote.last * 100 : 0);
  sig = sig * 2654435761u + a.quote.marketTime;
  for (uint8_t r = 0; r < RANGE_COUNT; r++) sig = sig * 31u + a.charts[r].n + (a.chartStale[r] ? 1u : 0) + (uint32_t)a.charts[r].okEpoch;
  // sale transactions: a new record or an undo repaints the portfolio page and page 4
  sig = sig * 2654435761u + a.pmeta.ver + a.realised.records;
  sig = sig * 2654435761u + (uint32_t)(int64_t)(isNum(a.realised.pl) ? a.realised.pl * 100 : 0);
  static uint32_t lastSig = 0;
  if (sig != lastSig) { lastSig = sig; a.rev++; }
}
