// Widgets.h - pieces shared by the three dashboard screens: header (title, page dots, Wi-Fi), S&P 500 ticker.
// Slot map (see UiKit.h; slots are per screen, the cache is reset on every screen change):
//   0 title, 39 header "AP" tag | 1..9, 17..19 screen specific | 10..16 ticker (portfolio/positions) | 20..29 chart | 30..38 screen specific
//   (positions rows use 20..34: that screen has no chart)
#pragma once
#include "UiKit.h"
#include "../core/AppState.h"

namespace display {

static const int HEADER_H = 24;           // header band 0..23, hairline at 24
static const int TICKER_Y = 174;           // ticker block 174..237 (hairline at 172)
static const uint8_t SLOT_TITLE = 0;
static const uint8_t SLOT_AP = 39;
static const uint8_t SLOT_TICKER = 10;    // 10..16

// full=true paints the static chrome (hairline, ...); text/icon updates are always cache-checked.
void drawHeader(ui::Ui &u, const AppState &a, const char *title, uint8_t page /*0..2*/, bool full);
// y0 = top of the ticker block (hairline drawn at y0-2); the block is 64 px tall.
void drawTicker(ui::Ui &u, const AppState &a, int y0, bool full);
int  wifiBars(const AppState &a);          // -1 = no link, 0..4
void resetWidgetCache();                   // after a full clear
const char *statusLabel(MarketStatus s);
uint16_t statusColor(MarketStatus s);
}
