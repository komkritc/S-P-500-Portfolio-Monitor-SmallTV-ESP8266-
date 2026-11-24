// FlatJson.h - parses a flat JSON object body ({"k":"v","n":1,"b":true}) into a small fixed table.
// Used for every POST body and for the on-flash config files. Nested containers are ignored
// (their scalar members are skipped); duplicate keys keep the last value.
#pragma once
#include "../finance/JsonStream.h"
#include <stddef.h>

class FlatJson {
 public:
  static const uint8_t MAX_KEYS = 14;
  static const uint8_t KEY_LEN = 20;

  // false on syntax error, truncated document, over-long string value (>= TEXT_CAP-1) or too many keys.
  bool parse(const char *json, size_t len);
  bool has(const char *key) const { return find(key) >= 0; }
  const char *str(const char *key, const char *def = "") const;       // text of any scalar (num/bool as written)
  bool isStr(const char *key) const;
  bool isNull(const char *key) const;
  // strict numbers: false when missing, not a JSON number, or (integerOnly) not an integer
  bool num(const char *key, double &out, bool integerOnly = false) const;
  bool boolean(const char *key, bool &out) const;

 private:
  struct Item { char key[KEY_LEN]; char val[JsonStream::TEXT_CAP]; JsonKind kind; };
  Item items_[MAX_KEYS];
  uint8_t n_ = 0;
  bool overflow_ = false;
  int find(const char *key) const;
  static void onEvent(void *ctx, const char *path, int idx, JsonKind k, const char *text);
};
