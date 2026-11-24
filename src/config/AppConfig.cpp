#include "AppConfig.h"
#include "../finance/ChartData.h"
#include <string.h>
#include <stdio.h>

const Limits LIM_PORTFOLIO = {15, 3600}, LIM_QUOTE = {15, 3600}, LIM_CHART = {60, 3600}, LIM_ROTATE = {3, 120}, LIM_OFFSET = {-720, 840};

bool validHostname(const char *h) {
  size_t n = strlen(h);
  if (n < 1 || n > 23 || h[0] == '-' || h[n - 1] == '-') return false;
  for (size_t i = 0; i < n; i++) {
    char c = h[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  }
  return true;
}

static bool getInt(const FlatJson &p, const char *k, const Limits &lim, int32_t &out, bool &present, char *err, size_t en) {
  present = p.has(k);
  if (!present) return true;
  double v;
  if (!p.num(k, v, true) || v < lim.lo || v > lim.hi) { snprintf(err, en, "%s must be %ld..%ld", k, (long)lim.lo, (long)lim.hi); return false; }
  out = (int32_t)v;
  return true;
}

bool settingsApply(Settings &s, const FlatJson &p, char *err, size_t en, bool *hostChanged) {
  Settings n = s;
  int32_t v; bool pr;
  if (!getInt(p, "portfolioRefreshS", LIM_PORTFOLIO, v, pr, err, en)) return false;
  if (pr) n.portfolioRefreshS = (uint16_t)v;
  if (!getInt(p, "quoteRefreshS", LIM_QUOTE, v, pr, err, en)) return false;
  if (pr) n.quoteRefreshS = (uint16_t)v;
  if (!getInt(p, "chartRefreshS", LIM_CHART, v, pr, err, en)) return false;
  if (pr) n.chartRefreshS = (uint16_t)v;
  if (!getInt(p, "pageRotateS", LIM_ROTATE, v, pr, err, en)) return false;
  if (pr) n.pageRotateS = (uint16_t)v;
  if (!getInt(p, "localOffsetMin", LIM_OFFSET, v, pr, err, en)) return false;
  if (pr) n.localOffsetMin = (int16_t)v;
  if (p.has("defaultRange")) {
    uint8_t r;
    if (!p.isStr("defaultRange") || !parseRange(p.str("defaultRange"), r)) { snprintf(err, en, "defaultRange must be 1d, 5d or 1mo"); return false; }
    n.defaultRange = r;
  }
  if (p.has("tzMode")) {
    const char *m = p.str("tzMode");
    if (!p.isStr("tzMode") || (strcmp(m, "exchange") && strcmp(m, "local"))) { snprintf(err, en, "tzMode must be exchange or local"); return false; }
    n.tzMode = strcmp(m, "local") == 0 ? 1 : 0;
  }
  bool hc = false;
  if (p.has("hostname")) {
    const char *h = p.str("hostname");
    if (!p.isStr("hostname") || !validHostname(h)) { snprintf(err, en, "hostname: 1-23 chars a-z 0-9 -, no leading/trailing -"); return false; }
    hc = strcmp(h, s.hostname) != 0;
    strcpy(n.hostname, h);
  }
  bool clear = false;
  if (p.has("clearAdminPass") && (!p.boolean("clearAdminPass", clear))) { snprintf(err, en, "clearAdminPass must be true/false"); return false; }
  if (p.has("adminPass")) {
    const char *a = p.str("adminPass");
    size_t l = strlen(a);
    if (!p.isStr("adminPass")) { snprintf(err, en, "adminPass must be a string"); return false; }
    if (l) {
      if (l < 4 || l > 32) { snprintf(err, en, "adminPass must be 4..32 characters"); return false; }
      for (size_t i = 0; i < l; i++) if ((unsigned char)a[i] < 0x20 || (unsigned char)a[i] > 0x7E || a[i] == ':') { snprintf(err, en, "adminPass: printable ASCII, no ':'"); return false; }
      strcpy(n.adminPass, a);
      clear = false;
    }
  }
  if (clear) n.adminPass[0] = 0;
  s = n;
  if (hostChanged) *hostChanged = hc;
  return true;
}

bool settingsApplyJson(Settings &s, const char *json, size_t len, char *err, size_t en, bool *hc) {
  FlatJson p;
  if (!p.parse(json, len)) { snprintf(err, en, "invalid JSON"); return false; }
  return settingsApply(s, p, err, en, hc);
}

static void limit(JsonWriter &w, const char *k, const Limits &l) {
  w.key(k).beginArr().integer(l.lo).integer(l.hi).endArr();
}

void settingsWrite(JsonWriter &w, const Settings &s, bool secrets, bool withLimits) {
  w.beginObj();
  w.kInt("portfolioRefreshS", s.portfolioRefreshS).kInt("quoteRefreshS", s.quoteRefreshS).kInt("chartRefreshS", s.chartRefreshS)
   .kInt("pageRotateS", s.pageRotateS).kStr("defaultRange", rangeName(s.defaultRange))
   .kStr("tzMode", s.tzMode ? "local" : "exchange").kInt("localOffsetMin", s.localOffsetMin).kStr("hostname", s.hostname);
  if (secrets) w.kStr("adminPass", s.adminPass); else w.kBool("adminPassSet", s.adminPass[0] != 0);
  if (withLimits) {
    w.key("limits").beginObj();
    limit(w, "portfolioRefreshS", LIM_PORTFOLIO); limit(w, "quoteRefreshS", LIM_QUOTE); limit(w, "chartRefreshS", LIM_CHART);
    limit(w, "pageRotateS", LIM_ROTATE); limit(w, "localOffsetMin", LIM_OFFSET);
    w.endObj();
  }
  w.endObj();
}

bool settingsLoadJson(Settings &s, const char *json, size_t len) {
  FlatJson p;
  if (!p.parse(json, len)) return false;
  // Field by field so one bad value does not discard the rest.
  const char *keys[] = {"portfolioRefreshS", "quoteRefreshS", "chartRefreshS", "pageRotateS", "defaultRange", "tzMode", "localOffsetMin", "hostname", "adminPass"};
  for (const char *k : keys) {
    char buf[200]; char tmp[96];
    int n = snprintf(buf, sizeof buf, "{\"%s\":%s%s%s}", k, p.isStr(k) ? "\"" : "", p.str(k), p.isStr(k) ? "\"" : "");
    if (p.has(k) && n > 0 && n < (int)sizeof buf) settingsApplyJson(s, buf, (size_t)n, tmp, sizeof tmp);
  }
  return true;
}
