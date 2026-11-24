// RealisedScreen.h - dashboard page 4: realised P/L from the append-only ledger (/sells.jsonl).
// Reads AppState only (realised summary + portfolio meta); all money maths happens in SaleEngine.
#pragma once
#include "UiKit.h"
#include "../core/AppState.h"

namespace display {
void drawRealised(ui::Ui &u, const AppState &a, bool full);
}