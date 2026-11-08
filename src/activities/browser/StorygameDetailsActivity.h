#pragma once

#include <Bitmap.h>

#include <atomic>
#include <string>

#include "QuestBookListParser.h"
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
class StorygameDetailsActivity final : public Activity, private UiAppHost {
 public:
  // `game` is copied: this page outlives nothing the catalogue frees while it
  // is stacked, but owning the row keeps the page independent of that rule.
  StorygameDetailsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string headerTitle,
                           QuestBookListParser::Row game);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  // The cover transfer runs inside loop() and resets the inactivity timer on
  // the pass that starts it; until that pass happens the page is not idle.
  bool preventAutoSleep() override { return coverPending; }

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
  // Cover drawing state, kept off the render task stack (Bitmap carries a
  // 256-entry palette). Bitmap borrows coverFile, so the order matters.
  HalFile coverFile;
  Bitmap coverBitmap{coverFile};
  // Kept off the render task stack: a CJK title's SD-font glyph load runs on
  // top of publicationPage().
  freeink::ui::PublicationPageProps pageProps{};
};
