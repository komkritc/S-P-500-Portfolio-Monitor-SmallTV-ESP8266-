// lcd_gfx.h — ST7789 240x240 device bring-up only.
//
// Borrowed from smalltv-mod 2.16.0 (src/Gfx.{h,cpp}, src/board_esp8266.h,
// src/config.h) and simplified for this firmware: no Settings dependency —
// the caller passes a brightness percent to lcdSetBacklight(), rotation 0, RGB
// order, no gain tint.
//
// This macOS-style UI draws everything itself (lcd_ui.h) via Arduino_GFX
// primitives, so unlike the original lcd_gfx.h this trimmed copy exposes
// only the hardware bring-up (lcdBegin/lcdDev) — no built-in-font text
// helpers, no smooth-font-dependent splash/AP screens.
#pragma once
#include <Arduino.h>

class Arduino_GFX;   // fwd-decl: only lcd_gfx.cpp pulls in the full lib

#define LCD_W 240
#define LCD_H 240

void         lcdBegin();          // backlight + SPI + panel init, clears screen (boot brightness)
void         lcdSetBacklight(int pct);   // LCD dim: BL_MIN_PCT..BL_MAX_PCT, clamped, live
Arduino_GFX* lcdDev();            // shared draw target (nullptr before begin)
