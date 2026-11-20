/* Host harness for test_web_untrusted_names.py (readiness G-50 / G-51).
 * tools/webui/js_engine.run_js prepends tools/webui/harness/_bootstrap.js,
 * which provides __out, slurp and __argv on every engine (node, deno, jsc, ...).
 *
 *   __argv[0]  WebServer_Utils.h getFileBrowserScript() block 1 (window.FileBrowser,
 *              createFileExplorer)
 *   __argv[1]  the same script's block 2 (window.BondFs)
 *   __argv[2]  the WebPage_Settings.h Part 4 <script> that defines scanNetworks
 *   __argv[3]  the hw runtime <script> of WebServer_Utils.cpp (hw.esc, fetchJSON,
 *              fetchText, postFormText)
 *   __argv[4]  System_Camera_DVP_Web.h loadCameraRecordings + deleteCameraRecording
 *   __argv[5]  System_Microphone_Web.h loadMicRecordings + deleteMicRecording
 *   __argv[6]  WebPage_Sensors.h hwRenderGenericSensor
 *   __argv[7]  WebPage_Bond.cpp renderBondProgress
 *
 * The recording lists (R), remote-sensor rows (G) and bond progress card (P)
 * run on the REAL hw runtime over a small node DOM: a recording name holding
 * markup and quotes must become no element, Play/Delete must get the original
 * name, and the delete command must quote it so it parses back unchanged.
 * H checks that hw.fetchJSON/fetchText reject with the reply's reason.
 *
 * All three run unmodified against a fake hw/DOM. A filename, path, peer label or
 * beacon SSID holding markup, quotes and backticks must reach innerHTML as literal
 * text, and every inline onclick handler must still hand the original string to
 * its function (evaluated here after entity decoding, the way a browser does).
 * The verdict is the HARNESS_RESULT line; exit codes differ across engines.
 */
var __passed = 0;
var __failed = 0;
function check(cond, label, detail) {
  if (cond) {
    __passed++;
    __out('PASS ' + label);
  } else {
    __failed++;
    __out('FAIL ' + label + (detail === undefined ? '' : ' :: ' + String(detail)));
  }
}

var win = (typeof globalThis !== 'undefined') ? globalThis : this;
win.window = win;
var consoleErrors = [];
var fakeConsole = {
  log: function () {},
  warn: function () {},
  info: function () {},
  error: function () {
    consoleErrors.push(Array.prototype.slice.call(arguments).map(String).join(' '));
  }
};

// Any script that escapes its string and runs calls one of these.
var alerts = [];
win.alert = function (m) { alerts.push(String(m)); };

var elements = {};
function el(id) {
  if (!elements[id]) elements[id] = { id: id, innerHTML: '', textContent: '', style: {}, value: '', dataset: {} };
  return elements[id];
}
var hw = {
  $: function (x) { return typeof x === 'string' ? el(x) : x; },
  show: function () {},
  hide: function () {},
  setText: function (x, t) { var e = (typeof x === 'string') ? el(x) : x; if (e) e.textContent = t; },
  fetchJSON: function () { return Promise.resolve({}); },
  postFormText: function () { return Promise.resolve(''); }
};

// /api/files/list replies, by requested path.
var listings = {};
function fakeFetch(url) {
  var m = String(url).match(/[?&]path=([^&]*)/);
  var path = m ? decodeURIComponent(m[1]) : '';
  var body = listings[path] || { success: true, files: [] };
  return Promise.resolve({
    status: 200,
    json: function () { return Promise.resolve(JSON.parse(JSON.stringify(body))); }
  });
}

var cliReplies = {};
function postSettingsCli(cmd) { return Promise.resolve(cliReplies[cmd] || ''); }
var settingsState = { state: { currentSSID: 'CleanAP', savedSSIDs: ['CleanAP'] } };

function load(path, label, names, values) {
  var source = slurp(path);
  var before = consoleErrors.length;
  try {
    Function.apply(null, names.concat([source])).apply(win, values);
  } catch (e) {
    consoleErrors.push(label + ' threw: ' + e);
  }
  check(consoleErrors.length === before, 'load ' + label + ' without errors', consoleErrors.slice(before).join(' | '));
}

// One pass, the way a browser decodes an attribute value: decimal and hex
// character references, the common named ones, and the legacy semicolon-less
// &quot &amp &lt &gt when no letter, digit or '=' follows. A payload spelled
// as an entity (&quot; &#34; &#x22;) must not be under-decoded here.
var NAMED = { quot: '"', amp: '&', lt: '<', gt: '>', apos: "'", nbsp: '\u00a0' };
function decodeEntities(s) {
  return s.replace(/&(?:#(\d+);?|#[xX]([0-9a-fA-F]+);?|([a-zA-Z]+);|(quot|amp|lt|gt)(?![A-Za-z0-9=]))/g,
    function (all, dec, hex, named, legacy) {
      if (dec) return String.fromCodePoint(parseInt(dec, 10));
      if (hex) return String.fromCodePoint(parseInt(hex, 16));
      var k = named || legacy;
      return Object.prototype.hasOwnProperty.call(NAMED, k) ? NAMED[k] : all;
    });
}

// Resolves after `cond()` holds or after `hops` microtask turns.
function settle(cond, hops) {
  return Promise.resolve().then(function () {
    if (cond() || hops <= 0) return undefined;
    return settle(cond, hops - 1);
  });
}

// Markup that must never survive as an element, whatever the sink.
function noLiveMarkup(html) {
  return html.indexOf('<img src=x') < 0 && html.indexOf('<svg') < 0 && html.indexOf('<b>') < 0 &&
         html.indexOf('<i>') < 0;
}

// Runs every onclick="..." of `html` the way a browser would (entities decoded,
// `event` bound) and returns the error list; spies record the calls.
function runHandlers(html) {
  var errors = [];
  var re = /onclick="([^"]*)"/g;
  var m;
  while ((m = re.exec(html)) !== null) {
    try {
      new Function('event', decodeEntities(m[1])).call(win, { stopPropagation: function () {} });
    } catch (e) {
      errors.push(m[1] + ' -> ' + e);
    }
  }
  return errors;
}

/* A small node DOM for the pages that build rows with createElement. Text is
 * escaped when serialized, so markup only survives as an element (a node) or as
 * a raw innerHTML string, and both are checked. */
var byId = {};
function escText(t) { return String(t).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;'); }
function makeNode(tag) {
  var n = { tagName: String(tag).toUpperCase(), children: [], attrs: {}, listeners: {}, style: {},
            className: '', _text: '', _html: '' };
  Object.defineProperty(n, 'textContent', {
    get: function () { return this._text + this.children.map(function (c) { return c.textContent; }).join(''); },
    set: function (v) { this._text = String(v); this._html = ''; this.children = []; }
  });
  Object.defineProperty(n, 'innerHTML', {
    get: function () { return escText(this._text) + this._html + this.children.map(outerHTML).join(''); },
    set: function (v) { this._html = String(v); this._text = ''; this.children = []; }
  });
  n.appendChild = function (c) { this.children.push(c); return c; };
  n.setAttribute = function (k, v) { this.attrs[k] = String(v); };
  n.getAttribute = function (k) { return Object.prototype.hasOwnProperty.call(this.attrs, k) ? this.attrs[k] : null; };
  n.addEventListener = function (t, f) { (this.listeners[t] = this.listeners[t] || []).push(f); };
  n.click = function () { var self = this; (this.listeners.click || []).forEach(function (f) { f.call(self, {}); }); };
  n.querySelectorAll = function () { return []; };
  return n;
}
function outerHTML(n) {
  var a = Object.keys(n.attrs).map(function (k) { return ' ' + k + '="' + escText(n.attrs[k]).replace(/"/g, '&quot;') + '"'; }).join('');
  if (n.className) a += ' class="' + escText(n.className).replace(/"/g, '&quot;') + '"';
  return '<' + n.tagName.toLowerCase() + a + '>' + n.innerHTML + '</' + n.tagName.toLowerCase() + '>';
}
function walk(n, fn) { fn(n); n.children.forEach(function (c) { walk(c, fn); }); }
function nodesBy(root, pred) { var out = []; walk(root, function (n) { if (pred(n)) out.push(n); }); return out; }
function rawHtmlIn(root) { var out = []; walk(root, function (n) { if (n._html) out.push(n._html); }); return out.join(''); }
var fakeDoc = {
  createElement: makeNode,
  getElementById: function (id) { return byId[id] || null; },
  documentElement: { dataset: {} },
  querySelectorAll: function () { return []; }
};
function domEl(id) { var n = makeNode('div'); n.id = id; byId[id] = n; return n; }

/* The device side of the runtime: replies by URL, CLI posts recorded. */
var cliPosts = [];
var cliStatus = {};
function reply(status, body) {
  return {
    ok: status >= 200 && status < 300, status: status,
    text: function () { return Promise.resolve(body); },
    json: function () { return Promise.resolve(JSON.parse(body)); }
  };
}
function formField(body, key) {
  var parts = String(body || '').split('&');
  for (var i = 0; i < parts.length; i++) {
    var kv = parts[i].split('=');
    if (decodeURIComponent(kv[0]) === key) return decodeURIComponent(kv.slice(1).join('='));
  }
  return null;
}
var routes = {};
function runtimeFetch(url, opts) {
  url = String(url);
  if (url === '/api/cli') {
    var cmd = formField(opts && opts.body, 'cmd');
    cliPosts.push(cmd);
    var st = cliStatus[cmd] || [200, 'Deleted'];
    return Promise.resolve(reply(st[0], st[1]));
  }
  var r = routes[url];
  return Promise.resolve(r ? reply(r[0], r[1]) : reply(404, ''));
}
// The device's rule for a positional quoted token (CommandArgs::parse): the
// token is everything up to the first " after the opening one, with no
// escapes; anything after that closing quote would be another argument,
// which micdelete/cameravideodelete refuse.
function parseQuotedToken(line, verb) {
  if (line.indexOf(verb + ' "') !== 0) return null;
  var body = line.slice(verb.length + 2), end = body.indexOf('"');
  if (end < 0 || end !== body.length - 1) return null;
  return body.slice(0, end);
}

function recordingsAndRuntime() {
  var rw = win;  // the page scripts name window globals (loadCameraRecordings) bare
  var pageAlerts = [];
  var confirms = [];
  var timers = [];
  var played = [];
  var deleted = [];
  load(__argv[3], 'hw runtime', ['window', 'document', 'fetch', 'console'], [rw, fakeDoc, runtimeFetch, fakeConsole]);
  var rhw = rw.hw || {};
  check(typeof rhw.esc === 'function', 'H0 hw.esc defined');
  if (typeof rhw.esc === 'function') {
    check(rhw.esc('<a href="x">&\'') === '&lt;a href=&quot;x&quot;&gt;&amp;&#39;', 'H0 hw.esc escapes & < > " \'', rhw.esc('<a href="x">&\''));
    check(rhw.esc(null) === '' && rhw.esc(7) === '7', 'H0 hw.esc takes null and numbers');
  }
  routes['/api/err-text'] = [400, 'Error: X'];
  routes['/api/err-json'] = [403, '{"ok":false,"error":"Error: admin only"}'];
  routes['/api/err-msg'] = [400, '{"message":"why not"}'];
  routes['/api/err-empty'] = [503, ''];
  routes['/api/ok'] = [200, '{"v":1}'];
  function rejects(p, label, message, status) {
    return p.then(function (v) {
      check(false, label, 'resolved with ' + JSON.stringify(v));
    }, function (e) {
      check(e && e.message === message && e.status === status, label,
            (e && e.message) + ' / ' + (e && e.status));
    });
  }
  var hp = (typeof rhw.fetchJSON === 'function') ? Promise.all([
    rejects(rhw.fetchJSON('/api/err-text'), 'H1 fetchJSON rejects with the reply text reason', 'X', 400),
    rejects(rhw.fetchText('/api/err-text'), 'H1 fetchText rejects with the reply text reason', 'X', 400),
    rejects(rhw.fetchJSON('/api/err-json'), 'H2 fetchJSON rejects with a JSON error field', 'admin only', 403),
    rejects(rhw.fetchText('/api/err-msg'), 'H2 fetchText rejects with a JSON message field', 'why not', 400),
    rejects(rhw.fetchJSON('/api/err-empty'), 'H3 an empty refusal falls back to HTTP n', 'HTTP 503', 503),
    rhw.fetchJSON('/api/ok').then(function (d) { check(d && d.v === 1, 'H4 fetchJSON still resolves a 200 reply'); },
                                  function (e) { check(false, 'H4 fetchJSON still resolves a 200 reply', e); })
  ]) : Promise.resolve(check(false, 'H runtime loaded'));

  /* R: camera and microphone recording lists. */
  var NAME = 'a"b\'<img src=x onerror=alert(1)>.avi';
  var SLASHED = 'c\\d.avi';
  var TRAILING = 'f\\';
  routes['/api/videos'] = [200, JSON.stringify({ sdAvailable: true, count: 4, files: [
    { name: NAME, size: 2048 }, { name: SLASHED, size: 1024 }, { name: TRAILING, size: 512 },
    { name: 'clean.avi', size: 4096 }] })];
  routes['/api/recordings'] = [200, JSON.stringify({ count: 4, files: [
    { name: NAME.replace('.avi', '.wav'), size: 2048 }, { name: SLASHED.replace('.avi', '.wav'), size: 1024 },
    { name: TRAILING, size: 512 }, { name: 'clean.wav', size: 4096 }] })];
  var camList = domEl('camera-recordings-list');
  domEl('camera-recordings-count');
  var micList = domEl('mic-recordings-list');
  domEl('mic-rec-count');
  rw.openAviPlayer = function (n) { played.push(n); };
  var stubs = {
    confirm: function (m) { confirms.push(m); return true; },
    hwConfirm: function (m) { confirms.push(m); return Promise.resolve(true); },
    alert: function (m) { pageAlerts.push(String(m)); },
    setTimeout: function (f) { timers.push(f); return 0; }
  };
  var names = ['window', 'hw', 'document', 'console', 'confirm', 'hwConfirm', 'alert', 'setTimeout'];
  var values = [rw, rhw, fakeDoc, fakeConsole, stubs.confirm, stubs.hwConfirm, stubs.alert, stubs.setTimeout];
  load(__argv[4], 'camera recordings script', names, values);
  load(__argv[5], 'microphone recordings script', names, values);
  return hp.then(function () {
    if (typeof rw.loadCameraRecordings === 'function') rw.loadCameraRecordings();
    if (typeof rw.loadMicRecordings === 'function') rw.loadMicRecordings();
    return settle(function () { return camList.children.length >= 4 && micList.children.length >= 4; }, 60);
  }).then(function () {
    [['camera', camList, '.avi'], ['mic', micList, '.wav']].forEach(function (c) {
      var list = c[1], ext = c[2], tag = 'R ' + c[0];
      var hostile = NAME.replace('.avi', ext), slashed = SLASHED.replace('.avi', ext);
      var html = list.innerHTML;
      check(nodesBy(list, function (n) { return n.tagName === 'IMG'; }).length === 0 && rawHtmlIn(list).indexOf('<img') < 0,
            tag + ' list renders no element from a recording name', rawHtmlIn(list).slice(0, 300));
      check(html.indexOf('<img src=x') < 0 && html.indexOf('&lt;img src=x onerror=alert(1)&gt;') >= 0,
            tag + ' recording name is shown as literal text', html.slice(0, 300));
      check(list.children.length === 4, tag + ' one row per recording', list.children.length);
      check(nodesBy(list, function (n) { return n.getAttribute('onclick') !== null || n.getAttribute('data-name') !== null; }).length === 0,
            tag + ' rows carry no inline handler or name attribute');
      var urls = nodesBy(list, function (n) { return n.getAttribute('href') !== null || n.getAttribute('src') !== null; })
        .map(function (n) { return n.getAttribute('href') || n.getAttribute('src'); });
      var base = c[0] === 'camera' ? '/api/videos/file?name=' : '/api/recordings/file?name=';
      check(urls.indexOf(base + encodeURIComponent(hostile)) >= 0 && urls.indexOf(base + encodeURIComponent(slashed)) >= 0 &&
            urls.indexOf(base + 'clean' + ext) >= 0,
            tag + ' file URLs encode the name', JSON.stringify(urls));
      check(list.textContent.indexOf(hostile) >= 0 && list.textContent.indexOf('clean' + ext) >= 0,
            tag + ' row text is the original name');
    });
    // Play and Delete get the original names.
    nodesBy(camList, function (n) { return n.tagName === 'BUTTON'; }).forEach(function (b) { b.click(); });
    nodesBy(micList, function (n) { return n.tagName === 'BUTTON'; }).forEach(function (b) { b.click(); });
    return settle(function () { return cliPosts.length >= 6 && pageAlerts.length >= 2; }, 60);
  }).then(function () {
    check(played.length === 4 && played[0] === NAME && played[1] === SLASHED && played[2] === TRAILING &&
          played[3] === 'clean.avi',
          'R camera Play receives the original names', JSON.stringify(played));
    function verbNames(verb) {
      return cliPosts.filter(function (l) { return String(l).indexOf(verb + ' ') === 0; })
        .map(function (l) { return parseQuotedToken(l, verb); });
    }
    var cam = verbNames('cameravideodelete'), mic = verbNames('micdelete');
    // The device's parser reads the name back exactly, backslashes (even a
    // trailing one) included; a name holding " is refused on the page and
    // nothing is posted for it.
    check(JSON.stringify(cam) === JSON.stringify([SLASHED, TRAILING, 'clean.avi']),
          'R cameravideodelete quotes the name so the device parses it back unchanged', JSON.stringify(cliPosts));
    check(JSON.stringify(mic) === JSON.stringify([SLASHED.replace('.avi', '.wav'), TRAILING, 'clean.wav']),
          'R micdelete quotes the name so the device parses it back unchanged', JSON.stringify(cliPosts));
    check(cliPosts.indexOf('cameravideodelete "clean.avi"') >= 0 && cliPosts.indexOf('micdelete "clean.wav"') >= 0 &&
          cliPosts.indexOf('cameravideodelete "c\\d.avi"') >= 0,
          'R a name is sent between quotes unchanged, with no escapes', JSON.stringify(cliPosts));
    check(cliPosts.filter(function (l) { return String(l).indexOf('onerror') >= 0; }).length === 0,
          'R a name holding a quote posts nothing', JSON.stringify(cliPosts));
    check(pageAlerts.length === 2 && pageAlerts[0].indexOf('cannot be deleted here') >= 0 &&
          pageAlerts[1].indexOf('cannot be deleted here') >= 0,
          'R a name holding a quote raises the cannot-delete alert', pageAlerts.join(' | '));
    check(confirms.length === 6 && confirms[0] === 'Delete ' + SLASHED + '?', 'R each delete asks with the original name', JSON.stringify(confirms));
    pageAlerts.length = 0;
    // A refused delete shows the device's reason.
    cliStatus['micdelete "clean.wav"'] = [400, 'Error: Recording is still active/finalizing'];
    var before = pageAlerts.length;
    return Promise.resolve(rw.deleteMicRecording && rw.deleteMicRecording('clean.wav')).then(function () {
      return settle(function () { return pageAlerts.length > before; }, 30);
    }).then(function () {
      check(pageAlerts[before] === 'Recording is still active/finalizing', 'R a refused mic delete shows the reason', pageAlerts.join(' | '));
    });
  }).then(function () {
    /* G: generic remote-sensor rows; keys come from the remote device's JSON. */
    load(__argv[6], 'generic sensor renderer', ['window'], [rw]);
    var g = makeNode('div');
    if (typeof rw.hwRenderGenericSensor === 'function') {
      rw.hwRenderGenericSensor(g, { list: [{ '<img src=x onerror=alert(1)>': 1, ok: 2 }], obj: { '<svg onload=alert(2)>': 'v' } });
    }
    var gh = g.innerHTML;
    check(gh.indexOf('<img src=x') < 0 && gh.indexOf('<svg') < 0, 'G nested sensor keys are not rendered as elements', gh);
    check(gh.indexOf('&lt;img src=x onerror=alert(1)&gt;:1') >= 0 && gh.indexOf('&lt;svg onload=alert(2)&gt;:v') >= 0 &&
          gh.indexOf('ok:2') >= 0, 'G nested sensor keys are shown as literal text', gh);

    /* P: the bond progress card; the peer name may be the pairing-registry name. */
    var render = null;
    try {
      render = Function('hw', 'Date', 'var bondProgressStartMs = null;\n' + slurp(__argv[7]) + '\nreturn renderBondProgress;')(rhw, Date);
    } catch (e) {
      check(false, 'load bond progress renderer', e);
    }
    var hostile = { bonded: true, peerConfigured: true, peerOnline: false, agreement: 'declined', agreementReason: 'ended',
                    peerName: '<img src=x onerror=alert(1)>', peerMac: 'AA:BB:CC:DD:EE:FF' };
    var card = makeNode('div');
    if (render) render(hostile, card);
    var ch = card.innerHTML;
    check(ch.indexOf('<img src=x') < 0, 'P peer name is not rendered as an element', ch.slice(0, 400));
    check(ch.indexOf('&lt;img src=x onerror=alert(1)&gt; &middot; AA:BB:CC:DD:EE:FF') >= 0 &&
          ch.indexOf('&lt;img src=x onerror=alert(1)&gt; ended the bond') >= 0,
          'P peer name is shown as literal text in the title and the hint');
    var clean = makeNode('div');
    hostile.peerName = 'Kitchen';
    if (render) render(hostile, clean);
    check(clean.innerHTML.indexOf('Kitchen &middot;') >= 0 && clean.innerHTML.indexOf('Kitchen ended the bond') >= 0,
          'P a clean peer name renders unchanged');
    return undefined;
  });
}

load(__argv[0], 'file browser block', ['window', 'hw', 'console', 'fetch'], [win, hw, fakeConsole, fakeFetch]);
load(__argv[1], 'BondFs block', ['window', 'hw', 'console'], [win, hw, fakeConsole]);
load(__argv[2], 'Settings scan script', ['window', 'hw', 'console', 'postSettingsCli', '__S'],
     [win, hw, fakeConsole, postSettingsCli, settingsState]);
check(typeof win.createFileExplorer === 'function', 'createFileExplorer defined');
check(win.BondFs && typeof win.BondFs.renderExplorer === 'function', 'BondFs.renderExplorer defined');
check(typeof win.scanNetworks === 'function', 'scanNetworks defined');

/* F: the shared local explorer (Files, Logging, Maps pages). Names the upload
 * path accepts on LittleFS: markup, single quote, backtick, a quote-break. */
var IMG = '<img src=x onerror=alert(1)>.txt';
var QUOTE = "a'b`c');alert(2);//.log";
var DQ = 'd"q.txt';
var DIR = 'dir<svg onload=alert(3)>';
// Entity spellings of a quote, built only from characters normalizeFsPath
// accepts. escHtml must turn the '&' into &amp; or the browser decodes the
// entity inside onclick="..." into a quote that ends the JS string.
var AMP = '&quot;);alert(5);//.txt';
var AMPDIR = '&#x22;);alert(12);(&#34;d';
var files = [
  { name: IMG, type: 'file', size: '10 bytes', perms: 0x0F },
  { name: QUOTE, type: 'file', size: '11 bytes', perms: 0x0F },
  { name: DQ, type: 'file', size: '12 bytes', perms: 0x0F },
  { name: DIR, type: 'folder', size: '1 items', perms: 0x0F },
  { name: AMP, type: 'file', size: '14 bytes', perms: 0x0F },
  { name: AMPDIR, type: 'folder', size: '1 items', perms: 0x0F },
  { name: 'clean.txt', type: 'file', size: '13 bytes', perms: 0x0F }
];
listings['/'] = { success: true, files: files };
listings['/' + DIR] = { success: true, files: files };
listings['/errdir'] = { success: false, error: '<img src=x onerror=alert(4)>' };
var selected = [];
var edited = [];
var explorer = win.createFileExplorer({
  containerId: 'fx', path: '/', mode: 'full',
  onSelect: function (p) { selected.push(p); },
  onEdit: function (p) { edited.push(p); }
});
win.createFileExplorer({ containerId: 'fy', path: '/errdir', mode: 'full' });
var listEl = el('fexp-fx-list');
var crumbEl = el('fexp-fx-breadcrumb');
var errEl = el('fexp-fy-list');
function listed(e) { return e.innerHTML !== '' && e.innerHTML.indexOf('Loading...') < 0; }

var rootHtml = '';
settle(function () { return listed(listEl) && listed(errEl); }, 50).then(function () {
  rootHtml = listEl.innerHTML;
  check(noLiveMarkup(rootHtml), 'F1 hostile file names are not rendered as elements',
        rootHtml.indexOf('<img src=x') >= 0 ? 'raw <img src=x present' : 'raw <svg present');
  check(rootHtml.indexOf('&lt;img src=x onerror=alert(1)&gt;.txt') >= 0, 'F1 file name is shown as literal text');
  check(rootHtml.indexOf('dir&lt;svg onload=alert(3)&gt;') >= 0, 'F1 folder name is shown as literal text');
  check(rootHtml.indexOf('a&#39;b&#96;c&#39;);alert(2);//.log') >= 0, 'F1 quotes and backticks are escaped in the name');
  check(rootHtml.indexOf('<span>clean.txt</span>') >= 0, 'F1 clean name renders unchanged');
  check(rootHtml.indexOf('&amp;quot;);alert(5);//.txt') >= 0 && rootHtml.indexOf('&amp;#x22;);alert(12);(&amp;#34;d') >= 0,
        'F1 entity spellings in names are shown as literal text');
  var e = errEl.innerHTML;
  check(e.indexOf('<img src=x') < 0 && e.indexOf('&lt;img src=x onerror=alert(4)&gt;') >= 0,
        'F1 listing error is shown as literal text', e);
  // Into the hostile folder: the breadcrumb shows the segment.
  win.fexp_fxNavigate('/' + DIR);
  return settle(function () { return listed(listEl); }, 50);
}).then(function () {
  var crumb = crumbEl.innerHTML;
  check(noLiveMarkup(crumb) && crumb.indexOf('>dir&lt;svg onload=alert(3)&gt;</span>') >= 0,
        'F2 breadcrumb segment is shown as literal text', crumb);
  var calls = [];
  ['Navigate', 'Select', 'Download', 'Edit', 'Rename', 'Delete'].forEach(function (fn) {
    win['fexp_fx' + fn] = function () { calls.push([fn].concat(Array.prototype.slice.call(arguments))); };
  });
  var errors = runHandlers(crumb).concat(runHandlers(rootHtml)).concat(runHandlers(listEl.innerHTML));
  check(errors.length === 0, 'F3 every explorer onclick parses and runs', errors.join(' | '));
  check(alerts.length === 0, 'F3 no injected script ran', alerts.join(' | '));
  function called(fn, path, extra) {
    return calls.some(function (c) { return c[0] === fn && c[1] === path && (extra === undefined || c[2] === extra); });
  }
  var sub = '/' + DIR + '/';
  var want = [
    ['Navigate', '/'], ['Navigate', '/' + DIR], ['Navigate', sub + DIR],
    ['Select', '/' + IMG], ['Download', '/' + IMG], ['Edit', '/' + IMG], ['Rename', '/' + IMG],
    ['Select', '/' + QUOTE], ['Download', '/' + QUOTE], ['Delete', '/' + QUOTE, false],
    ['Select', '/' + DQ], ['Rename', '/' + DQ],
    ['Delete', '/' + DIR, true], ['Select', sub + IMG], ['Download', sub + QUOTE], ['Select', '/clean.txt'],
    ['Select', '/' + AMP], ['Download', '/' + AMP], ['Edit', '/' + AMP], ['Rename', '/' + AMP],
    ['Navigate', '/' + AMPDIR], ['Delete', '/' + AMPDIR, true], ['Select', sub + AMP]
  ];
  var missing = want.filter(function (w) { return !called(w[0], w[1], w[2]); });
  check(missing.length === 0, 'F3 handlers receive the original paths', JSON.stringify(missing) + ' got ' + JSON.stringify(calls.slice(0, 6)));
  var bad = calls.filter(function (c) { return typeof c[1] !== 'string' || c[1].charAt(0) !== '/'; });
  check(bad.length === 0, 'F3 no handler gets a mangled argument', JSON.stringify(bad));
  check(explorer && explorer.getCurrentPath() === '/' + DIR, 'F3 explorer navigated into the hostile folder');

  /* B: the bonded-peer explorer (Files, Logging, ESP-NOW pages). A bonded
   * peer's FS_LIST reply and label are peer-controlled. */
  var navs = [];
  var views = [];
  var bpath = '/p<i>x&#34;);alert(11);(&#34;';
  win.BondFs.renderExplorer('bx', bpath, [
    { name: '<img src=x onerror=alert(6)>', isDir: false, size: 5 },
    { name: "q'`\"<b>x", isDir: true, size: 0 },
    { name: 'clean.log', isDir: false, size: 7 },
    { name: '&quot;);alert(10);(&quot;', isDir: false, size: 8 },
    { name: '&#34;);alert(13);(&#34;', isDir: true, size: 0 }
  ], {
    peerLabel: '<b>peer</b>',
    onNavigate: function (p) { navs.push(p); },
    fileActions: [{ label: 'View', fn: function (p) { views.push(p); } }]
  });
  var b = el('bx').innerHTML;
  check(noLiveMarkup(b), 'B1 peer names, path and label are not rendered as elements', b.slice(0, 400));
  check(b.indexOf('&lt;img src=x onerror=alert(6)&gt;') >= 0, 'B1 peer file name is shown as literal text');
  check(b.indexOf('&lt;b&gt;peer&lt;/b&gt;') >= 0, 'B1 peer label is shown as literal text');
  check(b.indexOf('Path: /p&lt;i&gt;x') >= 0, 'B1 status-bar path is shown as literal text');
  check(b.indexOf('<span>clean.log</span>') >= 0, 'B1 clean peer name renders unchanged');
  check(b.indexOf('&amp;quot;);alert(10);(&amp;quot;') >= 0 && b.indexOf('/p&lt;i&gt;x&amp;#34;);alert(11);') >= 0,
        'B1 entity spellings in peer names and path are shown as literal text');
  var berrors = runHandlers(b);
  check(berrors.length === 0, 'B2 every bonded explorer onclick parses and runs', berrors.join(' | '));
  check(alerts.length === 0, 'B2 no injected script ran', alerts.join(' | '));
  function has(list, v) { return list.indexOf(v) >= 0; }
  check(has(navs, '/') && has(navs, bpath) && has(navs, bpath + "/q'`\"<b>x") &&
        has(navs, bpath + '/&#34;);alert(13);(&#34;'),
        'B2 navigation handlers receive the original paths', JSON.stringify(navs));
  check(has(views, bpath + '/<img src=x onerror=alert(6)>') && has(views, bpath + '/clean.log') &&
        has(views, bpath + '/&quot;);alert(10);(&quot;'),
        'B2 file actions receive the original paths', JSON.stringify(views));
  win.BondFs.renderExplorer('by', '/', [], { status: 'Error: <img src=x onerror=alert(7)>' });
  var bs = el('by').innerHTML;
  check(bs.indexOf('<img src=x') < 0 && bs.indexOf('Error: &lt;img src=x onerror=alert(7)&gt;') >= 0,
        'B3 status text from the peer is shown as literal text', bs);

  /* S: the admin Settings Wi-Fi scan. Beacon SSIDs come from anyone in range. */
  cliReplies['wifiscan json'] = JSON.stringify({ networks: [
    { ssid: '<svg onload=alert(8)>', rssi: -40, channel: 6, auth: '3' },
    { ssid: "a'b\"c`d<i>", rssi: -50, channel: 1, auth: '0' },
    { ssid: 'CleanAP', rssi: -60, channel: 11, auth: '3' }
  ] });
  win.scanNetworks();
  var scanEl = el('wifi-scan-results');
  return settle(function () { return scanEl.innerHTML.indexOf('Nearby Networks') >= 0; }, 50);
}).then(function () {
  var s = el('wifi-scan-results').innerHTML;
  check(noLiveMarkup(s), 'S1 scanned SSIDs are not rendered as elements', s.slice(0, 400));
  check(s.indexOf('&lt;svg onload=alert(8)&gt;') >= 0, 'S1 hostile SSID is shown as literal text');
  check(s.indexOf('a&#39;b&quot;c`d&lt;i&gt;') >= 0, 'S1 quotes in an SSID are escaped');
  check(s.indexOf('>CleanAP <span') >= 0 && s.indexOf('(Connected)') >= 0, 'S1 clean SSID renders unchanged with its badge');
  var ds = [];
  var re = /data-ssid="([^"]*)"/g;
  var m;
  while ((m = re.exec(s)) !== null) ds.push(decodeURIComponent(decodeEntities(m[1])));
  check(ds.length === 3 && ds[0] === '<svg onload=alert(8)>' && ds[1] === "a'b\"c`d<i>" && ds[2] === 'CleanAP',
        'S2 Select buttons still carry the raw SSIDs', JSON.stringify(ds));
  cliReplies['wifiscan json'] = JSON.stringify({ networks: [], error: '<img src=x onerror=alert(9)>' });
  win.scanNetworks();
  var scanEl = el('wifi-scan-results');
  return settle(function () { return scanEl.innerHTML.indexOf('Nearby Networks') >= 0; }, 50);
}).then(function () {
  var s = el('wifi-scan-results').innerHTML;
  check(s.indexOf('<img src=x') < 0 && s.indexOf('Scan &lt;img src=x onerror=alert(9)&gt;') >= 0,
        'S3 scan error text is shown as literal text', s);
  return recordingsAndRuntime();
}).then(function () {
  check(alerts.length === 0, 'no injected script ran', alerts.join(' | '));
  check(consoleErrors.length === 0, 'no console errors while rendering', consoleErrors.join(' | '));
  __out('HARNESS_RESULT passed=' + __passed + ' failed=' + __failed);
  return undefined;
}).catch(function (e) {
  check(false, 'harness chain', e && e.stack ? e.stack : e);
  __out('HARNESS_RESULT passed=' + __passed + ' failed=' + __failed);
});
undefined;
