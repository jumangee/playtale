#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "QuestBook/StorygameEngine.h"

namespace qg = questbook_game;

namespace {

uint32_t fixedRandom() {
  static uint32_t state = 42;
  state = state * 1103515245u + 12345u;
  return (state >> 8) & 0x7FFFFFFFu;
}

qg::Expr r(const int32_t value, const int32_t amount) {
  qg::Expr e;
  e.type = qg::ExprType::Resource;
  e.value = value;
  e.amount = amount;
  return e;
}

qg::Expr k(const int32_t value) {
  qg::Expr e;
  e.type = qg::ExprType::Flag;
  e.value = value;
  return e;
}

qg::Task inc(const int32_t value, const int32_t amount = 0) {
  qg::Task t;
  t.cls = qg::TaskClass::Inc;
  t.type = qg::ParamType::Resource;
  t.value = value;
  t.amount = amount;
  return t;
}

qg::Task dec(const int32_t value, const int32_t amount = 0) {
  qg::Task t;
  t.cls = qg::TaskClass::Dec;
  t.type = qg::ParamType::Resource;
  t.value = value;
  t.amount = amount;
  return t;
}

qg::Task set(const int32_t value, const int32_t amount) {
  qg::Task t;
  t.cls = qg::TaskClass::Set;
  t.type = qg::ParamType::Resource;
  t.value = value;
  t.amount = amount;
  return t;
}

qg::Script condScript(const int32_t slot, const std::vector<qg::Expr>& exprs) {
  qg::Script s;
  s.kind = qg::ScriptKind::Condition;
  s.slot = slot;
  s.crit = true;
  s.orMode = false;
  s.exprs = exprs;
  return s;
}

qg::Script taskScript(const int32_t slot, const std::vector<qg::Task>& tasks) {
  qg::Script s;
  s.kind = qg::ScriptKind::Tasks;
  s.slot = slot;
  s.tasks = tasks;
  return s;
}

qg::TextBlock text(const std::string& value, int32_t ifSlot = -1, int32_t notSlot = -1) {
  qg::TextBlock t;
  t.text = value;
  t.ifSlot = ifSlot;
  t.notSlot = notSlot;
  return t;
}

qg::Choice choiceCmd(const std::string& label, int32_t goTo, int32_t doSlot = -1) {
  qg::Choice c;
  c.label = label;
  c.goTo = goTo;
  c.doSlot = doSlot;
  return c;
}

struct Harness {
  std::vector<qg::Paragraph> paras;
  static bool load(void* ctx, uint32_t ordinal, qg::Paragraph& out) {
    auto* h = static_cast<Harness*>(ctx);
    if (ordinal >= h->paras.size()) return false;
    out = h->paras[ordinal];
    return true;
  }
};

struct Game {
  std::vector<qg::GroupRecord> groups;
  std::vector<qg::ParamRecord> params;
  std::vector<qg::TaskList> functions;
  qg::Engine engine;
  Harness harness;
  qg::ParagraphSource source = {&harness, &Harness::load};

  bool begin() {
    qg::Engine::Header header;
    header.groups = &groups;
    header.params = &params;
    header.functions = &functions;
    header.startParagraph = 0;
    header.triggersParagraph = -1;
    header.paragraphCount = static_cast<uint32_t>(harness.paras.size());
    if (!engine.begin(header)) return false;
    engine.random = &fixedRandom;
    return engine.startGame(source);
  }
};

qg::GroupRecord group(const uint16_t ordinal, const char type, const uint16_t first, const uint16_t count,
                      const bool visible = true) {
  qg::GroupRecord g{};
  g.ordinal = ordinal;
  g.type = type;
  g.firstParam = first;
  g.itemCount = count;
  g.limit = -1;
  g.flags = visible ? 1 : 0;
  return g;
}

qg::ParamRecord param(const uint16_t groupOrdinal, const char type, const int32_t start) {
  qg::ParamRecord p{};
  p.group = groupOrdinal;
  p.type = type;
  p.start = start;
  p.limit = -1;
  return p;
}

}  // namespace

TEST(BbCode, Lowering) {
  const auto runs = qg::Engine::lowerBbCode("=== Заголовок ===\n[b]Жирный[/b] текст -- хорошо");
  ASSERT_GE(runs.size(), 3u);
  EXPECT_TRUE(runs[0].heading);
  EXPECT_EQ(runs[0].text, "Заголовок");
  bool sawBold = false;
  bool sawDash = false;
  for (const auto& run : runs) {
    if (run.style & qg::SG_BOLD) sawBold = true;
    if (run.text.find("\xE2\x80\x94") != std::string::npos) sawDash = true;
  }
  EXPECT_TRUE(sawBold);
  EXPECT_TRUE(sawDash);

  const auto link = qg::Engine::lowerBbCode("[url=http://x]Текст[/url]");
  EXPECT_EQ(link[0].text, "Текст");

  const auto color = qg::Engine::lowerBbCode("[color=red]Просто текст[/color]");
  EXPECT_EQ(color[0].text, "Просто текст");

  const auto bullet = qg::Engine::lowerBbCode("[list][*]Первый[/*][*]Второй[/*][/list]");
  int bullets = 0;
  for (const auto& run : bullet) {
    if (run.bullet) ++bullets;
  }
  EXPECT_EQ(bullets, 2);
}

TEST(BbCode, LineBreaksSurviveTagsAndCrLf) {
  // A newline followed by a tag must still start a new line.
  const auto tagAfterBreak = qg::Engine::lowerBbCode("Первая\n[b]Вторая[/b]");
  ASSERT_EQ(tagAfterBreak.size(), 2u);
  EXPECT_EQ(tagAfterBreak[0].text, "Первая");
  EXPECT_FALSE(tagAfterBreak[0].breakBefore);
  EXPECT_EQ(tagAfterBreak[1].text, "Вторая");
  EXPECT_TRUE(tagAfterBreak[1].breakBefore);

  // CRLF is one break, not two, and leaves no carriage return in the text.
  const auto crlf = qg::Engine::lowerBbCode("Один\r\nДва\r\nТри");
  ASSERT_EQ(crlf.size(), 3u);
  EXPECT_EQ(crlf[0].text, "Один");
  EXPECT_FALSE(crlf[0].breakBefore);
  EXPECT_EQ(crlf[1].text, "Два");
  EXPECT_TRUE(crlf[1].breakBefore);
  EXPECT_EQ(crlf[2].text, "Три");
  EXPECT_TRUE(crlf[2].breakBefore);
}

TEST(Engine, MissingParagraphIsADeadLink) {
  Game game;
  game.groups = {group(0, 'r', 0, 1)};
  game.params = {param(0, 'r', 0)};

  qg::Paragraph start;
  start.ordinal = 0;
  start.flags = qg::PARA_START;
  start.texts = {text("Hello")};
  start.choices = {choiceCmd("Битый", 7), choiceCmd("Рабочий", 1)};

  qg::Paragraph end;
  end.ordinal = 1;
  end.flags = qg::PARA_ENDING;
  end.texts = {text("End")};

  game.harness.paras = {start, end};
  ASSERT_TRUE(game.begin());
  ASSERT_EQ(game.engine.choices().size(), 2u);

  // The target paragraph is outside the game, so the choice does nothing: the
  // reader stays on the paragraph and the game does not fail.
  ASSERT_TRUE(game.engine.choose(game.source, 0));
  EXPECT_FALSE(game.engine.failed());
  EXPECT_EQ(game.engine.currentOrdinal(), 0u);
  EXPECT_EQ(game.engine.steps(), 1);
  ASSERT_EQ(game.engine.choices().size(), 2u);

  // The neighbouring valid choice still works.
  ASSERT_TRUE(game.engine.choose(game.source, 1));
  EXPECT_EQ(game.engine.currentOrdinal(), 1u);
  EXPECT_TRUE(game.engine.ended());
}

TEST(Engine, StartSeedsStateAndAdvances) {
  Game game;
  game.groups = {group(0, 'r', 0, 2), group(1, 'k', 2, 1)};
  game.params = {param(0, 'r', 10), param(0, 'r', 0), param(1, 'k', 0)};

  qg::Paragraph start;
  start.ordinal = 0;
  start.flags = qg::PARA_START;
  start.texts = {text("Hello", 0)};
  start.scripts = {condScript(0, {r(0, 10)})};
  start.choices = {choiceCmd("[b]Go[/b]", 1)};

  qg::Paragraph end;
  end.ordinal = 1;
  end.flags = qg::PARA_ENDING;
  end.texts = {text("End")};

  game.harness.paras = {start, end};
  ASSERT_TRUE(game.begin());

  EXPECT_EQ(game.engine.currentOrdinal(), 0u);
  EXPECT_EQ(game.engine.steps(), 1);
  ASSERT_EQ(game.engine.texts().size(), 1u);
  EXPECT_EQ(game.engine.texts()[0].runs[0].text, "Hello");
  ASSERT_EQ(game.engine.choices().size(), 1u);

  ASSERT_TRUE(game.engine.choose(game.source, 0));
  EXPECT_EQ(game.engine.currentOrdinal(), 1u);
  EXPECT_EQ(game.engine.steps(), 2);
  EXPECT_TRUE(game.engine.ended());
}

TEST(Engine, DoScriptChangesVisibilityWithoutNavigation) {
  Game game;
  game.groups = {group(0, 'r', 0, 1)};
  game.params = {param(0, 'r', 0)};

  qg::Paragraph start;
  start.ordinal = 0;
  start.flags = qg::PARA_START;
  start.scripts = {
      condScript(0, {r(0, 5)}),
      taskScript(1, {inc(0, 5)}),
  };
  start.texts = {text("Too early", 0), text("Enough", -1, 0)};
  start.choices = {choiceCmd("Gain", -1, 1)};  // no goto: re-render after do

  game.harness.paras = {start};
  ASSERT_TRUE(game.begin());
  ASSERT_EQ(game.engine.texts().size(), 1u);
  EXPECT_EQ(game.engine.texts()[0].runs[0].text, "Enough");

  ASSERT_TRUE(game.engine.choose(game.source, 0));
  // The do script raised the counter past 5; the `if` text is now visible, the
  // `ifnot` text is hidden, and the paragraph itself did not change.
  ASSERT_EQ(game.engine.texts().size(), 1u);
  EXPECT_EQ(game.engine.texts()[0].runs[0].text, "Too early");
  EXPECT_EQ(game.engine.currentOrdinal(), 0u);
}

TEST(Engine, ExpressionCondition) {
  Game game;
  game.groups = {group(0, 'r', 0, 1), group(1, 'k', 1, 1)};
  game.params = {param(0, 'r', 3), param(1, 'k', 1)};

  qg::Paragraph start;
  start.ordinal = 0;
  start.flags = qg::PARA_START;
  qg::Script s;
  s.kind = qg::ScriptKind::Condition;
  s.slot = 0;
  s.crit = true;
  qg::Expr e;
  e.type = qg::ExprType::Text;
  e.source = "r0 >= 3 and k1 and not (r0 == 9)";
  s.exprs = {e};
  start.scripts = {s};
  start.texts = {text("Match", 0)};
  start.choices = {};

  game.harness.paras = {start};
  ASSERT_TRUE(game.begin());
  ASSERT_EQ(game.engine.texts().size(), 1u);
  EXPECT_EQ(game.engine.texts()[0].runs[0].text, "Match");
}

// hasCharsheet() answers the same question charsheet() does: a group counts only
// when it is visible and holds at least one non-zero parameter.
TEST(Engine, HasCharsheetMatchesTheVisibleGroups) {
  Game game;
  // Group 0 is a visible counter group with one set and one zero parameter;
  // group 1 is a visible flag group with a set flag; group 2 is hidden but has a
  // set parameter; group 3 is visible and entirely zero.
  game.groups = {group(0, 'r', 0, 2), group(1, 'k', 2, 1), group(2, 'r', 3, 1, false), group(3, 'k', 4, 1)};
  game.params = {param(0, 'r', 4), param(0, 'r', 0), param(1, 'k', 1), param(2, 'r', 7), param(3, 'k', 0)};

  qg::Paragraph start;
  start.ordinal = 0;
  start.flags = qg::PARA_START;
  start.texts = {text("Hello")};
  start.choices = {choiceCmd("Gain", -1, 0)};
  start.scripts = {taskScript(0, {inc(4, 1)})};
  game.harness.paras = {start};
  ASSERT_TRUE(game.begin());

  EXPECT_TRUE(game.engine.hasCharsheet());
  std::vector<qg::Engine::CharsheetGroup> groups;
  game.engine.charsheet(groups);
  // Groups 0 and 1 only: the hidden group and the empty one are omitted.
  ASSERT_EQ(groups.size(), 2u);
  EXPECT_EQ(groups[0].groupOrdinal, 0u);
  EXPECT_EQ(groups[1].groupOrdinal, 1u);

  // The do script sets the empty visible flag group's only item, so that group
  // joins the two the sheet already listed.
  game.engine.choose(game.source, 0);
  ASSERT_FALSE(game.engine.failed());
  std::vector<qg::Engine::CharsheetGroup> afterDo;
  game.engine.charsheet(afterDo);
  ASSERT_EQ(afterDo.size(), 3u);
  EXPECT_EQ(afterDo.back().groupOrdinal, 3u);
  EXPECT_TRUE(game.engine.hasCharsheet());
}

// A game whose every parameter is zero (or hidden) has no character sheet to
// show, which is what keeps the reader's sheet button off the screen.
TEST(Engine, HasCharsheetIsFalseWithoutVisibleParameters) {
  Game game;
  game.groups = {group(0, 'r', 0, 1), group(1, 'k', 1, 1, false)};
  game.params = {param(0, 'r', 0), param(1, 'k', 5)};

  qg::Paragraph start;
  start.ordinal = 0;
  start.flags = qg::PARA_START;
  start.texts = {text("Hello")};
  game.harness.paras = {start};
  ASSERT_TRUE(game.begin());

  EXPECT_FALSE(game.engine.hasCharsheet());
  std::vector<qg::Engine::CharsheetGroup> groups;
  game.engine.charsheet(groups);
  EXPECT_TRUE(groups.empty());
}
