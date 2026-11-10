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
  // "Rating 4/5 · Likes 6 · Dislikes 1", omitting facts the game does not have.
  void buildFacts();
  // Draws the resolved cover, or the storygame icon when there is none.
  static bool paintCover(freeink::ui::DrawTarget& target, freeink::ui::Rect rect,
                         const freeink::ui::PublicationHeaderProps& props, void* user);
  // Resolves and, when needed, fetches the cover. Blocks; called once per
  // visit. True when a path became available and a repaint is needed.
  bool loadCover();

  // --- the two actions -------------------------------------------------------
  static void onPlayEvent(const freeink::ui::ActionEvent&, void* user);
  static void onLibraryEvent(const freeink::ui::ActionEvent&, void* user);
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
  // Built once in onEnter: the facts line holds localized numbers and must not
  // be reformatted on every repaint.
  std::string facts;
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
  // The last failure, shown in the metadata line. Cleared by the next attempt,
  // so the page keeps one layout and the button stays usable for a retry.
  std::string errorText;
  questbook_game::Importer importer;

  // Cover drawing state, kept off the render task stack (Bitmap carries a
  // 256-entry palette). Bitmap borrows coverFile, so the order matters.
  HalFile coverFile;
  Bitmap coverBitmap{coverFile};
  // Kept off the render task stack: a CJK title's SD-font glyph load runs on
  // top of publicationPage().
  freeink::ui::PublicationPageProps pageProps{};
};