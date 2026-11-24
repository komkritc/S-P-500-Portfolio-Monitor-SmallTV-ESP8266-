// ChartRenderer.h - compact index line chart for the 240x240 LCD: y-axis labels, grid, shaded area,
// 2 px line (green when the selected period is up, red when down), x-axis time/date labels.
// Layout is deterministic and pure drawing (host-previewable). The caller decides when to repaint
// (see chartSignature) so the plot is not redrawn on every tick.
#pragma once
#include "UiKit.h"
#include "../core/AppTypes.h"

namespace display {

struct ChartBox { int16_t x, y, w, h; };      // whole widget incl. axis labels

struct ChartParams {
  uint8_t range;                              // ChartRange
  uint8_t tzMode;
  int16_t localOffsetMin;
  bool    stale;
};

void drawChart(ui::Ui &u, const ChartBox &box, const Chart &c, const ChartStats &st, const ChartParams &p);
// Changes whenever the visible chart content changes.
uint32_t chartSignature(const Chart &c, const ChartParams &p);
}
