// lcd_font.h — anti-aliased Inter text for the macOS-style SmallTV LCD UI.
//
// Glyph atlases are produced offline by tools/gen_lcd_macui_font.py (Pillow,
// 8-bit alpha, printable ASCII + middot at 6 size/weight tiers matching the
// UI's logical text sizes 1/2/3/4/5/7).
//
// Unlike esp8266-smalltv-claude's lcd_font.cpp (which only ever blends
// against pure black), this UI draws text over several different flat
// backgrounds -- the light card, the dark translucent menu strip, colored
// alert circles -- so every call here takes an explicit `bg` color to blend
// against. Callers must pass the *actual* flat color immediately behind the
// glyph (no backing framebuffer on this MCU, so this can't be sampled back
// from the panel).
#pragma once
#include <stdint.h>

class Arduino_GFX;

// Width of `s` in px when drawn at logical text size `sz` (1/2/3/5/7).
int lcdSmoothW(const char* s, uint8_t sz);

// Height (full font cell) of logical size `sz`, for vertical layout math.
int lcdSmoothLineH(uint8_t sz);

// Draw `s` with its top-left cell corner at (x, yTop) in `fg`, alpha-blended
// against the flat background color `bg`. No wrapping, no clipping.
void lcdSmoothText(Arduino_GFX* gfx, int x, int yTop, const char* s,
                   uint8_t sz, uint16_t fg, uint16_t bg);

// Max glyph bitmap height (not line height) across all drawable glyphs in
// `s`, at logical size `sz`. Digits/caps share a common cap height (no
// descenders), so this gives an accurate vertical extent for centering a
// short numeric/caption string -- unlike the fixed em-box lcdSmoothLineH().
int lcdSmoothGlyphH(const char* s, uint8_t sz);

// Rows the glyphs of `s` extend BELOW the text cell (descenders, commas, parentheses) at size `sz`.
// The cell returned by lcdSmoothLineH() only covers the ascent: clear/overlap logic must add this.
int lcdSmoothDescent(const char* s, uint8_t sz);
