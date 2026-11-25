#include "WebServerManager.h"
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <time.h>
#include <memory>
#include <new>
#include "WiFiManager.h"
#include "OTAManager.h"
#include "../core/AppState.h"
#include "../core/JsonWriter.h"
#include "../core/FlatJson.h"
#include "../core/Format.h"
#include "../core/MarketClock.h"
#include "../config/AppConfig.h"
#include "../display/DisplayManager.h"   // display::setBacklight (LCD dim)
#include "../finance/ChartData.h"
#include "../finance/MarketData.h"
#include "../finance/Portfolio.h"
#include "../storage/StorageManager.h"
#include "../storage/SaleStore.h"
#include "../../web/web_assets.h"

namespace webserver {

static ESP8266WebServer server(80);
static const char JSON_CT[] = "application/json";

// ---------------------------------------------------------------- helpers
static void sinkChunk(void *, const char *d, size_t n) { server.sendContent(d, n); }

struct Reply {
  JsonWriter w;
  Reply(int code) : w(sinkChunk, nullptr) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Cache-Control", "no-store");
    if (!server.chunkedResponseModeStart(code, JSON_CT)) server.send(500, "text/plain", "chunked not supported");
  }
  ~Reply() { w.flush(); server.chunkedResponseFinalize(); }
};

static void sendJson(int code, const char *body) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, JSON_CT, body);
}

static void sendErr(int code, const char *msg) {
  char b[120];
  JsonWriter::Sink s = [](void *c, const char *d, size_t n) { static_cast<String *>(c)->concat(d, n); };
  String out; out.reserve(100);
  { JsonWriter w(s, &out); w.beginObj().kBool("ok", false).kStr("error", msg).endObj(); }
  (void)b;
  sendJson(code, out.c_str());
}

static void sendOk(int code = 200) { sendJson(code, "{\"ok\":true}"); }

// Mutating calls need the admin password when one is set. Physical presence (the WPA2 rescue AP key is
// shown only on the LCD) waives it so a forgotten password can always be recovered.
static bool authorized() {
  const char *pw = g_app.settings.adminPass;
  if (!pw[0] || wifimgr::rescueMode()) return true;
  return server.authenticate("admin", pw);
}

static bool requireAuth() {
  if (authorized()) return true;
  server.requestAuthentication(BASIC_AUTH, "SP500 Monitor", "unauthorized");
  return false;
}

// POST body (plain). ESP8266WebServer stores a JSON body as arg("plain"); cap the size.
// FlatJson is ~1.3 KB: on the ESP8266 request stack it leaves too little for the handler + LittleFS/JsonWriter
// and the chip hits the watchdog. Keep it on the heap.
#define HEAP_JSON(j, onFail) std::unique_ptr<FlatJson> j##_p(new (std::nothrow) FlatJson); \
  if (!j##_p) { sendErr(500, "out of memory"); onFail; } FlatJson &j = *j##_p

static bool body(FlatJson &j, char *err = nullptr, size_t en = 0) {
  const String &b = server.arg("plain");
  if (b.length() == 0) { return j.parse("{}", 2); }
  if (b.length() > 700) { if (err) snprintf(err, en, "body too large"); return false; }
  if (!j.parse(b.c_str(), b.length())) { if (err) snprintf(err, en, "invalid JSON"); return false; }
  return true;
}

static uint32_t epochNow() { time_t t = time(nullptr); return t > 1600000000L ? (uint32_t)t : 0; }

static void tzAbbr(char *out, size_t n, uint32_t utc) {
  const char *ab = ""; char buf[12];
  displayOffsetSec(utc, g_app.settings.tzMode, g_app.settings.localOffsetMin, &ab, buf);
  strlcpy(out, ab, n);
}

static const char *statusText(MarketStatus s) { return s == MS_OPEN ? "OPEN" : s == MS_CLOSED ? "CLOSED" : s == MS_PRE ? "PRE-MARKET" : "N/A"; }
static const char *quoteText(QuoteState q) { return q == Q_FRESH ? "fresh" : q == Q_STALE ? "stale" : q == Q_MISSING ? "missing" : "invalid"; }

// ---------------------------------------------------------------- pages
static void handleRoot() {
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", (PGM_P)WEB_INDEX_GZ, WEB_INDEX_GZ_LEN);
}

static void handleNotFound() {
  const String &uri = server.uri();
  if (uri.startsWith("/api/")) { sendErr(404, "not found"); return; }
  if (wifimgr::captivePortal()) {          // captive portal: every unknown URL (OS connectivity probes) lands on the Wi-Fi page
    String loc = String("http://") + wifimgr::apIpText() + "/#wifi";
    server.sendHeader("Location", loc);
    server.send(302, "text/plain", "");
    return;
  }
  server.send(404, "text/plain", "404 - not found");
}

// ---------------------------------------------------------------- read API
static void writeSummary(JsonWriter &w, const PortfolioSummary &s) {
  w.key("summary").beginObj();
  w.kNum("invested", s.invested).kNum("nav", s.nav).kNum("pl", s.pl).kNum("returnPct", s.retPct)
   .kNum("realisedPl", s.realisedPl).kNum("lifetimePl", s.lifetimePl)
   .kInt("positions", s.count).kInt("priced", s.priced).kInt("invalid", s.invalid).kBool("complete", s.complete).kBool("stale", s.stale);
  w.endObj();
}

static void apiStatus() {
  const AppState &a = g_app;
  uint32_t fsT, fsU; storage::fsInfo(fsT, fsU);
  Reply r(200); JsonWriter &w = r.w;
  w.beginObj().kStr("fw", FW_VERSION).kInt("uptime", millis() / 1000);
  w.key("time").beginObj().kInt("epoch", epochNow()).kBool("synced", a.clockSynced).endObj();
  w.key("heap").beginObj().kInt("free", ESP.getFreeHeap()).kInt("maxBlock", ESP.getMaxFreeBlockSize()).kInt("frag", ESP.getHeapFragmentation()).endObj();
  char id[10]; snprintf(id, sizeof id, "%06X", (unsigned)ESP.getChipId());
  w.key("chip").beginObj().kStr("id", id).kInt("cpuMHz", ESP.getCpuFreqMHz()).kInt("flashKB", ESP.getFlashChipRealSize() / 1024)
   .kInt("sketchKB", ESP.getSketchSize() / 1024).kInt("freeSketchKB", ESP.getFreeSketchSpace() / 1024).kStr("sdk", ESP.getSdkVersion()).endObj();
  {   // why the last boot happened; epc1 = crash PC (decode with addr2line against firmware.elf)
    const rst_info *ri = ESP.getResetInfoPtr();
    char epc[12], vaddr[12];
    snprintf(epc, sizeof epc, "0x%08X", (unsigned)ri->epc1);
    snprintf(vaddr, sizeof vaddr, "0x%08X", (unsigned)ri->excvaddr);
    w.key("reset").beginObj().kInt("reason", ri->reason).kInt("exccause", ri->exccause).kStr("epc1", epc).kStr("excvaddr", vaddr).endObj();
  }
  w.key("fs").beginObj().kBool("ok", storage::ok()).kInt("totalKB", fsT / 1024).kInt("usedKB", fsU / 1024).endObj();
  const NetInfo &n = a.net;
  char host[32]; snprintf(host, sizeof host, "%s.local", a.settings.hostname);
  w.key("wifi").beginObj().kStr("mode", n.mode == NetInfo::NET_AP_SETUP ? "ap-setup" : n.mode == NetInfo::NET_AP_RESCUE ? "ap-rescue" : "sta")
   .kBool("connected", n.connected).kStr("ssid", n.ssid).kStr("ip", n.ip).kInt("rssi", n.connected ? n.rssi : 0).kStr("mdns", host).endObj();
  w.key("data").beginObj().kInt("lastMarketOk", a.lastMarketOkEpoch).kStr("lastError", a.lastError).kInt("lastErrorEpoch", a.lastErrorEpoch)
   .kBool("quoteStale", a.quote.valid && a.quote.stale).kBool("portfolioStale", a.summary.stale)
   .kInt("effQuoteS", a.effQuoteS).kInt("effChartS", a.effChartS).endObj();
  w.key("ota").beginObj().kBool("active", ota::active()).endObj();
  w.endObj();
}

static void apiPortfolio() {
  const AppState &a = g_app;
  Reply r(200); JsonWriter &w = r.w;
  w.beginObj().kStr("currency", "THB").kInt("updated", a.portfolioEpoch);
  writeSummary(w, a.summary);
  w.endObj();
}

static void apiPositions() {
  const AppState &a = g_app;
  Reply r(200); JsonWriter &w = r.w;
  w.beginObj().kStr("currency", "THB");
  writeSummary(w, a.summary);
  w.key("positions").beginArr();
  for (uint8_t i = 0; i < a.posCount; i++) {
    const Position &p = a.positions[i]; const PositionResult &x = a.results[i];
    w.beginObj().kInt("i", i).kInt("id", p.id).kStr("symbol", p.symbol).kStr("date", p.date)
     .kNum("qty", p.qty, 6).kNum("oqty", p.oqty, 6).kNum("buy", p.buy, 6)
     .kNum("price", x.price).kNum("cost", x.cost).kNum("nav", x.nav).kNum("pl", x.pl).kNum("returnPct", x.retPct)
     .kStr("quote", quoteText(x.state)).kInt("quoteEpoch", x.quoteEpoch).endObj();
  }
  w.endArr().endObj();
}

static void apiMarket() {
  const AppState &a = g_app;
  const QuoteView &q = a.quote;
  Reply r(200); JsonWriter &w = r.w;
  w.beginObj().kStr("symbol", SP500_SYMBOL).kBool("valid", q.valid).kBool("stale", q.valid && q.stale);
  if (q.valid) {
    char txt[40], tz[12];
    formatQuoteTime(txt, sizeof txt, q.marketTime, a.settings.tzMode, a.settings.localOffsetMin);
    tzAbbr(tz, sizeof tz, q.marketTime);
    w.kNum("last", q.last).kNum("prevClose", q.prevClose).kNum("change", q.change).kNum("changePct", q.changePct)
     .kNum("open", q.open).kNum("high", q.high).kNum("low", q.low).kStr("status", statusText(q.status)).kStr("sessionDate", q.sessionDate)
     .kInt("quoteTime", q.marketTime).kStr("quoteTimeText", txt).kStr("timezone", tz).kStr("tzName", "America/New_York").kInt("fetchedEpoch", q.fetchedEpoch);
  } else {
    w.kNum("last", NAN).kNum("prevClose", NAN).kNum("change", NAN).kNum("changePct", NAN).kNum("open", NAN).kNum("high", NAN).kNum("low", NAN)
     .kStr("status", statusText(q.status)).kStr("sessionDate", "").kInt("quoteTime", 0).kStr("quoteTimeText", "N/A").kStr("timezone", "").kStr("tzName", "America/New_York").kInt("fetchedEpoch", 0);
  }
  w.endObj();
}

static void apiChart() {
  uint8_t range;
  if (!server.hasArg("range") || !parseRange(server.arg("range").c_str(), range)) { sendErr(400, "range must be 1d, 5d or 1mo"); return; }
  uint16_t maxPts = 0;
  if (server.hasArg("max")) { long m = server.arg("max").toInt(); if (m < 8) { sendErr(400, "max must be >= 8"); return; } maxPts = (uint16_t)(m > 400 ? 400 : m); }
  market::setViewRange(range);
  const Chart &c = g_app.charts[range];
  const ChartStats &s = g_app.chartStats[range];
  uint16_t n = c.valid ? c.n : 0;
  // optional min/max-preserving downsample into a small scratch buffer on the stack-free heap path
  uint32_t *dt = nullptr; float *dv = nullptr; uint16_t outN = n;
  const uint32_t *ot = c.t; const float *ov = c.v;
  if (maxPts && n > maxPts) {
    dt = (uint32_t *)malloc(maxPts * 4); dv = (float *)malloc(maxPts * 4);
    if (dt && dv) { outN = downsampleMinMax(c.t, c.v, n, dt, dv, maxPts); ot = dt; ov = dv; }
  }
  Reply r(200); JsonWriter &w = r.w;
  w.beginObj().kStr("range", rangeName(range)).kStr("interval", rangeInterval(range)).kBool("valid", c.valid && s.valid)
   .kBool("stale", g_app.chartStale[range]).kInt("updated", c.valid ? c.okEpoch : 0).kInt("count", outN);
  w.kNum("first", s.first).kNum("last", s.last).kNum("base", s.base).kNum("change", s.change).kNum("changePct", s.changePct).kNum("min", s.minV).kNum("max", s.maxV);
  w.key("t").beginArr(); for (uint16_t i = 0; i < outN; i++) w.integer(ot[i]); w.endArr();
  w.key("v").beginArr(); for (uint16_t i = 0; i < outN; i++) w.num(ov[i]); w.endArr();
  w.endObj();
  free(dt); free(dv);
}

static void apiSettingsGet() { Reply r(200); settingsWrite(r.w, g_app.settings, false, true); }

static void apiJobGet() {
  const market::AddJob &j = market::addJob();
  const char *st = j.state == market::JOB_PENDING ? "pending" : j.state == market::JOB_OK ? "ok" : j.state == market::JOB_ERROR ? "error" : "idle";
  Reply r(200);
  r.w.beginObj().kStr("state", st).kStr("error", j.error).kStr("symbol", j.symbol).kBool("updated", j.updated).endObj();
  if (j.state == market::JOB_OK || j.state == market::JOB_ERROR) market::consumeAddJob();
}

// ---------------------------------------------------------------- sale API (v1.1)
// Transaction management (sell / undo / history / export / realised) needs the admin password:
// these calls change (or expose) money, unlike the read-only market endpoints above.
// Every literal here goes to flash (S_LIT): this build is tight on RAM and these strings are ~1.2 KB.
static const char *saleStatusStr(uint8_t s) {
  return s == SALE_ST_REVERSED ? "reversed" : s == REC_REVERSAL ? "reversal" : "valid";
}

static void writeSaleLite(JsonWriter &w, const SaleLite &l) {
  w.beginObj()
   .kInt("seq", l.seq).kInt("ts", l.ts).kStr("id", l.id).kStr("refId", l.refId)
   .kInt("type", l.type).kStr("status", saleStatusStr(l.status))
   .kStr("symbol", l.symbol).kStr("date", l.date)
   .kNum("qty", l.qty, 8).kNum("price", l.price, 6).kNum("refPrice", l.refPrice, 6)
   .kStr("priceSource", l.priceSource == PRICE_YAHOO ? "yahoo" : "manual")
   .kNum("fee", l.fee, 4).kNum("gross", l.gross, 6).kNum("net", l.net, 6)
   .kNum("cost", l.cost, 6).kNum("realised", l.realised, 6)
   .endObj();
}

// {"symbol","date","method","mode","lotId","amount","qty","price","fee","priceSource","token"}
static bool parseSaleReq(FlatJson &j, SaleRequest &r, char *err, size_t errN) {
  memset(&r, 0, sizeof r);
  r.price = r.refPrice = NAN;
  if (!j.isStr("symbol") || !j.str("symbol")[0]) { snprintf(err, errN, "symbol required"); return false; }
  strlcpy(r.symbol, j.str("symbol"), sizeof r.symbol);
  if (!j.isStr("date") || strlen(j.str("date")) != 10) { snprintf(err, errN, "date required (YYYY-MM-DD)"); return false; }
  strlcpy(r.date, j.str("date"), sizeof r.date);
  const char *method = j.str("method", "fifo");
  r.method = !strcmp(method, "specific") ? SALE_SPECIFIC : !strcmp(method, "fifo") ? SALE_FIFO : 0xFF;
  if (r.method == 0xFF) { snprintf(err, errN, "method must be fifo or specific"); return false; }
  const char *mode = j.str("mode", "qty");
  r.mode = !strcmp(mode, "thb") ? SALE_BY_THB : !strcmp(mode, "all") ? SALE_BY_ALL
         : !strcmp(mode, "qty") ? SALE_BY_QTY : 0xFF;
  if (r.mode == 0xFF) { snprintf(err, errN, "mode must be thb, qty or all"); return false; }
  double v;
  if (j.num("lotId", v, true)) { if (v < 0 || v > 65535) { snprintf(err, errN, "bad lotId"); return false; } r.lotId = (uint16_t)v; }
  if (j.num("amount", v)) r.amount = v;
  if (j.num("qty", v)) r.qty = v;
  if (j.num("price", v)) r.price = v;
  if (j.num("refPrice", v)) r.refPrice = v;
  if (j.num("fee", v)) r.fee = v;
  if (j.num("token", v)) r.token = (uint32_t)v;
  const char *ps = j.str("priceSource", "manual");
  r.priceSource = !strcmp(ps, "yahoo") ? PRICE_YAHOO : PRICE_MANUAL;
  return true;
}

static void writeSalePreview(JsonWriter &w, const SalePreview &pv) {
  w.key("preview").beginObj();
  w.kStr("symbol", pv.symbol).kStr("date", pv.date)
   .kNum("price", pv.price).kNum("refPrice", pv.refPrice)
   .kStr("priceSource", pv.priceSource == PRICE_YAHOO ? "yahoo" : "manual")
   .kNum("qty", pv.qty, 8).kNum("available", pv.available, 8).kNum("fee", pv.fee, 4)
   .kNum("gross", pv.gross, 6).kNum("net", pv.net, 6).kNum("cost", pv.cost, 6).kNum("realised", pv.realised, 6)
   .kInt("token", pv.token);
  w.key("alloc").beginArr();
  for (uint8_t i = 0; i < pv.nAlloc; i++) {
    const LotAlloc &a = pv.alloc[i];
    w.beginObj().kInt("lot", a.lot).kStr("date", a.date).kNum("buy", a.buy, 6)
     .kNum("oqty", a.oqty, 8).kNum("before", a.before, 8).kNum("after", a.after, 8).endObj();
  }
  w.endArr().endObj();
}

static void apiRealised() {
  if (!requireAuth()) return;
  const AppState &a = g_app;
  const RealisedSummary &s = a.realised;
  Reply r(200); JsonWriter &w = r.w;
  w.beginObj()
   .kBool("valid", s.valid)
   .kNum("pl", s.valid ? s.pl : NAN)
   .kNum("lifetimePl", a.summary.lifetimePl)
   .kInt("count", s.count).kInt("reversed", s.reversed).kInt("records", s.records)
   .kInt("limit", LEDGER_LIMIT).kBool("warn", s.warn).kBool("full", s.full)
   .kBool("recovery", s.recovery).kBool("badLine", s.badLine)
   .kBool("hasLast", s.hasLast).kStr("lastId", s.lastId).kStr("lastSymbol", s.lastSymbol)
   .kStr("lastDate", s.lastDate).kNum("lastRealised", s.lastRealised).kInt("lastTs", s.lastTs);
  w.key("years").beginArr();
  for (uint8_t i = 0; i < s.nYears; i++) w.beginObj().kInt("year", s.year[i]).kNum("pl", s.yearPl[i]).endObj();
  w.endArr().endObj();
}

static void apiSales() {
  if (!requireAuth()) return;
  uint16_t offset = 0, limit = SALES_PAGE_MAX;
  if (server.hasArg("offset")) { long v = server.arg("offset").toInt(); if (v < 0) v = 0; offset = (uint16_t)(v > 65535 ? 65535 : v); }
  if (server.hasArg("limit")) { long v = server.arg("limit").toInt(); if (v < 1) v = 1; limit = (uint16_t)(v > SALES_PAGE_MAX ? SALES_PAGE_MAX : v); }
  char symbol[SYMBOL_LEN] = "";
  if (server.hasArg("symbol")) strlcpy(symbol, server.arg("symbol").c_str(), sizeof symbol);
  std::unique_ptr<storage::SalePage> pagep(new (std::nothrow) storage::SalePage);   // ~1 KB: heap, not the request stack
  if (!pagep) { sendErr(500, "out of memory"); return; }
  storage::SalePage &page = *pagep;
  char err[64] = "";
  if (!storage::salePage(g_app, symbol, offset, limit, page, err, sizeof err)) { sendErr(500, err); return; }
  Reply r(200); JsonWriter &w = r.w;
  w.beginObj().kStr("symbol", symbol).kInt("total", page.total).kInt("offset", offset)
   .kInt("count", page.count).kInt("limit", limit)
   .kInt("records", g_app.realised.records).kInt("limitRecords", LEDGER_LIMIT)
   .kBool("warn", g_app.realised.warn).kBool("full", g_app.realised.full)
   .kNum("pl", g_app.realised.valid ? g_app.realised.pl : NAN);
  w.key("sales").beginArr();
  for (uint16_t i = 0; i < page.count; i++) writeSaleLite(w, page.rows[i]);
  w.endArr().endObj();
}

static void apiSalesExport() {
  if (!requireAuth()) return;
  char symbol[SYMBOL_LEN] = "";
  if (server.hasArg("symbol")) strlcpy(symbol, server.arg("symbol").c_str(), sizeof symbol);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Content-Disposition", "attachment; filename=\"sells.csv\"");
  if (!server.chunkedResponseModeStart(200, "text/csv")) { sendErr(500, "chunked not supported"); return; }
  char err[64] = "";
  if (!storage::saleCsv(g_app, symbol, sinkChunk, nullptr, err, sizeof err)) Serial.printf("[csv] %s\n", err);
  server.chunkedResponseFinalize();
}

static void apiSaleJob() {
  if (!requireAuth()) return;
  const market::SalePriceJob &j = market::salePriceJob();
  const char *st = j.state == market::JOB_PENDING ? "pending" : j.state == market::JOB_OK ? "ok"
                 : j.state == market::JOB_ERROR ? "error" : "idle";
  Reply r(200); JsonWriter &w = r.w;
  w.beginObj().kStr("state", st).kStr("symbol", j.symbol).kStr("date", j.date)
   .kNum("price", j.state == market::JOB_OK ? j.price : NAN).kStr("error", j.error).endObj();
}

// Input -> Calculate Preview. Without an execution price the historical close is fetched first
// and the reply is 202 {state:"pending"}; the client re-posts the same body once the job is ok.
static void postSalePreview() {
  if (!requireAuth()) return;
  HEAP_JSON(j, return);
  char err[72] = "";
  if (!body(j, err, sizeof err)) { sendErr(400, err); return; }
  SaleRequest req;
  if (!parseSaleReq(j, req, err, sizeof err)) { sendErr(400, err); return; }
  if (!(isNum(req.price) && req.price > 0)) {
    const market::SalePriceJob &job = market::salePriceJob();
    bool same = job.state == market::JOB_OK && !strcmp(job.symbol, req.symbol) && !strcmp(job.date, req.date);
    if (!same) {
      if (!market::requestSalePrice(req.symbol, req.date)) { sendErr(409, "a price lookup is already running"); return; }
      const market::SalePriceJob &n = market::salePriceJob();
      if (n.state == market::JOB_ERROR) { char m[48]; strlcpy(m, n.error, sizeof m); market::consumeSalePriceJob(); sendErr(400, m); return; }
      sendJson(202, "{\"ok\":true,\"state\":\"pending\"}");
      return;
    }
    req.price = job.price;
    req.priceSource = PRICE_YAHOO;
  }
  SalePreview pv;
  if (!storage::salePreview(g_app, req, pv)) { sendErr(400, pv.error[0] ? pv.error : "invalid sale"); return; }
  Reply r(200); JsonWriter &w = r.w;
  w.beginObj().kBool("ok", true).kStr("state", "ready");
  writeSalePreview(w, pv);
  w.endObj();
}

// Confirm -> Commit. The token proves the preview was computed from the portfolio as it is now.
static void postSaleConfirm() {
  if (!requireAuth()) return;
  char err[80] = "";
  int code = 400;
  SaleRequest req;
  {
    // FlatJson is 1.3 KB: parse on the heap and drop it before the commit goes deep into LittleFS
    std::unique_ptr<FlatJson> jp(new (std::nothrow) FlatJson);
    if (!jp) { sendErr(500, "out of memory"); return; }
    if (!body(*jp, err, sizeof err)) { sendErr(400, err); return; }
    if (!parseSaleReq(*jp, req, err, sizeof err)) { sendErr(400, err); return; }
  }
  if (!req.token) { sendErr(400, "preview token required"); return; }
  SalePreview *pvp = (SalePreview *)calloc(1, sizeof(SalePreview));   // ~780 B: heap, the handler stack is small
  if (!pvp) { sendErr(500, "out of memory"); return; }
  SalePreview &pv = *pvp;
  if (!storage::saleCommit(g_app, req, pv, err, sizeof err, &code)) { free(pvp); sendErr(code, err); return; }
  market::positionsChanged();
  char txId[16];
  snprintf(txId, sizeof txId, "TX-%06lu", (unsigned long)(g_app.pmeta.nextTxn ? g_app.pmeta.nextTxn - 1 : 0));
  Reply r(200); JsonWriter &w = r.w;
  w.beginObj().kBool("ok", true).kStr("state", "committed").kStr("id", txId).kInt("seq", g_app.pmeta.appliedSeq);
  writeSalePreview(w, pv);
  w.endObj();
  free(pvp);
}

static void postSaleReverse() {
  if (!requireAuth()) return;
  char err[80] = "";
  int code = 400;
  double seq = 0;
  {
    std::unique_ptr<FlatJson> jp(new (std::nothrow) FlatJson);
    if (!jp) { sendErr(500, "out of memory"); return; }
    if (!body(*jp, err, sizeof err)) { sendErr(400, err); return; }
    if (!jp->num("seq", seq, true) || seq < 0 || seq > 4294967295.0) { sendErr(400, "seq required"); return; }
  }
  if (!storage::saleReverse(g_app, (uint32_t)seq, err, sizeof err, &code)) { sendErr(code, err); return; }
  market::positionsChanged();
  sendOk();
}

// Permanently removes a sale and its reversal from the history (an active sale is reversed first).
static void postSaleDelete() {
  if (!requireAuth()) return;
  char err[80] = "";
  int code = 400;
  double seq = 0;
  {
    HEAP_JSON(j, return);
    if (!body(j, err, sizeof err)) { sendErr(400, err); return; }
    if (!j.num("seq", seq, true) || seq < 1 || seq > 4294967295.0) { sendErr(400, "seq required"); return; }
  }
  if (!storage::saleDelete(g_app, (uint32_t)seq, err, sizeof err, &code)) { market::positionsChanged(); sendErr(code, err); return; }
  market::positionsChanged();
  sendOk();
}

// ---------------------------------------------------------------- Wi-Fi API
static void apiWifiStatus() {
  const NetInfo &n = g_app.net;
  const char *state = n.connected ? "connected" : wifimgr::connState() == wifimgr::CONN_CONNECTING || n.connecting ? "connecting" : "disconnected";
  const char *cs[] = {"idle", "connecting", "ok", "failed"};
  const char *ss[] = {"idle", "scanning", "done"};
  uint8_t cnt; wifimgr::scanResults(cnt);
  uint32_t age = wifimgr::scanAgeSec();
  char host[32]; snprintf(host, sizeof host, "%s.local", g_app.settings.hostname);
  Reply r(200); JsonWriter &w = r.w;
  w.beginObj().kStr("mode", n.mode == NetInfo::NET_AP_SETUP ? "ap-setup" : n.mode == NetInfo::NET_AP_RESCUE ? "ap-rescue" : "sta")
   .kStr("state", state).kStr("ssid", n.ssid).kStr("ip", n.ip).kInt("rssi", n.connected ? n.rssi : 0).kStr("mdns", host)
   .kBool("saved", wifimgr::hasSavedNetwork()).kStr("apSsid", n.apSsid).kStr("apIp", n.apIp);
  w.key("connect").beginObj().kStr("state", cs[wifimgr::connState()]).kStr("ssid", wifimgr::connSsid()).kStr("error", wifimgr::connError()).endObj();
  w.key("scan").beginObj().kStr("state", ss[wifimgr::scanState()]).kInt("ageSec", age == 0xFFFFFFFFu ? -1 : (int64_t)age).kInt("count", cnt).endObj();
  w.endObj();
}

static void apiWifiScan() {
  bool refresh = server.hasArg("refresh") && server.arg("refresh") == "1";
  wifimgr::startScan(refresh || wifimgr::scanAgeSec() == 0xFFFFFFFFu);
  uint8_t n; const wifimgr::ScanEntry *e = wifimgr::scanResults(n);
  uint32_t age = wifimgr::scanAgeSec();
  Reply r(200); JsonWriter &w = r.w;
  w.beginObj().kBool("scanning", wifimgr::scanState() == wifimgr::SCAN_RUNNING).kInt("ageSec", age == 0xFFFFFFFFu ? -1 : (int64_t)age);
  w.key("networks").beginArr();
  for (uint8_t i = 0; i < n; i++) w.beginObj().kStr("ssid", e[i].ssid).kInt("rssi", e[i].rssi).kBool("enc", e[i].enc).kInt("ch", e[i].channel).endObj();
  w.endArr().endObj();
}

// ---------------------------------------------------------------- write API
static void postWifiConnect() {
  if (!requireAuth()) return;
  HEAP_JSON(j, return); char err[40] = "";
  if (!body(j, err, sizeof err) || !j.isStr("ssid")) { sendErr(400, err[0] ? err : "ssid required"); return; }
  const char *ssid = j.str("ssid"), *pass = j.str("password");
  size_t sl = strlen(ssid), pl = strlen(pass);
  if (sl < 1 || sl > 32) { sendErr(400, "ssid must be 1..32 characters"); return; }
  if (pl > 64 || (pl > 0 && pl < 8)) { sendErr(400, "password must be 8..64 characters (or empty for an open network)"); return; }
  if (!wifimgr::requestConnect(ssid, pass)) { sendErr(409, "a connection attempt is already running"); return; }
  sendJson(202, "{\"ok\":true,\"state\":\"connecting\"}");
}

static void postPositions() {
  if (!requireAuth()) return;
  HEAP_JSON(j, return); char err[48] = "";
  if (!body(j, err, sizeof err)) { sendErr(400, err); return; }
  if (!j.isStr("symbol") || !j.isStr("date")) { sendErr(400, "symbol and date are required"); return; }
  const char *sym = j.str("symbol"), *date = j.str("date");
  double thb, qty, buy;
  if (j.num("thb", thb)) {
    if (market::addJob().state == market::JOB_PENDING) { sendErr(409, "another add is in progress"); return; }
    market::requestAddByThb(sym, date, thb);
    const market::AddJob &a = market::addJob();
    if (a.state == market::JOB_ERROR) { char m[48]; strlcpy(m, a.error, sizeof m); market::consumeAddJob(); sendErr(400, m); return; }
    sendJson(202, "{\"ok\":true,\"state\":\"pending\"}");
    return;
  }
  if (j.num("qty", qty) && j.num("buy", buy)) {
    AddResult r = portfolioAdd(g_app.positions, g_app.posCount, g_app.pmeta, sym, date, qty, buy, epochNow());
    if (r != ADD_OK && r != ADD_UPDATED) { sendErr(400, addResultText(r)); return; }
    bool saved = storage::savePortfolio(g_app);
    market::positionsChanged();
    if (!saved) { sendErr(500, "added, but saving to flash failed"); return; }
    sendJson(200, "{\"ok\":true,\"state\":\"ok\"}");
    return;
  }
  sendErr(400, "provide thb, or qty and buy");
}

static void postPositionsDelete() {
  if (!requireAuth()) return;
  HEAP_JSON(j, return); double idx; char err[40] = "";
  if (!body(j, err, sizeof err)) { sendErr(400, err); return; }
  if (!j.num("index", idx, true) || idx < 0 || idx > 255 || !portfolioDelete(g_app.positions, g_app.posCount, g_app.pmeta, (uint8_t)idx)) { sendErr(400, "bad index"); return; }
  bool saved = storage::savePortfolio(g_app);
  market::positionsChanged();
  if (!saved) { sendErr(500, "deleted, but saving to flash failed"); return; }
  sendOk();
}

static void postSettings() {
  if (!requireAuth()) return;
  HEAP_JSON(j, return); char err[64] = "";
  if (!body(j, err, sizeof err)) { sendErr(400, err); return; }
  Settings n = g_app.settings; bool hc = false;
  if (!settingsApply(n, j, err, sizeof err, &hc)) { sendErr(400, err); return; }
  Settings old = g_app.settings;
  g_app.settings = n;
  if (!storage::saveSettings(n)) { g_app.settings = old; sendErr(500, "saving to flash failed"); return; }
  g_app.effQuoteS = n.quoteRefreshS; g_app.effChartS = n.chartRefreshS; g_app.effPortfolioS = n.portfolioRefreshS;
  if (old.backlight != n.backlight) display::setBacklight(n.backlight);   // dim applies at once, no restart
  Reply r(200); JsonWriter &w = r.w;
  w.beginObj().kBool("ok", true).kBool("restart", hc || strcmp(old.adminPass, n.adminPass) != 0);
  w.key("settings"); settingsWrite(w, n, false, true);
  w.endObj();
}

static void postRefresh() {
  if (!requireAuth()) return;
  HEAP_JSON(j, return); body(j);
  const char *what = j.str("what", "all");
  market::Refresh r = market::REF_ALL;
  if (!strcmp(what, "quote")) r = market::REF_QUOTE; else if (!strcmp(what, "charts")) r = market::REF_CHARTS; else if (!strcmp(what, "portfolio")) r = market::REF_PORTFOLIO;
  else if (strcmp(what, "all")) { sendErr(400, "what must be all, quote, charts or portfolio"); return; }
  market::requestRefresh(r);
  sendOk(202);
}

static void postCacheClear() { if (!requireAuth()) return; market::clearCache(); sendOk(); }
static void postWifiReconnect() { if (!requireAuth()) return; wifimgr::requestReconnect(); sendOk(); }

static bool confirmed(const char *word) {
  HEAP_JSON(j, return false); if (!body(j)) return false;
  return j.isStr("confirm") && !strcmp(j.str("confirm"), word);
}

static void postWifiForget() {
  if (!requireAuth()) return;
  if (!confirmed("FORGET")) { sendErr(400, "confirm must be FORGET"); return; }
  if (!wifimgr::forget()) { sendErr(409, "a connection attempt is running"); return; }
  sendOk();
}

static void postRestart() { if (!requireAuth()) return; sendOk(); ota::scheduleRestart(1000); }

static void postResetSettings() {
  if (!requireAuth()) return;
  if (!confirmed("RESET")) { sendErr(400, "confirm must be RESET"); return; }
  storage::eraseAll(true, true);          // settings + cache go; Wi-Fi and portfolio stay
  sendOk();
  ota::scheduleRestart(1000);
}

static void postFactoryReset() {
  if (!requireAuth()) return;
  if (!confirmed("FACTORY")) { sendErr(400, "confirm must be FACTORY"); return; }
  storage::eraseAll(false, false);
  sendOk();
  ota::scheduleRestart(1000);
}

static void handleUpdateDone() { ota::webDone(server, authorized()); }
static void handleUpdateUpload() { ota::webUpload(server, authorized()); }

static void handleOptions() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.send(204, "text/plain", "");
}

void begin() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/index.html", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, apiStatus);
  server.on("/api/portfolio", HTTP_GET, apiPortfolio);
  server.on("/api/positions", HTTP_GET, apiPositions);
  server.on("/api/positions/job", HTTP_GET, apiJobGet);
  server.on("/api/realised", HTTP_GET, apiRealised);
  server.on("/api/sales", HTTP_GET, apiSales);
  server.on("/api/sales/export", HTTP_GET, apiSalesExport);
  server.on("/api/sales/job", HTTP_GET, apiSaleJob);
  server.on("/api/sales/preview", HTTP_POST, postSalePreview);
  server.on("/api/sales/confirm", HTTP_POST, postSaleConfirm);
  server.on("/api/sales/reverse", HTTP_POST, postSaleReverse);
  server.on("/api/sales/delete", HTTP_POST, postSaleDelete);
  server.on("/api/market", HTTP_GET, apiMarket);
  server.on("/api/chart", HTTP_GET, apiChart);
  server.on("/api/wifi/status", HTTP_GET, apiWifiStatus);
  server.on("/api/wifi/scan", HTTP_GET, apiWifiScan);
  server.on("/api/settings", HTTP_GET, apiSettingsGet);
  server.on("/api/settings", HTTP_POST, postSettings);
  server.on("/api/positions", HTTP_POST, postPositions);
  server.on("/api/positions/delete", HTTP_POST, postPositionsDelete);
  server.on("/api/refresh", HTTP_POST, postRefresh);
  server.on("/api/cache/clear", HTTP_POST, postCacheClear);
  server.on("/api/wifi/connect", HTTP_POST, postWifiConnect);
  server.on("/api/wifi/reconnect", HTTP_POST, postWifiReconnect);
  server.on("/api/wifi/forget", HTTP_POST, postWifiForget);
  server.on("/api/system/restart", HTTP_POST, postRestart);
  server.on("/api/system/reset-settings", HTTP_POST, postResetSettings);
  server.on("/api/system/factory-reset", HTTP_POST, postFactoryReset);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) { handleOptions(); return; }
    handleNotFound();
  });
  server.begin();
}

void loop() { server.handleClient(); }

}  // namespace webserver
