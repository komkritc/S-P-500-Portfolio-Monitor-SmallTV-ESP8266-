// Portfolio.h - position list operations, symbol rules and (de)serialisation.
// File format is v2 of /portfolio.json: {"version":2,"ver","appliedSeq","nextLotId","nextTxn",
// "positions":[{"id","symbol","qty","oqty","buy","date"}]}. v1.0 files load unchanged (missing
// ids are assigned 1..n in load order, oqty defaults to qty).
#pragma once
#include "../core/AppTypes.h"
#include "../core/JsonWriter.h"
#include "FinancialCalculator.h"

// Upper-cases, trims and maps aliases (SP500/S&P500/GSPC -> ^GSPC, SPX -> ^SPX, NDX/NASDAQ -> ^NDX).
// Returns false when the result is not 1..15 chars of [A-Z0-9^.=-].
bool normalizeSymbol(const char *in, char *out, size_t outN);

enum AddResult : uint8_t { ADD_OK = 0, ADD_UPDATED, ADD_FULL, ADD_TOO_MANY_SYMBOLS, ADD_BAD_SYMBOL, ADD_BAD_DATE, ADD_BAD_QTY, ADD_BAD_PRICE };
const char *addResultText(AddResult r);

// Same symbol + date replaces the existing row (original behaviour). `todayUtc` = 0 skips the future-date check.
// `m.nextLotId` is consumed for new lots and bumped on `m.ver` for any mutation (confirm tokens).
AddResult portfolioAdd(Position *pos, uint8_t &n, PortfolioMeta &m, const char *symbol, const char *date, double qty, double buy, uint32_t todayUtc);
bool portfolioDelete(Position *pos, uint8_t &n, PortfolioMeta &m, uint8_t index);
// qty = thb / buy, rejecting non-positive / non-finite inputs.
bool qtyFromThb(double thb, double buyPrice, double &qty);

// Distinct symbols of the portfolio, capped to MAX_SYMBOLS. Returns the count written to out[].
uint8_t distinctSymbols(const Position *pos, uint8_t n, char out[][SYMBOL_LEN], uint8_t maxOut);
// Brings the quote book in line with the portfolio: keeps existing quotes of still-used symbols,
// adds empty entries for new ones, drops unused ones.
void bookSync(QuoteBook &b, const Position *pos, uint8_t n);
SymQuote *bookFind(QuoteBook &b, const char *symbol);

void portfolioWrite(JsonWriter &w, const Position *pos, uint8_t n, const PortfolioMeta &m);
// Lenient load (skips bad rows, keeps the rest). Returns number of rows loaded.
// Also fills `m` (missing fields keep their defaults); 0 is returned on a syntax error.
uint8_t portfolioLoad(Position *pos, const char *json, size_t len, PortfolioMeta &m);
