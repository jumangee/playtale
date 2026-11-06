#include "QuestBookListParser.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Two entries in the service's own shape, including the string-typed counters
// and the Cyrillic fields that the real responses carry.
constexpr char kPageStart[] = R"({"result":"ok","data":{"items":[)";
constexpr char kItem1[] =
    R"({"game_id":"19568","index_name":"game19568","fullname":"Портал Зла","author_id":"5702",)"
    R"("author_name":"Дмитрий Дударь","rating":"0","created":"1792599955","updated":"1793279401",)"
    R"("cover":"http:\/\/quest-book.ru\/online\/upload\/5702_876e.jpg",)"
    R"("tags":"приключения, фэнтези","desc":"Книга-игра из серии Fighting Fantasy.",)"
    R"("likes":"6","dislikes":"1","level":"0","last_played":0,"stars":3,)"
    R"("gameachis":[["","\/online\/images\/promolevel1.png"]],"awards":[]})";
constexpr char kItem2[] =
    R"({"game_id":"19327","index_name":"lam","fullname":"Без рейтинга","author_name":"Автор",)"
    R"("tags":"","desc":"","likes":"0","dislikes":"0","stars":0,"gameachis":[],"awards":[]})";

std::string page(const std::string& items, const char* isMore = "true") {
  std::string out = kPageStart;
  out += items;
  out += R"(],"is_more":)";
  out += isMore;
  out += "}}";
  return out;
}

std::string defaultPage(const char* isMore = "true") {
  std::string items = kItem1;
  items += ',';
  items += kItem2;
  return page(items, isMore);
}

void feedAll(QuestBookListParser& parser, const std::string& text, const size_t chunk = 0) {
  const size_t step = chunk == 0 ? text.size() : chunk;
  for (size_t pos = 0; pos < text.size(); pos += step) {
    const size_t len = std::min(step, text.size() - pos);
    parser.feed(text.data() + pos, len);
  }
}

// True when `text` is well-formed UTF-8 up to its last byte, i.e. the cap cut a
// whole codepoint rather than half of one. A string may legitimately END in a
// continuation byte — that is the tail of a complete character — so the check
// walks back over the continuation bytes and tests the lead byte's length.
bool endsOnCodepointBoundary(const std::string& text) {
  if (text.empty()) return true;
  size_t index = text.size();
  size_t continuation = 0;
  while (index > 0 && (static_cast<unsigned char>(text[index - 1]) & 0xC0) == 0x80) {
    --index;
    ++continuation;
  }
  if (index == 0) return false;  // continuation bytes with no lead byte
  const unsigned char lead = static_cast<unsigned char>(text[index - 1]);
  const size_t expected = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
  return continuation == expected;
}

}  // namespace

TEST(QuestBookListParser, ReadsEveryListedField) {
  QuestBookListParser parser;
  const std::string text = defaultPage();
  feedAll(parser, text);
  ASSERT_TRUE(parser.parseEnd()) << "unexpected malformed input";
  ASSERT_TRUE(parser.ok());
  ASSERT_EQ(parser.rows().size(), 2u);

  const auto& first = parser.rows()[0];
  EXPECT_EQ(first.gameId, "19568");
  EXPECT_EQ(first.indexName, "game19568");
  EXPECT_EQ(first.title, "Портал Зла");
  EXPECT_EQ(first.author, "Дмитрий Дударь");
  EXPECT_EQ(first.tags, "приключения, фэнтези");
  EXPECT_EQ(first.description, "Книга-игра из серии Fighting Fantasy.");
  EXPECT_EQ(first.stars, 3);
  EXPECT_EQ(first.likes, 6);
  EXPECT_EQ(first.dislikes, 1);
  EXPECT_TRUE(parser.hasMore());
  EXPECT_FALSE(parser.rowsTruncated());
  EXPECT_FALSE(parser.failed());
}

TEST(QuestBookListParser, KeepsRowsWithoutRatingsOrTags) {
  QuestBookListParser parser;
  feedAll(parser, defaultPage());
  ASSERT_TRUE(parser.parseEnd());
  const auto& second = parser.rows()[1];
  EXPECT_EQ(second.indexName, "lam");
  EXPECT_EQ(second.stars, 0);
  EXPECT_TRUE(second.tags.empty());
  EXPECT_TRUE(second.description.empty());
}

TEST(QuestBookListParser, ReadsEmptyPage) {
  QuestBookListParser parser;
  feedAll(parser, page(std::string(), "false"));
  ASSERT_TRUE(parser.parseEnd());
  EXPECT_TRUE(parser.rows().empty());
  EXPECT_FALSE(parser.hasMore());
  EXPECT_TRUE(parser.ok());
}

TEST(QuestBookListParser, ChunkBoundariesDoNotChangeTheResult) {
  const std::string text = defaultPage();
  QuestBookListParser whole;
  feedAll(whole, text);
  ASSERT_TRUE(whole.parseEnd());

  for (const size_t chunk : {1u, 2u, 3u, 7u, 13u, 64u, 200u}) {
    QuestBookListParser split;
    feedAll(split, text, chunk);
    ASSERT_TRUE(split.parseEnd()) << "chunk " << chunk;
    ASSERT_EQ(split.rows().size(), whole.rows().size()) << "chunk " << chunk;
    for (size_t i = 0; i < whole.rows().size(); ++i) {
      EXPECT_EQ(split.rows()[i].title, whole.rows()[i].title) << "chunk " << chunk;
      EXPECT_EQ(split.rows()[i].tags, whole.rows()[i].tags) << "chunk " << chunk;
      EXPECT_EQ(split.rows()[i].likes, whole.rows()[i].likes) << "chunk " << chunk;
    }
    EXPECT_EQ(split.hasMore(), whole.hasMore()) << "chunk " << chunk;
  }
}

TEST(QuestBookListParser, EscapedUnicodeIsDecoded) {
  QuestBookListParser parser;
  const std::string text =
      R"({"result":"ok","data":{"items":[{"fullname":"\u041f\u043e\u0440\u0442\u0430\u043b",)"
      R"("author_name":"A"}],"is_more":false}})";
  feedAll(parser, text, 5);
  ASSERT_TRUE(parser.parseEnd());
  ASSERT_EQ(parser.rows().size(), 1u);
  EXPECT_EQ(parser.rows()[0].title, "Портал");
}

TEST(QuestBookListParser, ReportsTruncatedDocument) {
  QuestBookListParser parser;
  const std::string text = defaultPage();
  feedAll(parser, text.substr(0, text.size() / 2));
  EXPECT_FALSE(parser.parseEnd());
  EXPECT_TRUE(parser.malformed());
  EXPECT_FALSE(parser.ok());
}

TEST(QuestBookListParser, ReportsMismatchedClosers) {
  QuestBookListParser parser;
  const std::string text = R"({"result":"ok","data":{"items":[]]})";  // ] closes an object
  feedAll(parser, text);
  EXPECT_FALSE(parser.parseEnd());
  EXPECT_TRUE(parser.malformed());
}

TEST(QuestBookListParser, ReportsFailedServiceResult) {
  QuestBookListParser parser;
  const std::string text = R"({"result":"wrong request","data":{}})";
  feedAll(parser, text);
  ASSERT_TRUE(parser.parseEnd());
  EXPECT_TRUE(parser.failed());
  EXPECT_FALSE(parser.ok());
  EXPECT_TRUE(parser.rows().empty());
}

TEST(QuestBookListParser, RejectsWellFormedResponseWithoutItems) {
  // A 200 with no item array is a changed contract, not an empty page: the
  // document parses, but ok() must not present it as "no games".
  QuestBookListParser parser;
  const std::string text = R"({"result":"ok","data":{}})";
  feedAll(parser, text);
  ASSERT_TRUE(parser.parseEnd());
  EXPECT_FALSE(parser.ok());
  EXPECT_FALSE(parser.failed());
  EXPECT_TRUE(parser.rows().empty());
}

TEST(QuestBookListParser, DropsRowsBeyondTheBufferCap) {
  std::string items;
  for (size_t i = 0; i < QuestBookListParser::MAX_ROWS + 3; ++i) {
    if (i) items += ',';
    items += R"({"game_id":")" + std::to_string(i) + R"(","fullname":"Игра )" + std::to_string(i) + R"("})";
  }
  QuestBookListParser parser;
  feedAll(parser, page(items), 40);
  ASSERT_TRUE(parser.parseEnd());
  EXPECT_EQ(parser.rows().size(), QuestBookListParser::MAX_ROWS);
  EXPECT_TRUE(parser.rowsTruncated());
}

TEST(QuestBookListParser, TruncatesLongFieldsOnUtf8Boundary) {
  // 200 Cyrillic characters (2 bytes each) in the title: the cap must cut a
  // whole codepoint, not half of one. Both caps are even, so the cut falls on
  // a character boundary here and the whole prefix survives.
  std::string word;
  for (int i = 0; i < 200; ++i) word += "я";
  QuestBookListParser parser;
  feedAll(parser, page(R"({"fullname":")" + word + R"(","desc":")" + word + R"("})"));
  ASSERT_TRUE(parser.parseEnd());
  ASSERT_EQ(parser.rows().size(), 1u);
  const auto& row = parser.rows()[0];
  EXPECT_LE(row.title.size(), QuestBookListParser::TITLE_MAX);
  EXPECT_LE(row.description.size(), QuestBookListParser::DESCRIPTION_MAX);
  EXPECT_TRUE(endsOnCodepointBoundary(row.title));
  EXPECT_TRUE(endsOnCodepointBoundary(row.description));
  EXPECT_LT(row.title.size(), word.size());  // the cap did cut something
}

TEST(QuestBookListParser, OddCapBacksOffToTheWholeCodepoint) {
  // A cap that falls inside a two-byte character must drop that character
  // instead of emitting half of it. "я" is 0xD1 0x8F; one leading ASCII byte
  // shifts the odd cap boundary onto the continuation byte.
  std::string word;
  for (int i = 0; i < 200; ++i) word += "я";
  std::string item = "{\"fullname\":\"a";
  item += word;
  item += "\"}";
  ASSERT_TRUE(QuestBookListParser::TITLE_MAX % 2 == 0) << "the shift relies on an even cap";
  QuestBookListParser parser;
  feedAll(parser, page(item));
  ASSERT_TRUE(parser.parseEnd());
  ASSERT_EQ(parser.rows().size(), 1u);
  const auto& row = parser.rows()[0];
  EXPECT_LE(row.title.size(), QuestBookListParser::TITLE_MAX);
  EXPECT_TRUE(endsOnCodepointBoundary(row.title));
  EXPECT_LT(row.title.size(), item.size());
}

TEST(QuestBookListParser, ResetClearsPreviousPage) {
  QuestBookListParser parser;
  feedAll(parser, defaultPage());
  ASSERT_TRUE(parser.parseEnd());
  ASSERT_EQ(parser.rows().size(), 2u);

  parser.reset();
  EXPECT_TRUE(parser.rows().empty());
  EXPECT_FALSE(parser.hasMore());
  feedAll(parser, page(std::string(), "false"));
  ASSERT_TRUE(parser.parseEnd());
  EXPECT_TRUE(parser.rows().empty());
  EXPECT_TRUE(parser.ok());
}

// The captured live response in docs/storygames is the contract fixture.
TEST(QuestBookListParser, ParsesTheCapturedLivePage) {
  std::ifstream file(QUESTBOOK_FIXTURE_PATH, std::ios::binary);
  ASSERT_TRUE(file.good()) << "fixture missing: " << QUESTBOOK_FIXTURE_PATH;
  std::stringstream buffer;
  buffer << file.rdbuf();
  const std::string text = buffer.str();
  ASSERT_GT(text.size(), 1000u);

  QuestBookListParser parser;
  feedAll(parser, text, 37);  // the network delivers arbitrary chunk sizes
  ASSERT_TRUE(parser.parseEnd());
  ASSERT_TRUE(parser.ok());
  EXPECT_EQ(parser.rows().size(), 12u);
  EXPECT_TRUE(parser.hasMore());
  EXPECT_EQ(parser.rows()[0].title, "Портал Зла");
  EXPECT_EQ(parser.rows()[0].indexName, "game19568");
  EXPECT_EQ(parser.rows()[0].author, "Дмитрий Дударь");
  EXPECT_EQ(parser.rows()[0].stars, 3);
  EXPECT_FALSE(parser.rows()[0].tags.empty());
  for (const auto& row : parser.rows()) {
    EXPECT_FALSE(row.title.empty());
    EXPECT_FALSE(row.gameId.empty());
    EXPECT_FALSE(row.indexName.empty());
    EXPECT_LE(row.description.size(), QuestBookListParser::DESCRIPTION_MAX);
  }
}
