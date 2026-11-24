#include "RealisedScreen.h"
#include "Widgets.h"
#include "../core/Format.h"
#include "../core/MarketClock.h"
#include <Arduino_GFX_Library.h>
#include <stdio.h>
#include <string.h>

namespace display {
using namespace ui;

// ISO "2026-11-19" -> "19/11/2026" (the web dashboard shows the same format).
static void dateText(char *out, size_t n, const char *iso) {
  if (!iso || strlen(iso) != 10 || iso[4] != '-' || iso[7] != '-') { snprintf(out, n, "N/A"); return; }
  snprintf(out, n, "%c%c/%c%c/%c%c%c%c", iso[8], iso[9], iso[5], iso[6], iso[0], iso[1], iso[2], iso[3]);
}

// Calendar year of the display date (0 while the clock is not synced).
static int currentYear(const AppState &a) {
  if (!a.clockSynced || !a.nowEpoch) return 0;
  int32_t off = displayOffsetSec(a.nowEpoch, a.settings.tzMode, a.settings.localOffsetMin);
  return toLocal(a.nowEpoch, off).y;
}

static double yearPl(const RealisedSummary &s, int year) {
  if (year <= 0) return NAN;
  for (uint8_t i = 0; i < s.nYears; i++) if (s.year[i] == (uint16_t)year) return s.yearPl[i];
  return 0;                                              // known: no sale fell into this year
}

// Vertical budget (real glyph boxes: size1 = 15 px, size2 = 20 px, size3 = 29 px):
//   0-24 header | 28 label | 43 big (29) | 80-126 this year | 130-170 sales / reversed | 174-237 last sale
void drawRealised(Ui &u, const AppState &a, bool full) {
  Arduino_GFX *g = u.gfx();
  const RealisedSummary &s = a.realised;
  char b[40];
  drawHeader(u, a, "Realised P/L", 3, full);

  // ---- all-time realised P/L
  u.text(1, "ALL-TIME REALISED P/L", 1, 8, 28, A_LEFT, C_MUTED);
  if (!s.valid) {                                       // the ledger could not be read: N/A, never a made-up 0
    u.eraseSlot(2);
    u.text(2, "ledger unreadable", 2, 8, 46, A_LEFT, C_MUTED);
    u.eraseSlot(3); u.eraseSlot(4); u.eraseSlot(5); u.eraseSlot(6);
    u.eraseSlot(7); u.eraseSlot(8); u.eraseSlot(9);
    u.eraseSlot(17); u.eraseSlot(18); u.eraseSlot(19);
    return;
  }
  // A readable ledger with no sales is a known 0 (shown as such), not N/A.
  fmtThbSigned(b, sizeof b, s.pl);
  static const uint8_t bigSz[] = {3, 2, 1};
  u.fit(2, b, bigSz, 3, 224, 8, 43, A_LEFT, signColor(s.pl));

  if (full) {
    g->fillRoundRect(8, 80, 224, 46, 6, C_PANEL);
    g->fillRoundRect(8, 130, 142, 40, 6, C_PANEL);
    g->fillRoundRect(156, 130, 76, 40, 6, C_PANEL);
  }

  // ---- this year's share of it
  const int cy = currentYear(a);
  char l3[24];
  snprintf(l3, sizeof l3, cy ? "THIS YEAR %d" : "THIS YEAR N/A", cy);
  u.text(3, l3, 1, 16, 84, A_LEFT, C_MUTED, C_PANEL);
  const double ypl = yearPl(s, cy);
  fmtThbSigned(b, sizeof b, ypl);
  static const uint8_t ySz[] = {2, 1};
  u.fit(4, b, ySz, 2, 208, 16, 99, A_LEFT, signColor(ypl), C_PANEL);

  // ---- how many sales, how many were undone
  u.text(5, "SALES", 1, 16, 133, A_LEFT, C_MUTED, C_PANEL);
  snprintf(b, sizeof b, "%u", (unsigned)s.count);
  u.text(6, b, 2, 16, 148, A_LEFT, C_TEXT, C_PANEL);
  u.text(7, "REVERSED", 1, 164, 133, A_LEFT, C_MUTED, C_PANEL);
  snprintf(b, sizeof b, "%u", (unsigned)s.reversed);
  u.text(8, b, 2, 164, 148, A_LEFT, s.reversed ? C_WARN : C_TEXT, C_PANEL);

  // ---- the newest sale that still stands (an undo removes it from the summary ring)
  if (full) g->drawFastHLine(0, 172, 240, C_LINE);
  char lab[40];
  snprintf(lab, sizeof lab, "LAST SALE %s", s.hasLast ? s.lastId : "");
  u.text(17, lab, 1, 8, 175, A_LEFT, C_MUTED);
  if (!s.hasLast) {                                     // no sale stands (none yet, or all were undone)
    u.eraseSlot(18); u.eraseSlot(9);
    u.text(19, "no sales yet", 2, 8, 191, A_LEFT, C_MUTED);
    return;
  }
  dateText(b, sizeof b, s.lastDate);
  u.text(18, b, 1, 232, 175, A_RIGHT, C_MUTED);
  static const uint8_t symSz[] = {2, 1}, valSz[] = {2, 1};
  u.fit(19, s.lastSymbol, symSz, 2, 120, 8, 191, A_LEFT, C_TEXT);
  fmtThbSigned(b, sizeof b, s.lastRealised);
  u.fit(9, b, valSz, 2, 108, 232, 191, A_RIGHT, signColor(s.lastRealised));
}

}  // namespace display