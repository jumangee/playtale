#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "StorygameGame.h"

class GfxRenderer;
class MappedInputManager;

// The storygame reader: one paragraph of text and its choices laid out as one
// scrollable column, the character sheet on a left swipe (ARCHITECTURE.md §5.2,
// §6.4). Swipes scroll without moving the selected choice; buttons move the
// selection; Confirm activates it. This is a self-contained Activity, not a
// ReaderActivity: pages, sessions and the remote page-turner do not apply to a
// choice-based game.
//
// Lines own exactly one copy of their text (`text` holds the whole line, `spans`
// are styled slices into it), so a long paragraph costs its own length once, not
// once per word. `lines` is rebuilt on the loop task under RenderLock and read
// by the render task under the same lock.
class StorygameReaderActivity final : public Activity {
 public:
  StorygameReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string indexName);
  static std::unique_ptr<StorygameReaderActivity> create(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                         const std::string& indexName);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  bool loadGame();
  void rebuildLayout();
  void buildCharsheetLayout();

  // Line writer: runs are accumulated into `writerLine` and flushed into `lines`.
  void resetWriter(int y, int x, int width, int lh);
  void appendRun(const questbook_game::SgRun& run);
  void appendStyledText(const std::string& text, uint8_t style);
  void appendWord(uint8_t style, const std::string& word);
  void flushLine();
  void breakLine();

  // Input helpers.
  bool handleParagraphInput();
  bool handleCharsheetInput();
  void activateVisibleChoice(size_t index);
  void revealFocusedChoice();
  int visibleChoiceIndexAtY(int screenY);
  int bodyFontId() const;
  int viewportHeight() const;
  // Width of the gap between two words; shared by layout and rendering so the
  // measured line and the drawn line stay identical.
  int spaceAdvance(uint8_t style) const;

  std::string indexName;
  questbook_game::StorygameGame game;
  questbook_game::ParagraphSource source;
  bool failed = false;
  bool showingCharsheet = false;

  // Layout state (rebuilt per paragraph; drawn by scrolling a viewport).
  struct Span {
    uint16_t offset = 0;
    uint16_t length = 0;
    uint8_t style = 0;
  };
  struct Line {
    int y = 0;      // content-space top, relative to the content area's top
    int x = 0;      // content-space left, relative to the content area's left
    int height = 0;
    bool bullet = false;
    bool heading = false;
    bool choice = false;
    std::string text;     // the whole line, spans index into it
    std::vector<Span> spans;
  };
  std::vector<Line> lines;
  std::vector<int> choiceTop;     // content-space first-line top per visible choice
  std::vector<int> choiceBottom;  // content-space end per visible choice
  int contentHeight = 0;

  // Writer state owned here so layout runs without heap churn.
  int writerY = 0;
  int writerX = 0;
  int writerWidth = 0;
  int writerLh = 0;
  Line writerLine;
  int writerLineWidth = 0;
  bool writerChoice = false;
  bool writerBullet = false;
  std::string wordBuf;

  int scroll = 0;        // content-space pixels scrolled off the top
  int focusChoice = 0;   // index into the engine's visible choices
};