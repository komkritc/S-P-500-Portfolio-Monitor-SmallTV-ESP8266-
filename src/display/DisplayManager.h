// DisplayManager.h - owns the ST7789 panel and the automatic page rotation (the SmallTV has no buttons).
// Reads AppState only; never calculates money itself.
#pragma once
#include <Arduino.h>
#include "../core/AppState.h"

namespace display {
void begin();                             // panel bring-up (SPI mode 3, backlight) + splash
void loop(const AppState &a);             // call every loop(); repaints only what changed
void invalidate();                        // next loop() repaints the whole screen
void showOta(const char *source);         // firmware update screen (loop() paints nothing while active)
void otaProgress(int pct);
void otaResult(bool ok, const char *msg);
void showBoot(const char *line);
}
