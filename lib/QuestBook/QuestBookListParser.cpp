#include "QuestBookListParser.h"

#include <cstdlib>
#include <cstring>
#include <utility>

namespace {

// Copies at most `cap` bytes of `value`, backing off to a UTF-8 lead byte so a
// cut cannot leave half a codepoint in a rendered string.
void assignBounded(std::string& dst, const char* value, const size_t len, const size_t cap) {
  size_t take = len < cap ? len : cap;
  while (take > 0 && take < len && (static_cast<unsigned char>(value[take]) & 0xC0) == 0x80) --take;
  dst.assign(value, take);
}

// Digit fields arrive as JSON numbers (`stars`) or as strings (`likes`).
int countFrom(const char* value, const size_t len, const int limit) {
  if (len == 0 || len > 8) return 0;
  char buf[9];
  memcpy(buf, value, len);
  buf[len] = '\0';
  char* end = nullptr;
  const long parsed = strtol(buf, &end, 10);
  if (end == buf || *end != '\0' || parsed < 0) return 0;
  return parsed > limit ? limit : static_cast<int>(parsed);
}

}  // namespace

JsonCallbacks QuestBookListParser::makeCallbacks(QuestBookListParser* self) {
  JsonCallbacks callbacks;
  callbacks.ctx = self;
  callbacks.onKey = &QuestBookListParser::onKeyCb;
  callbacks.onString = &QuestBookListParser::onStringCb;
  callbacks.onNumber = &QuestBookListParser::onNumberCb;
  callbacks.onBool = &QuestBookListParser::onBoolCb;
  callbacks.onNull = &QuestBookListParser::onNullCb;
  callbacks.onObjectStart = &QuestBookListParser::onObjectStartCb;
  callbacks.onObjectEnd = &QuestBookListParser::onObjectEndCb;
  callbacks.onArrayStart = &QuestBookListParser::onArrayStartCb;
  callbacks.onArrayEnd = &QuestBookListParser::onArrayEndCb;
  return callbacks;
}

QuestBookListParser::QuestBookListParser() : json(makeCallbacks(this)) {
  // The accepted maximum, not the service's current page size: a page that
  // grows past 12 must not reallocate the row buffer mid-transfer.
  items.reserve(MAX_ROWS);
}

void QuestBookListParser::reset() {
  json.reset();
  items.clear();
  current = Row{};
  depth = itemsDepth = itemDepth = pageDepth = 0;
  arrayMask = 0;
  inRow = false;
  truncatedRows = false;
  resultFailed = false;
  badInput = false;
  sawContainer = false;
  sawItems = false;
  more = false;
  keyLen = 0;
  key[0] = '\0';
}

void QuestBookListParser::feed(const char* data, const size_t len) {
  if (badInput || len == 0) return;
  json.feed(data, len);
  if (json.hasError()) badInput = true;
}

bool QuestBookListParser::parseEnd() {
  if (json.hasError() || depth != 0 || !sawContainer) badInput = true;
  return !badInput;
}

// --- SAX dispatch ------------------------------------------------------------

void QuestBookListParser::onKeyCb(void* ctx, const char* key, const size_t len) {
  static_cast<QuestBookListParser*>(ctx)->onKey(key, len);
}
void QuestBookListParser::onStringCb(void* ctx, const char* value, const size_t len) {
  static_cast<QuestBookListParser*>(ctx)->onValue(value, len);
}
void QuestBookListParser::onNumberCb(void* ctx, const char* value, const size_t len) {
  static_cast<QuestBookListParser*>(ctx)->onValue(value, len);
}
void QuestBookListParser::onBoolCb(void* ctx, const bool value) {
  auto* self = static_cast<QuestBookListParser*>(ctx);
  self->onValue(value ? "1" : "0", 1);
  // `is_more` is the one boolean the catalogue reader needs.
  if (self->isPageFlag()) self->more = value;
}
void QuestBookListParser::onNullCb(void*) {}
void QuestBookListParser::onObjectStartCb(void* ctx) { static_cast<QuestBookListParser*>(ctx)->onObjectStart(); }
void QuestBookListParser::onObjectEndCb(void* ctx) { static_cast<QuestBookListParser*>(ctx)->onObjectEnd(); }
void QuestBookListParser::onArrayStartCb(void* ctx) { static_cast<QuestBookListParser*>(ctx)->onArrayStart(); }
void QuestBookListParser::onArrayEndCb(void* ctx) { static_cast<QuestBookListParser*>(ctx)->onArrayEnd(); }

bool QuestBookListParser::keyIs(const char* name, const size_t len) const {
  return keyLen == len && memcmp(key, name, len) == 0;
}

// True when the current member sits in the object that also holds `is_more`.
// Without the item array's depth there is no anchor, so an unrecognised page
// shape reports no further pages instead of guessing.
bool QuestBookListParser::isPageFlag() const {
  return pageDepth != 0 && depth == pageDepth && keyIs("is_more", 7);
}

void QuestBookListParser::onKey(const char* name, const size_t len) {
  const size_t take = len < sizeof(key) - 1 ? len : sizeof(key) - 1;
  if (take) memcpy(key, name, take);
  key[take] = '\0';
  keyLen = static_cast<uint8_t>(take);
}

void QuestBookListParser::onObjectStart() {
  if (badInput) return;
  if (depth >= 31) {
    badInput = true;  // deeper than the response contract allows
    return;
  }
  ++depth;
  sawContainer = true;
  arrayMask &= ~(1u << depth);
  if (itemsDepth != 0 && depth == static_cast<uint8_t>(itemsDepth + 1) && !inRow) {
    beginRow();
    itemDepth = depth;
  }
}

void QuestBookListParser::onObjectEnd() {
  if (badInput) return;
  if (depth == 0 || (arrayMask & (1u << depth)) != 0) {
    badInput = true;  // closer does not match the open container
    return;
  }
  if (inRow && depth == itemDepth) finishRow();
  --depth;
}

void QuestBookListParser::onArrayStart() {
  if (badInput) return;
  if (depth >= 31) {
    badInput = true;
    return;
  }
  ++depth;
  sawContainer = true;
  arrayMask |= 1u << depth;
  // The item array is the only one the reader descends into; its objects begin
  // at the next level and `is_more` sits in the object that contains it.
  if (itemsDepth == 0 && pageDepth == 0 && depth >= 2 && keyIs("items", 5)) {
    itemsDepth = depth;
    pageDepth = static_cast<uint8_t>(depth - 1);
    sawItems = true;
  }
}

void QuestBookListParser::onArrayEnd() {
  if (badInput) return;
  if (depth == 0 || (arrayMask & (1u << depth)) == 0) {
    badInput = true;
    return;
  }
  if (depth == itemsDepth) itemsDepth = 0;
  arrayMask &= ~(1u << depth);
  --depth;
}

// A member value of the open item object, or one of the catalogue-wide fields.
void QuestBookListParser::onValue(const char* value, const size_t len) {
  if (badInput) return;
  if (inRow && depth == itemDepth) {
    if (keyIs("fullname", 8)) {
      assignBounded(current.title, value, len, TITLE_MAX);
    } else if (keyIs("author_name", 11)) {
      assignBounded(current.author, value, len, AUTHOR_MAX);
    } else if (keyIs("game_id", 7)) {
      assignBounded(current.gameId, value, len, GAME_ID_MAX);
    } else if (keyIs("index_name", 10)) {
      assignBounded(current.indexName, value, len, INDEX_NAME_MAX);
    } else if (keyIs("tags", 4)) {
      assignBounded(current.tags, value, len, TAGS_MAX);
    } else if (keyIs("desc", 4)) {
      assignBounded(current.description, value, len, DESCRIPTION_MAX);
    } else if (keyIs("cover", 5)) {
      // An identifier, not display text: a value that does not fit is dropped
      // whole, because a truncated URL would collide with another game's key.
      if (len <= COVER_MAX) {
        assignBounded(current.cover, value, len, COVER_MAX);
      } else {
        current.cover.clear();
      }
    } else if (keyIs("stars", 5)) {
      current.stars = countFrom(value, len, 5);
    } else if (keyIs("likes", 5)) {
      current.likes = countFrom(value, len, 0x7FFF);
    } else if (keyIs("dislikes", 8)) {
      current.dislikes = countFrom(value, len, 0x7FFF);
    }
    return;
  }
  // Root object: the service reports a failed request in `result` with HTTP 200.
  if (depth == 1 && keyIs("result", 6)) {
    resultFailed = !(len == 2 && memcmp(value, "ok", 2) == 0);
  }
}

void QuestBookListParser::beginRow() {
  current = Row{};
  inRow = true;
}

void QuestBookListParser::finishRow() {
  inRow = false;
  itemDepth = 0;
  if (current.title.empty()) return;  // a row without a name cannot be listed
  if (items.size() >= MAX_ROWS) {
    truncatedRows = true;
    return;
  }
  items.push_back(std::move(current));
  current = Row{};
}
