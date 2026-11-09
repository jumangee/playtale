# КвестБук integration and storygame runtime — architecture draft

> **Status: draft for review.** Most of this document describes a design that is **not implemented**.
> It is a plan, not a record of current behaviour. What ships: the catalogue, the game page and its
> download (`QuestBookCatalogActivity`, `StorygameDetailsActivity`, `lib/QuestBook/StorygameImporter.*`),
> and the storygame runtime and reader that Play launches (`lib/QuestBook/StorygameEngine.*`,
> `StorygameGame.*`, `StorygamePayload.*`, `src/activities/reader/StorygameReaderActivity.*`). The byte
> layout those files are written in now lives in [file-formats.md](../file-formats.md), because it is
> current behaviour rather than a plan. This section says what those screens do today and what is still
> unbuilt; once further parts ship, they must move to the documents that describe current behaviour, and
> this text must be reduced to whatever is still unbuilt. See the "Documents follow code" rule in
> `AGENTS.md`.

This document covers two connected pieces of planned work:

1. **The КвестБук catalogue** — finding a game on quest-book.ru, reading its page, and downloading it
   (roadmap Phase 1).
2. **The storygame runtime** — playing a downloaded game on the device (roadmap Phase 2).

Both were planned in `ROADMAP.md` before the game format was studied. This draft replaces the
guesses in those phases with measurements taken from the live service and from 35 downloaded games.

A third piece emerged from the study and is now a prerequisite for the first two: **a server-side
converter** on quest-book.ru that re-emits each game in a compact, directly indexable form (§4).
Because the platform is under the project's control, the format and the memory problem can be solved
once, on the server, instead of on every device.

Evidence conventions used below:

- **[M]** — measured in this study. The method and numbers are given inline.
- **[C]** — a constraint from this repository (a rule, a resource limit, or existing code).
- **[O]** — open question; requires a decision or data this study could not obtain.

---

## 1. What was established

The single most consequential finding: **storygame authors do not write JavaScript.**

A storygame is a declarative state machine. Paragraphs, choices and parameter effects are data.
The only executable code in a game file is a fixed platform framework (`mitril`), identical in every
game, which the device does not need to interpret at all — it needs to be reimplemented once,
natively.

Consequences, expanded in the sections below:

- No JavaScript engine is required. `ROADMAP.md` records the scripting engine as Phase 2's critical
  path ("nothing suitable is vendored today"); that constraint is removed.
- Instead, a small **task interpreter** for ~18 declarative operation classes and a small
  **expression evaluator** for one condition type are required.
- Because the format is declarative, the server can **re-emit a game in an indexed form** (§4), so a
  paragraph lookup is an array index rather than a parse — and the device needs no game-format parser
  at all.
- Separately, and more cheaply, the service **already gzips** its responses if asked (§4.11). The
  firmware never asks; that alone gives a 4× reduction on game downloads.

### 1.1 Script census [M]

Method: download every game listed on `/online/api/list/new/?page=N`, `/best`, `/editorial`
(35 distinct games), extract every `<script type="localNNN_XXXXXXXXX">` and every
`<script type="preload">` body, and classify the body by shape.

| Measurement | Result |
| --- | --- |
| Games studied | 35 |
| Local condition/effect scripts | 11 007 |
| Scripts not matching one of the declarative shapes | **0** |
| `preload` scripts, all of shape `mitril.scriptWrap(vars, [ ... ])` | 2 107 |
| Scripts using `isglobal="TRUE"` | 0 |
| Games whose `<handlers>` block differ from the others | **0** (one byte-identical block in all 35) |

Every script body is exactly one of:

```js
return mitril.checkCondition(vars.items, [ <cond>, ... ], true|false);
return !mitril.checkCondition(vars.items, <cond>, false);
mitril.scriptWrap(vars, [ <task>, ... ]);
```

`<cond>` and `<task>` are JSON. Verified by parsing: all 3,395 `scriptWrap` bodies across the corpus
are valid JSON, and every referenced `localNNN_XXXXXXXXX` name is defined in the same paragraph.

### 1.2 Task classes actually used [M]

Method: parse every `scriptWrap` task array in the corpus and count `class` values.

| Class | Count | Class | Count |
| --- | --- | --- | --- |
| `set` | 3 282 | `nlg` | 37 |
| `inc` | 1 996 | `sub` | 28 |
| `dec` | 1 863 | `back` | 18 |
| `fun` | 229 | `rrr` | 12 |
| `if` | 200 | `save` | 6 |
| `del` | 185 | `rnr` | 6 |
| `eql` | 90 | `goto` | 3 |
| `add` | 76 | `achi` | 1 |
| `rnd` | 62 | `mlp` | 1 |

Not observed in the corpus: `div`, `rng`, `rrg`. `nul` is `executeCleanTask`, invoked directly
rather than as a task. The reference framework implements 22 classes in total; the device should
implement the 18 observed ones plus `div`/`rng`/`rrg` only if a concrete game needs them, and must
handle an unknown class by logging and continuing rather than failing the paragraph.

### 1.3 Conditions [M]

Two condition types are used inside `checkCondition`:

- `{"type":"k", "val":"<uid>"}` — a keyword flag is set.
- `{"type":"r", "val":"<uid>", "amnt":N, "eq":1?}` — a resource counter is `>= amnt`, or `== amnt`
  when `eq` is present. `amnt` may itself be another uid [C: `game_tpl.xml:190-195`].
- `{"or":1, "c":[...]}` — the array is combined with OR instead of the default AND
  [C: `game_tpl.xml:205-212`].

A third type, `{"type":"s","val":"<expression source>"}`, contains a **text expression**. It is used
by 4 of the 35 games: 289 expressions in total, 148 distinct [M].

Grammar observed across all 289 [M]:

| Element | Evidence |
| --- | --- |
| Identifiers `k<32hex>` / `r<32hex>` | 100 % of operands; the 32 hex characters are the parameter uid with dashes removed [M] |
| Comparisons `<` `<=` `>` `>=` `==` `!=` | `<` 152, `>` 172, `>=` 89, `==` 82, `<=` 54, `!=` 3 |
| Keywords `and` / `or` / `not` | `and` 184, `or` 76, `not` present |
| Parentheses | max depth 4, p90 depth 2 |
| Arithmetic `+` `-` `*` `/` `%` | 2 occurrences (one `+`, one `/`) |
| Function calls | **none** — the apparent matches were `and(`/`or(`/`not(` |
| Ternary `? :`, `in`, `**`, `^` | none |
| Expression length | min 37, median 85, p90 220, max 584 characters |

The reference implementation evaluates an `s` expression by building a name→value map from **all**
declared parameters (a missing parameter becomes `0`) and evaluating the source with the third-party
`expr-eval` library [C: `game_tpl.xml:156-179`]. The engine ignores an expression whose length is
not between 3 and 2000 characters; the corpus maximum is 584, so that guard never triggers [M].

**Design decision required:** the `s` type needs a real expression evaluator. See §5.

### 1.4 Rendering features present in real games [M]

Method: strip the platform `onload` script and `<handlers>` block from each of the 35 files, then
count constructs in the remaining game data.

| Construct | Games using it | Occurrences |
| --- | --- | --- |
| `<text spoiler="…">` | 6 | 46 |
| BBCode `[b]` | many | 5 694 |
| BBCode `[i]` / `[u]` | many | 380 / 101 |
| BBCode `[list]`/`[*]` | many | 40 / 27 |
| BBCode `[sup]` | many | 35 |
| BBCode `[player]` | many | 19 |
| BBCode `[url=…]` | many | 10 |
| BBCode `[color=…]` | many | 3 |
| `===` / `==` headings | many | 75 / 86 |
| `--` em-dash | many | 189 |
| `<music>` | **0** | 0 |
| `<cssdata>` / `<css>` | **0** | 0 |
| `<a>` links in text | **0** | 0 |
| `soundtrack` in setup | **0** | 0 |
| `checkpoints` in setup | **0** | 0 |
| `<action go="…">` (legacy attribute) | **0** | 0 |

So the device needs **BBCode lowering and spoilers**. Music, CSS and links can be ignored. Source
`<img>` elements are recognised but deliberately omitted from the first converted format: the ESP32
should not download, resize or render paragraph images in the initial implementation. Paragraph-image
support is a deferred rendering enhancement, not a requirement for the first runtime. `goto` is
always used; `go` never appears, but the reference accepts it and so should the parser.

### 1.5 Scale [M]

Method: measure every game reachable from the three list endpoints.

| Measurement | Value |
| --- | --- |
| Largest game file | 1 692 719 bytes (`game18152`) |
| Largest article count | 878 (`game17109`) |
| Largest single `<article>` | 343 625 bytes (`hockey_sssr`, the `mitril` bootstrap) |
| Articles over 64 KiB | 2 of 7 614 |
| Median article (two example files) | 1 186 / 947 bytes |
| Non-article overhead | 3.3 % / 4.6 % of the file |

The `mitril` article is the outlier in every game: 24–32 % of the file in the small examples, and
343 KB in the worst case. It contains the framework plus the parameter setup, and only the parameter
setup is needed to play.

---

## 2. The КвестБук HTTP contract [M]

All measurements in this section were taken against the live service.

### 2.1 Catalogue listing

```
GET https://quest-book.ru/online/api/list/<kind>/?page=<N>
```

| Item | Value |
| --- | --- |
| `<kind>` | `new` (newest), `editorial` (editor's choice), `best` (highest rated) |
| `page` | 1-based. `page=1` and `page=2` return different games; `page=0` behaves as page 1 |
| Page size | 12 items, fixed |
| Beyond the last page | `{"result":"ok","data":{"items":[],"is_more":false}}` |
| `is_more` | `true` while further pages exist |

Response shape:

```jsonc
{"result":"ok","data":{
  "items":[{
    "game_id":"19568", "index_name":"game19568", "fullname":"Портал Зла",
    "author_id":"5702", "author_name":"Дмитрий Дударь",
    "rating":"0", "created":"1792599955", "updated":"1793279401",
    "cover":"http://quest-book.ru/online/upload/5702_876e….jpg",
    "tags":"приключения, фэнтези, перевод, FightingFantasy",
    "desc":"Книга-игра из серии Fighting Fantasy. …",
    "likes":"6", "dislikes":"1", "level":"0",
    "last_played":0, "stars":3,
    "gameachis":[["","/online/images/promolevel1.png"]], "awards":[]
  }],
  "is_more":true}}
```

Field notes [M]:

- Almost every field is a **string**, including `likes`, `dislikes`, `stars`' siblings and numeric
  ids. `last_played` is the only integer; `stars` is an integer; `gameachis` and `awards` are arrays.
- `desc` is complete in this response: across 144 listed games the maximum is exactly 140 characters
  and none exceeds it, matching the 140-character cap on the platform's own description form. Five
  entries end in an ellipsis, but that is how the author wrote them, not response truncation.
  **No per-game detail request is needed** for the fields the device displays.
- `cover` is always an absolute URL on `quest-book.ru`. Over 60 listed games: extensions `jpg` 44,
  `png` 11, `jpeg` 4, **`gif` 1**; schemes split `http` 29 / `https` 31. The device keeps the value but
  never downloads it: it is the key of the on-device cover cache (§6.3).
- `tags` is one comma-separated string, 1–7 tags per game (mean 3.9). Tag names may contain spaces.
- `gameachis` and `awards` are promotion/achievement metadata the device does not need to show.

**There is no per-game detail endpoint** [M]: `api/game/<id>`, `api/item/<id>` and similar all
return empty or error. Everything the catalogue and detail screens need is in the list response.

### 2.2 Tag catalogue

```
GET https://quest-book.ru/online/api/tag/
```

Returns `{"result":"ok","data":{"groups":{…},"tags":{…}}}`:

- `groups` maps a group id to a name: `2=Жанр`, `12=Сеттинг`, `13=Список`, `16=Награды`,
  `17=Фанфик`, `18=Серия`.
- `tags` maps a tag id to `[name, group_id]`, 88 tags total (25 genres, 39 settings, and so on).
- One request returns the whole catalogue, so the tag screen needs no paging and no cache [M].

### 2.3 Listing games by tag [O]

The JSON API has no working tag filter [M]: `/online/api/list/where/?t[]=6` answers
`wrong request`, and `?tag=`/`?t[]=`/`?tags=` on `/online/api/list/new/` are accepted but silently
ignored — the response is byte-identical to the unfiltered list.

The HTML route does filter: `https://quest-book.ru/online/list/where/?t[]=6` returns a page
containing 12 game links for tag 6, but it is HTML-only, **not paginated** (`&page=2` returns the
same page), and unrelated to the JSON contract.

**This is the main blocker for the tag screen.** Options, needing a decision:

1. Ask the platform owner to add tag filtering to the JSON API (preferred — it is their service and
   the HTML route already has the capability).
2. Scrape the HTML route. Rejected as a design: it couples the firmware to page markup, and does not
   paginate.
3. Ship the tag screen against a filter that works for at most one page of results.

Until this is resolved, the tag screen can show the tag list (from 2.2) but cannot reliably list the
games carrying a tag beyond the first 12.

### 2.4 Substring search [M]

- The JSON API has no search endpoint; `api/search/`, `api/list/search/`, `api/find/` are empty or
  `wrong request`.
- `https://quest-book.ru/online/search/?q=<text>` returns HTML containing matching game ids (14 for
  `порт`), and `page=2` returns the same page. Same limitation as 2.3.

### 2.5 Downloading a game file

```
GET https://quest-book.ru/online/<index_name>/data/
```

| Item | Value |
| --- | --- |
| Content-Type | `text/xml; charset=utf-8` |
| Body | the game XML, `<?xml …?><book guid="…">…</book>` |
| `Content-Length` | absent — the body is sent chunked [M] |
| `Content-Encoding` | none unless requested; `Accept-Encoding: gzip` yields gzip [M] |
| `Accept-Ranges` | absent, and a `Range` request is ignored (HTTP 200 with the whole body) [M] |
| Reproducibility | byte-identical across repeated requests [M] |

Two consequences that shape the design:

- **Compression is available but unused.** With `Accept-Encoding: gzip` the same 715 024-byte game
  arrives as 177 748 bytes. The firmware currently never sends that header [M]. See §4.11 for why
  reading the response is not free on this device.
- **Partial download is not possible.** The server ignores `Range`, so a game cannot be fetched in
  pieces and a resumed download must restart. This is why the converted form is fetched as one
  sequential document (§4.3) rather than streamed paragraph by paragraph.

Also note the route is a catch-all: `/data/<anything>` returns the same XML, so a client cannot probe
for alternative formats by path [M].

**Use `index_name`, not `game_id`** [M]. For the current `new` list both work, because
`index_name` happens to equal `game<game_id>`; but real values include `lam`, `sf`,
`hockey_sssr`, `mest_torgovca`, `Firefighter` and `parabeda`. Requesting
`/online/game10628/data/` where `index_name` is `skeptiki1` returns a **163 KB HTML page with HTTP
200**, not a game. The loader must therefore validate the response, not trust the status code.

### 2.6 Transport posture

The firmware's HTTP client currently calls `setInsecure()` on every connection — there is no CA
bundle and no pinning [C: `src/network/HttpDownloader.cpp:29`, `src/util/PluginHttp.cpp:27-29`].
КвестБук is the project's own first-party service [C: `SCOPE.md:37-51`], so this is a decision to
take deliberately rather than inherit. Recorded as [O] in §8.

---

## 3. The game file format

### 3.1 Document structure [M]

```xml
<?xml version="1.0" encoding="UTF-8"?>
<book guid="a86a2525-330c-11ec-9c87-002590e2f74e">
  <article id="mitril"><script type="onload">…</script></article>
  <article id="1f0a24bf-…">…</article>          <!-- an ordinary paragraph -->
  …
  <article id="_mitril_triggers">…</article>
  <handlers>8 × <handler name="…">…</handler></handlers>
</book>
```

- Files are UTF-8 with no BOM.
- `<article>` elements are **flat children** of `<book>`; there is no nesting.
- `<handlers>` appears once, after the first article.
- Article ids are lowercase GUIDs, except exactly two: `mitril` and `_mitril_triggers` [M: confirmed
  in all 35 games].
- Paragraph order in the file is story order, **not** sorted id order [M].

A paragraph contains: `<text>` (one or more), optionally `<img>`, optionally `<script>`, and zero or
more `<action>`.

```xml
<article id="50d39f37-…">
  <text if="local1001_822868323">Ярость захлёстывает вас…</text>
  <text spoiler="Правила">…</text>
  <img name="article-img1">/online/player/mitril/images/50d39f37-…b331668cd3a2fd1468b3b867afc29cc2.jpg</img>
  <script type="local1001_822868323"><![CDATA[ return …; ]]></script>
  <action class="btnBlue" do="local1002_…" goto="a08d2d65-…">Текст кнопки</action>
</article>
```

- `<action>` uses `goto` (always present in the corpus), `do`, `if`, `ifnot`, `class`. The legacy
  `go` attribute does not appear but should still be accepted.
- `<img>`'s URL is its **text content**, not an attribute; `name` is only a CSS class.
- Image URLs are relative, always under `/online/player/mitril/images/`, and the file name is the
  owning article's GUID followed by 32 hex characters and an extension from `{jpg, gif, png}` [M].
- `<text>` bodies contain **no HTML** — only BBCode. HTML appears solely inside the platform's own
  CDATA scripts [M].

### 3.2 The `mitril` bootstrap and the start paragraph [M]

The `mitril` article is a single `onload` script containing three concatenated parts:

| Part | Size in the two examples |
| --- | --- |
| Framework bootstrap (`var mitrilSetup = (function(rawSetup){…})(`) | 2 158 / 2 161 bytes |
| `rawSetup` JSON — keywords, resources, functions, achis, settings, tags | 9 421 / 5 113 bytes |
| `mitrilController()` — the platform's own DOM/network layer | 25 222 / 27 102 bytes |

Two facts matter operationally:

1. **The start paragraph is only discoverable from this script.** It appears as
   `jsIQ.showArticle("<guid>")` inside the framework code, not in the markup. In the two examples it
   is `1f0a24bf-…` and `acc5841f-…` [M]. A converter must extract it, or the game cannot begin.
   The same literal appears twice (initial start and save-restore fallback).
2. **`mitrilController()` never needs to run.** It is fixed platform code — byte-identical in
   structure across all 35 games — implementing DOM manipulation, animation and server calls. Its
   only game-relevant exports are `checkCondition`, the task interpreter, `getItemChangesList`, the
   charsheet builder and the BBCode table [C: `docs/storygames/examples/game_tpl.xml`].

`rawSetup` is ASCII-only (Cyrillic is `\u04XX`-escaped), which makes it cheap to parse with a
byte-oriented JSON reader [M].

### 3.3 Parameters [M]

```jsonc
{"keywords": [ {"name":"…","uid":"<guid>","visible":"true","limit":"false",
                "items":[{"name":"…","uid":"<guid>","start":"0","limit":"0"}]} ],
 "resources":[ … same shape … ],
 "functions":[ {"func_uid":"…","func_name":"…","func_script":"[ …tasks… ]"} ],
 "achis":[…], "settings":[…], "tags":[…]}
```

- `keywords` groups are flags (`k`), `resources` groups are counters (`r`).
- `visible`, `limit`, `start` are **strings** (`"true"`/`"false"`/numeric), not JSON booleans.
- Corpus range: 0–322 parameter items; median 26 [M].
- `functions` embeds task arrays as JSON **strings** that must be parsed a second time.

### 3.4 Source data required for conversion [M]

These source components explain the startup data and the conversion boundary:

| Source item | Size |
| --- | --- |
| `<handlers>` block | 5 079 bytes; measured during format research, but not needed by the converter or copied to the output |
| Framework bootstrap | ~2 160 bytes; parsed on the server to locate `rawSetup` and the start paragraph |
| `rawSetup` | 5–16 KB; converted into header tables |
| `_mitril_triggers` article | 48–521 bytes; converted to the trigger paragraph ordinal/payload |

The device does not load the source bootstrap or handler block. It receives only the converted
header and paragraph payloads. In the source game, **about 87 % of the file is never needed at
start-up** and can remain on the SD card until the player reaches the relevant paragraph.

`_mitril_triggers` deserves emphasis: when it contains an `<action>`, the framework appends it to
**every** paragraph, which is how games implement a global rule such as "if health ≤ 0, go to the
death ending" [C: `game_tpl.xml:1179-1185`]. The runtime must reproduce that behaviour.

Under the server conversion of §4, the server extracts `rawSetup`, the start id and the
`_mitril_triggers` ordinal/flag into the converted header. The platform handler bodies are not copied
into the output: their behaviour is reimplemented natively and compatibility is maintained manually
(§4.4). The device simply reads the converted header at load.

---

## 4. Server conversion and the on-device container

### 4.1 Where conversion runs, and what it is for

The server converts, at quest-book.ru. This section defines the storage the device reads **at play
time**, which is the part that decides whether the feature is pleasant to use.

The design goal is not a smaller download. It is that every play-time operation is a bounded read
from a known offset:

| Play-time operation | Required lookup | How it is answered |
| --- | --- | --- |
| Show a paragraph | paragraph by id | `game.dat` offset from `game.idx` at `id × stride` |
| Evaluate `if` / `ifnot` on text or a choice | script **inside the current paragraph** | slot in the paragraph payload already in RAM |
| Run `do` before a transition | task list inside the paragraph | same |
| Follow `goto` | paragraph by id | index lookup, as above |
| `back` | paragraph by `lastarticle` | index lookup |
| Read a parameter in a condition | value by parameter ordinal | array index into `vars` |
| Call a function (`fun`) | task list by function ordinal | `game.hdr` table |

Two properties of the source format make this cheap, and both were verified rather than assumed:

1. **A paragraph is self-contained.** Every `localNNN_XXXX` referenced by `if`/`ifnot`/`do` is
   defined in the *same* `<article>`. Measured across the corpus: 11 007 references, **0** resolving
   to another paragraph, **0** script names defined twice [M]. So a paragraph payload carries its own
   scripts.
2. **Everything looked up is a small integer**, once the server renumbers ids (§4.5). There is no
   string search anywhere on the play path.

### 4.2 Files on the device

```
/<library>/storygames/<index_name>/
  game.hdr      header: start paragraph, parameter tables, functions, flags, record counts
  game.idx      paragraph index, fixed stride
  game.dat      paragraph payloads, back to back
  meta.json     catalogue fields needed for offline display (title, author, tags, description)
  state.bin     player state: vars, current paragraph, steps, lastarticle
  cover.bmp     the game page's cover: a 1-bit BMP of at most 128x192 (~3 KB when full size).
                Read by the game page; written by its Add to Library action after a successful import (§6.3)
```

The tree above is the **permanent** game layout. Catalogue actions also use the same game-content files
in one shared temporary workspace, `/<library>/storygames/___temp/` (without an `<index_name>`
subdirectory); `cover.bmp` is not one of them, because only Download writes it. That workspace is
opened by Play but is never registered as a library item. Its contents belong only to the currently
played temporary game; cleanup/replacement timing is specified in §6.3.

`/<library>` is the library root the firmware indexes today, which is the SD card's root — every
`/<library>/…` path in this document resolves relative to it.

`meta.json` is the one deliberate exception to the "XML everywhere" rule, and it is JSON for a
concrete reason: these fields originate in the catalogue's own JSON API (§2.1) and are read exactly
once, when the library is built — never on the play path — so re-encoding them buys nothing. It is a
flat object of short strings, far below the size where the 2 KB reader limit matters. Store only the
fields needed to identify and describe an offline game: title (`fullname`), author (`author_name`),
tags, and description (`desc`), plus identifiers needed for catalogue/service integration
(`game_id`, `index_name`) where useful. Popularity/rating counters (`likes`, `dislikes`, `stars`)
are not part of the on-device game metadata contract. The cover is not a metadata field either: it
is a separate 1-bit BMP beside the game, so a library rebuild never has to parse an image. Everything
the runtime reads per step is fixed-layout binary plus one payload slice.

### 4.3 The import document

The server exposes the converted game as one XML document, so the device performs a single
sequential import with no random access and no second request:

```
GET https://quest-book.ru/online/<index_name>/playtale/
Content-Type: text/xml; charset=utf-8
```

```xml
<?xml version="1.0" encoding="UTF-8"?>
<storygame v="1">
  <header>                                                        <!-- §4.4 -->
    <counts g="12" i="53" p="67" f="0" />
    …
  </header>
  <data>                                                          <!-- §4.6 -->
    <p n="0" f="1">…</p>
    <p n="1" f="0">…</p>
  </data>
  <meta>…</meta>                                                  <!-- catalogue fields -->
</storygame>
```

The loader walks the document once, writing `game.hdr`, `game.idx` and `game.dat` as it goes, and
writes `meta.json` at the end. The import is a straight sequential write, so its cost does not depend
on how the payload is encoded — the encoding only matters at play time.

Three requirements on this document:

- **It must be streamed, not buffered.** The largest game is 2.46 MB and one paragraph alone reaches
  72 KB; expat is fed in bounded chunks and never holds the document
  [C: `.agents/rules/architecture-hal.md:103-108`].
- **It must validate structural completeness using record counts.** `<header>` carries
  `<counts g="…" i="…" p="…" f="…" />`: group, item/parameter, paragraph, and function counts.
  These declared counts are the format's completeness check for v1: the loader counts the corresponding
  records while importing and compares the actual totals with the declared values. It also checks that
  paragraph-index end offsets agree with the number of bytes written to `game.dat`. A mismatch means
  the import is incomplete or structurally inconsistent, so the partial game is rejected. No content
  hash or checksum is calculated or required. This check does not detect arbitrary content changes
  that leave record counts and offsets consistent; detecting that kind of corruption is outside the
  v1 requirements.
- **`meta` must contain the catalogue fields needed for offline identification and description**:
  `fullname`, `author_name`, `tags`, `desc`, and the identifiers `game_id`/`index_name`. It does not
  need to preserve `cover`, `likes`, `dislikes` or `stars`; those fields are not required by the
  on-device game experience. The cover is not carried by the import document either: the game page
  fetches it from its own endpoint into its cover cache, and a downloaded game's own copy beside the
  game is written by Download (§6.3).

The original `.../data/` XML endpoint stays as it is. The converted endpoint is additive, so a
converter regression cannot break existing clients.

### 4.4 `game.hdr`

> **Shipped.** The byte layout the importer writes is documented in
> [file-formats.md](../file-formats.md), because it is current behaviour. It follows the table below
> with three differences, which that document states concretely: **no `Platform` field** (the runtime
> reimplements the handlers, so nothing carried a mitril flag), the **names are not one blob** (each
> record carries an absolute offset and a length into a streamed region), and the record strides are
> fixed at 16/16/32 bytes so every stride divides the 512-byte sector.

Everything needed before the first paragraph renders. Modeled on the existing `CLX1` header rather
than invented: magic + format version + explicit section offsets and sizes, all sections 512-aligned
so reads are aligned by construction
[C: `lib/LibraryIndex/LibraryFormat.h:9-21`, `:71-92`].

| Field group | Contents |
| --- | --- |
| Identity | magic, `formatVersion`, flags, the source `<book guid>` |
| Platform | whether a `mitril` bootstrap was present |
| Counts | `groupCount`, `itemCount`, `paragraphCount`, `functionCount` (from `<counts g i p f>` in the import document) |
| Start | `startParagraph` — the entry ordinal extracted from the framework (§3.2) |
| Triggers | the ordinal of `_mitril_triggers`, or absent (§3.4) |
| Groups | per group: name, type (`k`/`r`), `visible`, `limit`, and the **contiguous ordinal range** of its items |
| Params | per param: type (`k`/`r`), owning group ordinal, start value, limit, name |
| Functions | per function: ordinal, name, and its task list (§4.5) |
| Names | one string blob holding group and item names, for the character sheet |

Two deliberate choices:

- **Platform handler compatibility is maintained manually.** The runtime reimplements the fixed
  platform behaviour rather than interpreting the source JavaScript. The converted header does not
  store handler names or a fingerprint. If the platform handlers change, compatibility is reviewed
  and updated manually; automatic change detection is explicitly out of scope for this format version.
- **Items of a group are stored as one contiguous ordinal range.** The server is free to order
  parameters, so it can lay each group's items next to each other. Then `getGroupResourceCount` — the
  group-limit sum the task interpreter needs [C: `game_tpl.xml:233-253`] — is a loop over
  `[first, first + count)` with no membership test, and no per-param back-reference is needed.
- **Names stay in the header**, because only the character sheet displays them. The play path never
  touches a parameter name.

`limit` is encoded as a sentinel for "no limit", because the source distinguishes `false` from `0`
[C: `game_tpl.xml:266`, `:380`].

### 4.5 Numeric id normalisation

This is the change that removes strings from the play path. The server renumbers, per game:

| Source form | Converted to | Evidence it is safe |
| --- | --- | --- |
| Paragraph GUID in `goto`/`go` | paragraph ordinal | all targets resolve; 0 dangling in the corpus |
| Script name in `if`/`ifnot`/`do` | **slot number within that paragraph** | 11 007 refs, 0 cross-paragraph, 0 duplicate names |
| Parameter GUID in a condition or task `val` | parameter ordinal | 0–322 params/game, 0 unresolvable in the corpus |
| Parameter GUID in `amnt` (a value read from another param) | parameter ordinal | same |
| Function GUID in a `fun` task `val` | function ordinal | header table |
| `_mitril_triggers` | a header flag | §3.4 |
| Checkpoint uid in a `save` task | left as text, or dropped | see the note below |

The parameter renumbering has a further payoff: **`vars` becomes a flat integer array indexed by
parameter ordinal**. At the measured maximum of 322 parameters that is 322 × 2 bytes = **644 bytes**
for the whole game state, with O(1) access and no hash table, no key comparison, and no per-state
allocation. The reference semantics map cleanly onto it, because a missing parameter already behaves
as `0` [C: `game_tpl.xml:168`]:

- `k` flags: `0` = absent, `1` = set. The reference tests truthiness, so this is equivalent.
- `r` counters: integer, `0` when unset [C: `game_tpl.xml:188`].

Two source details the server must normalise, both verified across the corpus:

- **Conditions.** 7 436 use the array form (AND); 176 use `{"or":1,"c":[…]}`. Element fields are
  `type`, `val`, `amnt`, and `eq` where present. The server should emit a normalised list rather than
  re-encoded JSON.
- **Nested `if` tasks.** All 530 are `type:"64"` — base64-wrapped JSON holding
  `action_if`, `action_ifnot` and `action_script` (all three always present) [M]. Base64 must be
  decoded **on the server**, so the device never carries a base64 decoder for this purpose.

**Published games contain dangling parameter references, and they exist only inside base64.** In
`game18152` (the largest in the corpus) two `if` tasks reference parameter uids that appear nowhere
else in the file — not in the markup, and not in `rawSetup`'s parameter tables. They are visible only
after decoding the base64 payload, so a converter that scanned the raw XML would not see them at all,
and a reference implementation would leave `items[uid]` undefined.

This has two consequences:

1. **The converter must report unresolvable references rather than silently emitting `-1`.** The
   script does; four warnings on that game are correct behaviour, not a bug. Whoever runs it should
   expect them on real content and check whether the game still plays.
2. **The runtime must tolerate an out-of-range ordinal.** A missing parameter already reads as `0`
   [C: `game_tpl.xml:168`], so reading ordinal `-1` as `0` is the closest equivalent to the reference
   behaviour and is what the runtime should do. Rejecting the game outright would refuse a game that
   the platform itself plays.

Task field usage across the corpus (which classes carry which fields) is in Appendix D; it is the field
list the converter must emit, and the runtime must accept.

Note on `save`: 37 `save` tasks exist in the corpus, but **no game in it declares checkpoints**, and
`checkpoints` and `soundtrack` are absent from every `rawSetup` [M]. Server-side checkpoints are out
of scope for the first version, so a `save` task is a no-op offline; the converter should preserve it
so the information is not lost.

### 4.6 Payload syntax: XML

The transport is XML and the payload is XML: the device already ships expat and uses it for EPUB,
XHTML and OPDS, so one parser covers the whole import-and-play path. JSON was considered and rejected
for a specific, measured reason rather than a preference.

**Why not JSON.** `StreamingJsonParser`, the reader the project uses for OPDS and OTA, buffers each
string into a fixed token buffer and, when a string does not fit, emits **only the prefix and
discards the rest** [C: `freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp:299-323`;
`TOKEN_BUF_SIZE = 2048` at `.../include/StreamingJsonParser.h:30`]. Game text overruns that badly:

| Single text body or choice label | Median | p90 | p99 | Max |
| --- | --- | --- | --- | --- |
| Measured over 24 357 fields | 50 B | 1 006 B | 3 186 B | **8 802 B** |

**3.26 % of fields exceed 2 048 bytes** [M], so a naive reuse would silently truncate text rather than
fail. expat has no such cliff: it delivers character data in as many callbacks as it needs, and the
EPUB HTML pipeline already consumes split words that way
[C: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp:2310`, `:991`]. It is also configured with
`XML_CONTEXT_BYTES=0`, so it keeps no copy of the input [C: `platformio.ini:50`].

Verified against the vendored library rather than assumed [M]:

| Property | Result |
| --- | --- |
| Long text (14 KB) in one element | delivered, no size limit |
| The same text fed in 257-byte chunks | byte-identical result |
| CDATA content | arrives through the *same* character-data callback; no second handler |
| Leap seconds / parser reset for reuse | `XML_ParserReset` exists [C: `lib/expat/expat.h:278`] |
| A single `<p>…</p>` fragment, no XML declaration | parses; expat needs one root element, not a document prologue |

**The one trap this measurement found.** expat normalises attribute values but not element content:

| Where the text sits | Newline and tab survive? |
| --- | --- |
| In an **attribute** value | **No** — `\n` and `\t` become spaces |
| In **element content** | **Yes** — `\n` is preserved as `\n`, and `\r\n` / `\r` normalise to `\n` |

The corpus has **16 693 newlines and 404 tabs inside paragraph text**, and those newlines are
meaningful: the platform's BBCode converter turns `\n` into a line break
[C: `game_tpl.xml:1189-1226`]. **Therefore paragraph text and choice labels must be element content,
never attribute values.** Any field that carries prose is content; attributes are for numbers, flags
and short identifiers only.

Related measurement: no text field contains `]]>` (0 of 24 357), so CDATA is safe if ever wanted, but
escaping is far cheaper and is what the converter should do — only 161 characters in the entire
corpus need `&lt;`/`&gt;`/`&amp;` escaping, versus a 15-byte CDATA wrapper per field
(292 284 bytes of pure overhead across the corpus) [M].

**Paragraph payload.** One `<p>` element per paragraph, its ordinal as an attribute, its text as
content. This is the shape `playtale.php` emits, verbatim:

```xml
<p n="12" f="6">
  <s k="c" slot="0" or="0" crit="1" not="0">
    <e t="r" v="15" a="5"/>
  </s>
  <s k="c" slot="1" or="0" crit="0" not="1">
    <e t="k" v="4"/>
  </s>
  <s k="t" slot="2">
    <k c="inc" t="k" v="4"/>
    <k c="dec" t="r" v="15" a="5"/>
  </s>
  <t i="0" n="1" sp="Правила">Текст абзаца
вторая строка</t>
  <a g="42" d="1" i="0" c="btnBlue">Текст кнопки</a>
</p>
```

| Element or attribute | Meaning |
| --- | --- |
| `p@n` | paragraph ordinal (sanity check against the index) |
| `p@f` | bit flags: 1 ending (no choices), 2 reserved (always 0 in v1; paragraph images are omitted), 4 has scripts, 8 is the start, 16 is the triggers paragraph |
| `s@k` | `c` = condition, `t` = task list, `x` = unrecognised script kept verbatim |
| `s@slot` | the script's own index within the paragraph; `t@i`, `t@n`, `a@d` name it |
| `s@r` | role, present only when it is not a plain local script: `p` = preload, `u` = unload |
| `s@or` | 1 = the expressions are OR-ed, 0 = AND-ed |
| `s@crit` | the reference's `crit` argument: 1 expects the expressions true, 0 expects them false |
| `s@not` | 1 when the predicate was negated with a leading `!` |
| `e` | one condition expression: `@t` type, `@v` parameter ordinal, `@a` literal amount, `@ap` amount read from another parameter, `@eq` equality instead of `>=` |
| `e` with `@t="s"` | a text expression; its source is **element content**, with `k`/`r` identifiers already rewritten to ordinals |
| `k` | one task: `@c` class, `@t` type, `@v` operand, `@a` literal amount, `@p` parameter whose value is read |
| `t` | text block: `@i` if-slot, `@n` ifnot-slot, `@sp` spoiler heading, `@cl` class, `@id` element id; content is the text |
| `a` | choice: `@g` goto ordinal, `@d` do-slot, `@i`/`@n` condition slots, `@c` class; content is the label |
| `m` | a music track; content is the URL. Absent from every game in the corpus, preserved for completeness |

A conditional task nests instead of carrying a payload:

```xml
<k c="if">
  <if><c or="0" crit="1"><e t="k" v="14"/></c></if>
  <ifnot><none/></ifnot>
  <then><k c="inc" t="k" v="57"/></then>
</k>
```

`<none/>` records a branch the reference skips, which it does when the source string is two
characters or shorter (an empty `[]`). `<raw>` appears only when a payload does not decode.

Two attributes deliberately carry the reference's own parameter names (`or`, `crit`, `not`) rather
than a normalised form. Collapsing them into "all true"/"any true" is possible, but the reference
combines `crit` with a possibly-negated return in a way that is easy to get subtly wrong, so the
runtime implements the reference algorithm and the attributes carry its inputs.

Deliberately absent: no name survives anywhere on the play path — paragraph references, script
references, parameters, groups and functions are all ordinals — and no base64 or nested JSON string
survives conversion, because all 530 nested `if` tasks are decoded by the converter.

**Buffer bounds** this format must fit, measured [M]:

| Quantity | Median | p99 | Max |
| --- | --- | --- | --- |
| Paragraph payload bytes | 1 185 | 10 934 | 72 279 |
| Local script body bytes | 124 | 599 | 16 571 |
| Choice labels per paragraph | 1 | 10 | 156 |
| Text blocks per paragraph | 1 | 9 | 123 |
| Source images per paragraph (not emitted in v1) | 0 | 1 | 1 |

The source corpus contains paragraph images, but the v1 converter intentionally drops them: it does
not emit an image element, retain an image URL, or set the image flag. This avoids image downloads,
resizing and rendering costs on ESP32 devices with small displays. The `2` flag bit remains reserved
for format compatibility. Paragraph-image support may be added in a later rendering iteration, with
a separate decision about image delivery, scaling and device-specific presentation.

The index gives the payload length before the read, so the runtime sizes its buffer from the index
rather than guessing; the maxima above are what the occasional large paragraph needs.

### 4.7 `game.idx`

One fixed-size record per paragraph, addressed by ordinal with no key column and no search:

| Field | Width | Notes |
| --- | --- | --- |
| offset | u32 | byte offset into `game.dat` |
| length | u32 | payload length for this paragraph |
| flags | u16 | duplicated from the payload header, so a list view need not read the payload |
| reserved | u16 | keeps the record 16 bytes |

**Stride 16** — chosen for the same reason the library index uses 128: 16 divides 512, so 32 records
fill a sector exactly, every record read is 512-aligned by construction, and no record ever straddles
a buffer boundary [C: `lib/LibraryIndex/LibraryFormat.h:17-21`]. Paragraph *k* is at
`recordStart + 16k`.

Cost: 878 paragraphs × 16 = **14 048 bytes** at the measured worst case, against 25.5 KB for a keyed
design — because the ordinal *is* the key [M]. `length` also tells the runtime how much buffer a
paragraph needs before it reads one, which matters because paragraph sizes span three orders of
magnitude: median 1 185 B, p99 10 934 B, max 72 279 B, and 0.38 % exceed 16 KB [M].

### 4.8 `game.dat`

Paragraph payloads back to back, in offset order. Order is the server's choice, because the index
carries every offset; keeping them in ordinal order is simplest and makes a fresh index regenerable
by a single sequential pass.

Nothing in the file is self-describing beyond the payload header, by design: the schema is fixed and
versioned by `game.hdr`, so the runtime parses by schema rather than by inference. This is what keeps
the play path allocation-free.

### 4.9 Library integration

**Shipped.** A downloaded game appears in the Library beside the books, one row standing for the whole
game. Its row is titled and authored from the game's own `meta.json` when metadata reading is enabled —
the same setting that governs books, so with **Использовать метаданные книг** off the row shows the
folder name instead. That is what the existing index is for, and it is reused — but the reuse has a
boundary worth stating, because a naive reading of "put it in LibraryIndex" does not work:

- **The game goes into `CLX1` as one record**, exactly like a book. That buys, with no new code:
  library listing, arrival ordering, duplicate detection, and the path-hash identity
  [C: `lib/LibraryIndex/LibraryIndexFile.h:30-37`, `:66-93`].
- **Paragraph records cannot go into `CLX1`.** Its records are 128 bytes shaped for books (a 96-byte
  fold and a 12-byte author key), and `CLIX_MAX_RECORDS` is 4 096
  [C: `lib/LibraryIndex/LibraryFormat.h:49-54`, `:104-116`]. A single 878-paragraph game would consume
  21 % of the whole card's capacity, and the record shape carries nothing a paragraph needs.

So the paragraph index is a **sibling format that follows the same conventions** — magic, explicit
format version, section offsets, 512 alignment, fixed stride, and a pure layout module with no
`HalStorage`/Arduino dependency so it is host-testable [C: `lib/LibraryIndex/LibraryFormat.h:5-8`].
It does not share the file.

**The record is the game's directory.** `CLX1` stores a folder plus a name and rebuilds
`"<folder>/<name>"`, so the walk lists `/storygames/<index_name>` as one item: the folder is
`/storygames` and the name is the game's `index_name`. The choice matters three ways, and it is why
the "one canonical file" alternative was not taken:

- **The duplicate filter stays honest.** It keys on `(hash(name), size)`
  [C: `lib/LibraryIndex/LibraryBuilder.cpp:489-506`]. The window is per directory, so the durable reason
  for the directory-as-record is not the collision itself: it is that the record's name must be the
  game's own name, so the row title, `readPath()` and the dedup key are all right with no special case.
  A canonical `game.hdr` would have made every game share the name `game.hdr`.
- **The title fallback survives.** The builder's fallback is the file name without its extension; for
  `game.hdr` that is `game`, and for a directory name such as `Firefighter_2.1` `stemOf()` would cut
  the version off at the dot. The walk passes the directory name and `stageRecord()` skips the stem
  rule for a game.
- **Removing the row removes the game.** The Library's delete path hands the record's path to
  `removeBookFile()`; for a game it calls `questbook_game::removeGameAtPath()` instead, which deletes
  the tree rather than one file of it.

The walk recognises a game with `questbook_game::isGameAtPath()` and never descends into it: the
files are not books, and a half-imported game (no `meta.json`) is therefore not listed at all — the
row would open a game that cannot play. `game.hdr` supplies the record's size and modification time,
so a re-import invalidates the reused metadata instead of keeping a stale title.

**Title and author come from `meta.json`**, which is the game's own copy of the catalogue fields
[C: `lib/QuestBook/StorygameStore.h`, `readMetaFile()`]. The library walk is compiled on the host,
where ArduinoJson is not part of the build. The SDK's `StreamingJsonParser` would work there, but it
carries a fixed 2 KB token buffer and a SAX callback table, which the walk's memory budget argues
against for a file of six short strings, so `lib/QuestBook/StorygameMeta.h` reads it directly: no fixed
buffer, strings only as large as the values, and anything that is not the flat object the importer
writes is refused. A corrupt file falls back to the folder name and never hides the game.

**Opening a row opens the game page, not a reader.** `ReaderActivity::create()` would hand anything
that is not `.xtc` to the EPUB reader, so `ActivityManager::goToReader()` dispatches a game directory
to `StorygameDetailsActivity::forLibrary()`, which builds the page from `meta.json` [C:
`src/activities/ActivityManager.cpp`]. That page is also where the game is downloaded or removed, so
the library row and the catalogue row reach the same screen. The page carries a name of its own in
that case (`StorygameDetailsLibrary`), because `ActivityManager::goHome()` dispatches on the activity
name and Home must return to the Library rather than to the КвестБук row.

**What is still planned:** games never enter the recent-books store, because only the reader activities
add entries there. They are therefore absent from the pinned "recently opened" rows at the top of the
Recent tab and from the home screen's continue-reading slot, even though the Recent tab lists them with
the rest of the index. The Library has no separate games grouping — a game sits in the general lists like
any book, and its row carries the storygame glyph instead of the generic file icon.

### 4.10 What the device does at import

> **Shipped.** The sequence below follows `lib/QuestBook/StorygameImporter.cpp`. The authoritative account
> of what is written, and of the checks that accept a document, is
> [file-formats.md](../file-formats.md).

1. Open `/playtale/`, stream it with a bounded buffer, and write `game.hdr`, `game.idx` and
   `game.dat` in one forward pass (offsets are known as payloads are written, so no second pass).
   `game.hdr` is written out of order on purpose: the tables and the name stream first, and the core
   is written back over the reserved first sector last, so a reader never sees a header that
   describes sections which are not there yet.
2. Close those three files, then write `meta.json`. It merges the document's `<meta>` with the
   catalogue row the page already holds, because the captured documents omit `author_name`. Writing
   it last is the commit marker: a game counts as downloaded only when all four files exist.
3. Accept the document only if the tables declared in `<counts g="…" i="…" p="…" f="…" />` were
   filled exactly, the ordinals arrived in order, the root is `<storygame v="1">` and the served
   `<meta><index_name>` names the game that was asked for. Any failure deletes the whole directory.
   The importer does not calculate or verify a content hash/checksum, and it does not re-read
   `game.dat` to cross-check the index: `dataBytes` is the length it wrote. Arbitrary content
   corruption that leaves record counts and offsets consistent is outside the v1 scope.

The device does not parse the source game XML, extract `rawSetup`, discover the start paragraph,
validate references, or decode base64. All of that is the server's, done once per game instead of
once per device.

### 4.11 Compression

Compression is a transport detail and does not change any of the above, but it has one device-side
consequence worth recording: the firmware currently neither sends `Accept-Encoding` nor handles
`Content-Encoding`, and the fast inflater cannot read a gzip container [C:
`lib/miniz/third_party/miniz.h:36`], while `uzlib` can but wants a 32 KB ring
[C: `lib/InflateReader/InflateReader.h:53-60`]. Since the converted document is fetched once per
game, this is a second-order concern; the options are listed in §8 and it should not gate the format
work.

### 4.12 Why not an embedded database

SQLite is the natural instinct here and deserves an explicit answer, because it would be the right
tool for a different requirement.

**What it would give:** SQL, transactions, B-tree indices, a query planner, multi-reader safety.

**What the play path actually needs:** `payload WHERE id = ?` — one lookup by integer key in one
table. A fixed-stride array answers that with one multiplication and one read. A B-tree answers it in
O(log n) with page traversal, and the page cache is the RAM cost.

**What it would not remove:** the conversion. The XML still has to be parsed, renumbered and written
into rows; only the container changes.

**What it would not provide here:** "what games are already loaded" is already answered by the
filesystem and by `CLX1` (§4.9); that does not need a second store.

**Costs:** page cache (the dominant term, realistically several KB to tens of KB), statement state,
and the SQLite amalgamation in flash — the largest dependency the project would have added, for an
operation that is an array index. On a device with 380 KB total, where a TLS session already demands
≥ 40 KB free and ≥ 20 KB in one block, and where repeated `malloc`/`free` is the documented enemy
[C: `.agents/rules/hardware-resources.md:42`], this is the wrong trade.

**When it would become right** — recorded so the choice is revisitable rather than permanent:
full-text search across paragraphs, cross-game queries, per-column updates, concurrent readers, or a
single transactional store for many games' state. None is required now.

The conclusion is not "databases are wrong" but "this is an indexed container, not a database".

---

## 5. Runtime design

### 5.1 What must be reimplemented

| Component | Source of truth | Notes |
| --- | --- | --- |
| Condition evaluation | `game_tpl.xml:156-231` | `k`, `r`, `{or,c}`, and `s` |
| Task interpreter | `game_tpl.xml:255-929` | 18 classes observed |
| Task wrapper semantics | `game_tpl.xml:962-983` | what `scriptWrap` does around a task list |
| BBCode lowering | `game_tpl.xml:1189-1226` | 12 constructs, all observed |
| Item-change diff | `game_tpl.xml:931-960` | drives the character sheet; visible groups only |
| Charsheet data | `game_tpl.xml:997-1063` | must be built natively, not as HTML |
| Start/save semantics | `game_tpl.xml:1233-1258` | offline: always "no save found" |

The remaining ~60 % of `game_tpl.xml` is DOM chrome, animation, and server calls that the device
replaces with its own rendering and its own save file.

### 5.2 Rendering requirements from real games

- **BBCode lowering** for `[b] [i] [u] [list] [*] [sup] [color=…] [url=…] [player]`, `===`/`==`
  headings, `--`, and newlines (§1.4). `[url=…]` and `[color=…]` are rare: the first can be shown as
  its text, the second as plain text, rather than requiring styling on-device.
- **Spoilers**: 46 uses in 6 games. A spoiler is text hidden behind a heading until the reader opens
  it. The reference engine ignores the attribute entirely — it is the web portal that renders it
  [C: `STORYGAME_TECH.md` §6.1] — so this is a device-side presentation decision with no reference
  behaviour to copy. A list row per spoiler, opened by Confirm, is the natural fit.
- **Paragraph images are deferred.** Although source games may contain `<img>`, v1 conversion
  drops these elements and their URLs (§4.6). Supporting them later requires a separate design for
  image delivery, resizing and rendering on small device displays; predictable source URLs alone do
  not make that work suitable for the ESP32.

### 5.3 The expression evaluator

The `s` condition type needs a real evaluator; a stub that returns `false` (as the offline analysis
port does) silently breaks 4 of 35 games.

**Grammar required** (§1.3): `k`/`r` identifiers resolving to integers, six comparison operators,
`and`, `or`, `not`, parentheses, and optional `+ - * / %`. No function calls, no ternary, no `in`,
no `^`, no string literals, no assignment.

**Candidate libraries.** All three suggested candidates were examined against this firmware's build
constraints. The repository compiles C++20 with `-fno-exceptions` and no RTTI
[C: `platformio.ini:54`, `:120`, `:122-124`].

| Candidate | Verdict |
| --- | --- |
| [exprtk](https://github.com/ArashPartow/exprtk) | **Rejected.** 1 661 427 bytes / 46 766 lines in one header; 12 `throw`, 10 `try`, 18 `catch`, 76 `dynamic_cast`, 880 `std::string`. Its size alone is disqualifying on a device whose whole usable heap is 380 KB, and it cannot build without exceptions. |
| [muparser](https://github.com/beltoforion/muparser) | **Rejected.** Throws its own exception types (`muParserError.h` includes `<stdexcept>`), needs several source files plus a build-system integration, and carries `<iostream>`, `<locale>` and `<map>` in its public headers. Making it exception-free means patching the vendored copy. |
| [tinyexpr++](https://github.com/Blake-Madden/tinyexpr-plusplus) | **Rejected.** 136 177 bytes / 3 642 lines in `tinyexpr.cpp` plus a 1 328-line header; 69 `throw` in the `.cpp` and 7 in the header, plus `<stdexcept>`, `std::variant` and `std::span`. It also requires C++20 library facilities (`std::span`) not guaranteed on this toolchain. |
| [tinyexpr](https://github.com/codeplea/tinyexpr) (the C original) | **Closest fit, but still not a drop-in.** 25 155 bytes / 758 lines, zlib licence, **zero** `throw`/`try`/`catch`, no RTTI, includes only `<ctype.h> <limits.h> <math.h> <stdio.h> <stdlib.h> <string.h>`. It has a `TE_MAX_DEPTH` limit (default 512) and accepts `&&`/`||`/`!` but **not** `and`/`or`/`not` [M]. It binds variables through a pre-built `te_variable[]` array registered before compiling, and it `malloc`s one node per expression element. |

**Consequence.** No candidate satisfies the constraints as-is. `tinyexpr` is by far the closest, and
three adaptations make it viable:

1. **Keyword operators.** Either pre-translate `and`/`or`/`not` to `&&`/`||`/`!` in a scratch copy of
   the expression (safe: no identifier contains those words, and only `k<hex>`/`r<hex>` operands and
   operators appear), or extend the vendored tokeniser. Pre-translation is a ~20-line pass and keeps
   the vendored copy pristine.
2. **Variable binding.** `tinyexpr` needs every variable registered with a pointer before
   `te_compile`. A game has at most 322 parameters [M]; registering 322 `te_variable` entries
   (each a name plus a `double*`) is bounded and needs no lazy resolver. **Note the type mismatch:**
   `k` flags are present/absent and the reference evaluates a missing value as `0`, while `r`
   counters are integers. The device should materialise a `double` array for the parameters a given
   expression references — at most 9 in the corpus, median 2 [M] — and register only those, which
   keeps the per-expression table tiny.
3. **Heap allocation per parsed node.** `tinyexpr` builds its syntax tree with `malloc` — its
   `new_expr` helper allocates once per node and `te_free` frees the tree recursively [M: one
   `malloc` call site, invoked once per node, in `tinyexpr.c`]. On the C3, repeated new/free in a
   loop is exactly the pattern the resource protocol warns against
   [C: `.agents/rules/hardware-resources.md:42`], and a condition is evaluated on every paragraph
   render. Adaptation options: compile once per distinct expression string and cache the tree, or
   give the vendored copy a fixed arena via a custom allocator. Either way this is real work, and it
   is the strongest argument against vendoring at all.

**However**, given the grammar is this small — no functions, no ternary, no power, max nesting depth
4 — a purpose-written recursive-descent evaluator is roughly 150–250 lines of C++, needs no heap, no
`double` (integer arithmetic suffices, since every observed operand and constant is an integer), no
library, and no licence notice. It would also be testable in the existing host test suites without
vendoring anything.

**Recommendation:** decide explicitly between (a) vendoring `tinyexpr` with the three adaptations,
and (b) a small purpose-written evaluator. The trade is a well-tested third-party parser plus a
licence notice, a keyword-operator pass, an allocator change and per-expression caching, against
full control and no dependency for a grammar already known to be closed. This document does not make
the call; it records that the three suggested candidates are all unusable on this build as-is, and
that the C original is the only one worth considering.

### 5.4 Save state

The reference model is `{article, savedata, steps, lastarticle}` uploaded after every transition
[C: `game_tpl.xml:1139-1156`]. Offline, the device writes the same shape locally. Storage should
follow the existing per-book cache conventions under `/.crosspoint/` rather than introducing a new
root [C: `AGENTS.md` naming rules; `docs/file-formats.md`].

Two semantics must be preserved:

- `vars.steps` counts paragraph entries and is incremented on each non-bootstrap entry.
- A restored save must **not** re-run the paragraph's `preload` script, or its effects are applied
  twice [C: `game_tpl.xml:1233-1258`].

Server-side features — achievements, checkpoints, restart, soundtrack — have no offline equivalent
and are out of scope for the first version. The corpus shows `checkpoints` and `soundtrack` are
**unused by every game studied** [M], and `achi` appears once in 11 007 scripts [M], so this costs
nothing today.

### 5.5 Transitions to a paragraph that is not there

A `goto` or `<a>` may name a paragraph the container does not carry: a game can be converted from
source with a dangling reference, and a payload can fail to read or parse at runtime. The runtime
treats both as a **dead link**, not as a fatal error: `Engine::moveTo` loads the target before it
touches the current paragraph, and on failure it returns without changing state, so the reader keeps
the paragraph it was on and its remaining choices stay usable. Only the entry paragraph is fatal —
a game whose `start` paragraph cannot be loaded does not start.

This is a deliberate departure from the reference engine, which would show an empty article. On a
device with no way back to the catalogue other than leaving the reader, silently staying put is the
friendlier outcome [C: `lib/QuestBook/StorygameEngine.cpp` `moveTo`/`choose`].

---

## 6. Device UI

> **Partly shipped.** §6.1's catalogue, game-page and reader rows describe code that now exists:
> `src/activities/browser/QuestBookCatalogActivity.*`, `StorygameDetailsActivity.*` and
> `src/activities/reader/StorygameReaderActivity.*`, with the home menu entry that opens the first.
> The shipped catalogue offers the `new` and `editorial` lists only and shows no covers in its rows;
> tags and search remain unbuilt. The game page shows a cover, **downloads**, **plays**, and removes:
> its second action imports the game into `/storygames/<index_name>/` and removes that directory again
> on request (§6.3), and Play opens the runtime reader — downloading the game with the same import
> first when it is not on the card. A downloaded game is also one row of the Library (§4.9).
> Progress persistence and the `___temp` play workspace are still plans (§6.3).

### 6.1 Screens

| Screen | Base | Responsibility |
| --- | --- | --- |
| КвестБук catalogue | `CatalogActivity` | list of games with paging; **shipped** for `new` + `editorial`, no tags/search entry yet |
| Game page | own activity, `freeink::ui::publicationPage` | name, author, tags, description, likes/dislikes, stars, the cover, and **Play / Add to Library / Remove from Library**; **shipped**, Play opens the runtime reader |
| Tag list | `UiListActivity` | the tag catalogue from §2.2; selecting one lists its games |
| Tag results | catalogue list with a filter | [O] blocked on §2.3 |
| Search | `CatalogActivity::launchSearch()` | [O] blocked on §2.4 |
| Reader | own activity | the game itself: paragraph text formatted from BBCode, choices, spoilers, character sheet; **shipped** |

### 6.2 Why `CatalogActivity`

`CatalogActivity` already owns the whole flow this feature needs: Wi-Fi check and selection, the
search keyboard, loading and error screens, download with progress and cancel, and the auto-sleep
policy [C: `src/activities/CatalogActivity.h:40-57`]. It is an abstract base with four existing
subclasses (`OpdsBookBrowserActivity`, `PluginCatalogActivity`, `PluginHubInstallActivity`, and now
`QuestBookCatalogActivity`), so the catalogue follows an established pattern rather than adding a new
one.

### 6.3 Why the game page is not `DetailsActivity`

The shipped page is its own activity (`StorygameDetailsActivity`), built on the same
`freeink::ui::publicationPage` component `DetailsActivity` uses. `DetailsSpec` could have carried most
of this page (`source` for the tags, `metadata` for the facts, `description` for the text, an empty
action label for no button), and one extra field — a line count for the tags — would have closed the
gap. The page is separate anyway, for two reasons:

- **The fork diff stays out of an upstream file.** `DetailsActivity` came from upstream (OPDS 2.0, then
  the stack-safety fix in `f73d788a`); a storygame-only field there means carrying it through every
  upstream merge, while a separate file merges as "added by us".
- **No HTML layout pass.** The service's description is plain text capped at 140 characters (§2.1), so
  the page needs none of `DetailsActivity`'s styled-description machinery and none of its `More`
  viewer.

The cost is a second screen with the same shape: if upstream changes `publicationPage`'s contract,
both must be revisited. The page carries two action buttons; the storage behavior of the second one,
which downloads and removes, is defined below.

| Required | Where it goes |
| --- | --- |
| `fullname` | `props.book.title` |
| `author_name` | `props.book.author` |
| `tags` | `props.book.series` (the hero's extra text line) |
| `likes` / `dislikes` / `stars` | `props.metadata`, as `Rating 4/5 · Likes: 6 · Dislikes: 1` |
| `desc` | `props.description` |
| `cover` | the cache key for the hero slot's image |

#### The cover

The page reserves a hero slot of at most 128×192 and fills it with a cover, or with the storygame icon
when there is none (`lib/QuestBook/QuestBookCover.h`, `StorygameDetailsActivity::paintCover`). The slot
is reserved whether or not an image resolves, so the page has exactly one layout and the text reaches
the panel before any transfer starts.

The service converts the catalogue's `cover` URL itself, so the device never downloads the original
upload and never runs an image decoder for a cover:

```
GET https://quest-book.ru/online/cover/<index_name>/?w=128&h=192
Content-Type: image/bmp
Body         a 1-bit BMP for a slot of at most 128x192: 62-byte header + 1 bpp rows,
             3 134 bytes when the source is 2:3 and nothing smaller is needed
404          no cover for this game
```

The endpoint preserves the source aspect ratio and never upscales, so a wide or square upload arrives
shorter than 192 rows; the page centres whatever it gets in the slot.

Resolution order, one attempt per visit:

1. `/<library>/storygames/<index_name>/cover.bmp` — the copy beside a downloaded game, written by the
   page's Add action right after a successful import. It is read first so a downloaded game keeps its
   cover offline and across an emptied cache.
2. `/.crosspoint/qbcover/<md5(cover)>_v1_128x192.bmp` — the screen-slot cache.
3. The endpoint above; the downloader stages the body and the file is kept only after it passes the
   cover library's check — 1 bpp, within the slot, and exactly as long as its own header and row count
   declare. The same check guards both local tiers, so a truncated file is never drawn.

The first two tiers are resolved without a radio, so a page opened offline still shows a cover it
already has; only the third needs a session.

`cover` is therefore retained by the catalogue parser (`QuestBookListParser::Row::cover`) as the cache
key, not as an address to fetch: the URL carries the upload's own random suffix, so a re-uploaded
cover is a new key with no separate invalidation step. The field is an identifier rather than display
text, so a URL that exceeds its cap is dropped whole instead of truncated — a cut URL would hash onto
another game's key. Without a key there is no cache entry and no fetch either, so such a game shows the
icon until the catalogue changes. A failed conversion (404) is not cached, so a game with no cover is
retried on a later visit rather than showing a broken image forever.

#### Download and Play actions

> **Both shipped.** The page's second action is **Add to Library** (Download described below): it
> fetches the converted document and imports it into `/<library>/storygames/<index_name>/`, then the
> same button becomes **Remove from Library**, which asks for confirmation and deletes that directory.
> **Play** opens the runtime reader directly; when the game is not on the card yet, Play first runs the
> same import and opens the reader when it succeeds, so for now a catalogue Play also leaves the game as
> a permanent library item. The reserved `___temp` workspace below is still the plan for a later Play
> that does not. The importer's exact behaviour (streaming, the counts check, the rejection rules) and
> the byte layout it writes are current behaviour, documented in
> [file-formats.md](../file-formats.md).

The game page exposes two distinct actions. Both fetch the converted `/playtale/` document using
`index_name` (§2.5, §4.3), then import it into the on-device file layout (§4.2). They differ in
whether they open the reader immediately:

- **Download (Add to Library)** stores the game persistently at
  `/<library>/storygames/<index_name>/`. The game remains available for later offline play. Downloading
  a game that is already present replaces its files in that directory — in the shipped page, Add is
  offered only while the game is absent, so replacing means Remove and then Add. The game then appears
  in the Library as one row (§4.9).
- **Play** opens the reader from the downloaded game. When the game is absent, Play performs the same
  import and opens the reader on success, so a catalogue Play currently also leaves the permanent
  library entry. The planned behaviour below — importing into the shared temporary workspace
  `/<library>/storygames/___temp/` so a catalogue Play does not create a library item — is not
  implemented yet.

The `___temp` directory would be a single shared workspace, not a per-game directory. A Play operation
that uses it must therefore treat its contents as belonging only to the game currently being played and
must not register that workspace as a library item. The reader itself reads from its opened workspace
and never from another game's directory (the engine's `ParagraphSource` is bound to one open
`StorygameGame`). The exact cleanup policy (for example, clearing it before the next Play operation
and/or when leaving the reader) is an implementation detail to settle when it is built; it must not
affect games stored in permanent `<index_name>` directories.

Covers are fetched per game page, from the converted endpoint above. The cover file is at most ~3 KB
of 1-bit BMP, so it neither competes with the retained catalogue page nor needs a decoder; an
unavailable cover leaves the slot's icon, never delays the first frame and never fails the page.

### 6.4 Resource constraint that shapes the navigation

On the ESP32-C3 a TLS transfer needs ≥ 40 KB free heap and ≥ 20 KB largest block, and decoding an
original cover would add ~20 KB (JPEG) or ~44 KB (PNG) on demand [C: `src/network/HttpDownloader.h:34-35`,
`lib/Epub/Epub/converters/`]. A child activity retains its parent [C: `.agents/rules/ui-activities.md`],
so the catalogue's parsed page and a cover transfer overlap.

OPDS solves the decode overlap by releasing its entries before fetching a cover and re-fetching the
catalogue when the detail page closes [C: `src/activities/browser/OpdsBookBrowserActivity.cpp:866-869`].
The shipped КвестБук flow keeps the page instead of re-fetching, and pays for that with a budget it can
state: a page is at most sixteen rows of capped strings (title 160 B, author 64 B, tags 128 B,
description 288 B, cover 96 B, plus ids), against the ~40 KB a fresh TLS session needs. That figure is
arithmetic from the parser's caps, not a measurement — 16 × ~784 B of fields is ~13 KB before
`std::string` and allocator overhead, so expect ~15–18 KB, still small against the session and
unverified on a device.

The cover itself costs no decoder: the fetched body is a 1-bit BMP read row by row by
`GfxRenderer::drawBitmap1Bit`, and the transfer is a 3 KB body over the Wi-Fi association the
catalogue already established — the HTTP client opens its own connection per call, so what is shared
is the association, not a session. That transfer is the one remaining peak: it runs on the main task
after the first frame is on the panel, so the page's text is visible before the session allocates, and
it is attempted once per visit. It also blocks that task, so input is unresponsive for the duration of
a transfer; the catalogue's own fetches have the same property. The release of those rows takes the
render lock, because a queued repaint may still be reading their strings.

### 6.5 Stars have no glyph

Built-in fonts cover no symbol block that contains `★` (U+2605); the compiled intervals stop before
the geometric-shapes and miscellaneous-symbols ranges [C: `lib/EpdFont/scripts/fontconvert.py:45-142`].
The SD-font `symbols` preset does include that range [C: `lib/EpdFont/scripts/fontconvert_sdcard.py:66-68`],
but SD fonts are optional and cannot be assumed.

Therefore the rating is rendered as text, `Rating 4/5`, on the game page. The list rows carry no
rating at all.

### 6.6 New strings and a new menu entry cost real work

- Every new user-visible string goes to `lib/I18n/translations/english.yaml` and requires running the
  generator [C: `docs/i18n.md:123-156`]. Seven keys were added for this section
  (`STR_QUESTBOOK`, the two list names, rating, likes, dislikes, and the description heading), and six
  more for the page's actions: Play, Add to Library, Remove from Library, the removal question, and the
  two failure lines. That is the whole cost of a feature that speaks to the reader: each of them has to be
  translated for every locale, and until it is, that locale falls back to English
  [C: `scripts/gen_i18n.py:224-237`].
- A new entry on the home screen is an enum value plus hand-maintained index arithmetic in
  `HomeActivity::menuItemToIndex`, `indexToMenuItem` and `getMenuItemCount`, which are easy to
  desynchronise [C: `src/activities/ActivityManager.h:21`,
  `src/activities/home/HomeActivity.cpp`]. **Decided:** the КвестБук entry leads the menu
  unconditionally, so it needs no settings flag; in themes that put «Продолжить чтение» in the menu it
  sits behind that row, and it is still the first constant entry. The cover-grid home's tab table
  follows the same order, and a theme that pages a tall menu now exposes its page start
  (`BaseTheme::homeMenuPageStart`) so the home screen's touch targets follow the drawn rows. The order
  is now stated in five places (the two converters, the count, the render vectors, and the cover-grid
  tab table); collapsing them into one table is the next change that adds an entry, not this one.

---

## 7. Scope check

Against `SCOPE.md`:

- КвестБук download, accounts and storygame play are explicitly in scope [C: `SCOPE.md:91-93`].
- The service is the project's designated single online source [C: `SCOPE.md:37-51`], so no new
  third-party connector is added.
- Replacing the scripting-engine assumption with a task interpreter **reduces** flash and RAM
  pressure, which serves the project's stated priorities [C: `SCOPE.md:53-64`].
- Catalogue cover caching is a separate UI concern, now settled (§8 #5); it is not part of the
  paragraph-image payload, which is omitted from the v1 storygame format.

The one genuine scope risk is feature creep in the runtime: the character sheet, spoilers and BBCode
are all required by real games, but each adds rendering surface. Paragraph images are deliberately
excluded from v1. The mitigation is to implement exactly the twelve BBCode constructs measured in
§1.4 and no more.

---

## 8. Open questions

| # | Question | Blocks |
| --- | --- | --- |
| 1 | ~~How a multi-file game appears in `CLX1`: one canonical file, or a directory-as-item rule (§4.9)~~ **Decided and shipped:** the directory is the item (§4.9) | — |
| 2 | Who owns and maintains the server converter, and how its format version is tracked against the platform framework (§4.1) | Everything downstream |
| 3 | JSON API for listing games by tag (§2.3) | Tag screen results beyond one page |
| 4 | JSON API for substring search (§2.4) | The search screen |
| 5 | ~~Catalogue cover handling: keep the current no-cover experience, re-download per view, or add a cache keyed by `game_id`? This is separate from paragraph images, which are omitted from v1 (§4.6).~~ **Decided:** the service converts the cover to a 1-bit BMP of at most 128×192 (`/online/cover/<index_name>/?w=&h=`), the device caches it under `/.crosspoint/qbcover/` keyed by `md5(cover)`, and Download will keep a copy beside the game (§6.3). Separate from paragraph images, which are omitted from v1 (§4.6). | — |
| 6 | TLS posture for a first-party service: keep `setInsecure()`, add a CA bundle, or pin | Security review |
| 7 | ~~Expression evaluator: vendor `tinyexpr` with adaptations, or write the small evaluator (§5.3)~~ **Decided and shipped:** a small handwritten recursive-descent evaluator in `lib/QuestBook/StorygameEngine`; no third-party engine is added | — |
| 8 | Compression: `uzlib` + 32 KB ring, a server switch to raw deflate, or neither (§4.11) | Download flow, memory |
| 9 | ~~Game file root under the library (§4.2)~~ **Decided:** permanent games use `/<library>/storygames/<index_name>/`; catalogue Play uses the shared `/<library>/storygames/___temp/` workspace (§6.3) — the `___temp` part is still to build; Play currently imports into the permanent directory | — |
| 10 | ~~Home-screen placement for the КвестБук entry (§6.6)~~ **Decided:** the first constant entry of the home menu | — |
| 11 | ~~Whether the storygame screen reuses the reader's paginated layout or needs its own view (§5.2)~~ **Decided and shipped:** its own activity (`StorygameReaderActivity`), a vertically swipe-scrollable paragraph+choices column, not the paginated book reader | — |
| 12 | Should `STORYGAME_TECH.md` be translated to English and corrected to match this study? | Documentation |

---

## 9. Suggested sequencing

1. **Catalogue only** — `QuestBookCatalogActivity` and the game page. Useful on its own, no runtime,
   no server dependency. Validates the catalogue contract and the memory plan. **Shipped:** the
   catalogue (two list kinds, paging), the game page and its download/import (§6.3).
2. **Settle the remaining format question** — who owns the converter (question 2), now that how a game
   appears in `CLX1` is decided and shipped (§4.9). It is cheap to decide now and expensive to change
   later.
3. **Server converter** — implement §4.4–§4.8 on quest-book.ru and publish the XML document at
   `/playtale/`, alongside the existing `/data/` XML.
4. **Device loader** — stream the document into `game.hdr`/`game.idx`/`game.dat`, verify, register the
   game in the library. **Shipped** (`lib/QuestBook/StorygameImporter.*`, §4.9).
5. **Minimal runtime** — text, choices, conditions, tasks, saves. Enough to play a simple game.
6. **Rendering extras** — BBCode, spoilers and character sheet. Paragraph images are deferred to a
   later enhancement and are not part of the initial converted format or runtime.
7. **Tags and search** — once questions 3 and 4 are answered.

Steps 3–5 are where host-side tests pay off: the corpus is downloadable, the format is declarative,
and the reference implementation in `docs/storygames/examples/` can generate expected results for
comparison.
That makes the runtime testable without hardware, which is unusual for this project.

---

## Appendix A — Corrections to earlier assumptions

Recorded because they changed the plan:

| Earlier assumption | What the data shows |
| --- | --- |
| "A scripting engine must be chosen and sized" (`ROADMAP.md` Phase 2) | No JS engine is needed; the format is declarative (§1.1) |
| `?page=N` pagination was broken | Fixed by the platform; `?page=1` is the first page (§2.1) |
| Games are "easily 1 MB" | Largest observed is 1.69 MB; largest single paragraph 343 KB (§1.5) |
| `type:"s"` expressions are unused | 289 expressions in 4 of 35 games; a false stub breaks them (§1.3) |
| A per-game detail request would be needed | The list response already carries every displayed field (§2.1) |
| Download by `game_id` | Must use `index_name`; the wrong form returns HTML with status 200 (§2.5) |
| Music, CSS and links need handling | None appear in any of the 35 games (§1.4) |
| Spoilers could be ignored | 46 spoilers in 6 games (§1.4) |
| The device must convert XML at download time | The server can convert instead, which removes the parser, the validator and the 343 KB worst case from the device (§4) |
| Conversion is mainly a bandwidth win | It is a device-simplicity win: the server's existing gzip already gives most of the bandwidth (§4.11) |
| A converted format needs a 16-byte key per paragraph | Sequential server-assigned ordinals remove the key and the binary search: 16 bytes per paragraph including flags, with no key at all (§4.7) |
| JSON is the natural converted format | It was the first choice, then rejected: the project's existing JSON reader **drops** any string over 2 048 bytes, and 3.26 % of text fields exceed that. XML and expat have no such limit and are already shipped (§4.6) |
| A paragraph's text could be an XML attribute | No: expat normalises newlines and tabs in attribute values to spaces, and 16 693 newlines in the corpus are meaningful. Prose must be element content (§4.6) |
| A paragraph's scripts might live elsewhere in the file | They never do: 11 007 references across 35 games, 0 cross-paragraph, 0 duplicate names (§4.1) |
| Games could go into the existing library index as-is | The game does; its 878 paragraph records cannot — `CLX1` caps at 4 096 records and its 128-byte shape is built for books (§4.9) |
| An embedded database would help | The play path is `payload WHERE id = ?`; a fixed stride beats a B-tree and avoids a page cache on a 380 KB device (§4.12) |
| Partial or resumed download is available if needed | The server ignores `Range` and sends chunked with no `Content-Length` (§2.5) |

## Appendix B — Measurement provenance

- **Corpus**: 35 distinct games from `/online/api/list/{new,best,editorial}/`, downloaded during this
  study. 7 614 articles, 11 007 local condition/effect scripts, 2 107 preload scripts, 24 357
  string fields.
- **Format analysis**: `docs/storygames/examples/gamedata-example1.xml`, `gamedata-example2.xml`, and
  the reference implementations under `docs/storygames/examples/` (`jsiq-engine.js`, `dom.js`,
  `game_tpl.xml`).
- **Live contract**: HTTP responses recorded in §2, including pagination boundaries, tag catalogue
  contents, cover extension census, download content types, gzip behaviour and `Range` handling.
- All byte counts are byte-accurate on the raw UTF-8 buffers. Character counts would understate
  article sizes by about 26 %, because roughly a quarter of the characters are two-byte Cyrillic.
- Three methodological traps worth repeating for whoever builds the converter:
  1. Measure in bytes, not characters.
  2. Mask CDATA before scanning for tags — the platform's JavaScript contains `&lt;` and `<div>`.
  3. **Decode entities before judging text content.** `<text>` looked like pure BBCode until entity
     decoding revealed real `<h6 style=…>` markup in one game. A raw-tag scan finds nothing.

## Appendix C — The converter

`playtale.php` in `docs/storygames/examples/` is the converter. It is PHP 5.3 compatible, takes a game
XML and writes the `/playtale/` document described in §4.3 and §4.6. The response root is
`<storygame v="1">`.

```
php playtale.php <input.xml> [output.xml] [options]
```

With no output path it writes to stdout. The options (`--name=`, `--author=`, `--index=`, `--tags=`, `--desc=`, `--game_id=`) fill the
`<meta>` section. `--author=` supplies `author_name`; the converter recovers `game_id` and `index`
from the bootstrap when they are not given. Cover and popularity/rating fields (`--cover=`,
`--likes=`, `--dislikes=`, `--stars=`, `--level=`, `--created=`) are not required in the on-device
metadata contract.

What it does, in order:

| Step | What it does |
| --- | --- |
| Split | Reads the file once and splits it on `<article>` boundaries; the prologue yields `<book guid>` |
| Bootstrap | Extracts `rawSetup`, the start paragraph and the platform scalars from the mitril article's JavaScript |
| Number | Assigns paragraph, group, parameter and function ordinals |
| Rewrite | Replaces every GUID and script name with an ordinal, including `k…`/`r…` identifiers inside `s` expressions |
| Decode | Decodes base64 nested `if` tasks and the JSON inside them |
| Emit | Writes `<header>` (tables and start), `<data>` (one `<p>` per paragraph) and `<meta>` |

Two implementation notes that matter if it is ever rewritten:

- **It parses by string search, not with a DOM.** It runs on the server, where a 2.4 MB document is
  nothing. `<article>` elements are flat children of `<book>`, and the literal `<article` never
  appears inside the CDATA of any published game, so the delimiters are unambiguous. It counts start
  tags against split results and warns on a mismatch, so a future export that nests articles or
  embeds those literals is reported rather than silently truncated.
- **Scripts are extracted before anything else.** The platform's own JavaScript contains HTML inside
  its CDATA, so scanning for `<text>`/`<action>` in the raw article would match that markup.

An earlier throwaway prototype produced the size figures quoted in §4 and was deleted after the
format decision. Its findings, which the converter inherits, were: every script in the corpus reduces
to a declarative shape; the start paragraph and `rawSetup` are extractable mechanically; task lists
are valid JSON in all 3 395 observed cases; and all 530 nested `if` tasks are base64 around a
three-key object.

What the converter does **not** do, and the service around it must:

- Serve the document over HTTP, and choose the route (proposed: an additive `/playtale/`).
- Keep the converter/runtime implementation compatible with platform framework changes. There is
  no handler fingerprint in the converted header; changes are reviewed and handled manually when
  needed (§4.4).

## Appendix D — Task and condition schema

The field sets the converter must emit, measured over every `scriptWrap` task list in the corpus
(3 395 lists, no unparseable ones) [M]. A field not listed for a class does not occur for it.

| Class | Count | Fields present |
| --- | --- | --- |
| `set` | 3 686 | `type`, `val`, `amnt` |
| `inc` | 2 893 | `type`, `val`, `amnt` (2720) |
| `dec` | 2 269 | `type`, `val`, `amnt` (2240) |
| `fun` | 1 097 | `type`, `val`, `amnt` (229) |
| `if` | 530 | `type`, `val`, `amnt` (200) — always `type:"64"`, base64 |
| `del` | 290 | `type`, `val`, `amnt` (185) |
| `eql` | 183 | `type`, `val`, `amnt`, `param` |
| `rnd` | 168 | `type`, `val`, `amnt` (62) |
| `add` | 83 | `type`, `val`, `amnt`, `param` |
| `achi` | 81 | `val` (plus `type`/`amnt` once each) |
| `nlg` | 56 | `type`, `val`, `amnt` |
| `rnr` | 45 | `type`, `val`, `amnt`, `param` |
| `save` | 37 | `val` (plus `type`/`amnt` 6 times) |
| `sub` | 37 | `type`, `val`, `amnt`, `param` |
| `back` | 18 | `type`, `val`, `amnt` |
| `rrr` | 18 | `type`, `val`, `amnt`, `param` |
| `mlp` | 12 | `type`, `val`, `amnt`, `param` |
| `rng` | 3 | `type`, `val`, `amnt`, `param` |
| `goto` | 3 | `type`, `val`, `amnt` |
| `div` | 2 | `type`, `val`, `amnt`, `param` |

Not observed: `rrg`, and `nul` (which is `executeCleanTask`, called directly rather than as a task).
The runtime should implement the observed classes, treat an unknown class as a logged no-op rather
than a failure, and preserve `save`/`achi` even though both are inert offline.

**Conditions.** Element fields are `type`, `val`, `amnt`, and `eq` where present:
7 436 arrays in AND form, 176 objects in OR form (`{"or":1,"c":[…]}`); `eq` appears 3 252 times.
`val` may reference a parameter directly (the common case) or, in the `s` type, be an expression
source string (§1.3).

**Nested `if` tasks.** All 530 decode to an object with exactly three keys — `action_if`,
`action_ifnot`, `action_script` — all three always present, each a JSON string; `[]` means absent
[C: `game_tpl.xml:805-829`]. The server decodes the base64 and the inner JSON, so neither reaches the
device.

**Maximum shape sizes**, for buffer sizing: 33 tasks in one list, 9 distinct parameters referenced in
one `s` expression, and no `s` expression longer than 584 characters [M].


