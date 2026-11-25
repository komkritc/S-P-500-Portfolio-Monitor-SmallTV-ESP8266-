#include "PositionsScreen.h"
#include "Widgets.h"
#include "ViewSchedule.h"
#include "../core/Format.h"
#include <Arduino_GFX_Library.h>
#include <stdio.h>
#include <string.h>

namespace display {
using namespace ui;

// 0-24 header | 2 rows x 43 px from y=27 (line 1: symbol, qty, NAV; line 2: P/L, return) | totals 117/132/147 | ticker 174-237
static const int ROW_Y0 = 27, ROW_H = 43;
static inline uint8_t rs(uint8_t r, uint8_t k) { return (uint8_t)(20 + r * 5 + k); }

void drawPositions(Ui &u, const AppState &a, uint8_t window, bool full) {
  Arduino_GFX *g = u.gfx();
  const PortfolioSummary &s = a.summary;
  char b[48], c[40];
  const uint8_t windows = windowCount(a.posCount);
  if (window >= windows) window = 0;
  const uint8_t first = (uint8_t)(window * ROWS_PER_WINDOW);

  if (a.posCount > ROWS_PER_WINDOW) {
    unsigned last = first + ROWS_PER_WINDOW; if (last > a.posCount) last = a.posCount;
    snprintf(b, sizeof b, "Pos %u-%u/%u", first + 1u, last, (unsigned)a.posCount);   // short: the header clock takes the right side
  } else snprintf(b, sizeof b, "Positions (%u)", (unsigned)a.posCount);
  drawHeader(u, a, b, 1, full);

  for (uint8_t r = 0; r < ROWS_PER_WINDOW; r++) {
    const int y = ROW_Y0 + r * ROW_H;
    const uint8_t i = (uint8_t)(first + r);
    if (i >= a.posCount) {
      for (uint8_t k = 0; k < 5; k++) u.eraseSlot(rs(r, k));
      g->fillCircle(128, y + 9, 3, C_BG);
      if (r == 0) { u.text(1, "No positions", 2, 120, y + 8, A_CENTER, C_MUTED); u.text(2, "Add one on the web page", 1, 120, y + 32, A_CENTER, C_MUTED); }
      continue;
    }
    if (r == 0) { u.eraseSlot(1); u.eraseSlot(2); }
    const Position &p = a.positions[i];
    const PositionResult &x = a.results[i];
    if (full && r) g->drawFastHLine(8, y - 3, 224, C_LINE);

    int symW = u.text(rs(r, 0), p.symbol, 2, 8, y, A_LEFT, C_TEXT);
    if (x.state == Q_INVALID) {
      u.text(rs(r, 2), "invalid data", 2, 232, y, A_RIGHT, C_NEG);
      u.eraseSlot(rs(r, 1)); u.eraseSlot(rs(r, 3)); u.eraseSlot(rs(r, 4));
      g->fillCircle(128, y + 9, 3, C_BG);
      continue;
    }
    // NAV first (it decides how much room the quantity gets)
    fmtNum(b, sizeof b, x.nav, 2, SIGN_NONE);
    static const uint8_t navSz[] = {2, 1};
    uint8_t used = 2;
    int navW = u.fit(rs(r, 2), b, navSz, 2, 232 - (8 + symW + 40), 232, y, A_RIGHT, isNum(x.nav) ? C_TEXT : C_MUTED, C_BG, &used);
    // quantity: only drawn when it fits between the symbol and the NAV (the web table always has it)
    int room = (232 - navW) - (8 + symW + 6) - 14;
    fmtNum(c, sizeof c, p.qty, p.qty < 100 ? 2 : 0, SIGN_NONE);
    snprintf(b, sizeof b, "x%s", c);
    if (Ui::width(b, 1) <= room) u.text(rs(r, 1), b, 1, 8 + symW + 6, y + 4, A_LEFT, C_MUTED);
    else u.eraseSlot(rs(r, 1));
    // stale marker (amber dot) right before the NAV
    g->fillCircle(232 - navW - 8, y + 9, 3, x.state == Q_STALE ? C_WARN : C_BG);
    // line 2: P/L and return, coloured by sign, N/A in grey
    fmtNum(b, sizeof b, x.pl, 2, SIGN_ALWAYS);
    int plW = u.text(rs(r, 3), b, 2, 8, y + 20, A_LEFT, signColor(x.pl));
    fmtPct(b, sizeof b, x.retPct);
    static const uint8_t rSz[] = {2, 1};
    u.fit(rs(r, 4), b, rSz, 2, 224 - plW - 10, 232, y + 20, A_RIGHT, signColor(x.retPct));
  }

  // ---- totals (single engine: the numbers come from the summary)
  if (full) g->drawFastHLine(0, 114, 240, C_LINE);
  u.text(5, "Total NAV", 1, 8, 117, A_LEFT, C_MUTED);
  fmtThb(b, sizeof b, s.nav, 0);   // whole baht, matching the Portfolio page's NAV
  u.text(6, b, 1, 232, 117, A_RIGHT, isNum(s.nav) ? C_TEXT : C_MUTED);
  u.text(7, "Total Invested", 1, 8, 132, A_LEFT, C_MUTED);
  fmtThb(b, sizeof b, s.invested, 0);   // whole baht, matching the Portfolio page's invested figure
  u.text(8, b, 1, 232, 132, A_RIGHT, C_TEXT);
  u.text(9, "Unrealised P/L", 1, 8, 147, A_LEFT, C_MUTED);
  char amt[40]; fmtThbSigned(amt, sizeof amt, s.pl, 0); fmtPct(c, sizeof c, s.retPct);
  snprintf(b, sizeof b, "%s (%s)", amt, c);
  const int maxW = 224 - Ui::width("Unrealised P/L", 1) - 6;
  if (Ui::width(b, 1) > maxW) { fmtNum(amt, sizeof amt, s.pl, 2, SIGN_ALWAYS); snprintf(b, sizeof b, "%s (%s)", amt, c); }   // drop "THB" when tight
  if (Ui::width(b, 1) > maxW) { snprintf(b, sizeof b, "%s", amt); }                                          // then the percentage
  u.text(17, b, 1, 232, 147, A_RIGHT, signColor(s.pl));

  drawTicker(u, a, TICKER_Y, full);
}

}  // namespace display
