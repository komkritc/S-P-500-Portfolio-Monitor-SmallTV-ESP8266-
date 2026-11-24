#include "Widgets.h"
#include "../core/Format.h"
#include "../core/MarketClock.h"
#include <Arduino_GFX_Library.h>
#include <stdio.h>
#include <string.h>

namespace display {
using namespace ui;

static int s_lastBars = -99;
static int s_lastDots = -1;
static bool s_lastAp = false;
void resetWidgetCache() { s_lastBars = -99; s_lastDots = -1; s_lastAp = false; }

const char *statusLabel(MarketStatus s) { return s == MS_OPEN ? "OPEN" : s == MS_CLOSED ? "CLOSED" : s == MS_PRE ? "PRE-MARKET" : "N/A"; }
uint16_t statusColor(MarketStatus s) { return s == MS_OPEN ? C_POS : s == MS_PRE ? C_WARN : s == MS_CLOSED ? C_MUTED : C_MUTED; }

int wifiBars(const AppState &a) {
  if (!a.net.connected) return -1;
  int r = a.net.rssi;
  return r >= -55 ? 4 : r >= -65 ? 3 : r >= -75 ? 2 : 1;
}

void drawHeader(Ui &u, const AppState &a, const char *title, uint8_t page, bool full) {
  Arduino_GFX *g = u.gfx();
  if (full) g->drawFastHLine(0, HEADER_H, 240, C_LINE);
  static const uint8_t sizes[] = {2, 1};
  u.fit(SLOT_TITLE, title, sizes, 2, 150, 8, 3, A_LEFT, C_HEAD);

  const bool ap = a.net.mode == NetInfo::NET_AP_SETUP || a.net.mode == NetInfo::NET_AP_RESCUE;
  int bars = ap ? -2 : wifiBars(a);
  if (full || bars != s_lastBars || ap != s_lastAp) {
    g->fillRect(206, 2, 32, 20, C_BG);
    if (ap) u.text(SLOT_AP, "AP", 1, 232, 5, A_RIGHT, C_WARN);
    else { u.eraseSlot(SLOT_AP); wifiIcon(g, 216, 5, bars < 0 ? 0 : bars, bars >= 3 ? C_POS : C_WARN, C_LINE, bars < 0); }
    s_lastBars = bars; s_lastAp = ap;
  }
  if (full || page != s_lastDots) { g->fillRect(150, 6, 60, 12, C_BG); pageDots(g, 180, 12, page == 3 ? 4 : 3, page); s_lastDots = page; }
}

void drawTicker(Ui &u, const AppState &a, int y0, bool full) {
  Arduino_GFX *g = u.gfx();
  const QuoteView &q = a.quote;
  if (full) g->drawFastHLine(0, y0 - 2, 240, C_LINE);
  const uint8_t S = SLOT_TICKER;
  const int yLabel = y0 + 1, yValue = y0 + 17, yDetail = y0 + 48;       // 15 / 29 / 15 px tall -> block ends at y0 + 63
  u.text(S + 0, "S&P 500 (^GSPC)", 1, 8, yLabel, A_LEFT, C_MUTED);
  u.text(S + 1, statusLabel(q.status), 1, 232, yLabel, A_RIGHT, q.valid ? statusColor(q.status) : C_MUTED);

  char b[40];
  if (!q.valid) {
    u.text(S + 3, "N/A", 3, 8, yValue, A_LEFT, C_MUTED);
    u.eraseSlot(S + 2); u.eraseSlot(S + 4);
    u.text(S + 5, a.net.connected ? "waiting for quote" : "offline - no data yet", 1, 8, yDetail, A_LEFT, C_MUTED);
    u.eraseSlot(S + 6);
    return;
  }
  fmtIndex(b, sizeof b, q.last);
  int vw = u.text(S + 3, b, 3, 8, yValue, A_LEFT, C_TEXT);
  fmtPct(b, sizeof b, q.changePct);
  static const uint8_t pctSz[] = {3, 2};
  u.fit(S + 4, b, pctSz, 2, 224 - vw - 8, 232, yValue, A_RIGHT, signColor(q.changePct));

  fmtIndexSigned(b, sizeof b, q.change);
  int cw = u.text(S + 5, b, 1, 8, yDetail, A_LEFT, signColor(q.change));
  char t[40];
  formatQuoteTime(t, sizeof t, q.marketTime, a.settings.tzMode, a.settings.localOffsetMin);
  int tw = Ui::width(t, 1);
  // "STALE" fits in the gap between the change and the timestamp; when it does not, the timestamp itself turns amber
  int gap = (232 - tw) - (8 + cw) - 16;
  bool room = q.stale && gap >= Ui::width("STALE", 1);
  if (room) u.text(S + 2, "STALE", 1, 8 + cw + 8, yDetail, A_LEFT, C_WARN); else u.eraseSlot(S + 2);
  u.text(S + 6, t, 1, 232, yDetail, A_RIGHT, q.stale ? C_WARN : C_MUTED);
}

}  // namespace display
