/* Host harness for test_espnow_page_render.py. tools/webui/js_engine.run_js
 * prepends tools/webui/harness/_bootstrap.js, which provides __out, slurp and
 * __argv on every engine (node, deno, jsc, ...).
 *
 *   __argv[0]  the shipping web_assets/espnow-core.js
 *   __argv[1]  the WebPage_ESPNow.h <script> chunk that defines refreshGraphView
 *   __argv[2]  the chunk that defines espnowApplyStatus / refreshStatus
 *   __argv[3]  the chunk that defines refreshMeshRole
 *   __argv[4]  the chunk that defines loadLocalDeviceMetadata / refreshChannel
 *   __argv[5]  the chunk that defines setupButtonHandlers
 *
 * All scripts run unmodified against a fake hw/DOM. The checks cover the ESP-NOW
 * device list, the peer metadata panel and the graph view: peer- and
 * registry-supplied names and metadata must reach innerHTML as literal text, and
 * the Pair button must still pass a discovered name with quotes intact.
 * The status checks (S*) cover the page's reading of /api/espnow/status: every
 * decision comes from a parsed json reply, a text reply (the old wording
 * included) decides nothing, a slot that is not json is tolerated, and settings
 * strings reach innerHTML shape-checked and escaped.
 * The button checks (B*) click the Initialize button and the mode toggle that
 * setupButtonHandlers wires: the name setup opens from the json name fields
 * (empty name, or nameValid false), and the toggle reads 'espnowmode json'.
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
  if (!elements[id]) {
    elements[id] = { id: id, innerHTML: '', textContent: '', className: '', style: {}, value: '', dataset: {},
                     checked: false, options: [], shown: null, parentElement: { shown: null },
                     listeners: {}, classes: {}, focused: false,
                     addEventListener: function (evt, fn) { (this.listeners[evt] = this.listeners[evt] || []).push(fn); },
                     focus: function () { this.focused = true; } };
    elements[id].classList = (function (e) {
      return { add: function (c) { e.classes[c] = true; }, remove: function (c) { delete e.classes[c]; },
               contains: function (c) { return e.classes[c] === true; } };
    })(elements[id]);
  }
  return elements[id];
}
// Runs the handlers a shipping script registered for `evt` on element `id`.
function fire(id, evt) {
  var target = el(id);
  (target.listeners[evt] || []).forEach(function (fn) { fn.call(target, { type: evt }); });
  return (target.listeners[evt] || []).length;
}
var alerts = [];
var h1 = { textContent: '' };
var cliReplies = {};
var cliRejects = {};
var cliCommands = [];
var jsonReplies = {};
var jsonRejects = {};
var fetchedUrls = [];
var hw = {
  $: el,
  qs: function (sel) { return sel === 'h1' ? h1 : null; },
  show: function (e) { if (e) e.shown = true; },
  hide: function (e) { if (e) e.shown = false; },
  toggle: function (e, on) { if (e) e.shown = !!on; },
  setText: function (x, t) { var e = (typeof x === 'string') ? el(x) : x; if (e) e.textContent = t; },
  setHTML: function (x, t) { var e = (typeof x === 'string') ? el(x) : x; if (e) e.innerHTML = t; },
  fetchJSON: function (url) {
    fetchedUrls.push(url);
    if (jsonRejects[url]) return Promise.reject(new Error(jsonRejects[url]));
    return Promise.resolve(jsonReplies[url]);
  },
  postFormText: function (url, form) {
    var cmd = form && form.cmd;
    cliCommands.push(cmd);
    if (cliRejects[cmd]) return Promise.reject(new Error(cliRejects[cmd]));
    return Promise.resolve(cliReplies[cmd] || '');
  },
  // The raw response form (the page reads the body of a refused command from it).
  postForm: function (url, form) {
    var cmd = form && form.cmd;
    cliCommands.push(cmd);
    return Promise.resolve({ text: function () { return Promise.resolve(cliReplies[cmd] || ''); } });
  },
  on: function (e, evt, fn) { if (e) e.addEventListener(evt, fn); }
};
// The page calls a bare alert(); the scripts run as functions of the global object.
win.alert = function (message) { alerts.push(String(message)); };

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
load(__argv[2], 'status chunk');
load(__argv[3], 'mesh role chunk');
load(__argv[4], 'metadata chunk');
load(__argv[5], 'button handlers chunk');
check(typeof win.renderUnifiedDeviceList === 'function', 'renderUnifiedDeviceList defined');
check(typeof win.loadPeerMetadata === 'function', 'loadPeerMetadata defined');
check(typeof win.refreshGraphView === 'function', 'refreshGraphView defined');
check(typeof win.espnowParseReply === 'function' && typeof win.espnowIsMacText === 'function',
      'espnowParseReply and espnowIsMacText defined');
check(typeof win.espnowApplyStatusSlots === 'function' && typeof win.refreshStatus === 'function' &&
      win.refreshStatusBatch === win.refreshStatus, 'status functions defined (one loader)');
check(typeof win.refreshMeshRole === 'function' && typeof win.loadLocalDeviceMetadata === 'function' &&
      typeof win.refreshChannel === 'function', 'mesh role, metadata and channel functions defined');
check(typeof win.setupButtonHandlers === 'function', 'setupButtonHandlers defined');
// The graph chunk's mesh polling uses timers this harness does not provide.
var meshPolling = null;
win.startMeshStatusPolling = function () { meshPolling = true; };
win.stopMeshStatusPolling = function () { meshPolling = false; };

// One click on Initialize with `infoReply` as the 'espnowdeviceinfo json' answer,
// then enough turns for the start commands and the status refresh to finish.
function clickInit(infoReply) {
  cliCommands.length = 0;
  fetchedUrls.length = 0;
  el('setup-modal').classes = {};
  el('setup-device-name').focused = false;
  cliReplies['espnowdeviceinfo json'] = infoReply;
  fire('btn-espnow-init', 'click');
  return settle(function () { return false; }, 40);
}
// One click on the mode toggle with `modeReply` as the 'espnowmode json' answer.
function clickToggle(modeReply) {
  cliCommands.length = 0;
  fetchedUrls.length = 0;
  alerts.length = 0;
  cliReplies['espnowmode json'] = modeReply;
  fire('btn-espnow-toggle-mode', 'click');
  return settle(function () { return false; }, 40);
}

/* S0: reply parsing. */
check(win.espnowParseReply('{"schema":1,"initialized":true}').initialized === true, 'S0 a json reply parses');
check(win.espnowParseReply('ESP-NOW Status:\n  Initialized: Yes') === null, 'S0 a text reply is no data');
check(win.espnowParseReply('{"schema":1,"ok":false,"error":"buffer"}') === null, 'S0 a json error reply is no data');
check(win.espnowParseReply('[1,2]') === null && win.espnowParseReply('7') === null && win.espnowParseReply(undefined) === null &&
      win.espnowParseReply('') === null, 'S0 arrays, scalars, empty and missing replies are no data');
check(win.espnowIsMacText('02:00:00:00:00:0a') && !win.espnowIsMacText('02:00:00:00:00:0') && !win.espnowIsMacText(':::::::::::::::::') &&
      !win.espnowIsMacText('<img src=x onerror=1>') && !win.espnowIsMacText(null), 'S0 MAC shape check');

/* The replies the firmware sends (text forms as printed by the commands today). */
var STATUS_TEXT = 'ESP-NOW Status:\n  Initialized: Yes\n  Channel: 6\n  MAC Address: 02:00:00:00:00:01\n  Paired Devices: 1';
var SELF_MAC = '02:00:00:00:00:01';
var BOND_MAC = '02:00:00:00:00:0b';
function statusSlots() {
  return [
    JSON.stringify({ schema: 1, initialized: true, channel: 6, channelPref: 11, masterChannel: 0, follow: 'off',
                     masterSilentMs: 0, sweepFailures: 0, nextSweepMs: 0, mac: SELF_MAC, pairedDevices: 1 }),
    JSON.stringify({ schema: 1, enabled: true, mode: 'mesh' }),
    JSON.stringify({ schema: 1, enabled: true, role: 'master', peer: BOND_MAC, peerName: 'partner', online: true }),
    JSON.stringify({ schema: 1, devices: [{ mac: BOND_MAC.toUpperCase(), name: 'partner', encrypted: true, meshId: 0 }], count: 1 }),
    JSON.stringify({ schema: 1, meshes: [] }),
    JSON.stringify({ schema: 1, name: 'node1', nameValid: true, deviceNameSet: true, friendlyName: 'Küche "A"', room: 'Living-Room 2', zone: '',
                     tags: '', stationary: true, meshRole: 'master', mac: SELF_MAC }),
    JSON.stringify({ schema: 1, role: 'backup', masterMac: '02:00:00:00:00:0c', backupEnabled: true, backupMac: '' }),
    JSON.stringify({ peers: [], unpaired: [] }),
    STATUS_TEXT
  ];
}
function resetStatusDom() {
  ['espnow-status-indicator', 'espnow-status-data', 'btn-espnow-init', 'btn-espnow-disable', 'btn-espnow-toggle-mode',
   'en-not-init', 'en-panels', 'mesh-views-card', 'mesh-role-card', 'mesh-role-status', 'channel-status', 'channel-select',
   'friendly-name', 'room-name', 'zone-name', 'tags-input', 'stationary-checkbox', 'master-mac', 'backup-mac',
   'backup-master-enabled', 'backup-mac-group'].forEach(function (id) { delete elements[id]; });
  win.__bondedPeerMac = null;
  win.espnowIsMesh = false;
  meshPolling = null;
}

/* S1: every slot json, as the endpoint sends it. */
resetStatusDom();
el('friendly-name').value = 'stale'; el('zone-name').value = 'stale'; el('tags-input').value = 'stale';
var s1 = null;
try { s1 = win.espnowApplyStatusSlots(statusSlots()); } catch (e) { check(false, 'S1 espnowApplyStatusSlots runs', e); }
check(s1 && s1.initialized === true && s1.mesh === true, 'S1 initialized and mesh come from the json fields', JSON.stringify(s1));
check(el('espnow-status-indicator').className === 'status-indicator status-enabled' && el('en-panels').style.display === 'block' &&
      el('en-not-init').style.display === 'none' && el('btn-espnow-init').style.display === 'none', 'S1 the page shows the initialized state');
check(el('espnow-status-data').textContent === STATUS_TEXT && el('espnow-status-data').style.display === 'block',
      'S1 the status box shows the text slot unchanged');
check(el('btn-espnow-toggle-mode').textContent === 'Mode: Mesh' && win.espnowIsMesh === true && meshPolling === true &&
      el('mesh-views-card').shown === true && el('mesh-role-card').shown === true, 'S1 mesh mode shows the mesh cards and starts polling');
check(win.__bondedPeerMac === BOND_MAC.toUpperCase(), 'S1 the bond partner comes from bondstatus json "peer"', win.__bondedPeerMac);
check(el('device-list').innerHTML.indexOf('partner') >= 0, 'S1 the device list renders from the espnowlist slot');
check(el('friendly-name').value === 'Küche "A"' && el('room-name').value === 'Living-Room 2' && el('zone-name').value === '' &&
      el('tags-input').value === '' && el('stationary-checkbox').checked === true, 'S1 metadata inputs come from espnowdeviceinfo json (unset = empty)');
check(el('channel-select').value === '11' && el('channel-status').innerHTML.indexOf('<strong>Preference:</strong> Channel 11<br>') >= 0 &&
      el('channel-status').innerHTML.indexOf('<strong>Radio now:</strong> channel 6<br>') >= 0, 'S1 the channel card is filled on load from espnowstatus json',
      el('channel-status').innerHTML);
var roleHtml = el('mesh-role-status').innerHTML;
check(roleHtml === '<strong>Current Role:</strong> Backup<br><strong>Master MAC:</strong> 02:00:00:00:00:0c<br><strong>Backup MAC:</strong> Not set',
      'S1 the mesh role card renders from espnowmeshrole json', roleHtml);
check(el('backup-master-enabled').checked === true && el('master-mac').value === '02:00:00:00:00:0c', 'S1 mesh role inputs follow the json');

/* S2: the old text replies decide nothing (no wording is matched). */
resetStatusDom();
el('friendly-name').value = 'kept';
var textSlots = statusSlots();
textSlots[0] = STATUS_TEXT;
textSlots[1] = 'ESP-NOW mode: mesh';
textSlots[2] = 'Bond Mode: ENABLED\nPeer MAC: ' + BOND_MAC;
textSlots[5] = '=== Device Metadata ===\nFriendly Name: Text Name\nRoom:          R\nStationary:    true';
textSlots[6] = 'Mesh role: master\nMaster MAC: 02:00:00:00:00:0c\nBackup enabled: yes';
var s2 = null;
try { s2 = win.espnowApplyStatusSlots(textSlots); } catch (e) { check(false, 'S2 text slots do not throw', e); }
check(s2 && s2.initialized === false && s2.mesh === false, 'S2 text "Initialized: Yes" and "mode: mesh" are not matched', JSON.stringify(s2));
check(el('espnow-status-indicator').className === 'status-indicator status-disabled' && el('en-panels').style.display === 'none' &&
      el('espnow-status-data').style.display === 'none' && meshPolling === false, 'S2 the page shows the not-initialized state');
win.listDevices(textSlots[2], textSlots[3]);
check(win.__bondedPeerMac === null, 'S2 text "Peer MAC:" is not matched');
win.loadLocalDeviceMetadata(textSlots[5]);
check(el('friendly-name').value === 'kept' && el('stationary-checkbox').checked === false, 'S2 a text metadata reply leaves the inputs alone');
win.refreshMeshRole(textSlots[6]);
check(el('mesh-role-status').innerHTML.indexOf('Unknown') >= 0 && el('mesh-role-status').innerHTML.indexOf('Master MAC:</strong> Not set') >= 0,
      'S2 a text mesh role reply shows unknown / Not set', el('mesh-role-status').innerHTML);

/* S3: slots that are not json (a build without the command, a refusal, a json error). */
resetStatusDom();
var mixed = statusSlots();
mixed[1] = 'Error: admin required';
mixed[2] = 'Unknown command: bondstatus';
mixed[5] = '{"schema":1,"ok":false,"error":"buffer"}';
mixed[6] = undefined;
var s3 = null;
var errorsBeforeS3 = consoleErrors.length;
el('room-name').value = 'kept';
try { s3 = win.espnowApplyStatusSlots(mixed.slice(0, 6).concat(['Error: Out of memory', mixed[7], mixed[8]])); }
catch (e) { check(false, 'S3 non-json slots do not throw', e); }
check(s3 && s3.initialized === true && s3.mesh === false && el('btn-espnow-toggle-mode').textContent === 'Mode: Direct',
      'S3 a refused mode reply leaves the mode unknown (Direct)', JSON.stringify(s3));
check(win.__bondedPeerMac === null && el('room-name').value === 'kept' && el('device-list').innerHTML.indexOf('partner') >= 0,
      'S3 the other slots still apply');
check(consoleErrors.length === errorsBeforeS3, 'S3 no console errors from non-json slots', consoleErrors.slice(errorsBeforeS3).join(' | '));
resetStatusDom();
var s3b = win.espnowApplyStatusSlots(['{"schema":1,"ok":false,"error":"response too large"}', mixed[1]]);
check(s3b.initialized === false && el('espnow-status-data').textContent === '', 'S3 a json error status reply is the not-initialized state');

/* S4: not initialized; the status text stays visible only with a start error. */
resetStatusDom();
win.espnowApplyStatusSlots([JSON.stringify({ schema: 1, initialized: false, channel: 0, channelPref: 0 }), '{"schema":1,"mode":"direct"}',
                            '', '', '', '', '', '', 'ESP-NOW: Not initialized']);
check(el('espnow-status-data').style.display === 'none' && el('en-not-init').style.display === 'flex' &&
      el('channel-status').innerHTML === '', 'S4 not initialized: status text hidden, panels not filled');
win.espnowApplyStatusSlots([JSON.stringify({ schema: 1, initialized: false, lastInitError: 'no device name' }), '{"schema":1,"mode":"direct"}',
                            '', '', '', '', '', '', 'ESP-NOW: Not initialized\nLast start error: no device name']);
check(el('espnow-status-data').style.display === 'block' && el('espnow-status-data').textContent.indexOf('no device name') >= 0,
      'S4 a start error keeps the status text visible (from lastInitError)');

/* S5: channel values are coerced; hostile or missing values never reach innerHTML. */
resetStatusDom();
win.refreshChannel({ channelPref: 0 });
check(el('channel-select').value === 'auto' && el('channel-status').innerHTML.indexOf('Auto (follow Wi-Fi)<br>') >= 0 &&
      el('channel-status').innerHTML.indexOf('channel ?<br>') >= 0, 'S5 preference 0 is auto and a missing channel is "?"', el('channel-status').innerHTML);
win.refreshChannel({ channelPref: '<img src=x onerror=alert(5)>', channel: '<svg onload=alert(6)>' });
check(el('channel-status').innerHTML.indexOf('<img') < 0 && el('channel-status').innerHTML.indexOf('<svg') < 0 &&
      el('channel-select').value === 'auto' && el('channel-status').innerHTML.indexOf('channel ?<br>') >= 0, 'S5 hostile channel values are dropped');
win.refreshChannel(null);
check(el('channel-select').value === 'auto' && el('channel-status').innerHTML.indexOf('channel ?<br>') >= 0, 'S5 no status object shows auto / "?"');
check(el('channel-select').innerHTML.indexOf('<option value="13">Channel 13</option>') >= 0, 'S5 the channel select is filled once');

/* S6: settings strings in the mesh role reply are shape-checked and escaped. */
resetStatusDom();
win.refreshMeshRole(JSON.stringify({ schema: 1, role: '<img src=x onerror=alert(7)>', masterMac: '"><svg onload=alert(8)>',
                                     backupEnabled: true, backupMac: '<b>02:00:00:00:00:0d</b>' }));
var hostileRole = el('mesh-role-status').innerHTML;
check(hostileRole.indexOf('<img') < 0 && hostileRole.indexOf('<svg') < 0 && hostileRole.indexOf('<b>') < 0 &&
      hostileRole === '<strong>Current Role:</strong> Unknown<br><strong>Master MAC:</strong> Not set<br><strong>Backup MAC:</strong> Not set',
      'S6 hostile role and MAC strings never reach innerHTML', hostileRole);
check(el('master-mac').value === '' && el('backup-mac').value === '', 'S6 hostile MACs are not copied into the inputs');

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
  tags: 't1,<img src=x>', stationary: true, source: 'mesh',
  pendingRoom: '<svg onload=alert(4)>', pendingTags: '<img src=x>'
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
// The graph reads the json form. The text form is present too, with wording that
// an earlier stub pretended the firmware printed; nothing may match on it.
cliReplies.espnowstatus = 'MAC: 02:00:00:00:00:0f';
cliReplies['espnowstatus json'] = JSON.stringify({ schema: 1, initialized: true, mac: SELF_MAC });
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
  /* R5: a room and tags not accepted on this device are marked, as literal text, with the accept button. */
  check(p.indexOf('Not accepted here:') >= 0 && p.indexOf('room &lt;svg onload=alert(4)&gt;, tags &lt;img src=x&gt;') >= 0,
        'R5 pending room and tags are marked as literal text');
  check(p.indexOf('onclick="acceptPeerGroups(\'' + metaMac + '\')"') >= 0 && typeof win.acceptPeerGroups === 'function',
        'R5 the pending mark carries the accept button');

  var c = cleanPanel.innerHTML;
  check(c.indexOf('>Living-Room 2<') >= 0 && c.indexOf('>Küche<') >= 0 && c.indexOf('>stationary,thermal<') >= 0 &&
        c.indexOf('>Upstairs<') >= 0 && c.indexOf('>peerB<') >= 0, 'R4 clean metadata renders unchanged');
  check(c.indexOf('Bonded Mode (Cached)') >= 0 && c.indexOf('>No<') >= 0, 'R4 clean panel keeps its source and stationary lines');
  check(c.indexOf('Not accepted here') < 0 && c.indexOf('acceptPeerGroups') < 0, 'R5 an accepted room and tags carry no mark');

  var g = graph.innerHTML;
  check(g.indexOf('<img') < 0, 'R3 graph peer name is not rendered as an <img> element', g.indexOf('<img') >= 0 ? 'raw <img present' : '');
  check(g.indexOf('&lt;img src=x onerror=alert(3)&gt;') >= 0, 'R3 graph peer name is shown as literal text');
  check(g.indexOf('Host<b>') < 0 && g.indexOf('Host&lt;b&gt;name') >= 0, 'R3 graph self name is shown as literal text');
  check(g.indexOf('Garage (02:00:00:00:00:09)') >= 0, 'R4 clean graph peer renders unchanged');
  check(g.indexOf('Host&lt;b&gt;name (' + SELF_MAC + ')') >= 0 && g.indexOf('02:00:00:00:00:0f') < 0,
        'S7 graph self MAC comes from espnowstatus json, not from the text reply', g.slice(0, 600));
  check(cliCommands.indexOf('espnowstatus json') >= 0 && cliCommands.indexOf('espnowstatus') < 0,
        'S7 the graph asks for the json form only');

  /* S7b: a hostile "mac" is dropped (shape check), the graph falls back to THIS DEVICE. */
  cliReplies['espnowstatus json'] = JSON.stringify({ schema: 1, initialized: true, mac: '<img src=x onerror=alert(9)>' });
  graph.innerHTML = '';
  win.refreshGraphView();
  return settle(function () { return graph.innerHTML !== ''; }, 50);
}).then(function () {
  var g = graph.innerHTML;
  check(g.indexOf('alert(9)') < 0 && g.indexOf('THIS DEVICE') >= 0, 'S7 a hostile mac never reaches the graph', g.slice(0, 600));
  /* S7c: a text reply (not json) gives the same fallback. */
  cliReplies['espnowstatus json'] = 'ESP-NOW Status:\n  MAC Address: 02:00:00:00:00:01';
  graph.innerHTML = '';
  win.refreshGraphView();
  return settle(function () { return graph.innerHTML !== ''; }, 50);
}).then(function () {
  check(graph.innerHTML.indexOf('THIS DEVICE') >= 0 && graph.innerHTML.indexOf('(02:00:00:00:00:01)') < 0,
        'S7 a text status reply gives no self MAC');

  /* S8: refreshStatus (page load and the Refresh button) reads /api/espnow/status. */
  resetStatusDom();
  cliCommands.length = 0;
  fetchedUrls.length = 0;
  jsonReplies['/api/espnow/status'] = { success: true, results: statusSlots() };
  return win.refreshStatus();
}).then(function () {
  check(fetchedUrls.length === 1 && fetchedUrls[0] === '/api/espnow/status' && cliCommands.length === 0,
        'S8 one endpoint request, no /api/cli request', fetchedUrls.join(',') + ' | ' + cliCommands.join(','));
  check(el('espnow-status-indicator').className === 'status-indicator status-enabled' && win.__bondedPeerMac === BOND_MAC.toUpperCase() &&
        el('channel-select').value === '11', 'S8 the endpoint reply is applied');

  /* S9: endpoint unavailable -> the same json forms over /api/cli; a refused
     admin command (espnowmode) does not fail the refresh. */
  resetStatusDom();
  cliCommands.length = 0;
  jsonRejects['/api/espnow/status'] = 'HTTP 500';
  var slots = statusSlots();
  cliReplies['espnowstatus json'] = slots[0];
  cliReplies.espnowstatus = STATUS_TEXT;
  cliRejects['espnowmode json'] = 'HTTP 400';
  cliReplies['bondstatus json'] = slots[2];
  cliReplies.espnowlist = slots[3];
  cliReplies['espnowdeviceinfo json'] = slots[5];
  return win.refreshStatus().then(function () {
    return settle(function () { return win.__bondedPeerMac !== null && el('room-name').value !== ''; }, 50);
  });
}).then(function () {
  check(el('espnow-status-indicator').className === 'status-indicator status-enabled' && el('espnow-status-data').textContent === STATUS_TEXT,
        'S9 the fallback applies espnowstatus json and shows the text', el('espnow-status-data').textContent);
  check(el('btn-espnow-toggle-mode').textContent === 'Mode: Direct', 'S9 a refused espnowmode leaves Direct, the refresh goes on');
  check(win.__bondedPeerMac === BOND_MAC.toUpperCase() && el('room-name').value === 'Living-Room 2' && el('channel-select').value === '11',
        'S9 the single refreshes use the json forms');
  var textForms = ['espnowmode', 'bondstatus', 'espnowdeviceinfo', 'espnowmeshrole', 'espnowchannel', 'devicename'];
  var askedText = cliCommands.filter(function (c) { return textForms.indexOf(c) >= 0; });
  check(askedText.length === 0, 'S9 no text-form status command is requested', askedText.join(','));

  check(consoleErrors.length === 0, 'no console errors while rendering', consoleErrors.join(' | '));

  /* S10: a reply that cannot be applied (here: /assets/espnow-core.js did not
     load, so there is no parser) is reported in the status box; the refresh
     neither rejects nor leaves the page on "Loading...". */
  resetStatusDom();
  delete jsonRejects['/api/espnow/status'];
  jsonReplies['/api/espnow/status'] = { success: true, results: statusSlots() };
  el('espnow-status-data').textContent = 'Loading...';
  var savedParse = win.espnowParseReply;
  win.espnowParseReply = undefined;
  var s10 = { rejected: null };
  return win.refreshStatus().then(null, function (e) { s10.rejected = e; }).then(function () {
    win.espnowParseReply = savedParse;
    check(s10.rejected === null && el('espnow-status-data').textContent.indexOf('Error: ') === 0 &&
          el('espnow-status-data').style.display === 'block', 'S10 a status reply that cannot be applied shows an error',
          String(s10.rejected) + ' | ' + el('espnow-status-data').textContent);
    check(consoleErrors.length === 1, 'S10 the apply error is logged once', consoleErrors.join(' | '));
    consoleErrors.length = 0;
  });
}).then(function () {
  /* B: the Initialize button and the mode toggle, as setupButtonHandlers wires them. */
  resetStatusDom();
  delete cliRejects['espnowmode json'];
  var setupError = null;
  try { win.setupButtonHandlers(); } catch (e) { setupError = e; }
  check(setupError === null && (el('btn-espnow-init').listeners.click || []).length === 1 &&
        (el('btn-espnow-toggle-mode').listeners.click || []).length === 1, 'B0 setupButtonHandlers wires the buttons', setupError);
  cliReplies.openespnow = 'ESP-NOW initialized';
  // The empty name alone decides (the key that calls it invalid is checked next).
  return clickInit(JSON.stringify({ schema: 1, name: '' }));
}).then(function () {
  check(el('setup-modal').classList.contains('show') && el('setup-device-name').focused === true &&
        cliCommands.join(',') === 'espnowdeviceinfo json', 'B1 an empty name opens the name setup and does not start ESP-NOW',
        cliCommands.join(','));
  return clickInit(JSON.stringify({ schema: 1, name: 'bad name!', nameValid: false }));
}).then(function () {
  check(el('setup-modal').classList.contains('show') && cliCommands.join(',') === 'espnowdeviceinfo json',
        'B2 a saved name that is not valid (nameValid false) opens the name setup', cliCommands.join(','));
  return clickInit(JSON.stringify({ schema: 1, name: 'node1', nameValid: true }));
}).then(function () {
  check(!el('setup-modal').classList.contains('show') &&
        cliCommands.join(',') === 'espnowdeviceinfo json,openespnow' &&
        fetchedUrls.join(',') === '/api/espnow/status', 'B3 a valid name starts ESP-NOW and refreshes the status',
        cliCommands.join(',') + ' | ' + fetchedUrls.join(','));
  // A reply without the key (nothing says the name is invalid) starts as well.
  return clickInit(JSON.stringify({ schema: 1, name: 'node1' }));
}).then(function () {
  check(!el('setup-modal').classList.contains('show') && cliCommands.indexOf('openespnow') === 1,
        'B3 a name without nameValid starts ESP-NOW', cliCommands.join(','));
  // The old 'devicename' text wording decides nothing: start, and let its reply say what is wrong.
  return clickInit('Device name: HardwareOne-B2C3 (default)');
}).then(function () {
  check(!el('setup-modal').classList.contains('show') && cliCommands.indexOf('openespnow') === 1,
        'B4 a text deviceinfo reply is not matched', cliCommands.join(','));
  return clickToggle('{"schema":1,"enabled":true,"mode":"mesh"}');
}).then(function () {
  check(cliCommands.join(',') === 'espnowmode json,espnowmode direct' && alerts.length === 0 && fetchedUrls.length === 1, 'B5 the toggle switches mesh to direct from espnowmode json', cliCommands.join(',') + ' | ' + alerts.join(','));
  return clickToggle('{"schema":1,"enabled":true,"mode":"direct"}');
}).then(function () {
  check(cliCommands.join(',') === 'espnowmode json,espnowmode mesh' && alerts.length === 0 && fetchedUrls.length === 1,
        'B5 the toggle switches direct to mesh', cliCommands.join(',') + ' | ' + alerts.join(','));
  return clickToggle('ESP-NOW mode: mesh');
}).then(function () {
  check(cliCommands.join(',') === 'espnowmode json' && alerts.join('|') === 'Error: could not read the current mode' &&
        fetchedUrls.length === 0, 'B6 a text mode reply changes nothing and says so', cliCommands.join(',') + ' | ' + alerts.join('|'));
  cliRejects['espnowmode json'] = 'HTTP 400';
  return clickToggle('');
}).then(function () {
  check(cliCommands.join(',') === 'espnowmode json' && alerts.join('|') === 'Error: HTTP 400',
        'B6 a refused espnowmode changes nothing and shows the reason', cliCommands.join(',') + ' | ' + alerts.join('|'));
  /* B7: pairing is encrypted only (ESP-NOW 1.0). The Add Device form has one Pair button, which runs
     espnowpairsecure; there is no unencrypted Pair button and no espnowpair command. */
  check(el('btn-pair-device').listeners.click === undefined && (el('btn-pair-secure').listeners.click || []).length === 1,
        'B7 only the encrypted Pair button is wired');
  var listed = 0;
  win.listDevices = function () { listed++; };
  cliCommands.length = 0;
  alerts.length = 0;
  el('pair-mac').value = '02:00:00:00:00:09';
  el('pair-name').value = 'node9';
  cliReplies['espnowpairsecure 02:00:00:00:00:09 node9'] = 'Encrypted device paired successfully: node9 (02:00:00:00:00:09)';
  fire('btn-pair-secure', 'click');
  return settle(function () { return listed > 0; }, 40).then(function () {
    check(cliCommands.join(',') === 'espnowpairsecure 02:00:00:00:00:09 node9' && listed === 1 &&
          el('pair-mac').value === '' && alerts.length === 1 && alerts[0].indexOf('other device') >= 0,
          'B7 Pair runs espnowpairsecure and says to pair from the other device too', cliCommands.join(',') + ' | ' + alerts.join('|'));
    /* P1: Pair on a discovered device sends an encrypted pair request; the other device accepts. */
    cliCommands.length = 0;
    alerts.length = 0;
    win.hwConfirm = function () { return Promise.resolve(true); };
    cliReplies['espnowpairrequest 02:00:00:00:00:03'] = "Pair request sent to 'node3'.";
    return win.pairUnpairedDevice('02:00:00:00:00:03', 'node3');
  }).then(function () {
    return settle(function () { return alerts.length > 0; }, 40);
  }).then(function () {
    check(cliCommands[0] === 'espnowpairrequest 02:00:00:00:00:03' && cliCommands.every(function (c) { return c.indexOf('espnowpair ') !== 0; }),
          'P1 Pair on a discovered device sends espnowpairrequest', cliCommands.join(','));
  });
}).then(function () {
  check(consoleErrors.length === 0, 'no console errors from the button handlers', consoleErrors.join(' | '));
  __out('HARNESS_RESULT passed=' + __passed + ' failed=' + __failed);
  return undefined;
}).catch(function (e) {
  check(false, 'harness chain completes', e && e.stack ? e.stack : e);
  __out('HARNESS_RESULT passed=' + __passed + ' failed=' + __failed);
});
undefined;
