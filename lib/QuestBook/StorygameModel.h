#pragma once

#include <cstdint>
#include <string>
#include <vector>

// In-memory shapes the storygame runtime works on. Nothing here touches the HAL
// or expat, so the engine logic that consumes these types compiles on the host.
//
// The on-SD container is documented in docs/file-formats.md; the payload grammar
// these structs mirror is docs/storygames/ARCHITECTURE.md §4.6.
namespace questbook_game {

// A game parameter is either a keyword flag (k) or a resource counter (r)
// (ARCHITECTURE.md §3.3).
enum class ParamType : uint8_t { Flag = 'k', Resource = 'r' };

// The condition-expression kind. Expr has a third kind `Text`: an `s` expression
// whose source string the evaluator parses, so it needs its own enum rather
// than reusing ParamType.
enum class ExprType : uint8_t { Flag = 'k', Resource = 'r', Text = 's' };

// One parsed condition expression (`<e>`). For `type == Text` the evaluator
// reads `source`; otherwise it compares the parameter at `value` against
// `amount` (or the parameter at `amountParam`).
struct Expr {
  ExprType type = ExprType::Flag;
  int32_t value = -1;        // parameter ordinal (v)
  int32_t amount = 0;        // literal amount (a)
  int32_t amountParam = -1;  // amount read from another parameter (ap)
  bool equal = false;        // == instead of >= (eq)
  std::string source;        // only for type Text: the expression text
};

// A condition list as the runtime evaluates it: the `<c>` element inside a
// nested `if`, or a whole `<s k="c">` script. `crit` is the reference's third
// checkCondition argument (true expects the expressions true, false expects them
// false); `orMode` selects OR over AND; `none` records a skipped branch.
struct CondGroup {
  bool none = true;
  bool orMode = false;
  bool crit = true;
  std::vector<Expr> exprs;
};

enum class ScriptKind : uint8_t { Condition, Tasks, Raw };

// Task classes the platform framework implements (ARCHITECTURE.md §1.2, §7.4).
enum class TaskClass : uint8_t {
  Unknown,
  Set,
  Inc,
  Dec,
  Fun,
  If,
  Del,
  Eql,
  Rnd,
  Add,
  Achi,
  Nlg,
  Rnr,
  Save,
  Sub,
  Back,
  Rrr,
  Mlp,
  Rng,
  Rrg,
  Div,
  Goto,
  Nul,
};

// One parsed task (`<k>`). `value` carries the operand: a parameter ordinal, a
// group ordinal (rnd/del/nlg/rng/rrg), a function ordinal (fun), or a paragraph
// ordinal (goto). `then` holds the tasks of a nested `if`'s action_script.
struct Task {
  TaskClass cls = TaskClass::Unknown;
  ParamType type = ParamType::Flag;
  int32_t value = -1;
  int32_t amount = 0;
  int32_t param = -1;  // second parameter operand (add/sub/mlp/div/eql/rn*)
  CondGroup ifCond;    // `if` only: action_if
  CondGroup ifnotCond; // `if` only: action_ifnot
  std::vector<Task> then;  // `if` only: action_script
};

// One parsed script (`<s>`). Role 0 is a plain local script; 1 is preload, 2 is
// unload (ARCHITECTURE.md §4.6). Raw scripts (k="x") carry no executable shape
// and are dropped by the parser rather than stored. A condition script carries
// the reference's `crit` and `not` inputs on the `<s>` element itself (§4.6).
struct Script {
  ScriptKind kind = ScriptKind::Raw;
  int32_t slot = -1;
  uint8_t role = 0;
  bool orMode = false;   // condition scripts only
  bool crit = true;      // condition scripts only
  bool negated = false;  // condition scripts only (`not` attribute)
  std::vector<Expr> exprs;  // condition scripts only
  std::vector<Task> tasks;  // task scripts only
};

using TaskList = std::vector<Task>;

// One text block (`<t>`). The slots name condition scripts within the same
// paragraph; spoiler is the collapsed heading, empty when there is none.
struct TextBlock {
  int32_t ifSlot = -1;
  int32_t notSlot = -1;
  std::string spoiler;
  std::string text;  // raw BBCode, lowered by the engine
};

// One choice (`<a>`).
struct Choice {
  int32_t goTo = -1;
  int32_t doSlot = -1;
  int32_t ifSlot = -1;
  int32_t notSlot = -1;
  std::string label;  // raw BBCode, lowered by the engine
};

// One parsed paragraph payload (`<p>`).
struct Paragraph {
  uint32_t ordinal = 0;
  uint16_t flags = 0;
  std::vector<Script> scripts;
  std::vector<TextBlock> texts;
  std::vector<Choice> choices;
};

// --- Styled text -------------------------------------------------------------

// Style bits carried by SgRun::style. The activity maps these to
// EpdFontFamily::Style when it draws a run.
enum SgStyle : uint8_t {
  SG_REGULAR = 0,
  SG_BOLD = 1,
  SG_ITALIC = 2,
  SG_UNDERLINE = 4,
  SG_SUP = 8,
};

// One inline BBCode-lowered run. Runs flow left to right on a line; the activity
// owns the wrapping. `heading` marks a whole line (centred, bold); `bullet`
// prefixes a list item; `breakBefore` starts a new line before the run.
struct SgRun {
  std::string text;
  uint8_t style = SG_REGULAR;
  bool heading = false;
  bool bullet = false;
  bool breakBefore = false;
};

using StyledText = std::vector<SgRun>;

// One visible text block, already lowered and condition-resolved, ready for the
// activity to flow into lines.
struct VisibleText {
  StyledText runs;
  std::string spoiler;  // heading, empty when the block is not a spoiler
};

// One visible choice, already lowered and condition-resolved. `source` is the
// paragraph the choice came from (the current paragraph or the appended
// triggers paragraph), so a do slot resolves against the right script table.
struct VisibleChoice {
  const Paragraph* source = nullptr;
  StyledText runs;
  int32_t goTo = -1;
  int32_t doSlot = -1;
};

}  // namespace questbook_game