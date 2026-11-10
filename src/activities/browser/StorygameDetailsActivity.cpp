#include "StorygameDetailsActivity.h"

#include <GfxRenderer.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <utility>

#include "MappedInputManager.h"
#include "QuestBookCover.h"
#include "RecentBooksStore.h"
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
#include "components/icons/star-filled24.h"
#include "components/icons/star-outline24.h"
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
// owns that conversion and explains it; this page keeps one buffer per icon
// because all of them are drawn within one screen build, so they cannot share a
// scratch buffer.
constexpr int ICON_SIDE = 32;
constexpr int ICON_BYTES = ICON_SIDE * ICON_SIDE / 8;

// The rating is a row of stars, drawn from the same pre-rotated assets: the
// filled one for each point the game has, the outline for the rest of the five.
constexpr int STAR_SIDE = 24;
constexpr int STAR_GAP = 2;
constexpr int STAR_COUNT = 5;
// Below this a star stops reading as a star, so a cover column narrower than
// that many pixels loses the rating instead of drawing dots.
constexpr int STAR_MIN_SIDE = 14;

// The assets are `static const uint8_t[]`, not `constexpr`, so they cannot be
// read inside a constant expression; the transposition therefore runs once, in
// onEnter(), before this page's first render and on the same task that later
// only reads the result.
using IconImage = std::array<uint8_t, ICON_BYTES>;
using StarImage = std::array<uint8_t, STAR_SIDE * STAR_SIDE / 8>;
IconImage playIconImage{};
IconImage deleteIconImage{};
IconImage libraryIconImage{};
StarImage starOnImage{};
StarImage starOffImage{};
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
  icon_orientation::transpose(StarFilledIcon24, starOnImage.data(), STAR_SIDE);
  icon_orientation::transpose(StarOutlineIcon24, starOffImage.data(), STAR_SIDE);
}

fui::BitmapRef icon32(const IconImage& bits) {
  return fui::bitmapFromIcon(freeink::Icon({ICON_SIDE, ICON_SIDE, 16, bits.data()}));
}

fui::BitmapRef starIcon(const bool filled) {
  return fui::bitmapFromIcon(
      freeink::Icon({STAR_SIDE, STAR_SIDE, STAR_SIDE / 2, filled ? starOnImage.data() : starOffImage.data()}));
}

// The size one star is drawn at in a row `width` wide: the artwork's own 24 px
// when the slot has that much room, and a smaller square (still square, four
// gaps of STAR_GAP between them) when a narrow panel gives the cover less, so
// the rating never disappears on a board whose cover column is narrow.
int16_t starSideFor(const int16_t width) {
  const int16_t side = static_cast<int16_t>((width - (STAR_COUNT - 1) * STAR_GAP) / STAR_COUNT);
  return side < STAR_SIDE ? side : STAR_SIDE;
}

// Draws one wrapped block at the column's current line and returns the y of the
// line below it. `limit` is the band the block may not leave: a block that does
// not fit keeps only the lines that do, so the hero column can never run into
// the description that follows it.
int16_t drawTextBlock(fui::DrawTarget& target, const int16_t x, int16_t y, const int16_t limit, const int16_t width,
                      const char* text, fui::TextStyle style, const uint8_t maxLines, const int16_t gap) {
  if (!text || !*text || width <= 0 || y >= limit) return y;
  const int16_t line = target.lineHeight(style.font);
  if (line <= 0) return y;
  const int room = (limit - y) / line;
  if (room <= 0) return y;
  style.maxLines = room < maxLines ? static_cast<uint8_t>(room < 255 ? room : 255) : maxLines;
  if (style.maxLines == 0) return y;
  const fui::Size size = fui::measureWrappedText(target, text, style, width);
  if (size.height <= 0) return y;
  target.text(fui::Rect{x, y, width, size.height}, text, style);
  return static_cast<int16_t>(y + size.height + gap);
}

}  // namespace

StorygameDetailsActivity::StorygameDetailsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   std::string headerTitle, QuestBookListParser::Row game,
                                                   const Origin origin)
    : Activity(origin == Origin::Library ? "StorygameDetailsLibrary" : "StorygameDetails", renderer, mappedInput),
      UiAppHost(renderer),
      headerTitle(std::move(headerTitle)),
      game(std::move(game)) {}

std::unique_ptr<StorygameDetailsActivity> StorygameDetailsActivity::forLibrary(GfxRenderer& renderer,
                                                                               MappedInputManager& mappedInput,
                                                                               const std::string& gameDir,
                                                                               const Origin origin) {
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
  return makeUniqueNoThrow<StorygameDetailsActivity>(renderer, mappedInput, tr(STR_LIBRARY), std::move(row), origin);
}

void StorygameDetailsActivity::onEnter() {
  Activity::onEnter();
  buildCounts();
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

// The two count lines beside the cover: "Likes 6" and "Dislikes 1", each kept
// only when the game has that count, and shown as separate lines. The rating is
// stars under the cover now, so it is not part of these.
void StorygameDetailsActivity::buildCounts() {
  likesText.clear();
  dislikesText.clear();
  char buffer[32];
  if (game.likes > 0) {
    snprintf(buffer, sizeof(buffer), tr(STR_QUESTBOOK_LIKES), game.likes);
    likesText = buffer;
  }
  if (game.dislikes > 0) {
    snprintf(buffer, sizeof(buffer), tr(STR_QUESTBOOK_DISLIKES), game.dislikes);
    dislikesText = buffer;
  }
}

void StorygameDetailsActivity::detailsScreen(UiScreen& screen, void* user) {
  static_cast<StorygameDetailsActivity*>(user)->buildScreen(screen);
}

// The cover is optional chrome: an unresolved, unreadable or partially drawn
// file leaves the slot's icon in place, never delays the first frame and never
// fails the page.
void StorygameDetailsActivity::paintCover(fui::DrawTarget& target, const fui::Rect rect) {
  if (!coverPath.empty() && Storage.openFileForRead("QBOOK", coverPath, coverFile)) {
    if (coverBitmap.parseHeaders() == BmpReaderError::Ok && drawCoverBitmap(renderer, coverBitmap, rect)) {
      coverFile.close();
      return;
    }
    // A draw can fail after painting some rows, so the slot is wiped before the
    // icon: the icon only stamps ink, it cannot cover what is already there.
    LOG_ERR("QBOOK", "Unusable cover, falling back to the icon: %s", coverPath.c_str());
    target.fill(rect, fui::Paint::solid(fui::Color::White));
    coverFile.close();
  }
  target.bitmap(rect, listIconFor(UIIcon::Storygame, 32), fui::BitmapMode::Center);
}

// The rating as five stars: one filled per point, outlines for the rest, each
// `side` pixels square with STAR_GAP between them.
void StorygameDetailsActivity::drawStars(fui::DrawTarget& target, const int16_t x, const int16_t y, const int16_t side,
                                         const int rating) const {
  int filled = rating;
  if (filled < 0) filled = 0;
  if (filled > STAR_COUNT) filled = STAR_COUNT;
  for (int i = 0; i < STAR_COUNT; ++i) {
    const fui::Rect star{static_cast<int16_t>(x + i * (side + STAR_GAP)), y, side, side};
    target.bitmap(star, starIcon(i < filled), fui::BitmapMode::Contain);
  }
}

void StorygameDetailsActivity::buildScreen(UiScreen& screen) {
  catalogScreenHeader(screen, renderer, headerTitle.c_str());
  const auto& theme = screen.theme();
  fui::DrawTarget& target = screen.target();
  const auto asPtr = [](const std::string& s) { return s.empty() ? nullptr : s.c_str(); };
  const int16_t line = target.lineHeight(theme.bodyText.font);
  if (line <= 0) return;
  const fui::Rect body =
      screen.body().inset(fui::Insets{theme.spaceLg, theme.spaceLg, theme.spaceLg, theme.spaceLg});
  if (body.empty()) return;

  // The two actions keep the bottom edge of the content band: the description
  // viewport ends above them, so a long description scrolls behind the buttons
  // instead of pushing them off the page.
  const int16_t actionHeight = theme.rowHeight > theme.minTouchSize ? theme.rowHeight : theme.minTouchSize;
  const int16_t actionGap = theme.spaceSm;
  const int16_t actionsHeight = static_cast<int16_t>(2 * actionHeight + actionGap);
  const int16_t actionsTop = static_cast<int16_t>(body.bottom() - actionsHeight);
  const int16_t headingHeight = static_cast<int16_t>(line + theme.spaceSm);
  // What the hero (cover and its text column) may take once the description
  // heading and a few description lines are reserved.
  int16_t heroCap = static_cast<int16_t>(actionsTop - headingHeight - 3 * line - body.y);
  if (heroCap < 0) heroCap = 0;

  // An import in flight and a failed attempt take a full-width line under the
  // hero rather than a column line: both are sentences, and the column beside the
  // cover is too narrow for them. The column — and the cover with it — leaves that
  // line its room, and a normal page has no such line, so the space goes to the
  // description instead.
  const char* statusLine = downloading ? tr(STR_DOWNLOADING) : (!errorText.empty() ? errorText.c_str() : nullptr);
  const int16_t statusReserve = statusLine ? static_cast<int16_t>(2 * line + theme.spaceSm) : 0;
  const int16_t columnCap = heroCap > statusReserve ? static_cast<int16_t>(heroCap - statusReserve) : heroCap;
  const int16_t columnLimit = static_cast<int16_t>(body.y + columnCap);

  // The cover keeps the 128x192 slot the service renders for, shrunk only when
  // the band has to be: the page then still shows the whole description area.
  int16_t coverWidth = questbook_cover::SLOT_WIDTH;
  if (coverWidth > body.width / 3) coverWidth = static_cast<int16_t>(body.width / 3);
  int16_t coverHeight = static_cast<int16_t>(coverWidth * questbook_cover::SLOT_HEIGHT / questbook_cover::SLOT_WIDTH);
  if (coverHeight > columnCap) {
    // A band too short for the slot shrinks the cover rather than letting it run
    // into the status line or the description: with no band left the slot is
    // simply not drawn.
    coverHeight = columnCap;
    coverWidth = static_cast<int16_t>(coverHeight * questbook_cover::SLOT_WIDTH / questbook_cover::SLOT_HEIGHT);
  }
  const int16_t columnX = static_cast<int16_t>(body.x + coverWidth + theme.spaceLg);
  int16_t columnWidth = static_cast<int16_t>(body.width - coverWidth - theme.spaceLg);
  if (columnWidth < 0) columnWidth = 0;
  // A band with no room for the slot draws neither the image nor the fallback
  // icon, and must not open the file to find that out.
  if (coverWidth > 0 && coverHeight > 0) {
    paintCover(target, fui::Rect{body.x, body.y, coverWidth, coverHeight});
  }

  // The rating sits right under the cover, in the cover's own column: the row is
  // as wide as the slot the service renders for, so it reads as part of the cover
  // block. A column too narrow for five legible stars loses the rating rather
  // than overlapping the text column.
  int16_t heroHeight = coverHeight;
  const int16_t starSide = starSideFor(coverWidth);
  if (game.stars > 0 && starSide >= STAR_MIN_SIDE) {
    const int16_t starsY = static_cast<int16_t>(body.y + coverHeight + theme.spaceXs);
    if (static_cast<int16_t>(starsY + starSide) <= columnLimit) {
      drawStars(target, body.x, starsY, starSide, game.stars);
      heroHeight = static_cast<int16_t>(starsY + starSide - body.y);
    }
  }

  // The hero column beside the cover: title, author and tags, then — after a gap
  // that keeps them apart from the tags — the like and dislike counts, one per
  // line.
  fui::TextStyle titleText = theme.bodyText;
  titleText.bold = true;
  int16_t y = drawTextBlock(target, columnX, body.y, columnLimit, columnWidth, asPtr(game.title), titleText, 3,
                            theme.spaceXs);
  y = drawTextBlock(target, columnX, y, columnLimit, columnWidth, asPtr(game.author), theme.bodyText, 1,
                    theme.spaceSm);
  y = drawTextBlock(target, columnX, y, columnLimit, columnWidth, asPtr(game.tags), theme.bodyText, 3, 0);
  // The gap only exists when there is something to separate the counts from.
  const bool haveCounts = !likesText.empty() || !dislikesText.empty();
  if (haveCounts) y = static_cast<int16_t>(y + theme.spaceMd);
  y = drawTextBlock(target, columnX, y, columnLimit, columnWidth, asPtr(likesText), theme.bodyText, 2, theme.spaceXs);
  y = drawTextBlock(target, columnX, y, columnLimit, columnWidth, asPtr(dislikesText), theme.bodyText, 2, 0);

  if (y > columnLimit) y = columnLimit;
  const int16_t columnHeight = static_cast<int16_t>(y - body.y);
  if (columnHeight > heroHeight) heroHeight = columnHeight;
  if (heroHeight > columnCap) heroHeight = columnCap;
  if (statusLine) {
    const int16_t statusTop = static_cast<int16_t>(body.y + heroHeight + theme.spaceSm);
    const int16_t statusEnd = drawTextBlock(target, body.x, statusTop, static_cast<int16_t>(body.y + heroCap),
                                            body.width, statusLine, theme.bodyText, 2, 0);
    // Only a line that was actually drawn grows the band: a status that does not
    // fit must not push the heading off the reserved area.
    if (statusEnd > statusTop) heroHeight = static_cast<int16_t>(statusEnd - body.y);
  }

  // Description heading, then the viewport the text scrolls in. Both exist only
  // when the game has a description: an empty one would leave a heading over
  // blank paper. The viewport's width always leaves the reading-position bar's
  // column free, so the wrap does not change when the text grows past it; the
  // bar's side and inset come from the theme, the same tokens the list component
  // reads.
  const char* description = asPtr(game.description);
  fui::TextStyle headingText = theme.bodyText;
  headingText.bold = true;
  headingText.maxLines = 1;
  const int16_t headingY = static_cast<int16_t>(body.y + heroHeight + theme.spaceSm);
  if (description) {
    target.text(fui::Rect{body.x, headingY, body.width, line}, tr(STR_QUESTBOOK_ABOUT_GAME), headingText);
  }

  const int16_t barWidth = theme.listScrollWidth > 0 ? theme.listScrollWidth : 0;
  const int16_t barInset = theme.listScrollInset > 0 ? theme.listScrollInset : 0;
  const int16_t barReserve = barWidth > 0 ? static_cast<int16_t>(barWidth + barInset) : 0;
  const bool barOnLeft = theme.listScrollSide == 1;
  const int16_t viewportTop = static_cast<int16_t>(headingY + headingHeight);
  const int16_t viewportBottom = static_cast<int16_t>(actionsTop - theme.spaceSm);
  fui::Rect viewport{static_cast<int16_t>(barOnLeft ? body.x + barReserve : body.x), viewportTop,
                     static_cast<int16_t>(body.width - barReserve),
                     static_cast<int16_t>(viewportBottom > viewportTop ? viewportBottom - viewportTop : 0)};

  fui::TextStyle descriptionText = theme.bodyText;
  descriptionText.maxLines = 255;
  int16_t contentHeight = 0;
  if (description && viewport.width > 0 && viewport.height > 0) {
    contentHeight = fui::measureWrappedText(target, description, descriptionText, viewport.width).height;
  }
  // The loop task scrolls with these, and this pass holds the render lock that
  // guards them.
  descMaxScroll = contentHeight > viewport.height ? static_cast<int16_t>(contentHeight - viewport.height) : 0;
  descPageStep = viewport.height > line ? static_cast<int16_t>(viewport.height - line) : line;
  if (descScroll > descMaxScroll) descScroll = descMaxScroll;
  if (descScroll < 0) descScroll = 0;
  if (contentHeight > 0) {
    // The lines are laid out once more through the same layoutText() the height
    // came from and drawn one by one, rather than drawn as a block: text()
    // centres a block inside its rect and wraps through the renderer's own
    // wrapper, so a block draw could disagree with the measured height by a line
    // and the scroll would be off. The rect is exactly `contentHeight` tall and
    // shifted by the scroll, and the viewport clip hides the rest.
    const fui::Rect block{viewport.x, static_cast<int16_t>(viewport.y - descScroll), viewport.width, contentHeight};
    // Each emitted run is already a complete line, so it is drawn as one line:
    // a second wrap would re-break text the measurement already counted.
    fui::TextStyle lineText = descriptionText;
    lineText.maxLines = 1;
    const auto drawLine = [&target, &lineText](const char* run, const fui::Rect runRect) {
      target.text(runRect, run, lineText);
    };
    const fui::Rect previousClip = target.clipRect();
    target.setClipRect(viewport);
    fui::layoutText(target, block, description, descriptionText, drawLine);
    target.setClipRect(previousClip);
  }
  if (descMaxScroll > 0 && barWidth > 0) {
    fui::drawListScrollIndicator(target, fui::Rect{body.x, viewportTop, body.width, viewport.height},
                                 static_cast<uint32_t>(contentHeight), static_cast<uint32_t>(viewport.height),
                                 static_cast<uint32_t>(descScroll), barWidth, theme.listScrollSide, barInset);
  }

  // Same outlined action button as the storygame reader's choices, so the two
  // screens cannot drift apart. The primary action is Play; the second one is
  // the library action the game's presence on the card decides. One props object
  // serves both: fui::button() copies what it needs during the call.
  fui::ButtonProps& button = actionButton;
  button.styles = outlinedActionButtonStyles(theme.controlRadius);
  button.text = theme.bodyText;
  button.radius = theme.controlRadius;
  button.minTouchSize = theme.minTouchSize;

  button.label = tr(STR_QUESTBOOK_PLAY);
  button.icon = icon32(playIconImage);
  button.action = ACTION_PLAY;
  button.enabled = !downloading;
  // The selected action is drawn inverted (the outlined style's chosen state);
  // the other one keeps the outline. A disabled page ignores the selection,
  // because the disabled style resolves first.
  button.state = selected == Selected::Play ? fui::StateSelected : fui::StateNormal;
  fui::button(screen.frame(),
              fui::Rect{body.x, static_cast<int16_t>(actionsTop + actionHeight + actionGap), body.width, actionHeight},
              button);

  button.label = downloaded ? tr(STR_QUESTBOOK_REMOVE_FROM_LIBRARY) : tr(STR_QUESTBOOK_ADD_TO_LIBRARY);
  button.icon = icon32(downloaded ? deleteIconImage : libraryIconImage);
  button.action = ACTION_LIBRARY;
  button.enabled = !downloading;
  button.state = selected == Selected::Library ? fui::StateSelected : fui::StateNormal;
  fui::button(screen.frame(), fui::Rect{body.x, actionsTop, body.width, actionHeight}, button);
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

  // Confirm runs the selected button, the directions scroll the description and
  // move the selection, so the hints name both halves of that: Select for
  // Confirm, and the Left/Right pair that always steps the selection.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
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
  // Confirm runs the button the panel shows as selected. Read before the
  // directions step the selection, so a pass that carries both edges runs what
  // the panel was already showing rather than a selection made in that pass.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate(selected);
    return;
  }
  // Swipes scroll the description by a viewport, as they page the reader's
  // paragraph; a page step keeps one line of overlap as the cue that more text
  // follows.
  const MappedInputManager::SwipeDir swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    scrollDescription(swipe == MappedInputManager::SwipeDir::Up ? 1 : -1);
    return;
  }
  // The side directions scroll the description while it overflows and step the
  // selection while it fits — which is what they did before the text could
  // scroll at all — and the front Left/Right buttons always step the selection,
  // so the two actions stay reachable whichever the page shows.
  if (mappedInput.wasPressed(MappedInputManager::Button::ScreenUp) && !scrollDescription(-1)) {
    stepSelection();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenDown) && !scrollDescription(1)) {
    stepSelection();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenLeft) ||
             mappedInput.wasPressed(MappedInputManager::Button::ScreenRight)) {
    stepSelection();
  }
}

void StorygameDetailsActivity::stepSelection() {
  {
    // buildScreen() paints the selection on the render task.
    RenderLock lock;
    selected = selected == Selected::Play ? Selected::Library : Selected::Play;
  }
  requestUpdate();
}

bool StorygameDetailsActivity::scrollDescription(const int8_t direction) {
  bool scrollable = false;
  bool moved = false;
  {
    // buildScreen() published these on the render task under the same lock.
    RenderLock lock;
    scrollable = descMaxScroll > 0;
    if (scrollable) {
      int16_t next = static_cast<int16_t>(descScroll + (direction > 0 ? descPageStep : -descPageStep));
      if (next < 0) next = 0;
      if (next > descMaxScroll) next = descMaxScroll;
      moved = next != descScroll;
      descScroll = next;
    }
  }
  if (moved) requestUpdate();
  return scrollable;
}

// --- actions -----------------------------------------------------------------

void StorygameDetailsActivity::onPlayEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<StorygameDetailsActivity*>(user);
  self->app.clearTapFlash();
  self->activate(Selected::Play);
}

void StorygameDetailsActivity::onLibraryEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<StorygameDetailsActivity*>(user);
  self->app.clearTapFlash();
  self->activate(Selected::Library);
}

void StorygameDetailsActivity::activate(const Selected action) {
  if (downloading) return;
  {
    // A tap runs its own action, but the page keeps one selection: the one it
    // paints inverted is the one Confirm would run.
    RenderLock lock;
    selected = action;
  }
  if (action == Selected::Library) {
    // The second action follows whether the game is on the card.
    if (downloaded) {
      promptRemoveFromLibrary();
    } else {
      addToLibrary();
    }
    return;
  }
  play();
}

void StorygameDetailsActivity::play() {
  if (downloading) return;
  if (downloaded) {
    const std::string gameDir = questbook_game::directory(game.indexName);
    // A game downloaded before its cover arrived can get one now.
    storeCover(gameDir);
    openReader(gameDir);
    return;
  }
  // The workspace already holds this game — the reader is open again after a
  // return to this page — so there is nothing to transfer: the import would only
  // clear and rewrite the same tree. The page's cover may have arrived after that
  // import, so it is filled in before the reader draws the game's recent card.
  if (questbook_game::tempWorkspaceHolds(game.indexName)) {
    storeCover(questbook_game::tempDirectory());
    openReader(questbook_game::tempDirectory());
    return;
  }
  // A complete workspace that is not this game takes the same path, and so does
  // one whose meta.json could not be read or parsed. Replacing it is the fail-safe
  // direction: opening the reader on it would risk showing another game, while the
  // import can only cost time. The log names both cases, because they are not the
  // same fault.
  if (questbook_game::isDownloadedAt(questbook_game::tempDirectory())) {
    LOG_INF("QBOOK", "replacing the play workspace (another game or unreadable meta) with %s",
            game.indexName.c_str());
  }
  // §6.3: playing a game that is not on the card imports it into Play's shared
  // workspace and opens the reader from there. The workspace is not a library
  // item, so a catalogue Play leaves the library as it was; the game's own page
  // keeps offering Add to Library.
  startImport(questbook_game::tempDirectory(), /*openWhenDone=*/true);
}

void StorygameDetailsActivity::openReader(const std::string& gameDir) {
  auto reader = StorygameReaderActivity::create(renderer, mappedInput, gameDir);
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
  startImport(questbook_game::directory(game.indexName), /*openWhenDone=*/false);
}

void StorygameDetailsActivity::startImport(const std::string& gameDir, const bool openWhenDone) {
  if (downloading) return;
  if (gameDir.empty()) {
    // The path layer refused the game's name, so there is nowhere to import to.
    // The page reports it like any other failed import instead of doing nothing.
    LOG_ERR("QBOOK", "no destination for %s", game.indexName.c_str());
    RenderLock lock;
    errorText = tr(STR_QUESTBOOK_IMPORT_FAILED);
    requestUpdate();
    return;
  }
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
    startActivityForResult(std::move(wifi), [this, gameDir, openWhenDone](const ActivityResult& result) {
      if (result.isCancelled) return;
      startImport(gameDir, openWhenDone);
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

  // An import is about to replace the workspace. Its recent card names the game
  // that was there, and that game is gone once the tree is rewritten, so the card
  // is dropped before the first write rather than after: a card that outlives its
  // game would open the wrong one. Only reached once the radio and heap gates have
  // passed, so a Play refused before the transfer leaves the previous card alone.
  if (gameDir == questbook_game::tempDirectory()) {
    RECENT_BOOKS.removeByPath(questbook_game::tempDirectory());
  }

  const bool imported = transferGame(gameDir);
  // Only the permanent directory makes the game a library item; the workspace is
  // a scratch copy and leaves "downloaded" alone.
  const bool permanent = gameDir == questbook_game::directory(game.indexName);
  {
    RenderLock lock;
    downloading = false;
    if (imported && permanent) {
      downloaded = true;
    }
  }
  if (imported) {
    // The page's cover becomes the game's own copy in either location (§6.3). A
    // game without a cover simply keeps its icon.
    storeCover(gameDir);
  }
  if (imported && permanent) {
    // The library lists a downloaded game as one row; the next Library visit
    // rebuilds its index rather than waiting for some other reason to (§4.9).
    library::markLibraryIndexDirty();
    retireTempCopy();
  }
  // The interaction table is rebuilt from the next render, and a permanent import
  // changes the second action's label and icon. Closing routing on both exits keeps
  // a queued tap from landing on the table that has just been replaced.
  closeRouting();
  if (openWhenDone && imported) {
    openReader(gameDir);
    return;
  }
  requestUpdate(true);
}

// Gives the game its own copy of the page's cover. The destination is the directory
// the game was imported into: a permanent copy is what the game page resolves first
// next time, and the workspace copy is what the recent-books card reads, since the
// page itself only ever looks in `/storygames/<index_name>/` (§6.3). copyCoverTo()
// leaves an existing copy alone, so this is cheap on every launch. A game without a
// resolved cover, or a card that refused the write, is not an error: it keeps its
// icon.
void StorygameDetailsActivity::storeCover(const std::string& gameDir) {
  if (gameDir.empty() || coverPath.empty()) return;
  if (!questbook_game::copyCoverTo(gameDir, coverPath)) {
    LOG_ERR("QBOOK", "game %s imported without its cover copy", game.indexName.c_str());
  }
}

// A game played from the workspace leaves a copy of its files under
// /storygames/___temp/ and a recent card pointing there. Once the same game is a
// library item the copy is wrong — the same game twice on the card (§6.3) — but the
// card itself is right: the game was launched, so it stays in recents, now under the
// book rules. A workspace holding a different game is left alone: that game is still
// the one being played from the catalogue.
void StorygameDetailsActivity::retireTempCopy() {
  if (!questbook_game::tempWorkspaceHolds(game.indexName)) return;
  // Unreachable through startImport(): it refuses an empty destination before any
  // transfer, so a permanent import only happens with a real directory. Kept
  // because this function writes a recents path, and an empty one would be a card
  // for nothing.
  const std::string gameDir = questbook_game::directory(game.indexName);
  if (gameDir.empty()) return;
  RECENT_BOOKS.removeByPath(questbook_game::tempDirectory());

  std::string cardCover = questbook_game::pathIn(gameDir, questbook_game::COVER_FILE_NAME);
  // storeCover() runs before this call, so the game's own cover is the card image
  // when the import had one; otherwise the card is the coverless one.
  if (!Storage.exists(cardCover.c_str())) cardCover.clear();
  RECENT_BOOKS.addBook(gameDir, game.title.empty() ? game.indexName : game.title, game.author, cardCover);

  if (!questbook_game::removeTempWorkspace()) {
    LOG_ERR("QBOOK", "cannot clear the play workspace after adding %s", game.indexName.c_str());
  }
}

bool StorygameDetailsActivity::transferGame(const std::string& gameDir) {
  const std::string url = gameEndpointUrl(game.indexName);
  if (!importer.beginAt(gameDir, game.indexName, catalogueMeta())) {
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
    // path would make a re-add on this same visit a no-op: copyCoverTo() treats
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