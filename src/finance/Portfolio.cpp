#include "Portfolio.h"
#include "../core/MarketClock.h"
#include "JsonStream.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

bool normalizeSymbol(const char *in, char *out, size_t outN) {
  if (!in || !outN) return false;
  while (*in == ' ') in++;
  char t[24]; size_t n = 0;
  for (; *in && n < sizeof t - 1; in++) t[n++] = (char)toupper((unsigned char)*in);
  t[n] = 0;
  while (n && t[n - 1] == ' ') t[--n] = 0;
  const char *r = t;
  if (!strcmp(t, "SP500") || !strcmp(t, "S&P500") || !strcmp(t, "S&P 500") || !strcmp(t, "GSPC")) r = "^GSPC";
  else if (!strcmp(t, "SPX")) r = "^SPX";
  else if (!strcmp(t, "NDX") || !strcmp(t, "NASDAQ") || !strcmp(t, "NASDAQ100")) r = "^NDX";
  size_t l = strlen(r);
  if (l < 1 || l >= SYMBOL_LEN || l >= outN) return false;
  for (size_t i = 0; i < l; i++) {
    char c = r[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '^' || c == '.' || c == '=' || c == '-')) return false;
  }
  memcpy(out, r, l + 1);
  return true;
}

const char *addResultText(AddResult r) {
  switch (r) {
    case ADD_OK: return "ok"; case ADD_UPDATED: return "updated";
    case ADD_FULL: return "portfolio full (max 24 positions)";
    case ADD_TOO_MANY_SYMBOLS: return "too many different symbols (max 8)";
    case ADD_BAD_SYMBOL: return "invalid symbol"; case ADD_BAD_DATE: return "invalid date (YYYY-MM-DD, not in the future)";
    case ADD_BAD_QTY: return "quantity must be > 0"; default: return "buy price must be > 0";
  }
}

bool qtyFromThb(double thb, double buy, double &qty) {
  if (!isNum(thb) || thb <= 0 || !isNum(buy) || buy <= 0) return false;
  qty = thb / buy;
  return isNum(qty) && qty > 0;
}

uint8_t distinctSymbols(const Position *pos, uint8_t n, char out[][SYMBOL_LEN], uint8_t maxOut) {
  uint8_t c = 0;
  for (uint8_t i = 0; i < n; i++) {
    bool seen = false;
    for (uint8_t j = 0; j < c; j++) if (!strcmp(out[j], pos[i].symbol)) { seen = true; break; }
    if (!seen && c < maxOut) { strcpy(out[c++], pos[i].symbol); }
  }
  return c;
}

AddResult portfolioAdd(Position *pos, uint8_t &n, const char *symbol, const char *date, double qty, double buy, uint32_t todayUtc) {
  char sym[SYMBOL_LEN];
  if (!normalizeSymbol(symbol, sym, sizeof sym)) return ADD_BAD_SYMBOL;
  int y, m, d;
  if (!parseIsoDate(date, y, m, d)) return ADD_BAD_DATE;
  if (todayUtc && utcMidnight(y, m, d) > todayUtc + 86400) return ADD_BAD_DATE;    // allows the exchange's "today" ahead of UTC
  if (!isNum(qty) || qty <= 0) return ADD_BAD_QTY;
  if (!isNum(buy) || buy <= 0) return ADD_BAD_PRICE;
  for (uint8_t i = 0; i < n; i++)
    if (!strcmp(pos[i].symbol, sym) && !strcmp(pos[i].date, date)) { pos[i].qty = qty; pos[i].buy = buy; return ADD_UPDATED; }
  if (n >= MAX_POSITIONS) return ADD_FULL;
  char syms[MAX_POSITIONS][SYMBOL_LEN];
  uint8_t c = distinctSymbols(pos, n, syms, MAX_SYMBOLS);
  bool known = false;
  for (uint8_t j = 0; j < c; j++) if (!strcmp(syms[j], sym)) known = true;
  if (!known && c >= MAX_SYMBOLS) return ADD_TOO_MANY_SYMBOLS;
  Position &p = pos[n++];
  memset(&p, 0, sizeof p);
  strcpy(p.symbol, sym); strcpy(p.date, date);
  p.qty = qty; p.buy = buy;
  return ADD_OK;
}

bool portfolioDelete(Position *pos, uint8_t &n, uint8_t index) {
  if (index >= n) return false;
  for (uint8_t i = index; i + 1 < n; i++) pos[i] = pos[i + 1];
  n--;
  return true;
}

SymQuote *bookFind(QuoteBook &b, const char *symbol) {
  for (uint8_t i = 0; i < b.n; i++) if (!strcmp(b.q[i].symbol, symbol)) return &b.q[i];
  return nullptr;
}

void bookSync(QuoteBook &b, const Position *pos, uint8_t n) {
  char syms[MAX_POSITIONS][SYMBOL_LEN];
  uint8_t c = distinctSymbols(pos, n, syms, MAX_SYMBOLS);
  QuoteBook nb; memset(&nb, 0, sizeof nb);
  for (uint8_t i = 0; i < c; i++) {
    SymQuote *old = bookFind(b, syms[i]);
    SymQuote &q = nb.q[nb.n++];
    if (old) q = *old; else { memset(&q, 0, sizeof q); strcpy(q.symbol, syms[i]); q.price = NAN; }
  }
  b = nb;
}

void portfolioWrite(JsonWriter &w, const Position *pos, uint8_t n) {
  w.beginObj().key("positions").beginArr();
  for (uint8_t i = 0; i < n; i++)
    w.beginObj().kStr("symbol", pos[i].symbol).kNum("qty", pos[i].qty, 8).kNum("buy", pos[i].buy, 6).kStr("date", pos[i].date).endObj();
  w.endArr().endObj();
}

struct LoadCtx { Position *pos; uint8_t n; Position cur; bool have[4]; uint32_t rowId; const JsonStream *js; };

static void flush(LoadCtx *c) {
  if (!c->rowId) return;
  if (c->have[0] && c->have[1] && c->have[2] && c->n < MAX_POSITIONS) {
    Position &p = c->cur;
    char sym[SYMBOL_LEN];
    if (normalizeSymbol(p.symbol, sym, sizeof sym) && isNum(p.qty) && isNum(p.buy)) {
      strcpy(p.symbol, sym);
      int y, m, d;
      if (!c->have[3] || !parseIsoDate(p.date, y, m, d)) p.date[0] = 0;   // legacy rows without a date stay loadable
      c->pos[c->n++] = p;
    }
  }
  c->rowId = 0;
}

static void onLoad(void *ctx, const char *path, int, JsonKind k, const char *text) {
  LoadCtx *c = static_cast<LoadCtx *>(ctx);
  if (strncmp(path, "positions[].", 12) != 0) return;
  uint32_t id = c->js->containerId();            // one id per row object, whatever the key order
  if (id != c->rowId) { flush(c); memset(&c->cur, 0, sizeof c->cur); memset(c->have, 0, sizeof c->have); c->rowId = id; }
  const char *f = path + 12;
  if (!strcmp(f, "symbol") && k == JK_STR) { strncpy(c->cur.symbol, text, SYMBOL_LEN - 1); c->have[0] = true; }
  else if (!strcmp(f, "qty") && k == JK_NUM) { c->cur.qty = strtod(text, nullptr); c->have[1] = true; }
  else if (!strcmp(f, "buy") && k == JK_NUM) { c->cur.buy = strtod(text, nullptr); c->have[2] = true; }
  else if (!strcmp(f, "date") && k == JK_STR) { strncpy(c->cur.date, text, 10); c->cur.date[10] = 0; c->have[3] = true; }
}

uint8_t portfolioLoad(Position *pos, const char *json, size_t len) {
  LoadCtx c; memset(&c, 0, sizeof c); c.pos = pos;
  JsonStream js; c.js = &js; js.begin(onLoad, &c);
  if (!js.feed(json, len) || !js.complete()) return 0;
  flush(&c);
  return c.n;
}
