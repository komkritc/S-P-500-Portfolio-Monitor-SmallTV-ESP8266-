# HTTP API

Base URL: `http://<device-ip>/` or `http://sp500.local/`. All API bodies are JSON (`Content-Type: application/json`).
All responses are compact JSON. Money is THB (portfolio) or index points (S&P 500); the two are never mixed.

Conventions

* `null` means **not available** (never `0`). Clients must render `N/A`.
* `*Epoch`, `t[]`, `updated` are Unix seconds (UTC). `0` = never.
* `stale: true` = last refresh failed or is older than 3x the refresh interval (cached value is still shown).
* Errors: `{"ok":false,"error":"text"}` with 4xx/5xx. Unknown `/api/*` -> 404 same shape.
* `GET /api/*` carry `Access-Control-Allow-Origin: *`. Mutating calls are same-origin only.
* If an admin password is set (Settings), every `POST` and `/update` need HTTP Basic auth (`admin:<password>`); a 401 carries `WWW-Authenticate: Basic`.
* Passwords (Wi-Fi, admin) are write-only. They are never returned.

## Read endpoints

### GET /api/status
```json
{"fw":"1.0.0","uptime":3600,
 "time":{"epoch":1795210554,"synced":true},
 "heap":{"free":21000,"maxBlock":17000,"frag":12},
 "chip":{"id":"A1B2C3","cpuMHz":160,"flashKB":4096,"sketchKB":512,"freeSketchKB":1019,"sdk":"3.1.2"},
 "fs":{"ok":true,"totalKB":1024,"usedKB":16},
 "wifi":{"mode":"sta","connected":true,"ssid":"home","ip":"192.168.1.50","rssi":-61,"mdns":"sp500.local"},
 "data":{"lastMarketOk":1795210560,"lastError":"HTTP 429","lastErrorEpoch":1795210000,"quoteStale":false,"portfolioStale":false},
 "ota":{"active":false}}
```
`wifi.mode`: `sta` | `ap-setup` (no saved network) | `ap-rescue` (saved network unreachable). `lastError` is `""` when none.

### GET /api/portfolio
```json
{"currency":"THB","updated":1795210560,
 "summary":{"invested":250000.00,"nav":268500.00,"pl":18500.00,"returnPct":7.40,
            "positions":3,"priced":3,"invalid":0,"complete":true,"stale":false}}
```
`nav`, `pl`, `returnPct` are `null` unless `complete` (every valid position has a quote). `returnPct` is `null` when invested = 0.

### GET /api/positions
```json
{"currency":"THB","summary":{...same as above...},
 "positions":[{"i":0,"symbol":"^GSPC","date":"2025-11-20","qty":39.170000,"buy":6382.100000,
   "price":6910.39,"cost":250000.00,"nav":270690.05,"pl":20690.05,"returnPct":8.28,
   "quote":"fresh","quoteEpoch":1795210554}]}
```
`quote`: `fresh` | `stale` | `missing` | `invalid` (bad qty/buy -> excluded from totals). `price`, `nav`, `pl`, `returnPct` are `null` if `missing`/`invalid`. `quoteEpoch` is the market time of the price.

### GET /api/market
```json
{"symbol":"^GSPC","valid":true,"stale":false,
 "last":6910.39,"prevClose":7175.83,"change":-265.44,"changePct":-3.70,
 "open":7150.39,"high":7198.67,"low":6882.66,
 "status":"CLOSED","sessionDate":"2026-11-20",
 "quoteTime":1795210554,"quoteTimeText":"Nov 20, 16:35 EST","timezone":"EST","tzName":"America/New_York",
 "fetchedEpoch":1795210560}
```
`status`: `OPEN` | `CLOSED` | `PRE-MARKET` | `N/A` (clock not yet synced). `quoteTimeText` honours the Settings timezone mode. Numbers that are unknown are `null`. When `valid` is false everything except `symbol`/`status` is `null`.

### GET /api/chart?range=1d|5d|1mo
```json
{"range":"1d","interval":"5m","valid":true,"stale":false,"updated":1795210560,"count":79,
 "first":7150.39,"last":6910.39,"base":7175.83,"change":-265.44,"changePct":-3.70,
 "min":6882.66,"max":7198.67,
 "t":[1795185000,1795185300],"v":[7150.39,7135.65]}
```
`base` is the reference for `change`: previous close for `1d`, first point for `5d`/`1mo`. `t[]`/`v[]` are parallel arrays, no synthetic points. Optional `&max=N` (N>=8) downsamples (min/max preserving). If never fetched: `valid:false,count:0,t:[],v:[]`. Bad `range` -> 400.

### GET /api/wifi/status
```json
{"mode":"sta","state":"connected","ssid":"home","ip":"192.168.1.50","rssi":-61,"mdns":"sp500.local",
 "saved":true,"apSsid":"","apIp":"",
 "connect":{"state":"idle","ssid":"","error":""},
 "scan":{"state":"idle","ageSec":12,"count":7}}
```
`state`: `connected` | `connecting` | `disconnected`. `connect.state`: `idle` | `connecting` | `ok` | `failed` (`error` set on `failed`). `scan.state`: `idle` | `scanning` | `done`. `apSsid`/`apIp` only while an AP is up (`rescue` AP key is shown on the LCD only).

### GET /api/wifi/scan[?refresh=1]
Non-blocking. Starts a scan when none is cached/running (or `refresh=1`), returns immediately.
```json
{"scanning":false,"ageSec":3,"networks":[{"ssid":"home","rssi":-52,"enc":true,"ch":6}]}
```
While `scanning` the previous list (possibly empty) is returned. Poll every ~1.5 s.

### GET /api/settings
```json
{"portfolioRefreshS":60,"quoteRefreshS":60,"chartRefreshS":300,"pageRotateS":8,
 "defaultRange":"1d","tzMode":"exchange","localOffsetMin":420,"hostname":"sp500","adminPassSet":false,
 "limits":{"portfolioRefreshS":[15,3600],"quoteRefreshS":[15,3600],"chartRefreshS":[60,3600],
           "pageRotateS":[3,120],"localOffsetMin":[-720,840]}}
```
`defaultRange`: `1d`|`5d`|`1mo`. `tzMode`: `exchange` (America/New_York) | `local` (fixed UTC offset `localOffsetMin`, no DST).

### GET /api/positions/job
State of the last "add position" request: `{"state":"idle|pending|ok|error","error":"","symbol":"^GSPC","updated":false}`.

Notes the dashboard relies on

* `positions[].cost` is `null` for `invalid` rows (bad qty/buy); they are excluded from every total.
* `/api/portfolio` `updated: 0` = no quote refreshed yet.
* `/api/positions/job` reports `ok`/`error` once, then returns to `idle` (the result is consumed by the read).
* After `POST /api/wifi/connect`, `connect.state` stays `ok`/`failed` until the next connect, so a page that lost the connection can still read the result.
* The dashboard shows and accepts dates as dd/mm/yyyy; this API always uses ISO `yyyy-mm-dd`.

## Write endpoints

| Method + path | Body | Success | Notes |
|---|---|---|---|
| `POST /api/settings` | any subset of the settings fields; `adminPass` (string, "" = unchanged), `clearAdminPass:true` | `200 {"ok":true,"settings":{...as GET...}}` | all-or-nothing validation, `400 {"ok":false,"error":"quoteRefreshS must be 15..3600"}`. Changing `hostname` takes effect after restart (`"restart":true` in the reply). |
| `POST /api/refresh` | `{"what":"all"\|"quote"\|"charts"\|"portfolio"}` | `202 {"ok":true}` | queues an immediate refresh |
| `POST /api/cache/clear` | `{}` | `200 {"ok":true}` | drops cached quotes/charts (RAM + flash) and refetches |
| `POST /api/positions` | `{"symbol":"^GSPC","date":"YYYY-MM-DD","thb":10000}` **or** `{"symbol","date","qty","buy"}` | `202 {"ok":true,"state":"pending"}` (thb form, poll `/api/positions/job`) or `200 {"ok":true,"state":"ok"}` (qty/buy form) | `thb` form: buy price = close on that date, `qty = thb / buy`. `409` if a job is pending. Same symbol+date updates the existing row. Max 24 positions. |
| `POST /api/positions/delete` | `{"index":0}` | `200 {"ok":true}` | |
| `POST /api/wifi/connect` | `{"ssid":"home","password":"..."}` | `202 {"ok":true,"state":"connecting"}` | non-blocking; poll `/api/wifi/status` -> `connect.state`. Credentials are saved only after a successful join; on failure the old network is restored. |
| `POST /api/wifi/reconnect` | `{}` | `200 {"ok":true}` | |
| `POST /api/wifi/forget` | `{"confirm":"FORGET"}` | `200 {"ok":true}` | deletes saved credentials, device starts the setup AP |
| `POST /api/system/restart` | `{}` | `200 {"ok":true}` | restarts ~1 s later |
| `POST /api/system/reset-settings` | `{"confirm":"RESET"}` | `200 {"ok":true}` | defaults for settings only (Wi-Fi + portfolio kept), restarts |
| `POST /api/system/factory-reset` | `{"confirm":"FACTORY"}` | `200 {"ok":true}` | erases settings, Wi-Fi, portfolio, cache; restarts into the setup AP |
| `POST /update` | multipart form, file field `firmware` (`.bin`) | `200 text "OK, rebooting"` | on failure `500 text "update failed: ..."` |

Symbols are upper-case `[A-Z0-9^.=-]`, 1..15 chars. Aliases `SP500`, `S&P500`, `GSPC` -> `^GSPC`; `SPX` -> `^SPX`; `NDX`, `NASDAQ` -> `^NDX`.

## Web pages

`GET /` serves the single-page dashboard (gzip). Hash routes: `#portfolio` (default), `#market`, `#details`, `#wifi`, `#settings`, `#system`. In AP mode any unknown host/URL is redirected to `http://192.168.4.1/#wifi` (captive portal).
