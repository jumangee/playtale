#include "StorygameDetailsActivity.h"

#include <GfxRenderer.h>
#include <HalMemory.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#include "MappedInputManager.h"
#include "QuestBookCover.h"
#include "components/CatalogScreens.h"
#include "components/UiAppHelpers.h"
#include "components/UITheme.h"
#include "network/HttpDownloader.h"

namespace fui = freeink::ui;

namespace {

// The one place the service's cover route is written down in the firmware.
constexpr char COVER_ENDPOINT[] = "https://quest-book.ru/online/cover/";

bool sessionUp() { return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0); }

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

  const auto heap = HalMemory::getDefaultHeap();
  if (heap.freeBytes < HttpDownloader::MIN_TLS_FREE_HEAP ||
      heap.largestBlockBytes < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("QBOOK", "Low heap for a cover fetch (%u free, %u largest block)", static_cast<unsigned>(heap.freeBytes),
            static_cast<unsigned>(heap.largestBlockBytes));
    return questbook_cover::FetchStatus::LowMemory;
  }
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

}  // namespace

StorygameDetailsActivity::StorygameDetailsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   std::string headerTitle, QuestBookListParser::Row game)
    : Activity("StorygameDetails", renderer, mappedInput),
      UiAppHost(renderer),
      headerTitle(std::move(headerTitle)),
      game(std::move(game)) {}

void StorygameDetailsActivity::onEnter() {
  Activity::onEnter();
  buildFacts();
  resetUi();
  app.setScreen(&StorygameDetailsActivity::detailsScreen, this);
  requestUpdate();
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
  props.metadata = asPtr(facts);
  props.metadataLines = 2;
  props.descriptionHeading = tr(STR_QUESTBOOK_ABOUT_GAME);
  props.description = asPtr(game.description);
  props.headingText = theme.bodyText;
  props.bodyText = theme.bodyText;
  props.primary.label = nullptr;
  props.more.label = nullptr;

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

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
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

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
  }
}