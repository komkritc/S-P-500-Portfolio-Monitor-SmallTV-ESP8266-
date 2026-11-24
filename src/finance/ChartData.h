// ChartData.h - chart storage helpers (pure, host-testable).
#pragma once
#include "../core/AppTypes.h"

// Min/max-preserving downsample to at most maxOut points (keeps the first and last point and the
// extremes of every bucket, in time order). Never invents points. Returns the number written.
uint16_t downsampleMinMax(const uint32_t *t, const float *v, uint16_t n, uint32_t *outT, float *outV, uint16_t maxOut);

// Copy staged points into a stored chart (downsampling when it does not fit its capacity).
void chartStore(Chart &c, const uint32_t *t, const float *v, uint16_t n, float base, uint32_t okMs, uint32_t okEpoch);

const char *rangeName(uint8_t r);        // "1d" "5d" "1mo"
const char *rangeLabel(uint8_t r);       // "1D" "5D" "1M"
const char *rangeInterval(uint8_t r);    // "5m" "15m" "1d"
bool parseRange(const char *s, uint8_t &r);

// Close price for the buy date (original sketch's fetchCloseOnDate rule, tightened): the bar whose
// exchange-local date equals y-m-d; otherwise the last bar BEFORE that date (weekend/holiday);
// a date after every bar therefore yields the newest bar. A date before every bar returns false
// (no price is invented). Returns false when no bar qualifies.
// `fixedOffsetSec` = exchange UTC offset for non-US symbols; EXCHANGE_NY (default) uses the New York DST rules.
static const int32_t EXCHANGE_NY = 0x7fffffff;
bool pickCloseOnDate(const uint32_t *t, const float *v, uint16_t n, int y, int m, int d, double &price, bool *exact = nullptr, int32_t fixedOffsetSec = EXCHANGE_NY);

// "Nice" axis ticks (1/2/2.5/5 x 10^k) inside [lo, hi], at most maxN. Returns the count; ticks[] ascending.
// Pure, host-tested. A flat series (hi == lo) yields one tick at that value.
int niceTicks(double lo, double hi, double *ticks, int maxN, double *stepOut = nullptr);
