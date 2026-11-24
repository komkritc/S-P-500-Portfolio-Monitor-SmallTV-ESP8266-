// YahooParser.h - extracts what we need from a Yahoo `v8/finance/chart` JSON body, fed in
// small chunks straight from the socket (no body buffer, no DOM).
//   quote request : ?range=1d&interval=1d   -> meta.* + open/high/low of the session
//   chart request : ?range=1d|5d|1mo&interval=5m|15m|1d -> timestamp[] + close[]
#pragma once
#include "../core/AppTypes.h"
#include "JsonStream.h"

class YahooParser {
 public:
  // tsBuf/closeBuf (capacity `cap`) receive timestamp[] and close[]; either may be null if the
  // caller does not need the series.
  void begin(uint32_t *tsBuf, float *closeBuf, uint16_t cap);
  bool feed(const char *data, size_t len) { return js_.feed(data, len); }
  // true when the body was complete valid JSON containing a usable result. On false, error() says why.
  bool finish();
  const char *error() const { return err_; }

  // meta (NaN / 0 when absent)
  double price, prevClose, dayHigh, dayLow, open;
  uint32_t marketTime;
  int32_t gmtOffset;
  char tz[8];
  char symbol[SYMBOL_LEN];
  char tzName[28];            // exchangeTimezoneName, e.g. "America/New_York"
  // series: compacted (null closes and non-monotonic timestamps removed) by finish()
  uint16_t count;

 private:
  JsonStream js_;
  uint32_t *ts_ = nullptr;
  float *cl_ = nullptr;
  uint16_t cap_ = 0, tsN_ = 0, clN_ = 0;
  bool overflow_ = false, resultNull_ = false, haveResult_ = false, haveOpen_ = false;
  double prevClose2_ = NAN;
  char errCode_[24];
  char err_[40];

  static void onEvent(void *ctx, const char *path, int idx, JsonKind k, const char *text);
  void handle(const char *path, int idx, JsonKind k, const char *text);
};
