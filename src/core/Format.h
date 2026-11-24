// Format.h - number formatting shared by the LCD and the web API text fields.
// Unavailable (NaN/inf) values always render as "N/A", never as zero.
#pragma once
#include "AppTypes.h"

enum SignMode : uint8_t { SIGN_NONE = 0, SIGN_ALWAYS = 1 };

// Grouped decimal: 268500 -> "268,500.00". SIGN_ALWAYS adds '+'/'-' (nothing for a value that rounds to 0).
// Returns false (and writes "N/A") when v is not finite or does not fit.
bool fmtNum(char *out, size_t n, double v, uint8_t decimals, SignMode sign);
bool fmtThb(char *out, size_t n, double v);            // "THB 268,500.00" / "-THB 5,200.00"
bool fmtThbSigned(char *out, size_t n, double v);      // "+THB 18,500.00" / "-THB 5,200.00"
bool fmtPct(char *out, size_t n, double v);            // "+7.40%" / "-2.10%"
bool fmtIndex(char *out, size_t n, double v);          // "6,910.39"
bool fmtIndexSigned(char *out, size_t n, double v);    // "-265.44"
// -1 negative, 0 zero/unknown, +1 positive, judged on the value as displayed (2 decimals).
int  signOf2dp(double v);
