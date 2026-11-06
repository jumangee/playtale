#pragma once

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
// It draws no cover at all: cover caching is still an open decision (§8), and
// the hero gives the title the space instead.
class StorygameDetailsActivity final : public Activity, private UiAppHost {
 public:
  // `game` is copied: this page outlives nothing the catalogue frees while it
  // is stacked, but owning the row keeps the page independent of that rule.
  StorygameDetailsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string headerTitle,
                           QuestBookListParser::Row game);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static void detailsScreen(UiScreen& screen, void* user);
  void buildScreen(UiScreen& screen);
  // "Rating 4/5 · Likes 6 · Dislikes 1", omitting facts the game does not have.
  void buildFacts();

  std::string headerTitle;
  QuestBookListParser::Row game;
  // Built once in onEnter: the facts line holds localized numbers and must not
  // be reformatted on every repaint.
  std::string facts;
  // Kept off the render task stack: a CJK title's SD-font glyph load runs on
  // top of publicationPage().
  freeink::ui::PublicationPageProps pageProps{};
};
