# Сторигеймы КвестБук: полное описание функциональности

## 1. Термины и общая картина

- **Книга-игра** — произведение, подготовленное автором вне сайта (бумага/PDF/TXT), публикуется как книга.
- **Сторигейм** — интерактивное произведение, созданное **редактором КвестБук** и существующее как онлайн-игра. Отличие от книги-игры: механика может быть полностью «спрятана» от игрока, но она есть (ключевые слова, ресурсы, скрытые счётчики, случайности).
- **jsIQ 2.1.2** — универсальный браузерный движок-интерпретатор: читает XML-книгу, строит параграфы, исполняет скрипты, гоняет события. Исходник — [jsiqapi212.js](<jsiqapi212.js>) (⚠ файл в **CP1251**, не UTF-8 — при чтении нужен `TextDecoder('windows-1251')`; UTF-8-копия движка-порта лежит в [jsiq-engine.js](<jsiq-engine.js>)).
- **mitril 1.4** — НЕ часть движка, а «фреймворк поверх движка», который **сам является частью книги**: это огромный скрипт в параграфе `<article id="mitril">` плюс блок `<handlers>` в конце книги. Именно mitril даёт группы параметров, «лист персонажа», достижения, чекпойнты, сохранения, BBCode-разметку. Шаблон вырезан в [game_tpl.xml](<game_tpl.xml>).
- **Файл-модуль игры** — то, что скачивается с сервера (`.../online/game<ID>/data/`) и открывается движком: XML-книга, содержащая и контент, и весь код механики.

Схема слоёв:

```
XML-книга
 ├── <article id="mitril">   ← bootstrap: mitrilSetup (JSON настроек) + контроллер mitril + старт игры
 ├── <article id="...">      ← параграфы: text / img / action / script
 ├── <article id="charsheet"> (опционально) — HTML-шаблон листа персонажа
 ├── <article id="_mitril_triggers"> — параграф-«присадка» к каждому параграфу
 └── <handlers><handler name="..."> — обработчики событий книги (8 шт.)
```

---

## 2. Два формата входного файла

| Формат | Признак | Обработка |
|---|---|---|
| Чистый XML | начинается с `<?xml`, `<book`, или `<тег ...` | используется как есть |
| Сохранённая страница «View Source» | `<!DOCTYPE html>` / `<html>` со `<span id="lineN">` и HTML-эскейпом внутри | из `<body>` вырезаются `<script>`/`<style>`/все теги, затем декодируются сущности |

Реализация: `looksLikeRawXml` / `looksLikeViewSource` / `extractXmlFromViewSource` в [jsiq-hoster-app.js:18–37](<jsiq-hoster-app.js#L18-L37>), `decodeEntities` в [dom.js:11–28](<dom.js#L11-L28>). Пример «сырого» формата — `gamedata_wrong.xml`.

---

## 3. Структура XML-модуля-файла

### 3.1 Корень

```xml
<?xml version="1.0" encoding="UTF-8"?>
<book guid="d737acdc-f77c-11ef-9cc7-002590e2f74e">
  ...
</book>
```

`guid` — идентификатор книги. Внутри `<book>` могут лежать: `article*`, `handlers`, `cssdata`, `#charsheet`.

### 3.2 Параграф — `<article>`

```xml
<article id="50d39f37-f740-11ef-9cc7-002590e2f74e">
   <text>...</text>
   <img name="article-img1">/online/player/mitril/images/....jpg</img>
   <music>...</music>
   <script type="preload"><![CDATA[ ... ]]></script>
   <script type="local1001_822868323"><![CDATA[ return ...; ]]></script>
   <action class="btnGreen" if="local1001_..." do="local1002_..." goto="...">Текст кнопки</action>
</article>
```

- `id` — уникальный UID параграфа; **все переходы идут по UID**, а не по номерам. Специальные id: `mitril` (bootstrap), `charsheet` (шаблон листа), `_mitril_triggers` (триггеры).
- Дочерние элементы читаются только как **прямые дети** (`$(node).children(...)`), а `action` ищется по всему поддереву (`find('action')`).
- Порядок дочерних элементов в XML не важен для логики, важен для рендера текста (тексты выводятся в порядке следования).

### 3.3 Текст — `<text>`

| Атрибут | Смысл |
|---|---|
| `if="script"` | блок показывается, только если скрипт вернул истину |
| `ifnot="script"` | блок скрывается, если скрипт вернул истину |
| `class` | CSS-класс контейнера |
| `id` | DOM-id контейнера |
| `spoiler="Заголовок"` | платформенный «спойлер»: текст-заголовок, по клику раскрывается содержимое. **Движок jsIQ этот атрибут игнорирует** — обрабатывает портал/шаблон сайта |

Содержимое — CDATA или экранированный текст; оно проходит через BBCode-конвертер mitril `prepareText()` (см. §7.6). Условных `<text>` в реальных играх очень много: в `gamedata4.xml` — 1124 штуки с `if` и 42 с `ifnot`; в `battle.xml` — 13/4; в `gamedata2.xml` — 24/1.

### 3.4 Кнопка-выбор — `<action>`

| Атрибут | Смысл |
|---|---|
| `goto` / `go` | UID параграфа, куда ведёт выбор (`goto` приоритетнее, `go` — легаси) |
| `do` | имя скрипта-запроса, исполняемого **до** перехода; если вернул `false` — переход отменяется и параграф перерисовывается |
| `if` | условие видимости (скрипт-запрос) |
| `ifnot` | обратное условие видимости |
| `class` | стиль кнопки: `btnDefault`, `btnGreen`, `btnRed`, `btnBlue` и т. п. |
| `id` | DOM-id |
| текст внутри | подпись выбора |

Всего в играх тысячи `action` (например, `gamedata2.xml` — 495, из них 47 с `if`, 46 с `ifnot`, 34 с `do`).

### 3.5 Скрипты параграфа — `<script type="...">`

- `type="preload"` — исполняется **до** рендера параграфа (здесь обычно живут изменения параметров).
- `type="onload"` — исполняется **после** рендера (в `mitril`-параграфе — весь bootstrap).
- `type="unload"` — исполняется при уходе из параграфа.
- `type=""` (пустой/без атрибута) — исполняется немедленно при создании параграфа.
- `type="localNNNN_XXXXXXXXX"` — **именованный локальный скрипт-предикат**, возвращающий значение; на него ссылаются атрибуты `if`/`ifnot`/`do`. Авторский редактор генерирует такие имена автоматически.
- `isglobal="TRUE"` — скрипт регистрируется в глобальной таблице `jsIQ.scripts` и доступен **всем** параграфам (binds-механизм). В проверенных играх `isglobal` не встречается ни разу, но движок его поддерживает.

Особенности обработки ([jsiq-engine.js:677–772](<jsiq-engine.js#L677-L772>)):

1. **Проход 1** — собираются *имена* скриптов (чтобы ссылки `if="localN"` резолвились, даже если скрипт объявлен ниже).
2. **Проход 2** — текст скрипта оборачивается в функцию `wrapScript()` и исполняется; результат (для именованных) кладётся в `article.scripts[type]`.
3. Если объявлено **несколько** `preload` — сохранится только последний (`scripts[type] = script` перезаписывает). В реальных книгах `preload` в параграфе всегда один.

### 3.6 Ссылки внутри текста — `<a>`

Внутри `<text>` могут быть `<a do="script" go="uid" goto="uid">…</a>`. Обработчик `on_render_article` вызывает `makeLinkActionable()`, который:
- ставит `href="#"`, снимает атрибуты `do/go/goto`;
- по клику: сперва исполняет `do` (при `false` — перерисовка и отмена), затем переход `go/goto`, иначе перерисовка.

В оффлайн-движке клик не эмулируется, но семантика перехода сохранена ([jsiq-engine.js:534–555](<jsiq-engine.js#L534-L555>)).

### 3.7 Изображения, музыка, стили

- `<img name="...">URL</img>` — имя + адрес; собираются в `article.imgs`, рендерятся платформенным обработчиком `on_render_images`.
- `<music>...</music>` — трек параграфа, собирается в `article.music`.
- `<cssdata><css name="...">…</css></cssdata>` — стили книги; в браузере вставляются в `<head>` как `<style id="atril-style-NAME">`, в оффлайн-движке игнорируются.
- Саундтрек игры задаётся в настройках mitril и грузится отдельным AJAX к `/online/mitril/soundtrack/`.

### 3.8 Обработчики событий — `<handlers>`

```xml
<handlers>
  <handler name="on_text_render"><![CDATA[ ... process.next(); ]]></handler>
  ...
</handlers>
```

Тело оборачивается как `handler_<name>` и регистрируется через `addEventHandler`. Список в mitril-книгах всегда один и тот же (8 штук):

| Обработчик | Что делает |
|---|---|
| `update_charsheet` | пересобирает «лист персонажа» из `mitril.getCharsheetData(vars.items)` по шаблонам `getTpl('charsheet'/'charsheet-group'/'charsheet-item-k'/'charsheet-item-r'/'charsheet-checkpoints')` |
| `on_render_images` | вставляет картинки параграфа в контейнер текста |
| `on_article_preload` | сохраняет игру (`mitril.saveGame`) и инкрементирует `vars.steps` |
| `on_text_render` | прогоняет текст через `mitril.prepareText()` (BBCode → HTML) |
| `before_article_unload` | пишет `vars.lastarticle = env.params.current` (для задачи `back`), затем анимация ухода |
| `on_render_article` | `mitril.checkGameEnd(vars)` — детект финала |
| `after_article_loaded` | завершение анимации загрузки |
| `jsiq_init_done` | вешает обработчик `on_action_render`, добавляющий галочки к видимым кнопкам |

---

## 4. Конвейер исполнения jsIQ

### 4.1 Инициализация книги

```
jsIQ.init(xml)                                    // jsiq-engine.js:416
 └ triggerEvent('jsiq_init_start')
   └ loadBook(xml)                                // :377  (в браузере — $.ajax)
     └ triggerEvent('jsiq_book_loaded')
       ├ addArticle(book)                         // :160 — книга + вырезание #charsheet
       ├ регистрация <handlers><handler>          // :393
       ├ применение <cssdata><css>                // :404
       └ callback → initDone()                    // :431
         └ triggerEvent('jsiq_init_done')
```

### 4.2 Показ параграфа (главный конвейер)

`showArticle(uid, startparams)` ([jsiq-engine.js:851](<jsiq-engine.js#L851>)) — строго последовательная цепочка событий, где каждый следующий шаг запускается только через `process.next()`:

```
showArticle(uid)
 ├ before_article_unload   {current, next}   ← здесь фиксируется vars.lastarticle
 ├ on_article_unload       {current, next}   ← исполняется script type="unload"
 │   (перекурс с noUnLoadScript=true)
 ├ проверка articleExists(uid) → иначе triggerError('article not found')
 ├ createArticle(uid)      ← создание объекта параграфа, компиляция скриптов
 ├ on_article_preload      {article, noPreLoadScript} ← script type="preload"
 │    └ before_render_article {article, text, actions, title}
 │         └ renderArticle()
 │              ├ <text> по порядку → createText().render()  (условия if/ifnot!)
 │              ├ <action> по порядку → createAction().render() (условия if/ifnot!)
 │              ├ on_text_render      (linked-значения, BBCode/платформенный парс)
 │              ├ on_render_article   (makeLinkActionable для <a>)
 │              └ on_render_images    (картинки)
 │         └ on_article_onload {article, last, noOnLoadScript} ← script type="onload"
 │              └ after_article_loaded
```

Флаг `rendering` защищает от повторного входа в рендер (двойной рендер отбрасывается).

### 4.3 Модель процессов и окружение скриптов

- Любое событие — это **процесс** с очередью обработчиков. Обработчик обязан вызвать `process.next()`, иначе цепочка встанет. `process` доступен внутри скрипта, только если он был передан.
- `ScriptEnv` ([jsiq-engine.js:211–236](<jsiq-engine.js#L211-L236>)) — объект окружения: `name`, `params`, `parent`, `vars = getGlobs`, `process`, `from` (кто инициировал: кнопка/ссылка).
- Внутри любого обёрнутого скрипта доступны ([jsiq-engine.js:583–627](<jsiq-engine.js#L583-L627>)):
  - `vars` — **глобальное состояние игры**;
  - `arg` — параметры вызова (`env.params`);
  - `process` — текущий процесс события (если есть);
  - `local` (`this`) — локальные значения параграфа;
  - `env` — сам объект окружения;
  - `triggerError(msg, params)` — локальный помощник;
  - имена всех скриптов книги как функции (`binds`-механизм) — можно вызывать `local1002_xxx()` прямо из другого скрипта.
- Любая ошибка исполнения перехватывается обёрткой и уходит в `triggerError` → событие `jsiq_error` (не роняет страницу).

### 4.4 Как резолвятся скрипты из атрибутов

Три уровня, в порядке приоритета ([jsiq-engine.js:635–647](<jsiq-engine.js#L635-L647>)):

1. `current_article.scripts[name]` — локальный скрипт текущего параграфа;
2. `jsIQ.scripts[name]` — глобальный (`isglobal`);
3. `getLocalChecker(name)` — проверка «локальной переменной `this[name] === true`».

Дальше — три формы вызова:

| Функция | Когда используется |
|---|---|
| `articleScriptQuery("name?k=v;k2=v2\|other?k=v", env)` | атрибуты `if`/`ifnot`/`do`. Строка режется по `\|`; каждая часть — `имя` + необязательные параметры через `?` и `;`. Итог — логическое И по всем правдивым результатам |
| `articleScript("name", extargs, env)` | если нужны параметры как объект; иначе уходит в Query |
| `articleScriptInEnv(env, name)` | исполнение в уже готовом окружении |

Важное следствие: результат `do` сравнивается строго с `false` — переход отменяется только при явном `return false`, а `undefined`/`null`/`0` переход **не** отменяют.

---

## 5. Состояние игры: `vars`, сохранения, чекпойнты

### 5.1 Глобальное состояние

`jsIQ.getGlobals()` — единственный «сейв» игры. В mitril-играх там ровно три поля:

| Поле | Смысл |
|---|---|
| `vars.items` | **все игровые параметры**: `{uid: значение}`. Флаги — `true`/отсутствие; счётчики — числа |
| `vars.steps` | счётчик шагов; инкрементируется в `on_article_preload` при каждом входе в параграф (кроме bootstrap-параграфа `mitril`) |
| `vars.lastarticle` | UID предыдущего параграфа; ставится в `before_article_unload`, обнуляется задачей `back` |

Функции движка: `reCreateGlobals()`, `setGlobals(glob)`, `getGlobals()`.

### 5.2 Старт новой игры

В хвосте `mitril`-параграфа ([game_tpl.xml:1233–1258](<game_tpl.xml#L1233-L1258>)):

```js
window.mitril.getSavegame(function(result) {
  if (!result) {                       // сохранения нет
    vars.items = {};
    mitril.executeCleanTask(vars.items);   // проставить start-значения всех параметров
    vars.steps = 0;
    jsIQ.showArticle("<startUID>");        // стартовый параграф, зашитый в игру
    jsIQ.triggerEvent('update_charsheet', {update:true});
  } else {                             // сохранение есть
    vars.items   = result.savedata;
    vars.steps   = parseInt(result.steps) || 0;
    vars.lastarticle = result.lastarticle || undefined;
    jsIQ.showArticle(result.article || "<startUID>", {noPreLoadScript:true});
    jsIQ.triggerEvent('update_charsheet', {update:true});
  }
});
```

Здесь принципиальны два параметра `showArticle`:
- `noPreLoadScript: true` — при восстановлении сохранения `preload`-скрипт параграфа **не** исполняется (иначе повторно начислились бы эффекты);
- `noUnLoadScript` / `noOnLoadScript` — аналогичные выключатели для `unload`/`onload`.

### 5.3 Автосохранение

`mitril.saveGame(article, vars)` ([game_tpl.xml:1139–1156](<game_tpl.xml#L1139-L1156>)):

```js
{ article: <UID текущего параграфа>,
  savedata: {...vars.items},          // глубокая копия
  steps: vars.steps,
  lastarticle: vars.lastarticle }     // если задан
```

- Кладётся в `lastSaveData`, готовится тикет, затем **дебаунс 250 мс** → `mitrilJs.uploadSavegame(game_id, prod, lastSaveData)`.
- Вызывается автоматически из обработчика `on_article_preload` — то есть **после каждого перехода**. Слот сохранения один (текущая позиция игрока), плюс отдельные слоты-чекпойнты.
- `saveGame(jsIQ.current_article.title, varslink)` вызывается ещё и из `scriptWrap` — то есть после любого изменения параметров.

### 5.4 Загрузка сохранения

- `getSavegame(callback)` → POST `/online/mitril/loadgame/` с `game_id`, `prod`, `testuid`; ответ `{result:'ok', data:{savedata, steps, lastarticle, article}}` либо `false`.
- `loadCheckpoint(checkpoint_uid)` → POST `/online/mitril/loadgame/` с `checkpoint`; при успехе: прячет блок финала, ставит `vars.items = result.savedata`, `vars.steps`, `vars.lastarticle`, вызывает `showArticle(result.article)` и `update_charsheet`.

### 5.5 Чекпойнты (контрольные точки)

Чекпойнты объявлены в `rawSetup.checkpoints` — это массив:

```json
{ "uid": "a130f9f2-...", "name": "1",
  "vars": [ { "uid": "a1049475-...", "name": "1" } ] }
```

Механика:
- Внутриигровая задача `class:"save"` с `val = "<checkpointUID>--<variantUID>"` шлёт POST `/online/mitril/savegame/`; при `result == 'ok'` выставляет `items[checkpoint] = true`.
- `getCharsheetData()` собирает блок `checkpoints`: для каждого чекпойнта ищет пройденный вариант (`vars.items[uid + '--' + variant.uid] == true`), состояние `available` / `preview` / `skipped` (все предыдущие `preview` становятся `skipped`), а `update_charsheet` рисует иконки и **полосу прогресса**: `progress = round(visited / (checkpoints.length + 0.3) * 100)`.
- Клик по доступному чекпойнту → `loadCheckpoint(uid)`, то есть откат состояния к сохранённому слоту.

### 5.6 Рестарт

`restartGame(callback)` → `confirm('Начать игру с начала?')` → POST `/online/mitril/restart/` → перезагрузка страницы. Кнопка `.end-go-restart` привязывается в контроллере.

### 5.7 Достижения

`executeAchiTask` (задача `class:"achi"`, `val` = 1..4): POST `/online/mitril/achi/add/`, при успехе — всплывающее уведомление с иконкой и названием из `rawSetup.achis`. Для анонимных пользователей достижения не сохраняются (ответ `fake`).

### 5.8 Оффлайн-модель (для бота/анализа)

В [jsiq-engine.js:92–111](<jsiq-engine.js#L92-L111>) серверные вызовы заменены заглушками:
- `$.ajax` всегда отдаёт `{result: 'error'}` → `getSavegame` считает, что **сохранения нет**;
- `mitrilJs.uploadSavegame` / `gameEnd` / `prepareTicketData` / анимации — no-op;
- достижения и `save`-чекпойнты не срабатывают (AJAX-ответ не ok).

Практический вывод: оффлайн-состояние всегда соответствует **«анонимному игроку без сохранений»**, игра всегда начинается с `executeCleanTask` + стартовый UID. Это важно явно проговаривать агенту, который будет сравнивать логи бота с реальным поведением игрока.

---

## 6. Скрываемые блоки текста и «лист персонажа»

### 6.1 Условный текст

Механизм ровно один — атрибуты `if` / `ifnot`, значение которых — имя скрипта (обычно локального предиката):

```xml
<script type="local1001_822868323"><![CDATA[
  return !mitril.checkCondition(vars.items, [{"type":"r","val":"4c01...","amnt":3}], false);
]]></script>
<text ifnot="local1001_822868323">Ярость захлёстывает вас...</text>
```

Рендер ([jsiq-engine.js:787–803](<jsiq-engine.js#L787-L803>)):

- `if` истинно → контейнер `.textContainer` + класс видимости;
- `if` ложно → контейнер `.nonVisible`, текст пустой, `text.visible = false`;
- `ifnot` истинно → `.nonVisible` и `visible = false`;
- при повторном рендере класс снимается, если условие снова позволило показ.

Аналогично для кнопок ([jsiq-engine.js:479–515](<jsiq-engine.js#L479-L515>)): невидимые становятся `actionContainer.nonVisible` и `action.visible = false`; видимые получают `.Visible` и добавляются в контейнер выборов. Событие `on_action_render` получает `check: 'if' | 'ifnot'`.

Отдельный уровень — `spoiler="Заголовок"`: содержимое в DOM присутствует всегда, но визуально скрыто до клика. Это **не** условный блок; движок jsIQ его не интерпретирует.

### 6.2 «Лист персонажа» (charsheet) и видимость параметров

Каждая группа параметров в `rawSetup` имеет флаг `visible`:

- `visible: true` — группа попадает в лист персонажа (`getCharsheetData`), её элементы видит игрок;
- `visible: false` — скрытая механика (сюжетные маркеры, служебные счётчики, «1 из N» рулетки), в лист не попадает.

При этом в лист попадают только **непустые** значения, а также выводится `stored` — сумма по группе (для `k`-групп — количество, для `r` — сумма модулей) и `limit` группы, если он задан.

История изменений листа (`#charsheet-change-history`) формируется `getItemChangesList()` и **тоже фильтруется по `visible`**: скрытые параметры не показываются игроку даже как «параметр изменился». Формат записи: для счётчиков `Группа/Имя: 12 → 15 (15)`, для флагов — жирное имя (получено) или зачёркнутое (потеряно).

Именно этот фильтр воспроизводит анализатор: `itemChanges()` в [jsiq-hoster-app.js:276–294](<jsiq-hoster-app.js#L276-L294>) пропускает в лог только параметры групп с `visible === true`. Примеры из проверенных игр: `gamedata2.xml` — 9 видимых из 14; `gamedata4.xml` — 52 из 188; `gamedata.xml` и `gamedata3.xml` — 0 видимых (все группы скрыты).

### 6.3 Связанные значения (`linked`)

Дополнительный механизм подстановки значений прямо в HTML-шаблоны:

- в тексте/шаблоне есть элемент с классом `.linked` и `name="<ключ>"`;
- обработчик `on_text_render` собирает такие элементы и зовёт `updateValuesByList()`;
- значение берётся в порядке: `linked_cache` → `linked_vars` → `vars[name]` → `current_article.runLocal(this[name])` → глобальное `vars[name]`; результат кэшируется в `linked_cache` (сбрасывается на `before_article_unload`);
- публичный API: `jsIQ.linkValue(id, value)`, `unlinkValue(id)`, глобальные `curArticle()`, `run(name, params, env)`, `showValue(name, value)`.

Это то, чем в браузере живут динамические подписи вида «У вас N патронов» без условных блоков. В проверенных восьми играх `linked` не используется (шаблоны листа персонажа приходят с портала), но движок это умеет — знать полезно.

---

## 7. Фреймворк mitril: модель игровых параметров

### 7.1 `mitrilSetup` — конфигурация, вшитая в книгу

В `onload`-скрипте параграфа `mitril` лежит IIFE, которая парсит **гигантский JSON `rawSetup`** (передаётся прямо в коде вызова) и строит `mitrilSetup`:

```
mitrilSetup = {
  groups:     { uid: {uid, name, type:'k'|'r', visible, limit, items:[uid...]} },
  items:      { uid: {uid, name, group, start, limit} },
  functions:  { func_uid: [задачи...] },
  checkpoints:[ {uid, name, vars:[{uid,name}]} ],
  len, ends, prod, index, game_id,
  soundtrack (опц.)
}
```

Поля `rawSetup`: `keywords[]`, `resources[]`, `functions[]`, `achis[]`, `settings[]`, `tags[]`, `checkpoints[]`, `soundtrack{}`, `fixtickets{}`. Нормализация: `visible`/`limit`/`start` из строк `"true"/"false"/число` в `parseMixBoolean`; у элементов `limit > 0` → число, иначе 0 (без лимита).

Метаданные конкретных книг (проверено):

| Игра | game_id | index | len | ends | keywords | resources | functions | checkpoints |
|---|---|---|---|---|---|---|---|---|
| `gamedata.xml` | 18276 | game18276 | 280 | 43 | 1 (vis=false) | 1 (vis=false) | 0 | 5 |
| `gamedata2.xml` | 17497 | mest_torgovca | 405 | 2 | 0 | 3 (2 vis=true) | 1 | 0 |
| `gamedata3.xml` | 10628 | skeptiki1 | 67 | 5 | 10 (все false) | 2 (оба false) | 0 | 0 |
| `gamedata4.xml` | 18280 | sf | 75 | 1 | 8 (все false) | 12 (6 vis=true) | 40 | 5 |
| `gamedata5.xml` | 18397 | game18397 | 58 | 8 | 4 (vis=true) | 1 (vis=true) | 1 | 0 |
| `gamedata7.xml` | 19327 | game19327 | 100 | 6 | 1 (false) | 0 | 0 | 0 |
| `battle.xml` | 11799 | battle | 29 | 5 | 5 (false) | 6 (true) | 0 | 0 |
| `middleage.xml` | 4737 | MiddleAges | 159 | 55 | 0 | 0 | 0 | 0 |

`settings[]` — авторские настройки: `export/pdf`, `defaults/action-style`, `game/scores` (UID ресурса-счёта), `game/metagame`, `import/import-id`, `scheme/*`. `tags[]`, `fixtickets{}`, `achis[]` — метаданные публикации/достижений.

### 7.2 Два вида параметров

| Вид | `type` | Значения | Условие | Изменение |
|---|---|---|---|---|
| Ключевое слово (флаг) | `k` | `true` / отсутствует | «есть/нет» | `inc`, `rnd`, `del`, `nlg`, `set` (set для `k` не работает) |
| Ресурс (счётчик) | `r` | числа ≥ 0 | `>= amnt` или `== amnt` | `inc`, `dec`, `set`, `add`, `sub`, `mlp`, `div`, `eql`, `rnr`, `rng`, `rrr`, `rrg`, `nlg` |

Ограничения:
- `group.limit` — общий лимит суммы по группе; при превышении значение обрезается (для `k` — операция просто «не проходит», для `r` — обрезается до лимита);
- `item.limit > 0` — индивидуальный лимит элемента;
- `getGroupResourceCount` — текущая заполненность группы (для `k` — количество истинных, для `r` — сумма абсолютных значений).

### 7.3 Условия (`checkCondition` / `checkExpression`)

```js
mitril.checkCondition(items, cond, crit)
```

- Если у `cond` есть ключ `or` — режим ИЛИ, выражения берутся из `cond.c` (массив); иначе массив сам является списком и объединяется по И.
- `crit = true` — ждём истину, `crit = false` — инверсия (используется для `ifnot`-ветвей).

Выражение (`checkExpression`):

| `type` | Проверка |
|---|---|
| `k` | значение истинно |
| `r` | `value >= amnt`, а при `eq` — `value == amnt`; `amnt` может быть **UID другого элемента** (тогда берётся его значение), `null` трактуется как 0/undefined |
| `s` | строка-выражение для `exprEval.Parser`; в `items` подставляются переменные с именами `<group.type><uid без дефисов>`; при ошибке парсинга — `false` |
| `f` | отдельного типа условия нет: `type:"f"` встречается только у задачи `class:"fun"` |

Ключи `or`/`c` реально используются: в `battle.xml` — 2 раза, в `gamedata3.xml` — 4, в `gamedata4.xml` — 39. Тип `s` в приложенных играх не встречается ни разу; `eq` — очень часто (`gamedata4.xml` — 835 раз).

### 7.4 Классы задач (`executeTask`)

Скрипт-«программа» mitril — это JSON-массив задач, который исполняет `mitril.script(items, script)`; он же вызывается из `scriptWrap`, а тот дополнительно снимает до/после снимок и рисует изменения в истории листа.

| `class` | Аргументы | Действие |
|---|---|---|
| `inc` | `type k/r`, `val`, `amnt` | прибавить (по умолчанию 1), с учётом лимитов группы и элемента; для `k` — установить `true` |
| `dec` | `val`, `amnt` | вычесть; для `r` — не ниже 0; для `k` — снять флаг |
| `set` | `val`, `amnt` | присвоить (для `k` — **не работает**, возвращает false) |
| `add` | `val`, `param` | прибавить значение **другого элемента**, указанного в `param` |
| `sub` | `val`, `param` | вычесть значение другого элемента (не ниже 0) |
| `mlp` | `val`, `param` | умножить на значение другого элемента |
| `div` | `val`, `param` | поделить с округлением |
| `eql` | `val`, `param` | присвоить значение другого элемента |
| `rnr` | `val`, `param` | прибавить случайное 1..`items[param]` |
| `rng` | `val` (UID группы), `param` | прибавить случайное 1..`items[param]` **случайному элементу группы** |
| `rrr` | `val`, `param` | вычесть случайное 1..`items[param]` |
| `rrg` | `val` (группа), `param` | вычесть случайное у случайного элемента группы |
| `rnd` | `val` (UID группы) | выставить `true` **случайному** элементу группы (для `k`) |
| `del` | `val` (UID группы) | удалить все ключи группы |
| `nlg` | `val` (UID группы) | «обнулить» группу (удалить все ключи; для `k` — не работает) |
| `nul` | — | `executeCleanTask`: удалить всё, чего нет в описании, и проставить `start`-значения (полный сброс) |
| `goto` | `val` (UID параграфа) | немедленный `jsIQ.showArticle(val)` |
| `back` | — | `showArticle(vars.lastarticle)` и обнуление `lastarticle` |
| `achi` | `val` (1..4) | выдать достижение |
| `save` | `val` = `checkpointUID--variantUID` | сохранить чекпойнт (выполняется с задержкой 500 мс) |
| `if` | `type:"64"` или JSON, `val` | условный блок: разобрать `{action_if, action_ifnot, action_script}`, проверить условия, выполнить `action_script` |
| `fun` | `type:"f"`, `val` = `func_uid` | выполнить именованную функцию из `mitrilSetup.functions` (рекурсивно — это тоже массив задач) |

Семантика `class:"if"`: `action_if` и `action_ifnot` — **JSON-строки** (для `type:"64"` они внутри Base64-обёртки). Если `action_if` непусто и условие ложно — задача выходит; если `action_ifnot` непусто и условие истинно (`crit = false`) — тоже выходит; иначе исполняется `action_script` (тоже JSON-строка массива задач). Вложенность не ограничена — в `gamedata4.xml` встречаются `if` внутри `action_script` (проверено тестом).

Реально используемые классы по играм (частоты):

| Игра | Классы |
|---|---|
| `gamedata.xml` | set 52, rnr 26, save 5, inc 1, achi 1 |
| `gamedata2.xml` | eql 34, fun 34, inc 40, dec 13, add 1, achi 1 |
| `gamedata3.xml` | inc 40, dec 13, achi 6, del 2, rnd 6, nlg 1 |
| `gamedata4.xml` | set 302, if 150, rnd 112, del 112, inc 84, dec 66, fun 44, nlg 29, eql 24, add 13, sub 10, save 5 |
| `gamedata5.xml` | dec 32, inc 17, fun 8, achi 5 |
| `battle.xml` | inc 205, del 155, rnd 87, dec 74, sub 32, nlg 15, set 5, achi 1 |
| `gamedata7.xml` | inc 3 |
| `middleage.xml` | achi 3 |

Классы `mlp`, `div`, `rng`, `rrr`, `rrg`, `goto`, `back`, `nul` в этих книгах не встречаются, но полностью реализованы и в браузерном, и в оффлайн-движке.

### 7.5 Изменения и их отображение

`mitril.scriptWrap(vars, [задачи])` — стандартная обёртка для любого игрового изменения:

1. снимок `vars.items` до;
2. `mitril.script(...)`;
3. `getItemChangesList(old, new)` — диф по всем описанным элементам, **только видимых групп**, с человеко-читаемыми именами `Группа/Имя`;
4. запись в `#charsheet-change-history`;
5. `triggerEvent('update_charsheet')` — перерисовка листа;
6. `saveGame(...)` — автосохранение;
7. `renderArticle()` с задержкой 250 мс — перерисовка параграфа (текст и кнопки могли «переключиться»).

Последний пункт принципиален: **изменение параметров внутри параграфа может изменить видимый текст и доступные кнопки без перехода**.

### 7.6 BBCode-разметка текста

`mitril.prepareText(text)` прогоняет текст по 18–20 регуляркам ([game_tpl.xml:1187–1226](<game_tpl.xml#L1187-L1226>)):

| Разметка | Результат |
|---|---|
| `=== Заголовок ===` | `<h4><b>…</b></h4>` |
| `== Подзаголовок ==` | `<h4>…</h4>` |
| `[b]…[/b]`, `[i]`, `[u]` | соответствующие теги |
| `[list]`, `[*]` | `<ul>` / `<li>` |
| `[color=red]…[/color]` | `<span style="color:…">` (из белого списка 16 цветов) |
| `[sup]…[/sup]` | `<sup>` |
| `[url=…]текст[/url]` | ссылка |
| `[player]` | имя игрока (`playerInfo.name`) |
| `--` | длинное тире |
| `\n` | `<div class="text-breaker"><br/></div>` |

Это важно понимать анализатору логов: в сыром XML и в оффлайн-движке текст хранится **с BBCode-разметкой** (`[b]Торговля[/b]`), а игрок в браузере видит HTML. В логах бота BBCode сохраняется как есть (см. примеры `path*.txt`).

---

## 8. Триггерные действия и события

### 8.1 Что такое «триггер» в сторигейме

Три независимых механизма, которые стоит различать:

1. **Триггерные задачи в скрипте** — `goto`, `back`, `save`, `achi`, а также `class:"if"`/`fun`. Они не меняют параметры, а вызывают побочное действие (переход, сохранение, достижение, ветвление). Именно их авторы используют как «события».
2. **Событийная модель jsIQ** — `triggerEvent(name, params, flow)` с очередью обработчиков; любое авторское расширение можно повесить обработчиком в `<handlers>` (например, `before_render_article`).
3. **Параграф триггеров `_mitril_triggers`** — служебный параграф, содержимое которого mitril «приклеивает» к каждому отображаемому параграфу:

```js
var triggers = jsIQ.getArticle('_mitril_triggers');
if (triggers.length === 1 && $('action', triggers).length) {
    jsIQ.addEventHandler('before_render_article', function(params, vars, process) {
        jsIQ.current_article.appendArticle(triggers);   // доп. текст/кнопки/скрипты
        process.next();
    });
}
```

`appendArticle` дописывает в текущий параграф тексты, картинки, музыку, скрипты и **кнопки**, не переходя в другой параграф. В восьми проверенных книгах этот параграф присутствует, но пуст (`<text/>`), поэтому обработчик не регистрируется — механизм зарезервирован авторами фреймворка.

### 8.2 Полный список событий движка

| Событие | Когда | Типовое использование |
|---|---|---|
| `jsiq_init_start` | начало `init()` | ранняя инициализация |
| `jsiq_book_loaded` | книга распарсена | правка дерева книги до регистрации |
| `jsiq_init_done` | книга готова | навешивание глобальных обработчиков (в mitril — галочки на кнопках) |
| `before_article_unload` | перед уходом | `vars.lastarticle`, сброс `linked_cache`, блокировка ссылок |
| `on_article_unload` | уход | `script type="unload"` |
| `on_article_preload` | создание параграфа | `script type="preload"`, автосохранение, `vars.steps++` |
| `before_render_article` | перед рендером | присадка `_mitril_triggers`, модификация `params.article` |
| `on_text_render` | рендер текста | BBCode-конвертация, `linked`-значения |
| `on_render_article` | рендер завершён (текст+кнопки) | `makeLinkActionable` |
| `on_render_images` | после рендера, если есть картинки | вставка изображений |
| `on_article_onload` | всё отрисовано | `script type="onload"` |
| `after_article_loaded` | полное завершение | анимации, пост-эффекты |
| `on_create_article` | объект параграфа создан | аудит/расширения |
| `on_append_article` | `appendArticle` завершён | реакция на присадку |
| `on_action_render` | рендер каждой кнопки | галочки, стили, счётчики нажатий |
| `on_action_click` | клик по кнопке | аудит, аналитика, отмена действия |
| `preload_images` | предзагрузка картинок | прогресс/обработка ошибок |
| `update_charsheet` | перерисовка листа персонажа | сборка `#charsheet-data` |
| `jsiq_error` | любая ошибка скрипта | логирование, отчёт об ошибке |

### 8.3 Финал игры

Определяется через `mitril.checkGameEnd(vars)` из `on_render_article`:

```js
if (jsIQ.current_article && jsIQ.current_article.title != 'mitril' &&
    $('.actionContainer.Visible', jsIQ.containers.actions).length === 0)
    mitrilJs.gameEnd(mitrilSetup, {steps: vars.steps});
```

То есть **концовка — это параграф без видимых кнопок** (после задержки 500 мс). Никакого отдельного маркера «финал» в данных нет; авторский счётчик `mitrilSetup.ends` — просто метаданные. Бот использует тот же критерий: пустой список доступных действий → статус `finished`.

---

## 9. Прочие API и механизмы движка

- **`appendArticle(uid)`** — дописать в текущий параграф содержимое другого (`on_append_article`, повторный рендер). Скрипты при этом не перезатираются: локальные с тем же именем игнорируются.
- **`getArticle(uid)`** — вернуть узел книги; **`unloadArticle(uid)`** — вынуть из книги (используется для выгрузки тяжёлых параграфов).
- **`containers`**: `text`, `actions`, `title`, `charsheet` (+ в оригинале `char_list`). Блок `#charsheet` внутри книги автоматически вырезается и парсится в `containers.charsheet`; статья с `id="charsheet"` обрабатывается так же.
- **`config(name, value)`** — в браузере cookie-настройки (например, `absolutepath` для картинок); в оффлайне `$.cookie` возвращает `undefined`.
- **`preloadImages(list, onload, onerror)`** — предзагрузка изображений с картой результатов.
- **`linkValue/unlinkValue/updateValues/updateValuesByList/showValue/curArticle/run`** — работа со связанными значениями.
- **Уровни логов**: `log_level`, `msg(msg, level)` (1 — critical, 2 — error, 3 — notice, 4 — debug); `triggerError(iscritical, from, message, data, flow)`.

---

## 10. Оффлайн-движок бота: что портировано, а что заглушено

Порт 2.1.2 в [jsiq-engine.js](<jsiq-engine.js>) + мини-DOM/jQuery в [dom.js](<dom.js>) воспроизводят всю игровую логику, но с оговорками:

| Аспект | Состояние |
|---|---|
| Парсер XML | собственный рекурсивный парсер (`parseXml`), поддерживает CDATA, комментарии, PI, DOCTYPE, сущности, самозакрывающиеся теги |
| jQuery | класс `Jq` — селекторы (тег/#id/.class/:empty/:eq, `,`, `>`, потомок), `attr/text/html/append/prepend/find/children/contents/clone/remove/detach/addClass/…`; визуальные методы — no-op |
| Синхронность | все события выполняются синхронно (в браузере — через `setTimeout` 5–100 мс) и на логику не влияют |
| `$.ajax` | заглушка `{result:'error'}` → нет сохранений, нет достижений, нет чекпойнтов, нет саундтрека |
| `exprEval` | заглушка (всегда `false`) — условия `type:"s"` не поддерживаются; в приложенных играх не встречаются |
| `Base64` | **реализован полноценно** (нужен задачам `class:"if"`, `type:"64"`) |
| `on_render_images` | **не триггерится**: картинки в контейнеры оффлайн не вставляются (данные `article.imgs` при этом собираются) |
| `cssdata`, `soundtrack`, `spoiler`, анимации | игнорируются |
| `mitrilJs.*`, `playerInfo`, `confirm`, `alert`, `showNotification`, `location` | заглушки |
| API для бота | добавлены поверх движка: `getAvailableActions()`, `getCurrentTexts()`, `runActionDo(action)`, `navigateTo(target)`, `clickAction(action)`, `getGlobals()` |

Практический совет агенту: при оценке «полноты» воспроизведения игры проверяйте эти четыре пробела (картинки, спойлеры, `cssdata`, `exprEval`/`type:"s"`) — они не влияют на механику, но влияют на текстовый/визуальный опыт.

---

## 11. Как этой моделью пользуется анализатор (`JsiqHosterApp` + `JsiqBot`)

- **Загрузка**: чтение файла → снятие BOM → определение формата → `engine.init(xml)` → разбор `rawSetup` в карту `uid → {name, type, visible, groupUid, groupName}` ([jsiq-hoster-app.js:51–95](<jsiq-hoster-app.js#L51-L95>)) → список id параграфов.
- **Стартовый параграф**: приоритет `mitril` → `start` → `начало` → `intro` → первый id; фактически `mitril` — это bootstrap, а настоящий старт игра задаёт сама вызовом `showArticle("<startUID>")` внутри `getSavegame`. В `gamedata2.xml`, например, детектор даёт `mitril`, а реальный старт — `50d39f37-…` (проверено запуском).
- **Цикл бота** ([jsiq-bot.js:119–197](<jsiq-bot.js#L119-L197>)): снимок состояния при входе → diff (эффекты `preload`/`onload`) → список текстов и доступных кнопок → выбор (`random` или `novelty`) → `runActionDo` (эффекты `do`) → `navigateTo(goto)` → запись шага. Состояние после `do` — база для следующего «входного» diff'а, чтобы эффекты не удваивались.
- **Формат шага истории**: `{index, articleId, texts, stateEntered, enterChanges, availableActions, chosenAction, choiceChanges, stateAfterChoice}`.
- **Фильтр вывода**: только `items.*` + только группы `visible === true`; системные `steps`/`lastarticle` и параметры вне групп отбрасываются; имена — человеко-читаемые `Группа / Имя`; флаги печатаются как `нет → есть` ([jsiq-hoster-app.js:113–306](<jsiq-hoster-app.js#L113-L306>)).
- Если у книги вообще нет `rawSetup`, фильтр не применяется и выводятся все параметры (иначе логировать было бы нечего).

---

## 12. Чеклист для агента, описывающего/анализирующего сторигейм

1. **Формат** — уточните, чистый XML или View Source HTML; снимите BOM; кодировка исходного движка — CP1251 (книги при этом UTF-8).
2. **Наличие mitril** — есть ли `<article id="mitril">` и блок `</script>)({...JSON...})`. Если нет — книга «голая»: только `vars`, условия `if`/`ifnot` и скрипты без групп параметров.
3. **Параметры** — выпишите группы с `visible`; именно они формируют наблюдаемый «лист персонажа» и попадают в отчёт.
4. **Старт и шаги** — помните, что стартовый параграф задаёт не движок, а `getSavegame`-ветка внутри книги; `vars.steps` считает только «игровые» переходы.
5. **Сохранения** — модель `{article, savedata, steps, lastarticle}` + чекпойнты `<cpUID>--<variantUID>`; оффлайн-бот всегда играет «без сохранения».
6. **Скрытые блоки** — `text if/ifnot` (движок) и `text spoiler` (платформа); первое влияет на логику и лог, второе — только на подачу.
7. **Триггеры** — `do`-скрипты кнопок, задачи `goto/back/save/achi`, `class:"if"`/`fun`, обработчики книги, присадка `_mitril_triggers`.
8. **Финал** — отсутствие видимых `action`, а не какой-либо флаг.
9. **Разметка текста** — BBCode; в логах остаётся сырой, игрок видит HTML.
10. **Пробелы воспроизведения** — картинки, спойлеры, `cssdata`, `exprEval`/`type:"s"` (см. §10).
