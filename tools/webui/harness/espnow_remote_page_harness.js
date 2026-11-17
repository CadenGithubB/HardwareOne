/* Behavioral harness for the remote-command parts of the shipping
 * web_assets/espnow-core.js: espnowRemoteAck, doRemoteExec and the remote file
 * browser (browseRemoteFiles).
 *
 * The page talks to a fake device. What the fake device answers to
 * `espnowremote` is not written here: tools/webui/tests/test_espnow_remote_page.py
 * takes the acknowledgement and every refusal from System_ESPNow.cpp (the text
 * acknowledgement is printed by the production formatter) and hands them over
 * as a JSON fixture, so a reworded reply fails here instead of in a browser.
 *
 *   __argv[0]  the shipping web_assets/espnow-core.js
 *   __argv[1]  the fixture: {mac, acks:[{text,reqId}], jsonAck:{text,reqId},
 *              refusals:[text], httpRefusals:[text], listing:text}
 *
 * Requires _bootstrap.js from tools/webui/js_engine.py (__out, slurp, __argv).
 * Promises are a synchronous thenable and timers a virtual clock, so every
 * scenario is deterministic. The verdict is the HARNESS_RESULT line.
 */
var passes = 0, failures = 0;
function check(name, condition, detail) {
    if (condition) { passes++; __out('PASS ' + name); }
    else { failures++; __out('FAIL ' + name + (detail === undefined ? '' : '   [' + String(detail) + ']')); }
}

var fixture = JSON.parse(slurp(__argv[1]));
var MAC = fixture.mac;

/* Every exception a page callback throws is recorded; a non-empty list fails. */
var absorbed = [];
function P(value, isError) {
    function next(fn) {
        try { var r = fn(value); return (r && r.__thenable) ? r : P(r); }
        catch (e) { absorbed.push(String(e && e.stack ? e.stack : e)); return P(e, true); }
    }
    return {
        __thenable: true,
        then: function (f) { return (isError || !f) ? this : next(f); },
        catch: function (f) { return (isError && f) ? next(f) : this; },
        finally: function (f) { if (f) { try { f(); } catch (e) { absorbed.push(String(e)); } } return this; }
    };
}

/* Virtual clock: timers fire in (time, insertion) order. */
var now = 0, timers = [], nextTimer = 1, insertion = 0;
function setTimeoutStub(fn, ms) {
    var t = { id: nextTimer++, at: now + (ms || 0), ord: insertion++, fn: fn, every: 0 };
    timers.push(t);
    return t.id;
}
function setIntervalStub(fn, ms) {
    var t = { id: nextTimer++, at: now + ms, ord: insertion++, fn: fn, every: ms };
    timers.push(t);
    return t.id;
}
function clearStub(id) { timers = timers.filter(function (t) { return t.id !== id; }); }
function advance(ms) {
    var end = now + ms;
    for (;;) {
        timers.sort(function (a, b) { return a.at - b.at || a.ord - b.ord; });
        if (!timers.length || timers[0].at > end) break;
        var t = timers[0];
        now = t.at;
        if (t.every) { t.at += t.every; t.ord = insertion++; } else { timers.shift(); }
        try { t.fn(); } catch (e) { absorbed.push(String(e && e.stack ? e.stack : e)); }
    }
    now = end;
}

/* Fake DOM and device. */
var elements = {};
function el(id) {
    if (!elements[id]) elements[id] = { id: id, innerHTML: '', textContent: '', style: {}, value: '' };
    return elements[id];
}
var device = {
    cliReply: '',          // body of the next /api/cli response
    cliStatus: 200,
    posted: [],            // command lines the page posted
    store: [],             // /api/espnow/messages records, ascending seq
    pageSize: 3,
    fetches: [],           // URLs of every messages request
    fetchFails: false
};
function cliResponse() {
    var body = device.cliReply, status = device.cliStatus;
    return { ok: status >= 200 && status < 300, status: status, text: function () { return P(body); } };
}
var hw = {
    $: function (x) { return typeof x === 'string' ? el(x) : x; },
    setText: function (x, t) { var e = hw.$(x); if (e) e.textContent = t; },
    setHTML: function (x, h) { var e = hw.$(x); if (e) e.innerHTML = h; },
    postForm: function (url, form) {
        device.posted.push(form && form.cmd);
        return P(cliResponse());
    },
    postFormText: function (url, form) {
        device.posted.push(form && form.cmd);
        var r = cliResponse();
        return r.ok ? P(device.cliReply) : P(new Error('HTTP ' + r.status), true);
    },
    fetchJSON: function (url) {
        device.fetches.push(url);
        if (device.fetchFails) return P(new Error('HTTP 500'), true);
        var since = Number((/[?&]since=(\d+)/.exec(url) || [0, 0])[1]);
        var macArg = /[?&]mac=([^&]*)/.exec(url);
        var only = macArg ? decodeURIComponent(macArg[1]).toUpperCase() : null;
        var page = device.store.filter(function (m) {
            return m.seq > since && (!only || m.mac.toUpperCase() === only);
        }).slice(0, device.pageSize);
        return P({ messages: page, deliveries: [] });
    }
};
var win = (typeof globalThis !== 'undefined') ? globalThis : this;
var consoleErrors = [];
var fakeConsole = {
    log: function () {}, warn: function () {}, info: function () {},
    error: function () { consoleErrors.push(Array.prototype.slice.call(arguments).map(String).join(' ')); }
};
var alerts = [];
var explorerCalls = [];
win.BondFs = {
    renderExplorer: function (containerId, path, entries, opts) {
        explorerCalls.push({ containerId: containerId, path: path, entries: entries, opts: opts });
        el(containerId).innerHTML = '[explorer ' + path + ' ' + entries.length + ']';
    }
};

try {
    new Function('window', 'hw', 'console', 'setTimeout', 'setInterval', 'clearInterval', 'clearTimeout', 'alert',
                 slurp(__argv[0]))(win, hw, fakeConsole, setTimeoutStub, setIntervalStub, clearStub, clearStub,
                                   function (m) { alerts.push(String(m)); });
} catch (e) {
    consoleErrors.push('espnow-core.js threw: ' + e);
}
check('load espnow-core.js without errors', consoleErrors.length === 0, consoleErrors.join(' | '));

/* The message log: record lines instead of building DOM bubbles. */
var logLines = [];
function bubble() {
    var status = { innerHTML: '' }, text = { textContent: '' };
    return {
        className: 'message-bubble message-sent', status: status,
        querySelector: function (sel) { return sel === '.message-status' ? status : text; }
    };
}
win.appendLogLine = function (containerId, type, message, state) {
    var b = bubble();
    logLines.push({ container: containerId, type: type, message: message, state: state, bubble: b });
    return b;
};

/* Store helpers. Records are shaped as handleEspNowMessages emits them. */
var seq = 100;
function record(mac, reqId, text, piece, of, sent) {
    return { seq: ++seq, reqId: reqId, piece: piece || 1, of: of || 1, mac: mac, name: 'peer', msg: text,
             enc: true, ts: 1, type: sent ? 0 : 6, sent: !!sent, sendState: 0 };
}
function pieces(mac, reqId, text) {
    var out = [], total = Math.ceil(text.length / 200) || 1;
    for (var k = 0; k < total; k++) out.push(record(mac, reqId, text.substr(k * 200, 200), k + 1, total));
    return out;
}
function resetScenario() {
    timers = [];
    device.posted = []; device.store = []; device.fetches = []; device.fetchFails = false;
    device.cliStatus = 200; device.pageSize = 3;
    explorerCalls = []; logLines = []; alerts = [];
    el('remote-user-' + MAC).value = 'admin';
    el('remote-pass-' + MAC).value = 'hunter2';
    el('remote-fexplorer-' + MAC).innerHTML = '';
    el('remote-fstat-' + MAC).textContent = '';
    el('ru-' + MAC).value = 'admin';
    el('rp-' + MAC).value = 'hunter2';
    el('rc-' + MAC).value = 'status';
}
function panel() { return el('remote-fexplorer-' + MAC).innerHTML; }
function status() { return el('remote-fstat-' + MAC).textContent; }
function has(text, part) { return String(text).indexOf(part) >= 0; }

var ack = fixture.jsonAck;           // what `espnowremote ... files json "<path>"` answers
var OTHER = '02:00:00:00:00:99';

/* ---- A: reading the acknowledgement --------------------------------------- */
var ackReader = (typeof win.espnowRemoteAck === 'function') ? win.espnowRemoteAck : null;
check('A0 the page has one reader for the espnowremote reply', ackReader !== null);
if (ackReader) {
    fixture.acks.forEach(function (a, i) {
        var r = ackReader(a.text);
        check('A1 text acknowledgement ' + i + ' is recognised by its request id',
              r.sent === true && r.reqId === a.reqId, JSON.stringify(r) + ' for ' + a.text);
    });
    var j = ackReader(ack.text);
    check('A2 JSON acknowledgement is recognised by its request id', j.sent === true && j.reqId === ack.reqId,
          JSON.stringify(j));
    /* One check for all of them, so the number of checks does not move with
     * the number of refusals the firmware has. */
    var allRefusals = fixture.refusals.concat(fixture.httpRefusals);
    var misread = allRefusals.filter(function (text) {
        var r = ackReader(text);
        return !(r.sent === false && r.reqId === 0 && r.error.length > 0 && r.error.indexOf('OK: ') !== 0);
    });
    check('A3 no refusal of espnowremote is read as an acknowledgement (' + allRefusals.length + ' replies)',
          allRefusals.length >= 10 && misread.length === 0, misread.join(' || '));
    check('A4 an id inside an echoed command is not the request id',
          ackReader("OK: Target device 'x (match reqId 5)' not found or not paired.").sent === false &&
          ackReader('').sent === false && ackReader(null).sent === false);
}

/* ---- B: doRemoteExec ------------------------------------------------------ */
resetScenario();
device.cliReply = fixture.acks[0].text;
win.doRemoteExec(MAC);
check('B1 the command is posted as espnowremote <mac> <user> <pass> <command>',
      device.posted.length === 1 && device.posted[0] === 'espnowremote ' + MAC + ' admin hunter2 status',
      device.posted.join(' | '));
check('B1 the acknowledgement is not shown as the result',
      logLines.filter(function (l) { return has(l.message, 'Result:') || has(l.message, 'ASYNC'); }).length === 0,
      logLines.map(function (l) { return l.type + ':' + l.message; }).join(' | '));
check('B1 the sent bubble says Sent', logLines.length === 1 && has(logLines[0].bubble.status.innerHTML, 'Sent') &&
      !has(logLines[0].bubble.status.innerHTML, 'ASYNC'), logLines[0] && logLines[0].bubble.status.innerHTML);
check('B1 the command input is cleared after a sent command', el('rc-' + MAC).value === '');

fixture.refusals.slice(0, 3).concat(fixture.httpRefusals.slice(0, 1)).forEach(function (text, i) {
    resetScenario();
    device.cliReply = text;
    device.cliStatus = (fixture.httpRefusals.indexOf(text) >= 0) ? 400 : 200;
    win.doRemoteExec(MAC);
    var errors = logLines.filter(function (l) { return l.type === 'ERROR'; });
    check('B2 refusal ' + i + ' is shown as not sent, with its reason',
          errors.length === 1 && has(errors[0].message, 'Not sent') && logLines.length >= 1 &&
          has(logLines[0].bubble.status.innerHTML, 'Not sent') &&
          logLines.filter(function (l) { return has(l.message, 'Result:'); }).length === 0,
          logLines.map(function (l) { return l.type + ':' + l.message + '/' + l.bubble.status.innerHTML; }).join(' | '));
    check('B2 refusal ' + i + ' keeps the typed command', el('rc-' + MAC).value === 'status');
});

/* ---- C: remote file browser ----------------------------------------------- */
function browse(path) { win.browseRemoteFiles(MAC, path || '/'); }

/* C1: the listing arrives in pieces, behind unrelated records, over several pages. */
resetScenario();
device.cliReply = ack.text;
var stale = JSON.stringify({ success: true, dirPerms: 7, files: [{ name: 'STALE.txt', type: 'file', size: '1 bytes', perms: 7 }] });
device.store = [record(MAC, 0, 'hello'), record(OTHER, ack.reqId, 'not this peer')]
    .concat(pieces(MAC, ack.reqId - 1, stale))
    .concat([record(MAC, ack.reqId, 'echo of a sent row', 1, 1, true)]);
browse('/');
check('C1 the page sends files json with the path in quotes',
      device.posted.length === 1 && device.posted[0] === 'espnowremote ' + MAC + ' admin hunter2 files json "/"',
      device.posted.join(' | '));
check('C1 the acknowledgement is not shown in the file panel',
      !has(panel(), 'reqId') && !has(panel(), 'ASYNC') && !has(status(), 'ASYNC') && explorerCalls.length === 0,
      panel() + ' // ' + status());
check('C1 the page waits for the peer after the acknowledgement', timers.length === 1 && device.fetches.length >= 1,
      'timers=' + timers.length + ' fetches=' + device.fetches.length + ' status=' + status());
check('C1 the wait reads this peer\'s records only', device.fetches.every(function (u) {
    return has(u, 'mac=' + encodeURIComponent(MAC));
}), device.fetches.join(' | '));
device.store = device.store.concat(pieces(MAC, ack.reqId, fixture.listing));
advance(1000);
check('C1 the listing is rendered once all its pieces are read', explorerCalls.length === 1,
      'explorer calls=' + explorerCalls.length + ' status=' + status() + ' panel=' + panel());
if (explorerCalls.length === 1) {
    var call = explorerCalls[0];
    var names = call.entries.map(function (e) { return (e.isDir ? 'D:' : 'F:') + e.name; }).join(',');
    var expected = JSON.parse(fixture.listing).files.length;
    check('C1 every entry of the listing is shown, folders first', call.entries.length === expected &&
          names.indexOf('D:') === 0 && !has(names, 'STALE') && call.entries[0].isDir === true, names);
    check('C1 the explorer gets the browsed path and the peer label',
          call.path === '/' && call.containerId === 'remote-fexplorer-' + MAC && call.opts.peerLabel === MAC);
    var file = call.entries.filter(function (e) { return !e.isDir; })[0];
    check('C1 a file entry carries its byte count', file && typeof file.size === 'number' && file.size > 0,
          JSON.stringify(file));
}
check('C1 the status reports the finished browse', has(status(), 'Browse complete'), status());
check('C1 a finished browse leaves no timer running', timers.length === 0, timers.length);

/* C2: the text acknowledgement (no json word seen by the device) works the same. */
resetScenario();
device.cliReply = fixture.acks[0].text;
device.store = pieces(MAC, fixture.acks[0].reqId, fixture.listing);
browse('/');
check('C2 a text acknowledgement leads to the listing as well', explorerCalls.length === 1 && timers.length === 0,
      'explorer calls=' + explorerCalls.length + ' status=' + status());

/* C3: the peer refuses the account: its answer is shown, the wait ends. */
resetScenario();
device.cliReply = ack.text;
browse('/');
device.store = [record(MAC, ack.reqId, 'Auth failed')];
advance(5000);
check('C3 the peer\'s refusal is shown', has(panel(), 'Auth failed') && has(status(), 'Auth failed') &&
      explorerCalls.length === 0, panel() + ' // ' + status());
check('C3 the wait ends after the peer\'s answer', timers.length === 0, timers.length);

/* C4: nothing comes back: a clear message, and the wait ends. */
resetScenario();
device.cliReply = ack.text;
browse('/logs');
advance(60000);
check('C4 no answer ends with a clear message', has(panel(), 'No answer from ' + MAC) && has(status(), 'No answer') &&
      explorerCalls.length === 0, panel() + ' // ' + status());
check('C4 the page stops waiting', timers.length === 0, timers.length);
var fetchesAtEnd = device.fetches.length;
advance(60000);
check('C4 no request is made after the wait ended', device.fetches.length === fetchesAtEnd,
      device.fetches.length - fetchesAtEnd);

/* C5: this device refuses: nothing was sent, nothing is polled. */
fixture.refusals.slice(0, 2).concat(fixture.httpRefusals.slice(0, 1)).forEach(function (text, i) {
    resetScenario();
    device.cliReply = text;
    device.cliStatus = (fixture.httpRefusals.indexOf(text) >= 0) ? 400 : 200;
    browse('/');
    check('C5 refusal ' + i + ' ends the browse as not sent', has(panel(), 'Not sent') && has(status(), 'Not sent') &&
          device.fetches.length === 0 && timers.length === 0 && explorerCalls.length === 0,
          panel() + ' // ' + status() + ' fetches=' + device.fetches.length);
});

/* C6: the peer answers the listing request with an error document. */
resetScenario();
device.cliReply = ack.text;
device.store = [record(MAC, ack.reqId, '{"success":false,"error":"Directory not found or not accessible"}')];
browse('/nope');
check('C6 a listing error from the peer is shown', has(panel(), 'Directory not found or not accessible') &&
      explorerCalls.length === 0 && timers.length === 0, panel() + ' timers=' + timers.length);

/* C7: pieces of the listing are missing (pushed out of the device's store). */
resetScenario();
device.cliReply = ack.text;
device.store = pieces(MAC, ack.reqId, fixture.listing).slice(1);
browse('/');
advance(60000);
check('C7 an incomplete listing is reported as incomplete', has(panel(), 'incomplete') && has(panel(), ' of ') &&
      explorerCalls.length === 0 && timers.length === 0, panel() + ' timers=' + timers.length);

/* C8: peer text reaches the panel as text. */
resetScenario();
device.cliReply = ack.text;
device.store = [record(MAC, ack.reqId, '<img src=x onerror=alert(1)>')];
browse('/<b>');
advance(5000);
check('C8 a peer answer and the path are escaped', !has(panel(), '<img') && has(panel(), '&lt;img') &&
      !has(panel(), '<b>') && has(panel(), '&lt;b&gt;'), panel());

/* C9: a second browse of the same peer retires the first. */
resetScenario();
device.cliReply = ack.text;
browse('/');
device.cliReply = fixture.acks[1].text;
device.store = pieces(MAC, fixture.acks[1].reqId, fixture.listing);
browse('/logs');
check('C9 the newer browse renders its listing', explorerCalls.length === 1 && explorerCalls[0].path === '/logs',
      explorerCalls.length);
advance(60000);
check('C9 the retired browse does not overwrite it', has(status(), 'Browse complete') && has(panel(), '[explorer /logs'),
      status() + ' // ' + panel());

/* C10: a later browse starts where the last one stopped reading. */
resetScenario();
device.cliReply = ack.text;
var newer = pieces(MAC, ack.reqId, fixture.listing);
device.store = newer;
browse('/');
var sinceValues = device.fetches.map(function (u) { return Number((/since=(\d+)/.exec(u) || [0, 0])[1]); });
check('C10 a later browse does not re-read old records', sinceValues.length > 0 && sinceValues[0] > 0 &&
      explorerCalls.length === 1, sinceValues.join(','));

/* C11: the device restarted and numbers its records from 1 again. */
resetScenario();
device.cliReply = ack.text;
seq = 0;
device.store = pieces(MAC, ack.reqId, fixture.listing);
browse('/');
advance(60000);
check('C11 records numbered anew after a restart are still found', explorerCalls.length === 1 && timers.length === 0,
      'explorer calls=' + explorerCalls.length + ' status=' + status());

/* C12: the messages endpoint fails throughout: the wait still ends. */
resetScenario();
device.cliReply = ack.text;
device.fetchFails = true;
browse('/');
advance(60000);
check('C12 a failing messages endpoint still ends the wait', timers.length === 0 && has(status(), 'No answer'),
      status() + ' timers=' + timers.length);

/* C13: without a name or password nothing is sent. */
resetScenario();
el('remote-pass-' + MAC).value = '';
browse('/');
check('C13 a missing password sends nothing', device.posted.length === 0 && has(status(), 'username and password'),
      status());

check('no page callback threw', absorbed.length === 0, absorbed.join(' | '));
check('no console errors', consoleErrors.length === 0, consoleErrors.join(' | '));
__out('HARNESS_RESULT ' + (failures ? 'FAIL ' + failures : 'PASS') + ' checks=' + (passes + failures));
undefined;
