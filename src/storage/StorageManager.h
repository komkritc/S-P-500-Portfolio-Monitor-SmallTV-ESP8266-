// StorageManager.h - LittleFS persistence (replaces the ESP32 Preferences / LittleFS usage of the original).
//
//   /settings.json   Settings (+ admin password), JSON
//   /portfolio.json  positions, same format as the original sketch
//   /wifi.json       {"ssid","pass"}  - never exposed through any API
//   /cache.bin       last good quotes + charts (binary, CRC-checked), so a reboot while offline still shows data
//
// Every write goes to <file>.tmp and is renamed over the target, so a power cut leaves either the old
// or the new file, never a half-written one.
#pragma once
#include <Arduino.h>
#include "../core/AppState.h"

namespace storage {
bool begin();                         // mounts LittleFS, formats it on first use
bool ok();
void fsInfo(uint32_t &totalBytes, uint32_t &usedBytes);
bool busy();                          // true while a write is in progress (restart/OTA wait for it)

void loadSettings(Settings &s);       // keeps defaults for anything missing/invalid
bool saveSettings(const Settings &s);
uint8_t loadPortfolio(Position *pos);
bool savePortfolio(const Position *pos, uint8_t n);

struct WifiCreds { char ssid[33]; char pass[65]; };
bool loadWifi(WifiCreds &w);          // false if none saved
bool saveWifi(const char *ssid, const char *pass);
bool eraseWifi();

bool loadCache(AppState &a);
bool saveCache(const AppState &a);
bool eraseCache();

// Erases everything the firmware owns. keepWifi/keepPortfolio select what survives (settings reset).
void eraseAll(bool keepWifi, bool keepPortfolio);
}
