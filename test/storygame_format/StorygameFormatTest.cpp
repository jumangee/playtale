#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include "QuestBook/QuestBookCover.h"
#include "QuestBook/StorygameFormat.h"
#include "QuestBook/StorygameMeta.h"

namespace qg = questbook_game;

// The field offsets are the on-SD contract: the importer writes them and the
// runtime reads them, so a reordering has to break the build rather than the
// card. These checks are the reason the units are separate declarations at all.
TEST(StorygameFormat, CoreFieldOffsets) {
  EXPECT_EQ(offsetof(qg::HeaderCore, magic), 0u);
  EXPECT_EQ(offsetof(qg::HeaderCore, formatVersion), 4u);
  EXPECT_EQ(offsetof(qg::HeaderCore, coreBytes), 6u);
  EXPECT_EQ(offsetof(qg::HeaderCore, flags), 8u);
  EXPECT_EQ(offsetof(qg::HeaderCore, bookGuid), 12u);
  EXPECT_EQ(offsetof(qg::HeaderCore, groupCount), 52u);
  EXPECT_EQ(offsetof(qg::HeaderCore, itemCount), 56u);
  EXPECT_EQ(offsetof(qg::HeaderCore, paragraphCount), 60u);
  EXPECT_EQ(offsetof(qg::HeaderCore, functionCount), 64u);
  EXPECT_EQ(offsetof(qg::HeaderCore, startParagraph), 68u);
  EXPECT_EQ(offsetof(qg::HeaderCore, triggersParagraph), 72u);
  EXPECT_EQ(offsetof(qg::HeaderCore, groupTableOffset), 76u);
  EXPECT_EQ(offsetof(qg::HeaderCore, paramTableOffset), 80u);
  EXPECT_EQ(offsetof(qg::HeaderCore, funcTableOffset), 84u);
  EXPECT_EQ(offsetof(qg::HeaderCore, indexBytes), 88u);
  EXPECT_EQ(offsetof(qg::HeaderCore, dataBytes), 92u);
  EXPECT_EQ(sizeof(qg::HeaderCore), 120u);
  EXPECT_LE(sizeof(qg::HeaderCore), qg::CORE_BYTES);
}

TEST(StorygameFormat, RecordStrides) {
  EXPECT_EQ(sizeof(qg::GroupRecord), 16u);
  EXPECT_EQ(sizeof(qg::ParamRecord), 16u);
  EXPECT_EQ(sizeof(qg::FunctionRecord), 32u);
  EXPECT_EQ(sizeof(qg::IndexRecord), qg::INDEX_STRIDE);

  EXPECT_EQ(offsetof(qg::IndexRecord, offset), 0u);
  EXPECT_EQ(offsetof(qg::IndexRecord, length), 4u);
  EXPECT_EQ(offsetof(qg::IndexRecord, flags), 8u);
  EXPECT_EQ(offsetof(qg::FunctionRecord, nameOffset), 0u);
  EXPECT_EQ(offsetof(qg::FunctionRecord, ordinal), 6u);
  EXPECT_EQ(offsetof(qg::FunctionRecord, payloadOffset), 8u);
  EXPECT_EQ(offsetof(qg::FunctionRecord, payloadLength), 12u);

  // A section boundary and the index stride agree with the SD sector, which is
  // what makes every record read one aligned read: each table starts on a
  // boundary and each stride divides it, so no record can straddle a sector.
  EXPECT_EQ(qg::SECTION_ALIGN, 512u);
  EXPECT_EQ(qg::SECTION_ALIGN % qg::INDEX_STRIDE, 0u);
  EXPECT_EQ(qg::SECTION_ALIGN % sizeof(qg::GroupRecord), 0u);
  EXPECT_EQ(qg::SECTION_ALIGN % sizeof(qg::ParamRecord), 0u);
  EXPECT_EQ(qg::SECTION_ALIGN % sizeof(qg::FunctionRecord), 0u);
  EXPECT_EQ(qg::CORE_BYTES % qg::SECTION_ALIGN, 0u);
}

TEST(StorygameFormat, PathsFollowTheCoverLibrary) {
  EXPECT_EQ(qg::directory("skeptiki1"), "/storygames/skeptiki1");
  EXPECT_EQ(qg::path("skeptiki1", qg::HDR_FILE_NAME), "/storygames/skeptiki1/game.hdr");
  EXPECT_EQ(qg::path("skeptiki1", qg::IDX_FILE_NAME), "/storygames/skeptiki1/game.idx");
  EXPECT_EQ(qg::path("skeptiki1", qg::DAT_FILE_NAME), "/storygames/skeptiki1/game.dat");
  EXPECT_EQ(qg::path("skeptiki1", qg::META_FILE_NAME), "/storygames/skeptiki1/meta.json");
  // The page reads the stored cover before its cache, so the two writers must
  // agree on the leaf even though only this module builds the whole path.
  EXPECT_EQ(qg::path("skeptiki1", qg::COVER_FILE_NAME), questbook_cover::storedPath("skeptiki1"));
}

TEST(StorygameFormat, UnsafeNamesHaveNoPath) {
  EXPECT_TRUE(qg::directory("").empty());
  EXPECT_TRUE(qg::directory("..").empty());
  EXPECT_TRUE(qg::directory("a/../b").empty());
  EXPECT_TRUE(qg::directory("a b").empty());
  EXPECT_TRUE(qg::directory("game1/sub").empty());
  EXPECT_TRUE(qg::path("", qg::HDR_FILE_NAME).empty());
  // The catalogue caps index_name at 32 bytes, so a longer one is not a game.
  EXPECT_TRUE(qg::directory(std::string(33, 'a')).empty());
  EXPECT_FALSE(qg::directory(std::string(32, 'a')).empty());
  EXPECT_FALSE(qg::directory("Firefighter_2.1").empty());
}

// The library stores a game as its own directory, so this is the inverse the walk
// and the reader dispatch both call to recognise one.
TEST(StorygameFormat, PathsAreRecognisedAsGames) {
  EXPECT_EQ(qg::indexNameFromPath("/storygames/skeptiki1"), "skeptiki1");
  EXPECT_EQ(qg::indexNameFromPath("/storygames/Firefighter_2.1"), "Firefighter_2.1");
  EXPECT_EQ(qg::indexNameFromPath(qg::directory("skeptiki1")), "skeptiki1");
  // A file inside the game, the root itself, and anything outside it are not the
  // game's own directory.
  EXPECT_TRUE(qg::indexNameFromPath("/storygames/skeptiki1/game.hdr").empty());
  EXPECT_TRUE(qg::indexNameFromPath("/storygames").empty());
  EXPECT_TRUE(qg::indexNameFromPath("/storygames/").empty());
  EXPECT_TRUE(qg::indexNameFromPath("/Books/book.epub").empty());
  EXPECT_TRUE(qg::indexNameFromPath("").empty());
  EXPECT_TRUE(qg::indexNameFromPath("storygames/skeptiki1").empty());
  // The same name rule as directory(), so a path the writer would refuse is not
  // read back as a game either.
  EXPECT_TRUE(qg::indexNameFromPath("/storygames/..").empty());
  EXPECT_TRUE(qg::indexNameFromPath("/storygames/a b").empty());
  EXPECT_TRUE(qg::indexNameFromPath(std::string("/storygames/") + std::string(33, 'a')).empty());
}

// meta.json is what gives a library row the game's real title and author; the
// reader is hand-written because the library walk is compiled on the host, where
// ArduinoJson is not available.
TEST(StorygameMeta, ReadsTheFieldsTheLibraryShows) {
  const std::string json =
      R"({"game_id":"17","index_name":"skeptiki1","fullname":"Скептики","author_name":"И. Автор",)"
      R"("tags":"мистика,ужасы","desc":"Короткое описание"})";
  qg::Meta meta;
  ASSERT_TRUE(qg::parseMetaJson(json.data(), json.size(), meta));
  EXPECT_EQ(meta.gameId, "17");
  EXPECT_EQ(meta.indexName, "skeptiki1");
  EXPECT_EQ(meta.title, "Скептики");
  EXPECT_EQ(meta.author, "И. Автор");
  EXPECT_EQ(meta.tags, "мистика,ужасы");
  EXPECT_EQ(meta.description, "Короткое описание");
}

TEST(StorygameMeta, AcceptsFormattingUnknownFieldsAndEscapes) {
  const std::string json =
      " {\n  \"ignored\": {\"nested\": [1, 2, \"a}b\"]},\n  \"fullname\": \"A \\\"quoted\\\" name\",\n"
      "  \"extra\": 12.5e3,\n  \"desc\": \"line\\nbreak\\ttab \\u0416 \\/\"\n} ";
  qg::Meta meta;
  ASSERT_TRUE(qg::parseMetaJson(json.data(), json.size(), meta));
  EXPECT_EQ(meta.title, "A \"quoted\" name");
  EXPECT_EQ(meta.description, "line\nbreak\ttab \xD0\x96 /");
}

TEST(StorygameMeta, RejectsAnythingElse) {
  const auto rejected = [](const std::string& text) {
    qg::Meta meta;
    return !qg::parseMetaJson(text.data(), text.size(), meta);
  };
  EXPECT_TRUE(rejected(""));
  EXPECT_TRUE(rejected("[]"));
  EXPECT_TRUE(rejected("null"));
  EXPECT_TRUE(rejected(R"({"fullname": "unterminated})"));
  EXPECT_TRUE(rejected(R"({"fullname" "no colon"})"));
  EXPECT_TRUE(rejected(R"({"fullname": 42)"));
  // The library falls back to the folder name, so an empty object is not a title
  // but is still a readable file.
  qg::Meta empty;
  const std::string emptyJson = "{}";
  EXPECT_TRUE(qg::parseMetaJson(emptyJson.data(), emptyJson.size(), empty));
  EXPECT_TRUE(empty.title.empty());
}