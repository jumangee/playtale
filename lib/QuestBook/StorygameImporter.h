#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "StorygameFormat.h"

struct XML_ParserStruct;  // expat's opaque handle; <expat.h> stays in the .cpp

// Turns the service's converted game document into the on-device tree
// (docs/storygames/ARCHITECTURE.md §4.3, §4.10):
//
//   GET https://quest-book.ru/online/<index_name>/playtale/
//     -> /storygames/<index_name>/{game.hdr, game.idx, game.dat, meta.json}
//
// The document is walked once, as the response arrives, so the largest game
// (2.5 MB, §1.5) is never held: paragraph payloads go straight to game.dat,
// names and function task payloads straight to game.hdr, and one 16-byte record
// per paragraph to game.idx. The heap the import keeps is the three tables sized
// from the document's own `<counts>`, allocated once and capped (see MAX_*):
// about 5.4 KB for the largest game in the measured corpus, 11 KB for the
// theoretical maximum the caps allow, plus at most one 512-byte meta field.
// Everything else is bounded by the caller's chunk size.
//
// The tree is written into the game's final directory, not a staging copy. Every
// in-session failure deletes the directory (§4.10), but a power cut mid-import
// can leave game.hdr/game.idx/game.dat behind without meta.json; that is why
// meta.json is written last and why "downloaded" means all four files
// (StorygameStore.h). The next Add replaces such a leftover. Downloading a game
// that is already present therefore replaces it.
namespace questbook_game {

class Importer {
 public:
  // Ceilings for the per-game tables. The measured corpus (§3.3) tops out at 12
  // groups, 322 parameters and 1 function; these leave about 60 % headroom while
  // keeping a lying document from asking for an unbounded allocation. Worst case
  // is MAX_TABLE_BYTES below, allocated once, after the `<counts>` element that
  // declares it (a few hundred bytes into the body).
  static constexpr uint32_t MAX_GROUPS = 64;
  static constexpr uint32_t MAX_PARAMS = 512;
  static constexpr uint32_t MAX_FUNCTIONS = 64;
  static constexpr uint32_t MAX_TABLE_BYTES =
      MAX_GROUPS * sizeof(GroupRecord) + MAX_PARAMS * sizeof(ParamRecord) + MAX_FUNCTIONS * sizeof(FunctionRecord);
  // Longest group/parameter/function name kept. Names are display text for the
  // character sheet; a longer one is cut on a UTF-8 boundary. (Not `NAME_MAX`:
  // <limits.h> defines that as 255.)
  static constexpr size_t MAX_NAME_BYTES = 512;
  // The converted document is one sequential response; the largest game in the
  // corpus is 2.46 MB (§4.3), so anything past this is not a storygame.
  static constexpr uint32_t MAX_DOCUMENT_BYTES = 8u * 1024u * 1024u;

  enum class Status : uint8_t {
    Ok,
    Rejected,    // not a storygame document, or a required part is missing
    Incomplete,  // the declared counts and the written records disagree
    FileError,   // the card refused a write
    LowMemory,   // the parser or the tables could not be allocated
  };

  Importer() = default;
  ~Importer();
  Importer(const Importer&) = delete;
  Importer& operator=(const Importer&) = delete;

  // Creates the game directory and its files. `catalogue` supplies the fields
  // meta.json needs when the document's own <meta> omits them (§4.3 leaves
  // author_name out of the captured examples). False on an unusable name or a
  // card failure; nothing is left behind in that case either.
  bool begin(const std::string& indexName, const Meta& catalogue);

  // One response chunk. False means the document cannot be imported, so the
  // caller should stop the transfer: the header, the index or the data was
  // refused, or expat found the document malformed. status() then says why.
  bool feed(const uint8_t* data, size_t length);

  // Ends the document, checks it against its declared counts, writes the header
  // core and meta.json, and closes the files. A non-Ok status deletes the tree.
  Status finish();

  // Why feed() or finish() stopped. Read it before discard(), which resets it:
  // a transfer cut short by a callback reports through this, not through the
  // transport result.
  Status status() const { return state; }

  // Abandons the import and removes everything it created. Safe at any point.
  void discard();

 private:
  // --- expat entry points ---------------------------------------------------
  static void startElementCb(void* user, const char* name, const char** atts);
  static void endElementCb(void* user, const char* name);
  static void characterDataCb(void* user, const char* text, int length);

  void startElement(const char* name, const char** atts);
  void endElement(const char* name);
  void characterData(const char* text, size_t length);

  // Header section elements.
  void beginHeaderElement(const char* name, const char** atts);
  void beginGroup(const char** atts);
  void beginParam(const char** atts);
  void beginFunction(const char** atts);
  void finishFunction();
  bool allocateTables(uint32_t groupCount, uint32_t paramCount, uint32_t functionCount);

  // One paragraph of the data section: its children are mirrored into game.dat
  // and its index record closes it.
  void beginParagraph(const char** atts);
  void finishParagraph();
  bool writeIndex(IndexRecord record);

  // meta section elements, whose content is plain text.
  void beginMetaElement(const char* name);
  void endMetaElement(const char* name);

  // Payload mirroring: re-emits the child elements of <p> and <fn> as XML, which
  // is the payload syntax the runtime parses (§4.6). Attributes are serialized
  // from expat's normalized values, so only numbers, flags and short ids are
  // expected there; prose always arrives as element content.
  bool mirrorStart(const char* name, const char** atts);
  bool mirrorEnd(const char* name);
  bool mirrorText(const char* text, size_t length);
  bool writePayload(const char* data, size_t length);
  bool writeEscaped(const char* data, size_t length, bool attribute);

  bool writeZeros(HalFile& file, uint32_t count);
  // Pads game.hdr to the next SECTION_ALIGN boundary, so each table below starts
  // on a sector.
  bool alignHeader();
  bool appendName(const char* name, uint32_t& offsetOut, uint16_t& lengthOut);
  bool buildHeader();
  bool writeMeta();

  // Releases the parser, the files and the tables without touching the tree.
  void resetState();
  bool failed() const { return state != Status::Ok; }
  bool reject(const Status reason);

  XML_ParserStruct* parser = nullptr;
  std::string dir;
  // The index_name this import was asked for. The served <meta> must agree: the
  // download route is a catch-all that answers the wrong game with something
  // else entirely or with another game's document (§2.5).
  std::string requestedIndex;
  Meta meta;
  // First failure wins; named `state` because status() is the accessor for it.
  Status state = Status::Ok;

  HalFile headerFile;
  HalFile indexFile;
  HalFile dataFile;
  // Bytes written to each file, tracked here so offsets never depend on a card
  // round-trip inside a parse callback.
  uint32_t headerBytes = 0;
  uint32_t dataBytes = 0;
  // Response bytes seen so far, so a server that never stops cannot fill the
  // card and cannot wrap the u32 offsets above.
  uint32_t documentBytes = 0;

  enum class Section : uint8_t { None, Header, Data, Meta };
  Section section = Section::None;
  bool sawRoot = false;
  bool sawHeader = false;
  bool sawCounts = false;
  bool sawStart = false;
  bool sawData = false;

  int32_t declaredGroups = -1;
  int32_t declaredItems = -1;
  int32_t declaredParagraphs = -1;
  int32_t declaredFunctions = -1;

  std::unique_ptr<GroupRecord[]> groups;
  std::unique_ptr<ParamRecord[]> params;
  std::unique_ptr<FunctionRecord[]> functions;
  uint32_t groupTotal = 0;
  uint32_t paramTotal = 0;
  uint32_t functionTotal = 0;

  bool inGroups = false;
  bool inParams = false;
  bool inFuncs = false;

  // The payload currently being mirrored, and the file it goes to.
  enum class Payload : uint8_t { None, Paragraph, Function };
  Payload payload = Payload::None;
  HalFile* payloadFile = nullptr;
  uint32_t payloadStart = 0;
  uint32_t payloadBytes = 0;
  // A start tag has been written without its closing '>': the next event decides
  // between "/>" and ">", which is how an empty element round-trips.
  bool openTag = false;

  uint32_t paragraphs = 0;
  uint16_t paragraphFlags = 0;

  std::string bookGuid;
  int32_t startParagraph = -1;
  int32_t triggersParagraph = -1;

  enum class MetaField : uint8_t { None, GameId, IndexName, Title, Author, Tags, Description };
  MetaField metaField = MetaField::None;
  std::string metaText;
};

}  // namespace questbook_game