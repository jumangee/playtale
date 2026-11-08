#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "QuestBookCover.h"

// Where a downloaded storygame lives, and the on-device format its files follow
// (docs/storygames/ARCHITECTURE.md §4.2, §4.4, §4.6-§4.8; the byte layout is
// documented in docs/file-formats.md).
//
//   /storygames/<index_name>/
//     game.hdr    header: identity, counts, start paragraph, parameter tables,
//                 function task payloads (§4.4)
//     game.idx    16-byte record per paragraph, addressed by ordinal (§4.7)
//     game.dat    paragraph payloads back to back, XML as the service serves it
//                 (§4.6, §4.8)
//     meta.json   the catalogue fields needed for offline display (§4.2)
//     cover.bmp   this game's cover, written by Download (§6.3)
//
// The header and index are declared here, in one place, because the importer
// writes them and the runtime will read them: a field cannot drift between the
// two writers without changing this file. Nothing here touches the HAL, so the
// layout is checkable without hardware.
namespace questbook_game {

// --- Paths -----------------------------------------------------------------

// Plain file names inside one game directory. game.idx/game.dat/game.hdr are
// the §4.2 names; meta.json is the one JSON file in the tree, because its
// fields come from the catalogue's own JSON API (§2.1).
inline constexpr const char* HDR_FILE_NAME = "game.hdr";
inline constexpr const char* IDX_FILE_NAME = "game.idx";
inline constexpr const char* DAT_FILE_NAME = "game.dat";
inline constexpr const char* META_FILE_NAME = "meta.json";
// Same leaf as questbook_cover::storedPath(), which reads it before the
// screen-slot cache, so a downloaded game shows its cover offline.
inline constexpr const char* COVER_FILE_NAME = "cover.bmp";

// "/storygames/<index_name>", or empty when `indexName` is not a safe path
// segment. An unsafe name cannot be a real game (§2.1: index_name is the
// download path segment), so an empty path means "no game", never "unknown".
//
// The root and the name rule are still declared in QuestBookCover.h, because the
// cover tier predates the game tree; this module is now the tree's main consumer
// and owns every leaf below it. Moving the two constants here is the next change
// that adds a third consumer (Play's shared workspace, §6.3) — until then the
// path equality is pinned by test/storygame_format, which is a weaker guarantee
// than a single owner.
inline std::string directory(const std::string& indexName) {
  if (!questbook_cover::isSafeIndexName(indexName)) return {};
  std::string path = questbook_cover::GAMES_DIR;
  path += '/';
  path += indexName;
  return path;
}

// directory() + "/" + leaf; empty when the name is unsafe.
inline std::string path(const std::string& indexName, const char* leaf) {
  std::string dir = directory(indexName);
  if (dir.empty()) return {};
  dir += '/';
  dir += leaf;
  return dir;
}

// The inverse of directory(): the game's index_name for "/storygames/<name>",
// or empty when `gameDir` is not a game directory. This is how the library index
// and the reader dispatch recognise a game without testing its files — the
// library stores the directory itself as one item (§4.9), so every path that
// reaches them for a game has exactly this shape.
inline std::string indexNameFromPath(const std::string& gameDir) {
  const std::string prefix = std::string(questbook_cover::GAMES_DIR) + '/';
  if (gameDir.size() <= prefix.size() || gameDir.compare(0, prefix.size(), prefix) != 0) return {};
  const std::string name = gameDir.substr(prefix.size());
  // A nested path ("/storygames/a/b") is not a game, and neither is an unsafe
  // name: both would address something other than the game's own directory.
  if (name.find('/') != std::string::npos) return {};
  return questbook_cover::isSafeIndexName(name) ? name : std::string();
}

// --- game.hdr --------------------------------------------------------------

// "QBG1" — the magic of the storygame container, followed by the format
// version. A future reader switches on `formatVersion`, not on the magic: the
// magic only tells it this is our file at all.
inline constexpr char HDR_MAGIC[5] = "QBG1";
inline constexpr uint16_t FORMAT_VERSION = 1;

// The `v` of the document the service serves (`<storygame v="1">`). It is the
// service's own format version and moves on its own schedule: the converter can
// publish v2 while cards still hold v1 trees, so the importer compares this and
// the container carries FORMAT_VERSION.
inline constexpr int32_t DOCUMENT_VERSION = 1;

// The core occupies the file's first sector and nothing else, so every section
// behind it starts on a sector boundary: a record read is 512-aligned by
// construction and never straddles a buffer (the rule CLX1 follows,
// lib/LibraryIndex/LibraryFormat.h:17-21). Every record stride below divides
// this boundary, so no record can cross one.
inline constexpr uint32_t CORE_BYTES = 512;

// game.idx record stride: 32 records fill a 512-byte sector exactly, so
// paragraph `n` is one seek and one read at `recordStart + 16 * n` (§4.7).
inline constexpr uint16_t INDEX_STRIDE = 16;

// Every section of game.hdr starts on this boundary, and every table stride
// divides it (16, 16 and 32 bytes), so a table record is sector-aligned by
// construction: the same rule CLX1 follows
// (lib/LibraryIndex/LibraryFormat.h:17-21), and 512 is the SD sector size.
inline constexpr uint32_t SECTION_ALIGN = 512;

// Paragraph flag bits, copied into both the payload's `p@f` attribute and the
// index record so a list view need not read the payload (§4.6, §4.7).
inline constexpr uint16_t PARA_ENDING = 1;      // no choices: an ending
inline constexpr uint16_t PARA_IMAGE = 2;       // reserved; v1 conversion emits none
inline constexpr uint16_t PARA_SCRIPTS = 4;     // the paragraph carries scripts
inline constexpr uint16_t PARA_START = 8;       // the entry paragraph
inline constexpr uint16_t PARA_TRIGGERS = 16;   // the _mitril_triggers paragraph

// The header's fixed core, written at offset 0 of game.hdr. `formatVersion`
// decides how everything after it is read; `CORE_BYTES` is the room reserved
// for it, so a later version may grow these fields within that sector.
struct HeaderCore {
  char magic[4];               // 0   "QBG1"
  uint16_t formatVersion;      // 4
  uint16_t coreBytes;          // 6   sizeof(HeaderCore)
  uint32_t flags;              // 8   0 in v1: no optional section is present
  char bookGuid[40];           // 12  the source <book guid>, NUL padded
  uint32_t groupCount;         // 52
  uint32_t itemCount;          // 56  parameters across every group
  uint32_t paragraphCount;     // 60
  uint32_t functionCount;      // 64
  uint32_t startParagraph;     // 68  the entry ordinal (§3.2)
  int32_t triggersParagraph;   // 72  -1 when the game has none (§3.4)
  uint32_t groupTableOffset;   // 76  absolute offset into game.hdr
  uint32_t paramTableOffset;   // 80
  uint32_t funcTableOffset;    // 84
  uint32_t indexBytes;         // 88  game.idx length
  uint32_t dataBytes;          // 92  game.dat length, well inside u32 (§1.5)
  uint32_t reserved[6];        // 96..119
};

// One parameter group: `type` 'k' (a flag group) or 'r' (a counter group), with
// its items stored as one contiguous ordinal range so the group-limit sum is a
// loop over [firstParam, firstParam + itemCount) (§4.4).
struct GroupRecord {
  uint32_t nameOffset;  // absolute offset into game.hdr
  uint16_t nameLength;  // bytes; the name is UTF-8, not NUL terminated
  uint16_t firstParam;  // first parameter ordinal of this group
  uint16_t itemCount;   // parameters in the group
  uint8_t type;         // 'k' or 'r'
  uint8_t flags;        // bit0: visible on the character sheet
  int16_t limit;        // -1 = no limit (the source's `false`, §4.4)
  uint16_t ordinal;     // the group's own ordinal (`g@n`)
};

// One parameter. `start` is the value a new game begins with; `limit` is the
// per-parameter cap, -1 when the source declared none.
struct ParamRecord {
  uint32_t nameOffset;  // absolute offset into game.hdr
  uint16_t nameLength;
  uint16_t group;       // owning group ordinal
  uint8_t type;         // 'k' or 'r'
  uint8_t reserved;
  int16_t limit;        // -1 = no limit
  int32_t start;
};

// One function: its name and the task list the `fun` task class runs (§4.5).
// The task list is the same XML payload syntax a paragraph carries, so the
// runtime parses it with the parser it already has. The record is 32 bytes so
// the stride divides the 512-byte sector, like the other two tables.
struct FunctionRecord {
  uint32_t nameOffset;     // absolute offset into game.hdr
  uint16_t nameLength;
  uint16_t ordinal;        // the function's own ordinal (`fn@n`)
  uint32_t payloadOffset;  // absolute offset into game.hdr
  uint32_t payloadLength;
  uint32_t reserved[4];    // 0 in v1
};

// One paragraph index record, stored at `recordStart + INDEX_STRIDE * ordinal`
// in game.idx (§4.7). `length` is what lets the runtime size its payload buffer
// before reading it: measured payloads span 1 185 B median to 72 279 B max.
struct IndexRecord {
  uint32_t offset;  // byte offset into game.dat
  uint32_t length;
  uint16_t flags;   // the paragraph's `p@f` bits, so a list need not read it
  uint16_t reserved;
  uint32_t reserved2;
};

static_assert(sizeof(HeaderCore) == 120, "HeaderCore layout is the on-SD contract");
static_assert(sizeof(GroupRecord) == 16, "GroupRecord is the table stride");
static_assert(sizeof(ParamRecord) == 16, "ParamRecord is the table stride");
static_assert(sizeof(FunctionRecord) == 32, "FunctionRecord is the table stride");
static_assert(sizeof(IndexRecord) == INDEX_STRIDE, "game.idx record stride");
static_assert(sizeof(HeaderCore) <= CORE_BYTES, "the core must fit its sector");
static_assert(CORE_BYTES % SECTION_ALIGN == 0, "the core sector is a section boundary");
// A table record must never straddle a sector, which is what lets the runtime
// read one with a single aligned read.
static_assert(SECTION_ALIGN % sizeof(GroupRecord) == 0, "group records tile a sector");
static_assert(SECTION_ALIGN % sizeof(ParamRecord) == 0, "parameter records tile a sector");
static_assert(SECTION_ALIGN % sizeof(FunctionRecord) == 0, "function records tile a sector");

// meta.json's fields: the catalogue's own values, kept for offline display
// (§4.2). The identifiers matter beyond display — `index_name` is the download
// path segment and `game_id` ties the copy to the service's entry — so they are
// stored even though nothing reads them yet.
struct Meta {
  std::string gameId;
  std::string indexName;
  std::string title;        // fullname
  std::string author;       // author_name
  std::string tags;
  std::string description;  // desc
};

}  // namespace questbook_game