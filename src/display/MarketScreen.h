#pragma once
#include "UiKit.h"
#include "../core/AppState.h"
namespace display {
// chartDirty: repaint the plot (new data / range switch). Everything else is cache-checked per text slot.
void drawMarket(ui::Ui &u, const AppState &a, uint8_t range, bool full, bool chartDirty);
}
