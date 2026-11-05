// jsiq-engine.js — оффлайн-версия движка jsIQ (порт jsiqapi212.js 2.1.2) для Node.js.
//
// Назначение: полностью повторить логику браузерного jsIQ, чтобы бот "проходил" игру
// точно так же, как её проходит игрок. Разница с браузером только в том, что:
//   * XML-книга загружается из строки, а не через $.ajax;
//   * DOM/jQuery заменены минимальной оффлайн-обёрткой (см. dom.js);
//   * всё выполняется синхронно (в браузере события шли через setTimeout для UI).

const dom = require('./dom');

function createEngine(options) {
    options = options || {};

    const documentNode = dom.parseXml('<html></html>');
    const $ = dom.createJQuery(documentNode);
    const decodeHtml = dom.decodeEntities;

    // -----------------------------------------------------------------------
    // Глобальное окружение браузера (то, что видят скрипты игры)
    // -----------------------------------------------------------------------

    // Таймеры не должны удерживать процесс Node.js после завершения бота.
    const realSetTimeout = global.setTimeout;
    global.setTimeout = function (fn, ms) {
        const t = realSetTimeout(fn, ms);
        if (t && typeof t.unref === 'function') t.unref();
        return t;
    };

    global.window = global; // window === global, чтобы `window.mitril = X` давало доступ к `mitril`

    global.document = {
        createElement: function (tag) { return { nodeType: 1, tagName: String(tag).toLowerCase(), attributes: {}, childNodes: [], _classes: [] }; },
        createTextNode: function (t) { return { nodeType: 3, nodeValue: String(t), childNodes: [] }; },
        body: { nodeType: 1, tagName: 'body', attributes: {}, childNodes: [], _classes: [] },
        getElementById: function () { return null; },
    };

    global.$ = $;
    global.decodeHtml = decodeHtml;

    // --- вспомогательные глобальные функции из jsiqapi212.js ---
    function isValid(v) { return !(v === undefined || v === null); }
    global.isValid = isValid;
    global.makeUnclickable = function () { return false; };
    global.str_apost = function (str, k) { return k ? '"' + str + '"' : "'" + str + "'"; };
    global.jsiq_obj2attr = function (obj) {
        if (typeof obj !== 'object') return '';
        let res = '';
        for (const i in obj) res += ' ' + i + '="' + obj[i] + '"';
        return res.trim();
    };
    global.rnd = function (min, max) { return Math.round(Math.random() * (max - min)) + min; };
    global.customDice = function (dice, count, mod1, mod2, res) {
        if (!res) res = [];
        if (!dice) dice = 6;
        if (count === undefined) count = 1;
        if (!mod1) mod1 = 0;
        if (!mod2) mod2 = 0;
        let total = 0;
        for (let i = 0; i < count; i++) {
            const tt = global.rnd(1, dice) + Number(mod1);
            total += tt;
            res.push(tt);
        }
        total += mod2;
        return total;
    };
    global.getTextNodes = function (e, res) {
        if (!res) res = [];
        (e.contents ? e.contents() : new (function () { this.each = function () {}; })()).each(function () {
            if (this.nodeType === 3 && $.trim(this.nodeValue) !== '') res.push(this);
            else global.getTextNodes($(this), res);
        });
        return res;
    };
    global.Args2Arr = function (args) { return Array.prototype.slice.call(args, 0); };

    function getNodeContentUnHtml(node) {
        const first = dom.firstChildOf(node && node.get ? node.get(0) : (node && node.nodeType ? node : null));
        return decodeHtml($(first).text());
    }
    global.getNodeContentUnHtml = getNodeContentUnHtml;

    // --- сторонние библиотеки, которые использует игра (заглушки) ---
    global.exprEval = {
        Parser: class Parser {
            constructor(opts) { this.opts = opts; }
            parse() { return { evaluate: function () { return false; } }; }
        },
    };
    global.Base64 = {
        decode: function (s) {
            try { return Buffer.from(String(s), 'base64').toString('utf8'); } catch (e) { return ''; }
        },
        encode: function (s) { return Buffer.from(String(s), 'utf8').toString('base64'); },
    };
    global.mitrilJs = {
        prepareTicketData: function () {},
        uploadSavegame: function () {},
        gameEnd: function () {},
        articleFadeAnimStart: function () {},
        articleFadeAnimEnd: function (a, b, process) { if (process && typeof process.next === 'function') process.next(); },
    };
    global.playerInfo = { name: 'Игрок', testuid: undefined };
    global.confirm = function () { return true; };
    global.alert = function () {};
    global.location = { href: '', reload: function () {} };
    global.navigator = {};
    global.scroll = function () {};
    global.showNotification = function () {};

    // -----------------------------------------------------------------------
    // Движок jsIQ (порт)
    // -----------------------------------------------------------------------

    const jsIQ = new (function jsIQ() {
        this.v = '2.1.2';
        this.ready = false;
        this.article_state = null;
        this.containers = { text: null, actions: null, title: null, charsheet: null };
        this.cfg = {};
        this.getCfg = function (name, def) {
            const res = this.cfg[name];
            return res === undefined ? def : res;
        };
        this.current_article = null;
        this.log_level = (options.logLevel !== undefined) ? options.logLevel : 1;
        this.log_to = function (now, msg, level) {
            if (options.log) { options.log(msg, level); return; }
            if (level <= 2) console.error('[jsIQ][' + level + '] ' + now + ' ' + msg);
        };

        this.event_handlers = {};
        this.scripts = {};

        const self = this;

        // --- process management ---
        const processList = [];
        this.addProcess = function (process) { processList.push(process); return true; };
        this.removeProcess = function (process) {
            const p = processList.indexOf(process);
            if (p > -1) processList.splice(p, 1);
            else self.msg('jsIQ.removeProcess: process not found', 2);
        };
        this.getProcessList = function () { return processList; };

        this.msg = function (msg, level) {
            if (!isValid(level)) level = 4;
            const now = (new Date());
            const ts = now.getHours() + ':' + now.getMinutes() + ':' + now.getSeconds() + '.' + now.getMilliseconds();
            if (level <= this.log_level) {
                if (typeof this.log_to === 'function') this.log_to(ts, msg, level);
            }
        };

        // --- article management ---
        let book = $();
        this.addArticle = function (article) {
            const csCntr = this.containers.charsheet;
            function parseCharsheet(nodes) {
                if (!csCntr || !nodes.length) return;
                let html = '';
                nodes.find('text').each(function () {
                    html += getNodeContentUnHtml($(this));
                });
                csCntr.html(html);
            }
            if (book == null || book.length === 0) {
                book = $('book', article);
                const charsheets = book.find('#charsheet').remove();
                parseCharsheet(charsheets);
                if (book.length === 0) {
                    self.triggerError(true, 'jsIQ.addArticle', 'book element not found', { data: article });
                }
                return;
            }
            $('article', article).each(function () {
                const a = this;
                const id = $(this).attr('id');
                if (id === 'charsheet') { parseCharsheet(article); return; }
                if ($('article#' + id, book).length === 0) {
                    book.append(a);
                }
            });
        };

        this.unloadArticle = function (article) {
            if (book == null || book.length === 0) return false;
            const r = book.find('article#' + article).detach();
            return (r.length > 0);
        };

        this.getArticle = function (id) {
            if (!book) {
                self.triggerError(true, 'jsIQ.getArticle', 'book not created', { id: id });
                return $();
            }
            return book.find('article#' + id + ':eq(0)');
        };

        // --- globals (vars) ---
        let globals = {};
        this.reCreateGlobals = function () { globals = {}; };
        this.setGlobals = function (glob) { globals = glob; };
        function getGlobs() { return globals; }
        this.getGlobals = function () { return globals; };

        // --- ScriptEnv ---
        function ScriptEnv(name, params, parent, process) {
            this.name = name;
            this.parent = parent;
            this.vars = getGlobs;
            this.created = new Date();
            this.process = process ? process : (parent ? (parent.process ? parent.process : null) : null);
            this.params = params ? params : (parent ? (parent.params ? parent.params : {}) : null);
            this.from = parent ? (parent.from ? parent.from : undefined) : undefined;
            return this;
        }
        ScriptEnv.prototype.fillParams = function (names, values) {
            this.params = {};
            for (let i = 0; i < names.length; i++) this.params[names[i]] = values[i];
        };
        ScriptEnv.prototype.getTrace = function (res) {
            let doreturn = false;
            if (!res) { res = []; doreturn = true; }
            const params = global.jsiq_obj2attr(this.params);
            res.push(this.name + ': ' + (this.process ? ('@' + this.process + ', ') : '') + 'params: ' + (params !== '' ? params : 'n/a'));
            if (this.parent && this.parent !== this) this.parent.getTrace(res);
            if (doreturn) return res.join('\n');
            return undefined;
        };
        this.createEnv = function (name, params, parent, process) {
            return new ScriptEnv(name, params, parent, process);
        };

        // --- script execution ---
        this.runGlobal = function (script, env) {
            if (typeof script !== 'function') {
                this.triggerError(false, 'jsIQ.runGlobal', 'script not a function', { script: script, env: env });
                return null;
            }
            if (!env) env = this.createEnv('jsIQ.runGlobal');
            try {
                return script.apply(self, [env]);
            } catch (e) {
                this.triggerError(false, 'jsIQ.runGlobal', 'script run error', { message: e.message, script: script, exception: e, env: env });
            }
            return null;
        };

        this.addScript = function (name, script) {
            if (this.scripts[name] && !script) { delete this.scripts[name]; return; }
            if ($.trim(name) === '') return;
            if (typeof script !== 'function') {
                this.triggerError(false, 'jsIQ.addScript', 'not a script passed', {});
                return;
            }
            const selfRef = this;
            this.scripts[name] = (function globalscript() {
                const a = selfRef.current_article;
                if (!a) return;
                return a.runLocal(script).apply(selfRef, arguments);
            });
        };

        // --- event triggering ---
        this.triggerEvent = function (event, params, flow) {
            if (self.log_level > 1) this.msg('jsIQ: event "' + event + '"', 4);
            let flowing = true;
            function continueFlow(p) {
                if (p) p.cancel = true;
                if (flowing) {
                    flowing = false;
                    if (flow) {
                        try { flow.apply(self, [p]); } catch (e) {
                            self.triggerError(true, 'jsIQ.triggerEvent.queue', 'after "' + event + '" flow continue error', { message: e.message, handler: event, exception: e });
                        }
                    }
                }
            }

            if (typeof (this.event_handlers[event]) !== 'object') { continueFlow(); return; }
            const handlers = this.event_handlers[event].slice(0);
            if (handlers.length === 0) { continueFlow(); return; }

            if (!params) params = {};
            else params = $.extend({}, params);
            delete params.cancel;
            if (params.process) {
                params.parent = params.process;
                delete params.process;
                delete params.name;
            }
            params.event = event;
            params.queue = handlers;
            if (!params.name) params.name = event;

            const process = new (function process(queue) {
                const s = this;
                let name = 'noname';
                this.started = new Date();
                if (!self.addProcess(s)) params.cancel = true;
                this.name = function (newname) {
                    if ('' !== $.trim(newname)) name = newname;
                    if (params.parent) return params.parent.name() + '/' + name;
                    return name;
                };
                this.steps = function () { return queue.length; };
                this.run = function () {
                    if (true !== params.cancel && queue.length > 0) {
                        const handler = queue.shift();
                        if (handler) { this.execute(handler); return; }
                    }
                    this.continueFlow();
                };
                this.next = function () { s.run(); };
                this.continueFlow = function () {
                    params.cancel = true;
                    if (!flowing) return;
                    self.removeProcess(s);
                    if (self.log_level > 1) {
                        const timeSpent = (new Date()).valueOf() - this.started.valueOf();
                        self.msg('Process "' + this.name() + '" time spent: ' + timeSpent + 'ms', 4);
                    }
                    continueFlow(params);
                };
                this.execute = function (handler) {
                    try {
                        handler.apply(self, [params, globals, s]);
                    } catch (e) {
                        self.triggerError(false, 'jsIQ.triggerEvent.queue', 'handler "' + event + '" execute error: ' + e.message, { message: e.message, handler: event, process: this.name(), params: params, exception: e });
                    }
                    if (true === params.cancel) { queue = []; this.continueFlow(); return; }
                };
                this.name(params.name);
                return this;
            })(handlers);

            if (process) {
                params.process = process;
                process.run.call(process); // синхронно (в браузере — setTimeout 5мс)
            }
        };

        // --- error triggering ---
        this.triggerError = function (iscritical, from, msg, data, flow) {
            let full = from + ': ' + msg;
            if (data && data.exception && data.exception.stack) full += '\n' + data.exception.stack;
            this.msg(full, iscritical ? 1 : 2);
            const selfRef = this;
            selfRef.triggerEvent('jsiq_error', {
                from: from,
                message: msg,
                iscritical: iscritical,
                data: data,
                flow: flow,
            });
        };

        this.addEventHandler = function (eventName, func) {
            if (!this.event_handlers[eventName]) this.event_handlers[eventName] = [];
            this.event_handlers[eventName].push(func);
        };

        this.removeEventHandler = function (eventName, handler) {
            if (handler === 'all') { this.event_handlers[eventName] = []; return; }
            if (typeof (this.event_handlers[eventName]) !== 'object') return;
            const handlers = this.event_handlers[eventName];
            for (let i = 0; i < handlers.length; i++) {
                if (handlers[i] === handler) { this.event_handlers[eventName].splice(i, 1); return; }
            }
        };

        // --- загрузка книги (оффлайн: из строки XML) ---
        this.loadBook = function (xmlText, callback, onerror) {
            const selfRef = this;
            selfRef.msg('jsIQ.loadBook: (offline xml string)', 3);
            let data;
            try {
                data = dom.parseXml(xmlText);
            } catch (e) {
                if (onerror) { onerror(null, 'parsererror', String(e.message || e)); return; }
                selfRef.triggerError(true, 'jsIQ.loadBook', 'error parsing book', { desc: String(e.message || e) });
                return;
            }
            selfRef.msg('jsIQ.loadBook.success', 3);
            const params = { self: selfRef, data: data };
            selfRef.triggerEvent('jsiq_book_loaded', params, function () {
                selfRef.addArticle(params.data);

                $(params.data).find('handlers handler').each(function () {
                    const t = $(this);
                    const name = t.attr('name');
                    const raw = selfRef.parser.wrapScript('handler_' + name, getNodeContentUnHtml(t));
                    if (!raw) return;
                    selfRef.addEventHandler(name, function (params, vars, process) {
                        const env = selfRef.createEnv('handler_' + name, params, null, process);
                        raw.apply(this, [env]);
                    });
                });

                $(params.data).find('cssdata css').each(function () {
                    const t = $(this);
                    const name = t.attr('name');
                    const raw = $.trim(getNodeContentUnHtml(t));
                    if (!raw || raw === '') return;
                    // оффлайн: стили не применяются
                });

                if (typeof callback === 'function') callback();
            });
        };

        this.init = function (xmlText) {
            const selfRef = this;
            this.containers.text = $();
            this.containers.actions = $();
            this.containers.title = $();
            this.containers.char_list = $();
            this.triggerEvent('jsiq_init_start', { self: this }, function () {
                if (!isValid(xmlText)) {
                    selfRef.triggerError(true, 'jsIQ.init', 'no book xml specified', {});
                    return;
                }
                selfRef.loadBook(xmlText, function () { selfRef.initDone(); });
            });
        };

        this.initDone = function () {
            this.ready = true;
            this.msg('jsIQ инициализирован успешно');
            this.triggerEvent('jsiq_init_done', { self: this }, function () {});
        };

        this.callback = function (f, time) {
            const selfRef = this;
            f.apply(selfRef); // синхронно
        };

        this.config = function () { return undefined; };

        // --- action ---
        this.createAction = function (node) {
            const selfRef = this;
            const action = new (function () {
                const a = this;
                this.text = null;
                this.goto = null;
                this.script = null;
                this.link = null;
                this.style = '';
                this.clicks = 0;
                this.checkif = null;
                this.ifnot = null;
                this.node = $(node);
                this.visible = false;
                this.container = null;

                this.click = function () {
                    a.clicks++;
                    selfRef.triggerEvent('on_action_click', { action: a, dosync: false }, function () {
                        if (a.script) {
                            const env = selfRef.createEnv('ActionClick/' + a.script);
                            env.from = a;
                            if (selfRef.articleScriptQuery(a.script, env) === false) {
                                selfRef.renderArticle();
                                return false;
                            }
                        }
                        if (a.goto) selfRef.showArticle(a.goto);
                        return false;
                    });
                    if (!a.goto) selfRef.callback(function () { selfRef.renderArticle(); });
                    return false;
                };

                this.render = function () {
                    const actionContainer = $('<div></div>').addClass('actionContainer');
                    this.container = actionContainer;
                    a.visible = false;

                    if (this.checkif) {
                        const env = selfRef.createEnv('Action.render', {});
                        env.from = a;
                        if (!selfRef.articleScriptQuery(this.checkif, env)) {
                            actionContainer.addClass('nonVisible').empty();
                            selfRef.triggerEvent('on_action_render', { action: a, container: actionContainer, check: 'if', dosync: true });
                            return actionContainer;
                        }
                    }
                    if (this.ifnot) {
                        const env = selfRef.createEnv('Action.render', {});
                        env.from = a;
                        if (selfRef.articleScript(this.ifnot, null, env)) {
                            actionContainer.addClass('nonVisible').empty();
                            selfRef.triggerEvent('on_action_render', { action: a, container: actionContainer, check: 'ifnot', dosync: true });
                            return actionContainer;
                        }
                    }

                    actionContainer.addClass('Visible');
                    a.visible = true;
                    if (this.style) actionContainer.addClass(this.style);

                    const actionC = $('<a href="#"></a>');
                    if (this.id) actionC.attr('id', this.id);
                    actionC.append(this.text);
                    actionContainer.append(actionC);
                    actionContainer.click(function () { a.click(); return false; });

                    selfRef.triggerEvent('on_action_render', { action: a, container: actionContainer, dosync: true });
                    return actionContainer;
                };

                this.init = function () {
                    this.text = getNodeContentUnHtml(this.node);
                    this.goto = this.node.attr('goto') || this.node.attr('go');
                    this.script = this.node.attr('do');
                    this.style = this.node.attr('class');
                    this.id = this.node.attr('id');
                    this.checkif = this.node.attr('if');
                    this.ifnot = this.node.attr('ifnot');
                };

                this.init();
                return this;
            })();
            if (!node || $(node).length === 0) return action;
            return action;
        };

        this.makeLinkActionable = function (links) {
            const selfRef = this;
            $(links).each(function () {
                const a = $(this);
                if (a.attr('do') || a.attr('go') || a.attr('goto')) {
                    const todo = a.attr('do');
                    const togo = a.attr('go') || a.attr('goto');
                    // оффлайн: реальный DOM-клик не нужен, но сохраняем семантику перехода
                    if (todo) {
                        const env = selfRef.createEnv('ActionClick/clickLink');
                        env.from = { link: a };
                        if (selfRef.articleScriptQuery(todo, env) === false) {
                            selfRef.renderArticle();
                            return false;
                        }
                    }
                    if (togo) { selfRef.showArticle(togo); return false; }
                    selfRef.renderArticle();
                    return false;
                }
            });
        };
    })();

    // --- parser / script wrapping ---
    jsIQ.parser = {
        prepareScript: function (s) {
            const replace = { '&gt;': '>', '&lt;': '<', '&amp;': '&', '&nbsp;': ' ' };
            for (const i in replace) {
                while (s.search(i) > 0) s = s.replace(new RegExp(i, 'g'), replace[i]);
            }
            return s;
        },
        parseScriptQuery: function (query) {
            const params = {};
            const str = query.text;
            const s = str.indexOf('?');
            if (s === -1) { query.script = query.text; query.params = {}; return false; }
            const script = str.substr(0, s);
            let rest = str.substr(s + 1);
            const q = rest.split(';');
            for (let i = 0; i < q.length; i++) {
                const p = q[i].split('=');
                if (p[0] && p[1]) params[p[0]] = p[1];
            }
            query.script = script;
            query.params = params;
            return true;
        },
        wrapScript: function (scriptname, script_text, article) {
            const self = jsIQ;
            if ($.trim(script_text) === '') return null;

            function r(name, params, env) { return jsIQ.articleScript(name, params, env); }

            function getBinds() {
                const binds = ['var triggerError=function(m,p){p.env=env;jsIQ.triggerError(false,selfname,m,p)}'];
                if (article) if (article.localbinds != null) return article.localbinds;
                for (const name in self.scripts) {
                    if (!self.scripts.hasOwnProperty(name) || name === '' || !name) continue;
                    binds.push('var ' + name + '=function(p){return r(\'' + name + '\',p,env);}');
                }
                if (!article) return binds.join(';');
                const localscripts = article.scripts;
                if (localscripts) {
                    for (const name in localscripts) {
                        if (!localscripts.hasOwnProperty(name) || name === '' || !name) continue;
                        if ((['onload', 'preload', 'unload']).indexOf(name) !== -1) continue;
                        binds.push('var ' + name + '=function(p){return r(\'' + name + '\',p,env);}');
                    }
                }
                return (article.localbinds = binds.join(';'));
            }

            let res = null;
            const text = '(function() {' +
                'return function ' + scriptname + '(env, vars){\n' +
                'vars = env.vars();var arg = env.params;var selfname=\'' + scriptname + '\';var local=this;\n' +
                getBinds() + '\nif(env.process){var process=env.process}\n' +
                'try{' + script_text + '\n' +
                '}catch(e){' +
                '   jsIQ.triggerError(false, \'Script\', \'Error executing script "' + scriptname + '"\', {name: \'' + scriptname + '\', article: jsIQ.current_article ? jsIQ.current_article.title : \'n/a\', exception: e, env: env });' +
                '}}' +
                '})()';
            try {
                res = eval(text); // eslint-disable-line no-eval
            } catch (e) {
                jsIQ.triggerError(false, 'jsIQ.wrapScript', 'script parse error (' + e.message + '/' + scriptname + '////' + script_text + '!!!)', {
                    exception: e, name: scriptname, script: script_text,
                });
                return null;
            }
            return res;
        },
    };

    jsIQ.articleScript = function (scriptline, extargs, parentEnv) {
        if (!extargs) return jsIQ.articleScriptQuery(scriptline, parentEnv);
        return jsIQ.articleScriptWithParams(scriptline, extargs, parentEnv);
    };

    jsIQ.articleScriptInEnv = function (env, scriptname) {
        if (!this.current_article || !scriptname || !env) return null;
        let result = null;
        let script = this.current_article.scripts[scriptname];
        if (!script) script = this.scripts[scriptname];
        if (!script) script = this.current_article.getLocalChecker(scriptname);
        try {
            result = this.runGlobal(script, env);
        } catch (e) {
            this.triggerError(false, 'jsIQ.articleScriptInEnv', 'script error', { message: e.message, env: env, script: scriptname });
        }
        return result;
    };

    jsIQ.articleScriptWithParams = function (scriptname, params, parentEnv) {
        if (!this.current_article || !scriptname) return null;
        let result = null;
        const env = this.createEnv('jsIQ.articleScriptWithParams/' + scriptname, params, parentEnv);
        const res = jsIQ.articleScriptInEnv(env, scriptname);
        if (res) { if (result === null) result = res; else result = result && res; }
        return result;
    };

    jsIQ.articleScriptQuery = function (queryStr, parentEnv) {
        if (!this.current_article || !queryStr) return null;
        let result = null;
        const scripts = queryStr.split('|');
        for (let s = 0; s < scripts.length; s++) {
            const query = { text: scripts[s], params: null };
            this.parser.parseScriptQuery(query);
            if (!query.script || !query.params) {
                jsIQ.msg('script: !query.script || !query.params for ' + scripts[s], 2);
                continue;
            }
            const env = this.createEnv('jsIQ.articleScriptQuery/' + query.script, query.params, parentEnv);
            const res = jsIQ.articleScriptInEnv(env, query.script);
            if (res) { if (result === null) result = res; else result = result && res; }
        }
        return result;
    };

    // --- article ---
    jsIQ.createArticle = function (n) {
        const self = this;
        const node = this.getArticle(n);
        const texts = [];
        const scripts = {};
        const actions = [];
        const imgs = [];
        const musics = [];

        $(node).children('text').each(function () { texts.push(self.createText($(this))); });
        $(node).children('img').each(function () {
            const t = $(this);
            imgs.push({ name: t.attr('name'), src: $.trim(t.text()), node: t });
        });
        $(node).children('music').each(function () { musics.push($.trim($(this).text())); });
        $(node).children('script').each(function () {
            const t = $(this);
            const type = t.attr('type');
            if ($.trim(t.attr('isglobal')).toUpperCase() === 'TRUE') { self.scripts[type] = function () {}; return; }
            if (type !== '') scripts[type] = function () {};
        });
        $(node).find('action').each(function () { actions.push(self.createAction(this)); });

        const article = new (function article() {
            this.title = n;
            this.text = texts;
            this.scripts = scripts;
            this.actions = actions;
            this.imgs = imgs;
            this.music = musics;
            this.linked_vars = [];
            this.localbinds = null;
            this.createLocals = function () { return {}; };
            const locals = this.createLocals();

            this.runLocal = function (script) {
                if (typeof script !== 'function') {
                    jsIQ.triggerError(false, 'jsIQ.createArticle.runLocal', 'not a script passed', {});
                    return null;
                }
                return (function localscript(env) {
                    try { return script.apply(locals, [env]); } catch (e) {
                        jsIQ.triggerError(false, 'jsIQ.createArticle.runLocal', 'script error', { message: e.message, exception: e, script: String(script) });
                    }
                });
            };

            this.getLocalChecker = function (name) {
                return this.runLocal(function (env) { return (this[name] === true); });
            };

            this.appendArticle = function (appNode) {
                appNode = $(appNode);
                if (appNode.length === 0) return false;
                const articleRef = this;
                this.localbinds = null;
                appNode.children('text').each(function () { articleRef.text.push(self.createText($(this))); });
                appNode.children('img').each(function () {
                    const t = $(this);
                    articleRef.imgs.push({ name: t.attr('name'), src: $.trim(t.text()), node: t });
                });
                appNode.children('music').each(function () { articleRef.music.push($.trim($(this).text())); });
                appNode.children('script').each(function () {
                    const t = $(this);
                    const type = t.attr('type');
                    if ($.trim(t.attr('isglobal')).toUpperCase() === 'TRUE') { self.scripts[type] = function () {}; return; }
                    if (type !== '' && !articleRef.scripts[type]) articleRef.scripts[type] = function () {};
                });
                appNode.find('action').each(function () { articleRef.actions.push(self.createAction(this)); });
                appNode.children('script').each(function () {
                    const t = $(this);
                    const type = t.attr('type');
                    const raw = self.parser.wrapScript(type, getNodeContentUnHtml(t), articleRef);
                    if (!raw) return;
                    if ($.trim(t.attr('isglobal')).toUpperCase() === 'TRUE') { self.addScript(type, raw); return; }
                    const script = articleRef.runLocal(raw);
                    if (type === '') { self.runGlobal(script()); } else { articleRef.scripts[type] = script; }
                });
                self.triggerEvent('on_append_article', { dosync: true, article: articleRef, node: appNode });
            };
        })();

        $(node).children('script').each(function () {
            const t = $(this);
            const type = t.attr('type');
            const raw = self.parser.wrapScript(type, t.text(), article);
            if (!raw) return;
            if ($.trim(t.attr('isglobal')).toUpperCase() === 'TRUE') { self.addScript(type, raw); return; }
            const script = article.runLocal(raw);
            if (type === '') { self.runGlobal(script()); } else { scripts[type] = script; }
        });

        this.triggerEvent('on_create_article', { dosync: true, article: article, node: node });
        this.msg('jsIQ: Параграф "' + n + '" создан');
        return article;
    };

    jsIQ.createText = function (node) {
        if (!node || $(node).length === 0) return null;
        const self = this;
        const text = new (function () {
            const t = this;
            this.text = null;
            this.link = null;
            this.style = '';
            this.checkif = null;
            this.ifnot = null;
            this.node = $(node);
            this.visible = true;

            this.render = function () {
                t.visible = true;
                if (this.text === '') return '';
                if (this.checkif) {
                    if (!self.articleScriptQuery(this.checkif)) { t.visible = false; return $('<div></div>').addClass('nonVisible'); }
                }
                if (this.ifnot) {
                    if (self.articleScriptQuery(this.ifnot)) { t.visible = false; return $('<div></div>').addClass('nonVisible'); }
                }
                const textContainer = $('<div></div>').addClass('textContainer');
                this.link = textContainer;
                if (this.style) textContainer.addClass(this.style);
                if (this.id) textContainer.attr('id', this.id);
                textContainer.append(this.text);
                self.triggerEvent('on_text_render', { text: t, container: textContainer });
                return textContainer;
            };

            this.init = function () {
                this.text = $.trim(getNodeContentUnHtml(node));
                this.id = this.node.attr('id');
                this.checkif = this.node.attr('if');
                this.ifnot = this.node.attr('ifnot');
            };
            this.init();
            return this;
        })();
        return text;
    };

    jsIQ.articleExists = function (n) { return (this.getArticle(n).length > 0); };

    jsIQ.renderArticle = function (params, callback) {
        if (this.rendering !== true) {
            if (!params) params = { article: this.current_article };
            if (!params.article) return;
            this.rendering = true;
            jsIQ.msg('Start render:' + params.article.title);

            const texts = params.article.text;
            const c = this.containers.text;
            for (let i = 0; i < texts.length; i++) {
                const rendered = texts[i].render();
                if (rendered && typeof rendered !== 'string') c.append(rendered);
            }
            params.text = c;

            const actions = params.article.actions;
            const a = this.containers.actions;
            a.html('');
            for (let i = 0; i < actions.length; i++) {
                const rendered = actions[i].render();
                if (rendered) a.append(rendered);
            }

            this.triggerEvent('on_render_article', params, function () {
                delete this.rendering;
                if (callback) callback.apply(this);
            });
        } else {
            if (callback) callback.apply(this);
        }
    };

    jsIQ.showArticle = function (n, startparams) {
        const self = this;
        const last = this.current_article;
        const started = new Date();
        if (!startparams) startparams = {};
        if (!this.ready) {
            this.triggerError(true, 'jsIQ', 'not initialized', { article: n, last: (last ? last.title : null) });
            return;
        }
        if (startparams.noUnLoadScript !== true) {
            this.triggerEvent('before_article_unload', { current: (last ? last.title : null), next: n, dosync: true }, function (params) {
                if (params.current) {
                    this.triggerEvent('on_article_unload', { current: params.current.title, next: n, process: params.process }, function () {
                        startparams.noUnLoadScript = true;
                        self.showArticle(n, startparams);
                    });
                } else {
                    this.callback(function () {
                        startparams.noUnLoadScript = true;
                        self.showArticle(n, startparams);
                    });
                }
            });
            return;
        }

        this.current_article = null;
        this.containers.text.empty();
        this.containers.actions.empty();
        this.containers.title.html('');

        if (!this.articleExists(n)) {
            this.triggerError(true, 'jsIQ.showArticle', 'article not found', { article: n, nounload: startparams.noUnLoadScript, last: (last ? last.title : null) });
            return;
        }

        const article = this.createArticle(n);
        this.article_state = 'creating';
        jsIQ.current_article = article;

        this.triggerEvent('on_article_preload', { article: article, noPreLoadScript: startparams.noPreLoadScript === true }, function (params) {
            const rparams = { article: article, text: '', actions: $(), title: '', process: params.process };
            this.triggerEvent('before_render_article', rparams, function () {
                try {
                    this.renderArticle(rparams, function () {
                        this.article_state = 'visible';
                        this.triggerEvent('on_article_onload', {
                            article: self.current_article,
                            last: (last ? last.title : null),
                            noOnLoadScript: startparams.noOnLoadScript === true,
                        }, function () {
                            this.triggerEvent('after_article_loaded', { article: self.current_article, last: (last ? last.title : null) }, function () {
                                if (self.log_level > 1) {
                                    const timeSpent = (new Date()).valueOf() - started.valueOf();
                                    this.msg('jsIQ: article complete in ' + timeSpent + 'ms', 2);
                                }
                            });
                        });
                    });
                } catch (e) {
                    this.triggerError(true, 'jsIQ.showArticle', 'render error', { message: e.message, exception: e });
                    return;
                }
            });
        });
    };

    jsIQ.appendArticle = function (n, callback) {
        const self = this;
        const last = this.current_article;
        if (!this.ready || !this.current_article) {
            this.triggerError(true, 'jsIQ', 'not initialized', { article: n, last: (last ? last.title : null) });
            if (typeof callback === 'function') callback(false);
            return;
        }
        const node = this.getArticle(n);
        if (node.length === 0) {
            this.triggerError(true, 'jsIQ.appendArticle', 'article not found', { article: n, nounload: true, callback: callback, last: (last ? last.title : null) });
            if (typeof callback === 'function') callback(false);
            return;
        }
        this.current_article.appendArticle(node);
        if (typeof callback === 'function') callback(true);
        jsIQ.renderArticle();
    };

    // --- стандартные обработчики ---
    jsIQ.addEventHandler('on_text_render', function (params, vars, process) {
        const link_list = {};
        let count = 0;
        params.container.find('.linked').each(function () {
            const link = $(this);
            const name = $.trim(link.attr('name'));
            if (name !== '') { link_list[name] = link; count++; }
        });
        if (count > 0) this.updateValuesByList(link_list, vars);
        process.next();
    });

    jsIQ.addEventHandler('on_render_article', function (params, vars, process) {
        const self = this;
        params.text.find('a').each(function () { self.makeLinkActionable(this); });
        process.next();
    });

    jsIQ.addEventHandler('on_article_preload', function (params, vars, process) {
        if (params.noPreLoadScript !== true) this.articleScript('preload');
        process.next();
    });

    jsIQ.addEventHandler('on_article_onload', function (params, vars, process) {
        global.scroll(0, 0);
        this.articleScript('onload');
        process.next();
    });

    jsIQ.addEventHandler('before_article_unload', function (params, vars, process) {
        process.next();
    });

    jsIQ.addEventHandler('on_article_unload', function (params, vars, process) {
        this.articleScript('unload');
        process.next();
    });

    jsIQ.addEventHandler('preload_images', function (params, vars, process) {
        process.next();
    });

    jsIQ.addEventHandler('before_article_unload', function (params, vars, process) {
        this.linked_cache = {};
        process.next();
    });

    jsIQ.addEventHandler('update_charsheet', function (params, vars, process) {
        if (this.containers.charsheet) if (this.containers.charsheet.length > 0) this.triggerEvent('on_text_render', { container: this.containers.charsheet });
        process.next();
    });

    // --- linked values ---
    jsIQ.linked_vars = {};
    jsIQ.linkValue = function (id, value) {
        if (undefined === value) value = id;
        this.linked_vars[id] = value;
    };
    jsIQ.unlinkValue = function (id) { delete this.linked_vars[id]; };
    jsIQ.updateValues = function (article, vars) {
        function setValue(val, e) { e.html(val); }
        const links = article.linked_vars;
        for (const link in links) {
            const c = links[link];
            let val = null;
            if (typeof this.linked_vars[link] === 'function') {
                val = this.runGlobal(this.linked_vars[link]);
            } else {
                if (vars && vars[link]) val = vars[link];
                else {
                    val = this.current_article.runLocal(function () { return this[link]; });
                    if (!val) val = this.runGlobal(function (v) { return v[link]; });
                }
            }
            if (val) setValue(val, c);
        }
    };
    jsIQ.updateValuesByList = function (linked_list, vars) {
        function setValue(val, e) { e.html(val); }
        for (const link in linked_list) {
            if (!linked_list.hasOwnProperty(link)) continue;
            const c = linked_list[link];
            let val = null;
            if (typeof this.linked_cache[link] === 'function') { setValue(this.runGlobal(this.linked_cache[link]), c); continue; }
            else if (typeof this.linked_vars[link] === 'function') { setValue(this.runGlobal(this.linked_vars[link]), c); continue; }
            else {
                if (vars) {
                    const t = vars[link];
                    if (!(undefined === t || null === t)) { setValue(t, c); continue; }
                } else {
                    const t = function (v) { return v[link]; };
                    val = this.runGlobal(t);
                    if (!(undefined === val || null === val)) { this.linked_cache[link] = t; setValue(val, c); continue; }
                }
                if (undefined === val || null === val) {
                    if (!this.current_article) { this.msg('jsIQ.updateValuesByList, error on "' + link + '" value: this.current_article is NULL', 2); return; }
                    const t = this.current_article.runLocal(function () { return this[link]; });
                    val = t();
                    if (!(undefined === val || null === val)) { this.linked_cache[link] = t; setValue(val, c); }
                }
            }
        }
    };

    function curArticle() { return jsIQ.current_article; }
    global.curArticle = curArticle;
    global.run = function (scriptname, params, env) {
        if (!scriptname) return;
        return jsIQ.articleScript(scriptname, params, env);
    };
    global.showValue = function (name, val) {
        this.linked_cache[name] = function () { return val; };
        jsIQ.updateValuesByList([name]);
    };

    jsIQ.preloadImages = function (list, onload, onerror) {
        if (!onerror) onerror = onload;
        this.triggerEvent('preload_images', { images: list }, function (params) {
            if (params.success) { if (onload) onload.apply(this, [params]); return; }
            onerror.apply(this);
        });
    };

    // --- общедоступный API для бота (добавлен поверх движка) ---
    jsIQ.getAvailableActions = function () {
        const a = this.current_article;
        if (!a) return [];
        return a.actions.filter(function (act) { return act.visible; });
    };
    jsIQ.getCurrentTexts = function () {
        const a = this.current_article;
        if (!a) return [];
        return a.text.filter(function (t) { return t.visible !== false; }).map(function (t) { return t.text; });
    };
    jsIQ.runActionDo = function (action) {
        if (!action) return true;
        if (action.script) {
            const env = this.createEnv('ActionClick/' + action.script);
            env.from = action;
            if (this.articleScriptQuery(action.script, env) === false) {
                this.renderArticle();
                return false;
            }
        }
        return true;
    };

    jsIQ.navigateTo = function (target) {
        if (target) this.showArticle(target);
        else this.renderArticle();
        return this.current_article;
    };

    jsIQ.clickAction = function (action) {
        if (!action) return this.current_article;
        if (!this.runActionDo(action)) return this.current_article;
        return this.navigateTo(action.goto);
    };

    global.jsIQ = jsIQ;
    return jsIQ;
}

module.exports = { createEngine: createEngine };
