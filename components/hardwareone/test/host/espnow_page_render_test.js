/* Host harness for test_espnow_page_render.py. tools/webui/js_engine.run_js
 * prepends tools/webui/harness/_bootstrap.js, which provides __out, slurp and
 * __argv on every engine (node, deno, jsc, ...).
 *
 *   __argv[0]  the shipping web_assets/espnow-core.js
 *   __argv[1]  the WebPage_ESPNow.h <script> chunk that defines refreshGraphView
 *
 * Both scripts run unmodified against a fake hw/DOM. The checks cover the ESP-NOW
 * device list, the peer metadata panel and the graph view: peer- and
 * registry-supplied names and metadata must reach innerHTML as literal text, and
 * the Pair button must still pass a discovered name with quotes intact.
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
var consoleErrors = [];
var fakeConsole = {
  log: function () {},
  warn: function () {},
  info: function () {},
  error: function () {
    consoleErrors.push(Array.prototype.slice.call(arguments).map(String).join(' '));
  }
};

var elements = {};
function el(id) {
  if (!elements[id]) elements[id] = { id: id, innerHTML: '', textContent: '', style: {}, value: '', dataset: {} };
  return elements[id];
}
var h1 = { textContent: '' };
var cliReplies = {};
var jsonReplies = {};
var fetchedUrls = [];
var hw = {
  $: el,
  qs: function (sel) { return sel === 'h1' ? h1 : null; },
  show: function () {},
  hide: function () {},
  setText: function (x, t) { var e = (typeof x === 'string') ? el(x) : x; if (e) e.textContent = t; },
  fetchJSON: function (url) {
    fetchedUrls.push(url);
    return Promise.resolve(jsonReplies[url]);
  },
  postFormText: function (url, form) {
    return Promise.resolve(cliReplies[form && form.cmd] || '');
  }
};

// Runs a shipping script with the page globals it expects. Bare identifiers the
// script does not receive here (document, setTimeout, ...) are never reached by
// the code paths under test.
function load(path, label) {
  var source = slurp(path);
  var before = consoleErrors.length;
  try {
    new Function('window', 'hw', 'console', source)(win, hw, fakeConsole);
  } catch (e) {
    consoleErrors.push(label + ' threw: ' + e);
  }
  check(consoleErrors.length === before, 'load ' + label + ' without errors', consoleErrors.slice(before).join(' | '));
}

function decodeEntities(s) {
  return s.replace(/&(#39|quot|lt|gt|amp);/g, function (_, name) {
    return { '#39': "'", quot: '"', lt: '<', gt: '>', amp: '&' }[name];
  });
}

function countOf(haystack, needle) {
  return haystack.split(needle).length - 1;
}

// Resolves after `cond()` holds or after `hops` microtask turns.
function settle(cond, hops) {
  return Promise.resolve().then(function () {
    if (cond() || hops <= 0) return undefined;
    return settle(cond, hops - 1);
  });
}

load(__argv[0], 'espnow-core.js');
load(__argv[1], 'graph view chunk');
check(typeof win.renderUnifiedDeviceList === 'function', 'renderUnifiedDeviceList defined');
check(typeof win.loadPeerMetadata === 'function', 'loadPeerMetadata defined');
check(typeof win.refreshGraphView === 'function', 'refreshGraphView defined');

/* R1: the device list (paired, mesh-only and discovered rows). */
win.espnowIsMesh = true;
win.__bondedPeerMac = null;
win.__pairedDevices = [
  { mac: '02:00:00:00:00:01', name: '<img src=x onerror=alert(1)>', encrypted: true, meshId: -1 }
];
win.__meshPeers = [
  { mac: '02:00:00:00:00:02', name: '<svg onload=alert(2)>', alive: true, secondsSinceHeartbeat: 1, heartbeatCount: 3, ackCount: 1 }
];
win.__meshUnpaired = [
  { mac: '02:00:00:00:00:03', name: 'a"b\'c<i>d', rssi: -50, secondsSinceLastSeen: 2 }
];
try {
  win.renderUnifiedDeviceList();
} catch (e) {
  check(false, 'R1 renderUnifiedDeviceList runs', e);
}
var list = el('device-list').innerHTML;
check(list.indexOf('<img') < 0, 'R1 paired device name is not rendered as an <img> element', list.indexOf('<img') >= 0 ? 'raw <img present' : '');
check(list.indexOf('&lt;img src=x onerror=alert(1)&gt;') >= 0, 'R1 paired device name is shown as literal text');
check(list.indexOf('<svg') < 0, 'R1 mesh peer name is not rendered as an <svg> element');
check(list.indexOf('&lt;svg onload=alert(2)&gt;') >= 0, 'R1 mesh peer name is shown as literal text');
check(list.indexOf('<i>') < 0, 'R1 discovered device name is not rendered as an <i> element');
check(countOf(list, '<button') === 4, 'R1 device list keeps its four buttons', countOf(list, '<button'));
var pair = list.match(/onclick="pairUnpairedDevice\(([^"]*)\)"/);
var pairArgs = null;
if (pair) {
  try {
    pairArgs = new Function('return [' + decodeEntities(pair[1]) + '];')();
  } catch (e) {
    pairArgs = 'unparseable: ' + e;
  }
}
check(pair !== null, 'R1 Pair button onclick survives a quote in the discovered name',
      (list.match(/onclick="pairUnpairedDevice[^>]*/) || ['(no Pair onclick)'])[0]);
check(pairArgs !== null && typeof pairArgs === 'object' && pairArgs.length === 2 &&
      pairArgs[0] === '02:00:00:00:00:03' && pairArgs[1] === 'a"b\'c<i>d',
      'R1 Pair button passes the original MAC and name', JSON.stringify(pairArgs));

/* R4a: clean names render byte-identical; an empty name falls back to the MAC. */
win.__pairedDevices = [
  { mac: '02:00:00:00:00:04', name: 'peerB', encrypted: false },
  { mac: '02:00:00:00:00:05', name: '', encrypted: false },
  { mac: '02:00:00:00:00:06', name: 'Küche', encrypted: false }
];
win.__meshPeers = [];
win.__meshUnpaired = [{ mac: '02:00:00:00:00:07', name: 'Node_7', rssi: -70, secondsSinceLastSeen: 1 }];
win.renderUnifiedDeviceList();
var clean = el('device-list').innerHTML;
check(clean.indexOf('<strong>peerB</strong>') >= 0, 'R4 clean paired name unchanged');
check(clean.indexOf('<strong>02:00:00:00:00:05</strong>') >= 0, 'R4 empty name still falls back to the MAC');
check(clean.indexOf('<strong>Küche</strong>') >= 0, 'R4 UTF-8 name unchanged');
check(clean.indexOf('<strong>Node_7</strong>') >= 0, 'R4 clean discovered name unchanged');
var cleanPair = clean.match(/onclick="pairUnpairedDevice\(([^"]*)\)"/);
var cleanArgs = null;
try { cleanArgs = cleanPair && new Function('return [' + decodeEntities(cleanPair[1]) + '];')(); } catch (e) { cleanArgs = null; }
check(cleanArgs && cleanArgs[0] === '02:00:00:00:00:07' && cleanArgs[1] === 'Node_7',
      'R4 Pair button passes a clean discovered name', cleanPair ? cleanPair[1] : '(none)');

/* R2: the peer metadata panel. */
var metaMac = '02:00:00:00:00:02';
jsonReplies['/api/espnow/metadata?mac=' + encodeURIComponent(metaMac)] = {
  found: true, deviceName: 'B<b>', friendlyName: 'F"&', room: '<svg onload=alert(4)>', zone: 'z',
  tags: 't1,<img src=x>', stationary: true, source: 'mesh'
};
var panel = el('metadata-content-' + metaMac);
win.loadPeerMetadata(metaMac);

/* R4b: clean metadata renders unchanged. */
var cleanMac = '02:00:00:00:00:08';
jsonReplies['/api/espnow/metadata?mac=' + encodeURIComponent(cleanMac)] = {
  found: true, deviceName: 'peerB', friendlyName: 'Küche', room: 'Living-Room 2', zone: 'Upstairs',
  tags: 'stationary,thermal', stationary: false, source: 'bond'
};
var cleanPanel = el('metadata-content-' + cleanMac);
win.loadPeerMetadata(cleanMac);

/* R3: the graph view (self name from the page heading, peers from espnowmeshstatus). */
h1.textContent = 'Host<b>name';
cliReplies.espnowstatus = 'MAC: 02:00:00:00:00:01';
cliReplies.espnowmeshstatus = JSON.stringify({ peers: [
  { mac: '02:00:00:00:00:02', name: '<img src=x onerror=alert(3)>', alive: true },
  { mac: '02:00:00:00:00:09', name: 'Garage', alive: false }
] });
var graph = el('mesh-graph-view');
win.refreshGraphView();

function loading(e) { return e.innerHTML === '' || e.innerHTML.indexOf('Loading metadata') >= 0; }
settle(function () {
  return !loading(panel) && !loading(cleanPanel) && graph.innerHTML !== '';
}, 50).then(function () {
  var p = panel.innerHTML;
  check(p.indexOf('<svg') < 0, 'R2 metadata room is not rendered as an <svg> element', p.indexOf('<svg') >= 0 ? 'raw <svg present' : '');
  check(p.indexOf('&lt;svg onload=alert(4)&gt;') >= 0, 'R2 metadata room is shown as literal text');
  check(p.indexOf('<img') < 0, 'R2 metadata tags are not rendered as an <img> element');
  check(p.indexOf('B<b>') < 0 && p.indexOf('B&lt;b&gt;') >= 0, 'R2 metadata device name is shown as literal text');
  check(p.indexOf('F&quot;&amp;') >= 0, 'R2 metadata friendly name is escaped once');
  check(p.indexOf('Mesh/Pairing Mode') >= 0 && p.indexOf('>Yes<') >= 0, 'R2 panel keeps its source and stationary lines');

  var c = cleanPanel.innerHTML;
  check(c.indexOf('>Living-Room 2<') >= 0 && c.indexOf('>Küche<') >= 0 && c.indexOf('>stationary,thermal<') >= 0 &&
        c.indexOf('>Upstairs<') >= 0 && c.indexOf('>peerB<') >= 0, 'R4 clean metadata renders unchanged');
  check(c.indexOf('Bonded Mode (Cached)') >= 0 && c.indexOf('>No<') >= 0, 'R4 clean panel keeps its source and stationary lines');

  var g = graph.innerHTML;
  check(g.indexOf('<img') < 0, 'R3 graph peer name is not rendered as an <img> element', g.indexOf('<img') >= 0 ? 'raw <img present' : '');
  check(g.indexOf('&lt;img src=x onerror=alert(3)&gt;') >= 0, 'R3 graph peer name is shown as literal text');
  check(g.indexOf('Host<b>') < 0 && g.indexOf('Host&lt;b&gt;name') >= 0, 'R3 graph self name is shown as literal text');
  check(g.indexOf('Garage (02:00:00:00:00:09)') >= 0, 'R4 clean graph peer renders unchanged');

  check(consoleErrors.length === 0, 'no console errors while rendering', consoleErrors.join(' | '));
  __out('HARNESS_RESULT passed=' + __passed + ' failed=' + __failed);
  return undefined;
});
undefined;
