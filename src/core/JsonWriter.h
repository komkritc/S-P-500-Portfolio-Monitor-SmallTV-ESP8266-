// JsonWriter.h - streaming JSON writer with a small fixed buffer. Output goes to a sink callback
// (the HTTP chunked response, or a std::string in tests), so no response is ever held in RAM.
// Non-finite numbers are written as null (the API contract: null = not available, never 0).
#pragma once
#include <stdint.h>
#include <stddef.h>

class JsonWriter {
 public:
  typedef void (*Sink)(void *ctx, const char *data, size_t len);
  JsonWriter(Sink sink, void *ctx) : sink_(sink), ctx_(ctx) {}
  ~JsonWriter() { flush(); }

  JsonWriter &beginObj();
  JsonWriter &endObj();
  JsonWriter &beginArr();
  JsonWriter &endArr();
  JsonWriter &key(const char *k);                       // inside an object
  JsonWriter &num(double v, uint8_t decimals = 2);      // NaN/inf -> null
  JsonWriter &integer(int64_t v);
  JsonWriter &boolean(bool v);
  JsonWriter &null();
  JsonWriter &str(const char *s);                       // escaped; null pointer -> null
  JsonWriter &raw(const char *s);                       // pre-formatted JSON fragment
  // key + value helpers
  JsonWriter &kNum(const char *k, double v, uint8_t d = 2) { return key(k).num(v, d); }
  JsonWriter &kInt(const char *k, int64_t v) { return key(k).integer(v); }
  JsonWriter &kBool(const char *k, bool v) { return key(k).boolean(v); }
  JsonWriter &kStr(const char *k, const char *v) { return key(k).str(v); }
  void flush();

 private:
  Sink sink_; void *ctx_;
  char buf_[320]; size_t len_ = 0;
  bool first_[10]; uint8_t depth_ = 0; bool afterKey_ = false;
  void put(const char *s, size_t n);
  void putc_(char c) { put(&c, 1); }
  void sep();
};
