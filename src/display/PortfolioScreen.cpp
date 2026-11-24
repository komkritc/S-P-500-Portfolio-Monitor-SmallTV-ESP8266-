#include "PortfolioScreen.h"
#include "Widgets.h"
#include "../core/Format.h"
#include <Arduino_GFX_Library.h>
#include <stdio.h>
#include <string.h>

namespace display {
using namespace ui;

// Vertical budget (real glyph boxes: size1 = 15 px, size2 = 20 px, size3 = 29 px incl. descenders):
//   0-24 header | 28 label | 43 NAV (29) | 75-127 P/L panel | 130-170 invested/positions | 174-237 ticker
void drawPortfolio(Ui &u, const AppState &a, bool full) {
  Arduino_GFX *g = u.gfx();
  const PortfolioSummary &s = a.summary;
  char b[40], c[40];
  drawHeader(u, a, "S&P 500 Portfolio", 0, full);

  // ---- NAV: the largest number on the page
  u.text(1, "PORTFOLIO NAV", 1, 8, 28, A_LEFT, C_MUTED);
  if (s.count == 0) { u.eraseSlot(2); u.text(9, "No positions yet", 2, 8, 46, A_LEFT, C_MUTED); }
  else {
    u.eraseSlot(9);
    static const uint8_t navSz[] = {3, 2};
    fmtThb(b, sizeof b, s.nav);                                   // "N/A" when any quote is missing: never a made-up total
    u.fit(2, b, navSz, 2, 224, 8, 43, A_LEFT, isNum(s.nav) ? C_TEXT : C_MUTED);
  }
  if (s.count && !s.complete && s.invalid < s.count) u.text(3, s.priced ? "INCOMPLETE" : "NO QUOTES", 1, 232, 28, A_RIGHT, C_WARN);
  else if (s.stale) u.text(3, "STALE", 1, 232, 28, A_RIGHT, C_WARN);
  else u.eraseSlot(3);

  // ---- panels
  if (full) {
    g->fillRoundRect(8, 75, 224, 52, 6, C_PANEL);
    g->fillRoundRect(8, 130, 142, 40, 6, C_PANEL);
    g->fillRoundRect(156, 130, 76, 40, 6, C_PANEL);
  }
  u.text(4, "Total P/L", 1, 16, 79, A_LEFT, C_MUTED, C_PANEL);
  fmtPct(c, sizeof c, s.retPct);
  u.text(6, c, 2, 224, 77, A_RIGHT, signColor(s.retPct), C_PANEL);
  fmtThbSigned(b, sizeof b, s.pl);
  static const uint8_t plSz[] = {3, 2, 1};
  u.fit(5, b, plSz, 3, 208, 16, 94, A_LEFT, signColor(s.pl), C_PANEL);

  u.text(7, "Invested", 1, 16, 133, A_LEFT, C_MUTED, C_PANEL);
  fmtThb(b, sizeof b, s.invested);
  static const uint8_t invSz[] = {2, 1};
  u.fit(8, b, invSz, 2, 126, 16, 148, A_LEFT, C_TEXT, C_PANEL);
  u.text(17, "Positions", 1, 164, 133, A_LEFT, C_MUTED, C_PANEL);
  snprintf(b, sizeof b, "%u", (unsigned)s.count);
  u.text(18, b, 2, 164, 148, A_LEFT, C_TEXT, C_PANEL);
  if (s.invalid) { snprintf(b, sizeof b, "%u bad", (unsigned)s.invalid); u.text(19, b, 1, 226, 151, A_RIGHT, C_NEG, C_PANEL); }
  else u.eraseSlot(19);

  drawTicker(u, a, TICKER_Y, full);
}

}  // namespace display
