#include "OTAManager.h"
#include <ArduinoOTA.h>
#include <Updater.h>
#include "../storage/StorageManager.h"
#include "../finance/MarketData.h"
#include "../display/DisplayManager.h"

namespace ota {

static const uint32_t MIN_FREE_BLOCK = 6000;     // Updater's 4 KB sector buffer + headroom
static const uint32_t STALL_MS = 30000;

static bool s_active = false;
static uint32_t s_lastMs = 0;
static char s_err[48] = "";
static bool s_authOk = false;
static uint32_t s_restartAt = 0;
static bool s_restart = false;

const char *lastError() { return s_err; }
bool active() { return s_active; }
bool restartPending() { return s_restart; }

void scheduleRestart(uint32_t delayMs) { s_restart = true; s_restartAt = millis() + delayMs; }

void restartLoop() {
  if (!s_restart || (int32_t)(millis() - s_restartAt) < 0) return;
  if (storage::busy()) return;                   // never reset in the middle of a flash write
  market::flushCache();
  Serial.println(F("[sys] restarting"));
  Serial.flush();
  ESP.restart();
}

static void begin_(const char *src) {
  s_active = true;
  s_lastMs = millis();
  market::abortRequests();
  Serial.printf("[ota] start (%s) heap=%u block=%u\n", src, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize());
  display::showOta(src);
  display::otaProgress(0);
}

static void fail(const char *why) {
  Serial.printf("[ota] FAIL %s\n", why);
  strlcpy(s_err, why, sizeof s_err);
  if (s_active) { display::otaResult(false, why); }
  s_active = false;
  display::invalidate();
}

void webUpload(ESP8266WebServer &server, bool authorized) {
  HTTPUpload &up = server.upload();
  switch (up.status) {
    case UPLOAD_FILE_START: {
      s_err[0] = 0;
      s_authOk = authorized;
      if (!s_authOk) { strlcpy(s_err, "unauthorized", sizeof s_err); return; }
      if (storage::busy()) { strlcpy(s_err, "storage busy, try again", sizeof s_err); return; }
      if (ESP.getMaxFreeBlockSize() < MIN_FREE_BLOCK) { snprintf(s_err, sizeof s_err, "low memory (block %u), try again", (unsigned)ESP.getMaxFreeBlockSize()); return; }
      begin_("web upload");
      uint32_t maxSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
      if (!Update.begin(maxSpace, U_FLASH)) strlcpy(s_err, Update.getErrorString().c_str(), sizeof s_err);
      break;
    }
    case UPLOAD_FILE_WRITE:
      if (!s_authOk || s_err[0]) return;
      s_lastMs = millis();
      if (Update.write(up.buf, up.currentSize) != up.currentSize) { strlcpy(s_err, Update.getErrorString().c_str(), sizeof s_err); return; }
      if (up.contentLength) display::otaProgress((int)((uint64_t)up.totalSize * 100 / up.contentLength));
      break;
    case UPLOAD_FILE_END:
      if (!s_authOk || s_err[0]) return;
      if (Update.end(true)) display::otaProgress(100);
      else strlcpy(s_err, Update.getErrorString().c_str(), sizeof s_err);
      break;
    case UPLOAD_FILE_ABORTED:
      if (Update.isRunning()) Update.end();
      if (!s_err[0]) strlcpy(s_err, "upload aborted", sizeof s_err);
      fail(s_err);
      break;
  }
}

void webDone(ESP8266WebServer &server, bool authorized) {
  if (!authorized) { server.requestAuthentication(BASIC_AUTH, "SP500", "unauthorized"); return; }
  if (s_err[0]) {
    if (Update.isRunning()) Update.end();
    char msg[80]; snprintf(msg, sizeof msg, "update failed: %s", s_err);
    server.send(500, "text/plain", msg);
    char why[48]; strlcpy(why, s_err, sizeof why);
    fail(why);
    return;
  }
  if (!s_active) { server.send(400, "text/plain", "update failed: no file"); return; }
  server.send(200, "text/plain", "OK, rebooting");     // reply first, restart afterwards
  display::otaResult(true, "Done, rebooting");
  Serial.println(F("[ota] done"));
  s_active = false;
  scheduleRestart(800);
}

void begin(const char *hostname, const char *password) {
  ArduinoOTA.setPort(8266);
  ArduinoOTA.setHostname(hostname);
  if (password && password[0]) ArduinoOTA.setPassword(password);
  ArduinoOTA.onStart([]() { if (storage::busy()) return; begin_("ArduinoOTA"); });
  ArduinoOTA.onProgress([](unsigned int d, unsigned int t) { s_lastMs = millis(); if (t) display::otaProgress((int)((uint64_t)d * 100 / t)); });
  ArduinoOTA.onEnd([]() { display::otaProgress(100); display::otaResult(true, "Done, rebooting"); });
  ArduinoOTA.onError([](ota_error_t e) {
    fail(e == OTA_AUTH_ERROR ? "auth failed" : e == OTA_BEGIN_ERROR ? "begin failed" : e == OTA_CONNECT_ERROR ? "connect failed" : e == OTA_RECEIVE_ERROR ? "receive failed" : e == OTA_END_ERROR ? "end failed" : "error");
  });
  ArduinoOTA.begin(false);                       // mDNS is owned by WiFiManager (it advertises the _arduino service)
}

void loop() {
  ArduinoOTA.handle();
  if (s_active && (millis() - s_lastMs) > STALL_MS) {      // a web upload whose client vanished must not freeze the device
    if (Update.isRunning()) Update.end();
    fail("upload stalled");
  }
}

}  // namespace ota
