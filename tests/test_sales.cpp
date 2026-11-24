// Host unit tests for the pure sale engine (allocation, records, apply/undo, ledger scan,
// recovery, aggregates) and for the lifetime P/L identity in appRecompute().
// Build/run: see tests/run_tests.py
#include "../src/finance/SaleEngine.h"
#include "../src/core/AppState.h"
#include "../src/core/JsonWriter.h"
#include "../src/core/MarketClock.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <string>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_STR(a, b) do { const char *_a = (a), *_b = (b); if (_a && !strcmp(_a, _b)) g_pass++; else { g_fail++; printf("FAIL %s:%d  got \"%s\" want \"%s\"\n", __FILE__, __LINE__, _a ? _a : "(null)", _b); } } while (0)
#define CHECK_NEAR(a, b, e) do { double _a = (a), _b = (b); if (fabs(_a - _b) <= (e)) g_pass++; else { g_fail++; printf("FAIL %s:%d  got %.9f want %.9f\n", __FILE__, __LINE__, _a, _b); } } while (0)

static bool errHas(const SalePreview &pv, const char *sub) { return !pv.ok && pv.error[0] && strstr(pv.error, sub) != nullptr; }

static void addLot(Position *pos, uint8_t &n, PortfolioMeta &m, const char *sym, const char *date, double qty, double buy) {
  Position &p = pos[n++];
  memset(&p, 0, sizeof p);
  strcpy(p.symbol, sym);
  strcpy(p.date, date);
  p.qty = p.oqty = qty;
  p.buy = buy;
  if (!m.nextLotId) m.nextLotId = 1;
  p.id = m.nextLotId++;
  m.ver++;
}

// The standard fixture: two open ^GSPC lots (10 @ 6000, 5 @ 6100).
static void twoLots(Position *pos, uint8_t &n, PortfolioMeta &m) {
  n = 0;
  memset(&m, 0, sizeof m);
  addLot(pos, n, m, "^GSPC", "2026-01-02", 10, 6000);
  addLot(pos, n, m, "^GSPC", "2026-02-02", 5, 6100);
}

static const Position *byId(const Position *pos, uint8_t n, uint16_t id) {
  for (uint8_t i = 0; i < n; i++) if (pos[i].id == id) return &pos[i];
  return nullptr;
}

static SaleRequest R(const char *sym, const char *date) {
  SaleRequest r;
  memset(&r, 0, sizeof r);
  strcpy(r.symbol, sym);
  strcpy(r.date, date);
  r.method = SALE_FIFO;
  r.mode = SALE_BY_QTY;
  r.price = r.refPrice = NAN;
  r.fee = 0;
  r.priceSource = PRICE_MANUAL;
  return r;
}

// ---------------------------------------------------------------- allocation & preview
static void testPreview() {
  Position pos[MAX_POSITIONS]; uint8_t n; PortfolioMeta m;
  twoLots(pos, n, m);
  const uint32_t now = utcMidnight(2026, 11, 20);
  SalePreview pv;

  // 1. FIFO takes the oldest lot first and stops in the middle of the second one
  SaleRequest r = R("^GSPC", "2026-11-19");
  r.qty = 12; r.price = 6200; r.fee = 10;
  CHECK(sale::preview(pos, n, m, r, now, pv) && pv.ok);
  CHECK(pv.nAlloc == 2 && pv.alloc[0].lot == 1 && pv.alloc[0].before == 10 && pv.alloc[0].after == 0 &&
        pv.alloc[1].lot == 2 && pv.alloc[1].before == 5 && pv.alloc[1].after == 3);
  CHECK_NEAR(pv.qty, 12, 1e-9);
  CHECK_NEAR(pv.available, 15, 1e-9);
  CHECK_NEAR(pv.cost, 10 * 6000 + 2 * 6100, 1e-6);          // 72200 (2 units left in lot 2)
  CHECK_NEAR(pv.gross, 12 * 6200, 1e-6);                    // 74400
  CHECK_NEAR(pv.net, 74400 - 10, 1e-6);
  CHECK_NEAR(pv.realised, 74390 - 72200, 1e-6);             // 2190
  CHECK(pv.token != 0 && pv.token == sale::previewToken(pos, n, m));

  // 2. specific lot: only the chosen lot, its own cost basis
  SaleRequest s = R("^GSPC", "2026-11-19");
  s.method = SALE_SPECIFIC; s.lotId = 2; s.qty = 2; s.price = 6200;
  CHECK(sale::preview(pos, n, m, s, now, pv) && pv.ok);
  CHECK(pv.nAlloc == 1 && pv.alloc[0].lot == 2 && pv.alloc[0].before == 5 && pv.alloc[0].after == 3);
  CHECK_NEAR(pv.cost, 2 * 6100, 1e-6);
  CHECK_NEAR(pv.realised, 2 * 6200 - 12200, 1e-6);

  // 3. by-THB: qty = amount / price, then the same FIFO walk
  SaleRequest t = R("^GSPC", "2026-11-19");
  t.mode = SALE_BY_THB; t.amount = 62000; t.price = 6200;
  CHECK(sale::preview(pos, n, m, t, now, pv) && pv.ok);
  CHECK_NEAR(pv.qty, 10, 1e-9);
  CHECK(pv.nAlloc == 1 && pv.alloc[0].lot == 1 && pv.alloc[0].after == 0);
  CHECK_NEAR(pv.realised, 62000 - 60000, 1e-6);

  // 4. sell everything
  SaleRequest all = R("^GSPC", "2026-11-19");
  all.mode = SALE_BY_ALL; all.price = 6200;
  CHECK(sale::preview(pos, n, m, all, now, pv) && pv.ok);
  CHECK_NEAR(pv.qty, 15, 1e-9);
  CHECK(pv.nAlloc == 2 && pv.alloc[1].after == 0);

  // 5. over-sale
  SaleRequest over = R("^GSPC", "2026-11-19");
  over.qty = 100; over.price = 6200;
  CHECK(!sale::preview(pos, n, m, over, now, pv) && errHas(pv, "available"));

  // 6. unknown symbol
  SaleRequest unk = R("AAPL", "2026-11-19");
  unk.qty = 1; unk.price = 100;
  CHECK(!sale::preview(pos, n, m, unk, now, pv) && errHas(pv, "no open position"));

  // 7. future date
  SaleRequest fut = R("^GSPC", "2027-01-01");
  fut.qty = 1; fut.price = 6200;
  CHECK(!sale::preview(pos, n, m, fut, now, pv) && errHas(pv, "future"));

  // 8. invalid date
  SaleRequest bd = R("^GSPC", "2026-02-30");
  bd.qty = 1; bd.price = 6200;
  CHECK(!sale::preview(pos, n, m, bd, now, pv) && errHas(pv, "invalid date"));

  // 9. missing price (NaN) - never treated as 0
  SaleRequest noPx = R("^GSPC", "2026-11-19");
  noPx.qty = 1;
  CHECK(!sale::preview(pos, n, m, noPx, now, pv) && errHas(pv, "price"));

  // 10. negative fee
  SaleRequest fee = R("^GSPC", "2026-11-19");
  fee.qty = 1; fee.price = 6200; fee.fee = -5;
  CHECK(!sale::preview(pos, n, m, fee, now, pv) && errHas(pv, "fee"));

  // 11. specific lot rules: no lotId / unknown lot / lot smaller than the requested quantity
  SaleRequest noLot = R("^GSPC", "2026-11-19");
  noLot.method = SALE_SPECIFIC; noLot.qty = 1; noLot.price = 6200;
  CHECK(!sale::preview(pos, n, m, noLot, now, pv) && errHas(pv, "choose a lot"));
  SaleRequest wrongLot = noLot; wrongLot.lotId = 99;
  CHECK(!sale::preview(pos, n, m, wrongLot, now, pv) && errHas(pv, "lot not found"));
  SaleRequest tooBig = noLot; tooBig.lotId = 2; tooBig.qty = 9;
  CHECK(!sale::preview(pos, n, m, tooBig, now, pv) && errHas(pv, "lot has only"));

  // 12. zero quantity
  SaleRequest zero = R("^GSPC", "2026-11-19");
  zero.qty = 0; zero.price = 6200;
  CHECK(!sale::preview(pos, n, m, zero, now, pv) && errHas(pv, "quantity"));
}

// ---------------------------------------------------------------- token binding
static void testToken() {
  Position pos[MAX_POSITIONS]; uint8_t n; PortfolioMeta m;
  twoLots(pos, n, m);
  const uint32_t now = utcMidnight(2026, 11, 20);
  SaleRequest r = R("^GSPC", "2026-11-19");
  r.qty = 12; r.price = 6200;

  uint32_t t1 = sale::previewToken(pos, n, m);
  SalePreview pv;
  CHECK(sale::preview(pos, n, m, r, now, pv) && pv.token == t1);
  CHECK(sale::preview(pos, n, m, r, now, pv) && pv.token == t1);            // stable for the same state

  // any lot mutation invalidates the token
  pos[0].qty -= 1;
  CHECK(sale::previewToken(pos, n, m) != t1);
  pos[0].qty += 1;
  m.ver++;
  CHECK(sale::previewToken(pos, n, m) != t1);
  m.ver--;
  m.appliedSeq++;
  CHECK(sale::previewToken(pos, n, m) != t1);
}

// ---------------------------------------------------------------- records
static void testRecords() {
  Position pos[MAX_POSITIONS]; uint8_t n; PortfolioMeta m;
  twoLots(pos, n, m);
  m.nextTxn = 1;
  const uint32_t now = utcMidnight(2026, 11, 20);
  SaleRequest r = R("^GSPC", "2026-11-19");
  r.qty = 12; r.price = 6200; r.fee = 10; r.refPrice = 6180;
  SalePreview pv;
  CHECK(sale::preview(pos, n, m, r, now, pv) && pv.ok);

  SaleRecord rec;
  sale::makeRecord(pv, 1, 1700000000, m, rec);
  CHECK_STR(rec.id, "TX-000001");
  CHECK(rec.type == REC_SALE && rec.seq == 1 && rec.ts == 1700000000 && rec.nAlloc == 2);
  CHECK_STR(rec.symbol, "^GSPC");
  CHECK(rec.priceSource == PRICE_MANUAL);

  // line -> parse -> identical numbers (8 dp quantities, 2-6 dp money)
  char buf[1024];
  size_t len = sale::recordLine(rec, buf, sizeof buf);
  CHECK(len > 0 && len < sizeof buf && buf[len] == 0);
  SaleRecord back;
  CHECK(sale::parseRecord(buf, len, back));
  CHECK(back.seq == rec.seq && !strcmp(back.id, rec.id) && back.type == REC_SALE &&
        !strcmp(back.symbol, rec.symbol) && !strcmp(back.date, rec.date));
  CHECK_NEAR(back.qty, rec.qty, 1e-8);
  CHECK_NEAR(back.price, rec.price, 1e-9);
  CHECK_NEAR(back.fee, rec.fee, 1e-9);
  CHECK_NEAR(back.realised, rec.realised, 1e-6);
  CHECK_NEAR(back.refPrice, 6180, 1e-9);
  CHECK(back.nAlloc == 2 && back.alloc[0].lot == 1 && back.alloc[1].lot == 2 &&
        fabs(back.alloc[0].before - 10) < 1e-8 && fabs(back.alloc[0].after) < 1e-8 &&
        fabs(back.alloc[1].after - 3) < 1e-8 && !strcmp(back.alloc[1].date, "2026-02-02"));

  // priceSource survives the round trip as a string, NaN refPrice stays unknown
  SaleRecord y = rec;
  y.priceSource = PRICE_YAHOO;
  y.refPrice = NAN;
  len = sale::recordLine(y, buf, sizeof buf);
  CHECK(sale::parseRecord(buf, len, back) && back.priceSource == PRICE_YAHOO && isnan(back.refPrice));

  // malformed input is never a record
  CHECK(!sale::parseRecord("not json", 8, back));
  CHECK(!sale::parseRecord("{\"seq\":1}", 9, back));
  CHECK(!sale::parseRecord("{\"seq\":0,\"id\":\"TX-1\",\"symbol\":\"X\","
                           "\"alloc\":[{\"lot\":1,\"buy\":1,\"before\":1,\"after\":0}]}", 79, back));
  CHECK(sale::recordLine(rec, buf, 8) == 0);                 // buffer too small -> 0, never a truncated line
}

// ---------------------------------------------------------------- apply / undo
static void testApplyAndReverse() {
  Position pos[MAX_POSITIONS]; uint8_t n; PortfolioMeta m;
  twoLots(pos, n, m);
  m.nextTxn = 1;
  const uint32_t now = utcMidnight(2026, 11, 20);
  SaleRequest r = R("^GSPC", "2026-11-19");
  r.qty = 12; r.price = 6200; r.fee = 10;
  SalePreview pv;
  CHECK(sale::preview(pos, n, m, r, now, pv) && pv.ok);
  SaleRecord rec;
  sale::makeRecord(pv, 1, 1700000000, m, rec);

  char err[80] = "";
  // 1. apply: absolute lot quantities, the fully sold lot leaves the open list
  CHECK(sale::applyRecord(pos, n, m, rec, err, sizeof err));
  CHECK(n == 1 && pos[0].id == 2 && fabs(pos[0].qty - 3) < 1e-9 && m.appliedSeq == 1 && m.nextLotId == 3);

  // 2. replaying the same record is a no-op (power-loss recovery relies on this)
  uint8_t n1 = n;
  uint32_t v1 = m.ver;
  double q1 = pos[0].qty;
  CHECK(sale::applyRecord(pos, n, m, rec, err, sizeof err));
  CHECK(n == n1 && fabs(pos[0].qty - q1) < 1e-9 && m.appliedSeq == 1 && m.ver == v1 + 1);

  // 3. a sale that references a lot which is gone cannot be applied (unless it sold it out completely)
  SaleRecord orphan = rec;
  orphan.seq = 5;
  orphan.nAlloc = 1;
  orphan.alloc[0].lot = 42;
  orphan.alloc[0].date[0] = 0;
  orphan.alloc[0].after = 2;                               // leaves an open remainder that no longer exists
  CHECK(!sale::applyRecord(pos, n, m, orphan, err, sizeof err) && strstr(err, "lot is no longer"));

  // 4. undo: a reversal record restores both lots (the sold-out lot is re-created)
  SaleRecord rev;
  CHECK(sale::makeReversal(pos, n, rec, 2, 1700000600, m, rev, err, sizeof err));
  CHECK(rev.type == REC_REVERSAL && rev.refSeq == 1 && !strcmp(rev.refId, "TX-000001") &&
        rev.nAlloc == rec.nAlloc);
  CHECK_NEAR(rev.realised, -rec.realised, 1e-6);
  CHECK_NEAR(rev.net, -rec.net, 1e-6);
  CHECK(rev.alloc[0].lot == 1 && fabs(rev.alloc[0].before) < 1e-9 && fabs(rev.alloc[0].after - 10) < 1e-9);
  CHECK(rev.alloc[1].lot == 2 && fabs(rev.alloc[1].before - 3) < 1e-9 && fabs(rev.alloc[1].after - 5) < 1e-9);

  CHECK(sale::applyRecord(pos, n, m, rev, err, sizeof err));
  CHECK(n == 2 && m.appliedSeq == 2);
  const Position *l1 = byId(pos, n, 1);
  CHECK(l1 && fabs(l1->qty - 10) < 1e-9 && fabs(l1->oqty - 10) < 1e-9 &&
        !strcmp(l1->symbol, "^GSPC") && !strcmp(l1->date, "2026-01-02") && l1->buy == 6000);
  const Position *l2 = byId(pos, n, 2);
  CHECK(l2 && fabs(l2->qty - 5) < 1e-9);

  // 5. undoing an undo (or reversing a reversal) is rejected
  SaleRecord again;
  CHECK(!sale::makeReversal(pos, n, rev, 3, 0, m, again, err, sizeof err) && strstr(err, "only a sale"));
}

// ---------------------------------------------------------------- aggregates
static SaleRecord mkSale(uint32_t seq, const char *id, const char *date, double realised) {
  SaleRecord r;
  memset(&r, 0, sizeof r);
  r.seq = seq;
  r.type = REC_SALE;
  strcpy(r.id, id);
  strcpy(r.symbol, "^GSPC");
  strcpy(r.date, date);
  r.realised = realised;
  r.nAlloc = 1;
  r.alloc[0].lot = 1;
  return r;
}

static void testSummary() {
  RealisedSummary s;
  sale::summaryReset(s);
  CHECK(s.valid && s.records == 0 && s.count == 0 && !isnan(s.pl) && s.pl == 0 && !s.hasLast);

  SaleRecord a = mkSale(1, "TX-000001", "2026-01-05", 100);
  SaleRecord b = mkSale(2, "TX-000002", "2025-06-01", -40);
  sale::summaryAdd(s, a);
  CHECK(s.records == 1 && s.count == 1 && s.reversed == 0 && fabs(s.pl - 100) < 1e-9 && s.hasLast);
  CHECK_STR(s.lastId, "TX-000001");
  CHECK(s.nLast == 1);
  sale::summaryAdd(s, b);
  CHECK(s.records == 2 && s.count == 2 && fabs(s.pl - 60) < 1e-9 && s.nLast == 2);
  CHECK_STR(s.lastId, "TX-000002");
  CHECK(s.nYears == 2 && s.year[0] == 2026 && fabs(s.yearPl[0] - 100) < 1e-9 &&
        s.year[1] == 2025 && fabs(s.yearPl[1] + 40) < 1e-9);               // newest year first

  // undo of the newest sale: count down, P/L back to the first sale, "last sale" steps back
  SaleRecord rev;
  memset(&rev, 0, sizeof rev);
  rev.seq = 3;
  rev.type = REC_REVERSAL;
  strcpy(rev.id, "TX-000003");
  strcpy(rev.refId, "TX-000002");
  strcpy(rev.symbol, "^GSPC");
  strcpy(rev.date, "2025-06-01");
  rev.realised = 40;
  sale::summaryAdd(s, rev);
  CHECK(s.records == 3 && s.count == 1 && s.reversed == 1 && fabs(s.pl - 100) < 1e-9);
  CHECK_STR(s.lastId, "TX-000001");
  CHECK(s.nLast == 1);
  CHECK(s.nYears == 2 && fabs(s.yearPl[1] - 0) < 1e-9);                    // 2025 netted back to 0

  // the ring keeps only the newest 2 sales (RAM), and the last-sale fields follow it
  RealisedSummary big;
  sale::summaryReset(big);
  for (uint32_t i = 1; i <= 8; i++) {
    char id[16];
    snprintf(id, sizeof id, "TX-%06lu", (unsigned long)i);
    sale::summaryAdd(big, mkSale(i, id, "2026-03-01", (double)i));
  }
  CHECK(big.nLast == 2 && big.count == 8 && fabs(big.pl - 36) < 1e-9);
  CHECK_STR(big.lastId, "TX-000008");
  CHECK(!big.warn && !big.full && big.records == 8);

  // ledger capacity warning
  sale::summaryReset(big);
  big.records = LEDGER_WARN_AT - 1;
  sale::summaryAdd(big, mkSale(1, "TX-000001", "2026-03-01", 1));
  CHECK(big.warn && !big.full);
}

// ---------------------------------------------------------------- ledger scan
struct Mem {
  const char *p;
  size_t n, i;
  bool lastTerm;
};
static bool memOpen(void *c) { return static_cast<Mem *>(c)->p != nullptr; }
static int memNext(void *c, char *buf, size_t cap) {
  Mem *m = static_cast<Mem *>(c);
  if (m->i >= m->n) return 0;
  size_t k = 0;
  bool term = false, trunc = false;
  while (m->i < m->n) {
    char ch = m->p[m->i++];
    if (ch == '\n') { term = true; break; }
    if (k + 1 < cap) buf[k++] = ch;
    else trunc = true;                                  // drain the rest, report as unterminated
  }
  m->lastTerm = term && !trunc;
  return (int)k;
}
static bool memTerm(void *c) { return static_cast<Mem *>(c)->lastTerm; }
static void memClose(void *) {}
static sale::LineSrc memSrc(Mem &m) {
  sale::LineSrc s;
  s.ctx = &m;
  s.open = memOpen;
  s.next = memNext;
  s.terminated = memTerm;
  s.close = memClose;
  return s;
}

static const char *LINE1 =
 "{\"seq\":1,\"ts\":1700000000,\"id\":\"TX-000001\",\"type\":0,\"symbol\":\"^GSPC\",\"date\":\"2026-01-15\","
 "\"qty\":10,\"price\":6200,\"refPrice\":6200,\"priceSource\":\"yahoo\",\"fee\":0,\"gross\":62000,\"net\":62000,"
 "\"cost\":60000,\"realised\":2000,\"alloc\":[{\"lot\":1,\"date\":\"2026-01-02\",\"buy\":6000,\"oqty\":10,\"before\":10,\"after\":0}]}\n";
static const char *LINE2 =
 "{\"seq\":2,\"ts\":1700000600,\"id\":\"TX-000002\",\"type\":0,\"symbol\":\"^GSPC\",\"date\":\"2026-02-15\","
 "\"qty\":2,\"price\":6200,\"refPrice\":6200,\"priceSource\":\"manual\",\"fee\":0,\"gross\":12400,\"net\":12400,"
 "\"cost\":12200,\"realised\":200,\"alloc\":[{\"lot\":2,\"date\":\"2026-02-02\",\"buy\":6100,\"oqty\":5,\"before\":5,\"after\":3}]}\n";

static void testScan() {
  sale::ScanStats st;
  std::string ledger = std::string(LINE1) + LINE2;

  // two complete lines
  Mem m1{ledger.c_str(), ledger.size(), 0, false};
  sale::LineSrc s1 = memSrc(m1);
  sale::scan(s1, nullptr, nullptr, st);
  CHECK(st.opened && st.records == 2 && st.maxSeq == 2 && st.badLines == 0 && !st.partialTail);

  // a broken line is counted and skipped, never parsed
  std::string all = std::string(LINE1) + "garbage\n" + LINE2;
  Mem m2{all.c_str(), all.size(), 0, false};
  sale::LineSrc s2 = memSrc(m2);
  sale::scan(s2, nullptr, nullptr, st);
  CHECK(st.opened && st.records == 2 && st.badLines == 1 && st.maxSeq == 2 && !st.partialTail);

  // an unterminated tail (power cut mid-append) is never trusted
  std::string cut = std::string(LINE1) + "{\"seq\":9,\"id\":\"TX-000009\",\"sym";
  Mem m3{cut.c_str(), cut.size(), 0, false};
  sale::LineSrc s3 = memSrc(m3);
  sale::scan(s3, nullptr, nullptr, st);
  CHECK(st.opened && st.records == 1 && st.maxSeq == 1 && st.badLines == 0 && st.partialTail);

  // no file at all
  Mem none{nullptr, 0, 0, false};
  sale::LineSrc s4 = memSrc(none);
  sale::scan(s4, nullptr, nullptr, st);
  CHECK(!st.opened && st.records == 0 && !st.partialTail);
}

// ---------------------------------------------------------------- recovery planning
static void testPlanRecovery() {
  sale::ScanStats st;
  memset(&st, 0, sizeof st);
  SaleRecord pend;
  memset(&pend, 0, sizeof pend);
  pend.seq = 5;

  sale::RecoveryPlan p = sale::planRecovery(st, nullptr, false, false);
  CHECK(!p.opened && !p.appendPending && !p.truncateTail && !p.clearJournal && !p.hasJournal);

  st.opened = true;
  st.maxSeq = 3;
  p = sale::planRecovery(st, nullptr, false, true);                  // journal present but unreadable
  CHECK(p.opened && p.hasJournal && !p.appendPending && !p.truncateTail && p.clearJournal);

  p = sale::planRecovery(st, &pend, true, true);                     // journaled record lost from the ledger
  CHECK(p.appendPending && p.clearJournal && !p.truncateTail);

  pend.seq = 2;                                                      // already in the ledger
  p = sale::planRecovery(st, &pend, true, true);
  CHECK(!p.appendPending && p.clearJournal && !p.truncateTail);

  st.partialTail = true;
  p = sale::planRecovery(st, nullptr, false, true);                  // partial line + journal
  CHECK(p.truncateTail && p.clearJournal && !p.appendPending);
}

// ---------------------------------------------------------------- boot recovery
struct Counter {
  uint16_t seen;
  uint32_t maxSeq;
};
static void countFn(void *c, const SaleRecord &r, uint16_t) {
  Counter *k = static_cast<Counter *>(c);
  k->seen++;
  if (r.seq > k->maxSeq) k->maxSeq = r.seq;
}

static void testRecover() {
  std::string ledger = std::string(LINE1) + LINE2;

  // cold boot: nothing applied yet -> both records are applied to the open lots
  {
    Position pos[MAX_POSITIONS]; uint8_t n; PortfolioMeta m;
    twoLots(pos, n, m);
    RealisedSummary s;
    Mem mem{ledger.c_str(), ledger.size(), 0, false};
    sale::LineSrc src = memSrc(mem);
    sale::ScanStats st;
    sale::RecoverResult res = sale::recover(pos, n, m, s, src, st);
    CHECK(st.records == 2 && res.applied == 2 && res.alreadyApplied == 0 && res.failed == 0);
    CHECK(n == 1 && pos[0].id == 2 && fabs(pos[0].qty - 3) < 1e-9);
    CHECK(m.appliedSeq == 2 && m.nextTxn == 3 && m.nextLotId == 3);
    CHECK(s.valid && s.records == 2 && s.count == 2 && fabs(s.pl - 2200) < 1e-9 && s.hasLast);
    CHECK_STR(s.lastId, "TX-000002");
  }

  // warm boot: the first record is already reflected in the portfolio
  {
    Position pos[MAX_POSITIONS]; uint8_t n = 0;
    PortfolioMeta m;
    memset(&m, 0, sizeof m);
    m.nextLotId = 2;                                        // lot 1 was sold and left the open list
    addLot(pos, n, m, "^GSPC", "2026-02-02", 5, 6100);      // lot 2 is still open
    m.appliedSeq = 1;
    m.nextTxn = 2;
    RealisedSummary s;
    Mem mem{ledger.c_str(), ledger.size(), 0, false};
    sale::LineSrc src = memSrc(mem);
    sale::ScanStats st;
    sale::RecoverResult res = sale::recover(pos, n, m, s, src, st);
    CHECK(res.alreadyApplied == 1 && res.applied == 1 && res.failed == 0);
    CHECK(n == 1 && fabs(pos[0].qty - 3) < 1e-9 && m.appliedSeq == 2 && m.nextTxn == 3);
    CHECK(s.records == 2 && fabs(s.pl - 2200) < 1e-9);                // summary rebuilt from the ledger
  }

  // a ledger with only an unterminated tail -> the tail is ignored, the good line still replays
  {
    Position pos[MAX_POSITIONS]; uint8_t n; PortfolioMeta m;
    twoLots(pos, n, m);
    RealisedSummary s;
    std::string cut = std::string(LINE1) + "{\"seq\":9,\"id\":\"TX-000009\"";
    Mem mem{cut.c_str(), cut.size(), 0, false};
    sale::LineSrc src = memSrc(mem);
    sale::ScanStats st;
    sale::RecoverResult res = sale::recover(pos, n, m, s, src, st);
    CHECK(st.partialTail && res.applied == 1 && res.failed == 0 && n == 1 && fabs(pos[0].qty - 5) < 1e-9);
    CHECK(s.records == 1 && m.appliedSeq == 1 && m.nextTxn == 2);
  }

  // counting pass (the storage layer's stats-only scan)
  Mem mem{ledger.c_str(), ledger.size(), 0, false};
  sale::LineSrc src = memSrc(mem);
  Counter k;
  k.seen = 0;
  k.maxSeq = 0;
  sale::ScanStats st;
  sale::scan(src, countFn, &k, st);
  CHECK(k.seen == 2 && k.maxSeq == 2 && st.records == 2);
}

// ---------------------------------------------------------------- status helpers
static void testStatus() {
  SaleRecord r;
  memset(&r, 0, sizeof r);
  r.type = REC_SALE;
  CHECK(sale::statusOf(r, false) == REC_SALE);
  CHECK(sale::statusOf(r, true) == SALE_ST_REVERSED);
  r.type = REC_REVERSAL;
  CHECK(sale::statusOf(r, false) == REC_REVERSAL);
  CHECK(sale::statusOf(r, true) == REC_REVERSAL);
}

// ---------------------------------------------------------------- lifetime identity
static void testLifetime() {
  AppState a;
  appInit(a);
  uint8_t n = 0;
  addLot(a.positions, n, a.pmeta, "^GSPC", "2026-01-02", 10, 6000);
  a.posCount = n;

  SymQuote &q = a.book.q[a.book.n++];
  memset(&q, 0, sizeof q);
  strcpy(q.symbol, "^GSPC");
  q.price = 6200;
  q.valid = true;
  q.marketTime = 1700000000;
  q.okMs = 1000;
  q.okEpoch = utcMidnight(2026, 11, 20);

  const uint32_t now = 1000;
  const uint32_t epoch = utcMidnight(2026, 11, 20);
  appRecompute(a, now, epoch);
  CHECK(a.summary.complete && isNum(a.summary.pl) && isNum(a.summary.nav));
  CHECK_NEAR(a.summary.pl, (6200 - 6000) * 10, 1e-6);                  // unrealised 2000
  CHECK(isnan(a.summary.realisedPl) && isnan(a.summary.lifetimePl));   // ledger unknown -> N/A, never 0

  sale::summaryReset(a.realised);
  a.realised.pl = -3910;
  appRecompute(a, now, epoch);
  CHECK_NEAR(a.summary.realisedPl, -3910, 1e-9);
  CHECK_NEAR(a.summary.lifetimePl, a.summary.pl + a.summary.realisedPl, 1e-9);
  CHECK_NEAR(a.summary.lifetimePl, 2000 - 3910, 1e-6);                 // identity: open + realised

  a.realised.valid = false;                                            // ledger unreadable -> N/A on both
  appRecompute(a, now, epoch);
  CHECK(isnan(a.summary.realisedPl) && isnan(a.summary.lifetimePl));

  // no open lots: unrealised is unknown, so lifetime stays unknown even with a known realised total
  AppState b;
  appInit(b);
  sale::summaryReset(b.realised);
  b.realised.pl = 123;
  appRecompute(b, now, epoch);
  CHECK(isnan(b.summary.pl) && isnan(b.summary.lifetimePl));
}

static void testSeqAfterDelete() {
  // delete seqs 2 and 3 out of 1..5
  CHECK(sale::seqAfterDelete(1, 2, 3) == 1);
  CHECK(sale::seqAfterDelete(4, 2, 3) == 2);
  CHECK(sale::seqAfterDelete(5, 2, 3) == 3);
  CHECK(sale::seqAfterDelete(5, 3, 2) == 3);                 // argument order does not matter
  CHECK(sale::seqAfterDelete(0, 1, 2) == 0);                 // 0 = "none" stays 0
  CHECK(sale::seqAfterDelete(2, 1, 2) == 0);                 // appliedSeq == b after deleting the newest pair
  CHECK(sale::seqAfterDelete(7, 1, 2) == 5);
  CHECK(sale::seqAfterDelete(3, 3, 0) == 2);                 // single record (no reversal), it was the newest
  CHECK(sale::seqAfterDelete(4, 2, 0) == 3);
  CHECK(sale::seqAfterDelete(1, 2, 0) == 1);
}

int main(int, char **) {
  testSeqAfterDelete();
  testPreview();
  testToken();
  testRecords();
  testApplyAndReverse();
  testSummary();
  testScan();
  testPlanRecovery();
  testRecover();
  testStatus();
  testLifetime();
  printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
