#pragma once

#include <FreeInkUICore.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "StorygameGame.h"

class GfxRenderer;
class MappedInputManager;
struct Rect;

// The storygame reader: one paragraph of text and its choices laid out as one
// scrollable column, and the character sheet over it (ARCHITECTURE.md §5.2,
// §6.4). The sheet opens on a left swipe, or on the floating button the touch
// boards get at the right edge, or on the left direction where there is no
// touch. Swipes scroll without moving the selected choice; the up/down
// directions move the selection when the paragraph offers one and scroll the
// text when it does not; Confirm activates the selection. This is a
// self-contained Activity, not a ReaderActivity: pages, sessions and the remote
// page-turner do not apply to a choice-based game.
//
// Lines own exactly one copy of their text (`text` holds the whole line, `spans`
// are styled slices into it), so a long paragraph costs its own length once, not
// once per word. A hyphen the layout inserted at a line end is part of the line
// text too, so the measured line and the drawn line stay identical. The paragraph
// and the character sheet are two Columns of the same type drawn by the same code:
// keeping them apart lets the sheet wipe in over the paragraph still visible
// behind it, and returning to the paragraph keeps the reading position instead of
// rebuilding it.
//
// That costs two resident columns, not one: while the sheet is open the
// paragraph is still on screen behind the wipe, so it cannot be released. The
// old single-column code cleared its lines before building either one, so this
// is the price of the reveal and of keeping the reading position. Each column's
// line array keeps its capacity across rebuilds, so after the first paragraph
// and the first sheet opening a rebuild reuses the same block instead of
// reallocating.
//
// Both columns are rebuilt on the loop task under RenderLock and read by the
// render task under the same lock. Render also clamps each column's scroll
// offset to the area it measured, so a viewport that shrank (a font change)
// cannot leave the reader scrolled past the end.
class StorygameReaderActivity final : public Activity {
 public:
  StorygameReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string indexName);
  static std::unique_ptr<StorygameReaderActivity> create(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                         const std::string& indexName);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Span {
    uint16_t offset = 0;
    uint16_t length = 0;
    uint8_t style = 0;
  };
  struct Line {
    int y = 0;      // content-space top, relative to the content area's top
    int x = 0;      // content-space left, relative to the content area's left
    int height = 0;
    // Index of the choice button this line belongs to, or -1 for paragraph
    // text. The focused button draws its lines inverted, so the line has to
    // know its button without re-deriving it from geometry.
    int16_t choiceIndex = -1;
    bool bullet = false;
    bool heading = false;
    std::string text;     // the whole line, spans index into it
    std::vector<Span> spans;
  };
  // One accepted hyphenation break for a word that does not fit the pending
  // line: the bytes of the word that stay on this line and the width they take,
  // hyphen included.
  struct HyphenBreak {
    size_t bytes = 0;
    int width = 0;
    // A visible '-' is appended only when the break is a pattern break. A word
    // with its own hyphen breaks after that hyphen, which the prefix keeps.
    bool insertHyphen = false;
  };
  // One scrollable column of laid-out lines.
  struct Column {
    std::vector<Line> lines;
    int width = 0;   // content-space width the lines were laid out for
    int height = 0;  // content-space height, including the bottom padding
    int scroll = 0;  // content-space pixels scrolled off the top
    // Content-space button boxes, one per choice the column holds. They live
    // here so the code that draws a column paints the boxes of that column.
    std::vector<int> choiceTop;
    std::vector<int> choiceBottom;
  };
  // `View::Sheet` always comes with `1 <= revealStep <= SHEET_REVEAL_STEPS`: the
  // wipe is finished and input is live only at the top of that range.
  enum class View : uint8_t { Paragraph, Sheet };

  bool loadGame();
  void rebuildParagraphLayout();
  void rebuildSheetLayout();

  // Line writer: runs are accumulated into the column's pending line and
  // flushed into `writer->lines`.
  void resetWriter(Column& column, int y, int x, int width, int lh);
  void appendRun(const questbook_game::SgRun& run);
  void appendStyledText(const std::string& text, uint8_t style);
  void appendWord(uint8_t style, const std::string& word);
  // Appends one already-measured piece of a word to the pending line. A word is
  // appended in one piece unless it is hyphenated at the end of a line.
  void appendPiece(uint8_t style, const std::string& piece, int pieceWidth, int spaceWidth);
  // The widest hyphenation break of `word` whose prefix still fits the pending
  // line with its hyphen, or a `bytes == 0` break when none does. Pure reads of
  // the pending line and the font; the caller does the splitting.
  HyphenBreak hyphenSplit(const std::string& word, uint8_t style, int spaceWidth) const;
  void flushLine();
  void breakLine();

  // Input helpers.
  bool handleParagraphInput();
  bool handleCharsheetInput();
  void activateVisibleChoice(size_t index);
  void revealFocusedChoice();
  int choiceIndexAt(int screenX, int screenY);
  void openCharsheet();
  void closeCharsheet();
  // Moves the sheet's wipe one position closer when its interval has elapsed.
  void advanceSheetReveal();
  // True while any of the reader's buttons is still down, used to keep a release
  // from before the sheet opened out of the sheet.
  bool sheetButtonHeld() const;
  // Applies a content-space scroll delta to `column` and clamps it to its range.
  void scrollColumn(Column& column, int delta);
  // The one clamp rule for a column's scroll offset, against the viewport height
  // it is drawn in. The loop task and the render task both call it, the latter
  // because only render knows the area a font change produced.
  static void clampScroll(Column& column, int viewportHeight);
  // One page of `column`, as the content-space delta to add to its scroll,
  // measured on the laid-out lines rather than on a fraction of the screen:
  // forward, the page starts again at the last line it showed whole; backward,
  // at a line top above it. Both directions land on a line boundary.
  int pageStep(const Column& column, bool forward) const;
  int bodyFontId() const;
  int viewportHeight() const;
  // Content-space width of a text column: the page inset on the left, the same
  // inset plus the theme's scroll-indicator thickness on the right, where the bar
  // is drawn. The floating sheet tab is painted over the page and takes none of
  // this width.
  int textColumnWidth() const;
  // Width of the gap between two words; shared by layout and rendering so the
  // measured line and the drawn line stay identical.
  int spaceAdvance(uint8_t style) const;

  // Render helpers. `drawColumn` paints the lines already clipped by the caller
  // and, for the paragraph, the choice buttons behind them. `styles` is the
  // shared outlined action-button style, so a choice looks like the game page's
  // own buttons. `xOffset` shifts the whole column sideways: the sheet slides in
  // from the right edge with its leading edge first, so its content has to move
  // with the panel it is drawn on.
  void drawColumn(freeink::ui::DrawTarget& target, const freeink::ui::StyleSet& styles, const Rect& area,
                  const Column& column, int fontId, int lineHeight, bool drawChoices, int xOffset = 0);
  void drawChoiceButton(freeink::ui::DrawTarget& target, const freeink::ui::StyleSet& styles, const Rect& box,
                        bool selected);
  void drawScrollBar(freeink::ui::DrawTarget& target, const Rect& area, const Column& column);
  // The character-sheet tab: drawn from the renderer's own primitives because its
  // shape is a polygon (slanted top and bottom), which no style can express.
  void drawSheetButton(freeink::ui::DrawTarget& target, const Rect& area);
  // Fills and outlines one box from a resolved style: the two paint calls a
  // button face is, shared by the choice buttons and the floating button.
  static void paintBox(freeink::ui::DrawTarget& target, const Rect& box, const freeink::ui::BoxStyle& style);
  Rect sheetButtonRect(const Rect& area) const;
  // The floating sheet button is a touch affordance, and it exists only while
  // the character sheet has something to show.
  bool sheetButtonVisible() const;
  // The same conditions without the view, for callers that need to know whether
  // the affordance exists rather than whether it is on screen.
  bool sheetButtonEnabled() const;
  Rect sheetButtonHitRect(const Rect& area) const;

  std::string indexName;
  questbook_game::StorygameGame game;
  questbook_game::ParagraphSource source;
  bool failed = false;
  View view = View::Paragraph;
  // Cached engine query for the tab and the hint label: it only changes when a
  // choice rebuilds the paragraph.
  bool charsheetAvailable = false;
  // How much of the sheet is on screen: 0 = none, SHEET_REVEAL_STEPS = all.
  // The wipe is driven by advanceSheetReveal() from loop(), one position per
  // SHEET_REVEAL_STEP_MS.
  int revealStep = 0;
  uint32_t revealNextStepAt = 0;
  // False until the finished sheet has seen every button released once, so a
  // press made before the sheet opened cannot close it on release.
  bool sheetInputArmed = true;

  Column paragraph;
  Column sheet;
  // Outlined action-button style shared with the game page, built in onEnter().
  // A StyleSet is ~340 bytes, so it lives here rather than on the render stack.
  freeink::ui::StyleSet choiceButtonStyles;

  // Writer state owned here so a line is built in one place. Hyphenation adds one
  // lookup and one short prefix string per line-ending word, not per word.
  Column* writer = nullptr;
  int writerY = 0;
  int writerX = 0;
  int writerWidth = 0;
  int writerLh = 0;
  Line writerLine;
  int writerLineWidth = 0;
  int16_t writerChoice = -1;
  bool writerBullet = false;
  // Set by the layout that knows what it is writing: the paragraph's prose is
  // hyphenated, a choice's label and the character sheet's rows are not. Reset
  // by resetWriter(), so only the paragraph body turns it on.
  bool writerHyphenate = false;
  std::string wordBuf;

  int focusChoice = 0;  // index into the engine's visible choices
};
