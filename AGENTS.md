# PlayTale agent guide

PlayTale is the firmware behind **КвестБук Читалка**, an open-source e-reader for Xteink and other
FreeInk-supported devices. Its mission is a lightweight, high-performance reading experience for two
kinds of long-form content, both shipped: **EPUB books**, and **storygames** — interactive gamebooks
from quest-book.ru whose declarative rules run in a native interpreter on the device. Saving storygame
progress is the open piece; `ROADMAP.md` is the only place that records it. The
X4's ESP32-C3 remains the resource baseline: roughly 380 KB usable RAM, no PSRAM, and a single
framebuffer sized for the selected panel. Other board profiles have different CPUs, display sizes,
input, and memory capabilities; check the selected profile before assuming them.

## Naming and branding

Use these names consistently. They are not interchangeable.

| Context | Value |
| --- | --- |
| Human-facing product name | «КвестБук Читалка» |
| Technical / repository name | PlayTale (`playtale`, `PLAYTALE_*`) |
| Developer, brand, and service reference | «КвестБук» |
| Website and the only online source of content | `quest-book.ru` |

**Documentation language.** Agent rules, technical references, and commit messages are English, so they
stay aligned with the upstream codebase and its English comments. Documents written for people —
`README.md`, `USER_GUIDE.md`, and any new user-facing feature document — are Russian.

**Names that live in code, not in prose.** A rebrand does not rename these by editing documents. Change
them only in a dedicated code change, because each one is a stored-format, wire-protocol, or public-API
contract:

| Identifier | Contract it holds |
| --- | --- |
| `/.crosspoint/` | On-SD cache, settings, credentials, and state root. Created in `lib/Serialization/PersistableStore.cpp`; no migration path exists. |
| `CROSSPOINT_VERSION` | Build macro used by the version display, OTA comparison, web API, and plugin templates. |
| `CROSSPOINT-BOARD-V1:` | Image magic written by `src/network/FirmwareBoardTag.cpp`; OTA and SD-update validation read it. |
| `crosspoint.local`, `CrossPoint-Reader` | mDNS hostname and AP SSID as seen by users' browsers and phones. |
| `CrossPoint-ESP32-*` | HTTP User-Agent sent to servers. |
| `window.CrossPoint` | Public JavaScript API for SD-card plugins (`src/network/html/js/plugin-host.js`). |
| `firmwareidentity::FAMILY = "crosspoint"` | Plugin compatibility family advertised to catalogs. |

When a document must mention one of these, keep the identifier verbatim and note that renaming is
planned but not done. Do not make a document promise a name the firmware does not use yet.

## FreeInk SDK

The [FreeInk SDK](freeink-sdk/) supplies the board profiles, hardware drivers,
and shared UI components. Before adding an API or device-specific code, check
the firmware HAL and the SDK source at the revision pinned by this repository.
Use the [SDK documentation](freeink-sdk/docs/README.md) and
[FreeInk SDK index](https://freeink.org/llms.txt) to find the relevant contract.
Check `git submodule status`: a different local SDK revision must be accounted
for, not silently treated as the pinned firmware dependency.

## Start here

At session start, run `uname -s`, `git branch --show-current`, `git remote -v`, and `git status --short`. Integration work targets `develop`.

This repository is a fork. `origin` is the PlayTale fork and `upstream` is the original CrossPoint
Reader project. Fork work lands on the fork's `develop`; upstream is a read-only source of fixes to
merge, never a destination to push to. Keep the fork diff narrow so upstream merges stay cheap — see
[fork maintenance](.agents/rules/git-workflow.md#fork-maintenance).

Act as a senior embedded C++ engineer. Base claims on repository evidence: cite the paths and line numbers that justify a proposed change. Explain the mechanism behind performance or memory claims and justify every new heap allocation. For every fix, tell the human how to verify it.

First inspect the relevant code and current behavior to establish whether a change is needed. Before adding an API, setting, task, persistent state, driver change, or scheduling dependency, tie it to the human requirement and check existing mechanisms. Identify unexplained scope inherited from a stash or branch.

Use the table to find guidance for the decisions being changed. Read relevant sections, not every file associated with a touched subsystem. Load implementation skills when implementation is needed and `firmware-handoff` at final handoff. Reuse guidance already in context unless it changed or a concrete question requires another read.

| Decision being changed | Relevant guidance |
| --- | --- |
| Routine builds or host setup | [existing environment](.agents/rules/environment.md#build-in-an-existing-environment) |
| Personal build flags or ports | [local overrides](.agents/rules/environment.md#platformiolocalini-personal-overrides) |
| Allocation size/failure, buffer or cache lifetime, hot-path containers, or hardware limits | [hardware-resources.md](.agents/rules/hardware-resources.md); use `heap-discipline` for allocation decisions |
| Hardware/storage access, rendering contracts, input ownership, or SDK boundaries | [HAL contracts](.agents/rules/architecture-hal.md#hardware-abstraction-layer-hal) and relevant sections of `hal-and-abstractions` |
| Build flags, dependencies, or board profiles | [build environment and flags](.agents/rules/architecture-hal.md#build-environment) |
| C or C++ implementation | [coding conventions](.agents/rules/coding-standards.md#naming-conventions); consult the relevant pitfall/allocation section and use `control-flow-clarity` for state modeling |
| Lists, controls, touch routing, or layout | [shared UI](.agents/rules/ui-activities.md#shared-ui-and-input) and [orientation](.agents/rules/ui-activities.md#orientation-aware-logic) |
| Input edges or activity/popup transitions | [input ownership](.agents/rules/ui-activities.md#input-frames-and-ownership) and its transition/verification sections |
| Activity resources or worker tasks | [activity lifecycle](.agents/rules/ui-activities.md#activity-lifecycle-and-memory-management) and [workers](.agents/rules/ui-activities.md#freertos-task-guidelines) |
| Runtime font discovery, rendering, or cache lifetime | [font ownership](.agents/rules/ui-activities.md#runtime-fonts-and-cache-ownership) |
| Plugin, service, web endpoint, or protected-book contracts | [service contracts](.agents/rules/architecture-hal.md#service-and-plugin-contracts) and the linked contract that applies |
| Storygame formats, the storygame runtime, or download integration | [SD format](docs/file-formats.md#storygames-on-the-sd-card), [subsystem architecture](docs/contributing/architecture.md#storygames), and the design record in `docs/storygames/ARCHITECTURE.md` |
| Builds, formatting, or verification | [testing workflow](.agents/rules/testing-debugging.md#testing-and-verification-workflow) and [compact build output](.agents/rules/testing-debugging.md#compact-build-output) |
| Crashes, CI, or serial measurements | [crash diagnosis](.agents/rules/testing-debugging.md#debugging-crashes) or the matching CI/serial section |
| Branch creation, merges, commits, or publication | [git-workflow.md](.agents/rules/git-workflow.md) |
| User-facing strings or language metadata | [translation workflow](.agents/rules/generated-files-cache.md#translation-workflow) |
| Generated HTML/i18n or font assets | [generated sources](.agents/rules/generated-files-cache.md#files-generated-by-build-scripts) or [font generation](.agents/rules/generated-files-cache.md#font-generation) |
| Cache formats, layout persistence, or invalidation | [versioning](.agents/rules/generated-files-cache.md#cache-file-format-versioning) and [invalidation](.agents/rules/generated-files-cache.md#cache-invalidation-rules) |
| New features, activities, settings, libraries, or dependencies | `SCOPE.md` and the `scope-discipline` skill |
| Restructuring, extraction, or separating unrelated cleanup | the `refactor-for-review` skill |

Repository-local skills live under `.agents/skills/`. Their descriptions identify specific procedures to use when needed; a keyword match alone does not require loading a skill.

## Documents follow code

A document describes what the firmware does today. `ROADMAP.md` is the only place that describes
planned work, and every planned item there is marked as a plan rather than as shipped behavior. When
code and documentation disagree, the code is right and the document is the bug. Do not edit a document
to match an intended future state, and do not leave a document claiming behavior the firmware does not
have.

## Human ownership

A PR is a long-term maintenance commitment. Working code is not enough: prefer the simplest design that meets the real requirement, fits `SCOPE.md`, and can be understood and maintained by its human owner.

Fully autonomous end-to-end agents are forbidden. Review subagents inspect code,
diffs, history, and recorded checks only. The main agent owns verification:
reviewers may not write files (including temporary diagnostics), run tests/builds,
commit, publish, deploy, or flash.

The human must write PR descriptions; agents may give concise factual notes and test
results, never ready-to-paste PR prose. Creating/amending local commits requires
explicit human approval. Push only on an explicit human instruction to push;
edit/commit approval does not authorize it. Never open or close a PR.

Repository-facing prose: use plain English for non-native readers and standard
technical terms when clearest. Code comments must be short and useful after merge; follow the
[comment rules](.agents/rules/coding-standards.md#comment-style).

## Mandatory firmware handoff

For every logical change that can affect shipped firmware or its build—including C/C++, build configuration, partitions, code-generation sources, translations, and release scripts—the main agent must complete [firmware-handoff](.agents/skills/firmware-handoff/SKILL.md) before declaring the work ready or making an approved local commit. That skill owns the required checks, four independent reviews, bounded follow-up, and explicit human acceptance of behavior, architecture, and maintenance ownership.

Read-only inquiries, audits, and reviewers do not run implementation handoff. Pure tests, diagnostics, documentation, and host-only Python scripts use a lighter review unless they alter firmware output or its build. Hardware testing remains the human's responsibility and is required before a PR is opened.
