// ChartData.h - chart storage helpers (pure, host-testable).
#pragma once
#include "../core/AppTypes.h"

// Min/max-preserving downsample to at most maxOut points (keeps the first and last point and the
// extremes of every bucket, in time order). Never invents points. Returns the number written.
uint16_t downsampleMinMax(const uint32_t *t, const float *v, uint16_t n, uint32_t *outT, float *outV, uint16_t maxOut);

// Copy staged points into a stored chart (downsampling when it does not fit its capacity).
// `wRaw`, when the chart holds candles (c.w != null), is the parser's packed bar staging laid out as
// three arrays of `wRawCap` int16: opens, highs, lows (ohlcPack'ed, OHLC_BAD where Yahoo sent null).
// A chart with candles is never downsampled - the newest c.cap bars are kept so every candle stays
// attached to its own bar; bars whose candle is missing or contradicts open/close are stored as
// OHLC_BAD, which the API reports as null instead of inventing a range.
void chartStore(Chart &c, const uint32_t *t, const float *v, uint16_t n, float base, uint32_t okMs, uint32_t okEpoch,
                const int16_t *wRaw = nullptr, uint16_t wRawCap = 0);

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
// When *exact is false the close was borrowed from that earlier session: `barTs` (optional) reports the
// bar that was used, and the caller MUST show the user its date instead of silently inventing one.
bool pickCloseOnDate(const uint32_t *t, const float *v, uint16_t n, int y, int m, int d, double &price, bool *exact = nullptr, int32_t fixedOffsetSec = EXCHANGE_NY, uint32_t *barTs = nullptr);

// "Nice" axis ticks (1/2/2.5/5 x 10^k) inside [lo, hi], at most maxN. Returns the count; ticks[] ascending.
// Pure, host-tested. A flat series (hi == lo) yields one tick at that value.
int niceTicks(double lo, double hi, double *ticks, int maxN, double *stepOut = nullptr);
