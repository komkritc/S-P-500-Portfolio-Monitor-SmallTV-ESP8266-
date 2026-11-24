// JsonStream.h - tiny incremental JSON scanner: bytes in, (path, value) events out.
// Memory is fixed (~200 B), nothing is allocated, so a 12 KB Yahoo chart body can be
// consumed straight from the TLS socket without ever holding it in RAM.
//
// Paths look like "chart.result[].meta.regularMarketPrice"; every array level adds "[]".
// For a value inside an array, `idx` is the index within the innermost array, else -1.
#pragma once
#include <stdint.h>
#include <stddef.h>

enum JsonKind : uint8_t { JK_NULL = 0, JK_BOOL = 1, JK_NUM = 2, JK_STR = 3 };

class JsonStream {
 public:
  typedef void (*Callback)(void *ctx, const char *path, int idx, JsonKind kind, const char *text);
  static const uint8_t  MAX_DEPTH = 12;
  static const uint8_t  PATH_CAP = 112;
  static const uint8_t  TEXT_CAP = 72;   // longest value kept whole (Wi-Fi passphrase is max 63)

  void begin(Callback cb, void *ctx);
  bool feed(const char *data, size_t len);   // false = syntax error (parser stays failed)
  bool complete() const { return state_ == ST_DONE; }   // exactly one top-level value finished
  bool failed() const { return state_ == ST_ERR; }
  uint32_t containerId() const { return depth_ ? st_[depth_ - 1].id : 0; }   // unique per object/array, valid inside the callback
  bool truncated() const { return trunc_; }     // a string/key longer than TEXT_CAP-1 was cut

 private:
  enum State : uint8_t { ST_START, ST_VALUE, ST_STRING, ST_SCALAR, ST_DONE, ST_ERR };
  // phase: grammar position inside this container
  enum Phase : uint8_t { PH_FIRST, PH_KEY, PH_COLON, PH_VALUE, PH_AFTER };
  struct Frame { bool isObj; Phase phase; uint8_t baseLen; int32_t idx; uint32_t id; };

  Callback cb_ = nullptr;
  void *ctx_ = nullptr;
  State state_ = ST_START;
  Frame st_[MAX_DEPTH];
  uint8_t depth_ = 0;
  uint32_t nextId_ = 1;
  char path_[PATH_CAP];
  uint8_t pathLen_ = 0;
  char text_[TEXT_CAP];
  uint8_t textLen_ = 0;
  bool esc_ = false;
  uint8_t uni_ = 0;          // pending \uXXXX hex digits to skip
  bool isKey_ = false;       // string being read is an object key
  bool trunc_ = false;

  void pushFrame(bool isObj);
  void popFrame();
  void setKey();
  void emit(JsonKind k);
  void valueDone();
  void fail() { state_ = ST_ERR; }
  bool scalarOk(JsonKind k) const;
  void appendPath(const char *s, size_t n);
};
