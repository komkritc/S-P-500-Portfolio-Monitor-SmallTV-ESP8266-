// ViewSchedule.h - the SmallTV has no buttons, so navigation is automatic. This builds the ordered list of
// "views" the rotation walks through. Pure logic (host-tested).
//
//   Portfolio  ->  Positions (one view per window of 2 rows, so every row is reachable)
//              ->  Realised P/L
//              ->  Market 1D -> Market 5D -> Market 1M   (starting at the default chart range)
//              ->  [Wi-Fi setup/rescue screen, only while an AP is up]
#pragma once
#include <stdint.h>

namespace display {

enum ViewKind : uint8_t { V_PORTFOLIO = 0, V_POSITIONS, V_REALISED, V_MARKET, V_AP };
struct View { ViewKind kind; uint8_t arg; };      // arg: window index (positions) or chart range (market)

static const uint8_t ROWS_PER_WINDOW = 2;     // two 2-line rows fit between the header and the totals at the real font sizes
// MAX_POSITIONS/ROWS_PER_WINDOW position windows (MAX_POSITIONS = 12 -> 6) + portfolio + realised + 3 charts + AP
static const uint8_t MAX_VIEWS = 1 + 6 + 1 + 3 + 1;

// dashboard=false (no saved Wi-Fi, setup AP only) -> just the AP view. apView adds the AP screen to the rotation.
// `pages` = which dashboard pages rotate (Settings::pages): bit0 Portfolio, bit1 Positions, bit2 Market, bit3 Realised.
// The Wi-Fi setup/rescue screen is never filtered by it, so a bad mask cannot lock anyone out of setup.
uint8_t buildViews(View *out, uint8_t cap, uint8_t posCount, uint8_t defaultRange, bool apView, bool dashboard, uint8_t pages);
uint8_t pageDotCount(uint8_t pages);                 // dots shown in the header = enabled pages (>= 1)
uint8_t pageDotIndex(uint8_t pages, uint8_t page);   // position of `page` among the enabled pages
uint8_t pageOf(ViewKind k);                        // 0..2 the three dashboard pages, 3 = realised/AP
uint8_t windowCount(uint8_t posCount);             // >= 1
uint8_t nextIndex(uint8_t cur, uint8_t count);     // wraps; safe when count == 0
}
