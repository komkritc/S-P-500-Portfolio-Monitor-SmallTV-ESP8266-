// MarketData.h - refresh scheduler between Yahoo Finance and the shared AppState.
//
// One Yahoo request at a time (ESP8266 heap). Each cached item (^GSPC quote, every portfolio symbol,
// 1D/5D/1M charts) has its own due time and failure backoff. A failed refresh NEVER overwrites the last
// good value: it only sets `failed`, which the calculator turns into "stale".
//
// Request budget (defaults): quote 1/min, charts 3 x 1/5 min while the market is open. Once the US market
// is closed and the cached data already contains the final close, ^GSPC quotes drop to >= 15 min and
// charts to >= 60 min (reported in /api/status via the effective intervals); they resume at the next open.
#pragma once
#include <Arduino.h>
#include "../core/AppState.h"

namespace market {

enum Refresh : uint8_t { REF_ALL = 0, REF_QUOTE, REF_CHARTS, REF_PORTFOLIO };

enum JobState : uint8_t { JOB_IDLE = 0, JOB_PENDING, JOB_OK, JOB_ERROR };
struct AddJob { JobState state; char error[48]; char symbol[SYMBOL_LEN]; bool updated; };
// Historical close of `date` for `symbol`, fetched when a sell is previewed without an execution price.
struct SalePriceJob { JobState state; char error[48]; char symbol[SYMBOL_LEN]; char date[11]; double price; };

void begin();
void loop();                                   // call every loop(); never blocks beyond the TLS handshake
void requestRefresh(Refresh what);
void positionsChanged();                       // after add/delete: resync quote book and refresh new symbols
void setViewRange(uint8_t range);              // chart the user is looking at is refreshed first
bool requestAddByThb(const char *symbol, const char *date, double thb);   // false if a job is pending
const AddJob &addJob();
void consumeAddJob();                          // web UI has read the final state -> back to idle
// Fills SalePriceJob::price with the close on `date`. False when another price job is running.
bool requestSalePrice(const char *symbol, const char *date);
const SalePriceJob &salePriceJob();
void consumeSalePriceJob();
void clearCache();                             // drop quotes/charts (RAM + flash) and refetch
void flushCache();                             // write cache.bin now if dirty (before restart)
bool busy();                                   // a Yahoo request is in flight (OTA must wait)
void abortRequests();
}
