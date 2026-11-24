#include "WebServerManager.h"
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <time.h>
#include "WiFiManager.h"
#include "OTAManager.h"
#include "../core/AppState.h"
#include "../core/JsonWriter.h"
#include "../core/FlatJson.h"
#include "../core/Format.h"
#include "../core/MarketClock.h"
#include "../config/AppConfig.h"
#include "../finance/ChartData.h"
#include "../finance/MarketData.h"
#include "../finance/Portfolio.h"
#include "../storage/StorageManager.h"
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
    w.beginObj().kInt("i", i).kStr("symbol", p.symbol).kStr("date", p.date).kNum("qty", p.qty, 6).kNum("buy", p.buy, 6)
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
  FlatJson j; char err[40] = "";
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
  FlatJson j; char err[48] = "";
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
    AddResult r = portfolioAdd(g_app.positions, g_app.posCount, sym, date, qty, buy, epochNow());
    if (r != ADD_OK && r != ADD_UPDATED) { sendErr(400, addResultText(r)); return; }
    bool saved = storage::savePortfolio(g_app.positions, g_app.posCount);
    market::positionsChanged();
    if (!saved) { sendErr(500, "added, but saving to flash failed"); return; }
    sendJson(200, "{\"ok\":true,\"state\":\"ok\"}");
    return;
  }
  sendErr(400, "provide thb, or qty and buy");
}

static void postPositionsDelete() {
  if (!requireAuth()) return;
  FlatJson j; double idx; char err[40] = "";
  if (!body(j, err, sizeof err)) { sendErr(400, err); return; }
  if (!j.num("index", idx, true) || idx < 0 || idx > 255 || !portfolioDelete(g_app.positions, g_app.posCount, (uint8_t)idx)) { sendErr(400, "bad index"); return; }
  bool saved = storage::savePortfolio(g_app.positions, g_app.posCount);
  market::positionsChanged();
  if (!saved) { sendErr(500, "deleted, but saving to flash failed"); return; }
  sendOk();
}

static void postSettings() {
  if (!requireAuth()) return;
  FlatJson j; char err[64] = "";
  if (!body(j, err, sizeof err)) { sendErr(400, err); return; }
  Settings n = g_app.settings; bool hc = false;
  if (!settingsApply(n, j, err, sizeof err, &hc)) { sendErr(400, err); return; }
  Settings old = g_app.settings;
  g_app.settings = n;
  if (!storage::saveSettings(n)) { g_app.settings = old; sendErr(500, "saving to flash failed"); return; }
  g_app.effQuoteS = n.quoteRefreshS; g_app.effChartS = n.chartRefreshS; g_app.effPortfolioS = n.portfolioRefreshS;
  Reply r(200); JsonWriter &w = r.w;
  w.beginObj().kBool("ok", true).kBool("restart", hc || strcmp(old.adminPass, n.adminPass) != 0);
  w.key("settings"); settingsWrite(w, n, false, true);
  w.endObj();
}

static void postRefresh() {
  if (!requireAuth()) return;
  FlatJson j; body(j);
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
  FlatJson j; if (!body(j)) return false;
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
