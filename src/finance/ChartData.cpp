#include "ChartData.h"
#include "../core/MarketClock.h"
#include <string.h>
#include <math.h>
#include <strings.h>

uint16_t downsampleMinMax(const uint32_t *t, const float *v, uint16_t n, uint32_t *oT, float *oV, uint16_t maxOut) {
  if (n == 0 || maxOut == 0) return 0;
  if (n <= maxOut) { for (uint16_t i = 0; i < n; i++) { oT[i] = t[i]; oV[i] = v[i]; } return n; }
  if (maxOut < 4) { oT[0] = t[0]; oV[0] = v[0]; if (maxOut > 1) { oT[1] = t[n - 1]; oV[1] = v[n - 1]; return 2; } return 1; }
  // First and last are kept verbatim; the middle is split into buckets, each yielding its min and max.
  uint16_t out = 0;
  oT[out] = t[0]; oV[out++] = v[0];
  const uint16_t mid = n - 2, buckets = (maxOut - 2) / 2;
  for (uint16_t b = 0; b < buckets; b++) {
    uint16_t lo = 1 + (uint32_t)mid * b / buckets, hi = 1 + (uint32_t)mid * (b + 1) / buckets;
    if (hi <= lo) continue;
    uint16_t imin = lo, imax = lo;
    for (uint16_t i = lo; i < hi; i++) { if (v[i] < v[imin]) imin = i; if (v[i] > v[imax]) imax = i; }
    uint16_t a = imin < imax ? imin : imax, c = imin < imax ? imax : imin;
    oT[out] = t[a]; oV[out++] = v[a];
    if (c != a) { oT[out] = t[c]; oV[out++] = v[c]; }
  }
  oT[out] = t[n - 1]; oV[out++] = v[n - 1];
  return out;
}

void chartStore(Chart &c, const uint32_t *t, const float *v, uint16_t n, float base, uint32_t okMs, uint32_t okEpoch) {
  c.n = downsampleMinMax(t, v, n, c.t, c.v, c.cap);
  c.base = base;
  c.valid = c.n >= 2;
  c.failed = false;
  c.fromCache = false;
  c.okMs = okMs;
  c.okEpoch = okEpoch;
}

static const char *const NAMES[RANGE_COUNT] = {"1d", "5d", "1mo"};
static const char *const LABELS[RANGE_COUNT] = {"1D", "5D", "1M"};
static const char *const INTERVALS[RANGE_COUNT] = {"5m", "15m", "1d"};

const char *rangeName(uint8_t r) { return r < RANGE_COUNT ? NAMES[r] : NAMES[0]; }
const char *rangeLabel(uint8_t r) { return r < RANGE_COUNT ? LABELS[r] : LABELS[0]; }
const char *rangeInterval(uint8_t r) { return r < RANGE_COUNT ? INTERVALS[r] : INTERVALS[0]; }

bool parseRange(const char *s, uint8_t &r) {
  if (!s) return false;
  for (uint8_t i = 0; i < RANGE_COUNT; i++)
    if (!strcasecmp(s, NAMES[i]) || !strcasecmp(s, LABELS[i])) { r = i; return true; }
  return false;
}

bool pickCloseOnDate(const uint32_t *t, const float *v, uint16_t n, int y, int m, int d, double &price, bool *exact, int32_t fixedOffsetSec) {
  const int64_t target = daysFromCivil(y, m, d);
  int best = -1;                                     // newest valid bar on a day before the target
  for (uint16_t i = 0; i < n; i++) {
    if (!(v[i] > 0)) continue;
    LocalTime lt = toLocal(t[i], fixedOffsetSec == EXCHANGE_NY ? nyOffsetSec(t[i]) : fixedOffsetSec);
    int64_t day = daysFromCivil(lt.y, lt.m, lt.d);
    if (day == target) { price = v[i]; if (exact) *exact = true; return true; }
    if (day < target) best = i;                      // bars are time-ordered
  }
  if (best < 0) return false;                        // date precedes all data: never guess a price
  price = v[best];
  if (exact) *exact = false;
  return true;
}

int niceTicks(double lo, double hi, double *ticks, int maxN, double *stepOut) {
  if (maxN < 1 || !isNum(lo) || !isNum(hi) || hi < lo) return 0;
  if (hi - lo < 1e-9) { ticks[0] = lo; if (stepOut) *stepOut = 0; return 1; }
  // smallest "nice" step (1/2/2.5/5 x 10^k) that yields at most maxN ticks inside [lo, hi]
  static const double F[] = {1, 2, 2.5, 5};
  double mag = pow(10.0, floor(log10((hi - lo) / 40.0)));
  for (int guard = 0; guard < 40; mag *= 10, guard++) {
    for (int k = 0; k < 4; k++) {
      double step = F[k] * mag;
      double first = ceil(lo / step - 1e-9) * step;
      int cnt = first > hi + step * 1e-9 ? 0 : (int)floor((hi - first) / step + 1e-9) + 1;
      if (cnt < 1 || cnt > maxN) continue;
      for (int i = 0; i < cnt; i++) ticks[i] = first + i * step;
      if (stepOut) *stepOut = step;
      return cnt;
    }
  }
  ticks[0] = (lo + hi) / 2; if (stepOut) *stepOut = 0;
  return 1;
}
