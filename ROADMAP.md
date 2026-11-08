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

* A **КвестБук** entry in the device UI as the reader's route to quest-book.ru.
* Browsing the КвестБук catalogue and downloading a storygame to the SD card.
* A storygame file format placed under the library root, so downloaded games appear in the existing
  library and file browser.

**Shipped so far.** The **КвестБук** entry leads the home menu (behind «Продолжить чтение» in themes
that put it there) and opens `QuestBookCatalogActivity`: the service's `new` and `editorial` lists,
twelve games per page, each row showing the game's name and author. Selecting a row opens
`StorygameDetailsActivity` — name, author, tags, rating, like counts, the game's description and its
cover, which is fetched from the service's own conversion endpoint and cached on the SD card. For
the reader's view of these screens see [USER_GUIDE.md](USER_GUIDE.md#311-квестбук).

No game is downloaded yet: the game page is reference information while the download and the import
into the layout agreed for it are built, so this step validates the service contract and the memory plan
without the runtime. Only the cover travels to the device, into a cache.

**Still planned:** the game's own download and import into the layout it will live in, and Play.

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

**Planned work:**

* Parse the storygame's XML: the game modules, the transition map, and the game parameters.
* Execute the storygame's rules and drive the plot from the reader's choices.
* Persist progress and game parameters, so a game resumes where it stopped.
* Register the storygame format with the library so games and books appear together.

**What already exists and should be reused rather than rebuilt:**

* **XML: expat 2.7.3 is already vendored and used** by the EPUB parsers
  (`lib/expat/`, `lib/XmlParserUtils/XmlParserUtils.h`). No new XML dependency is needed; what is
  needed is a parser for the storygame schema on top of it.
* **Chapter/page layout and text rendering** are shared with the EPUB reader.

**Open decision — the scripting engine.** Nothing suitable is vendored today; a JS interpreter would be
a new dependency. This is the phase's critical path:

* The engine must fit the ESP32-C3 baseline (roughly 380 KB usable RAM, no PSRAM). On S3 boards PSRAM
  is available, but C3 support is required.
* Candidates (for example mJS, Elk, Espruino) must be evaluated for RAM, flash, and worst-case
  execution before one is chosen.
* **No engine is selected yet.** This document records the constraint, not a choice.

**Planned prerequisite:** moving built-in hyphenation data to the SD card, to free the flash budget the
runtime will need (German hyphenation alone is about 200 KB).

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
