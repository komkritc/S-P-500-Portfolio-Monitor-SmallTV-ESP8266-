#include "MarketScreen.h"
#include "Widgets.h"
#include "ChartRenderer.h"
#include "font/lcd_font.h"
#include "../core/Format.h"
#include "../core/MarketClock.h"
#include "../finance/ChartData.h"
#include <Arduino_GFX_Library.h>
#include <stdio.h>
#include <string.h>

namespace display {
using namespace ui;

// 0-24 header | 25 price (29) | 55 change line (15) | 72-90 tabs | 92-185 chart | 187 hairline | 190/211 value rows
static const int TAB_Y = 72, TAB_H = 18;
static const ChartBox CHART_BOX = {2, 92, 236, 94};
static int8_t s_lastRange = -1;

static void drawTabs(Ui &u, uint8_t range, bool full) {
  if (!full && s_lastRange == (int8_t)range) return;
  s_lastRange = (int8_t)range;
  Arduino_GFX *g = u.gfx();
  for (uint8_t i = 0; i < 3; i++) {
    const int x = 8 + i * 75;
    const bool on = i == range;
    g->fillRoundRect(x, TAB_Y, 71, TAB_H, 5, on ? C_HEAD : C_PANEL);
    const char *l = rangeLabel(i);
    lcdSmoothText(g, x + (71 - lcdSmoothW(l, 1)) / 2, TAB_Y + 2, l, 1, on ? C_BG : C_MUTED, on ? C_HEAD : C_PANEL);
  }
}

void drawMarket(Ui &u, const AppState &a, uint8_t range, bool full, bool chartDirty) {
  Arduino_GFX *g = u.gfx();
  const QuoteView &q = a.quote;
  const Chart &ch = a.charts[range];
  const ChartStats &st = a.chartStats[range];
  char b[48], c[40];
  if (full) s_lastRange = -1;
  drawHeader(u, a, "Market", 2, full);   // was "S&P 500 (^GSPC)": the ticker below already names the index, and the header clock needs the room

  // ---- current quote: independent of the selected chart range
  if (!q.valid) {
    u.text(1, "N/A", 3, 8, 25, A_LEFT, C_MUTED);
    u.text(2, a.net.connected ? "waiting for quote" : "offline - no data yet", 1, 8, 55, A_LEFT, C_MUTED);
    u.eraseSlot(3); u.eraseSlot(4); u.eraseSlot(9);
  } else {
    fmtIndex(b, sizeof b, q.last);
    u.text(1, b, 3, 8, 25, A_LEFT, C_TEXT);
    u.text(3, statusLabel(q.status), 1, 232, 28, A_RIGHT, statusColor(q.status));
    fmtIndexSigned(b, sizeof b, q.change);
    fmtPct(c, sizeof c, q.changePct);
    char both[64]; snprintf(both, sizeof both, "%s (%s)", b, c);
    int cw = u.text(2, both, 1, 8, 55, A_LEFT, signColor(q.change));
    // freshness of the quote: amber "STALE hh:mm" when the last refresh failed / is old
    char t[16];
    if (q.fetchedEpoch) {
      formatClock(t, sizeof t, q.fetchedEpoch, a.settings.tzMode, a.settings.localOffsetMin);
      snprintf(b, sizeof b, q.stale ? "STALE %s" : "upd %s", t);
    } else snprintf(b, sizeof b, q.stale ? "STALE" : "cached");
    if (8 + cw + 8 + Ui::width(b, 1) <= 232) u.text(4, b, 1, 232, 55, A_RIGHT, q.stale ? C_WARN : C_MUTED); else u.eraseSlot(4);
    u.eraseSlot(9);
  }

  drawTabs(u, range, full);

  // ---- chart: repainted only when its data / range changed
  ChartParams cp = {range, a.settings.tzMode, a.settings.localOffsetMin, a.chartStale[range]};
  if (full || chartDirty) drawChart(u, CHART_BOX, ch, st, cp);

  // ---- lower section: Open/High/Low/Prev for 1D and 5D; selected-period performance for 1M
  if (full) g->drawFastHLine(0, 187, 240, C_LINE);
  if (range == RANGE_1M) {
    for (uint8_t k = 5; k <= 8; k++) u.eraseSlot(k);
    for (uint8_t k = 13; k <= 16; k++) u.eraseSlot(k);
    u.text(5, "1M Change", 2, 8, 190, A_LEFT, C_MUTED);
    if (st.valid) {
      fmtIndexSigned(b, sizeof b, st.change);
      u.text(11, b, 2, 232, 190, A_RIGHT, signColor(st.change));
      fmtPct(c, sizeof c, st.changePct);
      u.text(12, c, 2, 232, 211, A_RIGHT, signColor(st.changePct));
    } else { u.text(11, "N/A", 2, 232, 190, A_RIGHT, C_MUTED); u.eraseSlot(12); }
    return;
  }
  u.eraseSlot(12);
  struct { const char *l; double v; } f[4] = {{"Open", q.open}, {"High", q.high}, {"Low", q.low}, {"Prev", q.prevClose}};
  static const uint8_t vSz[] = {2, 1};
  for (int i = 0; i < 4; i++) {
    const int row = i / 2, col = i % 2, y = 190 + row * 21;
    const int lx = col ? 122 : 6, rx = col ? 234 : 114;
    const int lw = u.text(5 + i, f[i].l, 1, lx, y + 3, A_LEFT, C_MUTED);
    fmtIndex(b, sizeof b, f[i].v);
    u.fit(13 + i, b, vSz, 2, (rx - lx) - lw - 4, rx, y, A_RIGHT, isNum(f[i].v) ? C_TEXT : C_MUTED);
  }
}

}  // namespace display
