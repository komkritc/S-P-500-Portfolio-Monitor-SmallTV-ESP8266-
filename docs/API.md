# HTTP API

Base URL: `http://<device-ip>/` or `http://sp500.local/`. All API bodies are JSON (`Content-Type: application/json`).
All responses are compact JSON. Money is THB (portfolio) or index points (S&P 500); the two are never mixed.

Conventions

* `null` means **not available** (never `0`). Clients must render `N/A`.
* `*Epoch`, `t[]`, `updated` are Unix seconds (UTC). `0` = never.
* `stale: true` = last refresh failed or is older than 3x the refresh interval (cached value is still shown).
* Errors: `{"ok":false,"error":"text"}` with 4xx/5xx. Unknown `/api/*` -> 404 same shape.
* `GET /api/*` carry `Access-Control-Allow-Origin: *`. Mutating calls are same-origin only.
* If an admin password is set (Settings), every `POST` and `/update` need HTTP Basic auth (`admin:<password>`), and so do all sale endpoints (`/api/realised`, `/api/sales*`) because they expose money; a 401 carries `WWW-Authenticate: Basic`.
* Passwords (Wi-Fi, admin) are write-only. They are never returned.

## Read endpoints

### GET /api/status
```json
{"fw":"1.1.3","uptime":3600,
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
            "realisedPl":-3910.00,"lifetimePl":14590.00,
            "positions":3,"priced":3,"invalid":0,"complete":true,"stale":false}}
```
`nav`, `pl`, `returnPct` are `null` unless `complete` (every valid position has a quote). `returnPct` is `null` when invested = 0.

`realisedPl` = all-time realised P/L from `/sells.jsonl` (`null` while the ledger has not been read). `lifetimePl` = `pl + realisedPl`, `null` while either side is unknown (never `0` for "unknown"). The open-position P/L is labelled **Unrealised P/L** everywhere in the UI.

### GET /api/positions
```json
{"currency":"THB","summary":{...same as above...},
 "positions":[{"i":0,"id":1,"symbol":"^GSPC","date":"2025-11-20","qty":39.170000,"oqty":39.170000,
   "buy":6382.100000,"price":6910.39,"cost":250000.00,"nav":270690.05,"pl":20690.05,"returnPct":8.28,
   "quote":"fresh","quoteEpoch":1795210554}]}
```
`quote`: `fresh` | `stale` | `missing` | `invalid` (bad qty/buy -> excluded from totals). `price`, `nav`, `pl`, `returnPct` are `null` if `missing`/`invalid`. `quoteEpoch` is the market time of the price.

`id` is the **lot id** (unique, never reused) and `oqty` the quantity at purchase; `qty` is what is still open. A sale reduces `qty` and disappears from this list once the lot is fully sold (its history stays in `/api/sales`).

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
{"portfolioRefreshS":60,"quoteRefreshS":60,"chartRefreshS":300,"pageRotateS":8,"backlight":90,
 "defaultRange":"1d","tzMode":"exchange","localOffsetMin":420,"hostname":"sp500","adminPassSet":false,
 "limits":{"portfolioRefreshS":[15,3600],"quoteRefreshS":[15,3600],"chartRefreshS":[60,3600],
           "pageRotateS":[3,120],"localOffsetMin":[-720,840],"backlight":[5,100]}}
```
`pages`: integer bitmask of the LCD pages that rotate, `1` Portfolio, `2` Positions, `4` Market, `8` Realised P/L (default `15` = all). `1..15`; `0` or anything else is rejected with `400 pages must be 1..15`, so at least one page always stays on. The Wi-Fi setup/rescue screen is not affected. `charts`: integer bitmask of the chart ranges the LCD Market page cycles through, `1` 1D, `2` 5D, `4` 1M (default `7` = all), `1..7`, else `400 charts must be 1..7`. It only affects the LCD; the web chart and the background refresh still cover all three ranges. `defaultRange`: `1d`|`5d`|`1mo`. `tzMode`: `exchange` (America/New_York) | `local` (fixed UTC offset `localOffsetMin`, no DST). Bangkok is `local` + `420` (ICT has never observed DST, so a fixed offset is exact); the web UI's **Zone preset** dropdown offers Bangkok, UTC+8, Tokyo/Seoul, India and UTC and fills the offset for you. In `local` mode the zone shown by `/api/market` is `UTC+07:00` while `tzName` stays the instrument's exchange zone - NYSE session hours and the OPEN/CLOSED badge always follow America/New_York rules, never the display zone.

`backlight` (`5..100`, default `90`, added in fw 1.1.6): LCD brightness in percent. POST it on its own and the panel dims straight away - no restart, and the **LCD brightness (%)** slider on the Settings tab does exactly that when you let go of it. The backlight is PWM on GPIO 5 (`analogWrite`, range 255) wired **active-low**: duty 0 is full on and 255 dark, so the level is inverted on the way to the pin. Duty is the slider position squared, because an LED's light output is linear in duty while the eye is not - 50 % is about a quarter of the light. `5 %` is a hard floor (6/255 on the high side), so the panel can never be dimmed past the point where only the web UI could bring it back. The level is kept in `settings.json`, so it survives reboots and OTA updates; a settings file from before 1.1.6 simply keeps `90`.

### GET /api/positions/job
State of the last "add position" request: `{"state":"idle|pending|ok|error","error":"","symbol":"^GSPC","updated":false}`.

### GET /api/realised
Aggregates over the whole sale ledger (admin auth). One streaming pass at boot, then kept in RAM.
```json
{"valid":true,"pl":-3910.00,"lifetimePl":14590.00,
 "count":3,"reversed":1,"records":4,
 "limit":2000,"warn":false,"full":false,"recovery":false,"badLine":false,
 "hasLast":true,"lastId":"TX-000003","lastSymbol":"^GSPC","lastDate":"2026-11-19",
 "lastRealised":-3910.00,"lastTs":1795210560,
 "years":[{"year":2026,"pl":-3910.00},{"year":2025,"pl":1200.00}]}
```
`count` = sales that still stand (reversed ones excluded), `reversed` = number of undo records, `records` = all lines in the ledger. `pl` is the sum over valid **and** reversal records (a reversal carries the negated amount). `years` holds up to 8 newest calendar years, newest first. `valid:false` means the ledger could not be read -> `pl` is `null` (never `0`). `warn`/`full` flag the 2000-line ledger cap (warning from 1800). `recovery`/`badLine` report boot-time repair.

### GET /api/sales?symbol=^GSPC&offset=0&limit=8
One page of the ledger, newest first (admin auth). `limit` is clamped to 1..8.
```json
{"symbol":"^GSPC","total":4,"offset":0,"count":2,"limit":8,
 "records":4,"limitRecords":2000,"warn":false,"full":false,"pl":-3910.00,
 "sales":[{"seq":4,"ts":1795210560,"id":"TX-000004","refId":"","type":0,"status":"valid",
   "symbol":"^GSPC","date":"2026-11-19","qty":10.00000000,"price":6200.000000,"refPrice":6200.000000,
   "priceSource":"yahoo","fee":10.0000,"gross":62000.000000,"net":61990.000000,
   "cost":63821.000000,"realised":-1831.000000}]}
```
`type`: `0` sale, `1` reversal (undo). `status`: `valid` | `reversed` (this sale was undone) | `reversal`. `refId` is set on reversals (id of the original sale). `priceSource`: `manual` | `yahoo`. Rows never carry their lot allocations; use the preview of a new sale or the LCD for those.

### GET /api/sales/export[?symbol=^GSPC]
`text/csv` attachment (`sells.csv`), streamed. Header:
`seq,date,id,type,status,symbol,qty,price,fee,gross,net,cost,realised`

### GET /api/sales/job
State of the last "preview a sale" price lookup: `{"state":"idle|pending|ok|error","symbol":"^GSPC","date":"2026-11-19","price":6200,"error":""}`.

Notes the dashboard relies on

* `positions[].cost` is `null` for `invalid` rows (bad qty/buy); they are excluded from every total.
* `/api/portfolio` `updated: 0` = no quote refreshed yet.
* `/api/positions/job` reports `ok`/`error` once, then returns to `idle` (the result is consumed by the read).
* After `POST /api/wifi/connect`, `connect.state` stays `ok`/`failed` until the next connect, so a page that lost the connection can still read the result.
* The dashboard shows and accepts dates as dd/mm/yyyy; this API always uses ISO `yyyy-mm-dd`.
* `realisedPl`/`lifetimePl` ride `/api/portfolio` (the page that already polls) so the header cards stay cheap; the sale endpoints need admin auth and are only called on the `#sales` page.

## Write endpoints

| Method + path | Body | Success | Notes |
|---|---|---|---|
| `POST /api/settings` | any subset of the settings fields; `adminPass` (string, "" = unchanged), `clearAdminPass:true` | `200 {"ok":true,"settings":{...as GET...}}` | all-or-nothing validation, `400 {"ok":false,"error":"quoteRefreshS must be 15..3600"}`. Changing `hostname` takes effect after restart (`"restart":true` in the reply). |
| `POST /api/refresh` | `{"what":"all"\|"quote"\|"charts"\|"portfolio"}` | `202 {"ok":true}` | queues an immediate refresh |
| `POST /api/cache/clear` | `{}` | `200 {"ok":true}` | drops cached quotes/charts (RAM + flash) and refetches |
| `POST /api/positions` | `{"symbol":"^GSPC","date":"YYYY-MM-DD","thb":10000}` **or** `{"symbol","date","qty","buy"}` | `202 {"ok":true,"state":"pending"}` (thb form, poll `/api/positions/job`) or `200 {"ok":true,"state":"ok"}` (qty/buy form) | `thb` form: buy price = close on that date, `qty = thb / buy`. `409` if a job is pending. Same symbol+date updates the existing row. Max 12 positions. |
| `POST /api/positions/delete` | `{"index":0}` | `200 {"ok":true}` | |
| `POST /api/sales/preview` | `{"symbol":"^GSPC","date":"YYYY-MM-DD","method":"fifo"\|"specific","mode":"qty"\|"thb"\|"all","lotId":1,"qty":10,"amount":10000,"price":6200,"fee":10}` | `200 {"ok":true,"state":"ready","preview":{...}}` | Validates + allocates the lots and computes the money **without writing anything**. Without `price` it answers `202 {"ok":true,"state":"pending"}` and looks up the closing price of `date` (poll `/api/sales/job`, then re-post the same body; the reply is then `ready` and `priceSource:"yahoo"`). `400` with a readable reason (`"only 51.67 available"`, `"no open position for that symbol"`, `"sale date is in the future"`, `"choose a lot"`, ...). |
| `POST /api/sales/confirm` | the preview body + `"token":<preview.token>` | `200 {"ok":true,"state":"committed","id":"TX-000004","seq":4,"preview":{...}}` | The token binds the commit to the portfolio state the preview was computed from: any lot change, new sale or undo in between gives `409` (`"preview is out of date..."`). Never deletes anything; the lot quantities are set to the preview's `after`. |
| `POST /api/sales/reverse` | `{"seq":4}` | `200 {"ok":true}` | Appends a reversal record (an undo is a new line, never an edit). `400` for an unknown `seq`, an already reversed sale, or a reversal of a reversal. |
| `POST /api/sales/delete` | `{"seq":4}` | `200 {"ok":true}` | **Permanently** removes one transaction from `/sells.jsonl`. A sale and its reversal are always removed together (`seq` may name either); an active sale is reversed first so the position is restored. Later records are renumbered (`TX-` ids are kept). `404` unknown `seq`, `409` ledger has unreadable lines. Rewrites the file through a temp copy, finished at boot if power is cut. |
| `POST /api/wifi/connect` | `{"ssid":"home","password":"..."}` | `202 {"ok":true,"state":"connecting"}` | non-blocking; poll `/api/wifi/status` -> `connect.state`. Credentials are saved only after a successful join; on failure the old network is restored. |
| `POST /api/wifi/reconnect` | `{}` | `200 {"ok":true}` | |
| `POST /api/wifi/forget` | `{"confirm":"FORGET"}` | `200 {"ok":true}` | deletes saved credentials, device starts the setup AP |
| `POST /api/system/restart` | `{}` | `200 {"ok":true}` | restarts ~1 s later |
| `POST /api/system/reset-settings` | `{"confirm":"RESET"}` | `200 {"ok":true}` | defaults for settings only (Wi-Fi + portfolio kept), restarts |
| `POST /api/system/factory-reset` | `{"confirm":"FACTORY"}` | `200 {"ok":true}` | erases settings, Wi-Fi, portfolio, cache; restarts into the setup AP |
| `POST /update` | multipart form, file field `firmware` (`.bin`) | `200 text "OK, rebooting"` | on failure `500 text "update failed: ..."` |

Symbols are upper-case `[A-Z0-9^.=-]`, 1..15 chars. Aliases `SP500`, `S&P500`, `GSPC` -> `^GSPC`; `SPX` -> `^SPX`; `NDX`, `NASDAQ` -> `^NDX`.

## Sale ledger

A sale is **Input -> Preview -> Confirm -> Commit**, never a single mutating call:

1. `POST /api/sales/preview` returns the exact allocation and the money:
```json
"preview":{"symbol":"^GSPC","date":"2026-11-19","price":6200.000000,"refPrice":6200.000000,"priceSource":"yahoo",
 "qty":10.00000000,"available":51.67000000,"fee":10.0000,
 "gross":62000.000000,"net":61990.000000,"cost":63821.000000,"realised":-1831.000000,"token":2461770063,
 "alloc":[{"lot":1,"date":"2025-11-20","buy":6382.100000,"oqty":39.17000000,"before":39.17000000,"after":29.17000000}]}
```
`qty` is what is actually taken (`mode:"thb"` divides by the price), `available` the open quantity of that symbol before the sale. `cost` = `Σ(before - after) x buy` of the touched lots (FIFO = oldest purchase date first; `specific` = one lot by `lotId`), `net = gross - fee`, `realised = net - cost`. `alloc[].after` is the lot's open quantity after the sale.

2. The client shows the preview, then `POST /api/sales/confirm` with the `token`. The commit applies the allocation, drops fully sold lots, bumps the portfolio `ver`/`appliedSeq` and appends one line to `/sells.jsonl`:
```json
{"seq":4,"ts":1795210560,"id":"TX-000004","type":0,"symbol":"^GSPC","date":"2026-11-19",
 "qty":10.00000000,"price":6200.000000,"refPrice":6200.000000,"priceSource":"yahoo",
 "fee":10.0000,"gross":62000.000000,"net":61990.000000,"cost":63821.000000,"realised":-1831.000000,
 "alloc":[{"lot":1,"date":"2025-11-20","buy":6382.100000,"oqty":39.17000000,"before":39.17000000,"after":29.17000000}]}
```
3. `POST /api/sales/reverse` appends `{"type":1,"refSeq":4,"refId":"TX-000004",...,"realised":1831.000000}` and gives the quantity back to the lots (a sold-out lot is re-created).

Guarantees

* Sales and undos are **append-only** records: an undo is a new record, never an edit. The one exception is the explicit `POST /api/sales/delete`, which removes a sale and its reversal for good.
* Applying a record sets `qty = alloc.after` (absolute), so replaying it twice is a no-op.
* Commit order is journal -> append -> save portfolio; `/txn.pending` holds the in-flight record so a power cut mid-append is completed (or the partial tail truncated) at the next boot. `/portfolio.json` `appliedSeq` marks what is already reflected.
* `version` 2 of `/portfolio.json` adds `ver`, `appliedSeq`, `nextLotId`, `nextTxn` and per-lot `id`/`oqty`; a v1 file loads unchanged and is migrated on the next write.
* Ledger lines are capped at 2000 (`warn` at 1800, then `full` and the UI blocks new sales).

## Web pages

`GET /` serves the single-page dashboard (gzip). Hash routes: `#portfolio` (default), `#market`, `#details`, `#sales`, `#wifi`, `#settings`, `#system`. In AP mode any unknown host/URL is redirected to `http://192.168.4.1/#wifi` (captive portal).
