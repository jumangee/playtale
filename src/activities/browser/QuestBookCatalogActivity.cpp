#include "QuestBookCatalogActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "MappedInputManager.h"
#include "activities/RenderLock.h"
#include "StorygameDetailsActivity.h"
#include "components/CatalogScreens.h"
#include "components/UITheme.h"
#include "network/HttpDownloader.h"

namespace fui = freeink::ui;

namespace {
// §2.1: the catalogue lives on one host and pages at a fixed twelve items.
constexpr char QUESTBOOK_API_BASE[] = "https://quest-book.ru/online/api/list/";
constexpr int MAX_PAGE = 9999;
}  // namespace

const QuestBookCatalogActivity::ListSpec QuestBookCatalogActivity::kLists[LIST_COUNT] = {
    {"new", StrId::STR_QUESTBOOK_LIST_NEW},
    {"editorial", StrId::STR_QUESTBOOK_LIST_EDITORIAL},
};

QuestBookCatalogActivity::QuestBookCatalogActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : CatalogActivity("QuestBookCatalog", renderer, mappedInput) {}

void QuestBookCatalogActivity::onEnter() {
  CatalogActivity::onEnter();
  showListPicker();
}

void QuestBookCatalogActivity::onExit() {
  // Rows go before the base tears Wi-Fi down and restarts the device. The
  // manager already holds the render lock here, so taking it would deadlock.
  parser.reset();
  releaseRows();
  CatalogActivity::onExit();
}

// The picker is a local screen: it needs neither Wi-Fi nor a request, so
// entering the catalogue costs no connection until a list is chosen.
void QuestBookCatalogActivity::showListPicker() {
  currentList = -1;
  page = 1;
  hasMore = false;
  {
    // The row buffer's labels point into the parser's rows: freeing them while
    // the render task may still be painting those pointers is a use-after-free.
    RenderLock lock(*this);
    parser.reset();
    releaseRows();
  }
  nav.reset();
  state = State::LIST_PICKER;
  requestUpdate();
}

void QuestBookCatalogActivity::startBrowse() {
  if (currentList < 0) {
    showListPicker();
    return;
  }
  beginLoading();
  fetchPage(1);
}

void QuestBookCatalogActivity::retryBrowse() {
  if (currentList < 0) {
    showListPicker();
    return;
  }
  beginLoading();
  fetchPage(page);
}

const QuestBookCatalogActivity::ListSpec& QuestBookCatalogActivity::list() const {
  return kLists[std::clamp(currentList, 0, LIST_COUNT - 1)];
}

std::string QuestBookCatalogActivity::pageUrl(const int pageNumber) const {
  std::string url = QUESTBOOK_API_BASE;
  url += list().segment;
  url += "/?page=";
  url += std::to_string(std::clamp(pageNumber, 1, MAX_PAGE));
  return url;
}

void QuestBookCatalogActivity::fetchPage(const int newPage) {
  page = newPage;
  if (!loadPage()) return;
  nav.reset();
  state = State::BROWSING;
  requestUpdate();
}

// Streams the response straight into the parser: nothing holds the document,
// so a page costs one SAX buffer plus the rows kept.
bool QuestBookCatalogActivity::loadPage() {
  // Free the previous page before measuring: its rows would otherwise be part
  // of the pre-flight reading, and the new page's rows are allocated during the
  // transfer.
  {
    RenderLock lock(*this);
    parser.reset();
    releaseRows();
  }
  // A TLS session needs one contiguous block for its record buffer, so the
  // free-heap floor alone does not prove the transfer can start.
  const auto heap = HalMemory::getDefaultHeap();
  if (heap.freeBytes < HttpDownloader::MIN_TLS_FREE_HEAP ||
      heap.largestBlockBytes < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("QBOOK", "Low heap for catalogue fetch (%u free, %u largest block)", static_cast<unsigned>(heap.freeBytes),
            static_cast<unsigned>(heap.largestBlockBytes));
    fail(StrId::STR_MEMORY_ERROR);
    return false;
  }
  const std::string url = pageUrl(page);
  const bool fetched = HttpDownloader::fetchUrl(url, [this](const uint8_t* data, const size_t len) {
    parser.feed(reinterpret_cast<const char*>(data), len);
    return true;  // a malformed page is reported after the transfer, not mid-body
  });
  if (!fetched) {
    LOG_ERR("QBOOK", "Catalogue fetch failed (HTTP %d): %s", HttpDownloader::lastStatus(), url.c_str());
    fail(StrId::STR_FETCH_FEED_FAILED);
    return false;
  }
  // A transfer resumed with a Range request would continue at a byte offset the
  // parser never saw; parseEnd() then rejects the document instead of listing
  // it, so an interrupted page is an error rather than a wrong page.
  if (!parser.parseEnd() || !parser.ok()) {
    LOG_ERR("QBOOK", "Catalogue page unusable (malformed %d, failed result %d): %s", parser.malformed(),
            parser.failed(), url.c_str());
    fail(StrId::STR_PARSE_FEED_FAILED);
    return false;
  }
  if (parser.rowsTruncated()) {
    LOG_ERR("QBOOK", "Catalogue page exceeded %u rows", static_cast<unsigned>(QuestBookListParser::MAX_ROWS));
  }
  // A truncated page is not a page whose end was reached: paging on from it
  // would skip the entries that were dropped.
  hasMore = parser.hasMore() && !parser.rowsTruncated();
  return true;
}

bool QuestBookCatalogActivity::prevRowVisible() const { return state == State::BROWSING && page > 1; }

bool QuestBookCatalogActivity::nextRowVisible() const {
  return state == State::BROWSING && hasMore && !games().empty();
}

int QuestBookCatalogActivity::listCount() const {
  if (state == State::LIST_PICKER) return LIST_COUNT;
  if (state != State::BROWSING) return 0;
  return static_cast<int>(games().size()) + (prevRowVisible() ? 1 : 0) + (nextRowVisible() ? 1 : 0);
}

void QuestBookCatalogActivity::activateIndex(const int index) {
  if (state == State::LIST_PICKER) {
    if (index < 0 || index >= LIST_COUNT) return;
    app.clearTapFlash();
    currentList = index;
    beginLoading();
    checkAndConnectWifi();
    return;
  }
  if (state != State::BROWSING) return;
  // The pager rows bracket the games: "Previous page" ahead of them past page
  // 1, "Next page" after them while more pages exist.
  const int gameIndex = index - (prevRowVisible() ? 1 : 0);
  if (prevRowVisible() && gameIndex == -1) {
    app.clearTapFlash();
    beginLoading();
    fetchPage(page - 1);
    return;
  }
  if (nextRowVisible() && gameIndex == static_cast<int>(games().size())) {
    app.clearTapFlash();
    beginLoading();
    fetchPage(page + 1);
    return;
  }
  openGame(gameIndex);
}

void QuestBookCatalogActivity::openGame(const int gameIndex) {
  if (gameIndex < 0 || gameIndex >= static_cast<int>(games().size())) return;
  auto details = makeUniqueNoThrow<StorygameDetailsActivity>(renderer, mappedInput,
                                                            I18N.get(StrId::STR_QUESTBOOK), games()[gameIndex]);
  if (!details) {
    LOG_ERR("QBOOK", "OOM: storygame page");
    fail(StrId::STR_MEMORY_ERROR);
    return;
  }
  app.clearTapFlash();
  // The page is pushed, not replaced: the catalogue keeps its page (a dozen
  // rows of short strings) and Back returns to it without a second fetch.
  startActivityForResult(std::move(details), [](const ActivityResult&) {});
}

// Back walks out one level at a time: from a list to the list picker, and from
// the picker out of the catalogue.
void QuestBookCatalogActivity::onBackButton() {
  if (state == State::LIST_PICKER || currentList < 0) {
    finish();
    return;
  }
  showListPicker();
}

void QuestBookCatalogActivity::buildScreen(UiScreen& screen) {
  // Fixed buffer: buildScreen runs on the render task, so the header label is
  // formatted in place instead of in a per-repaint std::string.
  char title[64];
  const char* label = I18N.get(state == State::BROWSING ? list().title : StrId::STR_QUESTBOOK);
  if (state == State::BROWSING && page > 1) {
    snprintf(title, sizeof(title), "%s %d", label, page);
  } else {
    snprintf(title, sizeof(title), "%s", label);
  }
  screenHeader(screen, title);
  if (buildStatusScreen(screen)) return;

  switch (state) {
    case State::BROWSING:
    case State::LIST_PICKER:
      buildBrowsingScreen(screen);
      return;
    default:
      return;
  }
}

void QuestBookCatalogActivity::buildBrowsingScreen(UiScreen& screen) {
  if (rowsDirty) {
    rebuildRowItems();
    rowsDirty = false;
  }
  if (rowItems.empty()) {
    screen.centeredText(tr(STR_NO_ENTRIES), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;  // air between the nav chevron and the row edge
  if (state == State::BROWSING) {
    // The author line under each game name.
    props.subtitleText = screen.theme().smallText;
    props.subtitleText.maxLines = 1;
  }
  syncListViewport(screen, props);
  screen.list(props);
}

// Labels point into the parser's rows and the static list table, both of which
// outlive the row buffer.
void QuestBookCatalogActivity::rebuildRowItems() {
  rowItems.clear();
  rowItems.reserve(listCount());
  const auto addRow = [&](const char* label, const char* subtitle = nullptr, const char* value = nullptr) {
    fui::ListItem row;
    row.label = label;
    row.subtitle = subtitle && subtitle[0] ? subtitle : nullptr;
    row.value = value && value[0] ? value : nullptr;
    row.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(row);
  };

  if (state == State::LIST_PICKER) {
    for (const ListSpec& spec : kLists) addRow(I18N.get(spec.title), nullptr, ">");
    return;
  }
  if (prevRowVisible()) addRow(tr(STR_PREV_PAGE), nullptr, ">");
  for (const Game& game : games()) addRow(game.title.c_str(), game.author.c_str(), ">");
  if (nextRowVisible()) addRow(tr(STR_NEXT_PAGE), nullptr, ">");
}

void QuestBookCatalogActivity::releaseRows() {
  // The app's interaction table holds row indices and hit rects for the old
  // rows; stop routing touches against it until the next render.
  closeRouting();
  rowsDirty = true;
}

void QuestBookCatalogActivity::drawFooter() {
  MappedInputManager::Labels labels;
  switch (state) {
    case State::BROWSING:
    case State::LIST_PICKER: {
      const int count = listCount();
      const char* confirm = count > 0 ? tr(STR_OPEN) : "";
      const char* up = count > 1 ? tr(STR_DIR_UP) : "";
      const char* down = count > 1 ? tr(STR_DIR_DOWN) : "";
      labels = mappedInput.mapLabels(tr(STR_BACK), confirm, up, down);
      break;
    }
    case State::ERROR:
      labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
      break;
    default:
      // CHECK_WIFI / LOADING / WIFI_SELECTION: the fetch blocks this activity,
      // and the Wi-Fi picker is a child that draws its own footer, so no hint
      // here can be acted on.
      labels = mappedInput.mapLabels("", "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
