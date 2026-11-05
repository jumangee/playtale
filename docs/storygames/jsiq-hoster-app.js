// jsiq-hoster-app.js — оркестратор: загружает игру из файла, запускает движок и бота,
// сохраняет пройденный путь в файл.
//
// Класс JsiqHosterApp можно наследовать и переопределять:
//   * readGameFile / extractXml        — как читать/чистить файл игры;
//   * createEngine / detectStartArticle— как создавать движок и точку старта;
//   * formatHistory                    — как сериализовать пройденный путь.

'use strict';

const fs = require('fs');
const path = require('path');

const { createEngine } = require('./jsiq-engine');
const { JsiqBot } = require('./jsiq-bot');
const { decodeEntities } = require('./dom');

function looksLikeViewSource(text) {
    const t = text.replace(/^\uFEFF/, '').replace(/^\s+/, '');
    return /^<!DOCTYPE\b/i.test(t) || /^<html\b/i.test(t);
}

function looksLikeRawXml(text) {
    const t = text.replace(/^\uFEFF/, '').replace(/^\s+/, '');
    return t.startsWith('<?xml') || t.startsWith('<book') || /^<[a-zA-Z][\w:.-]*(\s|>)/.test(t);
}

// Извлекает исходный XML из страницы "View Source" браузера.
function extractXmlFromViewSource(text) {
    const bodyMatch = /<body[^>]*>([\s\S]*)<\/body>/i.exec(text);
    const body = bodyMatch ? bodyMatch[1] : text;
    const cleaned = body
        .replace(/<script[^>]*>[\s\S]*?<\/script>/gi, '')
        .replace(/<style[^>]*>[\s\S]*?<\/style>/gi, '')
        .replace(/<[^>]*>/g, '');
    return decodeEntities(cleaned);
}

// Разбирает значение visible/limit из rawSetup: строка "true"/"false" или число.
function parseMixBoolean(value) {
    if (value === 'true') return true;
    if (value === 'false') return false;
    if (value === undefined || value === null) return false;
    const n = parseInt(value, 10);
    return isNaN(n) ? false : n;
}

// Извлекает JSON-настройку mitril из книги и строит карту uid -> описание
// игрового параметра: { name, type, visible, groupUid, groupName }.
// type: 'k' — ключевое слово (флаг), 'r' — ресурс (счётчик).
function resolveItemMeta(xmlText) {
    const map = {};
    try {
        const marker = '})({';
        const idx = xmlText.indexOf(marker);
        if (idx === -1) return map;
        const open = idx + 3; // позиция '{' после '})('
        let depth = 0, inStr = false, esc = false;
        let end = -1;
        for (let i = open; i < xmlText.length; i++) {
            const c = xmlText[i];
            if (inStr) {
                if (esc) esc = false;
                else if (c === '\\') esc = true;
                else if (c === '"') inStr = false;
                continue;
            }
            if (c === '"') { inStr = true; continue; }
            if (c === '{') depth++;
            else if (c === '}') { depth--; if (depth === 0) { end = i; break; } }
        }
        if (end === -1) return map;
        const setup = JSON.parse(xmlText.slice(open, end + 1));

        const collect = function (groups, type) {
            for (const g of groups) {
                const groupName = g.name || '';
                for (const item of (g.items || [])) {
                    map[item.uid] = {
                        name: (groupName ? groupName + ' / ' : '') + item.name,
                        type: type,
                        visible: parseMixBoolean(g.visible),
                        groupUid: g.uid,
                        groupName: groupName,
                    };
                }
            }
        };
        collect(setup.keywords || [], 'k');
        collect(setup.resources || [], 'r');
    } catch (e) {
        // не критично: карта метаданных — только для читаемости вывода
    }
    return map;
}

// Карта uid -> человеко-понятное имя (совместимость с прежним API).
function resolveItemNames(xmlText) {
    const meta = resolveItemMeta(xmlText);
    const map = {};
    for (const uid of Object.keys(meta)) map[uid] = meta[uid].name;
    return map;
}

function listArticleIds(xmlText) {
    const ids = [];
    const re = /<article\s+id="([^"]+)"/g;
    let m;
    while ((m = re.exec(xmlText))) ids.push(m[1]);
    return ids;
}

function formatValue(v) {
    if (v === undefined) return '(нет)';
    if (v === null) return 'null';
    if (typeof v === 'object') return JSON.stringify(v);
    return String(v);
}

// Количество прогонов: целое >= 1, по умолчанию 1.
function normalizeRuns(value) {
    if (value === undefined || value === null || value === '') return 1;
    const n = parseInt(value, 10);
    if (!isFinite(n) || n < 1) return 1;
    return n;
}

// Имя файла истории для прогона с номером: path.txt -> path1.txt, path2.txt, ...
// Расширение сохраняется, поэтому "прохождение.1.txt" из --out path.txt не выйдет.
function numberedFileName(filePath, index) {
    const dir = path.dirname(filePath);
    const ext = path.extname(filePath);
    const base = path.basename(filePath, ext);
    return path.join(dir, base + index + ext);
}

// Значение игрового параметра: для счётчиков (type r) отсутствие = 0,
// для флагов (type k) отсутствие = «нет», установлен = «есть».
function formatItemValue(v, type) {
    if (v === true) return 'есть';
    if (v === false || v === null) return 'нет';
    if (v === undefined) return type === 'k' ? 'нет' : '0';
    if (typeof v === 'object') return JSON.stringify(v);
    return String(v);
}

class JsiqHosterApp {
    /**
     * @param {object} [options]
     * @param {string} [options.gameFile='gamedata.xml']   — путь к файлу игры
     * @param {string} [options.outputFile='path.txt']     — куда сохранять историю
     *                                                        (базовое имя при нескольких прогонах)
     * @param {number} [options.runs=1]                    — сколько раз прогонять игру (см. runAll)
     * @param {string} [options.startArticle]              — стартовый параграф (авто по умолчанию)
     * @param {function} [options.BotClass]                — класс бота (по умолчанию JsiqBot)
     * @param {object} [options.botOptions]                — опции бота
     * @param {function} [options.formatter]               — (bot, context) => string
     */
    constructor(options) {
        this.options = options || {};
        this.gameFile = this.options.gameFile || 'gamedata.xml';
        this.outputFile = this.options.outputFile || 'path.txt';
    }

    readGameFile(filePath) {
        let raw = fs.readFileSync(filePath, 'utf8');
        raw = raw.replace(/^\uFEFF/, '');
        return raw;
    }

    extractXml(text) {
        if (looksLikeRawXml(text)) return text;
        if (looksLikeViewSource(text)) return extractXmlFromViewSource(text);
        // попытка интерпретировать как XML в любом случае
        return text;
    }

    createEngine() {
        return createEngine(this.options.engineOptions || {});
    }

    detectStartArticle(ids) {
        const prefer = ['mitril', 'start', 'начало', 'intro'];
        for (const p of prefer) if (ids.indexOf(p) !== -1) return p;
        return ids[0];
    }

    loadGame() {
        const text = this.readGameFile(this.gameFile);
        const xml = this.extractXml(text);
        this.xmlText = xml;
        this.itemMeta = resolveItemMeta(xml);
        this.itemNames = {};
        for (const uid of Object.keys(this.itemMeta)) this.itemNames[uid] = this.itemMeta[uid].name;
        this.articleIds = listArticleIds(xml);
        this.engine = this.createEngine();
        this.engine.init(xml);
        return this;
    }

    runBot() {
        const BotClass = this.options.BotClass || JsiqBot;
        this.bot = new BotClass(this.engine, this.options.botOptions || {});
        const start = this.options.startArticle || this.detectStartArticle(this.articleIds);
        this.startArticle = start;
        this.bot.play(start);
        return this;
    }

    formatHistory(bot) {
        if (this.options.formatter) return this.options.formatter(bot, this);
        return this.defaultFormatter(bot);
    }

    defaultFormatter(bot) {
        const lines = [];
        const guidMatch = /<book\s+guid="([^"]*)"/.exec(this.xmlText || '');
        lines.push('=== Пройденная история ===');
        //lines.push('Игра: ' + (guidMatch ? guidMatch[1] : '?'));
        lines.push('Шагов: ' + bot.history.length);
        lines.push('');

        for (const step of bot.history) {
            lines.push('### ' + step.index + '');
            //lines.push('Параграф: ' + step.articleId);

            const enterItems = this.itemChanges(step.enterChanges);
            if (enterItems.length) {
                lines.push('Изменения при входе в параграф:');
                for (const c of enterItems) lines.push('  ' + this.describeChange(c));
            } else {
                //lines.push('Изменения при входе: нет');
            }

            //lines.push('Текст:');
            for (const t of step.texts) {
                //for (const ln of String(t).split(/\r?\n/)) lines.push('  ' + ln);
				lines.push(t)
            }

            if (step.availableActions.length) {
                //lines.push('Доступные варианты:');
                step.availableActions.forEach((a, i) => {
                    lines.push('[' + (i + 1) + '] ' + a.text);
                });
                const chosen = step.chosenAction;
				if (step.availableActions.length > 1) {
					lines.push('Выбран: ' + (chosen ? chosen.text : '?'));
				}
                const choiceItems = this.itemChanges(step.choiceChanges);
                if (choiceItems.length) {
                    lines.push('Изменения при выборе:');
                    for (const c of choiceItems) lines.push('  ' + this.describeChange(c));
                } else {
                    //lines.push('Изменения при выборе: нет');
                }
            } else {
                lines.push('Доступные варианты: нет — игра завершена.');
            }
            lines.push('');
        }

        if (bot.result && bot.result.status === 'aborted') {
            lines.push('=== Конец (прохождение прервано: ' +
                (bot.result.reason || 'лимит шагов') + ', шагов ' + bot.history.length + ') ===');
        } else {
            lines.push('=== Конец ===');
        }
        return lines.join('\n');
    }

    // Оставляем только изменения игровых параметров (vars.items), да и то лишь
    // тех, чья группа в rawSetup помечена visible. Невидимые группы игрок не
    // видит — значит, и в прохождении их быть не должно.
    // Системные поля (lastarticle, steps) сюда не попадают по построению.
    itemChanges(changes) {
        const meta = this.itemMeta || {};
        const hasMeta = Object.keys(meta).length > 0;
        const out = [];
        for (const c of (changes || [])) {
            if (typeof c.path !== 'string' || c.path.indexOf('items.') !== 0) continue;
            const uid = c.path.slice(6);
            const info = meta[uid];
            if (info) {
                // у параметра есть группа — показываем только если группа видима
                if (info.visible !== true) continue;
            } else if (hasMeta) {
                // игра с rawSetup: параметры вне групп (служебные маркеры и т.п.)
                continue;
            }
            out.push({ uid: uid, info: info, before: c.before, after: c.after });
        }
        return out;
    }

    itemName(uid) {
        const info = this.itemMeta && this.itemMeta[uid];
        if (info) return info.name;
        return (this.itemNames && this.itemNames[uid]) ? this.itemNames[uid] : uid;
    }

    describeChange(change) {
        const name = change.info ? change.info.name : this.itemName(change.uid);
        const type = change.info ? change.info.type : undefined;
        return name + ': ' + formatItemValue(change.before, type) + ' -> ' + formatItemValue(change.after, type);
    }

    saveHistory(filePath) {
        const out = filePath || this.outputFile;
        const text = this.formatHistory(this.bot);
        fs.writeFileSync(out, text, 'utf8');
        return out;
    }

    /** Полный запуск: загрузить игру, прогнать бота, сохранить историю. */
    run() {
        this.loadGame();
        this.runBot();
        const outFile = this.saveHistory(this.outputFile);
        return {
            engine: this.engine,
            bot: this.bot,
            outputFile: outFile,
            startArticle: this.startArticle,
        };
    }

    /**
     * Имя файла прохождения для прогона index из total.
     * Один прогон — исходное имя (path.txt), несколько — нумерованные
     * path1.txt, path2.txt, ... (номер вставляется перед расширением).
     */
    outputFileForRun(index, total) {
        const count = normalizeRuns(total);
        if (count <= 1) return this.outputFile;
        return numberedFileName(this.outputFile, index);
    }

    /**
     * Прогоняет игру несколько раз. Каждый прогон начинается с чистого листа:
     * создаётся новый экземпляр приложения (а значит, новый движок и новый бот),
     * и результат пишется в свой файл — path1.txt, path2.txt, ...
     *
     * Ошибка в одном прогоне не прерывает остальные: она попадает в результат
     * этого прогона полем error.
     *
     * @param {number} [runs=1] количество прогонов
     * @returns {Array<object>} результаты прогонов (runIndex, runsTotal, bot, ...)
     */
    runAll(runs) {
        const total = normalizeRuns(runs !== undefined ? runs : this.options.runs);
        const AppClass = this.constructor || JsiqHosterApp;
        const results = [];
        for (let i = 1; i <= total; i++) {
            const outputFile = this.outputFileForRun(i, total);
            try {
                const runApp = new AppClass(Object.assign({}, this.options, {
                    gameFile: this.gameFile,
                    outputFile: outputFile,
                }));
                const result = runApp.run();
                result.runIndex = i;
                result.runsTotal = total;
                results.push(result);
            } catch (e) {
                results.push({
                    runIndex: i,
                    runsTotal: total,
                    outputFile: outputFile,
                    startArticle: this.options.startArticle,
                    error: e,
                });
            }
        }
        return results;
    }
}

module.exports = {
    JsiqHosterApp: JsiqHosterApp,
    extractXmlFromViewSource: extractXmlFromViewSource,
    resolveItemMeta: resolveItemMeta,
    resolveItemNames: resolveItemNames,
    listArticleIds: listArticleIds,
    numberedFileName: numberedFileName,
    normalizeRuns: normalizeRuns,
};
