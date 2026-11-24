// SaleStore.cpp - LittleFS side of the sale ledger (see SaleStore.h).
#include "SaleStore.h"
#include "StorageManager.h"
#include <LittleFS.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <memory>
#include <new>
#include "../finance/SaleEngine.h"

namespace storage {

static const char *LEDGER_PATH  = "/sells.jsonl";
static const char *JOURNAL_PATH = "/txn.pending";
static const char *LEDGER_TMP   = "/sells.jsonl.tmp";

static uint32_t nowEpoch() {
  time_t t = time(nullptr);
  return t > 1600000000L ? (uint32_t)t : 0;
}

static void setErr(char *err, size_t errN, const char *msg) {
  if (err && errN) snprintf(err, errN, "%s", msg);
}

// ---------------------------------------------------------------- buffered line source
// One scan at a time (the firmware is single-threaded): reading through a 512 B block keeps the
// per-byte cost of Stream::read() off the hot path for a full 2000-record ledger.
struct FsLineSrc {
  File   f;
  size_t pos = 0, len = 0;
  bool   eof = false, term = false, overflow = false;
  char   buf[128];
};
static FsLineSrc s_ls;

static bool lsOpen(void *ctx) {
  FsLineSrc *s = static_cast<FsLineSrc *>(ctx);
  s->f = LittleFS.open(LEDGER_PATH, "r");
  if (!s->f) return false;
  s->pos = s->len = 0;
  s->eof = s->term = s->overflow = false;
  return true;
}

static int lsNext(void *ctx, char *out, size_t cap) {
  FsLineSrc *s = static_cast<FsLineSrc *>(ctx);
  if (!s || !s->f) return 0;
  s->term = false;
  s->overflow = false;
  size_t o = 0;
  bool got = false, done = false;
  while (!done) {
    while (s->pos < s->len) {
      char c = s->buf[s->pos++];
      got = true;
      if (c == '\n') { s->term = true; done = true; break; }
      if (o + 1 < cap) out[o++] = c;
      else s->overflow = true;                          // drain the rest, report the line as bad
    }
    if (done || s->eof) break;
    int n = s->f.read(reinterpret_cast<uint8_t *>(s->buf), sizeof s->buf);
    if (n <= 0) { s->eof = true; break; }
    s->pos = 0;
    s->len = (size_t)n;
  }
  if (!got) return 0;
  out[o] = 0;
  if (s->overflow) { s->overflow = false; s->term = true; return -1; }
  return (int)o;
}

static bool lsTerminated(void *ctx) { return static_cast<FsLineSrc *>(ctx)->term; }

static void lsClose(void *ctx) {
  FsLineSrc *s = static_cast<FsLineSrc *>(ctx);
  if (s->f) { s->f.close(); }
}

static sale::LineSrc makeSrc() {
  sale::LineSrc l;
  l.ctx = &s_ls;
  l.open = lsOpen;
  l.next = lsNext;
  l.terminated = lsTerminated;
  l.close = lsClose;
  return l;
}

// ---------------------------------------------------------------- append / journal
static bool ledgerAppend(const SaleRecord &r) {
  // Heap, not stack: this runs inside a web request (small task stack) and a 4 KB local would overflow it.
  char *line = (char *)malloc(LEDGER_LINE_MAX);
  if (!line) return false;
  size_t n = sale::recordLine(r, line, LEDGER_LINE_MAX);
    bool ok = false;
  if (n) {
    File f = LittleFS.open(LEDGER_PATH, "a");
    if (f) {
      size_t w = f.write(reinterpret_cast<const uint8_t *>(line), n);
      if (w == n) w += f.write(reinterpret_cast<const uint8_t *>("\n"), 1);
      f.close();
      ok = w == n + 1;
    }
  }
  free(line);
  return ok;
}

static bool journalWrite(const SaleRecord &r) {
  char *line = (char *)malloc(LEDGER_LINE_MAX);
  if (!line) return false;
  size_t n = sale::recordLine(r, line, LEDGER_LINE_MAX);
  bool ok = n && writeAtomic(JOURNAL_PATH, reinterpret_cast<const uint8_t *>(line), n);
  free(line);
  return ok;
}

static bool journalRead(SaleRecord &out) {
  if (!LittleFS.exists(JOURNAL_PATH)) return false;
  File f = LittleFS.open(JOURNAL_PATH, "r");
  if (!f) return false;
  size_t sz = f.size();
  if (sz == 0 || sz > LEDGER_LINE_MAX) { f.close(); return false; }
  char *buf = (char *)malloc(sz + 1);
  if (!buf) { f.close(); return false; }
  size_t got = f.readBytes(buf, sz);
  f.close();
  buf[got] = 0;
  bool ok = sale::parseRecord(buf, got, out);
  free(buf);
  return ok;
}

static void journalClear() {
  if (LittleFS.exists(JOURNAL_PATH)) LittleFS.remove(JOURNAL_PATH);
}

// Drops everything after the last '\n' (an append that was interrupted by a power cut).
static bool truncateTail() {
  File f = LittleFS.open(LEDGER_PATH, "r");
  if (!f) return true;                                  // nothing to repair
  char buf[512];
  size_t off = 0, lastNl = 0;
  while (f.available()) {
    int n = f.read(reinterpret_cast<uint8_t *>(buf), sizeof buf);
    if (n <= 0) break;
    for (int i = 0; i < n; i++) if (buf[i] == '\n') lastNl = off + (size_t)i + 1;
    off += (size_t)n;
  }
  f.close();
  if (lastNl == 0) {                                    // the whole file is one partial line
    LittleFS.remove(LEDGER_PATH);
    Serial.println(F("[ledger] dropped an incomplete record"));
    return true;
  }
  if (lastNl == off) return true;                       // already '\n'-terminated
  File src = LittleFS.open(LEDGER_PATH, "r");
  if (!src) return false;
  String tmp = String(LEDGER_PATH) + ".tmp";
  File dst = LittleFS.open(tmp, "w");
  if (!dst) { src.close(); return false; }
  size_t left = lastNl;
  bool ok = true;
  while (left) {
    size_t want = left < sizeof buf ? left : sizeof buf;
    int n = src.read(reinterpret_cast<uint8_t *>(buf), want);
    if (n <= 0) { ok = false; break; }
    if (dst.write(reinterpret_cast<const uint8_t *>(buf), (size_t)n) != (size_t)n) { ok = false; break; }
    left -= (size_t)n;
  }
  src.close();
  dst.close();
  if (!ok) { LittleFS.remove(tmp); return false; }
  LittleFS.remove(LEDGER_PATH);
  if (!LittleFS.rename(tmp, LEDGER_PATH)) { LittleFS.remove(tmp); return false; }
  Serial.printf("[ledger] repaired a partial record (%u bytes kept)\n", (unsigned)lastNl);
  return true;
}

// ---------------------------------------------------------------- preview / commit
bool salePreview(AppState &a, const SaleRequest &req, SalePreview &pv) {
  return sale::preview(a.positions, a.posCount, a.pmeta, req, nowEpoch(), pv);
}

static bool commitRecord(AppState &a, SaleRecord &r, char *err, size_t errN, int *code) {
  auto fail = [&](const char *msg, int c) {
    setErr(err, errN, msg);
    if (code) *code = c;
    return false;
  };
  if (a.realised.records >= LEDGER_LIMIT) return fail("sale ledger is full (2000 records)", 409);
  if (!journalWrite(r)) return fail("cannot write the transaction journal", 500);
  if (!ledgerAppend(r)) {
    journalClear();                                     // abort: nothing was applied, nothing to replay
    return fail("cannot append to the sale ledger", 500);
  }
  // From here on the record is durable: the summary must include it even if a later step fails,
  // because the next boot would read it from the file anyway.
  sale::summaryAdd(a.realised, r);
  char applyErr[72] = "";
  bool applied = sale::applyRecord(a.positions, a.posCount, a.pmeta, r, applyErr, sizeof applyErr);
  if (a.pmeta.nextTxn == 0) a.pmeta.nextTxn = 1;
  a.pmeta.nextTxn++;
  bool saved = savePortfolio(a);
  if (applied && saved) { journalClear(); if (code) *code = 200; return true; }
  if (!applied) return fail(applyErr[0] ? applyErr : "could not apply the record", 500);
  return fail("recorded, but saving the portfolio failed (replayed at next boot)", 500);
}

bool saleCommit(AppState &a, const SaleRequest &req, SalePreview &pv, char *err, size_t errN, int *code) {
  if (code) *code = 400;
  if (!sale::preview(a.positions, a.posCount, a.pmeta, req, nowEpoch(), pv)) {
    setErr(err, errN, pv.error[0] ? pv.error : "invalid sale");
    return false;
  }
  if (!req.token || req.token != pv.token) {
    setErr(err, errN, "portfolio changed - run the preview again");
    if (code) *code = 409;
    return false;
  }
  if (a.pmeta.nextTxn == 0) a.pmeta.nextTxn = 1;
  // Heap, not stack: a SaleRecord is ~730 B and this runs inside a web request on the ~4 KB cont stack
  // (together with the request body parser and the preview it overflowed and reset the chip).
  SaleRecord *r = (SaleRecord *)calloc(1, sizeof(SaleRecord));
  if (!r) { setErr(err, errN, "out of memory"); if (code) *code = 500; return false; }
  sale::makeRecord(pv, a.pmeta.appliedSeq + 1, nowEpoch(), a.pmeta, *r);
  bool ok = commitRecord(a, *r, err, errN, code);
  free(r);
  return ok;
}

// ---------------------------------------------------------------- history helpers
struct FindCtx {
  uint32_t seq;          // 0 = newest sale
  SaleRecord rec;
  bool found;
};

static void findCb(void *ctx, const SaleRecord &r, uint16_t) {
  FindCtx *c = static_cast<FindCtx *>(ctx);
  if (c->seq) {
    if (r.seq == c->seq) { c->rec = r; c->found = true; }
  } else if (r.type == REC_SALE) {
    if (!c->found || r.seq > c->rec.seq) { c->rec = r; c->found = true; }
  }
}

// seq numbers are dense and <= LEDGER_LIMIT, so "which sales are reversed" fits in 250 bytes.
struct ReversedBits {
  uint8_t bit[(LEDGER_LIMIT + 7) / 8];
};
static void markReversedCb(void *ctx, const SaleRecord &r, uint16_t) {
  ReversedBits *b = static_cast<ReversedBits *>(ctx);
  if (r.type != REC_REVERSAL || r.refSeq == 0 || r.refSeq > LEDGER_LIMIT) return;
  uint32_t i = r.refSeq - 1;
  b->bit[i >> 3] |= (uint8_t)(1u << (i & 7));
}
static bool isReversed(const ReversedBits &b, uint32_t seq) {
  if (!seq || seq > LEDGER_LIMIT) return false;
  uint32_t i = seq - 1;
  return (b.bit[i >> 3] & (uint8_t)(1u << (i & 7))) != 0;
}

static bool saleReverseImpl(AppState &a, uint32_t seq, char *err, size_t errN, int *code, FindCtx &fc, SaleRecord &rev) {
  auto fail = [&](const char *msg, int c) {
    setErr(err, errN, msg);
    if (code) *code = c;
    return false;
  };
  if (code) *code = 400;
  sale::LineSrc src = makeSrc();
  sale::ScanStats st;
  memset(&fc, 0, sizeof fc);
  fc.seq = seq;
  sale::scan(src, findCb, &fc, st);
  if (!fc.found) return fail("record not found", 404);
  if (fc.rec.type != REC_SALE) return fail("only a sale can be reversed", 400);

  std::unique_ptr<ReversedBits> bitsp(new (std::nothrow) ReversedBits);
  if (!bitsp) { setErr(err, errN, "out of memory"); if (code) *code = 500; return false; }
  ReversedBits &bits = *bitsp;
  memset(&bits, 0, sizeof bits);
  sale::LineSrc src2 = makeSrc();
  sale::ScanStats st2;
  sale::scan(src2, markReversedCb, &bits, st2);
  if (isReversed(bits, fc.rec.seq)) return fail("this sale has already been reversed", 409);

  if (a.pmeta.nextTxn == 0) a.pmeta.nextTxn = 1;
  memset(&rev, 0, sizeof rev);
  char buildErr[72] = "";
  if (!sale::makeReversal(a.positions, a.posCount, fc.rec, a.pmeta.appliedSeq + 1, nowEpoch(), a.pmeta, rev, buildErr, sizeof buildErr)) {
    return fail(buildErr[0] ? buildErr : "cannot reverse this record", 400);
  }
  return commitRecord(a, rev, err, errN, code);
}

bool saleReverse(AppState &a, uint32_t seq, char *err, size_t errN, int *code) {
  // two ~730 B records: keep them off the request stack
  FindCtx *fc = (FindCtx *)calloc(1, sizeof(FindCtx));
  SaleRecord *rev = (SaleRecord *)calloc(1, sizeof(SaleRecord));
  bool ok = false;
  if (fc && rev) ok = saleReverseImpl(a, seq, err, errN, code, *fc, *rev);
  else { setErr(err, errN, "out of memory"); if (code) *code = 500; }
  free(fc); free(rev);
  return ok;
}

// ---------------------------------------------------------------- delete a transaction
struct DelFind {
  uint32_t target;               // pass 1: the record the caller named
  bool     found;
  uint8_t  type;
  uint32_t ref;                  // its refSeq (reversal -> the sale)
  uint32_t saleSeq, revSeq;      // pass 2: the pair
};
static void delFind1(void *ctx, const SaleRecord &r, uint16_t) {
  DelFind *c = static_cast<DelFind *>(ctx);
  if (r.seq == c->target) { c->found = true; c->type = r.type; c->ref = r.refSeq; }
}
static void delFind2(void *ctx, const SaleRecord &r, uint16_t) {
  DelFind *c = static_cast<DelFind *>(ctx);
  if (r.type == REC_REVERSAL && r.refSeq == c->saleSeq) c->revSeq = r.seq;
}

struct RewriteCtx {
  File     f;
  char    *line;
  uint32_t a, b;
  uint16_t kept;
  bool     ok;
};
static void rewriteCb(void *ctx, const SaleRecord &cr, uint16_t) {
  RewriteCtx *c = static_cast<RewriteCtx *>(ctx);
  if (!c->ok || cr.seq == c->a || cr.seq == c->b) return;
  SaleRecord &r = const_cast<SaleRecord &>(cr);          // scan()'s own scratch record
  r.seq = sale::seqAfterDelete(r.seq, c->a, c->b);
  if (r.refSeq) r.refSeq = sale::seqAfterDelete(r.refSeq, c->a, c->b);
  size_t n = sale::recordLine(r, c->line, LEDGER_LINE_MAX);
  if (!n) { c->ok = false; return; }
  if (c->f.write(reinterpret_cast<const uint8_t *>(c->line), n) != n || c->f.write(reinterpret_cast<const uint8_t *>("\n"), 1) != 1) { c->ok = false; return; }
  c->kept++;
}
static void summaryCb(void *ctx, const SaleRecord &r, uint16_t) { sale::summaryAdd(*static_cast<RealisedSummary *>(ctx), r); }

// Copies the ledger without records a/b (renumbering the rest) to a temp file and renames it over.
static bool rewriteWithout(uint32_t a, uint32_t b, char *err, size_t errN) {
  RewriteCtx c;
  c.line = (char *)malloc(LEDGER_LINE_MAX);
  c.a = a; c.b = b; c.kept = 0; c.ok = true;
  if (!c.line) { setErr(err, errN, "out of memory"); return false; }
  LittleFS.remove(LEDGER_TMP);
  c.f = LittleFS.open(LEDGER_TMP, "w");
  if (!c.f) { free(c.line); setErr(err, errN, "cannot create the temporary ledger"); return false; }
  sale::LineSrc src = makeSrc();
  sale::ScanStats st;
  sale::scan(src, rewriteCb, &c, st);
  c.f.close();
  free(c.line);
  if (!c.ok || st.badLines || st.partialTail) {
    LittleFS.remove(LEDGER_TMP);
    setErr(err, errN, "could not rewrite the ledger (flash full?)");
    return false;
  }
  // rename over the old file; if the FS refuses to replace, remove first (boot finishes a rename
  // that was cut between the two steps: ledgerBoot() promotes a leftover temp when the ledger is gone)
  if (!LittleFS.rename(LEDGER_TMP, LEDGER_PATH)) {
    LittleFS.remove(LEDGER_PATH);
    if (!LittleFS.rename(LEDGER_TMP, LEDGER_PATH)) { setErr(err, errN, "could not replace the ledger"); return false; }
  }
  return true;
}

bool saleDelete(AppState &a, uint32_t seq, char *err, size_t errN, int *code) {
  auto fail = [&](const char *msg, int cd) { setErr(err, errN, msg); if (code) *code = cd; return false; };
  if (code) *code = 400;
  if (!seq) return fail("seq required", 400);
  std::unique_ptr<DelFind> f(new (std::nothrow) DelFind);
  if (!f) return fail("out of memory", 500);
  memset(f.get(), 0, sizeof(DelFind));
  f->target = seq;
  {
    sale::LineSrc src = makeSrc();
    sale::ScanStats st;
    sale::scan(src, delFind1, f.get(), st);
    if (!f->found) return fail("record not found", 404);
    if (st.badLines || st.partialTail) return fail("the ledger has unreadable lines - export the CSV first", 409);
  }
  f->saleSeq = f->type == REC_REVERSAL ? f->ref : seq;
  for (int pass = 0; pass < 2; pass++) {
    f->revSeq = 0;
    sale::LineSrc src = makeSrc();
    sale::ScanStats st;
    sale::scan(src, delFind2, f.get(), st);
    if (f->revSeq) break;
    if (pass == 1) return fail("could not reverse the sale", 500);
    // still active: undo it first (restores the position), then remove the pair
    int rc = 500;
    char rerr[80] = "";
    if (!saleReverse(a, f->saleSeq, rerr, sizeof rerr, &rc)) return fail(rerr[0] ? rerr : "could not reverse the sale", rc);
  }
  uint32_t sa = f->saleSeq, sb = f->revSeq;
  busyBegin();
  bool ok = rewriteWithout(sa, sb, err, errN);
  busyEnd();
  if (!ok) { if (code) *code = 500; return false; }

  a.pmeta.appliedSeq = sale::seqAfterDelete(a.pmeta.appliedSeq, sa, sb);
  a.pmeta.ver++;                                        // invalidates any open preview token
  sale::summaryReset(a.realised);
  {
    sale::LineSrc src = makeSrc();
    sale::ScanStats st;
    sale::scan(src, summaryCb, &a.realised, st);
    if (!st.opened && LittleFS.exists(LEDGER_PATH)) a.realised.valid = false;
  }
  if (!savePortfolio(a)) return fail("removed, but saving the portfolio failed (repaired at next boot)", 500);
  if (code) *code = 200;
  return true;
}

struct CountCtx { const char *symbol; uint16_t n; };
static void countCb(void *ctx, const SaleRecord &r, uint16_t) {
  CountCtx *c = static_cast<CountCtx *>(ctx);
  if (c->symbol[0] && strcmp(c->symbol, r.symbol) != 0) return;
  if (c->n < 0xFFFF) c->n++;
}

struct PageCtx {
  uint16_t start, end;            // window over the index of *matching* records
  SaleLite *rows;
  uint16_t cap, count;
  const ReversedBits *bits;
  const char *symbol;
  uint16_t seen;
};

static void pageCb(void *ctx, const SaleRecord &r, uint16_t) {
  PageCtx *c = static_cast<PageCtx *>(ctx);
  if (c->symbol[0] && strcmp(c->symbol, r.symbol) != 0) return;
  uint16_t index = c->seen++;
  if (index < c->start || index >= c->end || c->count >= c->cap) return;
  SaleLite &l = c->rows[c->count++];
  memset(&l, 0, sizeof l);
  l.seq = r.seq;
  l.ts = r.ts;
  snprintf(l.id, sizeof l.id, "%s", r.id);
  snprintf(l.refId, sizeof l.refId, "%s", r.refId);
  snprintf(l.symbol, sizeof l.symbol, "%s", r.symbol);
  snprintf(l.date, sizeof l.date, "%s", r.date);
  l.type = r.type;
  l.priceSource = r.priceSource;
  l.status = (r.type == REC_SALE) ? (isReversed(*c->bits, r.seq) ? SALE_ST_REVERSED : REC_SALE) : REC_REVERSAL;
  l.qty = r.qty;
  l.price = r.price;
  l.refPrice = r.refPrice;
  l.fee = r.fee;
  l.gross = r.gross;
  l.net = r.net;
  l.cost = r.cost;
  l.realised = r.realised;
}

bool salePage(const AppState &a, const char *symbol, uint16_t offset, uint16_t limit, SalePage &out, char *err, size_t errN) {
  (void)a;
  memset(&out, 0, sizeof out);
  if (!symbol) symbol = "";
  if (limit == 0 || limit > SALES_PAGE_MAX) limit = SALES_PAGE_MAX;
  std::unique_ptr<ReversedBits> bitsp(new (std::nothrow) ReversedBits);
  if (!bitsp) { setErr(err, errN, "out of memory"); return false; }
  ReversedBits &bits = *bitsp;
  memset(&bits, 0, sizeof bits);
  sale::LineSrc src = makeSrc();
  sale::ScanStats st;
  sale::scan(src, markReversedCb, &bits, st);
  if (!st.opened && LittleFS.exists(LEDGER_PATH)) { setErr(err, errN, "cannot read the sale ledger"); return false; }
  uint16_t total;
  if (symbol[0]) {
    CountCtx cc{symbol, 0};
    sale::LineSrc srcA = makeSrc();
    sale::ScanStats stA;
    sale::scan(srcA, countCb, &cc, stA);
    total = cc.n;
  } else {
    total = st.records;
  }
  out.total = total;
  if (offset >= total || !total) return true;
  uint16_t count = (uint16_t)(total - offset);
  if (count > limit) count = limit;
  PageCtx pc;
  pc.start = (uint16_t)(total - offset - count);
  pc.end = (uint16_t)(total - offset);
  pc.rows = out.rows;
  pc.cap = SALES_PAGE_MAX;
  pc.count = 0;
  pc.bits = &bits;
  pc.symbol = symbol;
  pc.seen = 0;
  sale::LineSrc src2 = makeSrc();
  sale::ScanStats st2;
  sale::scan(src2, pageCb, &pc, st2);
  out.count = pc.count;
  return true;
}

bool saleReversed(const AppState &a, uint32_t seq, bool &out) {
  (void)a;
  out = false;
  if (!seq) return false;
  std::unique_ptr<ReversedBits> bitsp(new (std::nothrow) ReversedBits);
  if (!bitsp) return false;
  ReversedBits &bits = *bitsp;
  memset(&bits, 0, sizeof bits);
  sale::LineSrc src = makeSrc();
  sale::ScanStats st;
  sale::scan(src, markReversedCb, &bits, st);
  out = isReversed(bits, seq);
  return st.opened || !LittleFS.exists(LEDGER_PATH);
}

// ---------------------------------------------------------------- CSV
struct CsvCtx {
  CsvSink sink;
  void *ctx;
  const ReversedBits *bits;
  const char *symbol;
};

static void csvLine(const SaleRecord &r, const ReversedBits &bits, CsvSink sink, void *ctx) {
  char lots[256];
  size_t lo = 0;
  lots[0] = 0;
  for (uint8_t i = 0; i < r.nAlloc; i++) {
    const LotAlloc &a = r.alloc[i];
    double sold = r.type == REC_SALE ? a.before - a.after : a.after - a.before;
    int n = snprintf(lots + lo, sizeof lots - lo, "%s%u*%.8f@%.6f", i ? " " : "", (unsigned)a.lot, sold, a.buy);
    if (n < 0 || (size_t)n >= sizeof lots - lo) break;
    lo += (size_t)n;
  }
  char refPx[32];
  if (isNum(r.refPrice)) snprintf(refPx, sizeof refPx, "%.6f", r.refPrice);
  else refPx[0] = 0;                                    // unknown -> empty, never 0
  char line[704];
  int n = snprintf(line, sizeof line,
                   "%lu,\"%s\",%s,%s,%lu,\"%s\",\"%s\",%.8f,%.6f,%s,%s,%.4f,%.6f,%.6f,%.6f,%.6f,%lu,\"%s\",\"%s\"\n",
                   (unsigned long)r.seq, r.id,
                   r.type == REC_SALE ? "sale" : "reversal",
                   r.type == REC_REVERSAL ? "reversal" : (isReversed(bits, r.seq) ? "reversed" : "valid"),
                   (unsigned long)r.ts, r.symbol, r.date, r.qty, r.price,
                   refPx,
                   r.priceSource == PRICE_YAHOO ? "yahoo" : "manual",
                   r.fee, r.gross, r.net, r.cost, r.realised,
                   (unsigned long)r.refSeq, r.refId, lots);
  if (n < 0) return;
  if ((size_t)n >= sizeof line) n = (int)sizeof line - 1;
  sink(ctx, line, (size_t)n);
}

bool saleCsv(const AppState &a, const char *symbol, CsvSink sink, void *ctx, char *err, size_t errN) {
  (void)a;
  if (!sink) { setErr(err, errN, "no sink"); return false; }
  if (!symbol) symbol = "";
  std::unique_ptr<ReversedBits> bitsp(new (std::nothrow) ReversedBits);
  if (!bitsp) { setErr(err, errN, "out of memory"); return false; }
  ReversedBits &bits = *bitsp;
  memset(&bits, 0, sizeof bits);
  sale::LineSrc src = makeSrc();
  sale::ScanStats st;
  sale::scan(src, markReversedCb, &bits, st);
  if (!st.opened && LittleFS.exists(LEDGER_PATH)) { setErr(err, errN, "cannot read the sale ledger"); return false; }
  const char *header = "seq,id,type,status,ts,symbol,date,qty,price,refPrice,priceSource,fee,gross,net,cost,realised,refSeq,refId,lots\n";
  sink(ctx, header, strlen(header));
  CsvCtx cc{sink, ctx, &bits, symbol};
  struct Emit {
    static void cb(void *c, const SaleRecord &r, uint16_t) {
      CsvCtx *e = static_cast<CsvCtx *>(c);
      if (e->symbol[0] && strcmp(e->symbol, r.symbol) != 0) return;
      csvLine(r, *e->bits, e->sink, e->ctx);
    }
  };
  sale::LineSrc src2 = makeSrc();
  sale::ScanStats st2;
  sale::scan(src2, Emit::cb, &cc, st2);
  return true;
}

// ---------------------------------------------------------------- boot recovery
bool ledgerBoot(AppState &a) {
  if (LittleFS.exists(LEDGER_TMP)) {                    // a delete/rewrite was cut by a power loss
    if (!LittleFS.exists(LEDGER_PATH)) { LittleFS.rename(LEDGER_TMP, LEDGER_PATH); Serial.println(F("[ledger] finished an interrupted rewrite")); }
    else LittleFS.remove(LEDGER_TMP);                   // the old ledger is intact: drop the half-written copy
  }
  bool exists = LittleFS.exists(LEDGER_PATH);
  bool journalExists = LittleFS.exists(JOURNAL_PATH);
  SaleRecord pending;
  memset(&pending, 0, sizeof pending);
  bool pendingOk = journalRead(pending);

  sale::LineSrc src = makeSrc();
  sale::ScanStats st;
  sale::scan(src, nullptr, nullptr, st);                // phase 1: read-only stats

  sale::RecoveryPlan plan = sale::planRecovery(st, pendingOk ? &pending : nullptr, pendingOk, journalExists);
  bool fsOk = true;
  if (plan.truncateTail) fsOk = truncateTail();         // never append behind a partial line
  if (fsOk && plan.appendPending) fsOk = ledgerAppend(pending);
  if (fsOk && plan.clearJournal) journalClear();

  uint32_t ver0 = a.pmeta.ver, seq0 = a.pmeta.appliedSeq, txn0 = a.pmeta.nextTxn;
  sale::LineSrc src2 = makeSrc();
  sale::ScanStats st2;
  sale::RecoverResult rr = sale::recover(a.positions, a.posCount, a.pmeta, a.realised, src2, st2);
  a.realised.recovery = rr.applied > 0;
  a.realised.badLine = st2.badLines > 0;
  if (exists && !st2.opened) a.realised.valid = false;  // present but unreadable -> N/A, not 0
  if (a.pmeta.nextTxn == 0) a.pmeta.nextTxn = 1;
  if (st2.opened && a.pmeta.appliedSeq > st2.maxSeq) a.pmeta.appliedSeq = st2.maxSeq;   // a delete renumbered the ledger, the portfolio save was cut

  bool changed = a.pmeta.ver != ver0 || a.pmeta.appliedSeq != seq0 || a.pmeta.nextTxn != txn0;
  if (changed && !savePortfolio(a)) Serial.println(F("[ledger] saving the repaired portfolio failed"));
  if (rr.applied || rr.failed || st2.badLines || plan.appendPending || plan.truncateTail) {
    Serial.printf("[ledger] records=%u applied=%u already=%u failed=%u bad=%u tail=%d journal=%d\n",
                  (unsigned)st2.records, (unsigned)rr.applied, (unsigned)rr.alreadyApplied,
                  (unsigned)rr.failed, (unsigned)st2.badLines, (int)plan.truncateTail, (int)plan.appendPending);
  }
  return fsOk && rr.failed == 0;
}

}  // namespace storage
