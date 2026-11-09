#include "StorygameDetailsActivity.h"

#include <GfxRenderer.h>
#include <HalMemory.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <utility>

#include "MappedInputManager.h"
#include "QuestBookCover.h"
#include "StorygameStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/StorygameReaderActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/CatalogScreens.h"
#include "components/UiAppHelpers.h"
#include "components/UITheme.h"
#include "components/icons/delete32.h"
#include "components/icons/library.h"
#include "components/icons/play32.h"
#include "network/HttpDownloader.h"

namespace fui = freeink::ui;

namespace {

// The one place the service's cover route is written down in the firmware.
constexpr char COVER_ENDPOINT[] = "https://quest-book.ru/online/cover/";
// The converted game document (§4.3). Its path prefix is the catalogue's own
// host; the game is addressed by index_name, never by game_id (§2.5).
constexpr char GAME_ENDPOINT[] = "https://quest-book.ru/online/";

constexpr fui::ActionId ACTION_PLAY = 1;
constexpr fui::ActionId ACTION_LIBRARY = 2;

bool sessionUp() { return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0); }

// Both transfers this page makes need the same radio and the same TLS floor, so
// the pre-flight check lives here once. False is logged with the reading that
// refused it.
bool tlsSessionFits(const char* what) {
  const auto heap = HalMemory::getDefaultHeap();
  if (heap.freeBytes >= HttpDownloader::MIN_TLS_FREE_HEAP &&
      heap.largestBlockBytes >= HttpDownloader::MIN_TLS_MAX_ALLOC) {
    return true;
  }
  LOG_ERR("QBOOK", "Low heap for %s (%u free, %u largest block)", what, static_cast<unsigned>(heap.freeBytes),
          static_cast<unsigned>(heap.largestBlockBytes));
  return false;
}

// §4.3: GET /online/<index_name>/playtale/ answers with the converted document.
std::string gameEndpointUrl(const std::string& indexName) {
  std::string url = GAME_ENDPOINT;
  url += indexName;
  url += "/playtale/";
  return url;
}

// The slot the page reserves is also what the service is asked to render for.
std::string coverEndpointUrl(const std::string& indexName) {
  std::string url = COVER_ENDPOINT;
  url += indexName;
  url += "/?w=";
  url += std::to_string(questbook_cover::SLOT_WIDTH);
  url += "&h=";
  url += std::to_string(questbook_cover::SLOT_HEIGHT);
  return url;
}

// The transfer questbook_cover::ensure() asks for. It lives here because the
// HTTP client does, and so do the radio and heap preconditions: the cover
// library resolves its two local tiers without any of this.
questbook_cover::FetchStatus fetchCover(const std::string& url, const std::string& destPath) {
  if (!sessionUp()) return questbook_cover::FetchStatus::NoSession;
  if (!tlsSessionFits("a cover fetch")) return questbook_cover::FetchStatus::LowMemory;
  const auto status = HttpDownloader::downloadToFile(url, destPath);
  switch (status) {
    case HttpDownloader::OK:
      return questbook_cover::FetchStatus::Ok;
    case HttpDownloader::HTTP_ERROR:
      LOG_ERR("QBOOK", "Cover not served (HTTP %d)", HttpDownloader::lastStatus());
      return questbook_cover::FetchStatus::Refused;
    default:
      LOG_ERR("QBOOK", "Cover transfer failed: %d", static_cast<int>(status));
      return questbook_cover::FetchStatus::Failed;
  }
}

// Draws the cover centred in its slot at its natural size. The slot is 128x192
// on every supported panel, and the answer is never larger than that box, so
// this only scales on a layout that reserves a smaller slot; it is kept because
// the slot is a theme/SDK contract rather than a constant of this page.
bool drawCoverBitmap(GfxRenderer& renderer, Bitmap& bitmap, const fui::Rect rect) {
  const int width = bitmap.getWidth();
  const int height = bitmap.getHeight();
  if (width <= 0 || height <= 0 || rect.width <= 0 || rect.height <= 0) return false;

  float scale = 1.0f;
  if (width > rect.width) scale = static_cast<float>(rect.width) / static_cast<float>(width);
  if (height > rect.height) {
    const float fit = static_cast<float>(rect.height) / static_cast<float>(height);
    if (fit < scale) scale = fit;
  }
  // Same rounding as GfxRenderer::drawBitmap1Bit(), so the centred position
  // matches the pixels that call actually draws.
  const int drawnWidth = scale >= 1.0f ? width : static_cast<int>(std::floor((width - 1) * scale)) + 1;
  const int drawnHeight = scale >= 1.0f ? height : static_cast<int>(std::floor((height - 1) * scale)) + 1;
  const int x = rect.x + (rect.width - drawnWidth) / 2;
  const int y = rect.y + (rect.height - drawnHeight) / 2;
  return renderer.drawBitmap(bitmap, x, y, drawnWidth, drawnHeight);
}

// The 32-pixel icons the action buttons carry are the firmware's own 1-bpp
// assets (src/components/icons), which are pre-rotated for GfxRenderer::drawIcon
// rather than laid out row-major. icon_orientation::transpose (UiAppHelpers.h)
// owns that conversion and explains it; this page keeps three separate buffers
// because all three icons are drawn within one publicationPage() call, so they
// cannot share one scratch buffer.
constexpr int ICON_SIDE = 32;
constexpr int ICON_BYTES = ICON_SIDE * ICON_SIDE / 8;

// The assets are `static const uint8_t[]`, not `constexpr`, so they cannot be
// read inside a constant expression; the transposition therefore runs once, in
// onEnter(), before this page's first render and on the same task that later
// only reads the result.
using IconImage = std::array<uint8_t, ICON_BYTES>;
IconImage playIconImage{};
IconImage deleteIconImage{};
IconImage libraryIconImage{};
bool iconsReady = false;

void transposeIcon(const uint8_t (&asset)[ICON_BYTES], IconImage& out) {
  icon_orientation::transpose(asset, out.data(), ICON_SIDE);
}

// Called once per device, from onEnter().
void prepareIcons() {
  if (iconsReady) return;
  iconsReady = true;
  transposeIcon(PlayIcon32, playIconImage);
  transposeIcon(DeleteIcon32, deleteIconImage);
  transposeIcon(LibraryIcon, libraryIconImage);
}

fui::BitmapRef icon32(const IconImage& bits) {
  return fui::bitmapFromIcon(freeink::Icon({ICON_SIDE, ICON_SIDE, 16, bits.data()}));
}

}  // namespace

StorygameDetailsActivity::StorygameDetailsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   std::string headerTitle, QuestBookListParser::Row game,
                                                   const bool fromLibrary)
    : Activity(fromLibrary ? "StorygameDetailsLibrary" : "StorygameDetails", renderer, mappedInput),
      UiAppHost(renderer),
      headerTitle(std::move(headerTitle)),
      game(std::move(game)) {}

std::unique_ptr<StorygameDetailsActivity> StorygameDetailsActivity::forLibrary(GfxRenderer& renderer,
                                                                               MappedInputManager& mappedInput,
                                                                               const std::string& gameDir) {
  const std::string indexName = questbook_game::indexNameFromPath(gameDir);
  if (indexName.empty()) return nullptr;
  // A game that is no longer on the card still opens: the page then offers Add to
  // library again, which is the honest state after a removal from the file
  // browser. Only the folder name is left to show in that case.
  questbook_game::Meta meta;
  const bool haveMeta =
      questbook_game::readMetaFile(questbook_game::path(indexName, questbook_game::META_FILE_NAME), meta);
  QuestBookListParser::Row row;
  // Moved, not copied: the page is allocated right after this, and the two copies
  // would both be alive at that moment.
  row.gameId = std::move(meta.gameId);
  row.indexName = indexName;
  row.title = std::move(meta.title);
  if (row.title.empty()) row.title = indexName;
  row.author = std::move(meta.author);
  row.tags = std::move(meta.tags);
  row.description = std::move(meta.description);
  if (!haveMeta) LOG_ERR("QBOOK", "game %s has no readable meta.json", indexName.c_str());
  // The header names the section the page was opened from, as the catalogue does:
  // the game's own title is already the hero line right below it.
  return makeUniqueNoThrow<StorygameDetailsActivity>(renderer, mappedInput, tr(STR_LIBRARY), std::move(row),
                                                     /*fromLibrary=*/true);
}

void StorygameDetailsActivity::onEnter() {
  Activity::onEnter();
  buildFacts();
  refreshDownloaded();
  // Before the first render of this page: the button icons need one
  // transposition of the firmware's pre-rotated assets.
  prepareIcons();
  resetUi();
  app.on(ACTION_PLAY, &StorygameDetailsActivity::onPlayEvent, this);
  app.on(ACTION_LIBRARY, &StorygameDetailsActivity::onLibraryEvent, this);
  app.setScreen(&StorygameDetailsActivity::detailsScreen, this);
  requestUpdate();
}

void StorygameDetailsActivity::refreshDownloaded() {
  const bool present = questbook_game::isDownloaded(game.indexName);
  RenderLock lock;
  downloaded = present;
}

// The service reports 0 stars for a game nobody has rated, which is not the
// same claim as "rated zero"; that fact is omitted instead of shown as a zero.
void StorygameDetailsActivity::buildFacts() {
  facts.clear();
  const auto add = [this](const char* text) {
    if (!facts.empty()) facts += " \xC2\xB7 ";
    facts += text;
  };
  char buffer[48];
  if (game.stars > 0) {
    snprintf(buffer, sizeof(buffer), tr(STR_QUESTBOOK_RATING), game.stars, 5);
    add(buffer);
  }
  if (game.likes > 0) {
    snprintf(buffer, sizeof(buffer), tr(STR_QUESTBOOK_LIKES), game.likes);
    add(buffer);
  }
  if (game.dislikes > 0) {
    snprintf(buffer, sizeof(buffer), tr(STR_QUESTBOOK_DISLIKES), game.dislikes);
    add(buffer);
  }
}

void StorygameDetailsActivity::detailsScreen(UiScreen& screen, void* user) {
  static_cast<StorygameDetailsActivity*>(user)->buildScreen(screen);
}

// The cover is optional chrome: an unresolved, unreadable or partially drawn
// file leaves the slot's icon in place, never delays the first frame and never
// fails the page.
bool StorygameDetailsActivity::paintCover(fui::DrawTarget& target, const fui::Rect rect,
                                          const fui::PublicationHeaderProps&, void* user) {
  auto* self = static_cast<StorygameDetailsActivity*>(user);
  if (!self->coverPath.empty() && Storage.openFileForRead("QBOOK", self->coverPath, self->coverFile)) {
    if (self->coverBitmap.parseHeaders() == BmpReaderError::Ok &&
        drawCoverBitmap(self->renderer, self->coverBitmap, rect)) {
      self->coverFile.close();
      return true;
    }
    // A draw can fail after painting some rows, so the slot is wiped before the
    // icon: the icon only stamps ink, it cannot cover what is already there.
    LOG_ERR("QBOOK", "Unusable cover, falling back to the icon: %s", self->coverPath.c_str());
    target.fill(rect, fui::Paint::solid(fui::Color::White));
    self->coverFile.close();
  }
  target.bitmap(rect, listIconFor(UIIcon::Storygame, 32), fui::BitmapMode::Center);
  return true;
}

void StorygameDetailsActivity::buildScreen(UiScreen& screen) {
  catalogScreenHeader(screen, renderer, headerTitle.c_str());
  const auto& theme = screen.theme();
  const auto asPtr = [](const std::string& s) { return s.empty() ? nullptr : s.c_str(); };
  const int16_t line = uiTarget.lineHeight(theme.bodyText.font);

  fui::PublicationPageProps& props = pageProps;
  props.book.title = asPtr(game.title);
  props.book.author = asPtr(game.author);
  // The series slot is just a further hero line; the catalogue's tags sit there.
  props.book.series = asPtr(game.tags);
  props.book.titleText = theme.bodyText;
  props.book.detailText = theme.bodyText;
  props.book.titleLines = 3;
  props.book.authorLines = 1;
  props.book.seriesLines = 3;
  // The slot is always reserved, so the page has one layout whether or not the
  // image resolved; only the slot's contents change. paintCover() draws either
  // the cover or the storygame icon.
  props.book.coverSize = fui::Size{static_cast<int16_t>(questbook_cover::SLOT_WIDTH),
                                   static_cast<int16_t>(questbook_cover::SLOT_HEIGHT)};
  props.book.coverPainter = &StorygameDetailsActivity::paintCover;
  props.book.coverPainterUserData = this;

  const bool hasAuthor = !game.author.empty();
  const bool hasTags = !game.tags.empty();
  const int heroLines = props.book.titleLines + (hasAuthor ? props.book.authorLines : 0) +
                        (hasTags ? props.book.seriesLines : 0);
  const int16_t textHero = static_cast<int16_t>(
      heroLines * line + (hasAuthor ? 2 : 1) * props.book.titleGap + (hasTags ? props.book.textGap : 0));
  // The cover puts a floor under the hero. publicationPage() then clamps the
  // hero to min(heroHeight, max(budget, body.height / 2)) and the header caps
  // the cover at a third of the width, so a panel too short for the slot shrinks
  // the cover instead of pushing the description off the page.
  props.heroHeight = std::max<int16_t>(textHero, static_cast<int16_t>(questbook_cover::SLOT_HEIGHT));

  // Rating and like counts, not availability: a storygame is always available.
  // An import in flight and a failed attempt take the same line, so the page
  // never changes shape.
  if (downloading) {
    props.metadata = tr(STR_DOWNLOADING);
  } else if (!errorText.empty()) {
    props.metadata = errorText.c_str();
  } else {
    props.metadata = asPtr(facts);
  }
  props.metadataLines = 2;
  props.descriptionHeading = tr(STR_QUESTBOOK_ABOUT_GAME);
  props.description = asPtr(game.description);
  props.headingText = theme.bodyText;
  props.bodyText = theme.bodyText;

  // Same outlined themed button the OPDS publication page uses, so the two
  // detail screens read alike. The primary action is Play; the second one is
  // the library action the game's presence on the card decides.
  const auto styleButton = [&theme](fui::ButtonProps& button) {
    button.styles = theme.button;
    button.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    button.styles.normal.borderWidth = 1;
    button.text = theme.bodyText;
    button.radius = theme.controlRadius;
    button.minTouchSize = theme.minTouchSize;
  };
  props.primary.label = tr(STR_QUESTBOOK_PLAY);
  props.primary.icon = icon32(playIconImage);
  props.primary.action = ACTION_PLAY;
  props.primary.enabled = !downloading;
  styleButton(props.primary);

  props.secondary.label = downloaded ? tr(STR_QUESTBOOK_REMOVE_FROM_LIBRARY) : tr(STR_QUESTBOOK_ADD_TO_LIBRARY);
  props.secondary.icon = icon32(downloaded ? deleteIconImage : libraryIconImage);
  props.secondary.action = ACTION_LIBRARY;
  props.secondary.enabled = !downloading;
  styleButton(props.secondary);
  props.actionHeight = theme.rowHeight;

  fui::publicationPage(screen.frame(), screen.body(), props);
}

// Runs on the main task: the transfer blocks here, which is why it waits for the
// first frame. The two local tiers need no radio, so an offline page still shows
// a cover it already has.
bool StorygameDetailsActivity::loadCover() {
  std::string path;
  // Failures are logged where they are detected (lib/QuestBook/QuestBookCover.cpp
  // and fetchCover() above); an empty path is the one outcome this page acts on.
  questbook_cover::ensure(game.indexName, game.cover, coverEndpointUrl(game.indexName), &fetchCover, path);
  if (path.empty()) return false;
  {
    // render() reads this path while painting the slot.
    RenderLock lock;
    coverPath = std::move(path);
  }
  return true;
}

void StorygameDetailsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  renderUi();

  const char* libraryLabel = downloaded ? tr(STR_QUESTBOOK_REMOVE_FROM_LIBRARY) : tr(STR_QUESTBOOK_ADD_TO_LIBRARY);
  // Screen-relative labels, so a rotated or inverted screen paints the label on
  // the button that will run it: loop() checks the same Screen* buttons.
  const auto labels = mappedInput.mapDirectionalLabels(tr(STR_BACK), tr(STR_QUESTBOOK_PLAY), libraryLabel, "",
                                                       libraryLabel, "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
  // The panel now shows the text: loop() may start the cover transfer.
  painted.store(true, std::memory_order_release);
}

void StorygameDetailsActivity::loop() {
  const auto route = routeTouch(mappedInput);
  if (route.routed && app.invalidated()) requestUpdate();
  if (route) return;

  // One attempt per visit: a game with no cover keeps the icon until the page
  // is reopened, so a dead server cannot stall every repaint.
  if (coverPending && painted.load(std::memory_order_acquire)) {
    coverPending = false;
    if (loadCover()) requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (downloading) return;
  // Confirm is the primary action, as on any other detail page; the library
  // action sits on the screen's left and up directions, which is where render()
  // paints its label.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::ScreenRight) ||
      mappedInput.wasReleased(MappedInputManager::Button::ScreenDown)) {
    play();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::ScreenLeft) ||
      mappedInput.wasReleased(MappedInputManager::Button::ScreenUp)) {
    if (downloaded) {
      promptRemoveFromLibrary();
    } else {
      addToLibrary();
    }
  }
}

// --- actions -----------------------------------------------------------------

void StorygameDetailsActivity::onPlayEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<StorygameDetailsActivity*>(user);
  self->app.clearTapFlash();
  if (self->downloading) return;
  self->play();
}

void StorygameDetailsActivity::onLibraryEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<StorygameDetailsActivity*>(user);
  self->app.clearTapFlash();
  if (self->downloading) return;
  if (self->downloaded) {
    self->promptRemoveFromLibrary();
  } else {
    // The manual Add action never carries a play-after-download intent, even if
    // a previous Play-initiated download failed somewhere along the way.
    self->launchAfterDownload = false;
    self->addToLibrary();
  }
}

void StorygameDetailsActivity::play() {
  if (downloading) return;
  if (downloaded) {
    openReader();
    return;
  }
  // §6.3: playing a game that is not on the card downloads it first; the
  // transfer is the Add action's own, after which the reader opens.
  launchAfterDownload = true;
  addToLibrary();
}

void StorygameDetailsActivity::openReader() {
  auto reader = makeUniqueNoThrow<StorygameReaderActivity>(renderer, mappedInput, game.indexName);
  if (!reader) {
    LOG_ERR("QBOOK", "OOM: storygame reader");
    RenderLock lock;
    errorText = tr(STR_MEMORY_ERROR);
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(reader), [this](const ActivityResult&) {
    // Back from the reader returns to this page; the page re-reads the card on
    // entry, so a game removed elsewhere is not mistaken for a live one.
    refreshDownloaded();
    requestUpdate();
  });
}

questbook_game::Meta StorygameDetailsActivity::catalogueMeta() const {
  questbook_game::Meta meta;
  meta.gameId = game.gameId;
  meta.indexName = game.indexName;
  meta.title = game.title;
  meta.author = game.author;
  meta.tags = game.tags;
  meta.description = game.description;
  return meta;
}

void StorygameDetailsActivity::addToLibrary() {
  // The action is the page's own; a game that is already there is removed
  // instead, and the download that makes it present is this one.
  if (downloading || downloaded) return;
  if (!sessionUp()) {
    // The catalogue connected before it listed, so an idle page is the case
    // this covers: reopen the picker and continue when the radio is up.
    auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
    if (!wifi) {
      LOG_ERR("QBOOK", "OOM: Wi-Fi selection");
      RenderLock lock;
      errorText = tr(STR_MEMORY_ERROR);
      requestUpdate();
      return;
    }
    startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
      if (result.isCancelled) return;
      addToLibrary();
    });
    return;
  }

  // The session and its record buffer want one contiguous block, so the free
  // total alone does not prove the transfer can start.
  if (!tlsSessionFits("a game import")) {
    RenderLock lock;
    errorText = tr(STR_MEMORY_ERROR);
    requestUpdate();
    return;
  }

  {
    RenderLock lock;
    errorText.clear();
    downloading = true;
  }
  // The notification reaches the render task, which paints the import state
  // while this task is blocked inside the transfer.
  requestUpdate(true);

  const bool imported = transferGame();
  {
    RenderLock lock;
    downloading = false;
    if (imported) {
      downloaded = true;
    }
  }
  if (imported) {
    // The cover the page already resolved becomes the game's own copy, which is
    // the page's first choice next time and works with no cache (§6.3). A game
    // without a cover simply keeps its icon offline.
    if (!coverPath.empty() && !questbook_game::copyCover(game.indexName, coverPath)) {
      LOG_ERR("QBOOK", "game %s imported without its cover copy", game.indexName.c_str());
    }
    // The library lists a downloaded game as one row; the next Library visit
    // rebuilds its index rather than waiting for some other reason to (§4.9).
    library::markLibraryIndexDirty();
  }
  // The action's label and icon changed; the interaction table is rebuilt from
  // the next render, so close routing the way the remove path does.
  closeRouting();
  const bool launchNow = launchAfterDownload;
  launchAfterDownload = false;
  if (launchNow && imported) {
    openReader();
    return;
  }
  requestUpdate(true);
}

bool StorygameDetailsActivity::transferGame() {
  const std::string url = gameEndpointUrl(game.indexName);
  if (!importer.begin(game.indexName, catalogueMeta())) {
    LOG_ERR("QBOOK", "cannot start the import of %s", game.indexName.c_str());
    RenderLock lock;
    errorText = tr(STR_QUESTBOOK_IMPORT_FAILED);
    return false;
  }

  // The document streams straight into the importer: it is never held, and the
  // callback refuses a document the importer has already rejected so the
  // transfer stops instead of downloading the rest of a game we will delete.
  // This overload is a plain GET with no Range request, which the service would
  // ignore anyway (§2.5): a resumed body would only feed expat a second,
  // partial document.
  int httpStatus = 0;
  HttpDownloader::FetchOptions options;
  options.statusOut = &httpStatus;
  const bool fetched = HttpDownloader::fetchUrl(
      url, [this](const uint8_t* data, const size_t length) { return importer.feed(data, length); }, options);
  if (!fetched) {
    // The importer knows why it stopped; read that before discard() resets it.
    const auto reason = importer.status();
    importer.discard();
    LOG_ERR("QBOOK", "game transfer failed (HTTP %d, import %d): %s", httpStatus, static_cast<int>(reason),
            url.c_str());
    RenderLock lock;
    switch (reason) {
      case questbook_game::Importer::Status::Ok:
        // The importer never objected: the transport did.
        errorText = tr(STR_DOWNLOAD_FAILED);
        break;
      case questbook_game::Importer::Status::LowMemory:
        errorText = tr(STR_MEMORY_ERROR);
        break;
      default:
        errorText = tr(STR_QUESTBOOK_IMPORT_FAILED);
        break;
    }
    return false;
  }

  switch (importer.finish()) {
    case questbook_game::Importer::Status::Ok:
      LOG_INF("QBOOK", "imported %s", game.indexName.c_str());
      return true;
    case questbook_game::Importer::Status::LowMemory: {
      RenderLock lock;
      errorText = tr(STR_MEMORY_ERROR);
      return false;
    }
    case questbook_game::Importer::Status::Rejected:
    case questbook_game::Importer::Status::Incomplete:
    case questbook_game::Importer::Status::FileError: {
      // The service answers a wrong index_name with something else entirely
      // (§2.5), and a card that refused a write leaves no complete tree either.
      // Both end with no game on the card, so one message covers them.
      LOG_ERR("QBOOK", "game not imported: %s", url.c_str());
      RenderLock lock;
      errorText = tr(STR_QUESTBOOK_IMPORT_FAILED);
      return false;
    }
  }
  return false;
}

void StorygameDetailsActivity::promptRemoveFromLibrary() {
  if (downloading || !downloaded) return;
  // The dialog and the delete both want the card, and the page's cover file is
  // closed between repaints, so there is nothing to release before asking. The
  // caption is a question, as every other confirmation in the firmware is: the
  // button that opened it is labelled with the action, not with a question.
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(
      renderer, mappedInput, tr(STR_QUESTBOOK_REMOVE_CONFIRM), game.title);
  if (!confirmation) {
    LOG_ERR("QBOOK", "OOM: remove confirmation");
    RenderLock lock;
    errorText = tr(STR_MEMORY_ERROR);
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(confirmation), [this](const ActivityResult& result) {
    if (result.isCancelled) return;
    removeFromLibrary();
  });
}

void StorygameDetailsActivity::removeFromLibrary() {
  {
    // The render task paints this page's cover out of the game's own directory,
    // and this call unlinks that directory: without the lock a repaint already in
    // flight can be reading cover.bmp as it goes. The library's delete path takes
    // the same lock for the same reason.
    RenderLock lock;
    if (!questbook_game::removeGame(game.indexName)) {
      LOG_ERR("QBOOK", "cannot remove %s", game.indexName.c_str());
      errorText = tr(STR_QUESTBOOK_REMOVE_FAILED);
      requestUpdate();
      return;
    }
    downloaded = false;
    errorText.clear();
    // The stored cover lived in the directory that just went away. Keeping the
    // path would make a re-add on this same visit a no-op: copyCover() treats
    // source == destination as "nothing to do", so the new game would silently
    // lose its cover.
    coverPath.clear();
  }
  // The game is one row of the library index; removing it here must not leave that
  // row behind until something else happens to trigger a rebuild (§4.9).
  library::markLibraryIndexDirty();
  // The action's label changed, and the interaction table is rebuilt from the
  // next render; closing routing keeps a queued tap from landing on the old one.
  closeRouting();
  requestUpdate(true);
}