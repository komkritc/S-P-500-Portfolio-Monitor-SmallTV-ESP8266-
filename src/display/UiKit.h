// UiKit.h - small drawing toolkit on top of the reference firmware's anti-aliased Inter font renderer.
//
// Text is drawn through "slots". A slot remembers what it last drew (string, colour, size, position);
// drawing the same thing again is a no-op, and a changed value clears only its own rectangle. That gives
// flicker-free partial updates on a panel with no frame buffer (ESP8266 has no RAM for 115 KB).
#pragma once
#include <stdint.h>
#include <stddef.h>

class Arduino_GFX;

#define UI565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

namespace ui {

// Palette (dark theme, same family as the web dashboard)
static const uint16_t C_BG      = 0x0000;
static const uint16_t C_PANEL   = UI565(0x11, 0x1A, 0x26);
static const uint16_t C_LINE    = UI565(0x24, 0x30, 0x40);
static const uint16_t C_HEAD    = UI565(0x22, 0xD3, 0xEE);   // cyan headings
static const uint16_t C_TEXT    = UI565(0xFF, 0xFF, 0xFF);   // primary values
static const uint16_t C_MUTED   = UI565(0x8B, 0x95, 0xA5);   // secondary
static const uint16_t C_POS     = UI565(0x22, 0xC5, 0x5E);   // profit
static const uint16_t C_NEG     = UI565(0xEF, 0x44, 0x44);   // loss
static const uint16_t C_WARN    = UI565(0xF5, 0x9E, 0x0B);   // stale / pre-market
static const uint16_t C_BLUE    = UI565(0x3B, 0x82, 0xF6);

enum Align : uint8_t { A_LEFT = 0, A_CENTER, A_RIGHT };

struct Slot {
  char     s[30];
  uint16_t fg, bg;
  int16_t  ax;             // anchor x (left edge of the text) - the cache key
  int16_t  x, y, w, h;     // rectangle last painted (clamped to the screen, for clearing)
  uint8_t  sz;
  bool     used;
};

static const uint8_t MAX_SLOTS = 41;

struct Rect { int16_t x, y, w, h; };

class Ui {
 public:
  void begin(Arduino_GFX *g) { gfx_ = g; reset(); }
  Arduino_GFX *gfx() { return gfx_; }
  void reset();                                   // forget everything drawn (after the screen was cleared)
  void clear(uint16_t color = 0);                 // fillScreen + reset

  // Draw `s` at (x, y) with horizontal alignment relative to x. Returns the painted width.
  int text(uint8_t slot, const char *s, uint8_t size, int x, int y, Align a, uint16_t fg, uint16_t bg = C_BG);
  // Largest of sizes[0..n) (listed big -> small) whose width fits maxW; falls back to the smallest.
  int fit(uint8_t slot, const char *s, const uint8_t *sizes, uint8_t n, int maxW, int x, int y, Align a, uint16_t fg, uint16_t bg = C_BG, uint8_t *usedSize = nullptr);
  void eraseSlot(uint8_t slot);                   // clear a slot's painted area and forget it

  static int width(const char *s, uint8_t size);
  static int lineH(uint8_t size);          // text cell (ascent only)
  static int boxH(const char *s, uint8_t size);   // cell + descenders of this string: the real painted height

#ifdef UI_TEST_HOOKS
  const Slot *slotAt(uint8_t i) const { return &slots_[i]; }
#endif

 private:
  Arduino_GFX *gfx_ = nullptr;
  Slot slots_[MAX_SLOTS];
};

// Shapes
void wifiIcon(Arduino_GFX *g, int x, int y, int bars /*0..4*/, uint16_t on, uint16_t off, bool slash);
void pageDots(Arduino_GFX *g, int cx, int y, int count, int current);
uint16_t signColor(double v);                     // green / red judged on the displayed 2-decimal value, grey when unknown
}
