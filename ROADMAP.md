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

**Planned work:**

* A **Quest-Book.ru** entry in the device UI as the reader's route to quest-book.ru.
* Browsing the КвестБук catalogue and downloading a storygame to the SD card.
* A storygame file format placed under the library root, so downloaded games appear in the existing
  library and file browser. **Shipped:** a downloaded game is one row of the Library and its directory is
  browsable from the file browser.

**Shipped so far.** The **Quest-Book.ru** entry leads the home menu (behind «Продолжить чтение» in themes
that put it there) and opens `QuestBookCatalogActivity`: the service's `new` and `editorial` lists,
twelve games per page, each row showing the game's name and author. Selecting a row opens
`StorygameDetailsActivity` — name, author, tags, rating, like counts, the game's description and its
cover, which is fetched from the service's own conversion endpoint and cached on the SD card. For
the reader's view of these screens see [USER_GUIDE.md](USER_GUIDE.md#311-квестбук).

That page also downloads. Its second action reads the SD card: a game that is not there offers
**Add to Library**, which fetches the converted document (`/online/<index_name>/playtale/`), imports it
into `/storygames/<index_name>/` — `game.hdr`, `game.idx`, `game.dat`, `meta.json` and the game's own
`cover.bmp` — and then offers **Remove from Library**, which asks first and deletes that directory. The
format is described in [docs/file-formats.md](docs/file-formats.md). **Play** opens the storygame reader:
`lib/QuestBook/StorygameEngine` runs the game's tasks over its parameter array and follows the
paragraph transition map, and `StorygameReaderActivity` renders one paragraph's formatted text and
choices as a swipe-scrollable column with a reading-position bar, and shows the character sheet on a
left swipe or the reader's sheet button. A game that is not on
the card yet is imported first (the same transfer as Add), then read.

The page imports without holding the document: the response streams into the importer, which writes
each paragraph payload straight to `game.dat`. Every in-session failure deletes the game's directory;
a power cut can leave files behind, but `meta.json` is written last and a game counts as downloaded
only when all four files are there, so such a leftover is never offered and the next download replaces
it.

A downloaded game is also a row of the **Library**: the index lists `/storygames/<index_name>` as one
item, under the title and author from the game's own `meta.json`, and selecting that row opens the same
game page. Adding or removing a game marks the index stale, and deleting the row in the Library deletes
the whole game. Games never enter the recent-books store, so they are not among the «недавно открытые» rows
at the top of the Recent tab or in the home screen's continue-reading slot.

**Still planned:** persisting progress and game parameters, recent-books integration, and a separate
games grouping in the Library.

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

**Open question:** whether КвестБук accounts are needed during browsing, which pulls Phase 3 forward.

---

## Phase 2 — Storygame runtime

**Goal:** Play a downloaded storygame on the device.
**Status:** 🚧 in progress.

**Shipped so far.** The runtime is a native reimplementation of the game's declarative platform
framework, not a JS interpreter: `lib/QuestBook/StorygameGame` loads `game.hdr`/`game.idgame.dat`;
`StorygameEngine` evaluates the `k`/`r`/`s` conditions and runs the ~18 task classes over an integer
parameter array; `StorygamePayload` parses each paragraph payload with the vendored expat; and
`StorygameReaderActivity` draws one paragraph's BBCode-lowered text and choices as one scrollable
column with a reading-position bar, and shows the character sheet on a left swipe or its own sheet
button. Play is wired into `StorygameDetailsActivity` and
launches that reader (downloading the game first when it is not on the card).

**Still planned:**

* Persist progress and game parameters, so a game resumes where it stopped.
* Put games into the recent-books store (so they appear among «недавно открытые» and in continue-reading)
  and give games their own grouping in the Library, once there is a play state to remember.

**What already exists and should be reused rather than rebuilt:**

* **XML: expat 2.7.3 is already vendored and used** by the EPUB parsers
  (`lib/expat/`, `lib/XmlParserUtils/XmlParserUtils.h`), now also by `StorygamePayload`.
* **Chapter/page layout and text rendering** remain shared with the EPUB reader in the long term; the
  storygame reader currently lays out its own paragraph column.

**Open decision — progress persistence.** Play state is not saved yet: a new game always starts from
the entry paragraph, and `save`/`achi` tasks are no-ops offline. The state format and where to store it
(per-game under `/storygames/`, the existing `/.crosspoint/` caches, or a КвестБук account in Phase 3)
is the phase's remaining work. Persistent-format and invalidation rules follow
[generated-files-cache.md](.agents/rules/generated-files-cache.md) when it lands.

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
