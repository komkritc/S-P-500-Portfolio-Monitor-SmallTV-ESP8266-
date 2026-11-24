// OTAManager.h - firmware update over Wi-Fi.
//   * Web:  POST /update (multipart "firmware"), streamed straight into the Updater (4 KB sector buffer, no RAM copy).
//   * IDE:  ArduinoOTA (espota) on port 8266, password = admin password when one is set.
// An update is refused while a flash write is in progress or the heap is too fragmented; Yahoo requests
// are aborted first so BearSSL's memory is free. Restart happens only after the response was sent.
#pragma once
#include <Arduino.h>
#include <ESP8266WebServer.h>

namespace ota {
void begin(const char *hostname, const char *password);   // ArduinoOTA
void loop();
bool active();                            // an update is running: the rest of the app must stay out of the way
// Web upload handlers (register with server.on("/update", HTTP_POST, ota::webDone, ota::webUpload)).
void webUpload(ESP8266WebServer &server, bool authorized);
void webDone(ESP8266WebServer &server, bool authorized);
const char *lastError();
bool restartPending();
void scheduleRestart(uint32_t delayMs);   // restart once storage is idle and the delay elapsed
void restartLoop();
}
