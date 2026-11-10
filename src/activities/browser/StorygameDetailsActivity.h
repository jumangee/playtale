#pragma once

#include <Bitmap.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "QuestBookListParser.h"
#include "StorygameImporter.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"

class GfxRenderer;
class MappedInputManager;

// One storygame's own page: the name, the author, the catalogue's tags, the
// service's rating and like counts, over the game's short description
// (docs/storygames/ARCHITECTURE.md §2.1, §6.3).
//
// Separate from DetailsActivity because a storygame page shows facts a book
// page does not (a rating out of five, a tag list, like counts) and shows none
// it does (availability, price, loan). Descriptions here are the service's
// plain text capped at 140 characters, so this page needs no HTML layout pass.
//
// The page is laid out by hand rather than by fui::publicationPage, because its
// two halves need different treatment than that component gives them:
//
//   hero band     cover on the left with the rating as five stars right under it;
//                 title, author and tags in the column beside it, then the like
//                 and dislike counts, one per line. Everything the page knows
//                 about the game sits in this band, which is what leaves the
//                 page free for the description.
//   description   a viewport under the "About this game" heading, clipped just
//                 above the two action buttons, which stay attached to the bottom
//                 edge of the content band. Text longer than the viewport scrolls
//                 inside it and is cut just above the buttons.
//
// The hero reserves a cover slot that the service fills with a 1-bit BMP: the
// text is drawn first, the cover is resolved afterwards in loop(), and the slot
// shows the storygame icon until — or instead of — an image. See
// lib/QuestBook/QuestBookCover.h.
//
// Two actions sit under the description. Play is the primary one; the second one
// follows whether the game is on the card, which is the only fact that decides it:
//
//   not downloaded  -> "Add to Library"  downloads and imports the game (§4.10)
//   downloaded      -> "Remove from Library"  asks first, then deletes its tree
//
// Play never creates a library item: a game that is not on the card is imported
// into Play's shared workspace /storygames/___temp/ and read from there (§6.3).
//
// The two buttons are the page's only choice, so the direction keys move a
// selection between them and Confirm runs the selected one, as the start screen
// and the reader's choices do. The selected button is drawn inverted; Play starts
// selected, so Confirm opens the game as it did before there was a selection.
//
// "Downloaded" is a filesystem question, answered on entry and after each
// action, so a page reopened over a game that was removed elsewhere is right
// without keeping state between visits.
class StorygameDetailsActivity final : public Activity, private UiAppHost {
 public:
  // Which surface opened this page. It decides where Home goes back to, because
  // ActivityManager::goHome() dispatches on the activity name, so the two origins
  // are distinct names.
  enum class Origin : uint8_t { Catalogue, Library };

  // `game` is copied: this page outlives nothing the catalogue frees while it
  // is stacked, but owning the row keeps the page independent of that rule.
  StorygameDetailsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string headerTitle,
                           QuestBookListParser::Row game, Origin origin = Origin::Catalogue);

  // The page for a game the library lists, where no catalogue row exists: the
  // title and author come from the game's own meta.json, and the rating and like
  // counts are simply absent (§4.9). Null only when the path is not a game or the
  // page cannot be allocated. `origin` is the surface the page is opened from.
  static std::unique_ptr<StorygameDetailsActivity> forLibrary(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                              const std::string& gameDir,
                                                              Origin origin = Origin::Library);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  // The cover transfer runs inside loop() and resets the inactivity timer on the
  // pass that starts it; until that pass happens, and while the game import
  // blocks the loop task, the page is not idle.
  bool preventAutoSleep() override { return coverPending || downloading; }

 private:
  static void detailsScreen(UiScreen& screen, void* user);
  void buildScreen(UiScreen& screen);
  // "Likes 6" and "Dislikes 1" as two lines, each kept only when the game has
  // that count. The rating is stars under the cover, so it is not part of these.
  void buildCounts();
  // Draws the resolved cover, or the storygame icon when there is none.
  void paintCover(freeink::ui::DrawTarget& target, freeink::ui::Rect rect);
  // Draws the rating as five stars at (x, y), each `side` pixels square: one
  // filled per point, outlines for the rest.
  void drawStars(freeink::ui::DrawTarget& target, int16_t x, int16_t y, int16_t side, int rating) const;
  // Resolves and, when needed, fetches the cover. Blocks; called once per
  // visit. True when a path became available and a repaint is needed.
  bool loadCover();

  // --- the two actions -------------------------------------------------------
  // Which button the direction keys have selected. The touch handlers move the
  // selection to the button that was tapped, so the page always paints the
  // action Confirm would run.
  enum class Selected : uint8_t { Play, Library };

  static void onPlayEvent(const freeink::ui::ActionEvent&, void* user);
  static void onLibraryEvent(const freeink::ui::ActionEvent&, void* user);
  // Runs the action the selection names. The one place that maps a selection to
  // a handler, so the keys and the touch handlers cannot diverge.
  void activate(Selected action);
  // Steps the selection to the other button.
  void stepSelection();
  // Scrolls the description viewport by one screenful: `direction` -1 up /
  // +1 down. True when there was text to scroll, so the caller can let a side
  // direction step the selection instead when the whole description fits.
  bool scrollDescription(int8_t direction);
  // Primary action. Opens the reader on the downloaded game; a game that is not
  // on the card is imported into Play's shared workspace first (§6.3), so playing
  // it does not add it to the library.
  void play();
  // Opens the storygame reader on top of this page, on the game in `gameDir`;
  // Back returns here.
  void openReader(const std::string& gameDir);
  // Second action, chosen by whether the game is on the card.
  void addToLibrary();
  void promptRemoveFromLibrary();
  void removeFromLibrary();
  // Downloads the converted document and imports it into `gameDir`: the
  // permanent /storygames/<index_name> for Add, or the shared workspace for Play.
  // Sets errorText on failure. True when a complete game is on the card.
  bool transferGame(const std::string& gameDir);
  // Copies the page's resolved cover to `<gameDir>/cover.bmp` when the game has
  // none there yet: a permanent copy is what the game page resolves first, and the
  // workspace copy is what the recent card reads.
  void storeCover(const std::string& gameDir);
  // Runs transferGame() for `gameDir` with the Wi-Fi and heap preconditions.
  // `openWhenDone` is Play's intent and travels with the call rather than in a
  // member, so a refused or failed attempt cannot leak it into a later Add.
  void startImport(const std::string& gameDir, bool openWhenDone);
  // Re-reads the card: a game is "downloaded" only when its whole tree is there.
  void refreshDownloaded();
  // Repoints the workspace card to the game's own directory and removes the
  // workspace copy, so the same game is not on the card twice.
  void retireTempCopy();
  questbook_game::Meta catalogueMeta() const;

  std::string headerTitle;
  QuestBookListParser::Row game;
  // Built once in onEnter: the count lines hold localized numbers and must not
  // be reformatted on every repaint.
  std::string likesText;
  std::string dislikesText;
  // Empty until a cover resolves: the slot then keeps the icon.
  std::string coverPath;
  bool coverPending = true;
  // Set at the end of render(): the page's text reaches the panel before the
  // transfer starts, so a slow fetch never delays the first frame.
  std::atomic<bool> painted{false};

  // Whether /storygames/<index_name>/ holds a complete game.
  bool downloaded = false;
  // True while the import runs, which also holds the page's second action off.
  bool downloading = false;
  // The button the direction keys have selected. Written by the loop task under
  // the render lock, read by buildScreen().
  Selected selected = Selected::Play;
  // The description viewport: where the scroll sits, and what the last paint
  // measured as scrollable and as one step. buildScreen() publishes all three on
  // the render task; the loop task reads and moves them under the same lock.
  int16_t descScroll = 0;
  int16_t descMaxScroll = 0;
  int16_t descPageStep = 0;
  // The two action buttons share one props object, held here rather than in
  // buildScreen's frame: a ButtonProps carries a whole StyleSet (~400 B), and a
  // CJK title's SD-font glyph load runs on top of that frame on the render task.
  // One object is enough because fui::button() only reads the props during its
  // call, so the second button overwrites the first's fields after it is drawn.
  freeink::ui::ButtonProps actionButton;
  // The last failure, shown in its own line under the hero. Cleared by the next
  // attempt, so the page keeps one layout and the button stays usable for a retry.
  std::string errorText;
  questbook_game::Importer importer;

  // Cover drawing state, kept off the render task stack (Bitmap carries a
  // 256-entry palette). Bitmap borrows coverFile, so the order matters.
  HalFile coverFile;
  Bitmap coverBitmap{coverFile};
};