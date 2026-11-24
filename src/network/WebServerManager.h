// WebServerManager.h - HTTP server: gzip SPA at "/", JSON API (docs/API.md), captive portal, OTA upload.
// Responses are streamed through JsonWriter into chunked HTTP, so no response is ever built in RAM.
#pragma once
#include <Arduino.h>

namespace webserver {
void begin();
void loop();
}
