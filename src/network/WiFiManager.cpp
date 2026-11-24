#include "WiFiManager.h"
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <time.h>
#include "../storage/StorageManager.h"

namespace wifimgr {

static const uint32_t FIRST_CONNECT_MS = 20000;
static const uint32_t RETRY_MS         = 15000;
static const uint32_t AP_FALLBACK_MS   = 180000;   // saved network down this long -> rescue AP
static const uint32_t AP_STOP_STABLE_MS = 20000;   // STA must be up this long (and AP empty) before the AP stops
static const uint32_t AP_STOP_FORCE_MS = 300000;
static const uint32_t CONNECT_TIMEOUT_MS = 20000;
static const uint32_t SCAN_TTL_MS = 30000;
static const uint8_t  AP_PASS_LEN = 10;

enum ApMode : uint8_t { AP_NONE = 0, AP_SETUP, AP_RESCUE };

static char s_host[24] = "sp500";
static storage::WifiCreds s_creds;           // saved network
static bool s_haveCreds = false;
static ApMode s_ap = AP_NONE;
static DNSServer s_dns;
static char s_apSsid[32] = "";
static char s_apPass[AP_PASS_LEN + 1] = "";
static char s_apIp[16] = "";

static bool s_staWasUp = false;
static uint32_t s_staUpSince = 0, s_downSince = 0, s_lastRetry = 0, s_bootMs = 0;
static bool s_downTracked = false;
static bool s_mdnsUp = false, s_mdnsRestart = false;
static uint32_t s_mdnsAnnounce = 0;
static bool s_ntpStarted = false;

// connect job
static ConnState s_conn = CONN_IDLE;
static char s_connSsid[33] = "";
static char s_connPass[65] = "";
static char s_connErr[40] = "";
static uint32_t s_connStart = 0;
static uint8_t s_connFailStatus = 0;

// scan
static ScanState s_scan = SCAN_IDLE;
static ScanEntry s_res[SCAN_MAX];
static uint8_t s_resN = 0;
static uint32_t s_scanDoneMs = 0;
static bool s_scanEver = false, s_scanWanted = false;

static void startAp(bool secured) {
  uint32_t id = ESP.getChipId() & 0xFFFFFF;
  snprintf(s_apSsid, sizeof s_apSsid, "sp500-setup-%06X", (unsigned)id);
  s_apPass[0] = 0;
  if (secured) {
    static const char ALPHA[] = "abcdefghjkmnpqrstuvwxyz23456789";   // no 0/o/1/l/i
    for (int i = 0; i < AP_PASS_LEN; i++) s_apPass[i] = ALPHA[(RANDOM_REG32 ^ micros()) % (sizeof(ALPHA) - 1)];
    s_apPass[AP_PASS_LEN] = 0;
  }
  WiFi.mode(WIFI_AP_STA);
  if (secured) { WiFi.softAP(s_apSsid, s_apPass); WiFi.setAutoReconnect(false); }   // our own retries: SDK retries hop channel and drop the phone
  else WiFi.softAP(s_apSsid);
  s_ap = secured ? AP_RESCUE : AP_SETUP;
  strlcpy(s_apIp, WiFi.softAPIP().toString().c_str(), sizeof s_apIp);
  s_dns.setErrorReplyCode(DNSReplyCode::NoError);
  s_dns.start(53, "*", WiFi.softAPIP());
  Serial.printf("[wifi] %s AP \"%s\"%s%s -> http://%s/\n", secured ? "rescue" : "setup", s_apSsid, secured ? " key=" : "", s_apPass, s_apIp);
}

static void stopAp(const char *why) {
  s_dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setAutoReconnect(true);
  s_ap = AP_NONE;
  s_apPass[0] = 0; s_apSsid[0] = 0; s_apIp[0] = 0;
  s_mdnsRestart = true;
  Serial.printf("[wifi] AP stopped (%s)\n", why);
}

static void staBegin(const char *ssid, const char *pass) {
  WiFi.hostname(s_host);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.begin(ssid, pass);
}

static void startMdns() {
  if (s_mdnsUp) { MDNS.end(); s_mdnsUp = false; }
  if (MDNS.begin(s_host)) {
    MDNS.addService("http", "tcp", 80);
    MDNS.enableArduino(8266, g_app.settings.adminPass[0] != 0);
    s_mdnsUp = true;
    Serial.printf("[wifi] mDNS http://%s.local/\n", s_host);
  } else Serial.println(F("[wifi] mDNS begin failed"));
}

static void onConnected() {
  s_staWasUp = true;
  s_staUpSince = millis();
  s_downTracked = false;
  Serial.printf("[wifi] connected ip=%s rssi=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  // Clock stays UTC: market time is derived by MarketClock (America/New_York rules), never from a device TZ.
  configTime("UTC0", "pool.ntp.org", "time.nist.gov");
  s_ntpStarted = true;
  if (!s_mdnsUp) startMdns();
}

void begin(const char *hostname) {
  strlcpy(s_host, hostname, sizeof s_host);
  s_bootMs = millis();
  WiFi.persistent(false);                 // credentials live in /wifi.json, not in the SDK flash area
  s_haveCreds = storage::loadWifi(s_creds);
  if (!s_haveCreds) {
    startAp(false);
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  staBegin(s_creds.ssid, s_creds.pass);
  s_downTracked = true; s_downSince = millis(); s_lastRetry = millis();
  Serial.printf("[wifi] connecting to \"%s\"\n", s_creds.ssid);
}

void applyHostname(const char *h) { strlcpy(s_host, h, sizeof s_host); }

bool requestConnect(const char *ssid, const char *pass) {
  if (s_conn == CONN_CONNECTING) return false;
  size_t sl = strlen(ssid), pl = strlen(pass);
  if (sl < 1 || sl > 32 || pl > 64 || (pl > 0 && pl < 8)) return false;
  strcpy(s_connSsid, ssid); strcpy(s_connPass, pass);
  s_connErr[0] = 0; s_connFailStatus = 0;
  s_conn = CONN_CONNECTING;
  s_connStart = millis();
  s_scanWanted = false;
  if (s_scan == SCAN_RUNNING) { WiFi.scanDelete(); s_scan = SCAN_IDLE; }
  if (s_ap == AP_NONE) WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  staBegin(ssid, pass);
  Serial.printf("[wifi] connect request -> \"%s\"\n", ssid);
  return true;
}

void requestReconnect() {
  if (s_conn == CONN_CONNECTING || !s_haveCreds) return;
  WiFi.disconnect();
  staBegin(s_creds.ssid, s_creds.pass);
  s_lastRetry = millis();
}

bool forget() {
  if (s_conn == CONN_CONNECTING) return false;
  bool ok = storage::eraseWifi();
  s_haveCreds = false;
  memset(&s_creds, 0, sizeof s_creds);
  WiFi.disconnect(true);
  s_staWasUp = false;
  if (s_ap == AP_RESCUE) stopAp("forget");
  if (s_ap == AP_NONE) startAp(false);
  return ok;
}

// ---- scan ----
void startScan(bool force) {
  if (s_scan == SCAN_RUNNING || s_conn == CONN_CONNECTING) return;
  if (!force && s_scanEver && (uint32_t)(millis() - s_scanDoneMs) < SCAN_TTL_MS) return;
  if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
  WiFi.scanNetworks(true, false);        // async, visible networks only
  s_scan = SCAN_RUNNING;
}

static void collectScan(int n) {
  s_resN = 0;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0 || s.length() > 32) continue;
    int8_t rssi = (int8_t)WiFi.RSSI(i);
    int dup = -1;
    for (uint8_t j = 0; j < s_resN; j++) if (s.equals(s_res[j].ssid)) { dup = j; break; }
    if (dup >= 0) { if (rssi > s_res[dup].rssi) { s_res[dup].rssi = rssi; s_res[dup].channel = (uint8_t)WiFi.channel(i); } continue; }
    if (s_resN < SCAN_MAX) { ScanEntry &e = s_res[s_resN++]; strcpy(e.ssid, s.c_str()); e.rssi = rssi; e.channel = (uint8_t)WiFi.channel(i); e.enc = WiFi.encryptionType(i) != ENC_TYPE_NONE; }
    else {   // table full: replace the weakest if this one is stronger
      uint8_t w = 0; for (uint8_t j = 1; j < SCAN_MAX; j++) if (s_res[j].rssi < s_res[w].rssi) w = j;
      if (rssi > s_res[w].rssi) { ScanEntry &e = s_res[w]; strcpy(e.ssid, s.c_str()); e.rssi = rssi; e.channel = (uint8_t)WiFi.channel(i); e.enc = WiFi.encryptionType(i) != ENC_TYPE_NONE; }
    }
  }
  // strongest first (insertion sort, <=16 entries)
  for (uint8_t i = 1; i < s_resN; i++) { ScanEntry k = s_res[i]; int j = i - 1; while (j >= 0 && s_res[j].rssi < k.rssi) { s_res[j + 1] = s_res[j]; j--; } s_res[j + 1] = k; }
}

ScanState scanState() { return s_scan; }
uint32_t scanAgeSec() { return s_scanEver ? (millis() - s_scanDoneMs) / 1000 : 0xFFFFFFFFu; }
const ScanEntry *scanResults(uint8_t &n) { n = s_resN; return s_res; }

static void pollScan() {
  if (s_scan != SCAN_RUNNING) return;
  int c = WiFi.scanComplete();
  if (c == WIFI_SCAN_RUNNING) return;
  if (c >= 0) collectScan(c);
  WiFi.scanDelete();
  s_scan = SCAN_DONE;
  s_scanEver = true;
  s_scanDoneMs = millis();
}

// ---- connect job ----
static void pollConnect() {
  if (s_conn != CONN_CONNECTING) return;
  wl_status_t st = WiFi.status();
  if (st == WL_CONNECTED) {
    bool saved = storage::saveWifi(s_connSsid, s_connPass);
    strcpy(s_creds.ssid, s_connSsid); strcpy(s_creds.pass, s_connPass); s_haveCreds = true;
    s_conn = CONN_OK;
    s_connErr[0] = 0;
    if (!saved) strlcpy(s_connErr, "connected, but saving failed", sizeof s_connErr);
    s_staWasUp = false;      // re-run onConnected (new network)
    Serial.printf("[wifi] connect OK ip=%s\n", WiFi.localIP().toString().c_str());
    return;
  }
  if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) s_connFailStatus = st;
  uint32_t el = millis() - s_connStart;
  bool failFast = s_connFailStatus && el > 4000;
  if (el > CONNECT_TIMEOUT_MS || failFast) {
    s_conn = CONN_FAILED;
    strlcpy(s_connErr, s_connFailStatus == WL_NO_SSID_AVAIL ? "network not found" : s_connFailStatus == WL_CONNECT_FAILED ? "wrong password or rejected" : "timed out", sizeof s_connErr);
    Serial.printf("[wifi] connect FAILED: %s\n", s_connErr);
    // restore the previous network so a typo does not strand the device
    if (s_haveCreds) { WiFi.disconnect(); staBegin(s_creds.ssid, s_creds.pass); s_lastRetry = millis(); s_downTracked = true; s_downSince = millis(); }
  }
}

void loop() {
  if (s_ap != AP_NONE) s_dns.processNextRequest();
  pollScan();
  if (s_scanWanted) { s_scanWanted = false; startScan(true); }
  pollConnect();
  if (s_conn == CONN_CONNECTING) return;

  if (WiFi.status() == WL_CONNECTED) {
    s_downTracked = false;
    if (!s_staWasUp) onConnected();
    if (s_ap != AP_NONE && s_haveCreds) {
      uint32_t up = millis() - s_staUpSince;
      if (up >= AP_STOP_FORCE_MS || (up >= AP_STOP_STABLE_MS && WiFi.softAPgetStationNum() == 0)) stopAp("network is back");
    }
    if (s_mdnsRestart) { s_mdnsRestart = false; if (s_mdnsUp) { MDNS.end(); s_mdnsUp = false; } }
    if (!s_mdnsUp) { startMdns(); s_mdnsAnnounce = millis(); }
    if (s_mdnsUp) {
      MDNS.update();
      if (millis() - s_mdnsAnnounce >= 60000UL) { s_mdnsAnnounce = millis(); MDNS.announce(); }
    }
  } else {
    if (s_mdnsUp) s_mdnsRestart = true;
    s_staWasUp = false;
    if (!s_downTracked) { s_downTracked = true; s_downSince = millis(); }
    if (s_ap == AP_NONE && s_haveCreds && (millis() - s_downSince) >= AP_FALLBACK_MS) startAp(true);
    bool canRetry = s_haveCreds && (s_ap == AP_NONE || WiFi.softAPgetStationNum() == 0);
    if (canRetry && (millis() - s_lastRetry) >= RETRY_MS) {
      s_lastRetry = millis();
      Serial.println(F("[wifi] down, reconnecting"));
      WiFi.reconnect();
    }
  }
}

ConnState connState() { return s_conn; }
const char *connSsid() { return s_connSsid; }
const char *connError() { return s_connErr; }
bool apActive() { return s_ap != AP_NONE; }
bool captivePortal() { return s_ap != AP_NONE; }
bool rescueMode() { return s_ap == AP_RESCUE; }
bool staConnected() { return WiFi.status() == WL_CONNECTED; }
bool hasSavedNetwork() { return s_haveCreds; }
const char *apIpText() { return s_apIp; }
bool clockSynced() { return time(nullptr) > 1600000000L; }

void fillNetInfo(NetInfo &n) {
  n.mode = s_ap == AP_SETUP ? NetInfo::NET_AP_SETUP : s_ap == AP_RESCUE ? NetInfo::NET_AP_RESCUE : (s_haveCreds ? NetInfo::NET_STA : NetInfo::NET_BOOT);
  n.connected = WiFi.status() == WL_CONNECTED;
  n.connecting = s_conn == CONN_CONNECTING || (!n.connected && s_haveCreds);
  strlcpy(n.ssid, n.connected ? WiFi.SSID().c_str() : s_creds.ssid, sizeof n.ssid);
  if (n.connected) { strlcpy(n.ip, WiFi.localIP().toString().c_str(), sizeof n.ip); n.rssi = (int8_t)WiFi.RSSI(); }
  else { n.ip[0] = 0; n.rssi = 0; }
  strlcpy(n.apSsid, s_apSsid, sizeof n.apSsid);
  strlcpy(n.apIp, s_apIp, sizeof n.apIp);
  strlcpy(n.apPass, s_apPass, sizeof n.apPass);
}

}  // namespace wifimgr
