#include "FlatJson.h"
#include <string.h>
#include <stdlib.h>

void FlatJson::onEvent(void *ctx, const char *path, int, JsonKind k, const char *text) {
  FlatJson *f = static_cast<FlatJson *>(ctx);
  if (strchr(path, '.') || strchr(path, '[')) return;     // nested: ignore
  int i = f->find(path);
  if (i < 0) {
    if (f->n_ >= MAX_KEYS || strlen(path) >= KEY_LEN) { f->overflow_ = true; return; }
    i = f->n_++;
    strcpy(f->items_[i].key, path);
  }
  strncpy(f->items_[i].val, text, sizeof f->items_[i].val - 1);
  f->items_[i].val[sizeof f->items_[i].val - 1] = 0;
  f->items_[i].kind = k;
}

bool FlatJson::parse(const char *json, size_t len) {
  n_ = 0; overflow_ = false;
  if (!json) return false;
  size_t i = 0;
  while (i < len && (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' || json[i] == '\t')) i++;
  if (i >= len || json[i] != '{') return false;    // POST bodies and config files are objects
  JsonStream js;
  js.begin(&FlatJson::onEvent, this);
  if ( !js.feed(json, len) || !js.complete() || js.truncated() || overflow_) return false;
  return true;
}

int FlatJson::find(const char *key) const {
  for (uint8_t i = 0; i < n_; i++) if (!strcmp(items_[i].key, key)) return i;
  return -1;
}

const char *FlatJson::str(const char *key, const char *def) const { int i = find(key); return i < 0 ? def : items_[i].val; }
bool FlatJson::isStr(const char *key) const { int i = find(key); return i >= 0 && items_[i].kind == JK_STR; }
bool FlatJson::isNull(const char *key) const { int i = find(key); return i >= 0 && items_[i].kind == JK_NULL; }

bool FlatJson::num(const char *key, double &out, bool integerOnly) const {
  int i = find(key);
  if (i < 0 || items_[i].kind != JK_NUM) return false;
  const char *s = items_[i].val;
  char *end = nullptr;
  double v = strtod(s, &end);
  if (end == s || *end) return false;
  if (integerOnly && (strpbrk(s, ".eE") || v != (double)(long long)v)) return false;
  out = v;
  return true;
}

bool FlatJson::boolean(const char *key, bool &out) const {
  int i = find(key);
  if (i < 0 || items_[i].kind != JK_BOOL) return false;
  out = items_[i].val[0] == 't';
  return true;
}
