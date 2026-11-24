#include "ViewSchedule.h"
#include "../core/AppTypes.h"

namespace display {

uint8_t windowCount(uint8_t n) { return n == 0 ? 1 : (uint8_t)((n + ROWS_PER_WINDOW - 1) / ROWS_PER_WINDOW); }
uint8_t pageOf(ViewKind k) { return k == V_PORTFOLIO ? 0 : k == V_POSITIONS ? 1 : k == V_MARKET ? 2 : 3; }
uint8_t nextIndex(uint8_t cur, uint8_t count) { return count == 0 ? 0 : (uint8_t)((cur + 1) % count); }

uint8_t buildViews(View *out, uint8_t cap, uint8_t posCount, uint8_t defaultRange, bool apView, bool dashboard, bool realised) {
  uint8_t n = 0;
  auto add = [&](ViewKind k, uint8_t a) { if (n < cap) out[n++] = View{k, a}; };
  if (dashboard) {
    add(V_PORTFOLIO, 0);
    uint8_t w = windowCount(posCount);
    for (uint8_t i = 0; i < w; i++) add(V_POSITIONS, i);
    if (realised) add(V_REALISED, 0);
    for (uint8_t i = 0; i < 3; i++) add(V_MARKET, (uint8_t)((defaultRange + i) % 3));
  }
  if (apView || !dashboard) add(V_AP, 0);
  return n;
}

}  // namespace display
