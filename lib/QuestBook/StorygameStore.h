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
    if (!Storage.exists(pathIn(gameDir, leaf).c_str())) return false;
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

// True when `path` is a game directory that is not playable any more: the shape of a
// game's directory, with at least one of its own files still there. That is the
// evidence a tree is a broken game rather than something else — a book file inside
// /storygames/ has the shape but none of the files. Callers use it to reach the game
// page (where the game can be downloaded again) instead of a reader that cannot open
// it (§4.9). A directory emptied of every file is indistinguishable from a folder,
// so it is not recognised here.
inline bool isIncompleteGameAtPath(const std::string& path) {
  if (indexNameFromPath(path).empty()) return false;
  return Storage.exists(pathIn(path, HDR_FILE_NAME).c_str()) ||
         Storage.exists(pathIn(path, META_FILE_NAME).c_str());
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

// Removes the shared Play workspace and the one temporary game in it. True when
// nothing is left behind, including when the workspace was already empty. The
// workspace is not a game name, so this is the only way to delete it (§6.3).
bool removeTempWorkspace();

// True when the shared Play workspace holds the game named `indexName`. Its own
// meta.json records which game was imported there last, so this is how a caller
// tells "the workspace card is this same game" from "another game is in the
// workspace" (§6.3). False when the workspace is empty or holds a half import.
inline bool tempWorkspaceHolds(const std::string& indexName) {
  if (indexName.empty() || !isDownloadedAt(tempDirectory())) return false;
  Meta meta;
  if (!readMetaFile(tempPath(META_FILE_NAME), meta)) return false;
  return meta.indexName == indexName;
}

// removeGame() for a path the library index hands back, which for a game is the
// game's own directory. False when the path is not a game directory.
bool removeGameAtPath(const std::string& gameDir);

// Copies the page's resolved cover to `<gameDir>/cover.bmp`: the game's own
// directory for a downloaded game, or Play's shared workspace for a temporary one.
// A permanent copy is what the game page resolves first, so it draws the cover
// offline; the workspace copy is what the recent-books card reads, because the page
// only ever resolves `/storygames/<index_name>/cover.bmp` (§6.3). A missing or
// oversized source is not an error: the game simply keeps its icon offline.
bool copyCoverTo(const std::string& gameDir, const std::string& sourcePath);

}  // namespace questbook_game
