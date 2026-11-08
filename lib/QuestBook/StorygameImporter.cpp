#include "StorygameImporter.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <PersistableStore.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <cstdlib>
#include <cstring>

namespace questbook_game {

namespace {

// meta text is display-only and the service caps a description at 140 characters
// (§2.1), so this bounds what a changed document can make us keep.
constexpr size_t META_TEXT_MAX = 512;
constexpr int32_t NO_LIMIT = -1;

const char* attribute(const char** atts, const char* name) {
  if (!atts) return nullptr;
  for (size_t i = 0; atts[i]; i += 2) {
    if (std::strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

int32_t intAttribute(const char** atts, const char* name, const int32_t fallback) {
  const char* const value = attribute(atts, name);
  if (!value || !*value) return fallback;
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value) return fallback;
  return static_cast<int32_t>(parsed);
}

// `lim` is a number or the word "none" for "no limit", which the on-device
// record carries as -1 because the source distinguishes `false` from 0 (§4.4).
int16_t limitAttribute(const char** atts, const char* name) {
  const char* const value = attribute(atts, name);
  if (!value || !*value || std::strcmp(value, "none") == 0) return NO_LIMIT;
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value) return NO_LIMIT;
  if (parsed > INT16_MAX) return INT16_MAX;
  if (parsed < INT16_MIN) return INT16_MIN;
  return static_cast<int16_t>(parsed);
}

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

Importer::~Importer() { discard(); }

void Importer::resetState() {
  destroyXmlParser(parser);
  if (headerFile.isOpen()) headerFile.close();
  if (indexFile.isOpen()) indexFile.close();
  if (dataFile.isOpen()) dataFile.close();
  groups.reset();
  params.reset();
  functions.reset();
  groupTotal = 0;
  paramTotal = 0;
  functionTotal = 0;
  declaredGroups = -1;
  declaredItems = -1;
  declaredParagraphs = -1;
  declaredFunctions = -1;
  section = Section::None;
  sawRoot = false;
  sawHeader = false;
  sawCounts = false;
  sawStart = false;
  sawData = false;
  inGroups = false;
  inParams = false;
  inFuncs = false;
  headerBytes = 0;
  dataBytes = 0;
  documentBytes = 0;
  paragraphs = 0;
  paragraphFlags = 0;
  payload = Payload::None;
  payloadFile = nullptr;
  payloadStart = 0;
  payloadBytes = 0;
  openTag = false;
  bookGuid.clear();
  requestedIndex.clear();
  startParagraph = -1;
  triggersParagraph = -1;
  metaField = MetaField::None;
  metaText.clear();
  state = Status::Ok;
}

void Importer::discard() {
  const std::string target = dir;
  resetState();
  dir.clear();
  meta = Meta{};
  if (target.empty()) return;
  if (!Storage.exists(target.c_str())) return;
  if (!Storage.removeDir(target.c_str())) LOG_ERR("QBG", "cannot remove %s", target.c_str());
}

bool Importer::reject(const Status reason) {
  if (state == Status::Ok) state = reason;
  return false;
}

bool Importer::begin(const std::string& indexName, const Meta& catalogue) {
  discard();
  const std::string gameDir = directory(indexName);
  if (gameDir.empty()) {
    LOG_ERR("QBG", "refusing an unsafe index_name: %s", indexName.c_str());
    return false;
  }
  dir = gameDir;
  requestedIndex = indexName;
  meta = catalogue;
  meta.indexName = indexName;

  // A Download of a game that is already present replaces it (§6.3), and an
  // interrupted earlier attempt must not be mixed into this one.
  if (Storage.exists(dir.c_str()) && !Storage.removeDir(dir.c_str())) {
    LOG_ERR("QBG", "cannot clear %s", dir.c_str());
    discard();
    return false;
  }
  if (!Storage.mkdir(dir.c_str(), true)) {
    LOG_ERR("QBG", "cannot create %s", dir.c_str());
    discard();
    return false;
  }
  if (!Storage.openFileForWrite("QBG", path(indexName, HDR_FILE_NAME), headerFile) ||
      !Storage.openFileForWrite("QBG", path(indexName, IDX_FILE_NAME), indexFile) ||
      !Storage.openFileForWrite("QBG", path(indexName, DAT_FILE_NAME), dataFile)) {
    reject(Status::FileError);
    discard();
    return false;
  }
  // The core's sector is reserved now, so the header has no hole behind it and
  // finish() only overwrites its first bytes with the completed core.
  if (!writeZeros(headerFile, CORE_BYTES)) {
    reject(Status::FileError);
    discard();
    return false;
  }
  headerBytes = CORE_BYTES;

  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    reject(Status::LowMemory);
    discard();
    return false;
  }
  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, &Importer::startElementCb, &Importer::endElementCb);
  XML_SetCharacterDataHandler(parser, &Importer::characterDataCb);
  return true;
}

bool Importer::feed(const uint8_t* data, size_t length) {
  if (!parser || failed()) return false;
  if (length == 0) return true;
  // The corpus maximum is 2.46 MB (§4.3); anything past this is not a game, and
  // a server that never stops must not be able to fill the card.
  documentBytes += static_cast<uint32_t>(length);
  if (documentBytes > MAX_DOCUMENT_BYTES) {
    LOG_ERR("QBG", "document exceeds %u bytes", static_cast<unsigned>(MAX_DOCUMENT_BYTES));
    reject(Status::Rejected);
    return false;
  }
  const bool parsed =
      XML_Parse(parser, reinterpret_cast<const char*>(data), static_cast<int>(length), XML_FALSE) == XML_STATUS_OK;
  if (!parsed) {
    const XML_Error code = XML_GetErrorCode(parser);
    LOG_ERR("QBG", "document malformed at byte %ld: %s", static_cast<long>(XML_GetCurrentByteIndex(parser)),
            XML_ErrorString(code));
    // Expat runs out of memory inside the parser, which is this device running
    // out of heap, not a bad document.
    reject(code == XML_ERROR_NO_MEMORY ? Status::LowMemory : Status::Rejected);
    return false;
  }
  // A callback may have refused the document without stopping expat; the rest of
  // this chunk was parsed against that refusal, so the transfer ends here.
  return !failed();
}

Importer::Status Importer::finish() {
  if (!parser) return state == Status::Ok ? Status::Rejected : state;
  // The final chunk is what reports a document that ended early: a transfer cut
  // short lands here instead of at </storygame>.
  if (!failed() && XML_Parse(parser, "", 0, XML_TRUE) != XML_STATUS_OK) {
    LOG_ERR("QBG", "document ended early: %s", XML_ErrorString(XML_GetErrorCode(parser)));
    reject(Status::Rejected);
  }

  if (!failed() && (!sawRoot || !sawHeader || !sawCounts || !sawStart || !sawData)) {
    LOG_ERR("QBG", "document is not a complete storygame (root %d header %d counts %d start %d data %d)", sawRoot,
            sawHeader, sawCounts, sawStart, sawData);
    reject(Status::Rejected);
  }
  if (!failed() && !meta.indexName.empty() && meta.indexName != requestedIndex) {
    LOG_ERR("QBG", "served game %s where %s was asked for", meta.indexName.c_str(), requestedIndex.c_str());
    reject(Status::Rejected);
  }
  if (!failed()) {
    const bool countsMatch = groupTotal == static_cast<uint32_t>(declaredGroups) &&
                             paramTotal == static_cast<uint32_t>(declaredItems) &&
                             paragraphs == static_cast<uint32_t>(declaredParagraphs) &&
                             functionTotal == static_cast<uint32_t>(declaredFunctions);
    if (!countsMatch) {
      LOG_ERR("QBG", "declared counts g=%d i=%d p=%d f=%d, imported g=%u i=%u p=%u f=%u", declaredGroups,
              declaredItems, declaredParagraphs, declaredFunctions, static_cast<unsigned>(groupTotal),
              static_cast<unsigned>(paramTotal), static_cast<unsigned>(paragraphs),
              static_cast<unsigned>(functionTotal));
      reject(Status::Incomplete);
    }
  }
  // Both ordinals name a paragraph, so a value past the imported set would make
  // the first render read an index record that does not exist.
  if (!failed() && static_cast<uint32_t>(startParagraph) >= paragraphs) {
    LOG_ERR("QBG", "start paragraph %d outside the %u imported", startParagraph, static_cast<unsigned>(paragraphs));
    reject(Status::Rejected);
  }
  if (!failed() && triggersParagraph >= 0 && static_cast<uint32_t>(triggersParagraph) >= paragraphs) {
    LOG_ERR("QBG", "triggers paragraph %d outside the %u imported", triggersParagraph,
            static_cast<unsigned>(paragraphs));
    reject(Status::Rejected);
  }
  if (!failed() && !buildHeader()) reject(Status::FileError);

  if (!failed()) {
    // SdFat reports a deferred write or sync error at close, so the results
    // matter: a card that filled up mid-import must not read as a complete game.
    const bool indexClosed = indexFile.close();
    const bool dataClosed = dataFile.close();
    headerFile.flush();
    const bool headerClosed = headerFile.close();
    if (!indexClosed || !dataClosed || !headerClosed) {
      LOG_ERR("QBG", "card refused the written game (idx %d dat %d hdr %d)", indexClosed, dataClosed, headerClosed);
      reject(Status::FileError);
    } else if (!writeMeta()) {
      reject(Status::FileError);
    }
  }

  const Status result = state;
  if (result == Status::Ok) {
    dir.clear();
    resetState();
  } else {
    LOG_ERR("QBG", "import rejected: %d", static_cast<int>(result));
    discard();
  }
  return result;
}

// --- expat entry points ------------------------------------------------------

void Importer::startElementCb(void* user, const char* name, const char** atts) {
  static_cast<Importer*>(user)->startElement(name, atts);
}

void Importer::endElementCb(void* user, const char* name) {
  static_cast<Importer*>(user)->endElement(name);
}

void Importer::characterDataCb(void* user, const char* text, const int length) {
  static_cast<Importer*>(user)->characterData(text, length < 0 ? 0 : static_cast<size_t>(length));
}

// --- document structure ------------------------------------------------------

void Importer::startElement(const char* name, const char** atts) {
  if (failed()) return;

  // Below a <p> or <fn> element everything is payload, whatever it is called.
  if (payload != Payload::None) {
    mirrorStart(name, atts);
    return;
  }

  if (!sawRoot) {
    if (!xmlLocalNameEquals(name, "storygame")) {
      LOG_ERR("QBG", "not a storygame document (root %s)", xmlLocalName(name));
      reject(Status::Rejected);
      return;
    }
    sawRoot = true;
    if (intAttribute(atts, "v", 0) != DOCUMENT_VERSION) {
      LOG_ERR("QBG", "unsupported storygame document version");
      reject(Status::Rejected);
    }
    return;
  }

  switch (section) {
    case Section::Header:
      beginHeaderElement(name, atts);
      return;
    case Section::Data:
      if (xmlLocalNameEquals(name, "p")) beginParagraph(atts);
      return;
    case Section::Meta:
      beginMetaElement(name);
      return;
    case Section::None:
      if (xmlLocalNameEquals(name, "header")) {
        section = Section::Header;
        sawHeader = true;
      } else if (xmlLocalNameEquals(name, "data")) {
        section = Section::Data;
        sawData = true;
      } else if (xmlLocalNameEquals(name, "meta")) {
        section = Section::Meta;
      }
      return;
  }
}

void Importer::endElement(const char* name) {
  if (failed()) return;

  if (payload != Payload::None) {
    if (payload == Payload::Paragraph) {
      if (xmlLocalNameEquals(name, "p")) {
        finishParagraph();
        return;
      }
    } else if (xmlLocalNameEquals(name, "fn")) {
      finishFunction();
      return;
    }
    mirrorEnd(name);
    return;
  }

  switch (section) {
    case Section::Header:
      if (xmlLocalNameEquals(name, "groups")) {
        inGroups = false;
      } else if (xmlLocalNameEquals(name, "params")) {
        inParams = false;
      } else if (xmlLocalNameEquals(name, "funcs")) {
        inFuncs = false;
      } else if (xmlLocalNameEquals(name, "header")) {
        section = Section::None;
      }
      return;
    case Section::Data:
      if (xmlLocalNameEquals(name, "data")) section = Section::None;
      return;
    case Section::Meta:
      endMetaElement(name);
      if (xmlLocalNameEquals(name, "meta")) section = Section::None;
      return;
    case Section::None:
      return;
  }
}

void Importer::characterData(const char* text, const size_t length) {
  if (failed() || length == 0) return;
  if (payload != Payload::None) {
    mirrorText(text, length);
    return;
  }
  if (section == Section::Meta && metaField != MetaField::None && metaText.size() + length <= META_TEXT_MAX) {
    metaText.append(text, length);
  }
}

// --- header section ----------------------------------------------------------

void Importer::beginHeaderElement(const char* name, const char** atts) {
  if (xmlLocalNameEquals(name, "book")) {
    const char* const guid = attribute(atts, "guid");
    if (guid) {
      // Only the first 39 bytes are ever stored; capping here keeps document
      // text from growing a std::string (which aborts when heap runs out).
      const size_t length = std::strlen(guid);
      const size_t limit = sizeof(HeaderCore::bookGuid) - 1;
      bookGuid.assign(guid, length < limit ? length : limit);
    }
    return;
  }
  if (xmlLocalNameEquals(name, "start")) {
    startParagraph = intAttribute(atts, "p", -1);
    sawStart = startParagraph >= 0;
    return;
  }
  if (xmlLocalNameEquals(name, "triggers")) {
    triggersParagraph = intAttribute(atts, "p", -1);
    return;
  }
  if (xmlLocalNameEquals(name, "counts")) {
    if (sawCounts) {
      LOG_ERR("QBG", "second <counts> element");
      reject(Status::Rejected);
      return;
    }
    declaredGroups = intAttribute(atts, "g", -1);
    declaredItems = intAttribute(atts, "i", -1);
    declaredParagraphs = intAttribute(atts, "p", -1);
    declaredFunctions = intAttribute(atts, "f", -1);
    if (declaredGroups < 0 || declaredItems < 0 || declaredParagraphs < 0 || declaredFunctions < 0) {
      LOG_ERR("QBG", "counts are missing or negative");
      reject(Status::Rejected);
      return;
    }
    sawCounts = true;
    allocateTables(static_cast<uint32_t>(declaredGroups), static_cast<uint32_t>(declaredItems),
                   static_cast<uint32_t>(declaredFunctions));
    return;
  }
  if (xmlLocalNameEquals(name, "groups")) {
    inGroups = true;
    return;
  }
  if (xmlLocalNameEquals(name, "g")) {
    if (inGroups) beginGroup(atts);
    return;
  }
  if (xmlLocalNameEquals(name, "params")) {
    inParams = true;
    return;
  }
  if (xmlLocalNameEquals(name, "pm")) {
    if (inParams) beginParam(atts);
    return;
  }
  if (xmlLocalNameEquals(name, "funcs")) {
    inFuncs = true;
    return;
  }
  if (xmlLocalNameEquals(name, "fn") && inFuncs) {
    beginFunction(atts);
  }
}

bool Importer::allocateTables(const uint32_t groups_, const uint32_t params_, const uint32_t functions_) {
  if (groups_ > MAX_GROUPS || params_ > MAX_PARAMS || functions_ > MAX_FUNCTIONS) {
    LOG_ERR("QBG", "tables too large for this device (g=%u i=%u f=%u)", static_cast<unsigned>(groups_),
            static_cast<unsigned>(params_), static_cast<unsigned>(functions_));
    return reject(Status::Rejected);
  }
  if (groups_ > 0) {
    groups = makeUniqueNoThrow<GroupRecord[]>(groups_);
    if (!groups) {
      LOG_ERR("QBG", "no room for %u group records (%u bytes)", static_cast<unsigned>(groups_),
              static_cast<unsigned>(groups_ * sizeof(GroupRecord)));
      return reject(Status::LowMemory);
    }
  }
  if (params_ > 0) {
    params = makeUniqueNoThrow<ParamRecord[]>(params_);
    if (!params) {
      LOG_ERR("QBG", "no room for %u parameter records (%u bytes)", static_cast<unsigned>(params_),
              static_cast<unsigned>(params_ * sizeof(ParamRecord)));
      return reject(Status::LowMemory);
    }
  }
  if (functions_ > 0) {
    functions = makeUniqueNoThrow<FunctionRecord[]>(functions_);
    if (!functions) {
      LOG_ERR("QBG", "no room for %u function records (%u bytes)", static_cast<unsigned>(functions_),
              static_cast<unsigned>(functions_ * sizeof(FunctionRecord)));
      return reject(Status::LowMemory);
    }
  }
  return true;
}

void Importer::beginGroup(const char** atts) {
  // The tables are sized by <counts>, which the document puts first; a document
  // that does not is unusable rather than rewritable.
  if (!groups) {
    LOG_ERR("QBG", "group before the counts that size the table");
    reject(Status::Rejected);
    return;
  }
  if (groupTotal >= static_cast<uint32_t>(declaredGroups)) {
    LOG_ERR("QBG", "more groups than declared");
    reject(Status::Incomplete);
    return;
  }
  GroupRecord& record = groups[groupTotal];
  record = GroupRecord{};
  record.ordinal = static_cast<uint16_t>(intAttribute(atts, "n", static_cast<int32_t>(groupTotal)));
  const char* const type = attribute(atts, "t");
  record.type = (type && type[0] == 'r') ? 'r' : 'k';
  record.flags = intAttribute(atts, "vis", 0) != 0 ? 1 : 0;
  record.firstParam = static_cast<uint16_t>(intAttribute(atts, "first", 0));
  record.itemCount = static_cast<uint16_t>(intAttribute(atts, "count", 0));
  record.limit = limitAttribute(atts, "lim");
  if (!appendName(attribute(atts, "name"), record.nameOffset, record.nameLength)) return;
  ++groupTotal;
}

void Importer::beginParam(const char** atts) {
  if (!params) {
    LOG_ERR("QBG", "parameter before the counts that size the table");
    reject(Status::Rejected);
    return;
  }
  if (paramTotal >= static_cast<uint32_t>(declaredItems)) {
    LOG_ERR("QBG", "more parameters than declared");
    reject(Status::Incomplete);
    return;
  }
  ParamRecord& record = params[paramTotal];
  record = ParamRecord{};
  record.group = static_cast<uint16_t>(intAttribute(atts, "g", 0));
  const char* const type = attribute(atts, "t");
  record.type = (type && type[0] == 'r') ? 'r' : 'k';
  record.limit = limitAttribute(atts, "lim");
  record.start = intAttribute(atts, "start", 0);
  if (!appendName(attribute(atts, "name"), record.nameOffset, record.nameLength)) return;
  ++paramTotal;
}

void Importer::beginFunction(const char** atts) {
  if (!functions) {
    LOG_ERR("QBG", "function before the counts that size the table");
    reject(Status::Rejected);
    return;
  }
  if (functionTotal >= static_cast<uint32_t>(declaredFunctions)) {
    LOG_ERR("QBG", "more functions than declared");
    reject(Status::Incomplete);
    return;
  }
  FunctionRecord& record = functions[functionTotal];
  record = FunctionRecord{};
  record.ordinal = static_cast<uint16_t>(intAttribute(atts, "n", static_cast<int32_t>(functionTotal)));
  if (!appendName(attribute(atts, "name"), record.nameOffset, record.nameLength)) return;
  // The task list follows the name in the streamed region, so its offset is
  // simply where the name ended.
  record.payloadOffset = headerBytes;
  payload = Payload::Function;
  payloadFile = &headerFile;
  payloadStart = headerBytes;
  payloadBytes = 0;
  openTag = false;
}

void Importer::finishFunction() {
  functions[functionTotal].payloadLength = payloadBytes;
  ++functionTotal;
  payload = Payload::None;
  payloadFile = nullptr;
  payloadStart = 0;
  payloadBytes = 0;
  openTag = false;
}

// --- payload mirroring -------------------------------------------------------

bool Importer::mirrorStart(const char* name, const char** atts) {
  if (openTag && !writePayload(">", 1)) return false;
  openTag = false;
  if (!writePayload("<", 1) || !writePayload(name, std::strlen(name))) return false;
  for (size_t i = 0; atts && atts[i]; i += 2) {
    const char* const value = atts[i + 1] ? atts[i + 1] : "";
    if (!writePayload(" ", 1) || !writePayload(atts[i], std::strlen(atts[i])) || !writePayload("=\"", 2) ||
        !writeEscaped(value, std::strlen(value), true) || !writePayload("\"", 1)) {
      return false;
    }
  }
  openTag = true;
  return true;
}

bool Importer::mirrorEnd(const char* name) {
  if (openTag) {
    openTag = false;
    return writePayload("/>", 2);
  }
  return writePayload("</", 2) && writePayload(name, std::strlen(name)) && writePayload(">", 1);
}

bool Importer::mirrorText(const char* text, const size_t length) {
  if (openTag && !writePayload(">", 1)) return false;
  openTag = false;
  // Element content keeps its newlines, which the platform's BBCode lowering
  // turns into line breaks (§4.6), so only the markup characters are escaped.
  return writeEscaped(text, length, false);
}

bool Importer::writePayload(const char* data, const size_t length) {
  if (length == 0) return true;
  if (failed() || !payloadFile) return false;
  if (!writeAll(*payloadFile, data, length)) return reject(Status::FileError);
  if (payload == Payload::Paragraph) {
    dataBytes += static_cast<uint32_t>(length);
    payloadBytes += static_cast<uint32_t>(length);
  } else {
    headerBytes += static_cast<uint32_t>(length);
    payloadBytes += static_cast<uint32_t>(length);
  }
  return true;
}

bool Importer::writeEscaped(const char* data, const size_t length, const bool attribute) {
  size_t runStart = 0;
  for (size_t i = 0; i < length; ++i) {
    const char* replacement = nullptr;
    switch (data[i]) {
      case '&':
        replacement = "&amp;";
        break;
      case '<':
        replacement = "&lt;";
        break;
      case '>':
        replacement = "&gt;";
        break;
      case '"':
        if (attribute) replacement = "&quot;";
        break;
      default:
        break;
    }
    if (!replacement) continue;
    if (i > runStart && !writePayload(data + runStart, i - runStart)) return false;
    if (!writePayload(replacement, std::strlen(replacement))) return false;
    runStart = i + 1;
  }
  if (length > runStart) return writePayload(data + runStart, length - runStart);
  return true;
}

void Importer::beginParagraph(const char** atts) {
  const int32_t ordinal = intAttribute(atts, "n", -1);
  if (ordinal < 0 || static_cast<uint32_t>(ordinal) != paragraphs) {
    LOG_ERR("QBG", "paragraph ordinal %d out of order (expected %u)", ordinal, static_cast<unsigned>(paragraphs));
    reject(Status::Rejected);
    return;
  }
  if (paragraphs >= static_cast<uint32_t>(declaredParagraphs)) {
    LOG_ERR("QBG", "more paragraphs than declared");
    reject(Status::Incomplete);
    return;
  }
  const int32_t flags = intAttribute(atts, "f", 0);
  paragraphFlags = static_cast<uint16_t>(flags < 0 ? 0 : flags);
  payload = Payload::Paragraph;
  payloadFile = &dataFile;
  payloadStart = dataBytes;
  payloadBytes = 0;
  openTag = false;
  // The payload keeps its `<p>` element, so one index record addresses one
  // well-formed XML fragment: expat needs a single root, and `n`/`f` in the
  // payload give the runtime the same cross-check the index carries (§4.6).
  if (!mirrorStart("p", atts)) {
    payload = Payload::None;
    payloadFile = nullptr;
  }
}

void Importer::finishParagraph() {
  if (!mirrorEnd("p")) return;
  IndexRecord record{};
  record.offset = payloadStart;
  record.length = payloadBytes;
  record.flags = paragraphFlags;
  payload = Payload::None;
  payloadFile = nullptr;
  payloadStart = 0;
  payloadBytes = 0;
  openTag = false;
  if (!writeIndex(record)) return;
  ++paragraphs;
}

bool Importer::writeIndex(const IndexRecord record) {
  if (!writeAll(indexFile, reinterpret_cast<const char*>(&record), sizeof(record))) {
    return reject(Status::FileError);
  }
  return true;
}

// --- meta section ------------------------------------------------------------

void Importer::beginMetaElement(const char* name) {
  if (xmlLocalNameEquals(name, "game_id")) {
    metaField = MetaField::GameId;
  } else if (xmlLocalNameEquals(name, "index_name")) {
    metaField = MetaField::IndexName;
  } else if (xmlLocalNameEquals(name, "fullname")) {
    metaField = MetaField::Title;
  } else if (xmlLocalNameEquals(name, "author_name") || xmlLocalNameEquals(name, "author")) {
    metaField = MetaField::Author;
  } else if (xmlLocalNameEquals(name, "tags")) {
    metaField = MetaField::Tags;
  } else if (xmlLocalNameEquals(name, "desc") || xmlLocalNameEquals(name, "description")) {
    metaField = MetaField::Description;
  } else {
    metaField = MetaField::None;
  }
  metaText.clear();
}

void Importer::endMetaElement(const char* name) {
  if (xmlLocalNameEquals(name, "meta")) return;
  switch (metaField) {
    case MetaField::GameId:
      if (!metaText.empty()) meta.gameId = metaText;
      break;
    case MetaField::IndexName:
      if (!metaText.empty()) meta.indexName = metaText;
      break;
    case MetaField::Title:
      if (!metaText.empty()) meta.title = metaText;
      break;
    case MetaField::Author:
      if (!metaText.empty()) meta.author = metaText;
      break;
    case MetaField::Tags:
      if (!metaText.empty()) meta.tags = metaText;
      break;
    case MetaField::Description:
      if (!metaText.empty()) meta.description = metaText;
      break;
    case MetaField::None:
      break;
  }
  metaField = MetaField::None;
  metaText.clear();
}

// --- finalisation ------------------------------------------------------------

bool Importer::appendName(const char* name, uint32_t& offsetOut, uint16_t& lengthOut) {
  const size_t length = name ? std::strlen(name) : 0;
  size_t keep = length;
  if (keep > MAX_NAME_BYTES) {
    keep = MAX_NAME_BYTES;
    // Cut on a UTF-8 boundary: `name[keep]` is the first byte dropped, so step
    // back while it starts a continuation byte.
    while (keep > 0 && (static_cast<unsigned char>(name[keep]) & 0xC0) == 0x80) --keep;
  }
  offsetOut = headerBytes;
  lengthOut = static_cast<uint16_t>(keep);
  if (keep == 0) return true;
  if (!writeAll(headerFile, name, keep)) return reject(Status::FileError);
  headerBytes += static_cast<uint32_t>(keep);
  return true;
}

bool Importer::writeZeros(HalFile& file, const uint32_t count) {
  static const char zeros[64] = {};
  uint32_t remaining = count;
  while (remaining > 0) {
    const size_t chunk = remaining < sizeof(zeros) ? remaining : sizeof(zeros);
    if (!writeAll(file, zeros, chunk)) return false;
    remaining -= static_cast<uint32_t>(chunk);
  }
  return true;
}

bool Importer::alignHeader() {
  const uint32_t padding = (SECTION_ALIGN - headerBytes % SECTION_ALIGN) % SECTION_ALIGN;
  if (padding == 0) return true;
  if (!writeZeros(headerFile, padding)) return reject(Status::FileError);
  headerBytes += padding;
  return true;
}

bool Importer::buildHeader() {
  // The streamed region ends wherever the last name or task list did. Each table
  // then starts on the next sector boundary, and every stride divides a sector,
  // so a record read is one aligned read by construction. The core is written
  // back over the reserved first sector at the end.
  if (!alignHeader()) return false;

  const uint32_t groupTableOffset = headerBytes;
  if (groupTotal > 0) {
    const size_t bytes = sizeof(GroupRecord) * groupTotal;
    if (!writeAll(headerFile, reinterpret_cast<const char*>(groups.get()), bytes)) return reject(Status::FileError);
    headerBytes += static_cast<uint32_t>(bytes);
  }
  if (!alignHeader()) return false;
  const uint32_t paramTableOffset = headerBytes;
  if (paramTotal > 0) {
    const size_t bytes = sizeof(ParamRecord) * paramTotal;
    if (!writeAll(headerFile, reinterpret_cast<const char*>(params.get()), bytes)) return reject(Status::FileError);
    headerBytes += static_cast<uint32_t>(bytes);
  }
  if (!alignHeader()) return false;
  const uint32_t funcTableOffset = headerBytes;
  if (functionTotal > 0) {
    const size_t bytes = sizeof(FunctionRecord) * functionTotal;
    if (!writeAll(headerFile, reinterpret_cast<const char*>(functions.get()), bytes))
      return reject(Status::FileError);
    headerBytes += static_cast<uint32_t>(bytes);
  }

  HeaderCore core{};
  std::memcpy(core.magic, HDR_MAGIC, sizeof(core.magic));
  core.formatVersion = FORMAT_VERSION;
  core.coreBytes = static_cast<uint16_t>(sizeof(HeaderCore));
  core.flags = 0;
  const size_t guidBytes = bookGuid.size() < sizeof(core.bookGuid) ? bookGuid.size() : sizeof(core.bookGuid) - 1;
  std::memcpy(core.bookGuid, bookGuid.data(), guidBytes);
  core.groupCount = groupTotal;
  core.itemCount = paramTotal;
  core.paragraphCount = paragraphs;
  core.functionCount = functionTotal;
  core.startParagraph = static_cast<uint32_t>(startParagraph);
  core.triggersParagraph = triggersParagraph;
  core.groupTableOffset = groupTableOffset;
  core.paramTableOffset = paramTableOffset;
  core.funcTableOffset = funcTableOffset;
  core.indexBytes = paragraphs * INDEX_STRIDE;
  core.dataBytes = dataBytes;

  if (!headerFile.seek(0)) return reject(Status::FileError);
  if (!writeAll(headerFile, reinterpret_cast<const char*>(&core), sizeof(core))) return reject(Status::FileError);
  return true;
}

bool Importer::writeMeta() {
  const std::string metaPath = path(meta.indexName, META_FILE_NAME);
  if (metaPath.empty()) {
    LOG_ERR("QBG", "cannot build a meta path for %s", meta.indexName.c_str());
    return false;
  }
  JsonDocument doc;
  doc["game_id"] = meta.gameId.c_str();
  doc["index_name"] = meta.indexName.c_str();
  doc["fullname"] = meta.title.c_str();
  doc["author_name"] = meta.author.c_str();
  doc["tags"] = meta.tags.c_str();
  doc["desc"] = meta.description.c_str();
  return PersistableStoreBase::writeDocToFile(metaPath.c_str(), doc);
}

}  // namespace questbook_game