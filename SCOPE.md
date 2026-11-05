# Project Vision & Scope: PlayTale («КвестБук Читалка»)

PlayTale is the open-source firmware behind **КвестБук Читалка**, an e-reader for ESP32-based devices.
It targets the Xteink X4 and other FreeInk-supported boards, and it exists to do one thing
exceptionally well: **let someone read at length, comfortably, on cheap e-ink hardware.**

It serves two kinds of long-form content. Only the first works today; the second is the
project's goal and is not implemented:

- **Books** — EPUB 2/3, plus TXT and Markdown through the same reflowable pipeline, and XTC for
  pre-rendered pages. **Shipped.**
- **Storygames** — interactive gamebooks. The author writes the rules, the reader makes choices, and
  the plot continues from the author's transition map and game parameters. Storygames will be
  distributed by КвестБук and run on the device. **Planned** — see `ROADMAP.md`.

## 1. Core Mission

Provide a lightweight, high-performance firmware that maximizes the potential of ESP32 e-reader
hardware, prioritizing legibility, performance, and usability over "swiss-army-knife" functionality.

PlayTale is **not** a kitchen-sink firmware. It wants clean, maintainable code, and it runs across the
supported range of ESP32 e-reader devices. Every accepted change should make that goal easier, not
harder. Device-specific code lives behind the HAL / SDK boundary so the reader core stays portable.

## 2. Guiding Principle: One Excellent Reading Experience

PlayTale is a fork of CrossPoint Reader, and it deliberately narrows the product rather than widening
it. New work is evaluated against that:

* **Does it improve reading a book or playing a storygame?** If it serves a different use case, it is
  out of scope.
* **Does it fit the X4's resource ceiling?** The ESP32-C3 sets the budget. A feature that cannot fit
  the C3 baseline is either board-gated or not accepted.
* **Does it stay maintainable by its human owner?** Prefer the simplest design that meets the real
  requirement.

## 3. The Single Online Source

**КвестБук (`quest-book.ru`) is the only intended online source of content and accounts.** This
replaces the upstream project's blanket freeze on network connectors with a positive rule:

* Books and storygames are obtained from КвестБук. Download and account integration with it are the
  roadmap's destination, not a side quest.
* **Do not add new third-party connectors.** No new sync engines, cloud-storage clients, or remote
  file servers beyond КвестБук.
* **OPDS stays supported as it is today.** It is generic, already shipped, and useful for local and
  self-hosted libraries. What is closed is adding *new protocols or services*, not maintaining the
  existing generic one.
* **The CrossPoint Sync and KOReader sync paths currently shipped are temporary.** They exist in the
  code today and are documented as current behavior. The plan is to replace the CrossPoint Sync
  account with a КвестБук account for tracking game experience (see `ROADMAP.md`).

## 4. Current Focus

The codebase was inherited with a large surface area. The priorities are:

* **Memory footprint:** Reducing DRAM usage and heap fragmentation. The C3 is the tightest target and
  sets the ceiling, but the gains benefit every ESP32 variant.
* **Flash footprint:** Trimming binary size. Storygames need an XML parser and a scripting engine, and
  both must fit.
* **Code cleanup:** Refactoring, removing dead code, tightening abstractions, and improving
  readability.
* **Book reading experience:** EPUB parsing and rendering, typography, hyphenation, line spacing, font
  handling, and legibility.

## 5. Supported Devices

* **Xteink X4** (ESP32-C3) — the resource baseline and primary target.
* **Xteink X4 Pro**, **Xteink X4 Classic**, **Seeed reTerminal Sticky**, **M5Stack Paper Mono**.

The **Xteink X3** is no longer produced and is not a target. Its build environment still exists and may
still compile, but it is not released, not tested, and not supported; treat it as unmaintained.

The **Metalio E-Ink 4**, **EEGO A4**, and **Boox Picco** environments likewise still build but are not
supported targets.

## 6. Scope

### In-Scope

*Features that directly improve reading a book or a storygame, or the firmware's maintainability.*

* **EPUB Rendering & Optimization:** Improvements to the rendering engine, CSS/image handling, and
  parsing performance.
* **Typography & Legibility:** Custom font support, hyphenation, line and paragraph spacing, margins.
* **E-Ink Driver Refinement:** Reducing full-screen flashes (ghosting management) and improving
  general rendering.
* **Reading UX:** Bookmarks, progress tracking, button mapping, page navigation, and other in-reader
  interactions.
* **Library Management:** Simple, intuitive ways to organize and navigate a local collection.
* **Storygames:** Reading them, the format that carries them, the runtime that executes their rules,
  and downloading them from КвестБук.
* **КвестБук integration:** Downloading books and storygames, and the КвестБук account.
* **Memory, Flash, and Code Quality:** Refactors and cleanups that reduce resource use or improve
  maintainability, even without a user-visible feature.

### Out-of-Scope

*Rejected because they compromise the device's stability, maintainability, or core mission.*

* **General-purpose interactive apps:** No notepads, calculators, or unrelated games. Storygames are
  the deliberate exception and the reason the runtime exists.
* **Writing / Authoring Tools:** No typed notes, journals, or editors. Input hardware and RAM are
  wrong for this.
* **Active Connectivity:** No RSS readers, news aggregators, or web browsers. Background Wi-Fi drains
  the battery and complicates the single-core CPU.
* **PDF Rendering:** PDFs are fixed-layout documents, so rendering them requires displaying pages as
  images rather than reflowable text, resulting in constant panning and zooming that makes for a poor
  reading experience on e-ink.

## 7. PlayTale as a downstream foundation

PlayTale is itself a fork, and it may serve as a foundation for further forks. Supporting that is not
part of PlayTale's own product goal, but broadly reusable, product-neutral improvements to low-level
infrastructure may still be accepted when they do not impose new dependencies, resource cost, or
maintenance burden.

Downstream-specific UI, policy, workflows, and feature semantics remain out of scope. If a reusable
primitive cannot be cleanly separated from the downstream feature that needs it, that work belongs in
the downstream fork.

## 8. Upstream Relationship

PlayTale tracks the upstream CrossPoint Reader project as a source of fixes. PlayTale does not send
changes upstream and does not publish releases into the upstream project's channels. Because the two
products now diverge in mission, some upstream documents and workflows were removed from this fork;
`ROADMAP.md` records what replaced them.

## 9. Calls to Action

These are the areas where help is most valuable right now.

### The Storygame Runtime

The XML parser exists (expat is already vendored and used by the EPUB parsers). The scripting engine
does not exist, and nothing suitable is vendored. Choosing and sizing one for the C3's 380 KB budget is
the critical path for storygames. See `ROADMAP.md`.

### Font and Hyphenation Footprint

Built-in hyphenation data costs flash that the reader core and the storygame runtime both need. Moving
it to the SD card, analogous to the existing font downloader, frees that budget.

### Identifying Stock-Firmware Gaps

Help cataloguing what the stock firmware handles poorly or not at all, so future work has a clear
target list, particularly in RTL and non-Latin scripts. Concrete examples (screenshots, sample EPUBs,
expected vs actual behavior) are genuinely useful, with or without code.
