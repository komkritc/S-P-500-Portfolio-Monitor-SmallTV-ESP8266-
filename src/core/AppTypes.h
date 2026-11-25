// AppTypes.h - shared data model. Pure C++ (no Arduino dependency) so the finance
// engine can be unit-tested on a PC. Fixed-size arrays only: no String, no heap.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>

#define FW_VERSION "1.1.6"
#define SP500_SYMBOL "^GSPC"

static const uint8_t  MAX_POSITIONS = 12;   // open lots; trimmed from 24 to fit the sale ledger in RAM
static const uint8_t  MAX_SYMBOLS   = 8;    // distinct symbols quoted (one TLS request each)
static const uint8_t  SYMBOL_LEN    = 16;   // incl. NUL
static const uint16_t STAGE_CAP     = 144;  // parser staging capacity (points): holds the biggest raw fetch before downsampling

// Sale ledger (/sells.jsonl): append-only, one JSON object per line.
static const uint16_t LEDGER_LIMIT      = 2000;  // hard cap on records
static const uint16_t LEDGER_WARN_AT    = 1800;  // 90% of the cap -> warn
static const uint8_t  MAX_LOT_ALLOCS    = 12;    // a sale can touch every lot of one symbol (<= MAX_POSITIONS)
static const uint16_t SALES_PAGE_MAX    = 8;     // records returned by one /api/sales page (keeps SalePage small)
static const uint16_t LEDGER_LINE_MAX   = 1536;  // largest single record line (JSON, no '\n'): 12 allocs fit
static const uint8_t  YEAR_BUCKETS      = 4;     // "this year" + previous years kept for the LCD/web summary

enum ChartRange : uint8_t { RANGE_1D = 0, RANGE_5D = 1, RANGE_1M = 2, RANGE_COUNT = 3 };
static const uint16_t RANGE_CAP[RANGE_COUNT] = {80, 112, 24};   // stored points per range
static const uint16_t CHART_POOL = 80 + 112 + 24;

enum MarketStatus : uint8_t { MS_UNKNOWN = 0, MS_OPEN, MS_CLOSED, MS_PRE };
enum QuoteState   : uint8_t { Q_FRESH = 0, Q_STALE, Q_MISSING, Q_INVALID };

// One open lot. v1.0 stored (symbol, qty, buy, date); v1.1 adds `id` (unique, never reused) and
// `oqty` (quantity at purchase) so every open position can be rebuilt from the original lot plus
// the sale ledger. Files written by v1.1 still load in v1.0 (unknown keys are ignored there).
struct Position {
  uint16_t id;               // lot id, 0 = legacy row (assigned on load)
  char   symbol[SYMBOL_LEN];
  char   date[11];            // YYYY-MM-DD
  double qty;                 // open (remaining) quantity
  double oqty;                // original quantity at purchase (cost-basis baseline)
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
  double  invested;           // sum of cost of valid positions (open lots)
  double  nav;                // NaN unless complete
  double  pl;                 // unrealised, NaN unless complete  (labelled "Unrealised P/L" in v1.1)
  double  retPct;             // NaN unless complete and invested > 0
  double  realisedPl;         // all-time realised P/L from the ledger, NaN when unknown
  double  lifetimePl;         // unrealised + realised (open + realised), NaN when either is unknown
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

// LCD backlight bounds (percent). 5 % is the floor, not 0: the SmallTV's ST7789
// backlight is driven active-low from GPIO 5, so the panel can never be dimmed
// past the point where only the web UI could bring it back, and the bottom of
// the slider always changes something.
constexpr int32_t BL_MIN_PCT = 5, BL_MAX_PCT = 100;

struct Settings {
  uint16_t portfolioRefreshS;
  uint16_t quoteRefreshS;
  uint16_t chartRefreshS;
  uint16_t pageRotateS;
  uint8_t  backlight;         // LCD backlight percent (BL_MIN_PCT..BL_MAX_PCT, see above)
  uint8_t  defaultRange;      // ChartRange
  uint8_t  charts;            // chart ranges the LCD Market page cycles through: bit0 1D, bit1 5D, bit2 1M (default 7)
  uint8_t  pages;             // LCD pages in the rotation: bit0 Portfolio, bit1 Positions, bit2 Market, bit3 Realised P/L (default 15)
  uint8_t  tzMode;            // 0 = exchange (America/New_York), 1 = fixed local offset
  int16_t  localOffsetMin;
  char     hostname[24];
  char     adminPass[33];
};

// ---------------------------------------------------------------- v1.1 sale transactions
enum SaleMethod   : uint8_t { SALE_FIFO = 0, SALE_SPECIFIC = 1 };
enum SaleMode     : uint8_t { SALE_BY_THB = 0, SALE_BY_QTY = 1, SALE_BY_ALL = 2 };
enum PriceSource  : uint8_t { PRICE_MANUAL = 0, PRICE_YAHOO = 1 };
enum SaleRecType  : uint8_t { REC_SALE = 0, REC_REVERSAL = 1 };

// One lot touched by a sale (or by the reversal of one). A sale is for a single symbol, so the
// symbol is stored once on the record. `before`/`after` are the lot's open quantity immediately
// before/after the record: applying a record sets qty = after, which is idempotent.
struct LotAlloc {
  uint16_t lot;                // Position::id
  char     date[11];           // lot purchase date (re-creates a lot that was fully sold)
  double   buy;
  double   oqty;
  double   before;
  double   after;
};

// A single line of /sells.jsonl. The ledger is append-only: nothing is ever edited or deleted,
// an undo is a new REC_REVERSAL record that references the original id/seq.
struct SaleRecord {
  uint32_t seq;                // 1.. monotonic within the ledger
  uint32_t ts;                 // commit time (Unix seconds)
  uint32_t refSeq;             // reversal: seq of the original sale (0 for a sale)
  char     id[16];             // "TX-000123"
  char     refId[16];          // reversal: id of the original sale ("" for a sale)
  char     symbol[SYMBOL_LEN];
  char     date[11];           // sale date (a reversal keeps the original sale's date)
  uint8_t  type;               // SaleRecType
  uint8_t  priceSource;        // PriceSource
  double   qty;                // total quantity sold
  double   price;              // execution price
  double   refPrice;           // Yahoo historical close for `date` (NaN = not fetched)
  double   fee;
  double   gross, net, cost, realised;
  uint8_t  nAlloc;
  LotAlloc alloc[MAX_LOT_ALLOCS];
};

// History row: a record without its lot allocations (what /api/sales pages and CSV rows need).
struct SaleLite {
  uint32_t seq, ts;
  char     id[16], refId[16], symbol[SYMBOL_LEN], date[11];
  uint8_t  type, priceSource, status;   // status: REC_SALE (valid) / REC_REVERSAL / 2 = sale already reversed
  double   qty, price, refPrice, fee, gross, net, cost, realised;
};
static const uint8_t SALE_ST_REVERSED = 2;

// Inputs of one sell operation, exactly as the user entered them.
struct SaleRequest {
  char   symbol[SYMBOL_LEN];
  char   date[11];
  uint8_t method;              // SaleMethod
  uint8_t mode;                // SaleMode
  uint16_t lotId;              // SALE_SPECIFIC only
  double amount;               // SALE_BY_THB
  double qty;                  // SALE_BY_QTY
  double price;                // execution price (NaN = must be resolved first)
  double refPrice;             // Yahoo close for `date` if it was fetched (NaN = unknown)
  double fee;
  uint8_t priceSource;
  uint32_t token;              // 0 = preview, otherwise must equal previewToken(...)
};

// Result of the preview step (Input -> Calculate Preview -> Confirm -> Commit).
struct SalePreview {
  bool   ok;
  char   error[72];
  char   symbol[SYMBOL_LEN];
  char   date[11];
  double price, refPrice;
  uint8_t priceSource;
  double fee;
  double qty;                  // quantity actually taken from the lots
  double available;            // open quantity before the sale
  double gross, net, cost, realised;
  uint8_t nAlloc;
  LotAlloc alloc[MAX_LOT_ALLOCS];
  uint32_t token;              // echoed by POST /api/sales/confirm
};

// Aggregates over the whole ledger (kept in RAM, rebuilt by one streaming scan at boot).
struct RealisedSummary {
  bool    valid;               // the ledger was read successfully
  double  pl;                  // all-time realised P/L of valid (non-reversed) sales
  uint16_t count;              // valid sales
  uint16_t reversed;           // sales that carry a reversal
  uint16_t records;            // all records (sales + reversals)
  bool    hasLast;
  char    lastId[16], lastSymbol[SYMBOL_LEN], lastDate[11];
  double  lastRealised;
  uint32_t lastTs;
  // Newest valid sales, oldest first. An undo removes its entry, so "last sale" survives a
  // reversal of the newest sale without re-reading the ledger.
  uint8_t  nLast;
  SaleLite lastRing[2];
  uint8_t  nYears;
  uint16_t year[YEAR_BUCKETS];   // newest years kept
  double   yearPl[YEAR_BUCKETS];
  bool    warn;                // >= LEDGER_WARN_AT records
  bool    full;                // == LEDGER_LIMIT
  bool    recovery;            // records were re-applied at boot
  bool    badLine;             // at least one unparseable line was skipped
};

// /portfolio.json header. `ver` changes on every lot mutation (part of the confirm token),
// `appliedSeq` is the highest ledger seq already reflected in the open positions.
struct PortfolioMeta {
  uint32_t ver;
  uint32_t appliedSeq;
  uint16_t nextLotId;
  uint32_t nextTxn;            // next transaction number ("TX-%06u")
};

static inline bool isNum(double v) { return !isnan(v) && !isinf(v); }

// String literals: on ESP8266 a plain "literal" is copied into RAM (.rodata is in DRAM), which this
// memory-tight build cannot afford. S_LIT() puts them in flash (PSTR) instead; normal string functions
// read a flash `const char*` fine because the ESP8266 flash is memory-mapped. On the host it is a no-op.
#ifdef ARDUINO
#include <pgmspace.h>
#define S_LIT(x) PSTR(x)
#else
#define S_LIT(x) (x)
#endif
