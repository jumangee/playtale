#include "StorygameDetailsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <utility>

#include "MappedInputManager.h"
#include "components/CatalogScreens.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

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
  // Text-only hero: the title carries the page instead of a cover (see header).
  // Height covers every line the blocks may draw plus the gaps between them;
  // publicationPage() caps it at half the body on short screens.
  props.book.coverSize = fui::Size{0, 0};
  const bool hasAuthor = !game.author.empty();
  const bool hasTags = !game.tags.empty();
  const int heroLines = props.book.titleLines + (hasAuthor ? props.book.authorLines : 0) +
                        (hasTags ? props.book.seriesLines : 0);
  props.heroHeight = static_cast<int16_t>(
      heroLines * line + (hasAuthor ? 2 : 1) * props.book.titleGap + (hasTags ? props.book.textGap : 0));

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

void StorygameDetailsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  renderUi();

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void StorygameDetailsActivity::loop() {
  const auto route = routeTouch(mappedInput);
  if (route.routed && app.invalidated()) requestUpdate();
  if (route) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
  }
}
