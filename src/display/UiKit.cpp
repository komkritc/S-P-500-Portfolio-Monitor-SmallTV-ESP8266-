#include "UiKit.h"
#include "font/lcd_font.h"
#include <Arduino_GFX_Library.h>
#include <string.h>
#include <math.h>

namespace ui {

int Ui::width(const char *s, uint8_t size) { return lcdSmoothW(s, size); }
int Ui::lineH(uint8_t size) { return lcdSmoothLineH(size); }
int Ui::boxH(const char *s, uint8_t size) { return lcdSmoothLineH(size) + lcdSmoothDescent(s, size); }

void Ui::reset() { memset(slots_, 0, sizeof slots_); }
void Ui::clear(uint16_t c) { gfx_->fillScreen(c); reset(); }

void Ui::eraseSlot(uint8_t i) {
  if (i >= MAX_SLOTS) return;
  Slot &sl = slots_[i];
  if (sl.used) gfx_->fillRect(sl.x, sl.y, sl.w, sl.h, sl.bg);
  sl.used = false;
}

int Ui::text(uint8_t i, const char *s, uint8_t size, int x, int y, Align a, uint16_t fg, uint16_t bg) {
  if (i >= MAX_SLOTS) return 0;
  Slot &sl = slots_[i];
  const int w = lcdSmoothW(s, size);
  const int px = a == A_LEFT ? x : a == A_CENTER ? x - w / 2 : x - w;
  if (sl.used && sl.sz == size && sl.fg == fg && sl.bg == bg && sl.ax == px && sl.y == y && !strncmp(sl.s, s, sizeof sl.s - 1)) return w;
  if (sl.used) gfx_->fillRect(sl.x, sl.y, sl.w, sl.h, sl.bg);
  const int h = lcdSmoothLineH(size) + lcdSmoothDescent(s, size);   // cell + descenders
  int h2 = h;
  // glyphs can overhang the advance box by a pixel: pad 1 px each side, clamp to the screen
  int rx = px - 1, rw = w + 2;
  if (rx < 0) { rw += rx; rx = 0; }
  if (rx + rw > 240) rw = 240 - rx;
  if (y + h > 240) h2 = 240 - y;
  if (rw > 0) gfx_->fillRect(rx, y, rw, h2, bg);
  lcdSmoothText(gfx_, px, y, s, size, fg, bg);
  sl.used = true; sl.sz = size; sl.fg = fg; sl.bg = bg; sl.ax = px; sl.y = y; sl.x = rx; sl.w = rw > 0 ? rw : 0; sl.h = h;
  strncpy(sl.s, s, sizeof sl.s - 1); sl.s[sizeof sl.s - 1] = 0;
  return w;
}

int Ui::fit(uint8_t i, const char *s, const uint8_t *sizes, uint8_t n, int maxW, int x, int y, Align a, uint16_t fg, uint16_t bg, uint8_t *used) {
  uint8_t pick = sizes[n - 1];
  for (uint8_t k = 0; k < n; k++) if (lcdSmoothW(s, sizes[k]) <= maxW) { pick = sizes[k]; break; }
  if (used) *used = pick;
  // keep the text vertically at the bottom of the biggest cell so different sizes share a baseline-ish line
  return text(i, s, pick, x, y + (lcdSmoothLineH(sizes[0]) - lcdSmoothLineH(pick)) / 2, a, fg, bg);
}

void wifiIcon(Arduino_GFX *g, int x, int y, int bars, uint16_t on, uint16_t off, bool slash) {
  // four ascending bars, 3 px wide, 2 px gap; icon box 18 x 13 with its top-left at (x, y)
  for (int i = 0; i < 4; i++) {
    int h = 4 + i * 3;
    g->fillRect(x + i * 5, y + 13 - h, 3, h, i < bars ? on : off);
  }
  if (slash) {
    g->drawLine(x, y + 13, x + 17, y, C_NEG);
    g->drawLine(x, y + 14, x + 17, y + 1, C_NEG);
  }
}

void pageDots(Arduino_GFX *g, int cx, int y, int count, int current) {
  const int step = 14;
  int x0 = cx - (count - 1) * step / 2;
  for (int i = 0; i < count; i++) {
    int x = x0 + i * step;
    g->fillCircle(x, y, 4, C_BG);
    if (i == current) g->fillCircle(x, y, 4, C_HEAD);
    else { g->drawCircle(x, y, 4, C_MUTED); }
  }
}

uint16_t signColor(double v) {
  if (isnan(v) || isinf(v)) return C_MUTED;
  long long r = (long long)(fabs(v) * 100.0 + 0.5);
  if (r == 0) return C_TEXT;
  return v < 0 ? C_NEG : C_POS;
}

}  // namespace ui
