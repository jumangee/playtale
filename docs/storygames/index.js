// index.js — точка входа: запускает jsiqHosterApp, который грузит игру из файла
// и прогоняет по ней бота, сохраняя пройденный путь в path.txt.
//
// Игру можно прогнать несколько раз (--runs N, по умолчанию 1). Каждый прогон
// начинается с чистого листа и пишется в свой файл: path1.txt, path2.txt, ...
//
// Пример запуска:
//   node index.js
//   node index.js --game gamedata.xml --out path.txt
//   node index.js --start mitril --max-steps 500
//   node index.js --game gamedata2.xml --max-steps 1000 --runs 10

'use strict';

const path = require('path');
const { JsiqHosterApp } = require('./jsiq-hoster-app');

function parseArgs(argv) {
    const opts = {
        gameFile: 'gamedata.xml',
        outputFile: 'path.txt',
        startArticle: null,
        maxSteps: null,
        strategy: null,
        runs: null,
    };
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        const next = argv[i + 1];
        if (a === '--game' && next) { opts.gameFile = next; i++; }
        else if (a === '--out' && next) { opts.outputFile = next; i++; }
        else if (a === '--start' && next) { opts.startArticle = next; i++; }
        else if (a === '--max-steps' && next) { opts.maxSteps = parseInt(next, 10); i++; }
        else if (a === '--strategy' && next) { opts.strategy = next; i++; }
        else if (a === '--runs' && next) { opts.runs = next; i++; }
        else if (a === '--help' || a === '-h') { opts.help = true; }
    }
    return opts;
}

// Разбирает --runs: целое число >= 1. null — некорректное значение.
function parseRuns(value) {
    if (value === null || value === undefined) return 1;
    if (!/^\d+$/.test(String(value).trim())) return null;
    const n = parseInt(value, 10);
    return n >= 1 ? n : null;
}

function describe(bot) {
    const result = (bot && bot.result) || {};
    return {
        steps: bot && bot.history ? bot.history.length : 0,
        status: (result.status || '?') + (result.reason ? ' (' + result.reason + ')' : ''),
    };
}

function main() {
    const args = parseArgs(process.argv.slice(2));
    if (args.help) {
        console.log('Использование: node index.js [опции]');
        console.log('  --game <file>   файл игры (по умолчанию gamedata.xml)');
        console.log('  --out <file>    базовое имя файла истории (по умолчанию path.txt);');
        console.log('                  при --runs > 1 к нему добавляется номер: path1.txt, path2.txt, ...');
        console.log('  --runs <n>      сколько раз прогнать игру (по умолчанию 1);');
        console.log('                  каждый прогон пишется в свой файл');
        console.log('  --start <id>    стартовый параграф (по умолчанию авто)');
        console.log('  --max-steps <n> ограничение шагов (по умолчанию 10000)');
        console.log('  --strategy <s>  стратегия выбора: random (по умолчанию) или novelty');
        return;
    }

    const runs = parseRuns(args.runs);
    if (runs === null) {
        console.error('Ошибка: --runs ожидает целое число >= 1, получено: ' + args.runs);
        process.exitCode = 1;
        return;
    }

    const botOptions = {};
    if (args.maxSteps != null) botOptions.maxSteps = args.maxSteps;
    if (args.strategy) botOptions.strategy = args.strategy;

    // Относительные пути разрешаем относительно каталога скрипта, чтобы запуск
    // работал из любого рабочего каталога.
    const gameFile = path.isAbsolute(args.gameFile) ? args.gameFile : path.resolve(__dirname, args.gameFile);
    const outputFile = path.isAbsolute(args.outputFile) ? args.outputFile : path.resolve(__dirname, args.outputFile);

    const app = new JsiqHosterApp({
        gameFile: gameFile,
        outputFile: outputFile,
        startArticle: args.startArticle || undefined,
        botOptions: botOptions,
        runs: runs,
    });

    // runAll при runs === 1 ведёт себя как run(): один прогон в --out как есть.
    const results = app.runAll(runs);

    console.log('Игра загружена из: ' + gameFile);
    if (runs > 1) console.log('Прогонов: ' + runs);

    let failed = 0;
    for (const result of results) {
        if (result.error) {
            failed++;
            console.error('Прогон ' + result.runIndex + '/' + result.runsTotal + ': ОШИБКА — ' +
                (result.error.message || result.error));
            continue;
        }
        const info = describe(result.bot);
        if (runs === 1) {
            console.log('Стартовый параграф: ' + result.startArticle);
            console.log('Пройдено шагов: ' + info.steps);
            console.log('Статус: ' + info.status);
            console.log('История сохранена в: ' + result.outputFile);
        } else {
            console.log('Прогон ' + result.runIndex + '/' + result.runsTotal +
                ': параграф ' + result.startArticle +
                ', шагов ' + info.steps +
                ', статус ' + info.status +
                ' -> ' + result.outputFile);
        }
    }

    if (runs > 1) {
        console.log('Сохранено файлов прохождений: ' + (results.length - failed) + ' из ' + runs);
    }
    if (failed > 0) process.exitCode = 1;
}

if (require.main === module) {
    main();
}

module.exports = { parseArgs: parseArgs, parseRuns: parseRuns, main: main };
