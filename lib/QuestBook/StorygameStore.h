#pragma once

#include <HalStorage.h>

#include <string>

#include "StorygameFormat.h"
#include "StorygameMeta.h"

// The parts of a downloaded game's tree that are not the import itself: whether
// the game is on the card, how it is removed, and how its cover comes to sit
// beside it (docs/storygames/ARCHITECTURE.md §4.2, §4.9, §6.3).
//
// Paths and the on-SD layout are in StorygameFormat.h; this file is the HAL side
// of the same directory, so the game page, the library walk and the importer
// agree on what "downloaded" means instead of each testing a file of its own
// choosing.
namespace questbook_game {

// True when `gameDir` (the "/storygames/<index_name>" directory itself) holds a
// complete game: game.hdr, game.idx, game.dat and meta.json. That set is the
// visibility rule, not directory existence: the importer creates the directory
// and the three content files before any byte of the document arrives, and writes
// meta.json last, so an import interrupted by a power cut leaves files but is
// correctly not downloaded — and the next import replaces them. Anything that
// lists games must ask this, never test the directory (§4.9). cover.bmp is
// deliberately not required: a game whose cover never arrived is still a playable
// game.
inline bool isDownloadedAt(const std::string& gameDir) {
  if (gameDir.empty()) return false;
  for (const char* leaf : {HDR_FILE_NAME, IDX_FILE_NAME, DAT_FILE_NAME, META_FILE_NAME}) {
    std::string filePath = gameDir;
    filePath += '/';
    filePath += leaf;
    if (!Storage.exists(filePath.c_str())) return false;
  }
  return true;
}

// isDownloadedAt() for a bare index_name. False for an unsafe name.
inline bool isDownloaded(const std::string& indexName) { return isDownloadedAt(directory(indexName)); }

// True when `path` is a downloaded game: a game directory that holds a complete
// game. This is the one predicate every consumer outside the importer uses — the
// library walk, the reader dispatch and the delete paths — because the path shape
// alone is not enough. A book file the reader dropped into /storygames/ has a
// game-shaped path but is a book, and must open as one.
inline bool isGameAtPath(const std::string& path) {
  return !indexNameFromPath(path).empty() && isDownloadedAt(path);
}

// Reads the game's meta.json into `out`. False when it is missing or is not the
// flat object the importer writes; the caller then shows the folder name instead
// of a title. Inline because the library walk needs it and that walk is compiled
// on the host, where lib/QuestBook's other sources are not linked.
inline bool readMetaFile(const std::string& metaPath, Meta& out) {
  // The importer writes the catalogue's fields, whose description the service caps
  // at 140 characters; anything past this is not a meta.json.
  constexpr size_t MAX_META_BYTES = 8u * 1024u;
  // The size is bounded but the allocation is not guaranteed: the build has no
  // exceptions, so a std::string that cannot grow aborts instead of failing. This
  // file is untrusted card content, and HalStorage::readFileToString() already
  // probes the largest free block for exactly this reason.
  std::string text;
  if (!Storage.readFileToString("QBG", metaPath, MAX_META_BYTES, text)) return false;
  return parseMetaJson(text.data(), text.size(), out);
}

// Removes the whole game directory. True when nothing is left behind, including
// when there was nothing there.
bool removeGame(const std::string& indexName);

// removeGame() for a path the library index hands back, which for a game is the
// game's own directory. False when the path is not a game directory.
bool removeGameAtPath(const std::string& gameDir);

// Copies the page's resolved cover to /storygames/<index_name>/cover.bmp, which
// the page then prefers over its screen-slot cache (§6.3). A missing or
// oversized source is not an error: the game simply keeps its icon offline.
bool copyCover(const std::string& indexName, const std::string& sourcePath);

}  // namespace questbook_game
