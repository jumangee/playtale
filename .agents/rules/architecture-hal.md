## Project Architecture

### Build System: PlatformIO

Routine builds use the [existing environment](environment.md#build-in-an-existing-environment);
first-time IDE/toolchain setup is in [getting started](../../docs/contributing/getting-started.md).
Shared configuration is `platformio.ini` and `partitions.csv`; personal
overrides belong in ignored `platformio.local.ini`.

### Build Environment

C++20 (`-std=c++2a`), no exceptions or RTTI. Use `LOG_INF`, `LOG_DBG`, or
`LOG_ERR` from `Logging.h`, not raw Serial output.

Inspect the selected environment and inheritance in `platformio.ini`.
USB-MSC profiles use the prebuilt Arduino/TinyUSB graph instead of the
`firmware_tuned` core rebuild. Preserve pinned platforms, dependencies, and patches.

### Critical Build Flags

Read the selected configuration rather than copying flag values from examples.
Preserve the contracts behind:
`EINK_DISPLAY_SINGLE_BUFFER_MODE`, `ARDUINO_USB_MODE`,
`ARDUINO_USB_CDC_ON_BOOT`, `XML_CONTEXT_BYTES`, `USE_UTF8_LONG_NAMES`,
`XML_GE` (general-entity security), and `DESTRUCTOR_CLOSES_FILE`.
XML general entities remain disabled (`XML_GE=0`).

SdFat's `USE_SPI_ARRAY_TRANSFER` / `USE_SEPARATE_FAT_CACHE` depend on
`scripts/patch_sdfat.py` and its dependency pin. SPI array transfers add
send-path stack use; SDMMC uses the block-device interface.
Miniz defines live in `lib/miniz/src/MinizConfig.h`, not obsolete global flags.

### Hardware Abstraction Layer (HAL)

Firmware uses `lib/hal/`, not SDK classes directly:
`HalDisplay` wraps `EInkDisplay`, `HalGPIO` wraps `InputManager`, and
`Storage` (`HalStorage`) wraps `SDCardManager`.
Memory, power, system, time, frontlight, and tilt also have HAL services.
Expose a new SDK capability through the relevant HAL with its mutex, logging,
and error contract.

Use `HalFile`, not raw SdFat/Arduino handles. Every operation and its destructor
take `storageMutex`. Bypassing it races `SdSpiCard::m_spiActive` and can end an
SPI transaction against another task's lock, causing FreeRTOS
`xTaskPriorityDisinherit` assertions. No direct `SdFat`, `SdSpiCard`,
`FsBaseFile`, `FsFile`, or `SDCardManager` calls outside the HAL.

Shared state uses established `SETTINGS`, `APP_STATE`, `GUI`, `Storage`,
and `I18N` singleton access rather than threaded pointers.

### File handle lifetime

Local `HalFile` handles close at scope exit through the mutex and
`DESTRUCTOR_CLOSES_FILE`; do not add redundant closes.
Close explicitly before removing/reopening the same path or reusing its handle.
Member handles close at the intended release point, such as `onExit()`.

### Framebuffer ownership

The C3 baseline has one panel-sized framebuffer. For temporary grayscale
overlays, pair successful `storeBwBuffer()` with `restoreBwBuffer()` or discard
when the underlying page changed. Backups are chunked; do not add a second
full-screen buffer. Use renderer/display capabilities for strip grayscale.

`restoreBwBuffer(false)` avoids restoring an older differential baseline when
the glass still shows newer overlay content. Check [GfxRenderer.h](../../lib/GfxRenderer/GfxRenderer.h).
`FrameBufferLoan` lends the existing buffer during builds: no drawing/display
while loaned. Claim exclusive scratch through `lib/Memory/BuildScratch.h`,
release it before restoration, and redraw afterward.

### Storage modes

USB Drive gives the host exclusive raw SD ownership. Stop filesystem users and
pending navigation; follow `UsbDriveActivity` and `ActivityManager::loop()`.
Exit through the required restart instead of re-enabling filesystem access early.

### Service and plugin contracts

Service policy/formats live in SD-card plugins. Firmware provides generic web
endpoints, bounded jobs, declarative catalog screens, events, and protection
primitives. Browser `plugin.js` runs in the browser; the device interprets
manifests/request templates, not arbitrary JavaScript.

Consult the contract being changed:
- [sd-plugins.md](../../docs/sd-plugins.md): discovery, catalogs, jobs, authentication,
  paths, per-book keys, and isolation.
- [plugin-events.md](../../docs/plugin-events.md): event whitelist, deferred outboxes,
  sleep delivery, sessions, and metadata sidecars; preserve delivery/compatibility.
- [webserver-endpoints.md](../../docs/webserver-endpoints.md): request/response contracts;
  normalize user paths and escape filenames through existing helpers.
- Protected reads: inspect `Epub` and pinned SDK `ContentProtection` before changing
  decrypt-on-read/key handling. Keep service activation policy in plugins.

### Reader formats and the storygame runtime

Format detection is extension-based and centralized. Register a new content kind by
extending the predicates in `lib/FsHelpers/FsHelpers.cpp`, the library filter in
`lib/LibraryIndex/LibraryBuilder.cpp` (`isLibraryBookFile`), and the reader dispatch in
`src/activities/reader/ReaderActivity.cpp` (`create()`), plus the cache/progress helpers
in `src/util/` that map a path to cached state. Do not scatter extension checks through
activities.

Storygames are a **directory**, not a file, so they are recognised by content rather than
by extension: `ActivityManager::goToReader()` and `goToRecent()` ask
`questbook_game::isGameAtPath()`/`isTempWorkspace()`/`isIncompleteGameAtPath()` before
`ReaderActivity::create()`. A book file that happens to sit in `/storygames/` must still
open as a book, and a game directory must reach its game page rather than the EPUB
reader. Shape:

- **XML parsing reuses the vendored expat** (`lib/expat/`, already used by the EPUB
  parsers) through `lib/XmlParserUtils/`; `StorygamePayload` parses one paragraph per
  call. Do not add a second XML library. Keep the existing entity-security flags
  (`XML_GE=0`) and the small input-buffer setting unless a measured need changes them.
- **There is no scripting engine and none may be added for this format.** The rules are a
  hand-written task interpreter and condition evaluator in
  `lib/QuestBook/StorygameEngine.*`; the `s` expression type has its own allocated-nothing
  recursive-descent evaluator. It fits the ESP32-C3 baseline and is host-tested.
- **The on-SD container is a stored-format contract.** `lib/QuestBook/StorygameFormat.h`
  declares every record; `test/storygame_format` asserts the field offsets, so a
  reordering must break the build. Bump `FORMAT_VERSION` only with a re-download story:
  there is no migration, a mismatched header is refused and the game must be fetched
  again. The byte layout is documented in
  [file-formats.md](../../docs/file-formats.md#storygames-on-the-sd-card).
- **`meta.json` is written last and is the commit marker.** Anything that lists games asks
  `isDownloadedAt()`, never tests the directory.
- **Play state is not persisted yet.** When it is, it is per-game persistent data: follow
  the cache-format versioning rules in
  [generated-files-cache.md](generated-files-cache.md#cache-file-format-versioning), and
  add the format to `docs/file-formats.md` in the same change.
- **Do not allocate per step.** The play path is one 16-byte index read plus one payload
  parse; the payload buffer is released after each parse and the parameter array is sized
  once at load. See [architecture.md §Storygames](../../docs/contributing/architecture.md#storygames).
