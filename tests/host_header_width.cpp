// host_header_width.cpp - LCD header geometry check with the real font metrics.
//
// The header now draws, left to right: page title, then a clock "Nov 25, 22:12" right-aligned at
// x=202, then the Wi-Fi icon box that starts at x=206. Overlap is prevented by construction
// (the title is sized to fit the space left of the clock), EXCEPT when even the smallest title
// size does not fit - Ui::fit() then draws it anyway and it would run under the clock. This test
// measures the real pixel widths so that exception can be ruled out for every title the firmware
// can produce, including the worst positions-window label.
//
// Build/run (no device needed):
//   g++ -std=c++17 -I /tmp/ardstub -I src/display/font tests/host_header_width.cpp \
//       src/display/font/lcd_font.cpp src/display/font/lcd_fonts_data.cpp -o /tmp/hw && /tmp/hw
#include "lcd_font.h"
#include <cstdio>
#include <cstring>

// Must stay in sync with Widgets.cpp drawHeader().
static const int DT_RIGHT = 202;   // right edge of the clock
static const int GAP = 14;         // minimum clear space between title and clock
static const int TITLE_X = 8;
static const int WIFI_X = 206;     // Wi-Fi icon box starts here

static const char *const TITLES[] = {
  "S&P 500",                // portfolio page (was "S&P 500 Portfolio")
  "Market",                 // market page (was "S&P 500 (^GSPC)")
  "Realised P/L",           // realised page
  "Positions (12)",         // positions, everything fits one window
  "Positions (1)",
  "Pos 1-5/12",             // positions, paginating (widest real variants, MAX_POSITIONS = 12)
  "Pos 9-12/12",
  "Pos 10-12/12",
  "no clock",               // header clock before NTP answers
  "Nov 25, 22:12",          // the clock itself
  "Sep 30, 23:59",          // widest month/day + time variant
};

int main() {
  int fails = 0, checks = 0;
  const int clockW = lcdSmoothW("Nov 25, 22:12", 1);
  const int clockWWide = lcdSmoothW("Sep 30, 23:59", 1);
  const int maxTitleW = DT_RIGHT - (clockW > clockWWide ? clockW : clockWWide) - GAP;

  printf("clock  \"Nov 25, 22:12\" size1 = %3d px   \"Sep 30, 23:59\" = %3d px\n", clockW, clockWWide);
  printf("space left for a title  = x %d..%d  (%3d px)\n", TITLE_X, TITLE_X + maxTitleW, maxTitleW);
  printf("title size2 lineH %d  size1 lineH %d  clock baseline y = 3 + (%d-%d)/2 = %d\n",
         lcdSmoothLineH(2), lcdSmoothLineH(1), lcdSmoothLineH(2), lcdSmoothLineH(1),
         3 + (lcdSmoothLineH(2) - lcdSmoothLineH(1)) / 2);

  for (const char *t : TITLES) {
    const int w2 = lcdSmoothW(t, 2), w1 = lcdSmoothW(t, 1);
    const int pick = w2 <= maxTitleW ? 2 : 1;      // Ui::fit(): biggest that fits, else the smallest
    const int w = pick == 2 ? w2 : w1;
    const int clear = DT_RIGHT - (TITLE_X + w) - (clockW > clockWWide ? clockW : clockWWide);
    ++checks;
    if (w > maxTitleW) { printf("  OVERFLOW  \"%s\" size%d = %d px > %d px\n", t, pick, w, maxTitleW); ++fails; }
    else printf("  ok        \"%-16s\" size%d %3d px  clear space to clock %3d px\n", t, pick, w, clear);
    if (DT_RIGHT >= WIFI_X) { printf("  clock right edge %d must stay left of the Wi-Fi box at %d\n", DT_RIGHT, WIFI_X); ++fails; }
  }
  printf("%s %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
  return fails ? 1 : 0;
}
