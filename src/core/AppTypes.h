// AppTypes.h - shared data model. Pure C++ (no Arduino dependency) so the finance
// engine can be unit-tested on a PC. Fixed-size arrays only: no String, no heap.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>

#define FW_VERSION "1.0.0"
#define SP500_SYMBOL "^GSPC"

static const uint8_t  MAX_POSITIONS = 24;   // same cap as the original sketch
static const uint8_t  MAX_SYMBOLS   = 8;    // distinct symbols quoted (one TLS request each)
static const uint8_t  SYMBOL_LEN    = 16;   // incl. NUL
static const uint16_t STAGE_CAP     = 192;  // parser staging capacity (points)

enum ChartRange : uint8_t { RANGE_1D = 0, RANGE_5D = 1, RANGE_1M = 2, RANGE_COUNT = 3 };
static const uint16_t RANGE_CAP[RANGE_COUNT] = {96, 144, 40};   // stored points per range
static const uint16_t CHART_POOL = 96 + 144 + 40;

enum MarketStatus : uint8_t { MS_UNKNOWN = 0, MS_OPEN, MS_CLOSED, MS_PRE };
enum QuoteState   : uint8_t { Q_FRESH = 0, Q_STALE, Q_MISSING, Q_INVALID };

// One holding. Same fields as the original sketch's `Pos` (symbol, qty, buy, date).
struct Position {
  char   symbol[SYMBOL_LEN];
  char   date[11];            // YYYY-MM-DD
  double qty;
  double buy;                 // buy price per unit, THB
};

// Last good price for one symbol. Never overwritten by a failed fetch.
struct SymQuote {
  char     symbol[SYMBOL_LEN];
  double   price;
  uint32_t marketTime;        // Unix time of the price (from Yahoo)
  uint32_t okMs;              // millis() of last successful refresh
  uint32_t okEpoch;           // wall clock of last success (0 if clock unsynced)
  bool     valid;
  bool     failed;            // latest refresh attempt failed
  bool     fromCache;         // loaded from flash, not yet refreshed this boot
};

struct PositionResult {
  double     price;           // NaN = unavailable
  double     cost;
  double     nav;
  double     pl;
  double     retPct;
  uint32_t   quoteEpoch;
  QuoteState state;
};

struct PortfolioSummary {
  double  invested;           // sum of cost of valid positions
  double  nav;                // NaN unless complete
  double  pl;                 // NaN unless complete
  double  retPct;             // NaN unless complete and invested > 0
  uint8_t count;              // positions in the portfolio
  uint8_t priced;             // valid positions that have a quote
  uint8_t invalid;            // positions with bad qty/buy (excluded)
  bool    complete;           // every valid position has a quote
  bool    stale;              // some priced position is stale
};

// Detailed S&P 500 quote (^GSPC). Derived fields come from QuoteView.
struct SP500Quote {
  bool     valid;
  bool     failed;
  bool     fromCache;
  double   last, prevClose, open, high, low;   // NaN = unknown
  uint32_t marketTime;        // Unix time of the last trade
  int32_t  gmtOffset;         // exchange offset reported by Yahoo (seconds)
  char     tz[8];             // "EST"/"EDT"
  uint32_t okMs;
  uint32_t okEpoch;
};

struct QuoteView {
  bool         valid;
  bool         stale;
  double       last, prevClose, change, changePct, open, high, low;
  MarketStatus status;
  char         sessionDate[11];     // exchange-local date of the quote, "" if unknown
  char         tz[8];               // zone abbreviation of the quote time
  uint32_t     marketTime;
  uint32_t     fetchedEpoch;
};

struct Chart {
  bool     valid;
  bool     failed;
  bool     fromCache;
  uint16_t n;
  uint16_t cap;
  uint32_t* t;
  float*    v;
  float     base;             // reference for change: prev close (1D) or NaN = first point
  uint32_t  okMs;
  uint32_t  okEpoch;
};

struct ChartStats {
  bool   valid;
  double first, last, base, change, changePct, minV, maxV;
};

struct Settings {
  uint16_t portfolioRefreshS;
  uint16_t quoteRefreshS;
  uint16_t chartRefreshS;
  uint16_t pageRotateS;
  uint8_t  defaultRange;      // ChartRange
  uint8_t  tzMode;            // 0 = exchange (America/New_York), 1 = fixed local offset
  int16_t  localOffsetMin;
  char     hostname[24];
  char     adminPass[33];
};

static inline bool isNum(double v) { return !isnan(v) && !isinf(v); }
