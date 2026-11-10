# PlayTale Roadmap

This roadmap describes how PlayTale is moving toward the scope defined in [SCOPE.md](SCOPE.md). Phases
are sequential: a phase starts only when the prior one is wrapped or explicitly carried over.

**Everything below a "planned" marker is a plan, not shipped behavior.** Current behavior is described
in `README.md`, `USER_GUIDE.md`, and `docs/`; nothing here should be read as already implemented.

Status: ✅ done · 🚧 in progress · ❌ shelved

---

## ✅ Phase 0 — Rebrand and project baseline

**Goal:** Make the fork self-describing, so later feature work is not built on upstream assumptions.

* ✅ **Naming rules.** Product, technical, developer, and website names are fixed in
  [AGENTS.md](AGENTS.md#naming-and-branding).
* ✅ **Documentation reset.** Upstream community and process documents were removed (governance,
  contributor credits, competitor comparison, the Xteink unbricking guide, the upstream PR/issue
  templates and funding link, and the release workflows that published into the upstream project).
* ✅ **Scope rewritten** around one online source and two content kinds.
* ✅ **Version restarted** at `0.1.0` for PlayTale.
* ✅ **Line-ending normalization** so fork diffs stay readable and upstream merges stay cheap.

Phase 0 is closed.

---

## 🚧 Phase 1 — КвестБук section: browse and download

**Goal:** Let a reader find and download storygames from КвестБук without a computer.

**Status:** 🚧 in progress. Browsing, the game page and download are shipped; tags and search are not.

**Shipped**

* A **КвестБук** entry in the device UI as the reader's route to quest-book.ru. It leads the home
  menu (behind «Продолжить чтение» in themes that put it there) and opens `QuestBookCatalogActivity`:
  the service's `new` and `editorial` lists, twelve games per page, each row showing the game's name
  and author.
* Selecting a row opens `StorygameDetailsActivity` — name, author, tags, rating, like counts, the
  game's description and its cover, which is fetched from the service's own conversion endpoint and
  cached on the SD card. For the reader's view of these screens see
  [USER_GUIDE.md](USER_GUIDE.md#311-квестбук).
* **Add to Library** reads the SD card first: a game that is not there offers it, which fetches the
  converted document (`/online/<index_name>/playtale/`), imports it into `/storygames/<index_name>/`
  — `game.hdr`, `game.idx`, `game.dat`, `meta.json` and the game's own `cover.bmp` — and then offers
  **Remove from Library**, which asks first and deletes that directory. The format is described in
  [docs/file-formats.md](docs/file-formats.md#storygames-on-the-sd-card).
* The page imports without holding the document: the response streams into the importer, which writes
  each paragraph payload straight to `game.dat`. Every in-session failure deletes the game's directory;
  a power cut can leave files behind, but `meta.json` is written last and a game counts as downloaded
  only when all four files are there, so such a leftover is never offered and the next download
  replaces it.
* A downloaded game is also a row of the **Library** and is browsable from the file browser: the index
  lists `/storygames/<index_name>` as one item, under the title and author from the game's own
  `meta.json`, and selecting that row opens the same game page. Adding or removing a game marks the
  index stale, and deleting the row in the Library deletes the whole game.
* **Play** opens the storygame reader. A game that is not on the card is imported first (the same
  transfer as Add) into the shared `/storygames/___temp/` workspace, so playing a catalogue game does
  not add it to the library. Either destination also gets the page's cover copied beside the game.
* **Recent books.** Launching a game records it in the recent-books store, so it appears among the
  «недавно открытые» rows at the top of the Recent tab and in the home screen's continue-reading slot.
  A downloaded game is recorded under its own directory and follows the book rules; a game played from
  the `___temp` workspace is recorded under that workspace, so it is one card. Either card starts the
  game in the runtime reader while its files are intact, while a Library row still opens the game page.
  Adding a game played from the workspace to the library moves the card to the game's own directory and
  removes the temporary copy.

**Still planned**

* **A tag screen.** Listing the tag catalogue works, but listing the games that carry a tag does not:
  the service's JSON API has no working tag filter, and its HTML route filters without paginating
  (`docs/storygames/ARCHITECTURE.md` §2.3).
* **Substring search.** The JSON API has no search endpoint; the HTML search page does not paginate
  either (§2.4).
* **Accounts during browsing** become relevant only with Phase 3.

**Why a built-in section rather than a plugin catalogue.** The SD-plugin path below could list the
same games with no new firmware, and it is the cheaper route for a generic catalogue. It was not taken
because КвестБук is the project's own service: the download, the on-SD format and the runtime are
firmware work either way, and the list endpoint's contract is already recorded in
`docs/storygames/ARCHITECTURE.md` §2.1. A plugin manifest would add a second description of that
contract without removing any of the firmware work.

**What already exists and should be reused rather than rebuilt:**

* `CatalogActivity` (`src/activities/CatalogActivity.h`) already owns the shared catalogue flow: Wi-Fi
  check, search keyboard, loading, download with progress and cancel, and error screens.
* The home screen's library slot already swaps between Plugins and OPDS
  (`src/activities/home/HomeActivity.h`).

---

## 🚧 Phase 2 — Storygame runtime

**Goal:** Play a downloaded storygame on the device.
**Status:** 🚧 in progress. The game plays; progress is not saved.

**Shipped so far.** The runtime is a native reimplementation of the game's declarative platform
framework, not a JS interpreter:

* `lib/QuestBook/StorygameGame` loads `game.hdr`/`game.idx`/`game.dat` and serves paragraph payloads on
  demand.
* `StorygameEngine` evaluates the `k`/`r`/`s` conditions (including a hand-written integer expression
  evaluator for `s`), runs the ~18 task classes over an integer parameter array, lowers BBCode, and
  builds the character sheet.
* `StorygamePayload` parses each paragraph payload with the vendored expat.
* `StorygameReaderActivity` draws one paragraph's formatted text and choices as one scrollable column
  with a reading-position bar, hyphens long words when the reader's hyphenation setting is on, and
  shows the character sheet on a left swipe, the left direction, or its own sheet button.
* Play is wired into `StorygameDetailsActivity` and launches that reader, importing the game into the
  `___temp` workspace first when it is not on the card. Every launch that reaches the reader's first
  frame also records the game in the recent-books store.

The runtime and the format are described in
[docs/contributing/architecture.md](docs/contributing/architecture.md#storygames) and
[docs/file-formats.md](docs/file-formats.md#storygames-on-the-sd-card); the design record with the
corpus measurements is [docs/storygames/ARCHITECTURE.md](docs/storygames/ARCHITECTURE.md).

**Still planned:**

* **Persist progress and game parameters, so a game resumes where it stopped.** This is the phase's
  remaining work. The state is `{current paragraph, parameter array, steps, lastarticle}`; the open
  decision is where it lives — a sixth file in the game's own directory (`state.bin`), the
  `/.crosspoint/` cache root, or a КвестБук account in Phase 3. The format, its version and its
  invalidation rule follow
  [generated-files-cache.md](.agents/rules/generated-files-cache.md#cache-file-format-versioning) and
  must be added to `docs/file-formats.md` when they land.
* **Give games their own grouping in the Library**, once there is a play state to remember.
* **Paragraph images.** The converter drops them and leaves the format's image flag reserved; adding
  them needs a delivery, scaling and presentation decision for small panels.

**What already exists and should be reused rather than rebuilt:**

* **XML: expat 2.7.3 is already vendored and used** by the EPUB parsers
  (`lib/expat/`, `lib/XmlParserUtils/XmlParserUtils.h`), now also by `StorygamePayload`.
* **Chapter/page layout and text rendering** remain shared with the EPUB reader in the long term; the
  storygame reader currently lays out its own paragraph column.

**Planned prerequisite:** moving built-in hyphenation data to the SD card, to free flash budget as the
runtime grows (German hyphenation alone is about 200 KB).

---

## Phase 3 — КвестБук account and game experience

**Goal:** Track the reader's progress and experience in a КвестБук account.

**Planned work:**

* Sign in with a КвестБук account.
* Report game experience and progress to КвестБук.
* Replace the CrossPoint Sync account settings with КвестБук account settings.

**What this replaces.** The firmware currently ships a CrossPoint Sync / KOReader progress-sync path,
including a default server at `sync.crosspointreader.com` and its settings. That path remains in the
code and is documented as current behavior until this phase lands.

**Planned prerequisite:** a КвестБук API. Nothing in this repository can define it.

---

## Related planned work

These are not phases of their own, but they are needed and unowned:

* **OTA update channel.** The firmware's update check currently points at the upstream project's GitHub
  releases. Removing the upstream release workflows left it without a source of PlayTale builds. A
  replacement channel is required.
* **Plugin Hub and font download sources.** Two features fetch from upstream-hosted services
  (`pluginhub.crosspointreader.com` and the upstream fonts repository). Both need a PlayTale source or
  removal.
* **Identifier migration.** `/.crosspoint/` and the `CROSSPOINT-BOARD-V1:` image tag are still the
  upstream names. Renaming them is planned, but there is **no cache migration path** and the image tag
  is validated during OTA and SD-card updates, so any rename must handle both deliberately. See the
  code-identifier table in [AGENTS.md](AGENTS.md#naming-and-branding).
* **The server converter's ownership.** The converter that produces `/playtale/` documents lives on
  quest-book.ru and its source is not in this repository. Its output format is versioned separately
  from the on-device container, so a change to it needs a documented owner and a compatibility review
  (`docs/storygames/ARCHITECTURE.md` §8, Appendix C).
* **TLS posture for КвестБук.** Both transfers call `setInsecure()`; there is no CA bundle and no
  pinning. That is a deliberate leftover, not a decision (`docs/storygames/ARCHITECTURE.md` §2.6, §8).
* **Response compression.** The service gzips when asked, and the firmware neither asks nor handles
  it. Worth revisiting with the memory cost measured, not assumed (`docs/storygames/ARCHITECTURE.md`
  §4.11).

---

## Out of Roadmap

Explicitly *not* planned. See [SCOPE.md](SCOPE.md) for the rationale.

* General-purpose interactive apps (notepads, calculators, unrelated games).
* Writing / authoring tools.
* Active connectivity features (RSS, news, browsers).
* PDF rendering as a first-class format.
* New third-party network connectors beyond КвестБук.

---

## How This Roadmap Changes

* Phase boundaries are decided by the human owner, not by individual changes.
* If a phase is extended or an item carried over, that is noted here.
* A planned item is recorded as planned until the code ships; see the documents-follow-code rule in
  [AGENTS.md](AGENTS.md#documents-follow-code).
