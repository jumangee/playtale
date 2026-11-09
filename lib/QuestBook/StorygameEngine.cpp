#include "StorygameEngine.h"

#include <Logging.h>

#include <cstdlib>
#include <cstring>

namespace questbook_game {

namespace {

constexpr int MAX_TASK_DEPTH = 16;

// A deterministic fallback for host tests; the firmware sets Engine::random to
// esp_random in StorygameGame::load().
uint32_t fallbackRandom() {
  static uint32_t state = 0x9E3779B9u;
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

bool startsWith(const char* text, size_t length, size_t at, const char* prefix) {
  const size_t prefixLen = std::strlen(prefix);
  return at + prefixLen <= length && std::memcmp(text + at, prefix, prefixLen) == 0;
}

// A smallest-possible recursive-descent evaluator for the `s` condition grammar
// (ARCHITECTURE.md §5.3): k<ordinal>/r<ordinal> identifiers, integer arithmetic,
// six comparisons and the and/or/not keywords, with parentheses. No functions,
// no string literals, no assignment. Returns 0 and clears ok on any syntax error,
// mirroring the reference's "parse error = false".
struct TextEvaluator {
  const int32_t* vars;
  size_t count;
  const char* p;
  bool ok = true;
  int depth = 0;  // live parenthesis nesting; bounded like task nesting

  explicit TextEvaluator(const int32_t* v, size_t c, const char* s) : vars(v), count(c), p(s) {}

  int32_t read(const int32_t ordinal) const {
    return (ordinal >= 0 && static_cast<size_t>(ordinal) < count) ? vars[ordinal] : 0;
  }

  void skipWs() {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
  }

  bool match(const char* token) {
    const size_t n = std::strlen(token);
    if (std::strncmp(p, token, n) == 0) {
      p += n;
      return true;
    }
    return false;
  }

  bool keyword(const char* word) {
    skipWs();
    return match(word);
  }

  int32_t parse() {
    const int32_t result = parseOr();
    skipWs();
    if (*p != '\0') ok = false;
    return result;
  }

  int32_t parseOr() {
    int32_t lhs = parseAnd();
    while (ok) {
      if (keyword("or")) {
        lhs = lhs || parseAnd();
      } else {
        skipWs();
        if (p[0] == '|' && p[1] == '|') {
          p += 2;
          lhs = lhs || parseAnd();
        } else {
          return lhs;
        }
      }
    }
    return lhs;
  }

  int32_t parseAnd() {
    int32_t lhs = parseNot();
    while (ok) {
      if (keyword("and")) {
        lhs = lhs && parseNot();
      } else {
        skipWs();
        if (p[0] == '&' && p[1] == '&') {
          p += 2;
          lhs = lhs && parseNot();
        } else {
          return lhs;
        }
      }
    }
    return lhs;
  }

  int32_t parseNot() {
    if (keyword("not")) return static_cast<int32_t>(!parseNot());
    if (p[0] == '!') {
      ++p;
      return static_cast<int32_t>(!parseNot());
    }
    return parseCmp();
  }

  int32_t parseCmp() {
    const int32_t lhs = parseAdd();
    if (!ok) return 0;
    skipWs();
    if (match("==")) return static_cast<int32_t>(lhs == parseAdd());
    if (match("!=")) return static_cast<int32_t>(lhs != parseAdd());
    if (match("<=")) return static_cast<int32_t>(lhs <= parseAdd());
    if (match(">=")) return static_cast<int32_t>(lhs >= parseAdd());
    if (match("<")) return static_cast<int32_t>(lhs < parseAdd());
    if (match(">")) return static_cast<int32_t>(lhs > parseAdd());
    return lhs;
  }

  int32_t parseAdd() {
    int32_t lhs = parseMul();
    while (ok) {
      skipWs();
      if (match("+")) {
        lhs += parseMul();
      } else if (match("-")) {
        lhs -= parseMul();
      } else {
        return lhs;
      }
    }
    return lhs;
  }

  int32_t parseMul() {
    int32_t lhs = parseUnary();
    while (ok) {
      skipWs();
      if (match("*")) {
        lhs *= parseUnary();
      } else if (match("/")) {
        const int32_t rhs = parseUnary();
        if (rhs == 0) {
          ok = false;
          return 0;
        }
        lhs /= rhs;
      } else if (match("%")) {
        const int32_t rhs = parseUnary();
        if (rhs == 0) {
          ok = false;
          return 0;
        }
        lhs %= rhs;
      } else {
        return lhs;
      }
    }
    return lhs;
  }

  int32_t parseUnary() {
    skipWs();
    if (match("-")) return -parseUnary();
    return parsePrimary();
  }

  int32_t parsePrimary() {
    skipWs();
    if (match("(")) {
      if (++depth > 64) {
        ok = false;
        return 0;
      }
      const int32_t inner = parseOr();
      --depth;
      skipWs();
      if (!match(")")) ok = false;
      return inner;
    }
    if (*p >= '0' && *p <= '9') return parseNumber();
    if ((p[0] == 'k' || p[0] == 'r') && p[1] >= '0' && p[1] <= '9') {
      ++p;  // skip the k/r prefix; an ordinal follows
      int32_t ordinal = 0;
      while (*p >= '0' && *p <= '9') {
        ordinal = ordinal * 10 + (*p - '0');
        if (ordinal > 4096) {  // far beyond the 512-parameter cap
          ok = false;
          return 0;
        }
        ++p;
      }
      return read(ordinal);
    }
    ok = false;
    return 0;
  }

  int32_t parseNumber() {
    int32_t n = 0;
    while (*p >= '0' && *p <= '9') {
      n = n * 10 + (*p - '0');
      ++p;
    }
    return n;
  }
};

// Finds the matching closing tag after `open`; returns the offset just past it,
// or length when it is absent. `tag` is the closing token, e.g. "[/b]".
size_t findClose(const char* text, size_t length, size_t open, const char* tag) {
  const size_t tagLen = std::strlen(tag);
  for (size_t i = open; i + tagLen <= length; ++i) {
    if (std::memcmp(text + i, tag, tagLen) == 0) return i + tagLen;
  }
  return length;
}

}  // namespace

StyledText Engine::lowerBbCode(const char* text, size_t length) { return lowerBbCodeImpl(text, length, 0); }

StyledText Engine::lowerBbCodeImpl(const char* text, size_t length, const int depth) {
  if (depth > 8) return {};  // nest [url]/[color] no deeper than a real paragraph

  StyledText out;
  out.reserve(length / 24 + 4);

  uint8_t style = SG_REGULAR;
  std::string pending;
  pending.reserve(64);
  bool heading = false;
  bool bullet = false;
  bool breakBefore = false;
  bool atLineStart = true;

  // Emits the pending run. When nothing has accumulated the call is a no-op that
  // keeps `breakBefore`, so a newline followed by a tag still starts a new line.
  auto flush = [&]() {
    if (pending.empty() && !heading && !bullet) return;
    out.push_back(SgRun{std::move(pending), style, heading, bullet, breakBefore});
    pending.clear();
    heading = false;
    bullet = false;
    breakBefore = false;
  };

  size_t i = 0;
  while (i < length) {
    const char c = text[i];

    // CRLF sources are common; the pair is one break, a lone CR still breaks.
    if (c == '\n' || c == '\r') {
      flush();
      breakBefore = true;
      atLineStart = true;
      ++i;
      if (c == '\r' && i < length && text[i] == '\n') ++i;
      continue;
    }

    // === Heading === / == Subheading == — whole line, matched at line start
    // (the reference anchors them with the m flag, ARCHITECTURE.md §7.4).
    if (atLineStart) {
      bool isHeading = false;
      size_t contentStart = i;
      size_t contentEnd = i;
      if (startsWith(text, length, i, "=== ")) {
        isHeading = true;
        contentStart = i + 4;
        contentEnd = findClose(text, length, contentStart, " ===");
      } else if (startsWith(text, length, i, "== ")) {
        isHeading = true;
        contentStart = i + 3;
        contentEnd = findClose(text, length, contentStart, " ==");
      }
      if (isHeading) {
        if (contentEnd > contentStart) {
          flush();
          pending.assign(text + contentStart, contentEnd - contentStart);
          heading = true;
          flush();
        }
        // The heading runs to the next newline; skip any trailing junk up to it.
        while (i < length && text[i] != '\n') ++i;
        continue;
      }
    }

    if (startsWith(text, length, i, "[b]")) {
      flush();
      style = static_cast<uint8_t>(style | SG_BOLD);
      i += 3;
      continue;
    }
    if (startsWith(text, length, i, "[/b]")) {
      flush();
      style = static_cast<uint8_t>(style & ~SG_BOLD);
      i += 4;
      continue;
    }
    if (startsWith(text, length, i, "[i]")) {
      flush();
      style = static_cast<uint8_t>(style | SG_ITALIC);
      i += 3;
      continue;
    }
    if (startsWith(text, length, i, "[/i]")) {
      flush();
      style = static_cast<uint8_t>(style & ~SG_ITALIC);
      i += 4;
      continue;
    }
    if (startsWith(text, length, i, "[u]")) {
      flush();
      style = static_cast<uint8_t>(style | SG_UNDERLINE);
      i += 3;
      continue;
    }
    if (startsWith(text, length, i, "[/u]")) {
      flush();
      style = static_cast<uint8_t>(style & ~SG_UNDERLINE);
      i += 4;
      continue;
    }
    if (startsWith(text, length, i, "[sup]")) {
      flush();
      style = static_cast<uint8_t>(style | SG_SUP);
      i += 5;
      continue;
    }
    if (startsWith(text, length, i, "[/sup]")) {
      flush();
      style = static_cast<uint8_t>(style & ~SG_SUP);
      i += 6;
      continue;
    }
    if (startsWith(text, length, i, "[player]")) {
      atLineStart = false;
      i += 8;
      continue;
    }
    if (startsWith(text, length, i, "[list]")) {
      atLineStart = false;
      i += 6;
      continue;
    }
    if (startsWith(text, length, i, "[/list]")) {
      atLineStart = false;
      i += 7;
      continue;
    }
    if (startsWith(text, length, i, "[*]") || startsWith(text, length, i, "[/*]")) {
      flush();
      breakBefore = true;
      bullet = true;
      atLineStart = false;
      i += startsWith(text, length, i, "[/*]") ? 4 : 3;
      continue;
    }
    // Link and colour: keep only the inner text (the device renders neither).
    if (startsWith(text, length, i, "[url=")) {
      const size_t close = findClose(text, length, i, "]");
      if (close < length) {
        const size_t end = findClose(text, length, close, "[/url]");
        StyledText inner = lowerBbCodeImpl(text + close, end - close, depth + 1);
        flush();
        for (SgRun& run : inner) {
          if (!run.text.empty() || run.bullet || run.heading) out.push_back(std::move(run));
        }
        i = end;
        atLineStart = false;
        continue;
      }
    }
    if (startsWith(text, length, i, "[color=")) {
      const size_t close = findClose(text, length, i, "]");
      if (close < length) {
        const size_t end = findClose(text, length, close, "[/color]");
        StyledText inner = lowerBbCodeImpl(text + close, end - close, depth + 1);
        flush();
        for (SgRun& run : inner) {
          if (!run.text.empty() || run.bullet || run.heading) out.push_back(std::move(run));
        }
        i = end;
        atLineStart = false;
        continue;
      }
    }
    if (startsWith(text, length, i, "--")) {
      pending += "\xE2\x80\x94";  // em dash —
      i += 2;
      atLineStart = false;
      continue;
    }

    atLineStart = false;
    // Copy one UTF-8 codepoint (URL/colour content is ASCII-heavy, text isn't).
    const size_t start = i;
    ++i;
    while (i < length && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) ++i;
    pending.append(text + start, i - start);
  }
  flush();
  return out;
}

// --- header binding and state ------------------------------------------------

bool Engine::begin(const Header& header) {
  header_ = header;
  if (!header_.groups || !header_.params || header_.startParagraph >= header_.paragraphCount) {
    LOG_ERR("QBG", "unusable game header (g=%d p=%d start=%u count=%u)", header_.groups ? 1 : 0,
            header_.params ? 1 : 0, static_cast<unsigned>(header_.startParagraph),
            static_cast<unsigned>(header_.paragraphCount));
    return false;
  }
  vars_.assign(header_.params->size(), 0);
  bound_ = true;
  return true;
}

bool Engine::startGame(const ParagraphSource& source) {
  if (!bound_ || !source.load) return false;
  failed_ = false;
  stop_ = false;
  pendingTransition_ = false;
  pendingClearLast_ = false;
  steps_ = 0;
  lastOrdinal_ = -1;
  haveCurrent_ = false;

  // Offline model: no save found, so executeCleanTask seeds start values and the
  // entry paragraph then runs its preload (ARCHITECTURE.md §5.4, §5.2).
  for (size_t i = 0; i < vars_.size(); ++i) vars_[i] = (*header_.params)[i].start;

  haveTriggers_ = false;
  if (header_.triggersParagraph >= 0 && static_cast<uint32_t>(header_.triggersParagraph) < header_.paragraphCount) {
    haveTriggers_ = source.load(source.context, static_cast<uint32_t>(header_.triggersParagraph), triggersPara_);
  }

  // A game whose entry paragraph is missing cannot start at all.
  if (!moveTo(source, header_.startParagraph)) failed_ = true;
  if (failed_) return false;
  runPendingTransitions(source);
  return haveCurrent_ && !failed_;
}

bool Engine::moveTo(const ParagraphSource& source, const uint32_t target) {
  if (target >= header_.paragraphCount) {
    LOG_ERR("QBG", "paragraph %u outside the game's %u paragraphs", static_cast<unsigned>(target),
            static_cast<unsigned>(header_.paragraphCount));
    return false;
  }

  // The next payload is read before anything about the current paragraph
  // changes, so a missing paragraph leaves the reader where it was.
  Paragraph next;
  if (!source.load(source.context, target, next)) {
    LOG_ERR("QBG", "cannot read paragraph %u", static_cast<unsigned>(target));
    return false;
  }

  if (haveCurrent_) {
    lastOrdinal_ = static_cast<int32_t>(current_);
    runRoleTasks(currentPara_, /*role=*/2);  // unload the paragraph being left
  }

  current_ = target;
  currentPara_ = std::move(next);
  haveCurrent_ = true;

  runRoleTasks(currentPara_, /*role=*/1);  // preload
  ++steps_;
  rebuildVisible();
  return true;
}

void Engine::runPendingTransitions(const ParagraphSource& source) {
  while (pendingTransition_) {
    pendingTransition_ = false;
    const uint32_t target = pendingTarget_;
    const bool clearLast = pendingClearLast_;
    pendingClearLast_ = false;
    // A dead link stays on the current paragraph instead of ending the game.
    if (!moveTo(source, target)) break;
    if (clearLast) lastOrdinal_ = -1;
  }
}

void Engine::runRoleTasks(const Paragraph& paragraph, const uint8_t role) {
  for (const Script& script : paragraph.scripts) {
    if (script.kind != ScriptKind::Tasks || script.role != role) continue;
    stop_ = false;
    runTasks(script.tasks, 0);
    if (stop_) return;  // a goto/back inside; the outer loop applies it
  }
}

void Engine::rebuildVisible() {
  texts_.clear();
  choices_.clear();

  const auto addParagraph = [this](const Paragraph& paragraph) {
    for (const TextBlock& block : paragraph.texts) {
      if (block.ifSlot >= 0 && !scriptResult(paragraph, block.ifSlot)) continue;
      if (block.notSlot >= 0 && scriptResult(paragraph, block.notSlot)) continue;
      VisibleText visible;
      visible.spoiler = block.spoiler;
      visible.runs = lowerBbCode(block.text);
      texts_.push_back(std::move(visible));
    }
    for (const Choice& choice : paragraph.choices) {
      if (choice.ifSlot >= 0 && !scriptResult(paragraph, choice.ifSlot)) continue;
      if (choice.notSlot >= 0 && scriptResult(paragraph, choice.notSlot)) continue;
      VisibleChoice visible;
      visible.source = &paragraph;
      visible.goTo = choice.goTo;
      visible.doSlot = choice.doSlot;
      visible.runs = lowerBbCode(choice.label);
      choices_.push_back(std::move(visible));
    }
  };

  addParagraph(currentPara_);
  if (haveTriggers_) addParagraph(triggersPara_);
}

bool Engine::choose(const ParagraphSource& source, const size_t visibleIndex) {
  if (!haveCurrent_ || visibleIndex >= choices_.size()) return false;
  const int32_t doSlot = choices_[visibleIndex].doSlot;
  const int32_t goTo = choices_[visibleIndex].goTo;

  pendingTransition_ = false;
  pendingClearLast_ = false;
  stop_ = false;

  if (doSlot >= 0) {
    const Paragraph* owner = choices_[visibleIndex].source ? choices_[visibleIndex].source : &currentPara_;
    const Script* script = owner ? findScript(*owner, doSlot) : nullptr;
    if (script && script->kind == ScriptKind::Tasks) {
      runTasks(script->tasks, 0);
    } else if (script && script->kind == ScriptKind::Condition) {
      // The reference cancels a transition only on a strict `return false`
      // (STORYGAME_TECH.md §4.4); a do naming a condition is evaluated and a
      // false result keeps the paragraph.
      if (!evaluateScript(*script)) {
        rebuildVisible();
        return true;
      }
    }
  }

  if (pendingTransition_) {
    runPendingTransitions(source);
    return !failed_;
  }
  if (goTo >= 0) {
    if (!moveTo(source, static_cast<uint32_t>(goTo))) {
      // Dead link: the target paragraph is absent, so the choice does nothing and
      // the reader stays on this paragraph with its other choices.
      rebuildVisible();
      return true;
    }
    runPendingTransitions(source);
    return !failed_;
  }
  // A choice with no goto only ran its do; its effects may have changed what is
  // visible, so re-render the same paragraph (§7.4 "renderArticle").
  rebuildVisible();
  return !failed_;
}

// --- conditions --------------------------------------------------------------

const Script* Engine::findScript(const Paragraph& paragraph, const int32_t slot) const {
  for (const Script& script : paragraph.scripts) {
    if (script.slot == slot) return &script;
  }
  return nullptr;
}

bool Engine::scriptResult(const Paragraph& paragraph, const int32_t slot) const {
  const Script* script = findScript(paragraph, slot);
  return script ? evaluateScript(*script) : false;
}

bool Engine::evaluateScript(const Script& script) const {
  if (script.kind != ScriptKind::Condition) return false;
  bool result = evaluateConditions(script.exprs, script.orMode, script.crit);
  if (script.negated) result = !result;
  return result;
}

bool Engine::evaluateConditions(const std::vector<Expr>& exprs, const bool orMode, const bool crit) const {
  if (orMode) {
    for (const Expr& expr : exprs) {
      if (evaluateExpr(expr) == crit) return true;
    }
    return false;
  }
  for (const Expr& expr : exprs) {
    if (evaluateExpr(expr) != crit) return false;
  }
  return true;
}

bool Engine::evaluateExpr(const Expr& expr) const {
  switch (expr.type) {
    case ExprType::Flag:
      return value(expr.value) != 0;
    case ExprType::Resource: {
      const int32_t current = value(expr.value);
      const int32_t amount = expr.amountParam >= 0 ? value(expr.amountParam) : expr.amount;
      if (expr.equal) return current == amount;
      return current >= amount;
    }
    case ExprType::Text: {
      TextEvaluator evaluator(vars_.data(), vars_.size(), expr.source.c_str());
      const int32_t result = evaluator.parse();
      return evaluator.ok && result != 0;
    }
    default:
      return false;
  }
}

// --- task interpreter --------------------------------------------------------

void Engine::runTasks(const TaskList& tasks, const int depth) {
  if (depth > MAX_TASK_DEPTH) {
    LOG_ERR("QBG", "task nesting over %d; stopping the list", MAX_TASK_DEPTH);
    return;
  }
  for (const Task& task : tasks) {
    if (stop_) return;
    executeTask(task, depth);
  }
}

bool Engine::executeTask(const Task& task, const int depth) {
  switch (task.cls) {
    case TaskClass::Inc: {
      const ParamRecord* p = param(task.value);
      if (!p) return true;
      if (p->type == 'k') {
        setFlag(static_cast<uint32_t>(task.value), true);
      } else {
        int32_t amount = task.amount != 0 ? task.amount : 1;
        addParam(static_cast<uint32_t>(task.value), amount);
      }
      return true;
    }
    case TaskClass::Dec: {
      const ParamRecord* p = param(task.value);
      if (!p) return true;
      if (p->type == 'k') {
        setFlag(static_cast<uint32_t>(task.value), false);
      } else {
        const int32_t amount = task.amount != 0 ? task.amount : 1;
        const int32_t current = value(task.value);
        vars_[static_cast<size_t>(task.value)] = current - amount < 0 ? 0 : current - amount;
      }
      return true;
    }
    case TaskClass::Set: {
      const ParamRecord* p = param(task.value);
      if (!p || p->type == 'k') return true;  // set on a flag is a no-op (§7.4)
      assignParam(static_cast<uint32_t>(task.value), task.amount);
      return true;
    }
    case TaskClass::Add: {
      if (task.type == ParamType::Flag) return true;
      const int32_t amount = value(task.param);
      if (amount == 0) return true;
      addParam(static_cast<uint32_t>(task.value), amount);
      return true;
    }
    case TaskClass::Sub: {
      if (task.type == ParamType::Flag) return true;
      if (!param(task.value)) return true;
      const int32_t amount = value(task.param);
      if (amount == 0) return true;
      const int32_t current = value(task.value);
      vars_[static_cast<size_t>(task.value)] = current - amount < 0 ? 0 : current - amount;
      return true;
    }
    case TaskClass::Mlp: {
      if (task.type == ParamType::Flag) return true;
      const int32_t current = value(task.value);
      const int32_t factor = value(task.param);
      const int32_t result = (current != 0 && factor != 0) ? current * factor : 0;
      assignParam(static_cast<uint32_t>(task.value), result);
      return true;
    }
    case TaskClass::Div: {
      if (task.type == ParamType::Flag) return true;
      const int32_t divisor = value(task.param);
      const int32_t current = value(task.value);
      // Round half-up, matching the reference's Math.round (counters are >= 0).
      assignParam(static_cast<uint32_t>(task.value), divisor == 0 ? 0 : (current + divisor / 2) / divisor);
      return true;
    }
    case TaskClass::Eql: {
      if (task.type == ParamType::Flag) return true;
      const int32_t amount = value(task.param);
      assignParam(static_cast<uint32_t>(task.value), amount);
      return true;
    }
    case TaskClass::Rnr: {
      if (task.type == ParamType::Flag) return true;
      const int32_t top = value(task.param);
      const int32_t amount = randomInclusive(1, top);
      if (amount > 0) addParam(static_cast<uint32_t>(task.value), amount);
      return true;
    }
    case TaskClass::Rrr: {
      if (task.type == ParamType::Flag) return true;
      if (!param(task.value)) return true;
      const int32_t top = value(task.param);
      const int32_t amount = randomInclusive(1, top);
      const int32_t current = value(task.value);
      vars_[static_cast<size_t>(task.value)] = current - amount < 0 ? 0 : current - amount;
      return true;
    }
    case TaskClass::Rng: {
      if (task.type == ParamType::Flag) return true;  // `s` group, resource side
      const GroupRecord* g = group(task.value);
      if (!g) return true;
      const int32_t picked = randomParamInGroup(*g);
      if (picked < 0) return true;
      const int32_t top = value(task.param);
      addParam(static_cast<uint32_t>(picked), randomInclusive(1, top));
      return true;
    }
    case TaskClass::Rrg: {
      if (task.type == ParamType::Flag) return true;
      const GroupRecord* g = group(task.value);
      if (!g) return true;
      const int32_t picked = randomParamInGroup(*g);
      if (picked < 0 || static_cast<size_t>(picked) >= vars_.size()) return true;
      const int32_t top = value(task.param);
      const int32_t amount = randomInclusive(1, top);
      const int32_t current = value(picked);
      vars_[static_cast<size_t>(picked)] = current - amount < 0 ? 0 : current - amount;
      return true;
    }
    case TaskClass::Rnd: {
      const GroupRecord* g = group(task.value);
      if (!g || g->type != 'k') return true;
      const int32_t picked = randomParamInGroup(*g);
      if (picked >= 0) setFlag(static_cast<uint32_t>(picked), true);
      return true;
    }
    case TaskClass::Del: {
      const GroupRecord* g = group(task.value);
      if (!g) return true;
      if (g->type != 'k') return true;  // del on counters is a no-op (§7.4)
      clearGroup(*g);
      return true;
    }
    case TaskClass::Nlg: {
      const GroupRecord* g = group(task.value);
      if (!g) return true;
      if (g->type != 'r') return true;  // nlg on flags is a no-op (§7.4)
      clearGroup(*g);
      return true;
    }
    case TaskClass::Fun: {
      const int32_t ordinal = task.value;
      if (!header_.functions || ordinal < 0 || static_cast<size_t>(ordinal) >= header_.functions->size()) {
        LOG_ERR("QBG", "function ordinal %d not found", ordinal);
        return true;
      }
      runTasks((*header_.functions)[static_cast<size_t>(ordinal)], depth + 1);
      return true;
    }
    case TaskClass::If: {
      const auto guard = [this](const CondGroup& g) {
        if (g.none) return true;
        return evaluateConditions(g.exprs, g.orMode, g.crit);
      };
      if (!guard(task.ifCond)) return true;     // skip the branch
      if (!guard(task.ifnotCond)) return true;  // skip the branch
      runTasks(task.then, depth + 1);
      return true;
    }
    case TaskClass::Goto: {
      pendingTransition_ = true;
      pendingTarget_ = static_cast<uint32_t>(task.value);
      pendingClearLast_ = false;
      stop_ = true;
      return true;
    }
    case TaskClass::Back: {
      if (lastOrdinal_ >= 0) {
        pendingTransition_ = true;
        pendingTarget_ = static_cast<uint32_t>(lastOrdinal_);
        pendingClearLast_ = true;
        lastOrdinal_ = -1;
        stop_ = true;
      }
      return true;
    }
    case TaskClass::Nul: {
      for (size_t i = 0; i < vars_.size(); ++i) vars_[i] = (*header_.params)[i].start;
      return true;
    }
    case TaskClass::Achi:
    case TaskClass::Save:
      return true;  // offline no-ops (§5.4)
    case TaskClass::Unknown:
    default:
      LOG_ERR("QBG", "unknown task class; skipping it");
      return true;
  }
}

// --- parameter helpers -------------------------------------------------------

int32_t Engine::value(const int32_t ordinal) const {
  if (ordinal < 0 || static_cast<size_t>(ordinal) >= vars_.size()) return 0;
  return vars_[static_cast<size_t>(ordinal)];
}

const ParamRecord* Engine::param(const int32_t ordinal) const {
  if (ordinal < 0 || static_cast<size_t>(ordinal) >= header_.params->size()) return nullptr;
  return &(*header_.params)[static_cast<size_t>(ordinal)];
}

const GroupRecord* Engine::group(const int32_t ordinal) const {
  if (ordinal < 0 || static_cast<size_t>(ordinal) >= header_.groups->size()) return nullptr;
  return &(*header_.groups)[static_cast<size_t>(ordinal)];
}

const GroupRecord* Engine::groupOf(const int32_t paramOrdinal) const {
  const ParamRecord* p = param(paramOrdinal);
  return p ? group(p->group) : nullptr;
}

int32_t Engine::groupSum(const GroupRecord& group) const {
  int32_t sum = 0;
  const uint32_t start = group.firstParam;
  const uint32_t end = start + group.itemCount;
  for (uint32_t i = start; i < end && i < header_.params->size(); ++i) {
    const int32_t v = value(static_cast<int32_t>(i));
    if (v == 0) continue;
    if (group.type == 'k') {
      ++sum;
    } else {
      sum += v < 0 ? -v : v;
    }
  }
  return sum;
}

int32_t Engine::groupSumWithout(const GroupRecord& group, const uint32_t excluded) const {
  int32_t sum = 0;
  const uint32_t start = group.firstParam;
  const uint32_t end = start + group.itemCount;
  for (uint32_t i = start; i < end && i < header_.params->size(); ++i) {
    if (i == excluded) continue;
    const int32_t v = value(static_cast<int32_t>(i));
    if (v == 0) continue;
    if (group.type == 'k') {
      ++sum;
    } else {
      sum += v < 0 ? -v : v;
    }
  }
  return sum;
}

int32_t Engine::randomInclusive(const int32_t min, const int32_t max) const {
  if (max < min) return 0;
  if (random) {
    const uint32_t span = static_cast<uint32_t>(max - min + 1);
    return min + static_cast<int32_t>(random() % span);
  }
  return min + static_cast<int32_t>(fallbackRandom() % static_cast<uint32_t>(max - min + 1));
}

int32_t Engine::randomParamInGroup(const GroupRecord& group) const {
  if (group.itemCount == 0) return -1;
  const int32_t pick = randomInclusive(0, group.itemCount - 1);
  return static_cast<int32_t>(group.firstParam) + pick;
}

void Engine::assignParam(const uint32_t ordinal, const int32_t raw) {
  if (ordinal >= vars_.size()) return;
  int32_t v = raw < 0 ? 0 : raw;
  const ParamRecord* p = param(static_cast<int32_t>(ordinal));
  if (!p) return;
  if (p->limit > 0 && v > p->limit) v = p->limit;
  const GroupRecord* g = groupOf(static_cast<int32_t>(ordinal));
  if (g && g->limit >= 0) {
    const int32_t room = g->limit - groupSumWithout(*g, ordinal);
    if (room < 0) v = 0;
    else if (v > room) v = room;
  }
  vars_[ordinal] = v;
}

void Engine::addParam(const uint32_t ordinal, int32_t amount) {
  if (ordinal >= vars_.size() || amount <= 0) return;
  const ParamRecord* p = param(static_cast<int32_t>(ordinal));
  if (!p) return;
  const GroupRecord* g = groupOf(static_cast<int32_t>(ordinal));
  if (g && g->limit >= 0) {
    const int32_t stored = groupSum(*g);
    if (stored + amount > g->limit) amount = g->limit - stored;
    if (amount < 0) amount = 0;
  }
  if (p->limit > 0) {
    const int32_t current = value(static_cast<int32_t>(ordinal));
    if (current + amount > p->limit) amount = p->limit - current;
    if (amount < 0) amount = 0;
  }
  vars_[ordinal] = value(static_cast<int32_t>(ordinal)) + amount;
}

void Engine::setFlag(const uint32_t ordinal, const bool on) {
  if (ordinal >= vars_.size()) return;
  const ParamRecord* p = param(static_cast<int32_t>(ordinal));
  if (!p || p->type != 'k') return;
  if (!on) {
    vars_[ordinal] = 0;
    return;
  }
  const GroupRecord* g = groupOf(static_cast<int32_t>(ordinal));
  if (g && g->limit >= 0 && groupSum(*g) + 1 > g->limit) return;  // group full: no-op
  vars_[ordinal] = 1;
}

void Engine::clearGroup(const GroupRecord& group) {
  const uint32_t start = group.firstParam;
  const uint32_t end = start + group.itemCount;
  for (uint32_t i = start; i < end && i < vars_.size(); ++i) vars_[i] = 0;
}

void Engine::charsheet(std::vector<CharsheetGroup>& out) const {
  out.clear();
  if (!header_.groups || !header_.params) return;
  for (size_t g = 0; g < header_.groups->size(); ++g) {
    const GroupRecord& group = (*header_.groups)[g];
    if ((group.flags & 1) == 0 || group.itemCount == 0) continue;  // hidden or empty
    CharsheetGroup entry;
    entry.groupOrdinal = static_cast<uint32_t>(g);
    int32_t stored = 0;
    const uint32_t end = group.firstParam + group.itemCount;
    for (uint32_t p = group.firstParam; p < end && p < header_.params->size(); ++p) {
      const int32_t v = value(static_cast<int32_t>(p));
      if (v == 0) continue;
      entry.items.push_back(CharsheetItem{p, v});
      if (group.type == 'k') {
        ++stored;
      } else {
        stored += v < 0 ? -v : v;
      }
    }
    if (entry.items.empty()) continue;
    entry.stored = stored;
    out.push_back(std::move(entry));
  }
}

}  // namespace questbook_game