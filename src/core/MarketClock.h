// MarketClock.h - America/New_York time, NYSE calendar and market status.
// All functions are pure (take the instant as a parameter) so they are host-testable.
#pragma once
#include "AppTypes.h"

struct LocalTime { int y, m, d, hh, mm, ss, wday; };   // wday 0 = Sunday

int64_t  daysFromCivil(int y, int m, int d);
void     civilFromDays(int64_t z, int &y, int &m, int &d);
bool     parseIsoDate(const char *s, int &y, int &m, int &d);   // strict YYYY-MM-DD, validates the calendar
uint32_t utcMidnight(int y, int m, int d);

// US Eastern offset for a UTC instant (DST: 2nd Sun Mar 07:00 UTC .. 1st Sun Nov 06:00 UTC, 2007+).
int32_t  nyOffsetSec(uint32_t utc, bool *isDst = nullptr);
const char *nyAbbr(bool dst);
LocalTime toLocal(uint32_t utc, int32_t offsetSec);

bool     isNyseHoliday(int y, int m, int d);
int      nyseCloseMinute(int y, int m, int d);   // 960 normal, 780 early close
// Market state at `nowUtc`. OPEN needs the newest quote to belong to today's regular session,
// so an old closing quote is never labelled as live.
MarketStatus marketStatusAt(uint32_t nowUtc, uint32_t lastQuoteUtc);

// Close of the most recent regular session that has ended at or before `nowUtc` (UTC seconds).
uint32_t lastSessionCloseUtc(uint32_t nowUtc);
// Open (09:30 ET) of the next regular session strictly after `nowUtc`.
uint32_t nextSessionOpenUtc(uint32_t nowUtc);
// Exchange-local calendar date of a day offset: returns UTC time of that session's open/close.
uint32_t sessionOpenUtc(int y, int m, int d);
uint32_t sessionCloseUtc(int y, int m, int d);

// "Nov 20, 16:35 EST" in the chosen zone. tzMode 0 = exchange (DST aware), 1 = fixed offset.
void formatQuoteTime(char *out, size_t n, uint32_t utc, uint8_t tzMode, int16_t localOffsetMin);
// "2026-11-20" of the exchange-local day of `utc`.
void formatSessionDate(char *out, size_t n, uint32_t utc);
// "HH:MM" / "Nov 20" labels for chart axes.
void formatClock(char *out, size_t n, uint32_t utc, uint8_t tzMode, int16_t localOffsetMin);
void formatMonthDay(char *out, size_t n, uint32_t utc, uint8_t tzMode, int16_t localOffsetMin);
int32_t  displayOffsetSec(uint32_t utc, uint8_t tzMode, int16_t localOffsetMin, const char **abbr = nullptr, char *abbrBuf = nullptr);
