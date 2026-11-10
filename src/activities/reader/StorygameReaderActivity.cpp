#include "StorygameReaderActivity.h"

#include <Arduino.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <FreeInkUIGfxRenderer.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>
#include <utility>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "StorygameStore.h"
#include "components/UiAppHelpers.h"
#include "components/UITheme.h"

namespace qg = questbook_game;
namespace fui = freeink::ui;

namespace {

constexpr int CONTENT_PAD = 8;
// Drawn bullet plus the gap before its text; also the width the bullet takes
// from the line it marks.
constexpr int BULLET_INDENT = 8;
constexpr int TEXT_GAP = 10;
constexpr int CHOICE_GAP = 8;
constexpr int CHOICE_GAP_BEFORE = 16;
// Inner padding of a choice button: the label is inset so the border does not
// touch the text.
constexpr int CHOICE_PAD_X = 10;
constexpr int CHOICE_PAD_Y = 6;
// The floating character-sheet tab on the right edge: a page tab flush with the
// panel edge, so only its left profile is drawn -- a slanted top, a vertical
// side and a slanted bottom. The visual is small on purpose; the touch target is
// grown to the SDK's minimum (fui::ThemeTokens::minTouchSize) around it.
constexpr int SHEET_BUTTON_WIDTH = 30;   // from the panel's right edge inwards
constexpr int SHEET_BUTTON_HEIGHT = 54;
constexpr int SHEET_BUTTON_TAPER = 12;   // vertical extent of each slanted corner
constexpr int16_t SHEET_BUTTON_TOUCH_TARGET = 44;
// The sheet reveal. The wipe is paced by a clock in loop(), not by how long a
// panel refresh happens to take: on a host build a refresh is free, so a reveal
// that waited only for frames ran through every position in a few milliseconds
// and looked instantaneous. A position is taken only once the previous frame has
// been painted, so each step below is a frame the reader actually sees, and each
// one costs the longer of SHEET_REVEAL_STEP_MS and one panel refresh. Two
// positions -- half the page, then the page -- are enough to read as a sheet
// sliding over the paragraph without making the reader wait. Set this to 1 to
// open the sheet in a single frame.
constexpr int SHEET_REVEAL_STEPS = 2;
constexpr uint32_t SHEET_REVEAL_STEP_MS = 500;

// Longest word the line writer will hyphenate. Game text words are far shorter;
// the cap keeps every candidate prefix inside one fixed stack buffer.
constexpr size_t HYPHEN_TEXT_MAX = 96;

EpdFontFamily::Style toFontStyle(const uint8_t sg) {
  uint8_t style = 0;
  if (sg & qg::SG_BOLD) style |= static_cast<uint8_t>(EpdFontFamily::BOLD);
  if (sg & qg::SG_ITALIC) style |= static_cast<uint8_t>(EpdFontFamily::ITALIC);
  if (sg & qg::SG_UNDERLINE) style |= static_cast<uint8_t>(EpdFontFamily::UNDERLINE);
  if (sg & qg::SG_SUP) style |= static_cast<uint8_t>(EpdFontFamily::SUP);
  return static_cast<EpdFontFamily::Style>(style);
}

bool contains(const Rect& rect, const int x, const int y) {
  return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
}

}  // namespace

StorygameReaderActivity::StorygameReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 std::string gameDir)
    : Activity("StorygameReader", renderer, mappedInput), gameDir(std::move(gameDir)) {
  source = game.paragraphSource();
}

std::unique_ptr<StorygameReaderActivity> StorygameReaderActivity::create(GfxRenderer& renderer,
                                                                         MappedInputManager& mappedInput,
                                                                         const std::string& gameDir) {
  auto activity = makeUniqueNoThrow<StorygameReaderActivity>(renderer, mappedInput, gameDir);
  if (!activity) LOG_ERR("QBG", "no room for the storygame reader");
  return activity;
}

int StorygameReaderActivity::bodyFontId() const { return SETTINGS.getReaderFontId(); }

int StorygameReaderActivity::spaceAdvance(const uint8_t style) const {
  const int fontId = bodyFontId();
  const int w = renderer.getSpaceWidth(fontId, toFontStyle(style));
  // A font without a space glyph reports zero; keep words apart regardless.
  return w > 0 ? w : renderer.getLineHeight(fontId) / 4;
}

int StorygameReaderActivity::viewportHeight() const {
  const Rect area = UITheme::getInstance().getScreenSafeArea(renderer, /*front*/ true, /*side*/ false);
  return area.height;
}

bool StorygameReaderActivity::sheetButtonVisible() const {
  return view == View::Paragraph && sheetButtonEnabled();
}

bool StorygameReaderActivity::sheetButtonEnabled() const {
  // The button duplicates the swipe, so it only exists where a swipe can happen
  // and where the sheet has something to show.
  return charsheetAvailable && mappedInput.hasTouch();
}

int StorygameReaderActivity::textColumnWidth() const {
  const Rect area = UITheme::getInstance().getScreenSafeArea(renderer, /*front*/ true, /*side*/ false);
  // The floating sheet tab is painted over the page, so it takes no room from the
  // text: the right edge keeps the same CONTENT_PAD from the scroll indicator as
  // the left edge keeps from the page. The indicator's width is reserved even when
  // no bar is drawn, so a column does not re-wrap when a paragraph grows past one
  // screen.
  return area.width - CONTENT_PAD * 2 - UITheme::getInstance().getMetrics().listScrollWidth;
}

int StorygameReaderActivity::pageStep(const Column& column, const bool forward) const {
  // Recomputed from `column.scroll` on every call: a cached page index or
  // landing offset would go stale on a relayout, on a font change, and on the
  // mid-line offsets revealFocusedChoice() produces.
  const int vh = viewportHeight();
  size_t top = 0;
  while (top < column.lines.size() && column.lines[top].y + column.lines[top].height <= column.scroll) ++top;
  // No lines at all, or every line is above the viewport: nothing to move to.
  if (top == column.lines.size()) return 0;

  if (forward) {
    // The last line the page shows whole is where the next page starts, so the
    // bottom line of one page is the first line of the next.
    size_t bottom = top;
    while (bottom + 1 < column.lines.size() &&
           column.lines[bottom + 1].y + column.lines[bottom + 1].height <= column.scroll + vh) {
      ++bottom;
    }
    const int step = column.lines[bottom].y - column.scroll;
    // A viewport too short for two whole lines still has to move somewhere.
    return step > 0 ? step : column.lines[top].height;
  }

  // Backward mirrors that: the page ends on the last line top above this one, so
  // the text is read from its first line instead of being cut in half.
  const int target = column.lines[top].y + column.lines[top].height - vh;
  if (target <= column.lines.front().y) return -column.scroll;
  int previous = column.lines.front().y;
  for (size_t i = 0; i <= top; ++i) {
    if (column.lines[i].y >= target) {
      previous = column.lines[i].y;
      break;
    }
  }
  const int step = previous - column.scroll;
  return step < 0 ? step : -column.lines[top].height;
}

void StorygameReaderActivity::onEnter() {
  Activity::onEnter();
  {
    RenderLock lock(*this);
    view = View::Paragraph;
    revealStep = 0;
    revealNextStepAt = 0;
    sheetInputArmed = true;
    paragraph.scroll = 0;
    sheet.lines.clear();
    sheet.height = 0;
    sheet.scroll = 0;
    focusChoice = 0;
    choiceButtonStyles =
        outlinedActionButtonStyles(static_cast<uint8_t>(UITheme::getInstance().getMetrics().controlRadius));
  }
  failed = !loadGame();
  requestUpdate();
}

bool StorygameReaderActivity::loadGame() {
  if (gameDir.empty()) return false;
  if (!game.load(gameDir)) return false;
  source = game.paragraphSource();
  if (!game.engine().startGame(source)) return false;
  // Read once before the first layout, so the first frame already knows whether
  // the tab and the hint label apply. Published under the render lock the render
  // task reads it under.
  const bool sheetNow = game.engine().hasCharsheet();
  {
    RenderLock lock(*this);
    charsheetAvailable = sheetNow;
  }
  rebuildParagraphLayout();
  return !game.engine().failed();
}

// Deferred to the first painted frame for the reason ReaderActivity defers its own
// entry: the store's strings and its JSON write must not land between the
// reader's allocations. The path is the directory the game was opened from, so a
// game played from Play's shared workspace takes the workspace's single card
// instead of adding a library item that does not exist (ARCHITECTURE.md §6.3).
void StorygameReaderActivity::rememberRecentOnceRendered() {
  if (recentRemembered || failed || !painted.load(std::memory_order_acquire)) return;
  recentRemembered = true;

  std::string metaPath = qg::pathIn(gameDir, qg::META_FILE_NAME);
  questbook_game::Meta meta;
  // A game whose meta.json is unreadable still plays, and its card then carries the
  // fallback the library row uses: the folder name, which for a game is its own
  // name. The workspace's folder name is not one — "___temp" is internal — so a
  // temporary game without readable metadata takes the library's unknown-title
  // string rather than showing that name to the reader.
  questbook_game::readMetaFile(metaPath, meta);
  std::string title = std::move(meta.title);
  if (title.empty()) title = std::move(meta.indexName);
  if (title.empty()) {
    if (qg::isTempWorkspace(gameDir)) {
      title = tr(STR_LIBRARY_UNKNOWN_TITLE);
    } else {
      const size_t slash = gameDir.rfind('/');
      title = slash == std::string::npos ? gameDir : gameDir.substr(slash + 1);
    }
  }
  // The cover sits beside the game in either location: Add to Library writes it into
  // a permanent directory and Play writes it into the workspace before this reader
  // opens (§6.3), so a temporary game's card carries it too. It is the game page's
  // 128x192 slot art, and the home card draws a cover path 1:1 rather than scaling
  // it, so the game's cover shows smaller than a book's thumb, which is generated at
  // the card's own height. Card-sized game art would need a scaler this tree does not
  // have (ARCHITECTURE.md §4.9 is the canonical note).
  std::string coverPath = qg::pathIn(gameDir, qg::COVER_FILE_NAME);
  if (!Storage.exists(coverPath.c_str())) coverPath.clear();
  RECENT_BOOKS.addBook(gameDir, title, meta.author, coverPath);
}

// --- line writer -------------------------------------------------------------

void StorygameReaderActivity::resetWriter(Column& column, const int y, const int x, const int width, const int lh) {
  writer = &column;
  writerY = y;
  writerX = x;
  writerWidth = width;
  writerLh = lh;
  writerLine = Line{};
  writerLineWidth = 0;
  writerChoice = -1;
  writerBullet = false;
  writerHyphenate = false;
}

void StorygameReaderActivity::flushLine() {
  if (writer && (!writerLine.text.empty() || writerLine.heading || writerLine.bullet)) {
    writerLine.y = writerY;
    writerLine.x = writerX;
    writerLine.height = writerLh;
    writerLine.choiceIndex = writerChoice;
    writerLine.bullet = writerBullet;
    writer->lines.push_back(std::move(writerLine));
    writerY += writerLh;
  }
  writerLine = Line{};
  writerLineWidth = 0;
  writerBullet = false;
}

void StorygameReaderActivity::breakLine() { flushLine(); }

void StorygameReaderActivity::appendPiece(const uint8_t style, const std::string& piece, const int pieceWidth,
                                          const int spaceWidth) {
  writerLine.y = writerY;  // begin the line at the writer's current slot
  writerLine.x = writerX;
  if (writerLine.spans.empty() && writerBullet) {
    // The bullet is drawn left of the text, so its indent is part of the width
    // this line may use; charging it here keeps the wrap and the drawing equal.
    writerLineWidth = BULLET_INDENT;
  }
  if (!writerLine.spans.empty()) {
    writerLine.text.push_back(' ');
    writerLineWidth += spaceWidth;
  }
  const uint16_t offset = static_cast<uint16_t>(writerLine.text.size());
  const uint16_t length = static_cast<uint16_t>(piece.size());
  writerLine.spans.push_back(Span{offset, length, style});
  writerLine.text.append(piece);
  writerLineWidth += pieceWidth;
}

StorygameReaderActivity::HyphenBreak StorygameReaderActivity::hyphenSplit(const std::string& word, const uint8_t style,
                                                                          const int spaceWidth) const {
  // The gap before the word and the hyphen after the prefix are part of what has
  // to fit on the line, so only what is left after the gap is available.
  const int room = writerWidth - writerLineWidth - spaceWidth;
  if (room <= 0 || word.size() >= HYPHEN_TEXT_MAX) return HyphenBreak{};

  const int fontId = bodyFontId();
  const EpdFontFamily::Style fontStyle = toFontStyle(style);
  // ParsedText::hyphenateWordAtIndex() chooses the break the same way for the EPUB
  // reader, but it adds break sources this writer does not have (Hangul, fallback
  // splits for a first word) and weighs focus styling and justification. Keep the
  // rule itself in step when either side changes.
  const auto breaks = Hyphenator::breakOffsets(word, /*includeFallback=*/false);
  if (breaks.empty()) return HyphenBreak{};

  // Candidates are measured from a stack buffer. This runs once for a line that
  // ends inside a word, and a heap string per candidate would churn the
  // allocator for measurements that are discarded except for the widest one.
  char prefix[HYPHEN_TEXT_MAX + 2];  // prefix + inserted hyphen + NUL
  HyphenBreak best;
  int bestWidth = -1;
  for (const Hyphenator::BreakInfo& info : breaks) {
    const size_t bytes = info.byteOffset;
    if (bytes == 0 || bytes >= word.size()) continue;
    memcpy(prefix, word.data(), bytes);
    size_t length = bytes;
    // A word with its own hyphen breaks after that hyphen, which the prefix
    // already carries; a pattern break needs the hyphen drawn.
    if (info.requiresInsertedHyphen) prefix[length++] = '-';
    prefix[length] = '\0';
    const int width = renderer.getTextWidth(fontId, prefix, fontStyle);
    if (width > room || width <= bestWidth) continue;
    bestWidth = width;
    best.bytes = bytes;
    best.width = width;
    best.insertHyphen = info.requiresInsertedHyphen;
  }
  return best;
}

void StorygameReaderActivity::appendWord(const uint8_t style, const std::string& word) {
  if (word.empty()) return;
  const int spaceWidth = spaceAdvance(style);
  const EpdFontFamily::Style fontStyle = toFontStyle(style);

  // The part of the word still to place. Hyphenating at a line end leaves the
  // rest of the word here, so one word reaches the column as two pieces.
  wordBuf = word;
  for (;;) {
    const int wordWidth = renderer.getTextWidth(bodyFontId(), wordBuf.c_str(), fontStyle);
    const bool overflow = !writerLine.spans.empty() && writerLineWidth + spaceWidth + wordWidth > writerWidth;
    const bool canHyphenate = overflow && writerHyphenate;
    const HyphenBreak split = canHyphenate ? hyphenSplit(wordBuf, style, spaceWidth) : HyphenBreak{};
    if (split.bytes == 0) {
      // The word whole: on this line when it fits, on the next one when it does
      // not. A word wider than the column still overflows it, as it always has.
      if (overflow) flushLine();
      appendPiece(style, wordBuf, wordWidth, spaceWidth);
      return;
    }
    std::string piece(wordBuf, 0, split.bytes);
    if (split.insertHyphen) piece.push_back('-');
    appendPiece(style, piece, split.width, spaceWidth);
    flushLine();
    wordBuf.erase(0, split.bytes);
  }
}

void StorygameReaderActivity::appendStyledText(const std::string& text, const uint8_t style) {
  const auto isSpace = [](const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && isSpace(text[i])) ++i;
    const size_t start = i;
    while (i < text.size() && !isSpace(text[i])) ++i;
    if (i == start) break;
    wordBuf.assign(text, start, i - start);
    appendWord(style, wordBuf);
  }
}

void StorygameReaderActivity::appendRun(const qg::SgRun& run) {
  if (run.breakBefore) breakLine();
  if (run.heading) {
    breakLine();
    Line headingLine;
    headingLine.y = writerY;
    headingLine.x = writerX;
    headingLine.height = writerLh;
    headingLine.heading = true;
    // A heading inside a choice label has to invert with the rest of the button,
    // or it would vanish on the black fill.
    headingLine.choiceIndex = writerChoice;
    headingLine.text = run.text;
    headingLine.spans.push_back(
        Span{0, static_cast<uint16_t>(run.text.size()), static_cast<uint8_t>(run.style | qg::SG_BOLD)});
    if (writer) writer->lines.push_back(std::move(headingLine));
    writerY += writerLh;
    return;
  }
  if (run.bullet) writerBullet = true;
  if (!run.text.empty()) appendStyledText(run.text, run.style);
}

// --- layout ------------------------------------------------------------------

void StorygameReaderActivity::rebuildParagraphLayout() {
  RenderLock lock(*this);  // owns the column; the render task reads it under the same lock

  paragraph.lines.clear();
  paragraph.choiceTop.clear();
  paragraph.choiceBottom.clear();
  paragraph.height = 0;
  paragraph.width = textColumnWidth();

  const int fontId = bodyFontId();
  const int lh = renderer.getLineHeight(fontId);
  const int textX = CONTENT_PAD;
  const int width = paragraph.width;

  // The game format carries no language and quest-book.ru publishes Russian text,
  // so hyphenation runs on Russian patterns. A word those patterns do not cover
  // gets no break, so text in another script keeps wrapping at spaces. The engine
  // owns the choice as process-wide state, so it is set for this layout only and
  // cleared afterwards instead of being left for the next screen to inherit.
  const bool hyphenate = SETTINGS.hyphenationEnabled != 0;
  if (hyphenate) Hyphenator::setPreferredLanguage("ru");

  int y = CONTENT_PAD;
  resetWriter(paragraph, y, textX, width, lh);
  // The paragraph's own prose is the only text this writer hyphenates: a hyphen
  // in a choice's label reads as a typo, and the sheet lays out key-value rows.
  writerHyphenate = hyphenate;

  const auto& texts = game.engine().texts();
  for (const qg::VisibleText& block : texts) {
    if (!block.spoiler.empty()) {
      breakLine();
      Line spoilerLine;
      spoilerLine.y = writerY;
      spoilerLine.x = textX;
      spoilerLine.height = lh;
      spoilerLine.heading = true;
      spoilerLine.text = block.spoiler;
      spoilerLine.spans.push_back(
          Span{0, static_cast<uint16_t>(block.spoiler.size()), static_cast<uint8_t>(qg::SG_BOLD)});
      paragraph.lines.push_back(std::move(spoilerLine));
      writerY += lh;
    }
    for (const qg::SgRun& run : block.runs) appendRun(run);
    breakLine();
    writerY += TEXT_GAP;
    resetWriter(paragraph, writerY, textX, width, lh);
    writerHyphenate = hyphenate;
  }

  const auto& choices = game.engine().choices();
  if (!choices.empty()) {
    writerY += CHOICE_GAP_BEFORE;
    for (size_t i = 0; i < choices.size(); ++i) {
      // Each choice is one button: the label is laid out inside the padding, and
      // the button's own box is what render() fills and inverts.
      const int boxTop = writerY;
      resetWriter(paragraph, boxTop + CHOICE_PAD_Y, textX + CHOICE_PAD_X, width - CHOICE_PAD_X * 2, lh);
      writerChoice = static_cast<int16_t>(i);
      for (const qg::SgRun& run : choices[i].runs) appendRun(run);
      breakLine();
      int textBottom = writerY;
      // An empty label still gets a full-height button rather than a hairline.
      if (textBottom < boxTop + CHOICE_PAD_Y + lh) textBottom = boxTop + CHOICE_PAD_Y + lh;
      const int boxBottom = textBottom + CHOICE_PAD_Y;
      paragraph.choiceTop.push_back(boxTop);
      paragraph.choiceBottom.push_back(boxBottom);
      writerY = boxBottom + CHOICE_GAP;
    }
    writerChoice = -1;
  } else {
    breakLine();
    Line endLine;
    endLine.y = writerY;
    endLine.x = textX;
    endLine.height = lh;
    endLine.heading = true;
    endLine.text = tr(STR_QUESTBOOK_THE_END);
    endLine.spans.push_back(
        Span{0, static_cast<uint16_t>(endLine.text.size()), static_cast<uint8_t>(qg::SG_BOLD)});
    paragraph.lines.push_back(std::move(endLine));
    writerY += lh;
  }

  paragraph.height = writerY + CONTENT_PAD;
  writer = nullptr;
  writerHyphenate = false;
  // Back to the engine's boot state, so a screen that lays text out without a
  // language of its own (the text-settings preview, a dictionary page) is not
  // handed the rules this layout needed.
  if (hyphenate) Hyphenator::setPreferredLanguage("");
}

void StorygameReaderActivity::rebuildSheetLayout() {
  RenderLock lock(*this);

  sheet.lines.clear();
  sheet.height = 0;
  sheet.scroll = 0;
  // Both columns are laid out to the same width; the sheet simply has no tab
  // painted over it.
  sheet.width = textColumnWidth();

  const int fontId = bodyFontId();
  const int lh = renderer.getLineHeight(fontId);
  const int x = CONTENT_PAD;
  const int width = sheet.width;

  int y = CONTENT_PAD;
  resetWriter(sheet, y, x, width, lh);

  std::vector<qg::Engine::CharsheetGroup> groups;
  game.engine().charsheet(groups);

  // One header line per group plus one line per parameter is an upper bound for
  // anything that does not wrap; reserving before the first push keeps a long
  // sheet from growing the line array while the paragraph is still held.
  size_t expectedLines = 1 + groups.size();
  for (const qg::Engine::CharsheetGroup& group : groups) expectedLines += group.items.size();
  sheet.lines.reserve(expectedLines);

  // Title.
  wordBuf = tr(STR_QUESTBOOK_CHARSHEET);
  writerLine.y = y;
  writerLine.x = x;
  writerLine.height = lh;
  writerLine.heading = true;
  writerLine.text = wordBuf;
  writerLine.spans.push_back(Span{0, static_cast<uint16_t>(wordBuf.size()), qg::SG_BOLD});
  sheet.lines.push_back(std::move(writerLine));
  writerY = y + lh;
  writerLine = Line{};

  if (groups.empty()) {
    writerLine = Line{};
    const std::string noParams = tr(STR_QUESTBOOK_NO_PARAMS);
    appendStyledText(noParams, qg::SG_REGULAR);
    flushLine();
    sheet.height = writerY + CONTENT_PAD;
    writer = nullptr;
    return;
  }

  std::string nameBuf;
  std::string valueLine;
  for (const qg::Engine::CharsheetGroup& group : groups) {
    if (game.groupName(group.groupOrdinal, nameBuf) && !nameBuf.empty()) {
      Line header;
      header.y = writerY;
      header.x = x;
      header.height = lh;
      header.heading = true;
      header.text = nameBuf;
      header.spans.push_back(Span{0, static_cast<uint16_t>(nameBuf.size()), qg::SG_BOLD});
      sheet.lines.push_back(std::move(header));
      writerY += lh;
    }
    for (const qg::Engine::CharsheetItem& item : group.items) {
      valueLine = "  ";
      if (game.paramName(item.paramOrdinal, nameBuf)) {
        valueLine += nameBuf;
      } else {
        valueLine += "?";
      }
      valueLine += " : ";
      valueLine += std::to_string(item.value);
      appendStyledText(valueLine, qg::SG_REGULAR);
      breakLine();
    }
    writerY += TEXT_GAP;
  }
  sheet.height = writerY + CONTENT_PAD;
  writer = nullptr;
}

// --- input -------------------------------------------------------------------

int StorygameReaderActivity::choiceIndexAt(const int screenX, const int screenY) {
  RenderLock lock(*this);
  const Rect area = UITheme::getInstance().getScreenSafeArea(renderer, /*front*/ true, /*side*/ false);
  // Only the button's own box is a target: a tap beside the text must not advance
  // the story. The floating sheet tab paints over the right end of that box, so
  // handleParagraphInput() tests the tab first and keeps those taps off a choice.
  if (screenX < area.x + CONTENT_PAD || screenX >= area.x + CONTENT_PAD + paragraph.width) return -1;
  const int contentY = screenY - area.y + paragraph.scroll;
  for (size_t i = 0; i < paragraph.choiceTop.size(); ++i) {
    if (contentY >= paragraph.choiceTop[i] && contentY < paragraph.choiceBottom[i]) return static_cast<int>(i);
  }
  return -1;
}

void StorygameReaderActivity::clampScroll(Column& column, const int viewportHeight) {
  const int maxScroll = column.height - viewportHeight;
  if (maxScroll <= 0) {
    column.scroll = 0;
    return;
  }
  if (column.scroll < 0) column.scroll = 0;
  if (column.scroll > maxScroll) column.scroll = maxScroll;
}

void StorygameReaderActivity::scrollColumn(Column& column, const int delta) {
  column.scroll += delta;
  clampScroll(column, viewportHeight());
}

void StorygameReaderActivity::revealFocusedChoice() {
  const auto& choices = game.engine().choices();
  if (choices.empty()) return;
  RenderLock lock(*this);
  if (focusChoice < 0) focusChoice = 0;
  if (static_cast<size_t>(focusChoice) >= choices.size()) focusChoice = static_cast<int>(choices.size()) - 1;
  if (focusChoice < 0 || focusChoice >= static_cast<int>(paragraph.choiceTop.size())) return;

  const int vh = viewportHeight();
  const int top = paragraph.choiceTop[focusChoice];
  const int bottom = paragraph.choiceBottom[focusChoice];
  if (top < paragraph.scroll) {
    paragraph.scroll = top;
  } else if (bottom > paragraph.scroll + vh) {
    paragraph.scroll = bottom - vh;
  }
  clampScroll(paragraph, vh);
}

void StorygameReaderActivity::activateVisibleChoice(const size_t index) {
  const auto& choices = game.engine().choices();
  if (index >= choices.size()) return;
  game.engine().choose(source, index);
  if (game.engine().failed()) {
    RenderLock lock(*this);
    failed = true;
    requestUpdate();
    return;
  }
  // A choice changes the parameters, so both the sheet's contents and whether
  // the sheet is worth showing change with it. The engine query runs outside the
  // lock; only its result is published under it.
  const bool sheetNow = game.engine().hasCharsheet();
  {
    RenderLock lock(*this);
    charsheetAvailable = sheetNow;
    paragraph.scroll = 0;
    focusChoice = 0;
  }
  rebuildParagraphLayout();
  requestUpdate();
}

void StorygameReaderActivity::openCharsheet() {
  // The sheet is laid out before the view changes, so no frame can render a
  // sheet whose lines are still being built, and the view is published together
  // with the first reveal step, so no frame can see "sheet open, nothing
  // covered". The rest of the steps are advanced from loop() on a clock.
  rebuildSheetLayout();
  {
    RenderLock lock(*this);
    view = View::Sheet;
    revealStep = 1;
    revealNextStepAt = static_cast<uint32_t>(millis()) + SHEET_REVEAL_STEP_MS;
    sheetInputArmed = false;
  }
  requestUpdate();
}

void StorygameReaderActivity::advanceSheetReveal() {
  if (revealStep >= SHEET_REVEAL_STEPS) return;
  // One position per painted frame. The render task clears its notification
  // count when it starts a frame, so two requests raised while it is busy would
  // merge and the position in between would never reach the panel.
  if (RenderLock::peek()) return;
  // `revealNextStepAt` is written and read only by the loop task; the lock above
  // guards `revealStep`, which the render task reads.
  const uint32_t now = static_cast<uint32_t>(millis());
  // Signed difference: correct across the millis() wrap.
  if (static_cast<int32_t>(now - revealNextStepAt) < 0) return;
  {
    RenderLock lock(*this);
    if (revealStep < SHEET_REVEAL_STEPS) ++revealStep;
    revealNextStepAt = now + SHEET_REVEAL_STEP_MS;
  }
  requestUpdate();
}

void StorygameReaderActivity::closeCharsheet() {
  {
    RenderLock lock(*this);
    view = View::Paragraph;
    revealStep = 0;
    // Only the sheet's line payloads go: closeCharsheet keeps the vector's
    // capacity so the next open reuses the same block instead of growing a new
    // one beside the paragraph column the reader is still holding.
    sheet.lines.clear();
    sheet.height = 0;
    sheet.scroll = 0;
  }
  // The paragraph keeps its own column and scroll position, so closing the sheet
  // returns to where the reader was.
  requestUpdate();
}

bool StorygameReaderActivity::handleParagraphInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return true;
  }

  const MappedInputManager::SwipeDir swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left) {
    openCharsheet();
    return true;
  }
  if (swipe == MappedInputManager::SwipeDir::Right) {
    finish();
    return true;
  }
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const bool forward = swipe == MappedInputManager::SwipeDir::Up;
    {
      RenderLock lock(*this);
      scrollColumn(paragraph, pageStep(paragraph, forward));
    }
    requestUpdate();
    return true;
  }

  int tapX = 0;
  int tapY = 0;
  if (mappedInput.wasScreenTapped(tapX, tapY)) {
    if (sheetButtonVisible()) {
      const Rect area = UITheme::getInstance().getScreenSafeArea(renderer, /*front*/ true, /*side*/ false);
      if (contains(sheetButtonHitRect(area), tapX, tapY)) {
        openCharsheet();
        return true;
      }
    }
    const int index = choiceIndexAt(tapX, tapY);
    if (index >= 0) {
      activateVisibleChoice(static_cast<size_t>(index));
      return true;
    }
  }

  const auto& choices = game.engine().choices();
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!choices.empty()) {
      activateVisibleChoice(static_cast<size_t>(focusChoice));
      return true;
    }
    return true;
  }
  // The up/down directions are the side buttons on a button-only board, which is
  // the only way to move through a paragraph there: they scroll when the
  // paragraph has no choice to focus, and step the focus when it has.
  if (mappedInput.wasReleased(MappedInputManager::Button::ScreenUp)) {
    if (!choices.empty()) {
      {
        RenderLock lock(*this);
        --focusChoice;
        if (focusChoice < 0) focusChoice = static_cast<int>(choices.size()) - 1;
      }
      revealFocusedChoice();
    } else {
      RenderLock lock(*this);
      scrollColumn(paragraph, pageStep(paragraph, /*forward=*/false));
    }
    requestUpdate();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::ScreenDown)) {
    if (!choices.empty()) {
      {
        RenderLock lock(*this);
        focusChoice = (focusChoice + 1) % static_cast<int>(choices.size());
      }
      revealFocusedChoice();
    } else {
      RenderLock lock(*this);
      scrollColumn(paragraph, pageStep(paragraph, /*forward=*/true));
    }
    requestUpdate();
    return true;
  }
  // The swipe is the touch way into the character sheet; the left direction is
  // the same action on a board that has no touch, where a swipe cannot happen.
  if (mappedInput.wasReleased(MappedInputManager::Button::ScreenLeft)) {
    openCharsheet();
    return true;
  }
  return false;
}

bool StorygameReaderActivity::handleCharsheetInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::ScreenLeft)) {
    closeCharsheet();
    return true;
  }
  const MappedInputManager::SwipeDir swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Right) {
    closeCharsheet();
    return true;
  }

  int delta = 0;
  if (swipe == MappedInputManager::SwipeDir::Up) {
    delta = 1;
  } else if (swipe == MappedInputManager::SwipeDir::Down) {
    delta = -1;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::ScreenUp)) {
    delta = -1;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::ScreenDown)) {
    delta = 1;
  }
  if (delta == 0) return false;

  {
    RenderLock lock(*this);
    scrollColumn(sheet, pageStep(sheet, /*forward=*/delta > 0));
  }
  requestUpdate();
  return true;
}

void StorygameReaderActivity::loop() {
  rememberRecentOnceRendered();
  if (failed) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
    return;
  }
  if (view == View::Sheet) {
    // The wipe runs on a clock of its own; input is not read until the sheet has
    // fully arrived, so a press during the wipe cannot act on a half-drawn page.
    if (revealStep < SHEET_REVEAL_STEPS) {
      // Back cancels an opening sheet. It is the same escape the open sheet
      // honours, and the wipe must not swallow it while the panel is still moving.
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        closeCharsheet();
        return;
      }
      advanceSheetReveal();
      return;
    }
    if (!sheetInputArmed) {
      // A button held from before the sheet opened carries no intent for the
      // sheet, so the sheet is armed only once nothing is held -- and the pass
      // where the hold ends is dropped, because that is where its release edge
      // arrives.
      if (sheetButtonHeld()) return;
      sheetInputArmed = true;
      return;
    }
    handleCharsheetInput();
    return;
  }
  handleParagraphInput();
}

bool StorygameReaderActivity::sheetButtonHeld() const {
  return mappedInput.isPressed(MappedInputManager::Button::Back) ||
         mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
         mappedInput.isPressed(MappedInputManager::Button::ScreenLeft) ||
         mappedInput.isPressed(MappedInputManager::Button::ScreenUp) ||
         mappedInput.isPressed(MappedInputManager::Button::ScreenDown);
}

// --- render ------------------------------------------------------------------

Rect StorygameReaderActivity::sheetButtonRect(const Rect& area) const {
  // Flush with the panel's right edge, vertically centred on the content area.
  // The panel edge rather than the safe area is deliberate: a tab that stops
  // short of the edge is exactly what made the old button look like it floated.
  const int right = renderer.getScreenWidth();
  const int y = area.y + (area.height - SHEET_BUTTON_HEIGHT) / 2;
  return Rect{right - SHEET_BUTTON_WIDTH, y, SHEET_BUTTON_WIDTH, SHEET_BUTTON_HEIGHT};
}

Rect StorygameReaderActivity::sheetButtonHitRect(const Rect& area) const {
  const Rect box = sheetButtonRect(area);
  const fui::Rect visual{static_cast<int16_t>(box.x), static_cast<int16_t>(box.y), static_cast<int16_t>(box.width),
                         static_cast<int16_t>(box.height)};
  const fui::Rect screen{0, 0, static_cast<int16_t>(renderer.getScreenWidth()),
                         static_cast<int16_t>(renderer.getScreenHeight())};
  const fui::Rect hit = fui::ensureMinTouchRect(visual, SHEET_BUTTON_TOUCH_TARGET, screen);
  return Rect{hit.x, hit.y, hit.width, hit.height};
}

void StorygameReaderActivity::paintBox(fui::DrawTarget& target, const Rect& box, const fui::BoxStyle& style) {
  const fui::Rect rect{static_cast<int16_t>(box.x), static_cast<int16_t>(box.y), static_cast<int16_t>(box.width),
                       static_cast<int16_t>(box.height)};
  target.fill(rect, style.background, style.radius, style.corners);
  if (style.border.kind != fui::PaintKind::None && style.borderWidth > 0) {
    target.stroke(rect, style.border, style.borderWidth, style.radius, style.corners);
  }
}

void StorygameReaderActivity::drawChoiceButton(fui::DrawTarget& target, const fui::StyleSet& styles, const Rect& box,
                                               const bool selected) {
  paintBox(target, box, styles.resolve(selected ? fui::StateSelected : fui::StateNormal));
}

void StorygameReaderActivity::drawScrollBar(fui::DrawTarget& target, const Rect& area, const Column& column) {
  // A column that fits the viewport has no position to report, so it has no bar.
  if (column.height <= area.height) return;
  const int width = UITheme::getInstance().getMetrics().listScrollWidth;
  if (width <= 0) return;

  // The SDK indicator is written for lists, but its math is the ratio of visible
  // to total and of offset to range. Fed pixel counts it reports the scroll
  // position of a text column, so the bar reuses that geometry rather than
  // growing a second one beside it.
  fui::drawListScrollIndicator(
      target,
      fui::Rect{static_cast<int16_t>(area.x), static_cast<int16_t>(area.y), static_cast<int16_t>(area.width),
                static_cast<int16_t>(area.height)},
      static_cast<uint32_t>(column.height), static_cast<uint32_t>(area.height),
      static_cast<uint32_t>(column.scroll < 0 ? 0 : column.scroll), static_cast<int16_t>(width), 0, 0);
}

void StorygameReaderActivity::drawSheetButton(fui::DrawTarget& target, const Rect& area) {
  const Rect box = sheetButtonRect(area);
  const int right = box.x + box.width;
  const int top = box.y;
  const int bottom = box.y + box.height - 1;

  // The tab hangs on the panel edge, so only its left profile is drawn: a slanted
  // top, a vertical side and a slanted bottom. It takes no width from the text
  // column, so the paper fill covers the page's right edge under it -- the
  // reading-position bar and a choice button's outline there -- by design.
  renderer.fillRect(box.x, top + SHEET_BUTTON_TAPER, box.width, box.height - SHEET_BUTTON_TAPER * 2, false);
  for (int i = 1; i < SHEET_BUTTON_TAPER; ++i) {
    const int w = box.width * i / SHEET_BUTTON_TAPER;
    renderer.fillRect(right - w, top + i, w, 1, false);
    renderer.fillRect(right - w, bottom - i, w, 1, false);
  }
  renderer.drawLine(box.x, top + SHEET_BUTTON_TAPER, box.x, bottom - SHEET_BUTTON_TAPER, 1, true);
  renderer.drawLine(box.x, top + SHEET_BUTTON_TAPER, right - 1, top, 1, true);
  renderer.drawLine(box.x, bottom - SHEET_BUTTON_TAPER, right - 1, bottom, 1, true);

  target.bitmap(fui::Rect{static_cast<int16_t>(box.x), static_cast<int16_t>(box.y), static_cast<int16_t>(box.width),
                          static_cast<int16_t>(box.height)},
                fui::bitmapFromIcon(storygameIcon24()), fui::BitmapMode::Center);
}

void StorygameReaderActivity::drawColumn(fui::DrawTarget& target, const fui::StyleSet& styles, const Rect& area,
                                         const Column& column, const int fontId, const int lineHeight,
                                         const bool drawChoices, const int xOffset) {
  // Buttons are the background of their own text, so they are painted first.
  if (drawChoices) {
    for (size_t i = 0; i < column.choiceTop.size(); ++i) {
      const int top = area.y + column.choiceTop[i] - column.scroll;
      const int bottom = area.y + column.choiceBottom[i] - column.scroll;
      if (bottom <= area.y || top >= area.y + area.height) continue;
      drawChoiceButton(target, styles, Rect{area.x + CONTENT_PAD + xOffset, top, column.width, bottom - top},
                       static_cast<int>(i) == focusChoice);
    }
  }

  // A label is drawn in the paper colour only over a solid fill darker than paper.
  // A dithered or bitmap fill keeps black ink: white over a light pattern would
  // read worse than black. Deriving it from the fill, rather than from a colour
  // constant, keeps the label visible if a theme changes the selected background.
  const auto labelInk = [](const fui::BoxStyle& box) {
    return !(box.background.kind == fui::PaintKind::Solid && box.background.color != fui::Color::White);
  };

  for (const Line& line : column.lines) {
    const int top = area.y + line.y - column.scroll;
    if (top + line.height <= area.y || top >= area.y + area.height) continue;
    const bool inButton = drawChoices && line.choiceIndex >= 0;
    const bool ink =
        !inButton || labelInk(styles.resolve(line.choiceIndex == focusChoice ? fui::StateSelected : fui::StateNormal));

    if (line.heading) {
      int totalWidth = 0;
      for (const Span& span : line.spans) {
        wordBuf.assign(line.text, span.offset, span.length);
        totalWidth += renderer.getTextWidth(fontId, wordBuf.c_str(), toFontStyle(span.style));
      }
      // A heading centres in the column, except inside a choice, where centring
      // over the whole area would draw it outside its own button.
      const bool headingInButton = line.choiceIndex >= 0;
      const int left = area.x + xOffset + (headingInButton ? CONTENT_PAD : 0);
      const int spanWidth = headingInButton ? column.width : area.width;
      int x = left + (spanWidth - totalWidth) / 2;
      for (const Span& span : line.spans) {
        wordBuf.assign(line.text, span.offset, span.length);
        const EpdFontFamily::Style style = toFontStyle(span.style);
        const int w = renderer.getTextWidth(fontId, wordBuf.c_str(), style);
        renderer.drawText(fontId, x, top, wordBuf.c_str(), ink, style);
        if (span.style & qg::SG_UNDERLINE) {
          renderer.drawLine(x, top + lineHeight - 2, x + w, top + lineHeight - 2, 1, ink);
        }
        x += w;
      }
      continue;
    }

    int x = area.x + line.x + xOffset;
    if (line.bullet) {
      renderer.fillRect(x, top + (line.height - 5) / 2, 5, 5, ink);
      x += BULLET_INDENT;
    }
    for (size_t s = 0; s < line.spans.size(); ++s) {
      const Span& span = line.spans[s];
      wordBuf.assign(line.text, span.offset, span.length);
      const EpdFontFamily::Style style = toFontStyle(span.style);
      const int w = renderer.getTextWidth(fontId, wordBuf.c_str(), style);
      renderer.drawText(fontId, x, top, wordBuf.c_str(), ink, style);
      if (span.style & qg::SG_UNDERLINE) {
        renderer.drawLine(x, top + lineHeight - 2, x + w, top + lineHeight - 2, 1, ink);
      }
      x += w;
      // getTextWidth measures ink extents, so a separator space adds nothing to
      // it; the gap is advanced explicitly, matching the width the line writer
      // measured with the next word's style.
      if (s + 1 < line.spans.size()) {
        x += spaceAdvance(line.spans[s + 1].style);
      }
    }
  }
}

void StorygameReaderActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const Rect area = UITheme::getInstance().getScreenSafeArea(renderer, /*front*/ true, /*side*/ false);
  const int fontId = bodyFontId();
  const int lh = renderer.getLineHeight(fontId);

  fui::GfxRendererTarget target(renderer);

  // The render task holds the render lock (ActivityManager acquired it before
  // calling render). A column can still be scrolled past its end by a focused
  // choice or by a viewport that grew with a font change, so both offsets are
  // clamped to the area actually drawn, through the same rule the loop task uses.
  if (failed) {
    renderer.setClipRect(area.x, area.y, area.width, area.height);
    renderer.drawCenteredText(fontId, area.y + area.height / 2, tr(STR_QUESTBOOK_READ_FAILED));
  } else if (view == View::Sheet) {
    const bool revealing = revealStep < SHEET_REVEAL_STEPS;
    clampScroll(paragraph, area.height);
    clampScroll(sheet, area.height);
    renderer.setClipRect(area.x, area.y, area.width, area.height);
    // The sheet wipes in over the paragraph, so the paragraph stays on screen
    // behind the part the sheet has not covered yet.
    if (revealing) drawColumn(target, choiceButtonStyles, area, paragraph, fontId, lh, /*drawChoices=*/true);
    const int covered = revealing ? area.width * revealStep / SHEET_REVEAL_STEPS : area.width;
    if (covered > 0) {
      const int x = area.x + area.width - covered;
      renderer.setClipRect(x, area.y, covered, area.height);
      renderer.fillRect(x, area.y, covered, area.height, false);
      // The sheet's own left edge leads the wipe, so its content slides with the
      // panel instead of being uncovered from the right.
      drawColumn(target, choiceButtonStyles, area, sheet, fontId, lh, /*drawChoices=*/false, area.width - covered);
      // While the sheet is only part-way in, its leading edge is a rule: without
      // it the half-covered page reads as text simply stopping mid-screen.
      if (revealing) renderer.drawLine(x, area.y, x, area.y + area.height - 1, 1, true);
    }
    renderer.setClipRect(area.x, area.y, area.width, area.height);
    if (!revealing) drawScrollBar(target, area, sheet);
  } else {
    clampScroll(paragraph, area.height);
    renderer.setClipRect(area.x, area.y, area.width, area.height);
    drawColumn(target, choiceButtonStyles, area, paragraph, fontId, lh, /*drawChoices=*/true);
    drawScrollBar(target, area, paragraph);
    if (sheetButtonVisible()) {
      // The tab reaches the panel edge, which the content clip does not cover.
      renderer.setClipRect(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight());
      drawSheetButton(target, area);
    }
  }

  renderer.setClipRect(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight());
  // The sheet button is a touch affordance, so its button-device equivalent is
  // the left direction; label it only where it does something. The hint slot is
  // narrow (about 72px per line on the default theme), which "Character sheet"
  // overshoots, so this control carries its own short label.
  const char* sheetLabel = view == View::Paragraph && charsheetAvailable ? tr(STR_QUESTBOOK_SHEET) : "";
  const auto labels = mappedInput.mapDirectionalLabels(tr(STR_BACK), tr(STR_SELECT), sheetLabel, "", tr(STR_DIR_UP),
                                                       tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
  // The panel now shows the game: loop() may write its recent-books entry.
  painted.store(true, std::memory_order_release);
}
