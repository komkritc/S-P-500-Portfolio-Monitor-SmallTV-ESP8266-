#include "ChartRenderer.h"
#include "../core/MarketClock.h"
#include "../core/Format.h"
#include "../finance/ChartData.h"
#include <Arduino_GFX_Library.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

namespace display {
using namespace ui;

static uint16_t blend(uint16_t a, uint16_t b, uint8_t t /*0..255 of b*/) {
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31, br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  return (uint16_t)(((ar + (br - ar) * t / 255) << 11) | ((ag + (bg - ag) * t / 255) << 5) | (ab + (bb - ab) * t / 255));
}

uint32_t chartSignature(const Chart &c, const ChartParams &p) {
  uint32_t s = c.valid ? 1u : 0u;
  s = s * 31u + c.n; s = s * 31u + c.okEpoch; s = s * 31u + p.range; s = s * 31u + p.tzMode + (p.stale ? 7u : 0u) + (uint16_t)p.localOffsetMin;
  if (c.valid && c.n) { s = s * 31u + c.t[c.n - 1]; s = s * 31u + (uint32_t)(c.v[c.n - 1] * 100.0f); }
  return s;
}

namespace {
struct Plot { int x0, y0, w, h; double lo, hi; };

inline int yOf(const Plot &p, double v) {
  double f = (v - p.lo) / (p.hi - p.lo);
  int y = p.y0 + p.h - 1 - (int)(f * (p.h - 1) + 0.5);
  return y < p.y0 ? p.y0 : (y > p.y0 + p.h - 1 ? p.y0 + p.h - 1 : y);
}

// x position of point i. 1D is laid out on the session clock (09:30 to the close, early closes included) so a
// chart in progress grows from the left; 5D/1M use the point index, which collapses nights/weekends/holidays
// without inventing any data.
struct XMap {
  bool timeBased; uint32_t t0, span; uint16_t n; int x0, w;
  int at(const Chart &c, uint16_t i) const {
    if (timeBased) { uint64_t d = c.t[i] > t0 ? c.t[i] - t0 : 0; if (d > span) d = span; return x0 + (int)(d * (uint64_t)(w - 1) / span); }
    return x0 + (n > 1 ? (int)((uint32_t)i * (uint32_t)(w - 1) / (n - 1)) : 0);
  }
};

void xLabels(Ui &u, const Plot &pl, const Chart &c, const XMap &xm, const ChartParams &p, int labelY, uint8_t &slot) {
  char b[16];
  int lastEnd = -100;
  auto put = [&](const char *s, int cx) {
    int w = Ui::width(s, 1);
    int x = cx - w / 2;
    if (x < pl.x0 - 4) x = pl.x0 - 4;
    if (x + w > 238) x = 238 - w;
    if (x < lastEnd + 6) return;
    u.text(slot++, s, 1, x, labelY, A_LEFT, C_MUTED);
    lastEnd = x + w;
  };
  if (p.range == RANGE_1D) {
    // whole ET hours inside the session: 10:00, 12:00, 14:00, 16:00 (5 steps of 2 h look right at 186 px)
    LocalTime lt = toLocal(c.t[0], nyOffsetSec(c.t[0]));
    uint32_t open = sessionOpenUtc(lt.y, lt.m, lt.d);
    for (int h = 10; h <= 16; h += 2) {
      uint32_t t = open + (uint32_t)((h * 60 - 570) * 60);
      if (t > xm.t0 + xm.span) continue;
      formatClock(b, sizeof b, t, p.tzMode, p.localOffsetMin);
      put(b, xm.x0 + (int)((uint64_t)(t - xm.t0) * (xm.w - 1) / xm.span));
    }
  } else if (p.range == RANGE_5D) {
    int prevDay = -1;
    for (uint16_t i = 0; i < c.n; i++) {
      LocalTime lt = toLocal(c.t[i], displayOffsetSec(c.t[i], p.tzMode, p.localOffsetMin));
      int day = lt.y * 400 + lt.m * 32 + lt.d;
      if (day != prevDay) { prevDay = day; formatMonthDay(b, sizeof b, c.t[i], p.tzMode, p.localOffsetMin); put(b, xm.at(c, i)); }
    }
  } else {
    // 1M: four evenly spaced bars
    for (int k = 0; k < 4; k++) {
      uint16_t i = (uint16_t)((uint32_t)(c.n - 1) * k / 3);
      formatMonthDay(b, sizeof b, c.t[i], p.tzMode, p.localOffsetMin);
      put(b, xm.at(c, i));
    }
  }
}
}  // namespace

void drawChart(Ui &u, const ChartBox &box, const Chart &c, const ChartStats &st, const ChartParams &p) {
  Arduino_GFX *g = u.gfx();
  const int labelW = 38;                         // y-axis label column
  Plot pl;
  pl.x0 = box.x + labelW; pl.y0 = box.y + 2; pl.w = box.w - labelW - 2; pl.h = box.h - 2 - 15;
  uint8_t slot = 20;                              // chart owns slots 20..29 (<= 4 y labels + <= 5 x labels + 1 message)
  for (uint8_t i = 20; i < 30; i++) u.eraseSlot(i);
  g->fillRect(box.x, box.y, box.w, box.h, C_BG);

  if (!c.valid || c.n < 2 || !st.valid) {
    g->drawRect(pl.x0, pl.y0, pl.w, pl.h, C_LINE);
    u.text(slot++, c.failed ? "Chart unavailable" : "Loading chart...", 2, pl.x0 + pl.w / 2, pl.y0 + pl.h / 2 - 9, A_CENTER, C_MUTED);
    return;
  }

  // scale: pad 8 %; keep the previous close inside the view for 1D so the "vs prev close" picture is honest
  double lo = st.minV, hi = st.maxV;
  if (p.range == RANGE_1D && isNum(st.base)) { if (st.base < lo) lo = st.base; if (st.base > hi) hi = st.base; }
  double pad = (hi - lo) * 0.08;
  if (pad < 0.01) pad = hi * 0.001 + 0.5;
  pl.lo = lo - pad; pl.hi = hi + pad;

  const uint16_t col = st.change >= 0 ? C_POS : C_NEG;
  const uint16_t shade = blend(C_BG, col, 46);

  // y grid + labels
  double tk[6]; int maxT = pl.h / 22; if (maxT < 2) maxT = 2; if (maxT > 4) maxT = 4;
  int nt = niceTicks(pl.lo, pl.hi, tk, maxT);
  for (int i = 0; i < nt; i++) {
    int y = yOf(pl, tk[i]);
    for (int x = pl.x0; x < pl.x0 + pl.w; x += 4) g->drawFastHLine(x, y, 2, C_LINE);
    char b[16]; fmtNum(b, sizeof b, tk[i], tk[nt - 1] - tk[0] < 5 && nt > 1 ? 2 : 0, SIGN_NONE);
    int ly = y - 6; if (ly < box.y) ly = box.y; if (ly > pl.y0 + pl.h - 13) ly = pl.y0 + pl.h - 13;
    u.text(slot++, b, 1, pl.x0 - 4, ly, A_RIGHT, C_MUTED);
  }
  g->drawFastHLine(pl.x0, pl.y0 + pl.h, pl.w, C_LINE);

  XMap xm; xm.n = c.n; xm.x0 = pl.x0; xm.w = pl.w;
  xm.timeBased = p.range == RANGE_1D;
  if (xm.timeBased) {
    LocalTime lt = toLocal(c.t[0], nyOffsetSec(c.t[0]));
    xm.t0 = sessionOpenUtc(lt.y, lt.m, lt.d);
    uint32_t close = sessionCloseUtc(lt.y, lt.m, lt.d);
    uint32_t last = c.t[c.n - 1];
    uint32_t end = close > last ? close : last;
    if (xm.t0 > c.t[0]) xm.t0 = c.t[0];
    xm.span = end > xm.t0 ? end - xm.t0 : 1;
  } else { xm.t0 = 0; xm.span = 1; }

  // previous-close reference line (1D): dotted amber
  if (p.range == RANGE_1D && isNum(st.base)) {
    int y = yOf(pl, st.base);
    for (int x = pl.x0; x < pl.x0 + pl.w; x += 6) g->drawFastHLine(x, y, 3, C_MUTED);
  }

  // area + line
  const int base = pl.y0 + pl.h - 1;
  int px = xm.at(c, 0), py = yOf(pl, c.v[0]);
  for (uint16_t i = 1; i < c.n; i++) {
    int x = xm.at(c, i), y = yOf(pl, c.v[i]);
    if (x < px) x = px;
    int dx = x - px;
    for (int k = 0; k <= dx; k++) {                 // shaded area, one column per pixel
      int cy = dx ? py + (y - py) * k / dx : y;
      if (base > cy + 1) g->drawFastVLine(px + k, cy + 1, base - cy - 1, shade);
    }
    px = x; py = y;
  }
  px = xm.at(c, 0); py = yOf(pl, c.v[0]);
  for (uint16_t i = 1; i < c.n; i++) {
    int x = xm.at(c, i), y = yOf(pl, c.v[i]);
    g->drawLine(px, py, x, y, col);
    g->drawLine(px, py + 1, x, y + 1, col);
    px = x; py = y;
  }
  g->fillCircle(px, py, 2, col);                     // last point marker

  xLabels(u, pl, c, xm, p, pl.y0 + pl.h + 2, slot);
}

}  // namespace display
