// AppState.h - THE shared application state (see the architecture diagram in the README).
//
//   Yahoo -> MarketData/Fetcher -> raw inputs (positions, book, sp, charts)
//                                    |
//                          appRecompute()   <- the only place totals are computed
//                                    |
//                      derived outputs (results, summary, quote, chartStats)
//                      |                       |
//                SmallTV LCD            Web API / dashboard
//
// Both interfaces only READ the derived fields; neither does any arithmetic on money.
#pragma once
#include "AppTypes.h"
#include "../finance/FinancialCalculator.h"

struct NetInfo {
  enum Mode : uint8_t { NET_BOOT = 0, NET_STA, NET_AP_SETUP, NET_AP_RESCUE };
  Mode    mode;
  bool    connected;            // STA associated and has an IP
  bool    connecting;
  char    ssid[33];             // saved/current network ("" if none)
  char    ip[16];               // STA IP ("" if not connected)
  int8_t  rssi;                 // dBm, 0 if not connected
  char    apSsid[32];           // only while an AP is up
  char    apIp[16];
  char    apPass[12];           // rescue AP key ("" for the open setup AP) - shown on the LCD only
};

struct AppState {
  Settings settings;

  // ---- raw inputs ----
  Position positions[MAX_POSITIONS];
  uint8_t  posCount;
  QuoteBook book;               // last good price per symbol (staleness tracked per symbol)
  SP500Quote sp;                // detailed ^GSPC quote
  Chart    charts[RANGE_COUNT]; // 1D, 5D, 1M. t/v point into the shared pool below
  uint32_t chartT[CHART_POOL];        // one pool sized for the sum of RANGE_CAP (no per-range waste)
  float    chartV[CHART_POOL];

  // effective refresh intervals in seconds (the scheduler may stretch them while the market is closed);
  // staleness = 3 x the effective interval.
  uint16_t effPortfolioS, effQuoteS, effChartS;

  // ---- sale transactions (v1.1) ----
  PortfolioMeta pmeta;         // /portfolio.json header: version, appliedSeq, next ids (confirm tokens)
  RealisedSummary realised;    // aggregates over /sells.jsonl, rebuilt by the boot scan

  // ---- derived outputs (written only by appRecompute) ----
  PositionResult   results[MAX_POSITIONS];
  PortfolioSummary summary;
  QuoteView        quote;
  ChartStats       chartStats[RANGE_COUNT];
  bool             chartStale[RANGE_COUNT];
  uint32_t         nowMs, nowEpoch;     // nowEpoch = 0 while the clock is not synced
  bool             clockSynced;
  uint32_t         portfolioEpoch;      // wall time of the newest successful position-quote refresh (0 = never)

  // ---- diagnostics / network ----
  uint32_t lastMarketOkEpoch;           // last successful Yahoo response of any kind
  char     lastError[48];               // "" when the last request succeeded
  uint32_t lastErrorEpoch;
  NetInfo  net;

  uint32_t rev;                         // bumped on every recompute that changed visible data
};

extern AppState g_app;

void settingsDefaults(Settings &s);
void appInit(AppState &a);
// Recompute every derived field. nowEpoch = 0 when the clock is unsynced.
void appRecompute(AppState &a, uint32_t nowMs, uint32_t nowEpoch);
