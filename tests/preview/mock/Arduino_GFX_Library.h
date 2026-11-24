// Host framebuffer with the Arduino_GFX subset the UI uses. Counts out-of-bounds writes.
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <algorithm>
class Arduino_GFX {
 public:
  uint16_t fb[240 * 240];
  long oob = 0;
  int oobX = -1, oobY = -1;
  void px(int x, int y, uint16_t c) { if (x < 0 || y < 0 || x >= 240 || y >= 240) { if (!oob) { oobX = x; oobY = y; } oob++; return; } fb[y * 240 + x] = c; }
  void draw16bitRGBBitmap(int16_t x, int16_t y, uint16_t *b, int16_t w, int16_t h) { for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) px(x + i, y + j, b[j * w + i]); }
  void fillScreen(uint16_t c) { for (auto &p : fb) p = c; }
  void fillRect(int x, int y, int w, int h, uint16_t c) { for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) px(x + i, y + j, c); }
  void drawFastHLine(int x, int y, int w, uint16_t c) { fillRect(x, y, w, 1, c); }
  void drawFastVLine(int x, int y, int h, uint16_t c) { fillRect(x, y, 1, h, c); }
  void drawRect(int x, int y, int w, int h, uint16_t c) { drawFastHLine(x, y, w, c); drawFastHLine(x, y + h - 1, w, c); drawFastVLine(x, y, h, c); drawFastVLine(x + w - 1, y, h, c); }
  void drawLine(int x0, int y0, int x1, int y1, uint16_t c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) { px(x0, y0, c); if (x0 == x1 && y0 == y1) break; int e2 = 2 * err; if (e2 >= dy) { err += dy; x0 += sx; } if (e2 <= dx) { err += dx; y0 += sy; } }
  }
  void fillCircle(int cx, int cy, int r, uint16_t c) { for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++) if (x * x + y * y <= r * r) px(cx + x, cy + y, c); }
  void drawCircle(int cx, int cy, int r, uint16_t c) { for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++) { int d = x * x + y * y; if (d <= r * r && d > (r - 1) * (r - 1)) px(cx + x, cy + y, c); } }
  bool inRR(int x, int y, int w, int h, int r) { int cx = std::min(std::max(x, r), w - 1 - r), cy = std::min(std::max(y, r), h - 1 - r); int dx = x - cx, dy = y - cy; return dx * dx + dy * dy <= r * r; }
  void fillRoundRect(int x0, int y0, int w, int h, int r, uint16_t c) { for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (inRR(x, y, w, h, r)) px(x0 + x, y0 + y, c); }
};
