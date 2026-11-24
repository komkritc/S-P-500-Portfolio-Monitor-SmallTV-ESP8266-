// SaleEngine.cpp - pure sale accounting (see SaleEngine.h).
#include "SaleEngine.h"
#include "../core/JsonWriter.h"
#include "../core/MarketClock.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <new>

namespace sale {

// NOTE: JSON key names must stay plain RAM literals - JsonWriter::key()/findKey() read them with the ROM
// strlen/memcpy/memcmp, which fault (LoadStoreError) on PSTR flash strings. S_LIT is only for messages.
// ---------------------------------------------------------------- small helpers
static void copyStr(char *dst, size_t n, const char *src) {
  if (!n) return;
  size_t i = 0;
  if (src) for (; src[i] && i + 1 < n; i++) dst[i] = src[i];
  dst[i] = 0;
}

static bool fail(SalePreview &pv, const char *msg) {   // msg is S_LIT (PSTR in flash on the device)
  pv.ok = false;
#ifdef ARDUINO
  strncpy_P(pv.error, msg, sizeof pv.error - 1);          // byte reads of flash must go through *_P
  pv.error[sizeof pv.error - 1] = 0;
#else
  copyStr(pv.error, sizeof pv.error, msg);
#endif
  return false;
}

static bool failf(SalePreview &pv, const char *fmt, double a) {
  pv.ok = false;
#ifdef ARDUINO
  snprintf_P(pv.error, sizeof pv.error, fmt, a);          // fmt is a PSTR (flash)
#else
  snprintf(pv.error, sizeof pv.error, fmt, a);
#endif
  return false;
}

// ---------------------------------------------------------------- JSON field readers
// Bounded readers over one record line. The format is our own (flat keys + alloc[]), so a tiny
// scanner is enough - and it never reads past `e`.
static const char *skipWs(const char *p, const char *e) {
  while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
  return p;
}

static const char *findKey(const char *s, const char *e, const char *key) {
  size_t kl = strlen(key);
  for (const char *p = s; p + kl + 1 < e; p++) {
    if (*p != '"' || p[1] != key[0]) continue;
    if (memcmp(p + 1, key, kl) != 0 || p[kl + 1] != '"') continue;
    const char *q = skipWs(p + kl + 2, e);
    if (q >= e || *q != ':') continue;
    return skipWs(q + 1, e);
  }
  return nullptr;
}

static bool jStr(const char *s, const char *e, const char *key, char *out, size_t outN) {
  const char *v = findKey(s, e, key);
  if (!v || v >= e || *v != '"') return false;
  v++;
  size_t i = 0;
  while (v < e && *v != '"') {
    char c = *v;
    if (c == '\\' && v + 1 < e) { v++; c = *v; }         // our strings never need escapes, but be safe
    if (i + 1 < outN) out[i++] = c;
    v++;
  }
  if (v >= e) return false;                              // unterminated
  out[i] = 0;
  return true;
}

static bool jNum(const char *s, const char *e, const char *key, double &out) {
  const char *v = findKey(s, e, key);
  if (!v || v >= e) return false;
  if (v + 4 <= e && !memcmp(v, "null", 4)) { out = NAN; return true; }
  if (!(*v == '-' || (*v >= '0' && *v <= '9'))) return false;
  char tmp[40];
  size_t i = 0;
  while (v < e && i + 1 < sizeof tmp &&
         (isdigit((unsigned char)*v) || *v == '-' || *v == '+' || *v == '.' || *v == 'e' || *v == 'E'))
    tmp[i++] = *v++;
  tmp[i] = 0;
  if (!i) return false;
  out = strtod(tmp, nullptr);
  return true;
}

static bool jU32(const char *s, const char *e, const char *key, uint32_t &out) {
  double d;
  if (!jNum(s, e, key, d) || !isNum(d) || d < 0) return false;
  out = (uint32_t)d;
  return true;
}

// ---------------------------------------------------------------- records
static bool parseAllocs(const char *s, const char *e, SaleRecord &r) {
  const char *v = findKey(s, e, "alloc");
  if (!v || v >= e || *v != '[') return false;
  const char *p = v + 1;
  while (p < e && *p == '{') {
    const char *close = nullptr;
    for (const char *q = p; q < e; q++) if (*q == '}') { close = q; break; }
    if (!close) return false;
    if (r.nAlloc >= MAX_LOT_ALLOCS) return false;
    LotAlloc &a = r.alloc[r.nAlloc];
    memset(&a, 0, sizeof a);
    uint32_t lot = 0;
    if (!jU32(p, close, "lot", lot) || lot == 0 || lot > 0xFFFFu) return false;
    a.lot = (uint16_t)lot;
    if (!jStr(p, close, "date", a.date, sizeof a.date)) a.date[0] = 0;
    if (!jNum(p, close, "buy", a.buy) || !isNum(a.buy)) return false;
    if (!jNum(p, close, "oqty", a.oqty) || !isNum(a.oqty)) a.oqty = 0;
    if (!jNum(p, close, "before", a.before)) return false;
    if (!jNum(p, close, "after", a.after)) return false;
    if (!(a.oqty > 0)) a.oqty = a.before;              // legacy/short record: fall back to the open qty
    r.nAlloc++;
    p = close + 1;
    if (p < e && *p == ',') p++;
  }
  return r.nAlloc > 0 && p < e && *p == ']';
}

bool parseRecord(const char *line, size_t len, SaleRecord &out) {
  if (!line || len < 2) return false;
  const char *s = line, *e = line + len;
  while (e > s && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ')) e--;
  if (e - s < 2 || *s != '{') return false;
  SaleRecord r;
  memset(&r, 0, sizeof r);
  uint32_t v;
  if (!jU32(s, e, "seq", v) || v == 0) return false;
  r.seq = v;
  if (!jU32(s, e, "ts", r.ts)) r.ts = 0;
  if (!jStr(s, e, "id", r.id, sizeof r.id) || !r.id[0]) return false;
  if (!jStr(s, e, "symbol", r.symbol, sizeof r.symbol) || !r.symbol[0]) return false;
  if (!jStr(s, e, "date", r.date, sizeof r.date)) return false;
  if (!jU32(s, e, "type", v) || v > REC_REVERSAL) return false;
  r.type = (uint8_t)v;
  if (jU32(s, e, "refSeq", v)) r.refSeq = v;
  jStr(s, e, "refId", r.refId, sizeof r.refId);
  char ps[8];
  if (jStr(s, e, "priceSource", ps, sizeof ps)) r.priceSource = (strcmp(ps, "yahoo") == 0) ? PRICE_YAHOO : PRICE_MANUAL;
  else if (jU32(s, e, "priceSource", v)) r.priceSource = (uint8_t)v;
  double d;
  if (!jNum(s, e, "qty", d) || !isNum(d)) return false;
  r.qty = d;
  if (!jNum(s, e, "price", d) || !isNum(d)) return false;
  r.price = d;
  if (jNum(s, e, "refPrice", d)) r.refPrice = d; else r.refPrice = NAN;
  if (jNum(s, e, "fee", d) && isNum(d)) r.fee = d;
  if (jNum(s, e, "gross", d) && isNum(d)) r.gross = d;
  if (jNum(s, e, "net", d) && isNum(d)) r.net = d;
  if (jNum(s, e, "cost", d) && isNum(d)) r.cost = d;
  if (jNum(s, e, "realised", d) && isNum(d)) r.realised = d;
  if (!parseAllocs(s, e, r)) return false;
  out = r;
  return true;
}

void writeRecord(JsonWriter &w, const SaleRecord &r) {
  w.beginObj()
   .kInt("seq", r.seq).kInt("ts", r.ts).kStr("id", r.id)
   .kInt("type", r.type).kStr("symbol", r.symbol).kStr("date", r.date)
   .kNum("qty", r.qty, 8).kNum("price", r.price, 6)
   .kNum("refPrice", r.refPrice, 6)
   .kStr("priceSource", r.priceSource == PRICE_YAHOO ? "yahoo" : "manual")
   .kNum("fee", r.fee, 4)
   .kNum("gross", r.gross, 6).kNum("net", r.net, 6).kNum("cost", r.cost, 6).kNum("realised", r.realised, 6)
   .kInt("refSeq", r.refSeq).kStr("refId", r.refId)
   .key("alloc").beginArr();
  for (uint8_t i = 0; i < r.nAlloc; i++) {
    const LotAlloc &a = r.alloc[i];
    w.beginObj().kInt("lot", a.lot).kStr("date", a.date).kNum("buy", a.buy, 6)
     .kNum("oqty", a.oqty, 8).kNum("before", a.before, 8).kNum("after", a.after, 8).endObj();
  }
  w.endArr().endObj();
}

struct StrBuf { char *p; size_t n, len; bool full; };
static void sinkBuf(void *ctx, const char *d, size_t n) {
  StrBuf *s = static_cast<StrBuf *>(ctx);
  for (size_t i = 0; i < n; i++) {
    if (s->len + 1 >= s->n) { s->full = true; return; }   // no room left for the NUL
    s->p[s->len++] = d[i];
  }
}
size_t recordLine(const SaleRecord &r, char *buf, size_t cap) {
  if (!buf || cap < 4) return 0;
  StrBuf s{buf, cap, 0, false};
  // JsonWriter carries a 320 B buffer: keep it off the (small) request stack
  JsonWriter *w = new (std::nothrow) JsonWriter(sinkBuf, &s);
  if (!w) return 0;
  writeRecord(*w, r);
  delete w;                                               // flushes into the sink
  if (s.full) return 0;                                   // never hand back a truncated record
  buf[s.len] = 0;
  return s.len;
}

// ---------------------------------------------------------------- ledger iteration
void scan(LineSrc &src, RecordFn fn, void *ctx, ScanStats &st) {
  memset(&st, 0, sizeof st);
  if (!src.open || !src.open(src.ctx)) return;
  st.opened = true;
  char *buf = (char *)malloc(LEDGER_LINE_MAX + 2);
  if (!buf) { if (src.close) src.close(src.ctx); return; }
  uint16_t idx = 0;
  for (;;) {
    int r = src.next(src.ctx, buf, LEDGER_LINE_MAX + 2);
    if (r == 0) break;
    if (r < 0) { st.badLines++; break; }
    bool term = !src.terminated || src.terminated(src.ctx);
    size_t len = (size_t)r;
    while (len && (buf[len - 1] == '\r' || buf[len - 1] == '\n')) len--;
    if (!term) { st.partialTail = true; break; }        // never trust an unterminated line
    if (!len) continue;
    SaleRecord rec;
    if (!parseRecord(buf, len, rec)) { st.badLines++; continue; }
    if (rec.seq > st.maxSeq) st.maxSeq = rec.seq;
    st.records++;
    if (fn) fn(ctx, rec, idx);
    idx++;
  }
  free(buf);
  if (src.close) src.close(src.ctx);
}

// ---------------------------------------------------------------- allocation & preview
static uint32_t fnv(uint32_t h, const void *data, size_t n) {
  const uint8_t *p = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}

uint32_t previewToken(const Position *pos, uint8_t n, const PortfolioMeta &m) {
  uint32_t h = 2166136261u;
  h = fnv(h, &m.ver, sizeof m.ver);
  h = fnv(h, &m.appliedSeq, sizeof m.appliedSeq);
  h = fnv(h, &n, sizeof n);
  for (uint8_t i = 0; i < n; i++) {
    h = fnv(h, &pos[i].id, sizeof pos[i].id);
    h = fnv(h, &pos[i].qty, sizeof pos[i].qty);
    h = fnv(h, &pos[i].buy, sizeof pos[i].buy);
  }
  return h ? h : 1;
}

static void addAlloc(SalePreview &pv, const Position &lot, double take) {
  if (pv.nAlloc >= MAX_LOT_ALLOCS) return;
  LotAlloc &a = pv.alloc[pv.nAlloc++];
  a.lot = lot.id;
  copyStr(a.date, sizeof a.date, lot.date);
  a.buy = lot.buy;
  a.oqty = (isNum(lot.oqty) && lot.oqty > 0) ? lot.oqty : lot.qty;
  a.before = lot.qty;
  a.after = lot.qty - take;
}

bool preview(const Position *pos, uint8_t n, const PortfolioMeta &m, const SaleRequest &req,
             uint32_t nowEpoch, SalePreview &pv) {
  memset(&pv, 0, sizeof pv);
  pv.price = pv.refPrice = pv.qty = pv.available = pv.fee = pv.gross = pv.net = pv.cost = pv.realised = NAN;
  pv.priceSource = req.priceSource;
  pv.error[0] = 0;

  if (!req.symbol[0]) return fail(pv, S_LIT("symbol required"));
  int y, mo, d;
  if (!parseIsoDate(req.date, y, mo, d)) return fail(pv, S_LIT("invalid date (YYYY-MM-DD)"));
  if (nowEpoch && utcMidnight(y, mo, d) > nowEpoch + 86400UL) return fail(pv, S_LIT("sale date is in the future"));
  if (!isNum(req.price) || req.price <= 0) return fail(pv, S_LIT("execution price must be > 0"));
  double fee = isNum(req.fee) ? req.fee : 0;
  if (fee < 0) return fail(pv, S_LIT("fee must be >= 0"));

  uint16_t lots[MAX_POSITIONS];
  uint8_t c = 0;
  double avail = 0;
  for (uint8_t i = 0; i < n; i++) {
    if (strcmp(pos[i].symbol, req.symbol) != 0) continue;
    if (!isNum(pos[i].qty) || pos[i].qty <= 0 || !isNum(pos[i].buy) || pos[i].buy <= 0) continue;
    lots[c++] = i;
    avail += pos[i].qty;
  }
  if (!c) return fail(pv, S_LIT("no open position for that symbol"));
  if (!(avail > 0)) return fail(pv, S_LIT("open quantity is zero"));

  double qty;
  switch (req.mode) {
    case SALE_BY_ALL: qty = avail; break;
    case SALE_BY_THB:
      if (!isNum(req.amount) || req.amount <= 0) return fail(pv, S_LIT("amount must be > 0"));
      qty = req.amount / req.price;
      break;
    case SALE_BY_QTY:
      if (!isNum(req.qty) || req.qty <= 0) return fail(pv, S_LIT("quantity must be > 0"));
      qty = req.qty;
      break;
    default: return fail(pv, S_LIT("unknown sale mode"));
  }
  if (!isNum(qty) || qty <= 0) return fail(pv, S_LIT("quantity must be > 0"));
  const double EPS = 1e-7;
  if (qty > avail) {
    if (qty > avail + EPS * (avail > 1 ? avail : 1)) return failf(pv, S_LIT("only %.8g available"), avail);
    qty = avail;                                          // floating-point overshoot: clamp
  }

  pv.nAlloc = 0;
  if (req.method == SALE_SPECIFIC) {
    if (!req.lotId) return fail(pv, S_LIT("choose a lot"));
    const Position *lot = nullptr;
    for (uint8_t k = 0; k < c; k++) if (pos[lots[k]].id == req.lotId) { lot = &pos[lots[k]]; break; }
    if (!lot) return fail(pv, S_LIT("lot not found for that symbol"));
    if (lot->qty < qty - EPS * (lot->qty > 1 ? lot->qty : 1)) return failf(pv, S_LIT("lot has only %.8g"), lot->qty);
    addAlloc(pv, *lot, qty);
  } else if (req.method == SALE_FIFO) {
    double need = qty;
    for (uint8_t k = 0; k < c && need > 0; k++) {
      const Position &lot = pos[lots[k]];
      double take = lot.qty < need ? lot.qty : need;
      if (take <= 0) continue;
      addAlloc(pv, lot, take);
      need -= take;
    }
    if (need > EPS) { pv.nAlloc = 0; return fail(pv, S_LIT("not enough quantity in the open lots")); }
  } else {
    return fail(pv, S_LIT("unknown sale method"));
  }
  if (!pv.nAlloc) return fail(pv, S_LIT("nothing to sell"));

  double cost = 0;
  for (uint8_t i = 0; i < pv.nAlloc; i++) cost += (pv.alloc[i].before - pv.alloc[i].after) * pv.alloc[i].buy;

  copyStr(pv.symbol, sizeof pv.symbol, req.symbol);
  copyStr(pv.date, sizeof pv.date, req.date);
  pv.price = req.price;
  pv.refPrice = req.refPrice;
  pv.fee = fee;
  pv.qty = qty;
  pv.available = avail;
  pv.gross = qty * req.price;
  pv.net = pv.gross - fee;
  pv.cost = cost;
  pv.realised = pv.net - cost;
  pv.token = previewToken(pos, n, m);
  pv.ok = true;
  pv.error[0] = 0;
  return true;
}

// ---------------------------------------------------------------- records from a preview
void makeRecord(const SalePreview &pv, uint32_t seq, uint32_t ts, const PortfolioMeta &m, SaleRecord &out) {
  memset(&out, 0, sizeof out);
  out.seq = seq;
  out.ts = ts;
  out.type = REC_SALE;
  snprintf(out.id, sizeof out.id, "TX-%06lu", (unsigned long)m.nextTxn);
  copyStr(out.symbol, sizeof out.symbol, pv.symbol);
  copyStr(out.date, sizeof out.date, pv.date);
  out.priceSource = pv.priceSource;
  out.qty = pv.qty;
  out.price = pv.price;
  out.refPrice = pv.refPrice;
  out.fee = isNum(pv.fee) ? pv.fee : 0;
  out.gross = pv.gross;
  out.net = pv.net;
  out.cost = pv.cost;
  out.realised = pv.realised;
  out.nAlloc = pv.nAlloc;
  for (uint8_t i = 0; i < pv.nAlloc; i++) out.alloc[i] = pv.alloc[i];
}

static const Position *findLot(const Position *pos, uint8_t n, uint16_t id) {
  for (uint8_t i = 0; i < n; i++) if (pos[i].id == id) return &pos[i];
  return nullptr;
}

bool makeReversal(const Position *pos, uint8_t n, const SaleRecord &orig, uint32_t seq, uint32_t ts,
                  const PortfolioMeta &m, SaleRecord &out, char *err, size_t errN) {
  if (orig.type != REC_SALE) {
    if (err && errN) snprintf(err, errN, "only a sale can be reversed");
    return false;
  }
  if (!orig.nAlloc) {
    if (err && errN) snprintf(err, errN, "record has no allocations");
    return false;
  }
  memset(&out, 0, sizeof out);
  out.seq = seq;
  out.ts = ts;
  out.type = REC_REVERSAL;
  out.refSeq = orig.seq;
  copyStr(out.refId, sizeof out.refId, orig.id);
  snprintf(out.id, sizeof out.id, "TX-%06lu", (unsigned long)m.nextTxn);
  copyStr(out.symbol, sizeof out.symbol, orig.symbol);
  copyStr(out.date, sizeof out.date, orig.date);
  out.priceSource = orig.priceSource;
  out.qty = orig.qty;
  out.price = orig.price;
  out.refPrice = orig.refPrice;
  out.fee = 0;
  out.gross = -orig.gross;
  out.net = -orig.net;
  out.cost = -orig.cost;
  out.realised = -orig.realised;
  out.nAlloc = orig.nAlloc;
  for (uint8_t i = 0; i < orig.nAlloc && i < MAX_LOT_ALLOCS; i++) {
    const LotAlloc &a = orig.alloc[i];
    LotAlloc &b = out.alloc[i];
    double sold = a.before - a.after;                    // quantity the original sale took
    if (sold < 0) sold = 0;
    b.lot = a.lot;
    copyStr(b.date, sizeof b.date, a.date);
    b.buy = a.buy;
    b.oqty = a.oqty;
    const Position *lot = findLot(pos, n, a.lot);
    b.before = lot ? lot->qty : 0;
    b.after = b.before + sold;
  }
  return true;
}

// ---------------------------------------------------------------- apply
bool applyRecord(Position *pos, uint8_t &n, PortfolioMeta &m, const SaleRecord &r, char *err, size_t errN) {
  auto setErr = [&](const char *msg) {
    if (err && errN) snprintf(err, errN, "%s", msg);
    return false;
  };
  for (uint8_t i = 0; i < r.nAlloc; i++) {
    const LotAlloc &a = r.alloc[i];
    Position *lot = nullptr;
    for (uint8_t k = 0; k < n; k++) if (pos[k].id == a.lot) { lot = &pos[k]; break; }
    if (!lot) {
      if (r.type == REC_REVERSAL) {
        if (n >= MAX_POSITIONS) return setErr("portfolio full (cannot restore the lot)");
        if (a.lot == 0) return setErr("record references lot 0");
        lot = &pos[n++];
        memset(lot, 0, sizeof *lot);
        lot->id = a.lot;
        copyStr(lot->symbol, sizeof lot->symbol, r.symbol);
        copyStr(lot->date, sizeof lot->date, a.date);
        lot->qty = 0;
        lot->buy = a.buy;
        lot->oqty = (isNum(a.oqty) && a.oqty > 0) ? a.oqty : a.after;
      } else if (a.after <= 1e-9) {
        continue;                                         // sold-out lot already gone: replay is a no-op
      } else {
        return setErr("lot is no longer in the portfolio");
      }
    } else if (strcmp(lot->symbol, r.symbol) != 0) {
      return setErr("lot belongs to another symbol");
    }
    lot->qty = a.after;                                   // absolute: replaying is a no-op
    if (a.lot >= m.nextLotId) m.nextLotId = (uint16_t)(a.lot + 1);
  }
  // A fully sold lot leaves the open list (its history stays in the ledger).
  if (r.type == REC_SALE) {
    for (uint8_t i = 0; i < r.nAlloc; i++) {
      if (!(r.alloc[i].after <= 1e-9)) continue;
      for (uint8_t k = 0; k < n; k++) {
        if (pos[k].id != r.alloc[i].lot) continue;
        for (uint8_t j = k; j + 1 < n; j++) pos[j] = pos[j + 1];
        n--;
        break;
      }
    }
  }
  if (r.seq > m.appliedSeq) m.appliedSeq = r.seq;
  m.ver++;
  return true;
}

// ---------------------------------------------------------------- aggregates
static SaleLite liteOf(const SaleRecord &r) {
  SaleLite l;
  memset(&l, 0, sizeof l);
  l.seq = r.seq;
  l.ts = r.ts;
  copyStr(l.id, sizeof l.id, r.id);
  copyStr(l.refId, sizeof l.refId, r.refId);
  copyStr(l.symbol, sizeof l.symbol, r.symbol);
  copyStr(l.date, sizeof l.date, r.date);
  l.type = r.type;
  l.priceSource = r.priceSource;
  l.status = r.type;
  l.qty = r.qty;
  l.price = r.price;
  l.refPrice = r.refPrice;
  l.fee = r.fee;
  l.gross = r.gross;
  l.net = r.net;
  l.cost = r.cost;
  l.realised = r.realised;
  return l;
}

static void yearAdd(RealisedSummary &s, const char *date, double delta) {
  int y = 0;
  if (date && isdigit((unsigned char)date[0]) && isdigit((unsigned char)date[1]) &&
      isdigit((unsigned char)date[2]) && isdigit((unsigned char)date[3]))
    y = (date[0] - '0') * 1000 + (date[1] - '0') * 100 + (date[2] - '0') * 10 + (date[3] - '0');
  if (y <= 0) return;
  for (uint8_t i = 0; i < s.nYears; i++)
    if (s.year[i] == (uint16_t)y) { s.yearPl[i] += delta; return; }
  if (s.nYears >= YEAR_BUCKETS) return;                 // display buckets only; `pl` stays exact
  uint8_t k = s.nYears;
  while (k > 0 && s.year[k - 1] < (uint16_t)y) k--;      // keep newest first
  for (uint8_t j = s.nYears; j > k; j--) { s.year[j] = s.year[j - 1]; s.yearPl[j] = s.yearPl[j - 1]; }
  s.year[k] = (uint16_t)y;
  s.yearPl[k] = delta;
  s.nYears++;
}

uint32_t seqAfterDelete(uint32_t seq, uint32_t a, uint32_t b) {
  // = how many records with seq' <= seq remain. For a surviving seq that is its new number; for
  // appliedSeq it is the new "highest applied" even when that record itself was deleted.
  uint32_t gone = (a && a <= seq ? 1u : 0u) + (b && b != a && b <= seq ? 1u : 0u);
  return seq - gone;
}

void summaryReset(RealisedSummary &s) {
  memset(&s, 0, sizeof s);
  s.valid = true;
}

void summaryAdd(RealisedSummary &s, const SaleRecord &r) {
  s.records++;
  if (r.type == REC_SALE) {
    s.count++;
    s.pl += r.realised;
    if (s.nLast >= sizeof(s.lastRing) / sizeof(s.lastRing[0])) {
      memmove(&s.lastRing[0], &s.lastRing[1], (sizeof(s.lastRing) / sizeof(s.lastRing[0]) - 1) * sizeof(SaleLite));
      s.nLast--;
    }
    s.lastRing[s.nLast++] = liteOf(r);
    s.hasLast = true;
    copyStr(s.lastId, sizeof s.lastId, r.id);
    copyStr(s.lastSymbol, sizeof s.lastSymbol, r.symbol);
    copyStr(s.lastDate, sizeof s.lastDate, r.date);
    s.lastRealised = r.realised;
    s.lastTs = r.ts;
    yearAdd(s, r.date, r.realised);
  } else {
    if (s.count) s.count--;
    s.reversed++;
    s.pl += r.realised;                                 // the reversal carries -realised
    for (uint8_t i = 0; i < s.nLast; i++) {
      if (strcmp(s.lastRing[i].id, r.refId) != 0) continue;
      for (uint8_t j = i; j + 1 < s.nLast; j++) s.lastRing[j] = s.lastRing[j + 1];
      s.nLast--;
      break;
    }
    if (!strcmp(s.lastId, r.refId)) {
      if (s.nLast) {
        const SaleLite &t = s.lastRing[s.nLast - 1];
        copyStr(s.lastId, sizeof s.lastId, t.id);
        copyStr(s.lastSymbol, sizeof s.lastSymbol, t.symbol);
        copyStr(s.lastDate, sizeof s.lastDate, t.date);
        s.lastRealised = t.realised;
        s.lastTs = t.ts;
        s.hasLast = true;
      } else {
        s.hasLast = false;
        s.lastId[0] = s.lastSymbol[0] = s.lastDate[0] = 0;
        s.lastRealised = NAN;
        s.lastTs = 0;
      }
    }
    yearAdd(s, r.date, r.realised);
  }
  s.warn = s.records >= LEDGER_WARN_AT;
  s.full = s.records >= LEDGER_LIMIT;
}

// ---------------------------------------------------------------- boot scan
RecoveryPlan planRecovery(const ScanStats &st, const SaleRecord *pending, bool pendingOk, bool journalExists) {
  RecoveryPlan p;
  p.opened = st.opened;
  p.hasJournal = journalExists;
  p.appendPending = journalExists && pendingOk && pending && pending->seq > st.maxSeq;
  p.truncateTail = st.partialTail;
  p.clearJournal = journalExists;
  return p;
}

namespace {
struct RecoverCtx {
  Position *pos;
  uint8_t *n;
  PortfolioMeta *m;
  RealisedSummary *s;
  RecoverResult *res;
};

uint32_t txnOf(const char *id) {                        // "TX-000123" -> 123
  if (!id || id[0] != 'T' || id[1] != 'X' || id[2] != '-') return 0;
  return (uint32_t)strtoul(id + 3, nullptr, 10);
}

void onRecover(void *ctx, const SaleRecord &r, uint16_t) {
  RecoverCtx *c = static_cast<RecoverCtx *>(ctx);
  summaryAdd(*c->s, r);
  uint32_t txn = txnOf(r.id);
  if (txn >= c->m->nextTxn) c->m->nextTxn = txn + 1;    // never reuse a TX id after a crash
  if (r.seq <= c->m->appliedSeq) { c->res->alreadyApplied++; return; }
  char err[72];
  if (applyRecord(c->pos, *c->n, *c->m, r, err, sizeof err)) c->res->applied++;
  else c->res->failed++;
}
}  // namespace

RecoverResult recover(Position *pos, uint8_t &n, PortfolioMeta &m, RealisedSummary &s,
                      LineSrc &src, ScanStats &st) {
  RecoverResult res;
  memset(&res, 0, sizeof res);
  summaryReset(s);
  RecoverCtx c{pos, &n, &m, &s, &res};
  scan(src, onRecover, &c, st);
  // An empty ledger is a valid "no sales yet" (realised = 0). SaleStore downgrades `valid` when
  // the file exists but could not be read.
  return res;
}

uint8_t statusOf(const SaleRecord &r, bool reversed) {
  if (r.type == REC_REVERSAL) return REC_REVERSAL;
  return reversed ? SALE_ST_REVERSED : REC_SALE;
}

}  // namespace sale
