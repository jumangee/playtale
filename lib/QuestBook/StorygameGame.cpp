#include "StorygameGame.h"

#include <Logging.h>
#include <Memory.h>
#include <esp_random.h>

#include <cstring>

#include "StorygameImporter.h"
#include "StorygameStore.h"

namespace questbook_game {

namespace {

bool readAll(HalFile& file, void* dst, size_t bytes) {
  auto* cursor = static_cast<uint8_t*>(dst);
  size_t remaining = bytes;
  while (remaining > 0) {
    const int read = file.read(cursor, remaining);
    if (read <= 0) return false;
    cursor += static_cast<size_t>(read);
    remaining -= static_cast<size_t>(read);
  }
  return true;
}

}  // namespace

bool StorygameGame::load(const std::string& gameDir) {
  gameDir_ = gameDir;
  loaded_ = false;
  payloadBuffer_.reset();
  payloadCapacity_ = 0;
  groups_.clear();
  params_.clear();
  functions_.clear();
  closeFiles();

  if (gameDir_.empty() || !isDownloadedAt(gameDir_)) {
    LOG_ERR("QBG", "not a downloaded game: %s", gameDir_.c_str());
    return false;
  }
  if (!Storage.openFileForRead("QBG", gameDir_ + '/' + HDR_FILE_NAME, headerFile_) ||
      !Storage.openFileForRead("QBG", gameDir_ + '/' + IDX_FILE_NAME, indexFile_) ||
      !Storage.openFileForRead("QBG", gameDir_ + '/' + DAT_FILE_NAME, dataFile_)) {
    LOG_ERR("QBG", "cannot open the game files under %s", gameDir_.c_str());
    closeFiles();
    return false;
  }

  if (!readHeaderCore()) return false;
  if (!readGroups(groups_)) return false;
  if (!readParams()) return false;
  if (!readFunctions()) return false;
  payloadBuffer_.reset();
  payloadCapacity_ = 0;

  Engine::Header header;
  header.groups = &groups_;
  header.params = &params_;
  header.functions = &functions_;
  header.startParagraph = core_.startParagraph;
  header.triggersParagraph = core_.triggersParagraph;
  header.paragraphCount = core_.paragraphCount;
  if (!engine_.begin(header)) return false;
  engine_.random = &esp_random;

  loaded_ = true;
  return true;
}

void StorygameGame::closeFiles() {
  if (headerFile_.isOpen()) headerFile_.close();
  if (indexFile_.isOpen()) indexFile_.close();
  if (dataFile_.isOpen()) dataFile_.close();
}

bool StorygameGame::readHeaderCore() {
  HalFile& file = headerFile_;
  if (!file.seek(0)) {
    LOG_ERR("QBG", "cannot seek game.hdr core");
    return false;
  }
  HeaderCore core{};
  if (!readAll(file, &core, sizeof(core))) {
    LOG_ERR("QBG", "game.hdr is shorter than its core");
    return false;
  }
  if (std::memcmp(core.magic, HDR_MAGIC, sizeof(core.magic)) != 0 || core.formatVersion != FORMAT_VERSION) {
    LOG_ERR("QBG", "game.hdr is not a QBG1 v1 container");
    return false;
  }
  if (core.groupCount > Importer::MAX_GROUPS || core.itemCount > Importer::MAX_PARAMS ||
      core.functionCount > Importer::MAX_FUNCTIONS) {
    LOG_ERR("QBG", "game.hdr declares tables beyond this device's caps");
    return false;
  }
  core_ = core;
  return true;
}

bool StorygameGame::readGroups(std::vector<GroupRecord>& byOrdinal) {
  byOrdinal.assign(core_.groupCount, GroupRecord{});
  if (core_.groupCount == 0) return true;
  std::vector<GroupRecord> declared(core_.groupCount);
  if (!readAt(headerFile_, core_.groupTableOffset, declared.data(), declared.size() * sizeof(GroupRecord))) {
    LOG_ERR("QBG", "cannot read the group table");
    return false;
  }
  for (const GroupRecord& record : declared) {
    if (record.ordinal >= core_.groupCount) {
      LOG_ERR("QBG", "group ordinal %u outside the %u declared", record.ordinal, core_.groupCount);
      return false;
    }
    if (static_cast<uint32_t>(record.firstParam) + record.itemCount > core_.itemCount) {
      LOG_ERR("QBG", "group %u item range runs past the parameter table", record.ordinal);
      return false;
    }
    byOrdinal[record.ordinal] = record;
  }
  return true;
}

bool StorygameGame::readParams() {
  params_.assign(core_.itemCount, ParamRecord{});
  if (core_.itemCount == 0) return true;
  if (!readAt(headerFile_, core_.paramTableOffset, params_.data(), params_.size() * sizeof(ParamRecord))) {
    LOG_ERR("QBG", "cannot read the parameter table");
    return false;
  }
  return true;
}

bool StorygameGame::readFunctions() {
  functions_.assign(core_.functionCount, TaskList{});
  if (core_.functionCount == 0) return true;
  std::vector<FunctionRecord> records(core_.functionCount);
  if (!readAt(headerFile_, core_.funcTableOffset, records.data(), records.size() * sizeof(FunctionRecord))) {
    LOG_ERR("QBG", "cannot read the function table");
    return false;
  }
  for (const FunctionRecord& record : records) {
    if (record.ordinal >= core_.functionCount) {
      LOG_ERR("QBG", "function ordinal %u outside the %u declared", record.ordinal, core_.functionCount);
      return false;
    }
    if (!readFunctionPayload(record.payloadOffset, record.payloadLength, functions_[record.ordinal])) return false;
  }
  return true;
}

bool StorygameGame::readFunctionPayload(const uint32_t offset, const uint32_t length, TaskList& out) {
  if (length > MAX_FUNCTION_BYTES) {
    LOG_ERR("QBG", "function payload too large: %u bytes", static_cast<unsigned>(length));
    return false;
  }
  if (!ensurePayloadBuffer(length + 2)) return false;
  // Function payloads are a bare sequence of <k> elements; parseTaskList wraps
  // them in a synthetic root, so it only needs the raw slice.
  if (!readAt(headerFile_, offset, payloadBuffer_.get(), length)) {
    LOG_ERR("QBG", "cannot read a function payload");
    return false;
  }
  return parseTaskList(payloadBuffer_.get(), length, out);
}

bool StorygameGame::readIndex(const uint32_t ordinal, IndexRecord& out) {
  if (ordinal >= core_.paragraphCount) return false;
  out = IndexRecord{};
  if (!readAt(indexFile_, ordinal * INDEX_STRIDE, &out, sizeof(out))) return false;
  return true;
}

bool StorygameGame::loadParagraphFor(void* context, const uint32_t ordinal, Paragraph& out) {
  return static_cast<StorygameGame*>(context)->loadParagraphImpl(ordinal, out);
}

bool StorygameGame::loadParagraphImpl(const uint32_t ordinal, Paragraph& out) {
  IndexRecord record;
  if (!readIndex(ordinal, record)) {
    LOG_ERR("QBG", "paragraph %u has no index record", static_cast<unsigned>(ordinal));
    return false;
  }
  if (record.length > MAX_PARAGRAPH_BYTES || record.offset > core_.dataBytes ||
      record.length > core_.dataBytes - record.offset) {
    LOG_ERR("QBG", "paragraph %u index is outside game.dat", static_cast<unsigned>(ordinal));
    return false;
  }
  if (!ensurePayloadBuffer(record.length)) return false;
  if (!readAt(dataFile_, record.offset, payloadBuffer_.get(), record.length)) {
    LOG_ERR("QBG", "cannot read paragraph %u payload", static_cast<unsigned>(ordinal));
    return false;
  }
  const bool parsed = parseParagraph(payloadBuffer_.get(), record.length, out);
  // The parsed paragraph owns its strings now; free the largest contiguous
  // block before the reader renders, so a 96 KiB buffer never stays pinned.
  payloadBuffer_.reset();
  payloadCapacity_ = 0;
  if (!parsed) LOG_ERR("QBG", "paragraph %u payload did not parse", static_cast<unsigned>(ordinal));
  return parsed;
}

bool StorygameGame::readName(const uint32_t offset, const uint16_t length, std::string& out) {
  out.clear();
  if (length == 0) return true;
  out.resize(length);
  if (!readAt(headerFile_, offset, out.data(), length)) {
    out.clear();
    return false;
  }
  return true;
}

bool StorygameGame::groupName(const uint32_t ordinal, std::string& out) {
  if (ordinal >= groups_.size()) {
    out.clear();
    return false;
  }
  return readName(groups_[ordinal].nameOffset, groups_[ordinal].nameLength, out);
}

bool StorygameGame::paramName(const uint32_t ordinal, std::string& out) {
  if (ordinal >= params_.size()) {
    out.clear();
    return false;
  }
  return readName(params_[ordinal].nameOffset, params_[ordinal].nameLength, out);
}

bool StorygameGame::ensurePayloadBuffer(const size_t bytes) {
  if (payloadBuffer_ && payloadCapacity_ >= bytes) return true;
  auto next = makeUniqueNoThrow<char[]>(bytes);
  if (!next) {
    LOG_ERR("QBG", "no room for a %u-byte payload", static_cast<unsigned>(bytes));
    return false;
  }
  payloadBuffer_ = std::move(next);
  payloadCapacity_ = bytes;
  return true;
}

bool StorygameGame::readAt(HalFile& file, const uint32_t offset, void* dst, const size_t bytes) const {
  if (bytes == 0) return true;
  if (!file.seek(offset)) {
    LOG_ERR("QBG", "cannot seek to %u", static_cast<unsigned>(offset));
    return false;
  }
  return readAll(file, dst, bytes);
}

}  // namespace questbook_game