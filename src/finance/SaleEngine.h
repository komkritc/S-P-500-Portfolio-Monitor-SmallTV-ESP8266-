// SaleEngine.h - sale accounting: allocation, preview, ledger records, apply/undo, aggregates.
//
// Pure by design: no filesystem, no network, no Arduino headers, so every rule here is covered by
// tests/test_sales.cpp on the host. The LittleFS side (append, journal, repair, paging) lives in
// storage/SaleStore.cpp and only calls into this file.
//
// Invariants
//   * The ledger (/sells.jsonl) is append-only: a sale is a record, an undo is a new record of
//     type "reversal" that references the original id/seq. Nothing is ever edited or deleted.
//   * Applying a record sets every touched lot's open quantity to alloc.after (absolute), so
//     replaying the same record twice is a no-op - that is what makes power-loss recovery safe.
//   * Lifetime P/L = realised (ledger) + unrealised (open lots). Unknown stays NaN -> null -> N/A.
#pragma once
#include "../core/AppTypes.h"
#include "../core/JsonWriter.h"

namespace sale {

// ---------------------------------------------------------------- ledger iteration
// The file itself is SaleStore's business; tests drive the same code with an in-memory source.
struct LineSrc {
  void *ctx;
  bool (*open)(void *ctx);                       // false: no file / unreadable
  int  (*next)(void *ctx, char *buf, size_t cap); // >0 bytes (no '\n'), 0 = EOF, -1 = read error
  bool (*terminated)(void *ctx);                 // the line just read ended with '\n'
  void (*close)(void *ctx);
};

struct ScanStats {
  bool     opened;        // the ledger could be opened (false = no file yet)
  uint32_t maxSeq;        // highest seq seen
  uint16_t records;       // parseable records
  uint16_t badLines;      // unparseable lines (skipped, flagged)
  bool     partialTail;   // last line was not '\n'-terminated (power cut mid-append)
};

typedef void (*RecordFn)(void *ctx, const SaleRecord &r, uint16_t index);

// One streaming pass over the ledger. `fn` may be null (stats only). A partial trailing line is
// never parsed: it belongs to an in-flight transaction that only the journal can vouch for.
void scan(LineSrc &src, RecordFn fn, void *ctx, ScanStats &st);

// ---------------------------------------------------------------- records
bool   parseRecord(const char *line, size_t len, SaleRecord &out);
void   writeRecord(JsonWriter &w, const SaleRecord &r);                 // one object, no newline
size_t recordLine(const SaleRecord &r, char *buf, size_t cap);          // JSON + NUL, 0 = too small

// ---------------------------------------------------------------- allocation & preview
// Binds a preview to the portfolio + ledger state it was computed from (confirm must match).
uint32_t previewToken(const Position *pos, uint8_t n, const PortfolioMeta &m);

// Full preview: validation, allocation and money. `req.price` must already be the execution price
// (NaN = "not resolved yet", which is an error here). `nowEpoch` = 0 skips the future-date check.
bool preview(const Position *pos, uint8_t n, const PortfolioMeta &m, const SaleRequest &req,
             uint32_t nowEpoch, SalePreview &pv);

// ---------------------------------------------------------------- records from a preview
// Uses m.nextTxn for the TX id (the caller bumps it when the transaction is committed).
void makeRecord(const SalePreview &pv, uint32_t seq, uint32_t ts, const PortfolioMeta &m, SaleRecord &out);

// Reversal of `orig` against the CURRENT positions: each touched lot gains back the quantity the
// original sale took (lots that were fully sold are re-created from the original allocation).
bool makeReversal(const Position *pos, uint8_t n, const SaleRecord &orig, uint32_t seq, uint32_t ts,
                  const PortfolioMeta &m, SaleRecord &out, char *err, size_t errN);

// ---------------------------------------------------------------- apply
// Sets lot.qty = alloc.after for every allocation (creating missing lots for a reversal), drops
// fully sold lots, then advances m.appliedSeq and bumps m.ver.
bool applyRecord(Position *pos, uint8_t &n, PortfolioMeta &m, const SaleRecord &r, char *err, size_t errN);

// ---------------------------------------------------------------- aggregates
void summaryReset(RealisedSummary &s);
void summaryAdd(RealisedSummary &s, const SaleRecord &r);               // order matters for the ring

// ---------------------------------------------------------------- delete
// Deleting a sale (and its reversal, if any) removes up to two seqs (a, b; pass b = 0 for none).
// Every later seq, refSeq and appliedSeq moves down so the numbering stays dense.
uint32_t seqAfterDelete(uint32_t seq, uint32_t a, uint32_t b);

// ---------------------------------------------------------------- boot scan
// Phase 1 (read-only): what must the filesystem do before the ledger can be trusted?
// `pending`/`pendingOk` describe /txn.pending (nullptr when absent or unparseable).
struct RecoveryPlan {
  bool opened;          // ledger file exists
  bool truncateTail;    // drop the unterminated last line
  bool appendPending;   // the journaled record is not in the ledger yet -> append it
  bool clearJournal;    // the journal has served its purpose
  bool hasJournal;
};
RecoveryPlan planRecovery(const ScanStats &st, const SaleRecord *pending, bool pendingOk, bool journalExists);

// Phase 2 (after the plan's filesystem steps): replay the ledger into the open positions and the
// realised summary. Records with seq <= m.appliedSeq are already reflected (counted, not applied).
struct RecoverResult {
  uint16_t applied;      // records re-applied to the open positions
  uint16_t alreadyApplied;
  uint16_t failed;       // records that could not be applied (corrupt / lot missing)
};
RecoverResult recover(Position *pos, uint8_t &n, PortfolioMeta &m, RealisedSummary &s,
                      LineSrc &src, ScanStats &st);

// ---------------------------------------------------------------- status helpers
// REC_SALE, REC_REVERSAL or SALE_ST_REVERSED, from the id/seq of the records seen so far.
uint8_t statusOf(const SaleRecord &r, bool reversed);
}  // namespace sale
