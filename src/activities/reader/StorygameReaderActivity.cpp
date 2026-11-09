#include "StorygameReaderActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace qg = questbook_game;

namespace {

constexpr int CONTENT_PAD = 8;
constexpr int SCROLL_STRIP = 4;
constexpr int TEXT_GAP = 10;
constexpr int CHOICE_GAP = 4;
constexpr int CHOICE_GAP_BEFORE = 12;
constexpr int CHOICE_INDENT = 16;

EpdFontFamily::Style toFontStyle(const uint8_t sg) {
  uint8_t style = 0;
  if (sg & qg::SG_BOLD) style |= static_cast<uint8_t>(EpdFontFamily::BOLD);
  if (sg & qg::SG_ITALIC) style |= static_cast<uint8_t>(EpdFontFamily::ITALIC);
  if (sg & qg::SG_UNDERLINE) style |= static_cast<uint8_t>(EpdFontFamily::UNDERLINE);
  if (sg & qg::SG_SUP) style |= static_cast<uint8_t>(EpdFontFamily::SUP);
  return static_cast<EpdFontFamily::Style>(style);
}

}  // namespace

StorygameReaderActivity::StorygameReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 std::string indexName)
    : Activity("StorygameReader", renderer, mappedInput), indexName(std::move(indexName)) {
  source = game.paragraphSource();
}

std::unique_ptr<StorygameReaderActivity> StorygameReaderActivity::create(GfxRenderer& renderer,
                                                                         MappedInputManager& mappedInput,
                                                                         const std::string& indexName) {
  auto activity = makeUniqueNoThrow<StorygameReaderActivity>(renderer, mappedInput, indexName);
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

void StorygameReaderActivity::onEnter() {
  Activity::onEnter();
  failed = !loadGame();
  scroll = 0;
  focusChoice = 0;
  requestUpdate();
}

bool StorygameReaderActivity::loadGame() {
  const std::string dir = qg::directory(indexName);
  if (dir.empty()) return false;
  if (!game.load(dir)) return false;
  source = game.paragraphSource();
  if (!game.engine().startGame(source)) return false;
  rebuildLayout();
  return !game.engine().failed();
}

// --- line writer -------------------------------------------------------------

void StorygameReaderActivity::resetWriter(const int y, const int x, const int width, const int lh) {
  writerY = y;
  writerX = x;
  writerWidth = width;
  writerLh = lh;
  writerLine = Line{};
  writerLineWidth = 0;
  writerChoice = false;
  writerBullet = false;
}

void StorygameReaderActivity::flushLine() {
  if (!writerLine.text.empty() || writerLine.heading || writerLine.bullet) {
    writerLine.y = writerY;
    writerLine.x = writerX;
    writerLine.height = writerLh;
    writerLine.choice = writerChoice;
    writerLine.bullet = writerBullet;
    lines.push_back(std::move(writerLine));
    writerY += writerLh;
  }
  writerLine = Line{};
  writerLineWidth = 0;
  writerBullet = false;
}

void StorygameReaderActivity::breakLine() { flushLine(); }

void StorygameReaderActivity::appendWord(const uint8_t style, const std::string& word) {
  if (word.empty()) return;
  wordBuf = word;
  const EpdFontFamily::Style fontStyle = toFontStyle(style);
  const int wordWidth = renderer.getTextWidth(bodyFontId(), wordBuf.c_str(), fontStyle);
  const int spaceWidth = spaceAdvance(style);

  if (!writerLine.spans.empty() && writerLineWidth + spaceWidth + wordWidth > writerWidth) {
    flushLine();
  }
  writerLine.y = writerY;  // begin the line at the writer's current slot
  writerLine.x = writerX;
  if (!writerLine.spans.empty()) {
    writerLine.text.push_back(' ');
    writerLineWidth += spaceWidth;
  }
  const uint16_t offset = static_cast<uint16_t>(writerLine.text.size());
  const uint16_t length = static_cast<uint16_t>(word.size());
  writerLine.spans.push_back(Span{offset, length, style});
  writerLine.text.append(word);
  writerLineWidth += wordWidth;
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
    headingLine.text = run.text;
    headingLine.spans.push_back(
        Span{0, static_cast<uint16_t>(run.text.size()), static_cast<uint8_t>(run.style | qg::SG_BOLD)});
    lines.push_back(std::move(headingLine));
    writerY += writerLh;
    return;
  }
  if (run.bullet) writerBullet = true;
  if (!run.text.empty()) appendStyledText(run.text, run.style);
}

// --- layout ------------------------------------------------------------------

void StorygameReaderActivity::rebuildLayout() {
  RenderLock lock(*this);  // owns `lines`; the render task reads it under the same lock

  lines.clear();
  choiceTop.clear();
  choiceBottom.clear();
  contentHeight = 0;

  if (showingCharsheet) {
    buildCharsheetLayout();
    return;
  }

  const int fontId = bodyFontId();
  const int lh = renderer.getLineHeight(fontId);
  const Rect area = UITheme::getInstance().getScreenSafeArea(renderer, /*front*/ true, /*side*/ false);
  const int textX = CONTENT_PAD;
  const int width = area.width - CONTENT_PAD * 2 - SCROLL_STRIP;

  int y = CONTENT_PAD;
  resetWriter(y, textX, width, lh);

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
      lines.push_back(std::move(spoilerLine));
      writerY += lh;
    }
    for (const qg::SgRun& run : block.runs) appendRun(run);
    breakLine();
    writerY += TEXT_GAP;
    resetWriter(writerY, textX, width, lh);
  }

  const auto& choices = game.engine().choices();
  if (!choices.empty()) {
    writerY += CHOICE_GAP_BEFORE;
    resetWriter(writerY, textX + CHOICE_INDENT, width - CHOICE_INDENT, lh);
    writerChoice = true;
    for (size_t i = 0; i < choices.size(); ++i) {
      const int top = writerY;
      for (const qg::SgRun& run : choices[i].runs) appendRun(run);
      breakLine();
      writerY += CHOICE_GAP;
      resetWriter(writerY, textX + CHOICE_INDENT, width - CHOICE_INDENT, lh);
      writerChoice = true;
      choiceTop.push_back(top);
      choiceBottom.push_back(writerY - CHOICE_GAP);
    }
    writerChoice = false;
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
    lines.push_back(std::move(endLine));
    writerY += lh;
  }

  contentHeight = writerY + CONTENT_PAD;
}

void StorygameReaderActivity::buildCharsheetLayout() {
  const int fontId = bodyFontId();
  const int lh = renderer.getLineHeight(fontId);
  const Rect area = UITheme::getInstance().getScreenSafeArea(renderer, /*front*/ true, /*side*/ false);
  const int x = CONTENT_PAD;
  const int width = area.width - CONTENT_PAD * 2 - SCROLL_STRIP;

  int y = CONTENT_PAD;
  resetWriter(y, x, width, lh);

  // Title.
  wordBuf = tr(STR_QUESTBOOK_CHARSHEET);
  writerLine.y = y;
  writerLine.x = x;
  writerLine.height = lh;
  writerLine.heading = true;
  writerLine.text = wordBuf;
  writerLine.spans.push_back(Span{0, static_cast<uint16_t>(wordBuf.size()), qg::SG_BOLD});
  lines.push_back(std::move(writerLine));
  writerY = y + lh;
  writerLine = Line{};

  std::vector<qg::Engine::CharsheetGroup> groups;
  game.engine().charsheet(groups);

  if (groups.empty()) {
    writerLine = Line{};
    const std::string noParams = tr(STR_QUESTBOOK_NO_PARAMS);
    appendStyledText(noParams, qg::SG_REGULAR);
    flushLine();
    contentHeight = writerY + CONTENT_PAD;
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
      lines.push_back(std::move(header));
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
  contentHeight = writerY + CONTENT_PAD;
}

// --- input -------------------------------------------------------------------

int StorygameReaderActivity::visibleChoiceIndexAtY(const int screenY) {
  RenderLock lock(*this);
  const Rect area = UITheme::getInstance().getScreenSafeArea(renderer, /*front*/ true, /*side*/ false);
  const int contentY = screenY - area.y + scroll;
  for (size_t i = 0; i < choiceTop.size(); ++i) {
    if (contentY >= choiceTop[i] && contentY < choiceBottom[i]) return static_cast<int>(i);
  }
  return -1;
}

void StorygameReaderActivity::revealFocusedChoice() {
  const auto& choices = game.engine().choices();
  if (choices.empty()) return;
  RenderLock lock(*this);
  if (focusChoice < 0) focusChoice = 0;
  if (static_cast<size_t>(focusChoice) >= choices.size()) focusChoice = static_cast<int>(choices.size()) - 1;
  if (focusChoice < 0 || focusChoice >= static_cast<int>(choiceTop.size())) return;

  const int vh = viewportHeight();
  const int top = choiceTop[focusChoice];
  const int bottom = choiceBottom[focusChoice];
  if (top < scroll) {
    scroll = top;
  } else if (bottom > scroll + vh) {
    scroll = bottom - vh;
  }
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
  {
    RenderLock lock(*this);
    scroll = 0;
    focusChoice = 0;
  }
  rebuildLayout();
  requestUpdate();
}

bool StorygameReaderActivity::handleParagraphInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return true;
  }

  const MappedInputManager::SwipeDir swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left) {
    {
      RenderLock lock(*this);
      showingCharsheet = true;
      scroll = 0;
    }
    rebuildLayout();
    requestUpdate();
    return true;
  }
  if (swipe == MappedInputManager::SwipeDir::Right) {
    finish();
    return true;
  }
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int vh = viewportHeight();
    int step = vh * 3 / 4;
    if (step < 40) step = 40;
    {
      RenderLock lock(*this);
      scroll += swipe == MappedInputManager::SwipeDir::Up ? step : -step;
      const int maxScroll = contentHeight - vh;
      if (scroll < 0) scroll = 0;
      if (scroll > maxScroll && maxScroll >= 0) scroll = maxScroll;
    }
    requestUpdate();
    return true;
  }

  int tapX = 0;
  int tapY = 0;
  if (mappedInput.wasScreenTapped(tapX, tapY)) {
    const int index = visibleChoiceIndexAtY(tapY);
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
  if (mappedInput.wasReleased(MappedInputManager::Button::ScreenUp)) {
    if (!choices.empty()) {
      {
        RenderLock lock(*this);
        --focusChoice;
        if (focusChoice < 0) focusChoice = static_cast<int>(choices.size()) - 1;
      }
      revealFocusedChoice();
      requestUpdate();
    }
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::ScreenDown)) {
    if (!choices.empty()) {
      {
        RenderLock lock(*this);
        focusChoice = (focusChoice + 1) % static_cast<int>(choices.size());
      }
      revealFocusedChoice();
      requestUpdate();
    }
    return true;
  }
  return false;
}

bool StorygameReaderActivity::handleCharsheetInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    {
      RenderLock lock(*this);
      showingCharsheet = false;
      scroll = 0;
    }
    rebuildLayout();
    requestUpdate();
    return true;
  }
  const MappedInputManager::SwipeDir swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Right) {
    {
      RenderLock lock(*this);
      showingCharsheet = false;
      scroll = 0;
    }
    rebuildLayout();
    requestUpdate();
    return true;
  }
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int vh = viewportHeight();
    int step = vh * 3 / 4;
    if (step < 40) step = 40;
    {
      RenderLock lock(*this);
      scroll += swipe == MappedInputManager::SwipeDir::Up ? step : -step;
      const int maxScroll = contentHeight - vh;
      if (scroll < 0) scroll = 0;
      if (scroll > maxScroll && maxScroll >= 0) scroll = maxScroll;
    }
    requestUpdate();
    return true;
  }
  return false;
}

void StorygameReaderActivity::loop() {
  if (failed) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
    return;
  }
  if (showingCharsheet) {
    handleCharsheetInput();
    return;
  }
  handleParagraphInput();
}

// --- render ------------------------------------------------------------------

void StorygameReaderActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const Rect area = UITheme::getInstance().getScreenSafeArea(renderer, /*front*/ true, /*side*/ false);
  const int fontId = bodyFontId();
  const int lh = renderer.getLineHeight(fontId);

  // The render task already holds the render lock (ActivityManager acquired it
  // before calling render), so `lines` and `scroll` cannot be mutated here.
  const int maxScroll = contentHeight - area.height;
  if (scroll < 0) scroll = 0;
  if (scroll > maxScroll && maxScroll >= 0) scroll = maxScroll;

  renderer.setClipRect(area.x, area.y, area.width, area.height);

  if (failed) {
    const int y = area.y + area.height / 2;
    renderer.drawCenteredText(fontId, y, tr(STR_QUESTBOOK_READ_FAILED));
  } else {
    for (const Line& line : lines) {
      const int top = area.y + line.y - scroll;
      if (top + line.height <= area.y || top >= area.y + area.height) continue;

      if (line.heading) {
        int totalWidth = 0;
        for (const Span& span : line.spans) {
          wordBuf.assign(line.text, span.offset, span.length);
          totalWidth += renderer.getTextWidth(fontId, wordBuf.c_str(), toFontStyle(span.style));
        }
        int x = area.x + (area.width - totalWidth) / 2;
        for (const Span& span : line.spans) {
          wordBuf.assign(line.text, span.offset, span.length);
          const EpdFontFamily::Style style = toFontStyle(span.style);
          const int w = renderer.getTextWidth(fontId, wordBuf.c_str(), style);
          renderer.drawText(fontId, x, top, wordBuf.c_str(), true, style);
          if (span.style & qg::SG_UNDERLINE) renderer.drawLine(x, top + lh - 2, x + w, top + lh - 2, 1, true);
          x += w;
        }
        continue;
      }

      int x = area.x + line.x;
      if (line.bullet) {
        renderer.fillRect(x, top + (line.height - 5) / 2, 5, 5, true);
        x += 8;
      }
      for (size_t s = 0; s < line.spans.size(); ++s) {
        const Span& span = line.spans[s];
        wordBuf.assign(line.text, span.offset, span.length);
        const EpdFontFamily::Style style = toFontStyle(span.style);
        const int w = renderer.getTextWidth(fontId, wordBuf.c_str(), style);
        renderer.drawText(fontId, x, top, wordBuf.c_str(), true, style);
        if (span.style & qg::SG_UNDERLINE) renderer.drawLine(x, top + lh - 2, x + w, top + lh - 2, 1, true);
        x += w;
        // getTextWidth measures ink extents, so a separator space adds nothing to
        // it; the gap is advanced explicitly, matching the width the line writer
        // measured with the next word's style.
        if (s + 1 < line.spans.size()) {
          x += spaceAdvance(line.spans[s + 1].style);
        }
      }
    }

    // The focus marker indexes the layout snapshot (choiceTop/choiceBottom are
    // 1:1 with the engine's visible choices at layout time), never the engine's
    // live choices vector, which the loop task mutates outside this task's lock.
    if (!showingCharsheet && !choiceTop.empty()) {
      if (focusChoice >= 0 && static_cast<size_t>(focusChoice) < choiceTop.size()) {
        const int top = area.y + choiceTop[focusChoice] - scroll;
        if (top >= area.y && top + lh <= area.y + area.height) {
          const int cx = area.x + 2;
          const int cy = top + lh / 2;
          renderer.drawLine(cx, cy - 6, cx + 7, cy, 2, true);
          renderer.drawLine(cx, cy + 6, cx + 7, cy, 2, true);
        }
      }
    }
  }

  renderer.setClipRect(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight());
  const auto labels = mappedInput.mapDirectionalLabels(tr(STR_BACK), tr(STR_SELECT), "", "", tr(STR_DIR_UP),
                                                       tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}