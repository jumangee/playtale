#include "StorygameStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include "StorygameFormat.h"

namespace questbook_game {

namespace {

// One cover is at most 62 header bytes + 192 rows of 16 bytes for a 128-wide
// slot (3 134 bytes, §6.3). The cap is the point at which a file cannot be a
// cover for this page, so it is refused rather than copied.
constexpr size_t COVER_MAX_BYTES = 4096;
constexpr size_t COPY_CHUNK = 256;

bool writeAll(HalFile& file, const char* data, const size_t length) {
  size_t written = 0;
  while (written < length) {
    const size_t chunk = file.write(reinterpret_cast<const uint8_t*>(data + written), length - written);
    if (chunk == 0) return false;
    written += chunk;
  }
  return true;
}

}  // namespace

bool removeGame(const std::string& indexName) {
  const std::string dir = directory(indexName);
  if (dir.empty()) return false;
  if (!Storage.exists(dir.c_str())) return true;
  if (!Storage.removeDir(dir.c_str())) {
    LOG_ERR("QBG", "cannot remove game directory %s", dir.c_str());
    return false;
  }
  return true;
}

bool removeTempWorkspace() {
  const std::string dir = tempDirectory();
  if (!Storage.exists(dir.c_str())) return true;
  if (!Storage.removeDir(dir.c_str())) {
    LOG_ERR("QBG", "cannot remove the play workspace %s", dir.c_str());
    return false;
  }
  return true;
}

bool removeGameAtPath(const std::string& gameDir) {
  // Only the game's own directory is ever removed here: a path from the library
  // index that is not one is refused rather than handed to removeDir().
  const std::string indexName = indexNameFromPath(gameDir);
  if (indexName.empty()) return false;
  return removeGame(indexName);
}

bool copyCoverTo(const std::string& gameDir, const std::string& sourcePath) {
  if (gameDir.empty() || sourcePath.empty()) return false;
  // Only a game directory or Play's workspace may receive the file. The destination
  // is built from a caller-supplied path, and a cover must not be written anywhere
  // else on the card.
  if (!isTempWorkspace(gameDir) && indexNameFromPath(gameDir).empty()) {
    LOG_ERR("QBG", "%s is not a game directory", gameDir.c_str());
    return false;
  }
  const std::string destination = pathIn(gameDir, COVER_FILE_NAME);
  // A game that already has its cover keeps it, even when the catalogue later
  // re-uploads the art: the file is the game's own, and rewriting it on every Play
  // would cost a write for a change nobody asked for. A fresh import clears the
  // tree first, so a re-download does replace the art.
  if (sourcePath == destination || Storage.exists(destination.c_str())) return true;

  HalFile source;
  if (!Storage.openFileForRead("QBG", sourcePath, source)) return false;
  const size_t sourceBytes = source.fileSize();
  if (sourceBytes == 0 || sourceBytes > COVER_MAX_BYTES) {
    LOG_ERR("QBG", "cover at %s is not a page cover (%u bytes)", sourcePath.c_str(),
            static_cast<unsigned>(sourceBytes));
    source.close();
    return false;
  }

  HalFile target;
  // Staged like every other write this firmware does (SDCardManager::replaceFile,
  // and the downloader's own "<dest>.part"): an interrupted copy must not leave a
  // permanent bad cover under the real name, because the read path will not
  // delete a file that belongs to the game.
  const std::string staging = destination + ".tmp";
  if (!Storage.openFileForWrite("QBG", staging, target)) {
    source.close();
    return false;
  }

  char buffer[COPY_CHUNK];
  size_t copied = 0;
  bool ok = true;
  while (copied < sourceBytes) {
    const size_t want = sourceBytes - copied < COPY_CHUNK ? sourceBytes - copied : COPY_CHUNK;
    const int read = source.read(buffer, want);
    if (read <= 0) {
      ok = false;
      break;
    }
    if (!writeAll(target, buffer, static_cast<size_t>(read))) {
      ok = false;
      break;
    }
    copied += static_cast<size_t>(read);
  }
  target.flush();
  if (!target.close()) ok = false;
  source.close();
  if (ok) ok = Storage.replaceFile(staging.c_str(), destination.c_str());
  if (!ok) {
    // Only the staging file is removed: a cover already beside the game is left
    // alone, because a stale copy is better than none. `replaceFile` is what
    // makes the swap, so a failure there has already taken the old file away.
    LOG_ERR("QBG", "cover copy to %s failed", destination.c_str());
    Storage.remove(staging.c_str());
  }
  return ok;
}

}  // namespace questbook_game