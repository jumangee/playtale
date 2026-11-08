#pragma once

#include <StreamingJsonParser.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Streams one page of the КвестБук catalogue, as served by
//   GET https://quest-book.ru/online/api/list/<kind>/?page=<N>
//
//   {"result":"ok","data":{"items":[{...}],"is_more":true}}
//
// Only the fields the device shows are retained; promotion metadata
// (`gameachis`, `awards`) and the timestamps are skipped. The response streams
// through StreamingJsonParser, so nothing holds the document: a page of 12 games
// costs one row per game plus the SAX token buffer.
class QuestBookListParser {
 public:
  QuestBookListParser();
  // Holds a 2 KB SAX token buffer and the row strings: copying one would
  // duplicate both, so it is not copyable.
  QuestBookListParser(const QuestBookListParser&) = delete;
  QuestBookListParser& operator=(const QuestBookListParser&) = delete;

  // One catalogue entry. Strings keep the service's own UTF-8 text.
  struct Row {
    std::string gameId;     // game_id: numeric id as text
    std::string indexName;  // index_name: the download path segment
    std::string title;      // fullname
    std::string author;     // author_name
    std::string tags;       // tags, comma separated
    std::string description;  // desc, capped at 140 characters by the service
    // cover: the original upload's absolute URL. The device never fetches it —
    // it is the key of the on-device cover cache, so a re-uploaded cover (a new
    // URL) is a new cache entry.
    std::string cover;
    int stars = 0;            // 0..5; 0 when the game has no rating yet
    int likes = 0;
    int dislikes = 0;
  };

  // The service pages at 12 entries, so one page plus slack is the whole
  // window. The cap bounds what a changed or misbehaving response can make the
  // device retain; extra rows are dropped and reported through rowsTruncated().
  static constexpr size_t MAX_ROWS = 16;

  // Per-field byte caps, each with headroom over the captured page's maximums
  // (title 56 B, author 29 B, tags 101 B, description 255 B): the platform caps
  // a description at 140 characters, which is up to 280 bytes in Cyrillic.
  // Longer values are cut on a UTF-8 boundary so a truncated title cannot end
  // mid-codepoint.
  static constexpr size_t TITLE_MAX = 160;
  static constexpr size_t AUTHOR_MAX = 64;
  static constexpr size_t TAGS_MAX = 128;
  static constexpr size_t DESCRIPTION_MAX = 288;
  // The longest captured cover URL is 78 bytes. Unlike the text fields this one
  // is an identifier: a truncated value would hash to another game's cache key,
  // so a longer URL is dropped instead of cut, and that game simply has no
  // cover.
  static constexpr size_t COVER_MAX = 96;
  static constexpr size_t INDEX_NAME_MAX = 32;
  static constexpr size_t GAME_ID_MAX = 16;

  // Clears rows and flags for the next page, keeping the allocated capacity.
  void reset();

  // Delivers one response chunk. Chunks may split any token, including inside
  // a UTF-8 escape or a control character.
  void feed(const char* data, size_t len);
  // Reports the end of the response. False when it ended mid-document.
  bool parseEnd();

  const std::vector<Row>& rows() const { return items; }
  // The service's `is_more` flag: further pages exist.
  bool hasMore() const { return more; }
  // The response carried more rows than MAX_ROWS; the page is incomplete.
  bool rowsTruncated() const { return truncatedRows; }
  // `result` was present and not "ok".
  bool failed() const { return resultFailed; }
  // The input was malformed or truncated.
  bool malformed() const { return badInput; }
  // The response parsed, reported a usable result, and carried the item array:
  // an `items`-less answer is a changed contract, not an empty page.
  bool ok() const { return !badInput && !resultFailed && depth == 0 && sawItems; }

 private:
  // The SAX callback table for one reader instance.
  static JsonCallbacks makeCallbacks(QuestBookListParser* self);

  static void onKeyCb(void* ctx, const char* key, size_t len);
  static void onStringCb(void* ctx, const char* value, size_t len);
  static void onNumberCb(void* ctx, const char* value, size_t len);
  static void onBoolCb(void* ctx, bool value);
  static void onNullCb(void* ctx);
  static void onObjectStartCb(void* ctx);
  static void onObjectEndCb(void* ctx);
  static void onArrayStartCb(void* ctx);
  static void onArrayEndCb(void* ctx);

  void onKey(const char* name, size_t len);
  void onValue(const char* value, size_t len);
  void onObjectStart();
  void onObjectEnd();
  void onArrayStart();
  void onArrayEnd();
  void beginRow();
  void finishRow();
  bool keyIs(const char* name, size_t len) const;
  // The current member is the catalogue page's own `is_more`.
  bool isPageFlag() const;

  StreamingJsonParser json;
  std::vector<Row> items;
  Row current;
  // Open-container count (0 before the root object) and the depths of the item
  // array and the open item object, so member values are recognised without a
  // path stack. One bit per level records object vs array, which is what turns
  // a mismatched closer into a reported error instead of a silent misparse.
  uint8_t depth = 0;
  uint8_t itemsDepth = 0;
  uint8_t itemDepth = 0;
  // Depth of the object that carries `is_more`; it outlives the item array.
  uint8_t pageDepth = 0;
  uint32_t arrayMask = 0;
  bool inRow = false;
  bool truncatedRows = false;
  bool resultFailed = false;
  bool badInput = false;
  // Any container was seen: an empty body is not a document.
  bool sawContainer = false;
  // The `items` array was seen; without it the page is not a catalogue page
  // (a failed `result` answer has none, which ok() rejects but parseEnd()
  // still accepts as a well-formed document).
  bool sawItems = false;
  bool more = false;
  char key[24] = {};
  uint8_t keyLen = 0;
};
