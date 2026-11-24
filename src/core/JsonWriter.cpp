#include "JsonWriter.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

void JsonWriter::flush() { if (len_ && sink_) sink_(ctx_, buf_, len_); len_ = 0; }

void JsonWriter::put(const char *s, size_t n) {
  while (n) {
    size_t room = sizeof buf_ - len_;
    if (!room) { flush(); room = sizeof buf_; }
    size_t k = n < room ? n : room;
    memcpy(buf_ + len_, s, k);
    len_ += k; s += k; n -= k;
  }
}

void JsonWriter::sep() {
  if (afterKey_) { afterKey_ = false; return; }
  if (depth_ && !first_[depth_ - 1]) putc_(',');
  if (depth_) first_[depth_ - 1] = false;
}

JsonWriter &JsonWriter::beginObj() { sep(); putc_('{'); if (depth_ < 10) first_[depth_++] = true; return *this; }
JsonWriter &JsonWriter::endObj() { if (depth_) depth_--; putc_('}'); return *this; }
JsonWriter &JsonWriter::beginArr() { sep(); putc_('['); if (depth_ < 10) first_[depth_++] = true; return *this; }
JsonWriter &JsonWriter::endArr() { if (depth_) depth_--; putc_(']'); return *this; }

JsonWriter &JsonWriter::key(const char *k) {
  if (depth_ && !first_[depth_ - 1]) putc_(',');
  if (depth_) first_[depth_ - 1] = false;
  putc_('"'); put(k, strlen(k)); put("\":", 2);
  afterKey_ = true;
  return *this;
}

JsonWriter &JsonWriter::num(double v, uint8_t decimals) {
  sep();
  if (isnan(v) || isinf(v)) { put("null", 4); return *this; }
  char t[32];
  int n = snprintf(t, sizeof t, "%.*f", decimals, v);
  if (n > 0 && n < (int)sizeof t) {
    if (strcmp(t, "-0") == 0 || strncmp(t, "-0.", 3) == 0) {          // "-0.00" -> "0.00"
      bool allZero = true; for (const char *p = t + 1; *p; p++) if (*p != '0' && *p != '.') allZero = false;
      if (allZero) memmove(t, t + 1, strlen(t));
    }
    put(t, strlen(t));
  } else put("null", 4);
  return *this;
}

JsonWriter &JsonWriter::integer(int64_t v) {
  sep();
  char t[24]; int n = snprintf(t, sizeof t, "%lld", (long long)v); put(t, (size_t)n);
  return *this;
}

JsonWriter &JsonWriter::boolean(bool v) { sep(); put(v ? "true" : "false", v ? 4 : 5); return *this; }
JsonWriter &JsonWriter::null() { sep(); put("null", 4); return *this; }

JsonWriter &JsonWriter::str(const char *s) {
  sep();
  if (!s) { put("null", 4); return *this; }
  putc_('"');
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { putc_('\\'); putc_((char)c); }
    else if (c == '\n') put("\\n", 2);
    else if (c == '\r') put("\\r", 2);
    else if (c == '\t') put("\\t", 2);
    else if (c < 0x20) { char u[8]; int n = snprintf(u, sizeof u, "\\u%04x", c); put(u, (size_t)n); }
    else putc_((char)c);
  }
  putc_('"');
  return *this;
}

JsonWriter &JsonWriter::raw(const char *s) { sep(); put(s, strlen(s)); return *this; }
