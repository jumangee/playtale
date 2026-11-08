#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// The game page's cover: where it lives on the SD card and how it is fetched.
//
// The service converts the catalogue's `cover` URL into a 1-bit BMP sized for
// this page, so the device never decodes a JPEG/PNG/GIF for it and never pulls
// the multi-megabyte original (docs/storygames/ARCHITECTURE.md §8 #5). Drawing
// it is Bitmap + GfxRenderer::drawBitmap1Bit, the same path the home screen's
// thumbnails use, so no image decoder runs for a cover.
//
// The transfer is supplied by the caller (src/network/HttpDownloader.h), which
// also owns the endpoint URL and the radio preconditions: this library holds the
// naming, the validation and the storage policy, and knows nothing about HTTP.
// The header-inline helpers are what the host test exercises; ensure() and the
// BMP check need the HAL and must be verified on the device.
namespace questbook_cover {

// The hero slot the game page reserves, in pixels. The service is asked for
// exactly this box and the slot is part of the cache key, so a different slot
// is a different file. An answer is never larger than the box, but may be
// smaller: the endpoint preserves the source aspect ratio and never upscales.
inline constexpr int SLOT_WIDTH = 128;
inline constexpr int SLOT_HEIGHT = 192;

// Bumped when a cached file stops meaning what it meant: a different pixel
// pipeline, slot or dithering. It is part of every cache key, so a bump
// rebuilds the whole cache instead of drawing stale art (the reason
// Epub::getCoverBmpPath() carries `_legacy_v2` in its name).
inline constexpr int CACHE_VERSION = 1;

// Screen-slot cache. One file of at most ~3 KB per game, created on first use;
// nothing else lives here. CACHE_DIR_NAME is the leaf that
// src/util/BookCacheUtils.cpp recognises, so Settings -> Clear cache reclaims
// the directory.
inline constexpr const char* CACHE_DIR_NAME = "qbcover";
inline constexpr const char* CACHE_DIR = "/.crosspoint/qbcover";

// Downloaded games keep their own copy under the library, so a cover survives
// both an emptied cache and an offline library (ARCHITECTURE.md §4.2). The
// library root is the SD root today, which is what makes this the literal path;
// whatever writes a game's files must agree with it.
inline constexpr const char* GAMES_DIR = "/storygames";

// Matches QuestBookListParser::INDEX_NAME_MAX: the catalogue truncates longer
// names, so a longer one cannot be a real game.
inline constexpr size_t INDEX_NAME_MAX = 32;

// True for the characters every observed index_name uses. A name becomes both a
// path segment and a URL segment, so anything outside this set is refused
// rather than escaped: a game with such a name loses its cover, not the
// filesystem. Consecutive dots are refused because ".." would climb out.
inline bool isSafeIndexName(const std::string& indexName) {
  if (indexName.empty() || indexName.size() > INDEX_NAME_MAX || indexName == ".") return false;
  bool previousDot = false;
  for (const char c : indexName) {
    const bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (!alnum && c != '_' && c != '-' && c != '.') return false;
    if (c == '.' && previousDot) return false;
    previousDot = c == '.';
  }
  return true;
}

// "<hash>_v1_128x192.bmp". `urlHash` is the lowercase hex MD5 of the catalogue's
// `cover` URL: that URL carries the upload's own random suffix, so a re-uploaded
// cover is a different key with no extra invalidation step.
inline std::string cacheFileName(const std::string& urlHash, const int width, const int height) {
  std::string name = urlHash;
  name += "_v";
  name += std::to_string(CACHE_VERSION);
  name += '_';
  name += std::to_string(width);
  name += 'x';
  name += std::to_string(height);
  name += ".bmp";
  return name;
}

// CACHE_DIR + "/" + cacheFileName().
inline std::string cachePath(const std::string& urlHash, const int width, const int height) {
  std::string path = CACHE_DIR;
  path += '/';
  path += cacheFileName(urlHash, width, height);
  return path;
}

// GAMES_DIR + "/" + indexName + "/cover.bmp"; empty when the name is not safe.
inline std::string storedPath(const std::string& indexName) {
  if (!isSafeIndexName(indexName)) return {};
  std::string path = GAMES_DIR;
  path += '/';
  path += indexName;
  path += "/cover.bmp";
  return path;
}

// What the caller's transfer reported. Every non-Ok status ends the same way —
// the icon this visit, a fresh attempt on the next one — so the distinction is
// diagnostic: it decides what is logged, and nothing else.
enum class FetchStatus : uint8_t {
  Ok,         // the whole body is at the destination path
  Refused,    // the server answered, but not with a cover (HTTP error)
  NoSession,  // no radio association to transfer over
  LowMemory,  // no room for the transfer
  Failed,     // transport or file error
};

// Writes the whole body of `url` to `destPath`. Implemented by the firmware.
using Fetcher = FetchStatus (*)(const std::string& url, const std::string& destPath);

// Resolves the cover for one game and, when it is neither beside the game nor
// cached, fetches `fetchUrl` through `fetcher`. Blocks for the transfer, so it
// must not run on the render task. Sets `outPath` and returns true when a file
// to draw is available; the two local tiers are resolved without any network,
// so an offline page still shows a cover it already has.
bool ensure(const std::string& indexName, const std::string& coverUrl, const std::string& fetchUrl, Fetcher fetcher,
            std::string& outPath);

}  // namespace questbook_cover