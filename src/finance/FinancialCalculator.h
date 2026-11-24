// FinancialCalculator.h - THE one calculation engine. The LCD and the web API both read
// its output (PositionResult / PortfolioSummary / QuoteView / ChartStats); neither
// recomputes totals.
//
//   Cost = qty x buy            NAV = qty x last          P/L = NAV - Cost
//   Return% = P/L / Cost x 100  Total* = sums of valid positions
#pragma once
#include "../core/AppTypes.h"

struct QuoteBook {
  SymQuote q[MAX_SYMBOLS];
  uint8_t  n;
};

const SymQuote *findQuote(const QuoteBook &b, const char *symbol);

bool positionValid(const Position &p);     // finite qty > 0 and buy > 0, symbol non-empty

// nowMs/staleAfterMs decide fresh vs stale for quotes that were refreshed this boot; a quote
// loaded from flash, or whose latest refresh failed, is always stale.
void computePosition(const Position &p, const SymQuote *q, uint32_t nowMs, uint32_t staleAfterMs, PositionResult &out);
void computePortfolio(const Position *pos, uint8_t n, const QuoteBook &book, uint32_t nowMs, uint32_t staleAfterMs,
                      PositionResult *results /* n entries, may be null */, PortfolioSummary &out);

// S&P 500 derived values. Daily change = last - prevClose; pct = change / prevClose x 100 (safe).
void computeQuoteView(const SP500Quote &q, uint32_t nowEpoch, uint32_t nowMs, uint32_t staleAfterMs, QuoteView &out);

// Selected-period statistics. base = chart.base when finite, else the first point.
void computeChartStats(const Chart &c, ChartStats &out);

double safeDiv(double a, double b);        // NaN when b is 0/NaN
