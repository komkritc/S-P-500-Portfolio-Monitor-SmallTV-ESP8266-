#include "Format.h"
#include <stdio.h>
#include <string.h>

static const double POW10[] = {1, 10, 100, 1000, 10000};

bool fmtNum(char *out, size_t n, double v, uint8_t decimals, SignMode sign) {
  if (!n) return false;
  if (!isNum(v) || decimals > 4 || fabs(v) > 1e12) { snprintf(out, n, "N/A"); return false; }
  const double scale = POW10[decimals];
  unsigned long long scaled = (unsigned long long)(fabs(v) * scale + 0.5);
  unsigned long long ip = scaled / (unsigned long long)scale;
  unsigned long long fp = scaled % (unsigned long long)scale;

  char digits[24];
  int dn = 0;
  do { digits[dn++] = (char)('0' + ip % 10); ip /= 10; } while (ip);

  char tmp[40];
  int o = 0;
  if (sign == SIGN_ALWAYS && scaled != 0) tmp[o++] = v < 0 ? '-' : '+';
  else if (sign == SIGN_NONE && scaled != 0 && v < 0) tmp[o++] = '-';
  for (int i = dn - 1; i >= 0; i--) {
    tmp[o++] = digits[i];
    if (i > 0 && i % 3 == 0) tmp[o++] = ',';
  }
  if (decimals) {
    tmp[o++] = '.';
    for (int i = decimals - 1; i >= 0; i--) { tmp[o + i] = (char)('0' + fp % 10); fp /= 10; }
    o += decimals;
  }
  tmp[o] = 0;
  if ((size_t)o + 1 > n) { snprintf(out, n, "N/A"); return false; }
  memcpy(out, tmp, (size_t)o + 1);
  return true;
}

static bool thb(char *out, size_t n, double v, SignMode sign) {
  char num[32];
  if (!fmtNum(num, sizeof num, v, 2, SIGN_NONE)) { snprintf(out, n, "N/A"); return false; }
  char s = 0;
  const char *digits = num;
  if (num[0] == '-') { s = '-'; digits++; }
  else if (sign == SIGN_ALWAYS && signOf2dp(v) > 0) s = '+';
  if (s) snprintf(out, n, "%cTHB %s", s, digits);
  else snprintf(out, n, "THB %s", digits);
  return true;
}

bool fmtThb(char *out, size_t n, double v) { return thb(out, n, v, SIGN_NONE); }
bool fmtThbSigned(char *out, size_t n, double v) { return thb(out, n, v, SIGN_ALWAYS); }

bool fmtPct(char *out, size_t n, double v) {
  char num[32];
  if (!fmtNum(num, sizeof num, v, 2, SIGN_ALWAYS)) { snprintf(out, n, "N/A"); return false; }
  snprintf(out, n, "%s%%", num);
  return true;
}

bool fmtIndex(char *out, size_t n, double v) { return fmtNum(out, n, v, 2, SIGN_NONE); }
bool fmtIndexSigned(char *out, size_t n, double v) { return fmtNum(out, n, v, 2, SIGN_ALWAYS); }

int signOf2dp(double v) {
  if (!isNum(v)) return 0;
  long long r = (long long)(fabs(v) * 100.0 + 0.5);
  if (r == 0) return 0;
  return v < 0 ? -1 : 1;
}
