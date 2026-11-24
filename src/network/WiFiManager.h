// WiFiManager.h - non-blocking Wi-Fi: saved credentials, reconnect, async scan, web-driven connect,
// open setup AP (no credentials), WPA2 rescue AP (saved network unreachable), captive-portal DNS, mDNS, NTP.
// Patterns follow the reference firmware (esp8266_macui.ino): AP_STA so the STA radio can scan while the
// AP serves the portal, retry only while nobody is joined to the AP, mDNS restart after every reconnect.
#pragma once
#include <Arduino.h>
#include "../core/AppState.h"

namespace wifimgr {

enum ConnState : uint8_t { CONN_IDLE = 0, CONN_CONNECTING, CONN_OK, CONN_FAILED };
enum ScanState : uint8_t { SCAN_IDLE = 0, SCAN_RUNNING, SCAN_DONE };

struct ScanEntry { char ssid[33]; int8_t rssi; uint8_t channel; bool enc; };
static const uint8_t SCAN_MAX = 16;

void begin(const char *hostname);          // reads /wifi.json, starts STA or the setup AP. Never blocks.
void loop();                               // call every loop(); also services DNS and mDNS
void applyHostname(const char *hostname);  // takes effect after restart (mDNS name)

// Web-driven connect. Returns false when a connect is already running or the input is invalid.
bool requestConnect(const char *ssid, const char *pass);
void requestReconnect();
bool forget();                             // erase saved credentials and open the setup AP

void startScan(bool force);                // non-blocking
ScanState scanState();
uint32_t scanAgeSec();                     // 0xFFFFFFFF if never
const ScanEntry *scanResults(uint8_t &n);

ConnState connState();
const char *connSsid();
const char *connError();

bool apActive();
bool captivePortal();                      // true while the setup/rescue AP is up (redirect unknown URLs)
bool rescueMode();                         // WPA2 rescue AP (physical presence = auth: admin password waived)
bool staConnected();
bool hasSavedNetwork();
const char *apIpText();
void fillNetInfo(NetInfo &n);              // refreshes g_app.net
bool clockSynced();
}
