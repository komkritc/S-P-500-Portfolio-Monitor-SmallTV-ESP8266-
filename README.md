# S&P 500 Portfolio Monitor for GeekMagic SmallTV (ESP8266)

Portfolio-first dashboard on the SmallTV's 240x240 LCD, plus a full web dashboard, both fed by **one** shared state and **one** calculation engine.

```
Yahoo Finance -> MarketData (scheduler) -> AppState (raw inputs)
                                              |
                              FinancialCalculator (the only place money is computed)
                                              |
                      derived outputs: summary / results / quote / chart stats
                            |                                  |
                       SmallTV LCD                     Web API + dashboard
```

Ported from `sp500.ino` (ESP32/TFT_eSPI): same position model (`symbol, qty, buy, date`), same `/portfolio.json` format, same Yahoo `v8/finance/chart` source, same "buy date + THB amount -> buy price = that day's close, qty = THB / price" input convention. Wi-Fi/AP/OTA patterns come from `esp8266_macui.ino`; the display driver and Inter font renderer are copied from it (`src/display/font/`).

## Status - read this first

| Area | State |
|---|---|
| Firmware builds, flashes, boots | **Verified on a real device** (COM3) |
| Wi-Fi join, mDNS, NTP | **Verified on device** (joined, `sp500.local` announced) |
| Live Yahoo ^GSPC quote + 1D/5D/1M charts, TLS, memory | **Verified on device** (see *Memory*) |
| Portfolio maths | Host-tested (723 checks) **and** the on-device NAV/P&L matches an independent calculation |
| LCD layout | Rendered on the host with the real font renderer (47 frames, 0 overflow/overlap). **The physical LCD was not looked at** |
| Web dashboard | Browser-tested against a mock server (desktop + 390 px mobile). **Not yet tested against the real device** (the test PC was on a different subnet) |
| Rescue AP, captive portal, router-restart recovery, wrong password, OTA upload, settings persistence across reboot | Implemented, **not yet exercised on hardware** |
| Long-run stability (hours), heap fragmentation trend | **Not tested** (100 s runs only) |

## Hardware

GeekMagic SmallTV, ESP-12F (ESP8266, 4 MB flash), ST7789 240x240 (SPI **mode 3**, backlight active-low). Pins (from the reference firmware, `src/display/font/lcd_gfx.cpp`): SCLK 14, MOSI 13, DC 0, RST 2, CS 15, BL 5. **No buttons**, so pages and chart ranges rotate automatically (`pageRotateS`).

## Build and flash

Requirements: PlatformIO (tested 6.2.0, `espressif8266@4.1.0` = Arduino-ESP8266 core 3.1.x). Libraries are fetched automatically:

| Library | Version |
|---|---|
| GFX Library for Arduino (moononournation) | 1.6.8 |
| ESP8266 core: ESP8266WiFi, ESP8266WebServer, ESP8266mDNS, DNSServer, ArduinoOTA, Updater, LittleFS | bundled with core 3.1.x |

```bash
cd SP500_SmallTV
pio run                                        # build
pio run -t upload --upload-port COM3           # flash (USB-serial, CH340)
python tools/serial_log.py COM3 60 --reset     # watch the boot log
```

`tools/patch_gfx_lib.py` runs automatically before the build. It stubs one unused function in the *build copy* of the GFX library to free 6.9 KB of RAM (ESP8266 keeps non-PROGMEM `const` tables in RAM). Without it the Yahoo TLS handshake leaves ~3.6 KB of heap and the chip resets.

**Arduino IDE** (untested): board *NodeMCU 1.0 (ESP-12E)*, CPU 160 MHz, Flash *4MB (FS:2MB OTA:~1019KB)*. Open `SP500_SmallTV.ino`, install the libraries above, and apply the same RAM saving by hand (delete `Arduino_DataBus::writeYCbCrPixels` from the GFX library). PlatformIO is the supported path.

## First-time setup (no Wi-Fi saved)

1. Power on. The LCD shows a QR code and **`sp500-setup-XXXXXX`** (open AP).
2. Join it from a phone (scan the QR). The captive portal opens the dashboard on the **Wi-Fi** tab (or browse to `http://192.168.4.1`).
3. Press *Scan*, pick your **2.4 GHz** network (ESP8266 cannot use 5 GHz), enter the password, *Connect*. The result is reported; credentials are saved only after a successful join.
4. Reach the dashboard at **`http://sp500.local`** or the IP shown on the System tab.

**Wi-Fi recovery.** If the saved network is unreachable for 3 minutes (router replaced, password changed, router off), the device raises a **WPA2 rescue AP** `sp500-setup-XXXXXX` with a random key shown **only on the LCD** (also inside the QR). Join it, open `http://192.168.4.1`, pick the new network. While the rescue AP is up the admin password is waived (physical presence = authorization). The device keeps retrying the saved network and shuts the AP down once it is back and stable. *Forget saved network* (Wi-Fi tab) returns to the open setup AP.

## Using it

* **LCD pages** (auto-rotating, dots show the page): 1 Portfolio (NAV, P/L THB and %, invested, positions, S&P ticker), 2 Positions (2 rows per screen, every row reachable, totals, S&P ticker), 3 Market (price, change, status, 1D -> 5D -> 1M charts, O/H/L/Prev or 1M change). The `^GSPC` ticker stays on pages 1-2.
* **Web** `http://sp500.local`: Portfolio, Market (interactive chart, 1D/5D/1M), Details (sortable table, add/delete positions), Wi-Fi, Settings, System (diagnostics, restart, resets, OTA). The date field takes **dd/mm/yyyy** with a calendar button; the API itself uses ISO `yyyy-mm-dd`.
* **Adding a position:** symbol, purchase date, THB amount. The device fetches the close on that date (last bar *on or before* it for weekends/holidays; never a later bar) and stores `qty = THB / close`. Or enter quantity + buy price directly.

## Data rules

* THB (portfolio) and index points (S&P 500) are never mixed or formatted alike.
* Unknown data is `N/A` (`null` in the API), never `0`. NAV/P&L/return are `N/A` unless **every** valid position has a quote. Invalid rows (qty/buy <= 0) are flagged and excluded.
* A failed refresh keeps the last good value and marks it **stale** (3x the refresh interval, or a failed refresh, or loaded from flash). The LCD shows STALE; the API has `stale` flags and timestamps.
* Quote time is converted to America/New_York with real EST/EDT rules (not the device clock/timezone). Market status uses the NYSE calendar (weekends, holidays, 13:00 early closes) and an old closing quote is never labelled OPEN.
* After the market closes and the final close is cached, refreshes slow down (quote >= 15 min, charts >= 60 min) and resume at the next open (`/api/status` -> `data.effQuoteS`).

## API

See [docs/API.md](docs/API.md). Summary: `GET /api/status|portfolio|positions|market|chart?range=1d|5d|1mo|wifi/status|wifi/scan|settings|positions/job`, `POST /api/settings|positions|positions/delete|refresh|cache/clear|wifi/connect|wifi/reconnect|wifi/forget|system/restart|system/reset-settings|system/factory-reset`, `POST /update` (OTA). Passwords are write-only. If an admin password is set, every `POST` and `/update` need HTTP Basic auth (`admin:<password>`).

## OTA

* Web: System tab -> choose `firmware.bin` (`.pio/build/smalltv/firmware.bin`) -> Upload. Streamed straight into the Updater (no RAM copy). Refused while a flash write is in progress or heap is too fragmented; Yahoo requests are aborted first; restart only after the reply is sent.
* IDE/CLI: ArduinoOTA on port 8266 (password = admin password), e.g. `pio run -t upload --upload-port sp500.local`.

## Memory (measured on the device)

| | |
|---|---|
| Static RAM | 48.4 KB of 80 KB (after the GFX patch; was 54.8 KB) |
| Free heap after boot | ~28 KB |
| During a Yahoo request | ~10 KB free, largest block 7-10 KB (the handshake itself needs ~10 KB) |
| Guard | request refused below 14 KB largest block and aborted if < 6 KB heap remains after the handshake |
| Flash | 696 KB of the 1,044 KB app slot |

Yahoo's TLS Certificate record is **4,610 bytes**, so the receive buffer is 5 KB (the reference firmware's 4 KB would fail). One request at a time; JSON is parsed as a stream straight from the socket (no body buffer). TLS **certificate validation is off** (`setInsecure`, same as the original sketch): a LAN attacker could feed false quotes. Only the TLS handshake (~1.2-1.5 s) blocks the main loop; the web server and LCD run while the body is read.

## Tests

```bash
python tests/run_tests.py            # host unit tests (needs g++/clang++ or `pip install ziglang`)
python tests/preview/run_preview.py  # renders every LCD page to tests/preview/out/*.png + layout checks (needs Pillow)
python tools/mock_server.py          # serves the web UI at :8080 with fake data (MOCK_SCENARIO=normal|stale|missing|offline|empty)
python tools/gen_web_assets.py       # regenerates web/web_assets.h after editing web/index.html
```

Host tests (723 checks): NY DST boundaries, NYSE holidays/early closes, market status, number formats, per-position and portfolio maths (positive/negative P&L, zero/invalid qty, missing/stale quotes, millis wrap), streaming JSON parser (byte-at-a-time, malformed/truncated), real recorded Yahoo responses (`tests/fixtures`), settings validation and persistence, portfolio add/delete/load (incl. the original file format), chart downsampling and axis ticks, view rotation.

## Known limitations

* Unverified on hardware: web UI, Wi-Fi scan/connect from the page, rescue AP, OTA, settings persistence, reboot-offline cache, long-term heap behaviour.
* ESP8266 is 2.4 GHz only. A bad Wi-Fi password is detected after ~4-20 s.
* Yahoo's unofficial endpoint may rate-limit (HTTP 429/403): the device backs off up to 10 min and shows stale data.
* Up to 24 positions and 8 distinct symbols. Non-US symbols work for quotes/prices, but the market-status label always describes the NYSE.
* No buttons on this hardware, so no manual navigation; ranges and pages rotate on a timer.
* The admin password is sent as HTTP Basic over plain HTTP (LAN only).
* `ui.png` could not be viewed (the image backend returned 401); the LCD design follows the wireframes in the task and the palette, not the picture.

## Project layout

```
SP500_SmallTV.ino        setup()/loop(): wiring only
src/core/                AppTypes, AppState, MarketClock (NY time/NYSE), Format, JsonWriter, FlatJson
src/finance/             FinancialCalculator, Portfolio, YahooParser, YahooFinance (transport), MarketData (scheduler), ChartData
src/config/              AppConfig (settings limits/validation)
src/storage/             StorageManager (LittleFS, atomic writes, CRC'd cache)
src/network/             WiFiManager, WebServerManager, OTAManager
src/display/             DisplayManager, UiKit, Widgets, Portfolio/Positions/Market screens, ChartRenderer, ViewSchedule, font/ (reference driver + fonts)
web/                     index.html (source), web_assets.h (generated, gzip)
tests/ tools/ docs/
```
