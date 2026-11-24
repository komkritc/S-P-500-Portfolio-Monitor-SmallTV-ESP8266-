#include "MarketClock.h"
#include <stdio.h>
#include <string.h>

// Howard Hinnant's civil-date algorithms (public domain).
int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

void civilFromDays(int64_t z, int &y, int &m, int &d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int)(yoe + era * 400);
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = (int)(doy - (153 * mp + 2) / 5 + 1);
  m = (int)(mp < 10 ? mp + 3 : mp - 9);
  y += m <= 2;
}

static int dim(int y, int m) {
  static const uint8_t D[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
  return D[m - 1];
}

static int weekdayOfDays(int64_t days) { int w = (int)((days + 4) % 7); return w < 0 ? w + 7 : w; }  // 1970-01-01 = Thu

bool parseIsoDate(const char *s, int &y, int &m, int &d) {
  if (!s || strlen(s) != 10 || s[4] != '-' || s[7] != '-') return false;
  for (int i = 0; i < 10; i++) if (i != 4 && i != 7 && (s[i] < '0' || s[i] > '9')) return false;
  y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
  m = (s[5] - '0') * 10 + (s[6] - '0');
  d = (s[8] - '0') * 10 + (s[9] - '0');
  if (y < 1970 || y > 2100 || m < 1 || m > 12 || d < 1 || d > dim(y, m)) return false;
  return true;
}

uint32_t utcMidnight(int y, int m, int d) { return (uint32_t)(daysFromCivil(y, m, d) * 86400LL); }

// nth (1-based) weekday `wd` of month; n = 0 means the last one.
static int nthWeekday(int y, int m, int wd, int n) {
  if (n > 0) {
    int first = weekdayOfDays(daysFromCivil(y, m, 1));
    return 1 + (wd - first + 7) % 7 + (n - 1) * 7;
  }
  int last = dim(y, m);
  int lw = weekdayOfDays(daysFromCivil(y, m, last));
  return last - (lw - wd + 7) % 7;
}

int32_t nyOffsetSec(uint32_t utc, bool *isDst) {
  int y, m, d;
  civilFromDays((int64_t)utc / 86400, y, m, d);
  // DST start: 2nd Sunday of March 02:00 EST = 07:00 UTC; end: 1st Sunday of Nov 02:00 EDT = 06:00 UTC.
  uint32_t start = utcMidnight(y, 3, nthWeekday(y, 3, 0, 2)) + 7 * 3600;
  uint32_t end   = utcMidnight(y, 11, nthWeekday(y, 11, 0, 1)) + 6 * 3600;
  bool dst = utc >= start && utc < end;
  if (isDst) *isDst = dst;
  return dst ? -4 * 3600 : -5 * 3600;
}

const char *nyAbbr(bool dst) { return dst ? "EDT" : "EST"; }

LocalTime toLocal(uint32_t utc, int32_t offsetSec) {
  int64_t t = (int64_t)utc + offsetSec;
  int64_t days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
  int64_t sod = t - days * 86400;
  LocalTime lt;
  civilFromDays(days, lt.y, lt.m, lt.d);
  lt.hh = (int)(sod / 3600);
  lt.mm = (int)(sod % 3600 / 60);
  lt.ss = (int)(sod % 60);
  lt.wday = weekdayOfDays(days);
  return lt;
}

// Gregorian Easter (anonymous algorithm).
static void easter(int y, int &m, int &d) {
  int a = y % 19, b = y / 100, c = y % 100, dd = b / 4, e = b % 4, f = (b + 8) / 25;
  int g = (b - f + 1) / 3, h = (19 * a + b - dd - g + 15) % 30, i = c / 4, k = c % 4;
  int l = (32 + 2 * e + 2 * i - h - k) % 7, mm = (a + 11 * h + 22 * l) / 451;
  m = (h + l - 7 * mm + 114) / 31;
  d = (h + l - 7 * mm + 114) % 31 + 1;
}

// Holiday date shifted to the observed weekday (Sat -> Fri, Sun -> Mon). New Year on a Saturday
// is not observed (NYSE Rule 7.2).
static bool sameDay(int64_t days, int y, int m, int d) { return days == daysFromCivil(y, m, d); }

static bool observed(int y, int m, int d, int hm, int hd, bool newYear = false) {
  int64_t h = daysFromCivil(y, hm, hd);
  int w = weekdayOfDays(h);
  if (w == 6) { if (newYear) return false; h -= 1; }
  else if (w == 0) h += 1;
  return sameDay(h, y, m, d) ? true : false;
}

bool isNyseHoliday(int y, int m, int d) {
  int64_t days = daysFromCivil(y, m, d);
  int w = weekdayOfDays(days);
  if (w == 0 || w == 6) return true;
  if (m == 1 && observed(y, m, d, 1, 1, true)) return true;            // New Year's Day
  // (NYSE Rule 7.2: a Saturday Jan 1 is NOT observed on Dec 31, so there is no Dec 31 rule.)
  if (m == 1 && d == nthWeekday(y, 1, 1, 3)) return true;              // MLK Day
  if (m == 2 && d == nthWeekday(y, 2, 1, 3)) return true;              // Presidents' Day
  { int em, ed; easter(y, em, ed); if (days == daysFromCivil(y, em, ed) - 2) return true; }   // Good Friday
  if (m == 5 && d == nthWeekday(y, 5, 1, 0)) return true;              // Memorial Day
  if (y >= 2022 && m == 6 && observed(y, m, d, 6, 19)) return true;   // Juneteenth
  if (m == 7 && observed(y, m, d, 7, 4)) return true;                  // Independence Day
  if (m == 9 && d == nthWeekday(y, 9, 1, 1)) return true;              // Labor Day
  if (m == 11 && d == nthWeekday(y, 11, 4, 4)) return true;            // Thanksgiving
  if (m == 12 && observed(y, m, d, 12, 25)) return true;               // Christmas
  return false;
}

int nyseCloseMinute(int y, int m, int d) {
  int64_t days = daysFromCivil(y, m, d);
  int w = weekdayOfDays(days);
  if (m == 11 && d == nthWeekday(y, 11, 4, 4) + 1) return 780;         // day after Thanksgiving
  if (m == 7 && d == 3 && w >= 1 && w <= 4) return 780;                // Jul 3 (Mon-Thu)
  if (m == 12 && d == 24 && w >= 1 && w <= 4) return 780;              // Christmas Eve (Mon-Thu)
  return 960;
}

uint32_t sessionOpenUtc(int y, int m, int d) {
  uint32_t guess = utcMidnight(y, m, d) + 17 * 3600;            // noon-ish ET of that day, to pick the right DST offset
  return (uint32_t)((int64_t)utcMidnight(y, m, d) - nyOffsetSec(guess) + 570 * 60);
}

uint32_t sessionCloseUtc(int y, int m, int d) {
  uint32_t guess = utcMidnight(y, m, d) + 17 * 3600;
  return (uint32_t)((int64_t)utcMidnight(y, m, d) - nyOffsetSec(guess) + (int64_t)nyseCloseMinute(y, m, d) * 60);
}

uint32_t lastSessionCloseUtc(uint32_t nowUtc) {
  int64_t day = (int64_t)nowUtc / 86400 + 1;                    // start one day ahead: ET date can trail UTC by a day
  for (int i = 0; i < 12; i++, day--) {
    int y, m, d; civilFromDays(day, y, m, d);
    if (isNyseHoliday(y, m, d)) continue;
    uint32_t c = sessionCloseUtc(y, m, d);
    if (c <= nowUtc) return c;
  }
  return 0;
}

uint32_t nextSessionOpenUtc(uint32_t nowUtc) {
  int64_t day = (int64_t)nowUtc / 86400 - 1;
  for (int i = 0; i < 14; i++, day++) {
    int y, m, d; civilFromDays(day, y, m, d);
    if (isNyseHoliday(y, m, d)) continue;
    uint32_t o = sessionOpenUtc(y, m, d);
    if (o > nowUtc) return o;
  }
  return 0;
}

MarketStatus marketStatusAt(uint32_t nowUtc, uint32_t lastQuoteUtc) {
  if (nowUtc < 1600000000UL) return MS_UNKNOWN;     // clock not synced
  bool dst;
  int32_t off = nyOffsetSec(nowUtc, &dst);
  LocalTime lt = toLocal(nowUtc, off);
  if (isNyseHoliday(lt.y, lt.m, lt.d)) return MS_CLOSED;
  int mins = lt.hh * 60 + lt.mm;
  int closeM = nyseCloseMinute(lt.y, lt.m, lt.d);
  if (mins < 240) return MS_CLOSED;
  if (mins < 570) return MS_PRE;
  if (mins >= closeM) return MS_CLOSED;
  // Regular hours by the calendar: only OPEN if the newest quote is from today's session.
  uint32_t openUtc = (uint32_t)((int64_t)utcMidnight(lt.y, lt.m, lt.d) - off + 570 * 60);
  return lastQuoteUtc >= openUtc ? MS_OPEN : MS_CLOSED;
}

int32_t displayOffsetSec(uint32_t utc, uint8_t tzMode, int16_t localOffsetMin, const char **abbr, char *abbrBuf) {
  if (tzMode == 1) {
    int o = localOffsetMin;
    if (abbrBuf) {
      int a = o < 0 ? -o : o;
      snprintf(abbrBuf, 10, "UTC%c%02d:%02d", o < 0 ? '-' : '+', a / 60, a % 60);
      if (abbr) *abbr = abbrBuf;
    }
    return o * 60;
  }
  bool dst;
  int32_t off = nyOffsetSec(utc, &dst);
  if (abbr) *abbr = nyAbbr(dst);
  return off;
}

static const char *const MON[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

void formatQuoteTime(char *out, size_t n, uint32_t utc, uint8_t tzMode, int16_t localOffsetMin) {
  if (utc == 0) { snprintf(out, n, "N/A"); return; }
  const char *ab = ""; char buf[12];
  int32_t off = displayOffsetSec(utc, tzMode, localOffsetMin, &ab, buf);
  LocalTime lt = toLocal(utc, off);
  snprintf(out, n, "%s %d, %02d:%02d %s", MON[lt.m - 1], lt.d, lt.hh, lt.mm, ab);
}

void formatSessionDate(char *out, size_t n, uint32_t utc) {
  if (utc == 0) { if (n) out[0] = 0; return; }
  LocalTime lt = toLocal(utc, nyOffsetSec(utc));
  snprintf(out, n, "%04d-%02d-%02d", lt.y, lt.m, lt.d);
}

void formatClock(char *out, size_t n, uint32_t utc, uint8_t tzMode, int16_t localOffsetMin) {
  LocalTime lt = toLocal(utc, displayOffsetSec(utc, tzMode, localOffsetMin));
  snprintf(out, n, "%02d:%02d", lt.hh, lt.mm);
}

void formatMonthDay(char *out, size_t n, uint32_t utc, uint8_t tzMode, int16_t localOffsetMin) {
  LocalTime lt = toLocal(utc, displayOffsetSec(utc, tzMode, localOffsetMin));
  snprintf(out, n, "%s %d", MON[lt.m - 1], lt.d);
}
