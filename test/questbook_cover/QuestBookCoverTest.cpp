#include <gtest/gtest.h>

#include <string>

#include "QuestBook/QuestBookCover.h"

namespace qc = questbook_cover;

// The service's real values (ARCHITECTURE.md §2.5): `index_name` is not always
// `game<id>`.
TEST(QuestbookCover, AcceptsEveryObservedIndexName) {
  const char* names[] = {"game19568", "lam", "sf", "hockey_sssr", "mest_torgovca", "Firefighter", "parabeda"};
  for (const char* name : names) {
    EXPECT_TRUE(qc::isSafeIndexName(name)) << name;
  }
}

TEST(QuestbookCover, RefusesNamesThatLeaveTheirDirectory) {
  EXPECT_FALSE(qc::isSafeIndexName(""));
  EXPECT_FALSE(qc::isSafeIndexName("."));
  EXPECT_FALSE(qc::isSafeIndexName(".."));
  EXPECT_FALSE(qc::isSafeIndexName("../../etc"));
  EXPECT_FALSE(qc::isSafeIndexName("a/b"));
  EXPECT_FALSE(qc::isSafeIndexName("a\\b"));
  EXPECT_FALSE(qc::isSafeIndexName("..hidden"));
}

TEST(QuestbookCover, RefusesNamesOutsideTheCatalogueContract) {
  EXPECT_FALSE(qc::isSafeIndexName("name with space"));
  EXPECT_FALSE(qc::isSafeIndexName("name?x=1"));
  EXPECT_FALSE(qc::isSafeIndexName("name#frag"));
  EXPECT_FALSE(qc::isSafeIndexName(std::string(qc::INDEX_NAME_MAX + 1, 'a')));
  // The cap is the parser's, so a name at the cap is still a real one.
  EXPECT_TRUE(qc::isSafeIndexName(std::string(qc::INDEX_NAME_MAX, 'a')));
}

// The key carries the format version and the slot: a cached file is only
// reusable while all three still mean the same pixels.
TEST(QuestbookCover, CacheNameCarriesVersionAndSlot) {
  EXPECT_EQ(qc::cacheFileName("0123456789abcdef0123456789abcdef", 128, 192),
            "0123456789abcdef0123456789abcdef_v1_128x192.bmp");
  EXPECT_NE(qc::cacheFileName("0123456789abcdef0123456789abcdef", 128, 192),
            qc::cacheFileName("0123456789abcdef0123456789abcdef", 64, 96));
  // A re-uploaded cover is a different URL, hence a different hash and a
  // different file: that is the whole invalidation rule.
  EXPECT_NE(qc::cacheFileName("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 128, 192),
            qc::cacheFileName("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 128, 192));
  EXPECT_NE(qc::cacheFileName("hash", 128, 192).find("_v" + std::to_string(qc::CACHE_VERSION) + "_"),
            std::string::npos);
}

TEST(QuestbookCover, CachePathLivesUnderTheCrosspointRoot) {
  const std::string expected = std::string(qc::CACHE_DIR) + "/hash_v1_128x192.bmp";
  EXPECT_EQ(qc::cachePath("hash", 128, 192), expected);
  EXPECT_EQ(qc::cachePath("hash", 128, 192).rfind("/.crosspoint/", 0), 0u);
}

TEST(QuestbookCover, StoredCoverSitsInTheGameDirectory) {
  EXPECT_EQ(qc::storedPath("game19568"), "/storygames/game19568/cover.bmp");
  // An unusable name has no stored cover, so nothing is ever composed from it.
  EXPECT_TRUE(qc::storedPath("../x").empty());
  EXPECT_TRUE(qc::storedPath("").empty());
}
