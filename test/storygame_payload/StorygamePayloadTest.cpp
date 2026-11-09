#include <gtest/gtest.h>

#include <string>

#include "QuestBook/StorygamePayload.h"

namespace qg = questbook_game;

TEST(StorygamePayload, ParsesParagraphBasics) {
  const std::string xml = "<p n=\"0\" f=\"8\"><t>Hello</t><t>World</t><a g=\"1\">Go</a></p>";
  qg::Paragraph p;
  ASSERT_TRUE(qg::parseParagraph(xml.data(), xml.size(), p));
  EXPECT_EQ(p.ordinal, 0u);
  EXPECT_EQ(p.flags, qg::PARA_START);
  ASSERT_EQ(p.texts.size(), 2u);
  EXPECT_EQ(p.texts[0].text, "Hello");
  EXPECT_EQ(p.texts[1].text, "World");
  ASSERT_EQ(p.choices.size(), 1u);
  EXPECT_EQ(p.choices[0].goTo, 1);
}

TEST(StorygamePayload, ParsesConditionScriptAndSpoiler) {
  const std::string xml =
      R"(<p n="1" f="0"><s k="c" slot="3" or="0" crit="1" not="0"><e t="r" v="15" a="5"/></s>)"
      R"(<t i="3" sp="Открыть">Дальше</t></p>)";
  qg::Paragraph p;
  ASSERT_TRUE(qg::parseParagraph(xml.data(), xml.size(), p));
  ASSERT_EQ(p.scripts.size(), 1u);
  EXPECT_TRUE(p.scripts[0].kind == qg::ScriptKind::Condition);
  EXPECT_EQ(p.scripts[0].slot, 3);
  ASSERT_EQ(p.scripts[0].exprs.size(), 1u);
  EXPECT_TRUE(p.scripts[0].exprs[0].type == qg::ExprType::Resource);
  EXPECT_EQ(p.scripts[0].exprs[0].value, 15);
  EXPECT_EQ(p.scripts[0].exprs[0].amount, 5);
  ASSERT_EQ(p.texts.size(), 1u);
  EXPECT_EQ(p.texts[0].ifSlot, 3);
  EXPECT_EQ(p.texts[0].spoiler, "Открыть");
  EXPECT_EQ(p.texts[0].text, "Дальше");
}

TEST(StorygamePayload, KeepsNestedIfBranchInsideTheTask) {
  const std::string xml =
      R"(<p n="2" f="0"><s k="t" slot="0"><k c="if"><if><c or="0" crit="1"><e t="r" v="4" a="2"/></c></if>)"
      R"(<ifnot><none/></ifnot><then><k c="inc" t="r" v="4" a="1"/></then></k></s></p>)";
  qg::Paragraph p;
  ASSERT_TRUE(qg::parseParagraph(xml.data(), xml.size(), p));
  ASSERT_EQ(p.scripts.size(), 1u);
  ASSERT_EQ(p.scripts[0].tasks.size(), 1u);
  const qg::Task& task = p.scripts[0].tasks[0];
  EXPECT_TRUE(task.cls == qg::TaskClass::If);
  EXPECT_EQ(task.ifCond.exprs.size(), 1u);
  ASSERT_EQ(task.then.size(), 1u);
  EXPECT_TRUE(task.then[0].cls == qg::TaskClass::Inc);
}

TEST(StorygamePayload, ParsesFunctionFragmentNestedIf) {
  const std::string xml =
      R"(<k c="if"><if><c or="0" crit="1"><e t="k" v="16"/></c></if><ifnot><none/></ifnot><then><k c="set" t="r" v="8" a="0"/></then></k>)";
  qg::TaskList tasks;
  ASSERT_TRUE(qg::parseTaskList(xml.data(), xml.size(), tasks));
  ASSERT_EQ(tasks.size(), 1u);
  EXPECT_TRUE(tasks[0].cls == qg::TaskClass::If);
  ASSERT_EQ(tasks[0].then.size(), 1u);
  EXPECT_TRUE(tasks[0].then[0].cls == qg::TaskClass::Set);
}

TEST(StorygamePayload, RejectsMalformedInput) {
  const std::string xml = "<p n=\"0\" f=\"0\"><t>unclosed</p>";
  qg::Paragraph p;
  EXPECT_FALSE(qg::parseParagraph(xml.data(), xml.size(), p));
}

// Game 19568 paragraph 321 carries 91 condition scripts and 89 choices in one
// payload, which is a real shape, not a crafted one: the parser must accept it.
TEST(StorygamePayload, ParsesParagraphWithManyScriptsAndChoices) {
  std::string xml = "<p n=\"321\" f=\"4\">";
  for (int slot = 0; slot < 91; ++slot) {
    xml += "<s k=\"c\" slot=\"" + std::to_string(slot) + "\" or=\"0\" crit=\"1\" not=\"0\">";
    xml += "<e t=\"r\" v=\"7\" a=\"119\" eq=\"1\"/><e t=\"r\" v=\"8\" a=\"1\" eq=\"1\"/></s>";
  }
  for (int i = 0; i < 89; ++i) {
    const std::string label = std::to_string(i);
    xml += "<a g=\"" + std::to_string(i + 1) + "\" i=\"" + std::to_string(i) +
           "\" c=\"btnGreen\">" + label + "</a>";
  }
  xml += "</p>";

  qg::Paragraph p;
  ASSERT_TRUE(qg::parseParagraph(xml.data(), xml.size(), p));
  EXPECT_EQ(p.ordinal, 321u);
  ASSERT_EQ(p.scripts.size(), 91u);
  EXPECT_EQ(p.scripts[90].slot, 90);
  ASSERT_EQ(p.scripts[90].exprs.size(), 2u);
  ASSERT_EQ(p.choices.size(), 89u);
  EXPECT_EQ(p.choices[88].goTo, 89);
}