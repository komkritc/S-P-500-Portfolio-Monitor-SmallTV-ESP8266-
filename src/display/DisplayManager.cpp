#include "DisplayManager.h"
#include <Arduino_GFX_Library.h>
#include "font/lcd_gfx.h"
#include "font/lcd_qrcode.h"
#include "UiKit.h"
#include "ViewSchedule.h"
#include "Widgets.h"
#include "PortfolioScreen.h"
#include "PositionsScreen.h"
#include "RealisedScreen.h"
#include "MarketScreen.h"
#include "ChartRenderer.h"

namespace display {
using namespace ui;

static Ui s_ui;
static Arduino_GFX *g = nullptr;
static View s_views[MAX_VIEWS];
static uint8_t s_nViews = 0, s_cur = 0;
static bool s_full = true;
static uint32_t s_viewSince = 0, s_lastDraw = 0, s_lastRev = 0xFFFFFFFFu, s_chartSig = 0;
static bool s_otaMode = false;
static int s_otaPct = -1;
static uint8_t s_lastPos = 255, s_lastDefault = 255;
static bool s_lastAp = false, s_lastDash = true, s_lastRealised = false;
static ViewKind s_shownKind = V_PORTFOLIO;
static uint8_t s_shownArg = 255;

void invalidate() { s_full = true; s_otaMode = false; }

void begin() {
  lcdBegin();
  g = lcdDev();
  s_ui.begin(g);
  s_ui.clear();
  s_ui.text(0, "S&P 500 Monitor", 2, 120, 96, A_CENTER, C_HEAD);
  s_ui.text(1, "starting...", 1, 120, 126, A_CENTER, C_MUTED);
}

void showBoot(const char *line) {
  if (!g || s_otaMode) return;
  s_ui.text(1, line, 1, 120, 126, A_CENTER, C_MUTED);
}

// ---------------------------------------------------------------- QR + AP screen
static void qrEscape(char *out, size_t n, const char *s) {
  size_t o = strlen(out);
  for (; *s && o + 2 < n; s++) { if (strchr("\\;,:\"", *s)) out[o++] = '\\'; out[o++] = *s; }
  out[o] = 0;
}

static int drawQr(const char *text, int cx, int top, int maxPx) {
  static const uint8_t CAP[] = {17, 32, 53, 78, 106, 134};     // byte-mode capacity, ECC low, versions 1..6
  size_t len = strlen(text);
  uint8_t ver = 0;
  for (uint8_t i = 0; i < sizeof CAP; i++) if (len <= CAP[i]) { ver = i + 1; break; }
  if (!ver) return 0;
  uint8_t buf[211];
  QRCode qr;
  qrcode_initText(&qr, buf, ver, ECC_LOW, text);
  const int quiet = 2;
  int mod = maxPx / (qr.size + 2 * quiet);
  if (mod < 1) mod = 1;
  int box = (qr.size + 2 * quiet) * mod, x0 = cx - box / 2;
  g->fillRoundRect(x0, top, box, box, 4, 0xFFFF);                // always black on white: phone scanners dislike inverted codes
  for (uint8_t y = 0; y < qr.size; y++)
    for (uint8_t x = 0; x < qr.size; x++)
      if (qrcode_getModule(&qr, x, y)) g->fillRect(x0 + (quiet + x) * mod, top + (quiet + y) * mod, mod, mod, 0x0000);
  return box;
}

static void drawAp(const AppState &a) {
  const NetInfo &n = a.net;
  const bool secured = n.apPass[0] != 0;
  s_ui.clear();
  s_ui.text(0, secured ? "WI-FI LOST - RESCUE" : "WI-FI SETUP", 1, 120, 6, A_CENTER, secured ? C_WARN : C_HEAD);
  char payload[112] = "WIFI:T:";
  strncat(payload, secured ? "WPA" : "nopass", sizeof payload - strlen(payload) - 1);
  strncat(payload, ";S:", sizeof payload - strlen(payload) - 1);
  qrEscape(payload, sizeof payload, n.apSsid);
  if (secured) { strncat(payload, ";P:", sizeof payload - strlen(payload) - 1); qrEscape(payload, sizeof payload, n.apPass); }
  strncat(payload, ";;", sizeof payload - strlen(payload) - 1);
  int box = drawQr(payload, 120, 22, secured ? 122 : 130);
  int y = 22 + box + 6;
  s_ui.text(1, "Join this Wi-Fi:", 1, 120, y, A_CENTER, C_MUTED);
  s_ui.text(2, n.apSsid, 1, 120, y + 14, A_CENTER, C_TEXT);
  y += 30;
  if (secured) {
    char k[28]; snprintf(k, sizeof k, "key %s", n.apPass);
    s_ui.text(3, k, 1, 120, y, A_CENTER, C_HEAD);
    y += 15;
  }
  char u[40]; snprintf(u, sizeof u, "then open http://%s", n.apIp[0] ? n.apIp : "192.168.4.1");
  s_ui.text(4, u, 1, 120, y, A_CENTER, C_BLUE);
  if (y + 15 < 226) s_ui.text(5, secured ? "Saved network unreachable" : "Pick your network on the page", 1, 120, y + 15, A_CENTER, C_MUTED);
}

// ---------------------------------------------------------------- OTA screen
static const int OTA_X = 24, OTA_Y = 128, OTA_W = 192, OTA_H = 12;

void showOta(const char *source) {
  if (!g) return;
  s_otaMode = true;
  s_ui.clear();
  s_ui.text(0, "UPDATING", 1, 120, 60, A_CENTER, C_HEAD);
  s_ui.text(1, "Firmware update", 2, 120, 82, A_CENTER, C_TEXT);
  s_ui.text(2, source, 1, 120, 106, A_CENTER, C_MUTED);
  g->fillRoundRect(OTA_X, OTA_Y, OTA_W, OTA_H, OTA_H / 2, C_PANEL);
  s_ui.text(3, "Do not power off", 1, 120, 200, A_CENTER, C_WARN);
  s_otaPct = -1;
}

void otaProgress(int pct) {
  if (!g) return;
  pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
  if (pct == s_otaPct) return;
  s_otaPct = pct;
  int w = OTA_W * pct / 100;
  if (w >= OTA_H) g->fillRoundRect(OTA_X, OTA_Y, w, OTA_H, OTA_H / 2, C_HEAD);
  char b[8]; snprintf(b, sizeof b, "%d%%", pct);
  s_ui.text(4, b, 2, 120, 150, A_CENTER, C_TEXT);
}

void otaResult(bool ok, const char *msg) {
  if (!g) return;
  s_ui.text(3, msg, 1, 120, 200, A_CENTER, ok ? C_POS : C_NEG);
}

// ---------------------------------------------------------------- rotation
static void rebuildViews(const AppState &a) {
  bool ap = a.net.mode == NetInfo::NET_AP_SETUP || a.net.mode == NetInfo::NET_AP_RESCUE;
  bool dash = a.net.mode != NetInfo::NET_AP_SETUP;
  bool realised = a.realised.valid && a.realised.count > 0;      // page 4 only exists once a sale stands
  s_nViews = buildViews(s_views, MAX_VIEWS, a.posCount, a.settings.defaultRange, ap, dash, realised);
  s_lastPos = a.posCount; s_lastDefault = a.settings.defaultRange; s_lastAp = ap; s_lastDash = dash; s_lastRealised = realised;
  // keep showing the same view when the list changes
  uint8_t keep = 0;
  for (uint8_t i = 0; i < s_nViews; i++) if (s_views[i].kind == s_shownKind && s_views[i].arg == s_shownArg) { keep = i; break; }
  s_cur = keep;
}

void loop(const AppState &a) {
  if (!g || s_otaMode) return;
  uint32_t now = millis();

  bool ap = a.net.mode == NetInfo::NET_AP_SETUP || a.net.mode == NetInfo::NET_AP_RESCUE;
  bool dash = a.net.mode != NetInfo::NET_AP_SETUP;
  bool realised = a.realised.valid && a.realised.count > 0;
  if (!s_nViews || a.posCount != s_lastPos || a.settings.defaultRange != s_lastDefault || ap != s_lastAp || dash != s_lastDash
      || realised != s_lastRealised) rebuildViews(a);
  if (!s_nViews) return;

  // advance
  bool switched = false;
  if (s_nViews > 1 && (now - s_viewSince) >= (uint32_t)a.settings.pageRotateS * 1000UL) {
    s_cur = nextIndex(s_cur, s_nViews);
    switched = true;
  }
  const View v = s_views[s_cur];
  if (v.kind != s_shownKind || v.arg != s_shownArg) switched = true;
  if (switched) { s_viewSince = now; s_full = true; }

  const bool apScreen = v.kind == V_AP;
  if (!s_full && !apScreen && (now - s_lastDraw) < 250) return;           // cached slots make a redraw cheap, but no need to spin
  if (!s_full && apScreen) { return; }                                     // AP screen is static (key/IP never change while shown)
  s_lastDraw = now;

  if (s_full) {
    if (apScreen) { drawAp(a); s_full = false; s_shownKind = v.kind; s_shownArg = v.arg; return; }
    s_ui.clear();
    resetWidgetCache();
    s_chartSig = 0xFFFFFFFFu;
  }
  switch (v.kind) {
    case V_PORTFOLIO: drawPortfolio(s_ui, a, s_full); break;
    case V_POSITIONS: drawPositions(s_ui, a, v.arg, s_full); break;
    case V_REALISED: drawRealised(s_ui, a, s_full); break;
    case V_MARKET: {
      ChartParams cp = {v.arg, a.settings.tzMode, a.settings.localOffsetMin, a.chartStale[v.arg]};
      uint32_t sig = chartSignature(a.charts[v.arg], cp);
      bool dirty = sig != s_chartSig;
      drawMarket(s_ui, a, v.arg, s_full, dirty);
      s_chartSig = sig;
      break;
    }
    default: break;
  }
  s_full = false;
  s_shownKind = v.kind; s_shownArg = v.arg;
}

}  // namespace display
