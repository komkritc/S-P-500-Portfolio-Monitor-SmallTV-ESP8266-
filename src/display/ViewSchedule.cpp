#include "ViewSchedule.h"
#include "../core/AppTypes.h"

namespace display {

uint8_t windowCount(uint8_t n) { return n == 0 ? 1 : (uint8_t)((n + ROWS_PER_WINDOW - 1) / ROWS_PER_WINDOW); }
uint8_t pageOf(ViewKind k) { return k == V_PORTFOLIO ? 0 : k == V_POSITIONS ? 1 : k == V_MARKET ? 2 : 3; }
uint8_t nextIndex(uint8_t cur, uint8_t count) { return count == 0 ? 0 : (uint8_t)((cur + 1) % count); }

uint8_t pageDotCount(uint8_t pages) { uint8_t n = 0; for (uint8_t i = 0; i < 4; i++) if (pages & (1u << i)) n++; return n ? n : 1; }
uint8_t pageDotIndex(uint8_t pages, uint8_t page) { uint8_t n = 0; for (uint8_t i = 0; i < page && i < 4; i++) if (pages & (1u << i)) n++; return n; }

uint8_t buildViews(View *out, uint8_t cap, uint8_t posCount, uint8_t defaultRange, bool apView, bool dashboard, uint8_t pages, uint8_t charts) {
  if (!(charts & 7)) charts = 7;                      // an empty mask means "all three ranges"
  if (!(pages & 15)) pages = 15;                      // an empty mask means "all" (never an empty rotation)
  uint8_t n = 0;
  auto add = [&](ViewKind k, uint8_t a) { if (n < cap) out[n++] = View{k, a}; };
  if (dashboard) {
    if (pages & 1) add(V_PORTFOLIO, 0);
    if (pages & 2) { uint8_t w = windowCount(posCount); for (uint8_t i = 0; i < w; i++) add(V_POSITIONS, i); }
    if (pages & 8) add(V_REALISED, 0);
    if (pages & 4) for (uint8_t i = 0; i < 3; i++) { uint8_t r = (uint8_t)((defaultRange + i) % 3); if (charts & (1u << r)) add(V_MARKET, r); }
  }
  if (apView || !dashboard) add(V_AP, 0);
  return n;
}

}  // namespace display
