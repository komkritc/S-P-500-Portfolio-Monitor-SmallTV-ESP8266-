#include "JsonStream.h"
#include <string.h>

bool JsonStream::scalarOk(JsonKind k) const {
  if (k == JK_NULL) return !strcmp(text_, "null");
  if (k == JK_BOOL) return !strcmp(text_, "true") || !strcmp(text_, "false");
  if (!(text_[0] == '-' || (text_[0] >= '0' && text_[0] <= '9'))) return false;
  for (const char *p = text_; *p; p++)
    if (!((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E')) return false;
  return true;
}

void JsonStream::begin(Callback cb, void *ctx) {
  cb_ = cb; ctx_ = ctx;
  state_ = ST_START; depth_ = 0; nextId_ = 1; pathLen_ = 0; path_[0] = 0;
  textLen_ = 0; esc_ = false; uni_ = 0; isKey_ = false; trunc_ = false;
}

void JsonStream::appendPath(const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (pathLen_ + 1 >= PATH_CAP) break;     // overflow: path no longer matches anything
    path_[pathLen_++] = s[i];
  }
  path_[pathLen_] = 0;
}

void JsonStream::pushFrame(bool isObj) {
  if (depth_ >= MAX_DEPTH) { fail(); return; }
  Frame &f = st_[depth_++];
  f.isObj = isObj;
  f.phase = PH_FIRST;
  f.id = nextId_++;
  f.idx = isObj ? -1 : 0;
  if (!isObj) appendPath("[]", 2);
  f.baseLen = pathLen_;
}

void JsonStream::popFrame() {
  if (!depth_) { fail(); return; }
  depth_--;
  if (depth_) {
    Frame &p = st_[depth_ - 1];
    pathLen_ = p.baseLen;
    path_[pathLen_] = 0;
  } else {
    pathLen_ = 0; path_[0] = 0;
  }
}

void JsonStream::setKey() {
  Frame &f = st_[depth_ - 1];
  pathLen_ = f.baseLen;
  path_[pathLen_] = 0;
  if (pathLen_) appendPath(".", 1);
  appendPath(text_, textLen_);
  f.phase = PH_COLON;
}

void JsonStream::emit(JsonKind k) {
  text_[textLen_] = 0;
  int idx = (depth_ && !st_[depth_ - 1].isObj) ? (int)st_[depth_ - 1].idx : -1;
  if (cb_) cb_(ctx_, path_, idx, k, text_);
}

// A scalar/container finished: top level -> DONE, else wait for ',' or the closer.
void JsonStream::valueDone() {
  state_ = depth_ ? ST_VALUE : ST_DONE;
  if (depth_) st_[depth_ - 1].phase = PH_AFTER;
  textLen_ = 0;
}

bool JsonStream::feed(const char *d, size_t len) {
  for (size_t i = 0; i < len && state_ != ST_ERR; i++) {
    char c = d[i];
    switch (state_) {
      case ST_DONE:
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t' && c != 0) fail();
        break;

      case ST_START:
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || (uint8_t)c == 0xEF || (uint8_t)c == 0xBB || (uint8_t)c == 0xBF) break;
        if (c != '{' && c != '[') { fail(); break; }   // HTML / error page, not JSON
        pushFrame(c == '{');
        state_ = ST_VALUE;
        break;

      case ST_STRING: {
        if (uni_) { uni_--; if (!uni_ && textLen_ + 1 < TEXT_CAP) text_[textLen_++] = '?'; break; }
        if (esc_) {
          esc_ = false;
          if (c == 'u') { uni_ = 4; break; }
          char r = c == 'n' ? '\n' : c == 't' ? '\t' : c;
          if (textLen_ + 1 < TEXT_CAP) text_[textLen_++] = r;
          break;
        }
        if (c == '\\') { esc_ = true; break; }
        if (c == '"') {
          text_[textLen_] = 0;
          if (isKey_) { setKey(); textLen_ = 0; isKey_ = false; state_ = ST_VALUE; }
          else { emit(JK_STR); valueDone(); }
          break;
        }
        if (textLen_ + 1 < TEXT_CAP) text_[textLen_++] = c; else trunc_ = true;   // longer strings are cut, truncated() reports it
        break;
      }

      case ST_SCALAR:
        if (c == ',' || c == ']' || c == '}' || c == ' ' || c == '\n' || c == '\r' || c == '\t') {
          text_[textLen_] = 0;
          JsonKind k = (text_[0] == 'n') ? JK_NULL : (text_[0] == 't' || text_[0] == 'f') ? JK_BOOL : JK_NUM;
          if (!scalarOk(k)) { fail(); break; }
          emit(k);
          valueDone();
          i--;          // re-process the delimiter in ST_VALUE
          break;
        }
        if (textLen_ + 1 < TEXT_CAP) text_[textLen_++] = c; else { fail(); }
        break;

      case ST_VALUE: {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') break;
        Frame &f = st_[depth_ - 1];
        if (c == ',') {
          if (f.phase != PH_AFTER) { fail(); break; }
          if (f.isObj) f.phase = PH_KEY; else { f.idx++; f.phase = PH_VALUE; }
          break;
        }
        if (c == ':') {
          if (!f.isObj || f.phase != PH_COLON) { fail(); break; }
          f.phase = PH_VALUE;
          break;
        }
        if (c == '}' || c == ']') {
          if ((c == '}') != f.isObj || (f.phase != PH_AFTER && f.phase != PH_FIRST)) { fail(); break; }
          popFrame();
          if (state_ != ST_ERR) valueDone();
          break;
        }
        if (f.isObj && (f.phase == PH_FIRST || f.phase == PH_KEY)) {
          if (c != '"') { fail(); break; }
          isKey_ = true; textLen_ = 0; state_ = ST_STRING; break;
        }
        if (f.phase != PH_FIRST && f.phase != PH_VALUE) { fail(); break; }   // value where none is allowed
        if (c == '"') { isKey_ = false; textLen_ = 0; state_ = ST_STRING; break; }
        if (c == '{' || c == '[') { pushFrame(c == '{'); break; }   // container value: its path is the current key/array path
        textLen_ = 0; text_[textLen_++] = c; state_ = ST_SCALAR;
        break;
      }
      default: break;
    }
  }
  return state_ != ST_ERR;
}
