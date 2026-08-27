/*
 * llm_page_harness.js - behavioral harness for the device's /llm chat page.
 *
 * WHAT THIS IS
 *   The LLM chat page is ~28KB of JavaScript living inside a C++ raw string
 *   literal in components/hardwareone/WebPage_LLM.h. The firmware build never
 *   parses it -- to the compiler it is an opaque array of bytes -- and there is
 *   no browser in the loop, so the only way this logic was ever validated was
 *   by flashing a board and clicking around. That makes the page's most
 *   fragile behaviors (poll cadence, announcement de-duplication, catalog
 *   diffing) exactly the ones nobody re-checks after an edit.
 *
 *   So: extract the REAL shipping JS, run it against a stub DOM and a
 *   scriptable fake device, and drive it through the scenario that actually
 *   bites -- a CM5 co-processor whose model catalog arrives asynchronously and
 *   whose "load" is a host-side llama-server restart taking tens of seconds.
 *   Nothing here reimplements the page; if it did, it would only prove the
 *   reimplementation works. (Same rule the C host suites state at
 *   updater/test/host/CMakeLists.txt:9-10.)
 *
 * WHAT THIS IS NOT
 *   Not a browser. The stubs below are the smallest thing the page can run
 *   against, and every divergence from a real DOM is a place a bug can hide.
 *   The known ones are named in comments at the point where they occur. In
 *   particular the promise stub resolves SYNCHRONOUSLY, so this harness asserts
 *   an ordering a browser never produces (see P() below). It catches logic
 *   regressions, not rendering, layout, or event-loop bugs.
 *
 * OUTPUT CONTRACT (the Python driver parses this; do not change it casually)
 *   PASS <name>
 *   FAIL <name> <detail>
 *   HARNESS_RESULT PASS      |      HARNESS_RESULT FAIL <n>
 *
 * Requires the globals from _bootstrap.js (__out / slurp / __argv), which
 * tools/webui/js_engine.py prepends before running this file.
 *   __argv[0] = path to the extracted page JS
 *   __argv[1] = path to a JSON array of element ids declared in the page HTML
 */

var PAGE_JS_PATH = __argv[0];
var HTML_IDS_PATH = __argv[1];

var passes = 0, failures = 0;
function check(name, cond, detail) {
    if (cond) { passes++; __out("PASS " + name); }
    else { failures++; __out("FAIL " + name + (detail ? "   [" + detail + "]" : "")); }
}

/* ------------------------------------------------------------------------
 * Absorbed-exception recorder.
 *
 * This is the single most important line in the file. The thenable below
 * swallows anything the page throws and routes it to .catch(), which is what a
 * real promise does too -- and the page's own poll loop catches errors and
 * retries (WebPage_LLM.h, pollResult). The net effect is that a page which
 * crashes outright can still satisfy every assertion, because the assertions
 * only look at the DOM afterwards. That is not hypothetical: an earlier draft
 * of this harness reported "ALL CHECKS PASSED (27)" while the page was
 * throwing TypeError on every single generation.
 *
 * So every exception the thenable absorbs is recorded, and a non-empty list
 * fails the run. Green now means "the page ran", not "nothing reached the top
 * level".
 * ---------------------------------------------------------------------- */
var absorbed = [];

/* ------------------------------------------------------------------------
 * Synchronous thenable.
 *
 * KNOWN DIVERGENCE, deliberately accepted: real promises resolve on a
 * microtask, so a browser interleaves the page's callbacks with pending
 * timers. Here .then() runs before hw.fetchJSON() has even returned. That
 * makes the scenario deterministic and the virtual clock meaningful, at the
 * cost of asserting an ordering the browser never produces -- notably the
 * page schedules its next beat BEFORE the status response lands, so real
 * cadence lags one poll behind what this measures. Modelling latency properly
 * means re-tuning every scenario boundary, which is a separate change.
 * ---------------------------------------------------------------------- */
function P(val, isErr) {
    return {
        then: function (f) {
            if (isErr) return this;
            try { return P(f ? f(val) : val); }
            catch (e) { absorbed.push(String(e && e.stack ? e.stack : e)); return P(e, true); }
        },
        catch: function (f) {
            if (isErr && f) {
                try { f(val); } catch (e) { absorbed.push(String(e)); }
                return P(undefined);
            }
            return this;
        }
    };
}

/* ---- virtual clock -------------------------------------------------------
 * Timers fire in (time, insertion) order. Ties matter: the page can schedule
 * several callbacks for the same instant, and a sort that is not stable would
 * make this suite flaky for reasons that have nothing to do with the page.
 * ---------------------------------------------------------------------- */
var now = 0, timers = [], tid = 1, seq = 0;
function setTimeoutStub(fn, ms) {
    var t = { id: tid++, at: now + (ms || 0), ord: seq++, fn: fn };
    timers.push(t);
    return t.id;
}
function clearTimeoutStub(id) { timers = timers.filter(function (t) { return t.id !== id; }); }
function order(a, b) { return a.at - b.at || a.ord - b.ord; }
function advance(ms) {
    var end = now + ms;
    for (;;) {
        timers.sort(order);
        if (!timers.length || timers[0].at > end) break;
        var t = timers.shift();
        now = t.at;
        try { t.fn(); } catch (e) { absorbed.push(String(e)); }
    }
    now = end;
}
function nextDelay() {
    if (!timers.length) return null;
    timers.sort(order);
    return timers[0].at - now;
}

/* ---- DOM stub ------------------------------------------------------------
 * A real enough tree that the page's node manipulation works: parentNode,
 * sibling order, insertBefore and remove all operate on one children array.
 * Without this the page throws inside finishGen and the throw is invisible.
 * ---------------------------------------------------------------------- */
var sysLog = [], modelOpts = [], rebuilds = 0, initWrites = 0;

// The streamed answer, as the page renders it. Keyed on className, NOT on id:
// document.createElement returns El("_new"), so every dynamically created node
// shares one id and an id-based recorder would record nothing and pass
// vacuously — the exact failure this exists to catch.
var answerText = "";

// Error lines the page renders. Keyed on className, NOT id, for the same reason
// answerText is: document.createElement returns El("_new"), so every dynamic
// node shares one id and an id-keyed recorder would record nothing and pass
// vacuously.
var errLines = [];

function ClassList() { this._s = {}; }
ClassList.prototype = {
    add: function (c) { this._s[c] = true; },
    remove: function (c) { delete this._s[c]; },
    contains: function (c) { return !!this._s[c]; },
    toggle: function (c) { this._s[c] = !this._s[c]; return this._s[c]; }
};

function El(id) {
    this.id = id;
    this._text = "";
    this._html = "";
    this.value = "";
    this.className = "";
    this.disabled = false;
    this.style = {};
    this.children = [];
    this.parentNode = null;
    this.scrollTop = 0;
    this.scrollHeight = 0;
    this.classList = new ClassList();   // per-element: a shared one lets state leak between nodes
    this._listeners = {};               // per-element for the same reason
    this._rect = null;                  // set explicitly by the scenario when geometry matters
}
Object.defineProperty(El.prototype, "textContent", {
    get: function () { return this._text; },
    set: function (v) {
        if (this.id === "qa-init-msg") initWrites++;
        if ((this.className || "").indexOf("qa-a-text") >= 0) answerText = v;
        if ((this.className || "").indexOf("qa-err") >= 0) errLines.push(v);
        this._text = v;
    }
});
Object.defineProperty(El.prototype, "parentElement", {
    get: function () { return this.parentNode; }
});
Object.defineProperty(El.prototype, "nextSibling", {
    get: function () {
        if (!this.parentNode) return null;
        var sibs = this.parentNode.children, i = sibs.indexOf(this);
        return (i >= 0 && i + 1 < sibs.length) ? sibs[i + 1] : null;
    }
});
Object.defineProperty(El.prototype, "innerHTML", {
    get: function () { return this._html; },
    set: function (v) {
        // Clearing innerHTML detaches every child -- for EVERY element, not just
        // the model picker. Special-casing one element means the others silently
        // accumulate, and an assertion like "no duplicate options" can never fail.
        this._html = v;
        this.children.forEach(function (c) { c.parentNode = null; });
        this.children = [];
        // Destroying the options drops the selection. A real <select> auto-picks
        // the first remaining option rather than clearing, so "" is a
        // conservative stand-in — but WITHOUT it `value` survives the rebuild by
        // construction and "selection survived a rebuild" cannot fail, which
        // makes the keep/restore logic it guards deletable in silence.
        this.value = "";
        if (this.id === "qa-model") { rebuilds++; modelOpts = []; }
    }
});
El.prototype.appendChild = function (c) {
    if (c.parentNode) c.parentNode.removeChild(c);
    c.parentNode = this;
    this.children.push(c);
    this._record(c);
    return c;
};
El.prototype.insertBefore = function (c, ref) {
    if (c.parentNode) c.parentNode.removeChild(c);
    c.parentNode = this;
    var i = ref ? this.children.indexOf(ref) : -1;
    if (i < 0) this.children.push(c); else this.children.splice(i, 0, c);
    this._record(c);
    return c;
};
El.prototype.removeChild = function (c) {
    var i = this.children.indexOf(c);
    if (i >= 0) this.children.splice(i, 1);
    c.parentNode = null;
    return c;
};
El.prototype.remove = function () { if (this.parentNode) this.parentNode.removeChild(this); };
El.prototype._record = function (c) {
    if (this.id === "qa-list" && c.className === "qa-sys") sysLog.push(c.textContent);
    if (this.id === "qa-model") modelOpts.push({ v: c.value, t: c.textContent, d: !!c.disabled });
};
El.prototype.focus = function () {};
// A real listener registry. This was a no-op, which meant NOTHING the page
// registers with addEventListener was observable -- the Enter-to-send handler on
// the input has never been exercised, and no pointer-driven behaviour could be.
//
// `passive` is recorded rather than ignored: a listener registered passively
// cannot cancel the browser's default, so a page that registers passively and
// then relies on preventDefault has a real bug that this can model.
El.prototype.addEventListener = function (type, fn, opts) {
    if (typeof fn !== "function") return;
    var o = (opts && typeof opts === "object") ? opts : {};
    if (!this._listeners[type]) this._listeners[type] = [];
    this._listeners[type].push({ fn: fn, passive: !!o.passive, once: !!o.once });
};
El.prototype.removeEventListener = function (type, fn) {
    var l = this._listeners[type];
    if (!l) return;
    this._listeners[type] = l.filter(function (r) { return r.fn !== fn; });
};
// Geometry, for gestures that measure movement. Null until a scenario sets one,
// so a test that depends on coordinates has to say so out loud.
El.prototype.getBoundingClientRect = function () {
    return this._rect || { top: 0, left: 0, bottom: 0, right: 0, width: 0, height: 0 };
};
// Pointer capture. Load-bearing for mouse and pen, which get NO implicit capture
// -- without it the browser stops delivering move/up to the button the moment
// the cursor leaves it, which is exactly when an upward drag arms.
El.prototype.setPointerCapture = function (id) { this._captured = id; };
El.prototype.releasePointerCapture = function () { this._captured = null; };
El.prototype.hasPointerCapture = function (id) { return this._captured === id; };
El.prototype.querySelector = function (sel) {
    var want = sel.replace(/^\./, "");
    for (var i = 0; i < this.children.length; i++) {
        if ((this.children[i].className || "").split(/\s+/).indexOf(want) >= 0) return this.children[i];
    }
    return null;
};

/* getElementById returns null for an id the page never declared -- exactly as a
 * browser does. Auto-vivifying a working element for any id asked for would hide
 * the most realistic edit there is: renaming an id in the HTML chunk
 * (WebPage_LLM.h R"HTML() and forgetting the JS chunk (R"JS(), or the reverse.
 * That ships a page which TypeErrors on load and a harness that stays green. */
var declaredIds = JSON.parse(slurp(HTML_IDS_PATH));
var requestedIds = {};
// Deliver an event the way a browser would: refuse unhittable targets, run the
// element's own listeners, then bubble to ancestors, document and window.
//
// The hittability check matters more than it looks. A disabled button receives
// no pointer events at all, and the Ask button is disabled by the status poll on
// LOADING / UNLOADED / ERROR / offline -- so a gesture can lose its terminating
// event mid-press in ordinary use.
function hittable(el) {
    for (var e = el; e; e = e.parentNode) {
        if (e.disabled === true) return false;
        if (e.style && e.style.display === "none") return false;
    }
    return true;
}

function dispatch(target, type, init) {
    if (!target) return { delivered: false, reason: "no target" };
    var pointerish = /^(pointer|mouse|click|contextmenu)/.test(type);
    if (pointerish && !hittable(target)) return { delivered: false, reason: "not hittable" };

    var defaultPrevented = false;
    var evt = init || {};
    evt.type = type;
    evt.target = target;
    evt.preventDefault = function () { if (!evt._passive) defaultPrevented = true; };
    evt.stopPropagation = function () { evt._stopped = true; };

    var chain = [];
    for (var e = target; e; e = e.parentNode) chain.push(e);
    chain.push(documentStub, windowStub);

    for (var i = 0; i < chain.length && !evt._stopped; i++) {
        var regs = chain[i]._listeners && chain[i]._listeners[type];
        if (!regs) continue;
        var copy = regs.slice();
        for (var k = 0; k < copy.length; k++) {
            evt._passive = copy[k].passive;
            evt.currentTarget = chain[i];
            try { copy[k].fn(evt); } catch (err) { absorbed.push(String(err)); }
            if (copy[k].once) chain[i].removeEventListener(type, copy[k].fn);
        }
    }
    // An inline onclick= in the markup is NOT modelled: the stub never parses
    // HTML attributes. Only a handler assigned as a property is seen here.
    if (type === "click" && typeof target.onclick === "function" && !evt._stopped) {
        try { target.onclick(evt); } catch (err) { absorbed.push(String(err)); }
    }
    return { delivered: true, defaultPrevented: defaultPrevented };
}

var els = {};
declaredIds.forEach(function (id) { els[id] = new El(id); });

var documentStub = {
    hidden: false,
    _vis: [],
    getElementById: function (id) { requestedIds[id] = true; return els[id] || null; },
    createElement: function () { return new El("_new"); },
    _listeners: {},
    addEventListener: function (ev, fn, opts) {
        if (ev === "visibilitychange") documentStub._vis.push(fn);
        El.prototype.addEventListener.call(documentStub, ev, fn, opts);
    },
    removeEventListener: function (ev, fn) { El.prototype.removeEventListener.call(documentStub, ev, fn); }
};
var windowStub = {
    _listeners: {},
    addEventListener: function (ev, fn, opts) { El.prototype.addEventListener.call(windowStub, ev, fn, opts); },
    removeEventListener: function (ev, fn) { El.prototype.removeEventListener.call(windowStub, ev, fn); }
};

// Haptics. Feature-detected by the page, absent on iOS Safari entirely, so the
// recorder exists to prove the page GUARDS the call rather than assuming it.
var vibes = [];
var navigatorStub = { vibrate: function (p) { vibes.push(p); return true; }, maxTouchPoints: 5 };

/* ---- fake device ---------------------------------------------------------
 * Mirrors the JSON shapes of /api/llm/status, /api/llm/models, /api/llm/result
 * and /api/llm/generate as emitted by WebPage_LLM.cpp.
 * ---------------------------------------------------------------------- */
var DEV = {
    status: { state: "UNLOADED", model: "", arch: "", quant: "", menu: { groups: 0, gen: 0 }, ctxWarn: false },
    models: [],
    loadReject: null,
    endError: null,          // reason the device reports on the terminal poll
    dropPolls: 0,            // fail this many result polls at the transport level
    // A generate the device REFUSES ({ok:false}), versus one that never lands at
    // all (the fetch itself rejects). They take different branches in the page
    // and both were entirely unexercised until now.
    generateRefuse: null,
    generateThrows: false,
    streamForever: false,
    lastGenerateBody: null,
    // The device serves a real answer, a slice at a time, from the byte offset
    // the page asks for. A stub that ignored `offset` made two field-realistic
    // regressions invisible: dropping `ctx.pollOffset += j.text.length` (every
    // chunk re-served from 0, so the answer duplicates without bound) and
    // dropping the `j.stale` guard (a superseded generation bleeding into the
    // turn that replaced it). Neither shows up as a blank screen, which is why
    // neither would be noticed by hand.
    // Contains a MULTI-BYTE character on purpose. With a pure-ASCII answer the
    // exact-equality assertion below cannot distinguish a byte offset from a
    // code-unit offset, which is the one thing it is supposed to prove.
    answer: "The quick brown fox \u2014 jumps over the lazy dog.",
    // 10 bytes per poll, chosen so the em dash (bytes 20-22) lands WHOLE inside
    // a chunk. That isolates the page's own offset arithmetic from the separate
    // question of a device cutting a character in half at its buffer edge.
    answerChunk: 10,
    genSession: 0,       // the generation the device is currently serving
    nextSession: 41,
    statusHits: 0, modelHits: 0, resultHits: 0, stopHits: 0, unloadHits: 0
};

// UTF-8 codec for the fake device.
//
// The real endpoint indexes its stream buffer in BYTES (chatGetStreamChunk does
// memcpy(buf, t.text + offset, copy); the handler documents "buffered tokens
// since byte offset"), while a JS string is indexed in UTF-16 code units.
// Slicing DEV.answer with String.prototype.slice makes the two agree for ASCII
// and ONLY for ASCII -- precisely the regime where an offset bug cannot appear.
// A pure-ASCII fixture made the exact-answer assertion look like proof that the
// page's offset accounting was right when it proved nothing of the kind.
//
// escape/unescape rather than TextEncoder: this harness also runs on jsc and
// osascript, neither of which has TextEncoder. encodeURIComponent produces the
// UTF-8 bytes and unescape maps each to one code unit below 256, so the result
// can be sliced byte-accurately with ordinary string operations.
function u8bytes(text) { return unescape(encodeURIComponent(text)); }
function u8text(bytes) {
    // A chunk that ends mid-sequence cannot be decoded; the browser substitutes
    // U+FFFD there too. Only reachable when answerChunk splits a character.
    try { return decodeURIComponent(escape(bytes)); } catch (e) { return "\ufffd"; }
}

function queryOf(url) {
    var out = {}, q = url.indexOf("?");
    if (q < 0) return out;
    url.slice(q + 1).split("&").forEach(function (kv) {
        var i = kv.indexOf("=");
        if (i > 0) out[kv.slice(0, i)] = kv.slice(i + 1);
    });
    return out;
}
function clone(o) { return JSON.parse(JSON.stringify(o)); }

var hw = {
    /* mirrors the shipped hw.$ exactly: string -> getElementById (through the
     * requestedIds recorder), anything else passed through untouched */
    "$": function (x) { return typeof x === "string" ? documentStub.getElementById(x) : x; },
    /* Same bodies as the shipped runtime (WebServer_Utils.cpp:773): null-guarded
     * element ops routed through the recording $ so id-agreement still sees them. */
    show: function (x) { var el = hw.$(x); if (el) el.style.display = ""; },
    hide: function (x) { var el = hw.$(x); if (el) el.style.display = "none"; },
    toggle: function (x, sh) { (sh ? hw.show : hw.hide)(x); },
    setText: function (x, v) { var el = hw.$(x); if (el) el.textContent = v; },
    setHTML: function (x, v) { var el = hw.$(x); if (el) el.innerHTML = v; },
    on: function (e, v, f) { if (e) e.addEventListener(v, f); },
    fetchJSON: function (url) {
        if (url.indexOf("/api/llm/status") === 0) { DEV.statusHits++; return P(clone(DEV.status)); }
        if (url.indexOf("/api/llm/models") === 0) { DEV.modelHits++; return P(clone(DEV.models)); }
        if (url.indexOf("/api/llm/result") === 0) {
            DEV.resultHits++;
            // Transport failure, not an error REPLY: the fetch itself rejects,
            // which is the only thing that counts toward giving up.
            if (DEV.dropPolls > 0) { DEV.dropPolls--; return P(new Error("net"), true); }
            var q = queryOf(url);
            // A poll for a generation the device is no longer serving gets told
            // so. The real device does this when a turn is superseded or
            // stopped; the page is supposed to end the turn rather than treat
            // the reply as content.
            if (parseInt(q.session, 10) !== DEV.genSession) return P({ stale: true });
            var off = parseInt(q.offset, 10) || 0;
            var allBytes = u8bytes(DEV.answer);
            var sliceBytes = allBytes.slice(off, off + DEV.answerChunk);
            var slice = u8text(sliceBytes);
            // streamForever holds the turn open past the end of the answer
            // (empty slices, never done) so a test can span several heartbeat
            // intervals without the generation finishing under it.
            var done = !DEV.streamForever &&
                       (off + sliceBytes.length) >= allBytes.length;
            if (done && DEV.endError) {
                return P({ text: slice, done: true, len: allBytes.length,
                           next: off + sliceBytes.length, error: DEV.endError });
            }
            // `len` is the TOTAL buffered length, as the real handler emits it --
            // deliberately NOT the end of this chunk. A page that mistakes it for
            // one truncates the answer.
            // `next` is the ABSOLUTE byte cursor the device served up to --
            // authoritative, because only the device knows how many bytes it
            // copied. `len` is the TOTAL buffered length and is deliberately
            // NOT a cursor; a client that confuses the two truncates the answer.
            return P({ text: slice, done: done, len: allBytes.length,
                       next: off + sliceBytes.length });
        }
        if (url.indexOf("/api/llm/menu") === 0) return P({ schema: 1, gen: 0, groups: [] });
        return P({});
    },
    postJSON: function (url, body) {
        if (url.indexOf("/api/llm/load") === 0) {
            // A rejected select leaves device state untouched -- that is exactly
            // the case where the status poll cannot tell the user anything.
            if (DEV.loadReject) return P({ ok: false, error: DEV.loadReject });
            DEV.status = { state: "LOADING", model: body.model, arch: "", quant: "", menu: { groups: 0, gen: 0 } };
            return P({ ok: true });
        }
        if (url.indexOf("/api/llm/generate") === 0) {
            DEV.lastGenerateBody = body;
            if (DEV.generateRefuse) return P({ ok: false, error: DEV.generateRefuse });
            if (DEV.generateThrows) return P(new Error("net down"), true);
            DEV.status.state = "GENERATING";
            DEV.genSession = ++DEV.nextSession;
            return P({ ok: true, session: DEV.genSession });
        }
        if (url.indexOf("/api/llm/stop") === 0) {
            DEV.stopHits++;
            DEV.genSession = 0;          // nothing is being served now
            return P({ ok: true });
        }
        if (url.indexOf("/api/llm/unload") === 0) {
            DEV.unloadHits++;
            DEV.status = { state: "UNLOADED", model: "", arch: "", quant: "",
                           menu: { groups: 0, gen: 0 }, ctxWarn: false };
            return P({ ok: true });
        }
        return P({ ok: true });
    },
    postFormText: function () { return P(""); }
};

// Seed BEFORE the page runs: its first catalog fetch happens during the IIFE.
DEV.models = [
    { id: "/models/pokemon.bin", name: "pokemon", size: 6291456, storage: "flash", available: true },
    {
        id: "cm5:LFM2-8B-A1B-UD-Q3_K_XL", name: "LFM2-8B-A1B-UD-Q3_K_XL",
        size: 3676307456, storage: "remote", backend: "cm5", available: false
    }
];

/* ---- run the real page JS ------------------------------------------------
 * new Function() is load-bearing for portability, not a stylistic choice. Deno
 * runs a .js file as an ES module (strict mode, top-level var is not global);
 * jsc and osascript run it as a classic sloppy script. A new Function body is
 * sloppy-mode and parameter-injected on every engine, so browser-shaped page
 * code behaves identically everywhere. Do not "modernize" this to import/eval.
 * ---------------------------------------------------------------------- */
var pageSrc = slurp(PAGE_JS_PATH);
new Function("document", "window", "hw", "setTimeout", "clearTimeout", "console", "navigator", pageSrc)(
    documentStub, windowStub, hw, setTimeoutStub, clearTimeoutStub,
    { log: function () {}, error: function () {}, warn: function () {} }, navigatorStub
);

// The page assigns its handlers as `window.qaAsk = ...` and then CALLS them by
// bare name from its own listeners (the keydown handler does `qaAsk()`). In a
// browser that resolves because `window` IS the global object; here `window` is
// a parameter holding a plain object, so a bare `qaAsk` misses entirely and
// throws ReferenceError. Mirror the assignments onto the real global so page
// code behaves the way it does in a browser.
//
// Done AFTER the page has run, so the guided-ask strip's late re-wrap of qaAsk
// is the version mirrored. A page that reassigned window.X later would leave
// this stale -- acceptable, and noted so it is not a surprise.
for (var _k in windowStub) {
    if (typeof windowStub[_k] === "function") {
        try { globalThis[_k] = windowStub[_k]; } catch (e) { /* frozen global */ }
    }
}

var pill = function () { return els["qa-state"].textContent; };

/* ------------------------------------------------------------------------
 * PRECONDITION, checked before the page is even run: every id the JS looks up
 * must be declared in the page markup.
 *
 * The markup lives in one raw-string literal and the JS in another, in the same
 * header, with nothing tying them together. Renaming an id on one side only is a
 * realistic edit that ships a page which TypeErrors on load. This is scanned out
 * of the SOURCE rather than observed at runtime deliberately: a missing element
 * makes the page collapse a few statements later, and a cascade of downstream
 * failures buries the one line that says why.
 * ---------------------------------------------------------------------- */
var srcIds = [], m, idRe = /(?:getElementById|hw\.\$)\((['"])([A-Za-z0-9_-]+)\1\)/g;
while ((m = idRe.exec(pageSrc)) !== null) { if (srcIds.indexOf(m[2]) < 0) srcIds.push(m[2]); }
var undeclared = srcIds.filter(function (id) { return declaredIds.indexOf(id) < 0; });
check("every id the page JS looks up is declared in the page HTML",
      srcIds.length > 0 && undeclared.length === 0,
      srcIds.length === 0 ? "found no getElementById/hw.$ calls at all"
                          : "undeclared: " + undeclared.join(", "));

/* The scenario runs inside a function so that a crash still produces a verdict.
 * A harness that dies mid-way prints no sentinel at all, and "no output" is the
 * least actionable failure there is -- it looks identical to a harness that
 * never started. */
function scenario() {
    /* ═══ SCENARIO ══════════════════════════════════════════════════════════ */

    // 1. Page opens with the CM5 host DOWN: the onboard model is present and the
    //    remote row is listed but not selectable. This is the state a user hit.
    var r0 = rebuilds;
    check("initial catalog rendered", modelOpts.length === 2,
          modelOpts.map(function (o) { return o.t; }).join(" | "));
    check("remote size scales to GB", modelOpts.length > 1 && /3\.4GB/.test(modelOpts[1].t),
          modelOpts.length > 1 ? modelOpts[1].t : "(missing)");
    check("onboard size scales to MB", /6\.0MB/.test(modelOpts[0].t), modelOpts[0].t);
    check("unavailable row disabled", modelOpts.length > 1 && modelOpts[1].d === true);

    // 2. Idle heartbeats. Nothing changed, so nothing should be said or rebuilt --
    //    a poll that re-announces would spam the chat log every few seconds.
    advance(30000);
    check("idle: no-model announced exactly once",
          initWrites === 1 && /No model loaded/.test(els["qa-init-msg"].textContent),
          initWrites + " writes over " + DEV.statusHits + " polls");
    check("idle: catalog not rebuilt while unchanged", rebuilds === r0, "rebuilds=" + (rebuilds - r0));
    check("idle: heartbeat kept polling", DEV.statusHits > 5, DEV.statusHits + " status polls in 30s");

    // 3. The CM5 host comes up. The row must become selectable with NO page refresh
    //    -- the whole reason the heartbeat exists.
    DEV.models[1].available = true;
    advance(6000);
    check("host up: row selectable without refresh", modelOpts.length > 1 && modelOpts[1].d === false,
          modelOpts.length > 1 ? modelOpts[1].t : "(missing)");
    check("host up: catalog rebuilt exactly once", rebuilds === r0 + 1, "rebuilds=" + (rebuilds - r0));

    // 4. User picks the remote model and clicks Load. The host takes ~30s to swap.
    els["qa-model"].value = "cm5:LFM2-8B-A1B-UD-Q3_K_XL";
    windowStub.qaLoadModel();
    advance(1);
    check("load: pill shows Loading", pill() === "Loading...", pill());
    check("load: cadence tightened", nextDelay() <= 1000, "next beat in " + nextDelay() + "ms");
    var sysAtLoad = sysLog.length;
    DEV.models[0].size = 6291457;            // force a catalog change mid-load
    advance(30000);
    check("loading: pill still Loading", pill() === "Loading...", pill());
    check("loading: no chat spam during the wait", sysLog.length === sysAtLoad,
          sysLog.slice(sysAtLoad).join(" / "));
    check("loading: selection survived a rebuild",
          els["qa-model"].value === "cm5:LFM2-8B-A1B-UD-Q3_K_XL", els["qa-model"].value);

    // 5. The host finishes and reports ready. A remote model leaves LLMConfig
    //    ZEROED, which is what used to render as the fabricated "(Llama - FP32)".
    DEV.status = {
        state: "READY", model: "cm5:LFM2-8B-A1B-UD-Q3_K_XL", arch: "", quant: "",
        dim: 0, vocab: 0, layers: 0, heads: 0, seqLen: 0, psramKB: 0,
        menu: { groups: 0, gen: 0 }, ctxWarn: false
    };
    advance(1500);
    var loaded = sysLog.filter(function (m) { return /^Model loaded:/.test(m); });
    check("ready: announced without a refresh", loaded.length === 1, JSON.stringify(loaded));
    check("ready: no fabricated arch/quant", loaded.length === 1 && loaded[0].indexOf("(") === -1,
          loaded[0] || "(no announcement)");
    check("ready: pill shows Ready", pill() === "Ready", pill());

    // 6. Steady state: the announcement must not repeat on every beat.
    advance(60000);
    check("steady: model-loaded said exactly once",
          sysLog.filter(function (m) { return /^Model loaded:/.test(m); }).length === 1,
          sysLog.filter(function (m) { return /^Model loaded:/.test(m); }).length + " times");
    check("steady: cadence relaxed", nextDelay() > 1000, "next beat in " + nextDelay() + "ms");

    // 7. A hidden tab suspends polling; re-showing catches up immediately.
    var hitsBefore = DEV.statusHits;
    documentStub.hidden = true;
    advance(30000);
    check("hidden tab: polling suspended", DEV.statusHits === hitsBefore,
          (DEV.statusHits - hitsBefore) + " polls while hidden");
    documentStub.hidden = false;
    documentStub._vis.forEach(function (f) { f(); });
    advance(1);
    check("re-shown: caught up immediately", DEV.statusHits > hitsBefore,
          (DEV.statusHits - hitsBefore) + " polls after re-show");

    // 8. A dropped poll must not wipe a catalog we already have.
    var savedFetch = hw.fetchJSON;
    hw.fetchJSON = function (url) {
        if (url.indexOf("/api/llm/models") === 0) return P(new Error("net"), true);
        return savedFetch(url);
    };
    advance(6000);
    check("transient failure keeps the list", modelOpts.length === 2, modelOpts.length + " options remain");
    hw.fetchJSON = savedFetch;

    // 9. During generation the heartbeat must stand down: the page already runs a
    //    150ms result poll, and a second request stream would compete for the
    //    device's single httpd worker exactly when the LLM task is most
    //    latency-sensitive (httpd is one of the few things that can preempt it).
    //     The window has to be LONGER THAN A HEARTBEAT INTERVAL or the assertion is
    //     vacuous: advance less than one interval and no beat was ever due, so the
    //     count stays at zero whether the guard exists or not. (Measured -- an
    //     earlier 400ms window passed with the guard deleted.) So hold the answer
    //     stream open across several intervals instead.
    var WINDOW = 20000;
    // MEASURE the budget, do not compute it. This was
    // Math.floor(20000 / 5000) >= 3 — i.e. 4 >= 3 over two literals — which
    // could not fail for any page or any window, and fed a fabricated
    // denominator into the failure text below. A control window of identical
    // length with the page NOT generating says what the window is really worth.
    // One WINDOW constant drives both advances so they cannot silently decouple.
    var ctrl0 = DEV.statusHits;
    advance(WINDOW);
    var beatsDue = DEV.statusHits - ctrl0;

    DEV.streamForever = true;
    els["qa-input"].value = "hello";
    var sHits = DEV.statusHits, mHits = DEV.modelHits;
    windowStub.qaAsk();
    advance(WINDOW);
    check("generating: the answer stream ran", DEV.resultHits >= 10, DEV.resultHits + " result polls");
    check("generating: heartbeats were genuinely due", beatsDue >= 3, beatsDue + " intervals elapsed");
    check("generating: no heartbeat status polls", DEV.statusHits === sHits,
          (DEV.statusHits - sHits) + " status polls across " + beatsDue + " due beats");
    check("generating: no heartbeat catalog polls", DEV.modelHits === mHits,
          (DEV.modelHits - mHits) + " catalog polls across " + beatsDue + " due beats");
    // The device frames the prompt per backend at one chokepoint
    // (llmBackendFramePrompt): the onboard engine gets "Q: ...\nA:" and a remote
    // source deliberately gets NOTHING, because it applies its own chat template
    // and local scaffolding would appear verbatim in the answer. This page used
    // to frame client-side, which the onboard path tolerated (its framer passes
    // through anything already starting with "Q:") but which reached the CM5 raw.
    var sent = (DEV.lastGenerateBody && DEV.lastGenerateBody.prompt) || "";
    check("generate: prompt is sent unframed", sent === "hello", JSON.stringify(sent));

    DEV.streamForever = false;
    DEV.status = { state: "READY", model: "cm5:LFM2-8B-A1B-UD-Q3_K_XL", arch: "", quant: "", menu: { groups: 0, gen: 0 } };
    advance(12000);
    check("after generation: heartbeat resumed", DEV.statusHits > sHits + 1,
          (DEV.statusHits - sHits) + " polls after finish");
    // The page's headline feature, and until now its only coverage was a poll
    // COUNTER. Severing `ctx.aText.textContent += j.text` blanks the answer pane
    // for every user and left all 15 tests green.
    // EXACT equality, not a shape test. This is the assertion that proves the
    // page advanced its read offset correctly: served 7 bytes at a time, any
    // failure to advance duplicates chunks and any over-advance drops them, and
    // only the exact string rules both out.
    check("generation: the answer was rendered exactly once",
          answerText === DEV.answer,
          JSON.stringify(answerText));
    check("after generation: no stray re-announce",
          sysLog.filter(function (m) { return /^Model loaded:/.test(m); }).length === 1,
          sysLog.filter(function (m) { return /^Model loaded:/.test(m); }).length + " announcements total");

    // 12. A load the device REFUSES must say why. The status poll cannot report it:
    //     a select that fails without changing state leaves the signature identical,
    //     so the edge-triggered announcer is silent by design and this is the only
    //     place the reason can surface.
    DEV.loadReject = "model not found";
    var sysBeforeReject = sysLog.length;
    windowStub.qaLoadModel();
    advance(2000);
    var rejectLines = sysLog.slice(sysBeforeReject).filter(function (m) { return /Load failed/.test(m); });
    check("refused load reports the reason", rejectLines.length === 1 && /model not found/.test(rejectLines[0]),
          sysLog.slice(sysBeforeReject).join(" / ") || "(silence)");
    DEV.loadReject = null;

}

var scenarioError = null;
try { scenario(); }
catch (e) { scenarioError = String(e && e.stack ? e.stack : e); }
check("harness scenario ran to completion", scenarioError === null, scenarioError || "");


// 13. Do:-mode is on-device only. A remote model has never seen this device's
//     command vocabulary, so it would invent a plausible-looking command — and
//     the Do: UI renders the answer next to a Run button. The device refuses it
//     too; the page stopping first is what turns a failed round trip into an
//     explanation.
    DEV.status.cmdMode = false;
    advance(6000);                       // let the status poll publish cmdMode
    DEV.lastGenerateBody = null;
    var sysBeforeDo = sysLog.length;
    els["qa-input"].value = "do: turn off wifi";
    windowStub.qaAsk();
    advance(500);
    check("Do: on a remote model sends no request", DEV.lastGenerateBody === null,
          JSON.stringify(DEV.lastGenerateBody));
    var doLines = sysLog.slice(sysBeforeDo).filter(function (m) { return /Do: mode needs/.test(m); });
    check("Do: on a remote model explains why", doLines.length === 1,
          sysLog.slice(sysBeforeDo).join(" / ") || "(silence)");
    check("Do: on a remote model keeps the typed text",
          els["qa-input"].value === "do: turn off wifi", els["qa-input"].value);
    // ...and the button must not be offered at all for a model that cannot do it.
    check("Do: button is withdrawn for a model that cannot",
          els["qa-do"].style.display === "none", els["qa-do"].style.display);

//     ...and it must still work where it is supported, or the guard has simply
//     broken the feature rather than scoped it.
    DEV.status.cmdMode = true;
    advance(6000);
    DEV.streamForever = true;
    els["qa-input"].value = "do: turn off wifi";
    windowStub.qaAsk();
    advance(500);
    var doSent = (DEV.lastGenerateBody && DEV.lastGenerateBody.prompt) || "";
    check("Do: on the on-device model still runs", doSent === "Do: turn off wifi",
          JSON.stringify(doSent));
    DEV.streamForever = false;
    advance(3000);                       // drain it; the next step needs !busy

//     The Do: BUTTON, which is the discoverable path. A press-and-hold gesture
//     alone would strand keyboard, screen-reader and switch users on the one
//     feature that turns model output into an executed device command.
    // A disabled button is correct when the page is not ready, so establish
    // READY first -- otherwise this asserts the wrong thing and passes for the
    // wrong reason later.
    DEV.status = { state: "READY", model: "/models/pokemon.bin", arch: "", quant: "",
                   cmdMode: true, menu: { groups: 0, gen: 0 }, ctxWarn: false };
    advance(8000);
    check("Do: button is offered when the model allows it",
          els["qa-do"].style.display === "" && els["qa-do"].disabled === false,
          "display=" + JSON.stringify(els["qa-do"].style.display) +
          " disabled=" + els["qa-do"].disabled);
    DEV.lastGenerateBody = null;
    els["qa-input"].value = "turn off wifi";
    windowStub.qaAsk("do");
    advance(500);
    check("Do: button sends the Do: marker",
          DEV.lastGenerateBody && DEV.lastGenerateBody.prompt === "Do: turn off wifi",
          JSON.stringify(DEV.lastGenerateBody && DEV.lastGenerateBody.prompt));
    DEV.streamForever = false;
    advance(3000);

// 14. A superseded generation must END the turn, not bleed into the one that
//     replaced it. The device answers {stale:true}; a page that ignores it polls
//     a dead session forever and never releases the UI. This is invisible to
//     casual use — it is not a blank screen, it is a turn that quietly never
//     finishes — which is exactly why it needs a test.
    DEV.streamForever = true;
    els["qa-input"].value = "first question";
    windowStub.qaAsk();
    advance(600);
    var textAtSupersede = answerText;
    DEV.genSession = 9999;                   // the device moved on
    advance(3000);
    check("stale session ends the turn",
          els["qa-stop"].style.display === "none",
          "stop button display=" + JSON.stringify(els["qa-stop"].style.display));
    check("stale session appends nothing further",
          answerText === textAtSupersede, JSON.stringify(answerText));
    DEV.streamForever = false;
    advance(2000);

// 15. Stop. The abort path was never exercised at all; a bug here leaves a hung
//     turn or a session that keeps streaming into a page the user has moved on
//     from, neither of which looks like a failure from the outside.
    DEV.streamForever = true;
    els["qa-input"].value = "another question";
    windowStub.qaAsk();
    advance(600);
    var stopsBefore = DEV.stopHits, pollsAtStop = DEV.resultHits;
    windowStub.qaStop();
    advance(3000);
    check("stop: the device was told", DEV.stopHits === stopsBefore + 1,
          DEV.stopHits + " stop posts");
    check("stop: the turn ended", els["qa-stop"].style.display === "none",
          "stop button display=" + JSON.stringify(els["qa-stop"].style.display));
    // Exactly zero, not "few". The device also marks the session stale on stop,
    // so a page that never aborted client-side would still END the turn — one
    // poll later, via the stale reply. That masks the abort almost completely;
    // the one thing it still uniquely buys is not making that extra request.
    // Assert the round trip, or the abort has no test at all.
    check("stop: no further round trip", DEV.resultHits === pollsAtStop,
          (DEV.resultHits - pollsAtStop) + " polls after stop");
    DEV.streamForever = false;
    advance(2000);

// 16. Unload acknowledges explicitly instead of falling through to the generic
//     "no model loaded" line — and must not then be re-announced by the
//     heartbeat, which is what the afterGen signature adoption is for.
    var sysBeforeUnload = sysLog.length;
    windowStub.qaUnloadModel();
    advance(20000);
    var after = sysLog.slice(sysBeforeUnload);
    check("unload: acknowledged exactly once",
          after.filter(function (m) { return /^Model unloaded$/.test(m); }).length === 1,
          after.join(" / ") || "(silence)");
    check("unload: not re-announced by the heartbeat",
          after.filter(function (m) { return /No model loaded/.test(m); }).length === 0,
          after.join(" / ") || "(silence)");

// 17. The two ways a generation can fail before it ever starts: the device
//     REFUSES it, or the request never lands. Both branches end the turn today,
//     and neither was exercised — so nothing would notice if a refactor dropped
//     one of the finishGen calls.
//
//     The consequence of dropping one is why this is worth checking: `busy`
//     would stay true forever, Stop would stick on, the input would stay
//     locked, and beat() bails on `busy` — so the heartbeat would stand down
//     permanently too. Nothing on screen would say why. Only a reload recovers.
//
//     The scenario ends with no model, which disables the input for an ordinary
//     reason. Restore READY first, or every assertion below is vacuous.
    DEV.status = { state: "READY", model: "cm5:LFM2-8B-A1B-UD-Q3_K_XL", arch: "", quant: "",
                   cmdMode: true, menu: { groups: 0, gen: 0 }, ctxWarn: false };
    advance(8000);
    function uiState() {
      return els["qa-stop"].style.display + "|" + els["qa-ask"].style.display +
             "|" + els["qa-input"].disabled + "|" + els["qa-ask"].disabled;
    }
    var readyUi = uiState();

    // (a) the device refuses
    var errsBefore = errLines.length;
    var beatsBefore = DEV.statusHits;
    DEV.generateRefuse = "engine busy";
    els["qa-input"].value = "will be refused";
    windowStub.qaAsk();
    advance(2000);
    var refusedErrs = errLines.slice(errsBefore);
    check("refused generate: reports the reason",
          refusedErrs.length === 1 && /engine busy/.test(refusedErrs[0]),
          JSON.stringify(refusedErrs));
    check("refused generate: the turn ended", uiState() === readyUi,
          uiState() + "  (ready was " + readyUi + ")");
    DEV.generateRefuse = null;

    // The load-bearing one: buttons can look right while the page is still
    // dead, because the heartbeat stands down on `busy` and never recovers.
    advance(20000);
    check("refused generate: the heartbeat kept running",
          DEV.statusHits - beatsBefore >= 3,
          (DEV.statusHits - beatsBefore) + " status polls after the refusal");

    // (b) the request never lands
    errsBefore = errLines.length;
    var beatsBefore2 = DEV.statusHits;
    DEV.generateThrows = true;
    els["qa-input"].value = "will be rejected";
    windowStub.qaAsk();
    advance(2000);
    var rejectedErrs = errLines.slice(errsBefore);
    check("rejected generate: reports the failure",
          rejectedErrs.length === 1 && /request failed/.test(rejectedErrs[0]),
          JSON.stringify(rejectedErrs));
    check("rejected generate: the turn ended", uiState() === readyUi,
          uiState() + "  (ready was " + readyUi + ")");
    DEV.generateThrows = false;
    advance(20000);
    check("rejected generate: the heartbeat kept running",
          DEV.statusHits - beatsBefore2 >= 3,
          (DEV.statusHits - beatsBefore2) + " status polls after the rejection");

    // (c) and the page is genuinely usable again, not merely repainted
    DEV.lastGenerateBody = null;
    els["qa-input"].value = "a normal question";
    windowStub.qaAsk();
    advance(3000);
    check("after an error the next question still runs",
          DEV.lastGenerateBody !== null &&
          DEV.lastGenerateBody.prompt === "a normal question",
          JSON.stringify(DEV.lastGenerateBody));

// 18. Enter-to-send. This is the positive control for the event dispatcher: it
//     exercises a listener the PAGE registered, so if the registry or the bubble
//     chain regressed, this goes red rather than the gesture checks silently
//     passing on a stub that delivers nothing.
//
//     It also covers a real path that has never been tested. The keydown handler
//     was registered against a no-op addEventListener, so deleting it entirely
//     used to change nothing anywhere in this suite.
    DEV.status = { state: "READY", model: "/models/pokemon.bin", arch: "", quant: "",
                   cmdMode: true, menu: { groups: 0, gen: 0 }, ctxWarn: false };
    advance(8000);
    DEV.lastGenerateBody = null;
    els["qa-input"].value = "sent with the keyboard";
    dispatch(els["qa-input"], "keydown", { key: "Enter", shiftKey: false });
    advance(500);
    check("Enter in the input sends the question",
          DEV.lastGenerateBody &&
          DEV.lastGenerateBody.prompt === "sent with the keyboard",
          JSON.stringify(DEV.lastGenerateBody && DEV.lastGenerateBody.prompt));
    DEV.streamForever = false;
    advance(3000);

    // Shift+Enter is a newline, not a send. Without this the check above would
    // pass just as happily against a handler that sends on every keystroke.
    DEV.lastGenerateBody = null;
    els["qa-input"].value = "should not send";
    dispatch(els["qa-input"], "keydown", { key: "Enter", shiftKey: true });
    advance(500);
    check("Shift+Enter does not send", DEV.lastGenerateBody === null,
          JSON.stringify(DEV.lastGenerateBody));

// LAST, deliberately. This used to sit before sections 13-18, so anything they
// absorbed was never checked -- which is exactly how a ReferenceError thrown by
// the page's own keydown handler stayed invisible until the Enter control was
// written. An exception check that does not cover the whole run is decoration.
check("page threw no exceptions", absorbed.length === 0,
      absorbed.length + " absorbed: " + absorbed.slice(0, 3).join(" | "));

// 19. A turn abandoned by the DEVICE must say why. Observed on hardware:
//     stopping the CM5 daemon mid-generation ended the turn correctly but left
//     an empty answer and no explanation, because the only status refresh after
//     a turn runs with announcements suppressed.
    DEV.status = { state: "READY", model: "cm5:x", arch: "", quant: "",
                   cmdMode: false, menu: { groups: 0, gen: 0 } };
    advance(8000);
    var errsBeforeEnd = errLines.length;
    DEV.endError = "the CM5 went away mid-answer";
    els["qa-input"].value = "will be abandoned";
    windowStub.qaAsk();
    advance(4000);
    var endErrs = errLines.slice(errsBeforeEnd);
    check("an abandoned turn reports why",
          endErrs.length === 1 && /went away mid-answer/.test(endErrs[0]),
          JSON.stringify(endErrs));
    DEV.endError = null;

// 20. Losing the connection mid-answer. A SINGLE dropped poll is normal and
//     costs nothing -- the retry re-reads the same byte cursor -- so a blip must
//     not end the turn. An unbroken outage must, and must not resume afterwards.
    DEV.status = { state: "READY", model: "cm5:x", arch: "", quant: "",
                   cmdMode: false, menu: { groups: 0, gen: 0 } };
    advance(8000);

    // (a) a transient blip: the answer still assembles exactly.
    answerText = "";
    DEV.lastGenerateBody = null;
    els["qa-input"].value = "survives a blip";
    windowStub.qaAsk();
    advance(300);
    DEV.dropPolls = 12;                  // a few seconds of failures, then recovery
    advance(20000);
    check("a transient outage does not end the turn",
          answerText === DEV.answer, JSON.stringify(answerText));
    DEV.dropPolls = 0;
    advance(3000);

    // (b) an outage that never recovers: end it, say so, and stay ended.
    var errsBeforeLost = errLines.length;
    answerText = "";
    els["qa-input"].value = "will be lost";
    windowStub.qaAsk();
    advance(300);
    DEV.dropPolls = 100000;              // never comes back within the budget
    advance(40000);
    var lostErrs = errLines.slice(errsBeforeLost);
    check("a lost connection ends the turn",
          lostErrs.length === 1 && /connection lost/.test(lostErrs[0]) &&
          els["qa-stop"].style.display === "none",
          JSON.stringify(lostErrs) + " stop=" + els["qa-stop"].style.display);
    // The device comes back. The ended turn must NOT resume -- that is the whole
    // point of terminating rather than retrying indefinitely.
    var textAtEnd = answerText;
    var pollsAtEnd = DEV.resultHits;
    DEV.dropPolls = 0;
    advance(20000);
    check("a recovered connection does not resume an ended turn",
          answerText === textAtEnd && DEV.resultHits === pollsAtEnd,
          "text changed=" + (answerText !== textAtEnd) +
          " extra polls=" + (DEV.resultHits - pollsAtEnd));

__out(failures === 0 ? "HARNESS_RESULT PASS" : "HARNESS_RESULT FAIL " + failures);

// osascript echoes the script's last expression value to stdout; keep it empty.
undefined;
