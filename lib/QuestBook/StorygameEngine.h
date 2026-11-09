#pragma once

#include <cstdint>
#include <vector>

#include "StorygameFormat.h"
#include "StorygameModel.h"

// The storygame runtime: condition evaluation, the ~18-class task interpreter,
// BBCode lowering and the step semantics that drive a game from one paragraph to
// the next (docs/storygames/ARCHITECTURE.md §5). It reads no files and needs no
// expat, so its rules compile on the host; paragraph loading is supplied through
// ParagraphSource by whoever owns the game tree (StorygameGame.h).
namespace questbook_game {

struct ParagraphSource {
  void* context = nullptr;
  bool (*load)(void* context, uint32_t ordinal, Paragraph& out);
};

class Engine {
 public:
  // The header the loader read, normalised so every table is addressed by its
  // own ordinal: groups[g] is group ordinal g, params[p] is parameter ordinal p,
  // functions[f] is function ordinal f.
  struct Header {
    const std::vector<GroupRecord>* groups = nullptr;
    const std::vector<ParamRecord>* params = nullptr;
    const std::vector<TaskList>* functions = nullptr;
    uint32_t startParagraph = 0;
    int32_t triggersParagraph = -1;
    uint32_t paragraphCount = 0;
  };

  // Firmware must set this to esp_random (or equivalent); a null source makes
  // rnd/rnr/rng/rrr/rrg deterministic, which host tests use by injecting their
  // own source.
  using RandomFn = uint32_t (*)();
  RandomFn random = nullptr;

  bool begin(const Header& header);
  bool startGame(const ParagraphSource& source);
  // Runs the do script of the visible choice at `visibleIndex`, then follows its
  // transition (or stays and re-renders when the choice has none, §5.2).
  bool choose(const ParagraphSource& source, size_t visibleIndex);

  // The condition-resolved view of the current paragraph, rebuilt whenever the
  // state changes. Choices appends the triggers paragraph's choices (§3.4).
  const std::vector<VisibleText>& texts() const { return texts_; }
  const std::vector<VisibleChoice>& choices() const { return choices_; }
  bool ended() const { return choices_.empty(); }

  uint32_t currentOrdinal() const { return current_; }
  bool hasCurrent() const { return haveCurrent_; }
  int32_t steps() const { return steps_; }
  int32_t lastOrdinal() const { return lastOrdinal_; }
  bool failed() const { return failed_; }

  // The character sheet: visible groups only, empty groups omitted, matching
  // getCharsheetData's filter (ARCHITECTURE.md §5.1). `stored` is the group
  // limit sum. Callers resolve names through the header records.
  struct CharsheetItem {
    uint32_t paramOrdinal = 0;
    int32_t value = 0;
  };
  struct CharsheetGroup {
    uint32_t groupOrdinal = 0;
    int32_t stored = 0;
    std::vector<CharsheetItem> items;
  };
  void charsheet(std::vector<CharsheetGroup>& out) const;
  // True when charsheet() would produce at least one visible group. Answers the
  // "is there anything to show?" question without building the groups, so a
  // caller that only needs to hide a control pays no allocation.
  bool hasCharsheet() const;

  // The loaded header tables, for name-resolution by the caller.
  const Header& header() const { return header_; }

  // Lowers one paragraph text (or choice label) from BBCode to styled runs.
  static StyledText lowerBbCode(const char* text, size_t length);
  static StyledText lowerBbCode(const std::string& text) { return lowerBbCode(text.data(), text.size()); }

 private:
  static StyledText lowerBbCodeImpl(const char* text, size_t length, int depth);

  // The visibility filter charsheet() and hasCharsheet() share: a group reaches
  // the sheet only when it is visible and holds a non-zero parameter.
  bool groupIsListed(const GroupRecord& group) const;

  // --- state ----------------------------------------------------------------
  // Loads `target` and makes it current. A missing or unreadable paragraph is a
  // dead link: it returns false and leaves the current paragraph untouched.
  bool moveTo(const ParagraphSource& source, uint32_t target);
  void runPendingTransitions(const ParagraphSource& source);
  void runRoleTasks(const Paragraph& paragraph, uint8_t role);
  void rebuildVisible();

  // --- scripts and conditions ----------------------------------------------
  const Script* findScript(const Paragraph& paragraph, int32_t slot) const;
  bool scriptResult(const Paragraph& paragraph, int32_t slot) const;
  bool evaluateScript(const Script& script) const;
  bool evaluateConditions(const std::vector<Expr>& exprs, bool orMode, bool crit) const;
  bool evaluateExpr(const Expr& expr) const;

  // --- tasks ---------------------------------------------------------------
  void runTasks(const TaskList& tasks, int depth);
  bool executeTask(const Task& task, int depth);

  int32_t value(int32_t ordinal) const;
  const ParamRecord* param(int32_t ordinal) const;
  const GroupRecord* group(int32_t ordinal) const;          // by group ordinal
  const GroupRecord* groupOf(int32_t paramOrdinal) const;
  int32_t groupSum(const GroupRecord& group) const;
  int32_t groupSumWithout(const GroupRecord& group, uint32_t excluded) const;
  int32_t randomInclusive(int32_t min, int32_t max) const;
  int32_t randomParamInGroup(const GroupRecord& group) const;  // one param ordinal, -1 when empty

  void assignParam(uint32_t ordinal, int32_t raw);
  void addParam(uint32_t ordinal, int32_t amount);
  void setFlag(uint32_t ordinal, bool on);
  void clearGroup(const GroupRecord& group);

  Header header_{};
  bool bound_ = false;

  std::vector<int32_t> vars_;
  int32_t steps_ = 0;
  int32_t lastOrdinal_ = -1;
  uint32_t current_ = 0;
  bool haveCurrent_ = false;
  bool failed_ = false;

  Paragraph currentPara_;
  Paragraph triggersPara_;
  bool haveTriggers_ = false;

  std::vector<VisibleText> texts_;
  std::vector<VisibleChoice> choices_;

  // A goto/back task records a transition and stops its task list; the caller
  // unwinds and then applies it, so a paragraph swap never invalidates the list
  // still being executed.
  bool pendingTransition_ = false;
  uint32_t pendingTarget_ = 0;
  bool pendingClearLast_ = false;

  // Task-list walk state.
  bool stop_ = false;
};

}  // namespace questbook_game