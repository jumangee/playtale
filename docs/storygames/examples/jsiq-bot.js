// jsiq-bot.js — бот, который "проходит" игру через jsiq-engine.
//
// Делает выборы из доступных вариантов (по умолчанию — случайно), запоминает
// пройденный путь и изменения игровых параметров. Стратегия выбора, формат
// истории и способ снятия снимков состояния вынесены в методы — их можно
// переопределить в наследнике или через опции.

'use strict';

function isPlainObject(v) {
    return v != null && typeof v === 'object' && !Array.isArray(v);
}

function deepClone(v) {
    if (Array.isArray(v)) return v.map(deepClone);
    if (isPlainObject(v)) {
        const out = {};
        for (const k of Object.keys(v)) out[k] = deepClone(v[k]);
        return out;
    }
    if (v instanceof Date) return new Date(v.getTime());
    return v;
}

// Плоский diff двух состояний: список { path, before, after } для изменившихся
// листовых значений. Пути — строки через точку, например "items.<uid>".
function diffObjects(before, after) {
    const out = [];
    function walk(b, a, prefix) {
        const keys = new Set();
        if (isPlainObject(b)) for (const k of Object.keys(b)) keys.add(k);
        if (isPlainObject(a)) for (const k of Object.keys(a)) keys.add(k);
        for (const k of keys) {
            const bv = isPlainObject(b) ? b[k] : undefined;
            const av = isPlainObject(a) ? a[k] : undefined;
            const path = prefix ? prefix + '.' + k : k;
            if (isPlainObject(bv) && isPlainObject(av)) {
                walk(bv, av, path);
            } else if (!Object.is(bv, av)) {
                out.push({ path: path, before: bv, after: av });
            }
        }
    }
    walk(before, after);
    return out;
}

function serializeAction(a) {
    return {
        text: a.text,
        goto: a.goto || null,
        script: a.script || null,
        id: a.id || null,
    };
}

class JsiqBot {
    /**
     * @param {object} engine  — оффлайн-движок (jsiq-engine)
     * @param {object} [options]
     * @param {number} [options.maxSteps=10000]  — защита от зацикливания
     * @param {function} [options.random]        — источник случайности (для тестов)
     */
    constructor(engine, options) {
        this.engine = engine;
        this.options = options || {};
        this.maxSteps = this.options.maxSteps !== undefined ? this.options.maxSteps : 10000;
        this.random = this.options.random || Math.random;
        this.history = [];
        this.result = { status: 'idle', reason: null, stepsCount: 0 };
        this.visitCounts = new Map();
    }

    /**
     * Стратегия выбора варианта. По умолчанию — случайный выбор из доступных.
     * Переопределите метод в наследнике для более сложной логики.
     *
     * Опция options.strategy === 'novelty' включает альтернативную стратегию:
     * предпочитать параграфы, которые ещё не посещались (полезно для больших
     * игр, где случайный выбор долго не сходится).
     *
     * @param {Array} availableActions доступные варианты
     * @param {object} context { article, engine, stepIndex, availableActions }
     */
    chooseAction(availableActions, context) {
        if (!availableActions || availableActions.length === 0) return null;
        if (this.options.strategy === 'novelty') return this.chooseByNovelty(availableActions, context);
        const r = Math.min(Math.floor(this.random() * availableActions.length), availableActions.length - 1);
        return availableActions[r];
    }

    chooseByNovelty(availableActions, context) {
        const current = context && context.article ? context.article.title : null;
        let best = Infinity;
        const candidates = [];
        for (const action of availableActions) {
            const target = action.goto || current;
            const score = this.visitCounts.get(target) || 0;
            if (score < best) { best = score; candidates.length = 0; candidates.push(action); }
            else if (score === best) candidates.push(action);
        }
        return candidates[Math.min(Math.floor(this.random() * candidates.length), candidates.length - 1)];
    }

    snapshotState() {
        return deepClone(this.engine.getGlobals());
    }

    diffState(before, after) {
        return diffObjects(before, after);
    }

    /** Хук, вызываемый после каждого шага. */
    onStep(step) {}

    /** Хук, вызываемый по завершении прохождения. */
    onFinish(result) {}

    play(startArticle) {
        this.history = [];
        this.visitCounts = new Map();
        let prevState = this.snapshotState();

        this.engine.showArticle(startArticle);

        let guard = 0;
        for (;;) {
            guard++;
            const article = this.engine.current_article;
            const stateEntered = this.snapshotState();
            const enterChanges = this.diffState(prevState, stateEntered);
            const texts = this.engine.getCurrentTexts();
            const availableActions = this.engine.getAvailableActions();

            if (article && article.title) {
                this.visitCounts.set(article.title, (this.visitCounts.get(article.title) || 0) + 1);
            }

            if (availableActions.length === 0) {
                this.history.push({
                    index: guard,
                    articleId: article ? article.title : null,
                    texts: texts,
                    stateEntered: stateEntered,
                    enterChanges: enterChanges,
                    availableActions: [],
                    chosenAction: null,
                    choiceChanges: [],
                    stateAfterChoice: stateEntered,
                });
                this.result = { status: 'finished', reason: 'no-available-actions', stepsCount: guard };
                this.onFinish(this.result);
                return this;
            }

            const chosen = this.chooseAction(availableActions, {
                article: article,
                engine: this.engine,
                stepIndex: guard,
                availableActions: availableActions,
            });

            // 1) эффекты скрипта выбора (`do`) — «изменения при выборе варианта»
            const stateBeforeDo = this.snapshotState();
            this.engine.runActionDo(chosen);
            const stateAfterDo = this.snapshotState();
            const choiceChanges = this.diffState(stateBeforeDo, stateAfterDo);

            // 2) переход (goto + preload/onload следующего параграфа)
            this.engine.navigateTo(chosen ? chosen.goto : null);
            const stateAfterNav = this.snapshotState();

            const step = {
                index: guard,
                articleId: article ? article.title : null,
                texts: texts,
                stateEntered: stateEntered,
                enterChanges: enterChanges,
                availableActions: availableActions.map(serializeAction),
                chosenAction: chosen ? serializeAction(chosen) : null,
                choiceChanges: choiceChanges,
                stateAfterChoice: stateAfterNav,
            };
            this.history.push(step);
            this.onStep(step);

            // база для «изменений при входе» следующего параграфа — состояние
            // сразу после скрипта выбора, до перехода
            prevState = stateAfterDo;

            if (guard >= this.maxSteps) {
                this.result = { status: 'aborted', reason: 'max-steps-reached', stepsCount: guard };
                this.onFinish(this.result);
                return this;
            }
        }
    }
}

module.exports = {
    JsiqBot: JsiqBot,
    deepClone: deepClone,
    diffObjects: diffObjects,
};
