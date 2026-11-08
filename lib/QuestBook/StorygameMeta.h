#pragma once

#include <cstddef>
#include <string>

#include "StorygameFormat.h"

// Reads the flat meta.json the importer writes (docs/storygames/ARCHITECTURE.md
// §4.2, docs/file-formats.md). The library walk needs the game's real title and
// author from this file, and it runs inside lib/LibraryIndex, which is compiled on
// the host as well: ArduinoJson is not part of that build.
//
// The SDK's StreamingJsonParser is available there and does unescape \u, but it
// carries a fixed 2 KB token buffer plus a callback table, and the walk is
// deliberately allocation-frugal (see the long comment in LibraryBuilder.cpp). For
// six short strings out of a file the importer itself wrote, a reader with no fixed
// buffer and no dependency is the cheaper shape; it also refuses anything that is
// not the flat object of strings the importer writes, which is the failure the
// library cares about. Values are capped so a corrupt file cannot grow a
// std::string without bound.
namespace questbook_game {

namespace meta_detail {

// One value's ceiling. The importer refuses to write a field past
// StorygameImporter's META_TEXT_MAX (512), and the service caps the description at
// 140 characters, so a longer value is not something this firmware wrote. Keeping
// the two equal also keeps the reader's own growth small: the file body is already
// resident when the values are built, and a value that doubles past 512 bytes would
// ask for a heap block that the read's own probe never covered. The build has no
// exceptions, so such a request aborts rather than failing.
constexpr size_t MAX_META_VALUE_BYTES = 512;

inline bool isJsonSpace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

inline void skipJsonSpace(const char*& cursor, const char* const end) {
  while (cursor < end && isJsonSpace(*cursor)) ++cursor;
}

// One code point as UTF-8. A lone surrogate is not a character, so it becomes the
// replacement character instead of an invalid sequence.
inline void appendCodePoint(std::string& out, const unsigned codePoint) {
  if (codePoint == 0 || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
    out += "\xEF\xBF\xBD";
  } else if (codePoint < 0x80) {
    out += static_cast<char>(codePoint);
  } else if (codePoint < 0x800) {
    out += static_cast<char>(0xC0 | (codePoint >> 6));
    out += static_cast<char>(0x80 | (codePoint & 0x3F));
  } else if (codePoint < 0x10000) {
    out += static_cast<char>(0xE0 | (codePoint >> 12));
    out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (codePoint & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (codePoint >> 18));
    out += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (codePoint & 0x3F));
  }
}

// Four hex digits at `cursor`; advances past them.
inline bool readHex4(const char*& cursor, const char* const end, unsigned& value) {
  if (end - cursor < 4) return false;
  value = 0;
  for (int i = 0; i < 4; ++i) {
    const char c = cursor[i];
    value <<= 4;
    if (c >= '0' && c <= '9') {
      value |= static_cast<unsigned>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      value |= static_cast<unsigned>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      value |= static_cast<unsigned>(c - 'A' + 10);
    } else {
      return false;
    }
  }
  cursor += 4;
  return true;
}

// `cursor` points at the opening quote. The importer's writer emits raw UTF-8 and
// escapes only what JSON requires, but a hand-edited file may use \u, so both are
// accepted. A member longer than MAX_META_VALUE_BYTES fails the whole parse: the
// caller then falls back to the file name rather than showing a truncated title.
inline bool readJsonString(const char*& cursor, const char* const end, std::string& out) {
  if (cursor >= end || *cursor != '"') return false;
  ++cursor;
  out.clear();
  while (cursor < end) {
    const char c = *cursor++;
    if (c == '"') return true;
    if (c != '\\') {
      if (static_cast<unsigned char>(c) < 0x20) return false;  // raw control byte
      if (out.size() >= MAX_META_VALUE_BYTES) return false;
      out += c;
      continue;
    }
    if (cursor >= end) return false;
    const char escape = *cursor++;
    switch (escape) {
      case '"':
      case '\\':
      case '/':
        out += escape;
        break;
      case 'b':
        out += '\b';
        break;
      case 'f':
        out += '\f';
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      case 'u': {
        unsigned codePoint = 0;
        if (!readHex4(cursor, end, codePoint)) return false;
        // A high surrogate is only a character together with the low one that
        // follows it; without one, appendCodePoint() writes U+FFFD.
        if (codePoint >= 0xD800 && codePoint <= 0xDBFF && end - cursor >= 6 && cursor[0] == '\\' && cursor[1] == 'u') {
          const char* pairCursor = cursor + 2;
          unsigned low = 0;
          if (readHex4(pairCursor, end, low) && low >= 0xDC00 && low <= 0xDFFF) {
            codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
            cursor = pairCursor;
          }
        }
        appendCodePoint(out, codePoint);
        break;
      }
      default:
        return false;
    }
    if (out.size() > MAX_META_VALUE_BYTES) return false;
  }
  return false;  // unterminated
}

// Skips a value this reader does not keep: a string, a container, a literal or a
// number. Containers are walked with their strings honoured, so a brace inside a
// string cannot end the object early.
inline bool skipJsonValue(const char*& cursor, const char* const end) {
  skipJsonSpace(cursor, end);
  if (cursor >= end) return false;
  if (*cursor == '"') {
    std::string ignored;
    return readJsonString(cursor, end, ignored);
  }
  if (*cursor == '{' || *cursor == '[') {
    // Both bracket kinds count towards one depth: JSON containers nest properly,
    // and a bracket inside a string is skipped with the string, so a counter is
    // enough and `{"a": [{"b": 1}]}` ends where it should.
    int depth = 0;
    while (cursor < end) {
      const char c = *cursor;
      if (c == '"') {
        std::string ignored;
        if (!readJsonString(cursor, end, ignored)) return false;
        continue;
      }
      ++cursor;
      if (c == '{' || c == '[') {
        ++depth;
      } else if (c == '}' || c == ']') {
        if (--depth == 0) return true;
      }
    }
    return false;
  }
  // Literal or number: consume until the member or the object ends.
  const char* const start = cursor;
  while (cursor < end && *cursor != ',' && *cursor != '}' && *cursor != ']' && !isJsonSpace(*cursor)) ++cursor;
  return cursor > start;
}

inline bool assignMetaField(const std::string& key, std::string&& value, Meta& out) {
  if (key == "game_id") {
    out.gameId = std::move(value);
  } else if (key == "index_name") {
    out.indexName = std::move(value);
  } else if (key == "fullname") {
    out.title = std::move(value);
  } else if (key == "author_name") {
    out.author = std::move(value);
  } else if (key == "tags") {
    out.tags = std::move(value);
  } else if (key == "desc") {
    out.description = std::move(value);
  }
  // Unknown members are accepted and ignored: a newer writer may add fields and
  // this reader must not stop listing the game because of them.
  return true;
}

}  // namespace meta_detail

// Parses the importer's meta.json. False on anything that is not that flat object
// of strings; `out` is then partially filled and must be ignored.
inline bool parseMetaJson(const char* text, const size_t length, Meta& out) {
  using namespace meta_detail;
  if (text == nullptr) return false;
  const char* cursor = text;
  const char* const end = text + length;
  skipJsonSpace(cursor, end);
  if (cursor >= end || *cursor != '{') return false;
  ++cursor;
  skipJsonSpace(cursor, end);
  if (cursor < end && *cursor == '}') return true;  // an empty object has no fields
  while (cursor < end) {
    skipJsonSpace(cursor, end);
    std::string key;
    if (!readJsonString(cursor, end, key)) return false;
    skipJsonSpace(cursor, end);
    if (cursor >= end || *cursor != ':') return false;
    ++cursor;
    skipJsonSpace(cursor, end);
    if (cursor >= end) return false;
    if (*cursor == '"') {
      std::string value;
      if (!readJsonString(cursor, end, value)) return false;
      assignMetaField(key, std::move(value), out);
    } else if (!skipJsonValue(cursor, end)) {
      return false;
    }
    skipJsonSpace(cursor, end);
    if (cursor >= end) return false;
    if (*cursor == ',') {
      ++cursor;
      continue;
    }
    return *cursor == '}';
  }
  return false;
}

}  // namespace questbook_game
