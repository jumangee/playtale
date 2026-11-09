#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "StorygameEngine.h"
#include "StorygamePayload.h"

// Opens the on-SD game tree (game.hdr/game.idgame.dat) and feeds the runtime:
// reads the header tables into memory, parses the function task lists once, and
// loads paragraph payloads on demand through the Engine's ParagraphSource.
// Nothing here allocates in a hot path: table vectors are sized from the header
// once at load, and the payload buffer grows only when a larger paragraph arrives
// (docs/storygames/ARCHITECTURE.md §4.4-§4.8).
namespace questbook_game {

class StorygameGame {
 public:
  // Cap on one paragraph payload the reader is willing to hold. The measured
  // corpus maximum is 72 279 bytes §4.6, so this leaves headroom while refusing a
  // corrupt index that would ask for the whole card.
  static constexpr size_t MAX_PARAGRAPH_BYTES = 96u * 1024u;
  // Function payloads are task-list XML, far smaller than any paragraph; this
  // bounds a corrupt header field the same way.
  static constexpr size_t MAX_FUNCTION_BYTES = 64u * 1024u;

  bool load(const std::string& gameDir);
  bool loaded() const { return loaded_; }
  Engine& engine() { return engine_; }
  const Engine& engine() const { return engine_; }
  // The callback handle the Engine uses to pull paragraphs on demand. Exposed
  // so the reader binds one source without reaching into the loader's private
  // callback; `context` is `this` and stays valid for the game's lifetime.
  ParagraphSource paragraphSource() { return ParagraphSource{this, &loadParagraphFor}; }

  // Reads one name (group, parameter or function) from game.hdr; names are
  // unframed slices addressed by the records' offset+length.
  bool readName(uint32_t offset, uint16_t length, std::string& out);
  // Typed name accessors hide the on-SD record layout from the UI: callers ask
  // for the group/parameter name by ordinal and this looks up its record.
  bool groupName(uint32_t ordinal, std::string& out);
  bool paramName(uint32_t ordinal, std::string& out);

 private:
  static bool loadParagraphFor(void* context, uint32_t ordinal, Paragraph& out);
  bool loadParagraphImpl(uint32_t ordinal, Paragraph& out);
  bool readIndex(uint32_t ordinal, IndexRecord& out);
  bool readHeaderCore();
  bool readGroups(std::vector<GroupRecord>& byOrdinal);
  bool readParams();
  bool readFunctions();
  bool readFunctionPayload(uint32_t offset, uint32_t length, TaskList& out);
  bool ensurePayloadBuffer(size_t bytes);
  bool readAt(HalFile& file, uint32_t offset, void* dst, size_t bytes) const;
  void closeFiles();

  std::string gameDir_;
  HalFile headerFile_;
  HalFile indexFile_;
  HalFile dataFile_;

  HeaderCore core_{};
  std::vector<GroupRecord> groups_;
  std::vector<ParamRecord> params_;
  std::vector<TaskList> functions_;

  std::unique_ptr<char[]> payloadBuffer_;
  size_t payloadCapacity_ = 0;

  Engine engine_;
  bool loaded_ = false;
};

}  // namespace questbook_game