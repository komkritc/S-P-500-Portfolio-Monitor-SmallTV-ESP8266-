// lcd_font.cpp — see lcd_font.h.
#include "lcd_font.h"

#include <Arduino_GFX_Library.h>
#include <Arduino.h>
#include <string.h>

#include "lcd_fonts.h"

static const int FIRST = 0x20, LAST = 0x7E;
static const unsigned char MIDDOT = 0xB7;

static const LcdFont* pickFont(uint8_t sz) {
  switch (sz) {
    case 1: return &lcdFontSz1;
    case 2: return &lcdFontSz2;
    case 3: return &lcdFontSz3;
    case 4: return &lcdFontSz4;
    case 5: return &lcdFontSz5;
    default: return &lcdFontSz2;
  }
}

// Map a UTF-8 byte sequence's leading byte(s) to a glyph slot. This UI only
// ever emits ASCII plus the single middot separator (UTF-8 0xC2 0xB7); any
// other high-bit byte falls back to '?' rather than reading garbage.
static LcdGlyph glyphAt(const LcdFont* f, unsigned char c, int slot) {
  LcdGlyph g;
  memcpy_P(&g, &f->glyphs[slot], sizeof(g));
  (void)c;
  return g;
}

// Walk `s`, calling fn(slot, advanceOnly) for each drawable glyph. Handles
// the UTF-8 middot (0xC2 0xB7) as a single two-byte unit; every other byte
// is treated as plain ASCII (or '?' if out of the printable range).
template <typename Fn>
static void forEachGlyph(const char* s, Fn fn) {
  const unsigned char* p = (const unsigned char*)s;
  while (*p) {
    if (p[0] == 0xC2 && p[1] == MIDDOT) {
      fn(LAST - FIRST + 1);  // middot slot (index 96 in the 97-wide table... )
      p += 2;
      continue;
    }
    unsigned char c = *p;
    int slot = (c >= FIRST && c <= LAST) ? (c - FIRST) : ('?' - FIRST);
    fn(slot);
    p += 1;
  }
}

int lcdSmoothW(const char* s, uint8_t sz) {
  if (!s) return 0;
  const LcdFont* f = pickFont(sz);
  int w = 0;
  forEachGlyph(s, [&](int slot) {
    LcdGlyph g = glyphAt(f, 0, slot);
    w += g.adv;
  });
  return w;
}

int lcdSmoothLineH(uint8_t sz) {
  return pickFont(sz)->px;
}

int lcdSmoothGlyphH(const char* s, uint8_t sz) {
  if (!s) return 0;
  const LcdFont* f = pickFont(sz);
  int maxH = 0;
  forEachGlyph(s, [&](int slot) {
    LcdGlyph g = glyphAt(f, 0, slot);
    if (g.h > maxH) maxH = g.h;
  });
  return maxH;
}

int lcdSmoothDescent(const char* s, uint8_t sz) {
  if (!s) return 0;
  const LcdFont* f = pickFont(sz);
  int d = 0;
  forEachGlyph(s, [&](int slot) {
    LcdGlyph g = glyphAt(f, 0, slot);
    if (g.w && g.h) { int below = (int)g.h - (int)g.yoff; if (below > d) d = below; }
  });
  return d;
}

void lcdSmoothText(Arduino_GFX* gfx, int x, int yTop, const char* s,
                   uint8_t sz, uint16_t fg, uint16_t bg) {
  if (!gfx || !s) return;
  const LcdFont* f = pickFont(sz);
  const int base = yTop + f->baseY;
  const uint16_t fr = (fg >> 11) & 31, fg_ = (fg >> 5) & 63, fb = fg & 31;
  const uint16_t br = (bg >> 11) & 31, bgc = (bg >> 5) & 63, bb = bg & 31;
  uint16_t row[80]; /* widest glyph at size 7 (56px Bold) is well under this */
  int pen = x;

  forEachGlyph(s, [&](int slot) {
    LcdGlyph g = glyphAt(f, 0, slot);
    if (g.w && g.h && g.w <= (int)(sizeof(row) / sizeof(row[0]))) {
      const int gx = pen + g.xoff;
      const int gy = base - g.yoff;
      const uint8_t* src = f->data + g.off;
      for (uint8_t r = 0; r < g.h; r++, src += g.w) {
        for (uint8_t c = 0; c < g.w; c++) {
          uint8_t a = pgm_read_byte(src + c);
          if (a == 0) {
            row[c] = bg;
          } else if (a == 255) {
            row[c] = fg;
          } else {
            uint16_t r5 = (uint16_t)((fr * a + br * (255 - a) + 127) / 255);
            uint16_t g6 = (uint16_t)((fg_ * a + bgc * (255 - a) + 127) / 255);
            uint16_t b5 = (uint16_t)((fb * a + bb * (255 - a) + 127) / 255);
            row[c] = (r5 << 11) | (g6 << 5) | b5;
          }
        }
        gfx->draw16bitRGBBitmap(gx, gy + r, row, g.w, 1);
      }
    }
    pen += g.adv;
  });
}
