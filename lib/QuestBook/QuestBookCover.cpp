#include "QuestBookCover.h"

#include <Bitmap.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>
#include <Memory.h>

namespace questbook_cover {
namespace {

// The service answers with a 62-byte header plus 1 bpp rows: a 128x192 cover is
// 3 134 bytes. This bound only rejects a wrong answer (an error page, a full
// image), never a real cover.
constexpr size_t MAX_COVER_BYTES = 256 * 1024;

// Offset of bfOffBits in the BMP file header.
constexpr size_t BF_OFF_BITS = 10;

std::string md5Hex(const std::string& text) {
  MD5Builder md5;
  md5.begin();
  md5.add(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  md5.calculate();
  return md5.toString().c_str();
}

// A cover is usable when it is the answer this page asked for: 1 bpp, no larger
// than the slot, and carrying exactly the rows it declares. Bitmap checks the
// header but not the length, so without the size comparison a truncated file
// would be cached forever and the renderer would abandon it half way, leaving a
// partial image on the panel.
bool isUsableBmp(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("QBOOK", path, file)) return false;
  const ScopedCleanup closeFile{[&file]() { file.close(); }};

  const size_t size = file.size();
  if (size <= 62 || size > MAX_COVER_BYTES) return false;

  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return false;
  const int width = bitmap.getWidth();
  const int height = bitmap.getHeight();
  if (width <= 0 || height <= 0) return false;
  if (!bitmap.is1Bit() || width > SLOT_WIDTH || height > SLOT_HEIGHT) return false;

  // Take the pixel-data offset from the file instead of assuming the 62-byte
  // header this converter writes today: a longer palette would otherwise let a
  // truncated file pass by exactly its extra length.
  uint32_t dataOffset = 0;
  if (!file.seek(BF_OFF_BITS)) return false;
  if (file.read(&dataOffset, sizeof(dataOffset)) != static_cast<int>(sizeof(dataOffset))) return false;
  if (dataOffset < 62 || dataOffset > size) return false;

  const size_t needed = static_cast<size_t>(dataOffset) +
                        static_cast<size_t>(bitmap.getRowBytes()) * static_cast<size_t>(height);
  return size == needed;
}

}  // namespace

bool ensure(const std::string& indexName, const std::string& coverUrl, const std::string& fetchUrl,
            const Fetcher fetcher, std::string& outPath) {
  outPath.clear();

  // The name reaches a path, so an unusable one stops here.
  const std::string stored = storedPath(indexName);
  if (stored.empty()) {
    LOG_ERR("QBOOK", "Unusable index_name for a cover: %s", indexName.c_str());
    return false;
  }

  // 1. Beside the game. A downloaded game owns its cover, and reading it costs
  //    no network at all. A file that does not parse is not deleted — it belongs
  //    to the game — but it must not shadow the cache or the network either.
  if (Storage.exists(stored.c_str())) {
    if (isUsableBmp(stored)) {
      outPath = stored;
      return true;
    }
    LOG_ERR("QBOOK", "Unusable stored cover, looking further: %s", stored.c_str());
  }

  // 2. The screen-slot cache, keyed by the catalogue's cover URL.
  if (coverUrl.empty()) return false;
  const std::string cached = cachePath(md5Hex(coverUrl), SLOT_WIDTH, SLOT_HEIGHT);
  if (Storage.exists(cached.c_str())) {
    if (isUsableBmp(cached)) {
      outPath = cached;
      return true;
    }
    LOG_ERR("QBOOK", "Discarding an unusable cover cache file: %s", cached.c_str());
    Storage.remove(cached.c_str());
  }

  // 3. The service, which needs a URL and a transport.
  if (fetcher == nullptr || fetchUrl.empty()) return false;
  // mkdir() reports "already exists" as a failure, so the directory is only
  // created when it is absent: otherwise every visit after the first would stop
  // here and the cover would never be fetched again.
  if (!Storage.exists(CACHE_DIR) && !Storage.mkdir(CACHE_DIR, true)) {
    LOG_ERR("QBOOK", "Failed to create the cover cache directory: %s", CACHE_DIR);
    return false;
  }

  // HttpDownloader stages the body in "<dest>.part" and only renames it on a
  // complete transfer, so nothing partial can appear under the cache name.
  const FetchStatus status = fetcher(fetchUrl, cached);
  if (status != FetchStatus::Ok) {
    Storage.remove(cached.c_str());
    if (status == FetchStatus::Refused) {
      // An expected state: plenty of games have no cover. Kept above DBG so a
      // "why is there no picture" report can be answered from the log.
      LOG_INF("QBOOK", "Cover not served for %s", indexName.c_str());
    } else if (status == FetchStatus::NoSession) {
      LOG_INF("QBOOK", "No radio session for a cover: %s", indexName.c_str());
    } else if (status == FetchStatus::LowMemory) {
      LOG_ERR("QBOOK", "No room to fetch a cover for %s", indexName.c_str());
    } else {
      LOG_ERR("QBOOK", "Cover transfer failed for %s", indexName.c_str());
    }
    return false;
  }
  // The service answered, but not with something this page can draw: drop it,
  // so the next visit asks again instead of retrying a broken file forever.
  if (!isUsableBmp(cached)) {
    LOG_ERR("QBOOK", "Cover answer for %s is not a usable BMP", indexName.c_str());
    Storage.remove(cached.c_str());
    return false;
  }

  outPath = cached;
  return true;
}

}  // namespace questbook_cover