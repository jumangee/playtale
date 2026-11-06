#pragma once
#include <I18n.h>

#include <string>
#include <vector>

#include "QuestBookListParser.h"
#include "activities/CatalogActivity.h"

// The КвестБук catalogue: the storygames listed on quest-book.ru, twelve per
// page, from the service's own JSON list endpoints (docs/storygames/
// ARCHITECTURE.md §2.1). A row opens the game's own page; this screen downloads
// nothing and keeps no state between visits.
//
// The two list kinds are the ones chosen for the device: `new` and `editorial`.
class QuestBookCatalogActivity final : public CatalogActivity {
 public:
  explicit QuestBookCatalogActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;

 private:
  // One service list: its path segment under /online/api/list/ and its label.
  struct ListSpec {
    const char* segment;
    StrId title;
  };
  static constexpr int LIST_COUNT = 2;
  static const ListSpec kLists[LIST_COUNT];

  using Game = QuestBookListParser::Row;

  // Parses one page and owns the rows it kept: the page's games are the
  // parser's own vector, so a page costs one buffer instead of two.
  QuestBookListParser parser;
  // Row buffer over the games plus the pager rows, rebuilt on the render task
  // whenever the page or the list changes.
  std::vector<freeink::ui::ListItem> rowItems;
  bool rowsDirty = true;
  int page = 1;
  bool hasMore = false;
  int currentList = -1;  // index into kLists; -1 = the list picker

  // --- CatalogActivity / UiListActivity contract -----------------------------
  int listCount() const override;
  void startBrowse() override;
  void retryBrowse() override;
  bool hasSearch() const override { return false; }  // §2.4: the JSON API has no search
  void performSearch(const std::string&) override {}
  void downloadFinished(bool) override {}
  void activateIndex(int index) override;
  void buildScreen(UiScreen& screen) override;
  void buildBrowsingScreen(UiScreen& screen);
  void drawFooter() override;
  void rebuildRowItems();
  void releaseRows();
  void onBackButton() override;

  const std::vector<Game>& games() const { return parser.rows(); }
  bool prevRowVisible() const;
  bool nextRowVisible() const;
  // The list being browsed. Undefined while currentList is unset.
  const ListSpec& list() const;
  // Absolute URL of one page of the selected list.
  std::string pageUrl(int pageNumber) const;
  void showListPicker();
  void fetchPage(int newPage);
  // Streams the response through the parser. False after failing the screen.
  bool loadPage();
  void openGame(int gameIndex);
};
