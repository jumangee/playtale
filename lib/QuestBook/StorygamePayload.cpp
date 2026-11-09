#include "StorygamePayload.h"

#include <XmlParserUtils.h>
#include <expat.h>
#include <Logging.h>

#include <cstdlib>
#include <cstring>

namespace questbook_game {

namespace {

// Bounds for one paragraph's parsed shape. These are far above any measured
// game (§4.6: max 33 tasks in one list, 156 choices and 123 text blocks per
// paragraph) but make a crafted payload fail cleanly instead of letting
// emplace_back/std::string growth hit the throwing allocator under
// -fno-exceptions. Scripts are counted per paragraph, not per slot count, and a
// real game reaches 91 of them (game 19568 paragraph 321), so the script bound
// is set well clear of that; the 96 KiB payload cap already bounds the rest.
constexpr size_t MAX_SCRIPTS = 512;
constexpr size_t MAX_TEXTS = 256;
constexpr size_t MAX_CHOICES = 256;
constexpr size_t MAX_EXPRS = 64;         // expressions per condition group
constexpr size_t MAX_FLAT_TASKS = 512;   // tasks per task list (incl. functions)
constexpr size_t MAX_TEXT_BYTES = 128u * 1024u;  // one captured text/label/source

const char* attribute(const char** atts, const char* name) {
  if (!atts) return nullptr;
  for (size_t i = 0; atts[i]; i += 2) {
    if (std::strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

int32_t intAttribute(const char** atts, const char* name, int32_t fallback) {
  const char* value = attribute(atts, name);
  if (!value || !*value) return fallback;
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value) return fallback;
  return static_cast<int32_t>(parsed);
}

bool boolAttribute(const char** atts, const char* name, bool fallback) {
  const char* value = attribute(atts, name);
  if (!value) return fallback;
  return std::strcmp(value, "0") != 0 && *value != '\0';
}

TaskClass taskClass(const char* cls) {
  if (!cls) return TaskClass::Unknown;
  if (std::strcmp(cls, "set") == 0) return TaskClass::Set;
  if (std::strcmp(cls, "inc") == 0) return TaskClass::Inc;
  if (std::strcmp(cls, "dec") == 0) return TaskClass::Dec;
  if (std::strcmp(cls, "fun") == 0) return TaskClass::Fun;
  if (std::strcmp(cls, "if") == 0) return TaskClass::If;
  if (std::strcmp(cls, "del") == 0) return TaskClass::Del;
  if (std::strcmp(cls, "eql") == 0) return TaskClass::Eql;
  if (std::strcmp(cls, "rnd") == 0) return TaskClass::Rnd;
  if (std::strcmp(cls, "add") == 0) return TaskClass::Add;
  if (std::strcmp(cls, "achi") == 0) return TaskClass::Achi;
  if (std::strcmp(cls, "nlg") == 0) return TaskClass::Nlg;
  if (std::strcmp(cls, "rnr") == 0) return TaskClass::Rnr;
  if (std::strcmp(cls, "save") == 0) return TaskClass::Save;
  if (std::strcmp(cls, "sub") == 0) return TaskClass::Sub;
  if (std::strcmp(cls, "back") == 0) return TaskClass::Back;
  if (std::strcmp(cls, "rrr") == 0) return TaskClass::Rrr;
  if (std::strcmp(cls, "mlp") == 0) return TaskClass::Mlp;
  if (std::strcmp(cls, "rng") == 0) return TaskClass::Rng;
  if (std::strcmp(cls, "rrg") == 0) return TaskClass::Rrg;
  if (std::strcmp(cls, "div") == 0) return TaskClass::Div;
  if (std::strcmp(cls, "goto") == 0) return TaskClass::Goto;
  if (std::strcmp(cls, "nul") == 0) return TaskClass::Nul;
  return TaskClass::Unknown;
}

struct Frame {
  enum class Kind : uint8_t { Root, Paragraph, Script, Cond, Task, IfBlock, IfnotBlock, ThenBlock, Text, Choice, ExprS };
  Kind kind = Kind::Root;
  Script* script = nullptr;
  CondGroup* cond = nullptr;
  Task* task = nullptr;
  TextBlock* text = nullptr;
  Choice* choice = nullptr;
  Expr* expr = nullptr;
};

class PayloadParser {
 public:
  PayloadParser(const bool taskListMode, Paragraph* paragraph, TaskList* rootTasks)
      : taskListMode(taskListMode), paragraph(paragraph), rootTasks(rootTasks) {
    frames.push_back(Frame{});
  }

  void startElement(const char* name, const char** atts) {
    const char* local = xmlLocalName(name);

    if (skipDepth > 0) {  // inside an opaque subtree (raw scripts, unknown elements)
      ++skipDepth;
      frames.push_back(Frame{});
      return;
    }

    if (std::strcmp(local, "p") == 0) {
      if (frames.size() != 1 || taskListMode || !paragraph) return fail();
      paragraph->ordinal = static_cast<uint32_t>(intAttribute(atts, "n", 0));
      const int32_t flags = intAttribute(atts, "f", 0);
      paragraph->flags = flags < 0 ? 0 : static_cast<uint16_t>(flags);
      frames.push_back(Frame{Frame::Kind::Paragraph});
      return;
    }
    if (std::strcmp(local, "s") == 0) {
      if (!paragraph || frames.back().kind != Frame::Kind::Paragraph) return fail();
      if (paragraph->scripts.size() >= MAX_SCRIPTS) return reject("too many scripts");
      paragraph->scripts.emplace_back();
      Script& script = paragraph->scripts.back();
      const char* kind = attribute(atts, "k");
      if (kind && kind[0] == 'c') {
        script.kind = ScriptKind::Condition;
        script.orMode = boolAttribute(atts, "or", false);
        script.crit = boolAttribute(atts, "crit", true);
        script.negated = boolAttribute(atts, "not", false);
      } else if (kind && kind[0] == 't') {
        script.kind = ScriptKind::Tasks;
        const char* role = attribute(atts, "r");
        script.role = role ? (role[0] == 'p' ? 1 : (role[0] == 'u' ? 2 : 0)) : 0;
      } else {
        // k="x": an unrecognised script. Its subtree is opaque.
        script.kind = ScriptKind::Raw;
        skipDepth = 1;
        frames.push_back(Frame{});
        return;
      }
      script.slot = intAttribute(atts, "slot", -1);
      frames.push_back(Frame{Frame::Kind::Script, &script, nullptr, nullptr, nullptr, nullptr, nullptr});
      return;
    }
    if (std::strcmp(local, "e") == 0) {
      Expr* expr = pushExpr();
      if (!expr) return fail();
      const char* type = attribute(atts, "t");
      expr->type = !type || type[0] == 'k'   ? ExprType::Flag
                   : type[0] == 'r'          ? ExprType::Resource
                                             : ExprType::Text;
      expr->value = intAttribute(atts, "v", -1);
      expr->amount = intAttribute(atts, "a", 0);
      expr->amountParam = intAttribute(atts, "ap", -1);
      expr->equal = attribute(atts, "eq") != nullptr;
      if (expr->type == ExprType::Text) {  // source text captured until </e>
        textSink = &expr->source;
        frames.push_back(Frame{Frame::Kind::ExprS, nullptr, nullptr, nullptr, nullptr, nullptr, expr});
      }
      return;
    }
    if (std::strcmp(local, "k") == 0) {
      if (!pushTask(atts)) return fail();
      return;
    }
    if (std::strcmp(local, "c") == 0) {
      if (!startCondBlock(atts)) return fail();
      return;
    }
    if (std::strcmp(local, "if") == 0 || std::strcmp(local, "ifnot") == 0) {
      if (!startIfBlock(local[0] == 'i')) return fail();
      return;
    }
    if (std::strcmp(local, "then") == 0) {
      if (frames.size() < 2 || frames.back().kind != Frame::Kind::Task) return fail();
      Task* task = frames.back().task;
      if (!task || task->cls != TaskClass::If) return fail();
      frames.push_back(Frame{Frame::Kind::ThenBlock, nullptr, nullptr, task, nullptr, nullptr, nullptr});
      return;
    }
    if (std::strcmp(local, "none") == 0) {
      // A self-closing marker inside <if>/<ifnot>; never opens a frame.
      return;
    }
    if (std::strcmp(local, "t") == 0) {
      if (!paragraph || frames.back().kind != Frame::Kind::Paragraph) return fail();
      if (paragraph->texts.size() >= MAX_TEXTS) return reject("too many text blocks");
      paragraph->texts.emplace_back();
      TextBlock& block = paragraph->texts.back();
      block.ifSlot = intAttribute(atts, "i", -1);
      block.notSlot = intAttribute(atts, "n", -1);
      const char* spoiler = attribute(atts, "sp");
      if (spoiler) block.spoiler = spoiler;
      textSink = &block.text;
      frames.push_back(Frame{Frame::Kind::Text, nullptr, nullptr, nullptr, &block, nullptr, nullptr});
      return;
    }
    if (std::strcmp(local, "a") == 0) {
      if (!paragraph || frames.back().kind != Frame::Kind::Paragraph) return fail();
      if (paragraph->choices.size() >= MAX_CHOICES) return reject("too many choices");
      paragraph->choices.emplace_back();
      Choice& choice = paragraph->choices.back();
      choice.goTo = intAttribute(atts, "g", -1);
      if (choice.goTo < 0) choice.goTo = intAttribute(atts, "go", -1);
      choice.doSlot = intAttribute(atts, "d", -1);
      choice.ifSlot = intAttribute(atts, "i", -1);
      choice.notSlot = intAttribute(atts, "n", -1);
      textSink = &choice.label;
      frames.push_back(Frame{Frame::Kind::Choice, nullptr, nullptr, nullptr, nullptr, &choice, nullptr});
      return;
    }
    if (std::strcmp(local, "r") == 0 && taskListMode && frames.size() == 1) {
      // The synthetic root wrapped around a function payload.
      frames.push_back(Frame{Frame::Kind::Root});
      return;
    }

    // Anything else is opaque payload (e.g. <m>): skip its subtree.
    ++skipDepth;
    frames.push_back(Frame{});
  }

  void endElement(const char* name) {
    const char* local = xmlLocalName(name);
    if (frames.empty()) return fail();
    if (frames.size() == 1 && frames.back().kind == Frame::Kind::Root) return;

    if (skipDepth > 0) {
      --skipDepth;
      frames.pop_back();
      return;
    }

    // Elements that never opened a frame in startElement.
    if (std::strcmp(local, "none") == 0) return;
    if (std::strcmp(local, "e") == 0 && frames.back().kind != Frame::Kind::ExprS) return;

    // Everything else closes the frame its startElement pushed.
    const bool closedText = frames.back().kind == Frame::Kind::Text || frames.back().kind == Frame::Kind::Choice ||
                            frames.back().kind == Frame::Kind::ExprS;
    frames.pop_back();
    if (closedText) textSink = nullptr;
  }

  void characterData(const char* text, size_t length) {
    if (!textSink || length == 0) return;
    if (textSink->size() + length > MAX_TEXT_BYTES) {
      reject("text block too long");
      return;
    }
    textSink->append(text, length);
  }

  bool ok() const { return !failed_; }

 private:
  // The condition list new `<e>` elements append to: an open `<c>` block, or a
  // condition script's own list.
  std::vector<Expr>* currentExprTarget() {
    if (frames.empty()) return nullptr;
    const Frame& top = frames.back();
    if (top.kind == Frame::Kind::Cond && top.cond) return &top.cond->exprs;
    if (top.kind == Frame::Kind::Script && top.script && top.script->kind == ScriptKind::Condition) {
      return &top.script->exprs;
    }
    return nullptr;
  }

  Expr* pushExpr() {
    if (std::vector<Expr>* target = currentExprTarget()) {
      if (target->size() >= MAX_EXPRS) {
        reject("too many expressions in one condition");
        return nullptr;
      }
      target->emplace_back();
      return &target->back();
    }
    return nullptr;
  }

  TaskList* currentTaskList() {
    if (frames.empty()) return nullptr;
    const Frame& top = frames.back();
    // A nested `<then>` or a task script comes before the flat function root, so
    // a c="if" task's own branch stays inside it (function payloads also carry
    // those branches).
    if (top.kind == Frame::Kind::Script && top.script) return &top.script->tasks;
    if (top.kind == Frame::Kind::ThenBlock && top.task) return &top.task->then;
    if (taskListMode) return rootTasks;  // top-level function payload
    return nullptr;
  }

  bool pushTask(const char** atts) {
    TaskList* target = currentTaskList();
    if (!target) return false;
    if (target->size() >= MAX_FLAT_TASKS) {
      reject("too many tasks in one list");
      return false;
    }
    target->emplace_back();
    Task& task = target->back();
    task.cls = taskClass(attribute(atts, "c"));
    const char* type = attribute(atts, "t");
    task.type = (type && type[0] == 'k') ? ParamType::Flag : ParamType::Resource;
    task.value = intAttribute(atts, "v", -1);
    task.amount = intAttribute(atts, "a", 0);
    task.param = intAttribute(atts, "p", -1);
    frames.push_back(Frame{Frame::Kind::Task, nullptr, nullptr, &task, nullptr, nullptr, nullptr});
    return true;
  }

  bool startCondBlock(const char** atts) {
    const size_t size = frames.size();
    if (size < 2) return false;
    const Frame& top = frames.back();
    const Frame& parent = frames[size - 2];
    if (!(top.kind == Frame::Kind::IfBlock || top.kind == Frame::Kind::IfnotBlock)) return false;
    if (parent.kind != Frame::Kind::Task || !parent.task || parent.task->cls != TaskClass::If) return false;
    CondGroup* group = top.kind == Frame::Kind::IfBlock ? &parent.task->ifCond : &parent.task->ifnotCond;
    group->none = false;
    group->orMode = boolAttribute(atts, "or", false);
    group->crit = boolAttribute(atts, "crit", true);
    frames.push_back(Frame{Frame::Kind::Cond, nullptr, group, parent.task, nullptr, nullptr, nullptr});
    return true;
  }

  bool startIfBlock(const bool ifSide) {
    if (frames.empty()) return false;
    const Frame& top = frames.back();
    if (top.kind != Frame::Kind::Task || !top.task || top.task->cls != TaskClass::If) return false;
    frames.push_back(Frame{ifSide ? Frame::Kind::IfBlock : Frame::Kind::IfnotBlock, nullptr, nullptr, top.task, nullptr,
                           nullptr, nullptr});
    return true;
  }

  void fail() { failed_ = true; }

  // Limits are rejected loudly: a silent refusal surfaces only as "cannot read
  // paragraph N" at the caller, which hides which bound was hit.
  void reject(const char* why) {
    if (!failed_) LOG_ERR("QBG", "payload rejected: %s", why);
    failed_ = true;
  }

  bool taskListMode;
  Paragraph* paragraph;
  TaskList* rootTasks;
  std::vector<Frame> frames;
  std::string* textSink = nullptr;
  unsigned skipDepth = 0;
  bool failed_ = false;
};

struct Callbacks {
  static void startCb(void* user, const char* name, const char** atts) {
    static_cast<PayloadParser*>(user)->startElement(name, atts);
  }
  static void endCb(void* user, const char* name) { static_cast<PayloadParser*>(user)->endElement(name); }
  static void textCb(void* user, const char* text, int length) {
    static_cast<PayloadParser*>(user)->characterData(text, length < 0 ? 0 : static_cast<size_t>(length));
  }
};

bool runParser(const char* xml, size_t length, PayloadParser& parser) {
  XML_Parser ex = XML_ParserCreate(nullptr);
  if (!ex) return false;
  XML_SetUserData(ex, &parser);
  XML_SetElementHandler(ex, &Callbacks::startCb, &Callbacks::endCb);
  XML_SetCharacterDataHandler(ex, &Callbacks::textCb);
  const bool parsed = XML_Parse(ex, xml, static_cast<int>(length), XML_TRUE) == XML_STATUS_OK;
  const XML_Error error = XML_GetErrorCode(ex);
  destroyXmlParser(ex);
  if (!parsed) {
    LOG_ERR("QBG", "payload malformed: %s", XML_ErrorString(error));
    return false;
  }
  return parser.ok();
}

}  // namespace

bool parseParagraph(const char* xml, size_t length, Paragraph& out) {
  out = Paragraph{};
  PayloadParser parser(/*taskListMode=*/false, &out, nullptr);
  return runParser(xml, length, parser);
}

bool parseTaskList(const char* xml, size_t length, TaskList& out) {
  out.clear();
  // A function payload is a bare sequence of <k> elements; give it the single
  // root expat requires. Function payloads are small, so the extra copy is
  // bounded by the caller's read cap.
  std::string wrapped;
  wrapped.reserve(length + 8);
  wrapped.append("<r>");
  wrapped.append(xml, length);
  wrapped.append("</r>");
  PayloadParser parser(/*taskListMode=*/true, nullptr, &out);
  return runParser(wrapped.data(), wrapped.size(), parser);
}

}  // namespace questbook_game