// dom.js — минимальный XML DOM + jQuery-подобная обёртка для оффлайн-движка jsIQ.
// Это замена браузерному DOM/jQuery: только то подмножество, которое использует
// jsiqapi212.js и типовые скрипты игр (включая фреймворк mitril).

'use strict';

// ---------------------------------------------------------------------------
// Декодирование сущностей
// ---------------------------------------------------------------------------

function decodeEntities(s) {
    if (s == null) return '';
    return String(s).replace(/&(#x?[0-9a-fA-F]+|[a-zA-Z]+);/g, function (whole, body) {
        if (body[0] === '#') {
            const hex = body[1] === 'x' || body[1] === 'X';
            const code = parseInt(body.slice(hex ? 2 : 1), hex ? 16 : 10);
            try { return String.fromCodePoint(code); } catch (e) { return whole; }
        }
        switch (body) {
            case 'amp': return '&';
            case 'lt': return '<';
            case 'gt': return '>';
            case 'quot': return '"';
            case 'apos': return "'";
            default: return whole;
        }
    });
}

// ---------------------------------------------------------------------------
// XML-парсер (рекурсивный спуск). Даёт узлы: nodeType 1 (элемент), 3 (текст/CDATA), 9 (документ).
// ---------------------------------------------------------------------------

function makeText(value, parent) {
    return { nodeType: 3, nodeName: '#text', nodeValue: value, parentNode: parent, childNodes: [] };
}

function makeElement(tagName, parent) {
    return {
        nodeType: 1,
        nodeName: tagName,
        tagName: tagName.toLowerCase ? tagName.toLowerCase() : tagName,
        attributes: {},
        childNodes: [],
        parentNode: parent,
        ownerDocument: parent ? parent.ownerDocument : null,
        _classes: null,
    };
}

function skipWs(i) {
    const s = i.s;
    while (i.p < s.length && /\s/.test(s[i.p])) i.p++;
}

function parseXml(xmlText) {
    const doc = { nodeType: 9, nodeName: '#document', childNodes: [], parentNode: null, ownerDocument: null };
    doc.ownerDocument = doc;
    const i = { s: String(xmlText), p: 0 };

    function parseNode(parent) {
        const s = i.s;
        if (s[i.p] === '<') {
            if (s.startsWith('<!--', i.p)) { skipComment(); return null; }
            if (s.startsWith('<![CDATA[', i.p)) { return parseCdata(parent); }
            if (s.startsWith('<?', i.p)) { skipPI(); return null; }
            if (s.startsWith('<!', i.p)) { skipDeclaration(); return null; }
            if (s.startsWith('</', i.p)) { throw new Error('Unexpected closing tag at ' + i.p); }
            return parseElement(parent);
        }
        return parseText(parent);
    }

    function parseElement(parent) {
        const s = i.s;
        i.p++; // '<'
        const nameMatch = /^[a-zA-Z_][\w:.-]*/.exec(s.slice(i.p));
        if (!nameMatch) throw new Error('Bad tag at ' + i.p);
        const tagName = nameMatch[0];
        i.p += tagName.length;
        const elm = makeElement(tagName, parent);

        for (;;) {
            skipWs(i);
            const c = s[i.p];
            if (c === '>') { i.p++; break; }
            if (c === '/') {
                if (s[i.p + 1] === '>') { i.p += 2; return elm; }
                throw new Error('Bad self-closing tag at ' + i.p);
            }
            if (c === undefined || c === '<') break;
            const attrMatch = /^[a-zA-Z_][\w:.-]*/.exec(s.slice(i.p));
            if (!attrMatch) throw new Error('Bad attribute at ' + i.p);
            const attrName = attrMatch[0];
            i.p += attrName.length;
            skipWs(i);
            let val = '';
            if (s[i.p] === '=') {
                i.p++;
                skipWs(i);
                const quote = s[i.p];
                if (quote === '"' || quote === "'") {
                    i.p++;
                    const end = s.indexOf(quote, i.p);
                    if (end === -1) throw new Error('Unterminated attribute at ' + i.p);
                    val = s.slice(i.p, end);
                    i.p = end + 1;
                } else {
                    const m = /^[^\s>]*/.exec(s.slice(i.p));
                    val = m ? m[0] : '';
                    i.p += val.length;
                }
            }
            elm.attributes[attrName] = decodeEntities(val);
        }

        for (;;) {
            if (i.p >= s.length) break;
            if (s.startsWith('</', i.p)) {
                const gt = s.indexOf('>', i.p);
                if (gt === -1) throw new Error('Unterminated closing tag at ' + i.p);
                i.p = gt + 1;
                break;
            }
            const child = parseNode(elm);
            if (child) elm.childNodes.push(child);
        }
        return elm;
    }

    function parseText(parent) {
        const s = i.s;
        const lt = s.indexOf('<', i.p);
        if (lt === -1) {
            const t = s.slice(i.p);
            i.p = s.length;
            if (t === '') return null;
            return makeText(decodeEntities(t), parent);
        }
        if (lt === i.p) {
            // '<' directly — parse as node
            return parseNode(parent);
        }
        const t = s.slice(i.p, lt);
        i.p = lt;
        if (t === '') return null;
        return makeText(decodeEntities(t), parent);
    }

    function parseCdata(parent) {
        const s = i.s;
        i.p += 9; // '<![CDATA['
        const end = s.indexOf(']]>', i.p);
        if (end === -1) throw new Error('Unterminated CDATA at ' + i.p);
        const t = s.slice(i.p, end);
        i.p = end + 3;
        return makeText(t, parent);
    }

    function skipComment() {
        const s = i.s;
        const end = s.indexOf('-->', i.p);
        i.p = end === -1 ? s.length : end + 3;
    }

    function skipPI() {
        const s = i.s;
        const end = s.indexOf('?>', i.p);
        i.p = end === -1 ? s.length : end + 2;
    }

    function skipDeclaration() {
        // <!DOCTYPE ...> or other <!...>
        const s = i.s;
        if (s.startsWith('<!DOCTYPE', i.p)) {
            let depth = 0;
            while (i.p < s.length) {
                const c = s[i.p];
                if (c === '[') depth++;
                else if (c === ']') depth--;
                else if (c === '>' && depth <= 0) { i.p++; return; }
                i.p++;
            }
            return;
        }
        const end = s.indexOf('>', i.p);
        i.p = end === -1 ? s.length : end + 1;
    }

    // разбор корня
    const s = i.s;
    for (;;) {
        skipWs(i);
        if (i.p >= s.length) break;
        if (s.startsWith('<?', i.p)) { skipPI(); continue; }
        if (s.startsWith('<!--', i.p)) { skipComment(); continue; }
        if (s.startsWith('<!', i.p)) { skipDeclaration(); continue; }
        const node = parseNode(doc);
        if (node) doc.childNodes.push(node);
    }
    return doc;
}

// ---------------------------------------------------------------------------
// Хелперы для узлов
// ---------------------------------------------------------------------------

function firstChildOf(node) {
    return node && node.childNodes && node.childNodes.length ? node.childNodes[0] : null;
}

function nodeText(node) {
    if (!node) return '';
    if (node.nodeType === 3) return node.nodeValue;
    if (node.nodeType === 1) {
        let out = '';
        for (const c of node.childNodes || []) out += nodeText(c);
        return out;
    }
    return '';
}

function getClassList(node) {
    if (!node) return [];
    if (node._classes) return node._classes;
    let cls = [];
    if (node.attributes && node.attributes['class']) {
        cls = String(node.attributes['class']).split(/\s+/).filter(Boolean);
    }
    node._classes = cls;
    return cls;
}

function hasClass(node, name) {
    return getClassList(node).indexOf(name) !== -1;
}

function setClassList(node, list) {
    node._classes = list;
    if (node.attributes) node.attributes['class'] = list.join(' ');
}

function addClass(node, name) {
    const list = getClassList(node);
    if (list.indexOf(name) === -1) list.push(name);
    setClassList(node, list);
}

function removeClass(node, name) {
    const list = getClassList(node).filter(function (c) { return c !== name; });
    setClassList(node, list);
}

// ---------------------------------------------------------------------------
// Селекторы
// ---------------------------------------------------------------------------

function tokenizeSelector(selector) {
    const tokens = [];
    let i = 0;
    const n = selector.length;
    while (i < n) {
        while (i < n && /\s/.test(selector[i])) i++;
        if (i >= n) break;
        let combinator = tokens.length ? ' ' : null;
        if (selector[i] === '>') {
            combinator = '>';
            i++;
            while (i < n && /\s/.test(selector[i])) i++;
        }
        const start = i;
        while (i < n && !/\s/.test(selector[i]) && selector[i] !== '>') i++;
        if (i > start) tokens.push({ combinator: combinator, simple: selector.slice(start, i) });
    }
    return tokens;
}

function matchesSimple(elm, simple) {
    if (!elm || elm.nodeType !== 1) return false;
    let s = simple;
    let isEmpty = false;
    if (s.indexOf(':empty') !== -1) { isEmpty = true; s = s.replace(/:empty/g, ''); }
    s = s.replace(/:eq\(\d+\)/g, '');

    const idMatch = /#([\w-]+)/.exec(s);
    const id = idMatch ? idMatch[1] : null;
    const classes = [];
    const classRe = /\.([\w-]+)/g;
    let m;
    while ((m = classRe.exec(s))) classes.push(m[1]);
    const tag = s.replace(/#[\w-]+/g, '').replace(/\.[\w-]+/g, '').replace(/[^a-zA-Z]/g, '');

    if (tag && elm.tagName !== tag.toLowerCase()) return false;
    if (id && elm.attributes && elm.attributes['id'] !== id) return false;
    for (const c of classes) {
        if (!hasClass(elm, c)) return false;
    }
    if (isEmpty) {
        if (elm.childNodes && elm.childNodes.length > 0) return false;
        if (nodeText(elm).trim() !== '') return false;
    }
    return true;
}

function matchFrom(elm, tokens, idx) {
    const tok = tokens[idx];
    if (!matchesSimple(elm, tok.simple)) return false;
    if (idx === 0) return true;
    const comb = tok.combinator;
    let parent = elm.parentNode;
    if (comb === '>') {
        return parent ? matchFrom(parent, tokens, idx - 1) : false;
    }
    while (parent) {
        if (matchFrom(parent, tokens, idx - 1)) return true;
        parent = parent.parentNode;
    }
    return false;
}

function matches(elm, selector) {
    if (typeof selector !== 'string') return false;
    selector = selector.trim();
    if (selector === '') return false;
    if (selector.indexOf(',') !== -1) {
        const parts = selector.split(',');
        for (const p of parts) {
            if (matches(elm, p.trim())) return true;
        }
        return false;
    }
    const tokens = tokenizeSelector(selector);
    if (!tokens.length) return false;
    return matchFrom(elm, tokens, tokens.length - 1);
}

function collectDescendants(elm, selector, out) {
    for (const c of elm.childNodes || []) {
        if (matches(c, selector)) out.push(c);
        if (c.nodeType === 1) collectDescendants(c, selector, out);
    }
}

// ---------------------------------------------------------------------------
// Виртуальный DOM-элемент (для контейнеров, создаваемых через $('<div>…'))
// ---------------------------------------------------------------------------

function makeVirtual(tagName, ownerDocument) {
    const elm = makeElement(tagName || 'div', null);
    elm._virtual = true;
    elm.ownerDocument = ownerDocument;
    return elm;
}

// ---------------------------------------------------------------------------
// jQuery-подобная обёртка
// ---------------------------------------------------------------------------

class Jq {
    constructor(nodes) {
        this.nodes = nodes || [];
        this.length = this.nodes.length;
        for (let i = 0; i < this.nodes.length; i++) this[i] = this.nodes[i];
    }

    get(i) { return this.nodes[i]; }
    first() { return new Jq(this.nodes.length ? [this.nodes[0]] : []); }
    eq(i) { return new Jq(this.nodes[i] !== undefined ? [this.nodes[i]] : []); }

    each(fn) {
        for (let i = 0; i < this.nodes.length; i++) {
            if (fn.call(this.nodes[i], i, this.nodes[i]) === false) break;
        }
        return this;
    }

    find(selector) {
        const out = [];
        for (const n of this.nodes) {
            if (n.nodeType === 1 || n.nodeType === 9 || n.nodeType === 11) collectDescendants(n, selector, out);
        }
        return new Jq(out);
    }

    children(selector) {
        const out = [];
        for (const n of this.nodes) {
            for (const c of n.childNodes || []) {
                if (c.nodeType === 1 && (!selector || matches(c, selector))) out.push(c);
            }
        }
        return out ? new Jq(out) : new Jq([]);
    }

    contents() {
        const out = [];
        for (const n of this.nodes) {
            for (const c of n.childNodes || []) out.push(c);
        }
        return new Jq(out);
    }

    parent() {
        const out = [];
        for (const n of this.nodes) if (n.parentNode) out.push(n.parentNode);
        return new Jq(out);
    }

    attr(name, value) {
        if (value === undefined) {
            if (this.nodes.length === 0) return undefined;
            const n = this.nodes[0];
            return (n.attributes && n.attributes[name] !== undefined) ? n.attributes[name] : undefined;
        }
        for (const n of this.nodes) {
            if (n.attributes) n.attributes[name] = value;
        }
        return this;
    }

    removeAttr(name) {
        for (const n of this.nodes) {
            if (n.attributes) delete n.attributes[name];
        }
        return this;
    }

    text(value) {
        if (value === undefined) {
            let s = '';
            for (const n of this.nodes) s += nodeText(n);
            return s;
        }
        for (const n of this.nodes) {
            n.childNodes = [];
            if (value !== '' && value != null) n.childNodes.push(makeText(String(value), n));
        }
        return this;
    }

    html(value) {
        if (value === undefined) {
            let s = '';
            for (const n of this.nodes) s += nodeText(n);
            return s;
        }
        for (const n of this.nodes) {
            n.childNodes = [];
            if (value !== '' && value != null) n.childNodes.push(makeText(String(value), n));
        }
        return this;
    }

    empty() {
        for (const n of this.nodes) n.childNodes = [];
        return this;
    }

    append(item) {
        for (const n of this.nodes) {
            const nodes = toNodes(item, n);
            for (const c of nodes) {
                c.parentNode = n;
                n.childNodes.push(c);
            }
        }
        return this;
    }

    appendTo(target) {
        const t = target instanceof Jq ? target : new Jq([target]);
        t.append(this);
        return this;
    }

    prepend(item) {
        for (const n of this.nodes) {
            const nodes = toNodes(item, n);
            for (let k = nodes.length - 1; k >= 0; k--) {
                nodes[k].parentNode = n;
                n.childNodes.unshift(nodes[k]);
            }
        }
        return this;
    }

    remove() {
        for (const n of this.nodes) {
            if (n.parentNode) {
                const idx = (n.parentNode.childNodes || []).indexOf(n);
                if (idx !== -1) n.parentNode.childNodes.splice(idx, 1);
            }
        }
        return this;
    }

    detach() { return this.remove(); }

    clone() {
        // клон возвращаем как новый Jq с поверхностной копией первого узла
        const out = [];
        for (const n of this.nodes) out.push(shallowClone(n));
        return new Jq(out);
    }

    addClass(name) {
        for (const n of this.nodes) {
            const parts = name.split(/\s+/).filter(Boolean);
            for (const p of parts) addClass(n, p);
        }
        return this;
    }

    removeClass(name) {
        for (const n of this.nodes) {
            const parts = name.split(/\s+/).filter(Boolean);
            for (const p of parts) removeClass(n, p);
        }
        return this;
    }

    hasClass(name) {
        return this.nodes.some(function (n) { return hasClass(n, name); });
    }

    is(selector) {
        return this.nodes.some(function (n) { return matches(n, selector); });
    }

    not(selector) {
        const out = [];
        for (const n of this.nodes) if (!matches(n, selector)) out.push(n);
        return new Jq(out);
    }

    val(value) {
        if (value === undefined) {
            if (this.nodes.length === 0) return undefined;
            return this.nodes[0].attributes ? this.nodes[0].attributes['value'] : undefined;
        }
        for (const n of this.nodes) {
            if (n.attributes) n.attributes['value'] = value;
        }
        return this;
    }

    prop(name, value) {
        if (value === undefined) {
            if (this.nodes.length === 0) return undefined;
            return this.nodes[0][name];
        }
        for (const n of this.nodes) n[name] = value;
        return this;
    }

    // нижеследующие методы — визуальные/анимационные; в оффлайн-режиме это no-op
    click() { return this; }
    unbind() { return this; }
    show() { return this; }
    hide() { return this; }
    fadeIn() { return this; }
    fadeOut() { return this; }
    slideDown() { return this; }
    slideToggle() { return this; }
    stop() { return this; }
    animate() { return this; }
    width() { return this; }
    scroll() { return this; }
    data() { return undefined; }
    scrollTop() { return 0; }
}

function shallowClone(node) {
    if (node.nodeType === 3) return makeText(node.nodeValue, null);
    const copy = makeElement(node.tagName, null);
    copy.attributes = Object.assign({}, node.attributes);
    for (const c of node.childNodes || []) {
        const cc = shallowClone(c);
        cc.parentNode = copy;
        copy.childNodes.push(cc);
    }
    copy._virtual = node._virtual;
    return copy;
}

function toNodes(item, parent) {
    if (item instanceof Jq) return item.nodes.slice();
    if (Array.isArray(item)) return item.slice();
    if (item && typeof item === 'object' && item.nodeType) return [item];
    if (typeof item === 'string') return [makeText(item, parent)];
    if (item == null) return [];
    return [makeText(String(item), parent)];
}

// ---------------------------------------------------------------------------
// Фабрика $ (jQuery-подобная)
// ---------------------------------------------------------------------------

function createJQuery(documentNode) {
    function $(selector, context) {
        if (selector instanceof Jq) return selector;
        if (selector && typeof selector === 'object' && selector.nodeType !== undefined) {
            return new Jq([selector]);
        }
        if (Array.isArray(selector)) {
            return new Jq(selector);
        }
        if (typeof selector === 'function') {
            return new Jq([]);
        }
        if (typeof selector === 'string') {
            const s = selector.trim();
            if (s.charAt(0) === '<') {
                // создание виртуального элемента из html-строки
                const v = parseHtmlFragment(s, documentNode);
                return new Jq(v);
            }
            const rootNodes = [];
            if (context instanceof Jq) {
                for (const n of context.nodes) rootNodes.push(n);
            } else if (context && typeof context === 'object' && context.nodeType !== undefined) {
                rootNodes.push(context);
            } else {
                rootNodes.push(documentNode);
            }
            const out = [];
            for (const root of rootNodes) {
                if (matches(root, s)) out.push(root);
                if (root.nodeType === 1 || root.nodeType === 9) collectDescendants(root, s, out);
            }
            return new Jq(out);
        }
        return new Jq([]);
    }

    $.trim = function (s) { return s == null ? '' : String(s).replace(/^\s+|\s+$/g, ''); };
    $.extend = function () {
        let target = arguments[0] || {};
        for (let a = 1; a < arguments.length; a++) {
            const src = arguments[a];
            if (!src) continue;
            for (const k in src) {
                if (Object.prototype.hasOwnProperty.call(src, k)) target[k] = src[k];
            }
        }
        return target;
    };
    $.each = function (obj, fn) {
        if (obj == null) return obj;
        if (Array.isArray(obj)) {
            for (let i = 0; i < obj.length; i++) {
                if (fn.call(obj[i], i, obj[i]) === false) break;
            }
        } else if (typeof obj === 'object') {
            for (const k in obj) {
                if (!Object.prototype.hasOwnProperty.call(obj, k)) continue;
                if (fn.call(obj[k], k, obj[k]) === false) break;
            }
        }
        return obj;
    };
    $.parseJSON = function (s) {
        try { return JSON.parse(s); } catch (e) { return null; }
    };
    $.cookie = function () { return undefined; };
    $.ajax = function (opts) {
        // Оффлайн: имитируем ответ сервера "нет данных/ошибка".
        const response = { result: 'error' };
        if (opts && typeof opts.success === 'function') {
            try { opts.success(response); } catch (e) {}
        } else if (opts && typeof opts.error === 'function') {
            try { opts.error(response, 'offline', 'no network'); } catch (e) {}
        }
        return { done: function () { return this; }, fail: function () { return this; } };
    };

    return $;
}

// Парсинг простой html-строки вида '<div class="x">текст</div>' в виртуальные узлы.
function parseHtmlFragment(html, ownerDocument) {
    const s = html.trim();
    const out = [];
    let i = 0;
    while (i < s.length) {
        if (s[i] !== '<') {
            const lt = s.indexOf('<', i);
            const txt = (lt === -1 ? s.slice(i) : s.slice(i, lt));
            if (txt) out.push(makeText(decodeEntities(txt), null));
            if (lt === -1) break;
            i = lt;
            continue;
        }
        // элемент
        const gt = s.indexOf('>', i);
        if (gt === -1) { out.push(makeText(decodeEntities(s.slice(i)), null)); break; }
        const openTag = s.slice(i + 1, gt);
        const selfClosing = /\/\s*$/.test(openTag);
        const cleaned = openTag.replace(/\/\s*$/, '').trim();
        const nameMatch = /^[a-zA-Z][\w-]*/.exec(cleaned);
        if (!nameMatch) { i = gt + 1; continue; }
        const tagName = nameMatch[0];
        const elm = makeVirtual(tagName, ownerDocument);
        // атрибуты
        const attrRe = /([a-zA-Z][\w-]*)\s*=\s*("([^"]*)"|'([^']*)'|([^\s]+))/g;
        let m;
        while ((m = attrRe.exec(cleaned))) {
            elm.attributes[m[1]] = decodeEntities(m[3] !== undefined ? m[3] : (m[4] !== undefined ? m[4] : m[5]));
        }
        // class-атрибут в список классов
        if (elm.attributes['class']) {
            elm._classes = String(elm.attributes['class']).split(/\s+/).filter(Boolean);
        }
        if (selfClosing) {
            out.push(elm);
            i = gt + 1;
            continue;
        }
        // ищем соответствующий закрывающий тег
        const closeTag = '</' + tagName + '>';
        const closeIdx = s.indexOf(closeTag, gt + 1);
        if (closeIdx !== -1) {
            const inner = s.slice(gt + 1, closeIdx);
            const children = parseHtmlFragment(inner, ownerDocument);
            for (const c of children) { c.parentNode = elm; elm.childNodes.push(c); }
            i = closeIdx + closeTag.length;
        } else {
            i = gt + 1;
        }
        out.push(elm);
    }
    return out;
}

module.exports = {
    parseXml: parseXml,
    createJQuery: createJQuery,
    decodeEntities: decodeEntities,
    nodeText: nodeText,
    firstChildOf: firstChildOf,
    matches: matches,
};
