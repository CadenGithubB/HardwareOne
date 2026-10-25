'use strict';
// Exercise the shipping Logging page's System Logging > Configuration pane:
// the page-load handler, the category grid, the saved-options preload and
// Apply Configuration. Only the DOM and HTTP are fake; no page logic is copied.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const sourcePath = process.argv[2] || path.resolve(__dirname, '../../WebPage_Logging.h');
const source = fs.readFileSync(sourcePath, 'utf8');

// Top-level page code: a declaration runs from its first line to the next
// line that is exactly `}` or `};`.
function topLevel(startText) {
  const start = source.indexOf(startText);
  assert(start >= 0 && source.indexOf(startText, start + 1) < 0, `one production ${startText}`);
  const close = /^};?$/m;
  const rest = source.slice(start);
  const end = rest.search(close);
  assert(end > 0, `terminated production ${startText}`);
  return rest.slice(0, end + rest.slice(end).indexOf('\n'));
}
const script = [
  'window.onload = function() {',
  'var gFlagBankByTag = {};',
  'var gSysLogSaved = null;',
  'function escapeHtml(',
  'function populateFlagsPane(',
  'function parseSavedSystemLogFlags(',
  'function applySavedSystemLogConfig(',
  'function applySystemConfig(',
].map(text => text.startsWith('var ') ? text : topLevel(text)).join('\n\n');

let checks = 0;
// Page objects come from the vm's realm, so compare plain JSON copies.
function equal(actual, expected, label) {
  checks++;
  assert.deepEqual(JSON.parse(JSON.stringify(actual)), expected, label);
}
const flush = () => new Promise(resolve => setImmediate(resolve));

function deferred() {
  const request = {};
  request.promise = new Promise((resolve, reject) => {
    request.resolve = resolve;
    request.reject = reject;
  });
  return request;
}

// formatSystemLogFlags' text: 0xW3:W2:W1:W0, each word 16 upper-case hex digits.
function savedFlags(bits) {
  let mask = 0n;
  for (const bit of bits) mask |= 1n << BigInt(bit);
  const words = [3, 2, 1, 0].map(w => ((mask >> BigInt(64 * w)) & ((1n << 64n) - 1n))
    .toString(16).toUpperCase().padStart(16, '0'));
  return '0x' + words.join(':');
}

// /api/debug/flags rows as handleDebugFlags sends them (one per bit-bearing
// flag, then bitless subs, which carry no bit of their own and get no box).
const FLAG_ROWS = [
  {bit: 0, tag: 'AUTH', bank: 'Core', label: 'All Authentication'},
  {bit: 2, tag: 'HTTP', bank: 'Core', label: 'All HTTP'},
  {bit: 33, tag: 'ESPNOW_ROUTER', bank: 'ESP-NOW', label: 'Router'},
  {bit: 130, tag: 'GPS_LIFE', bank: 'GPS', label: 'Lifecycle'},
  {bit: 239, tag: 'RING_X', bank: 'R1 Ring', label: 'Spare'},
  {sub: true, parentBit: 2, tag: 'HTTP', bank: 'Core', label: 'Bitless sub'},
];

function page() {
  const elements = {};
  const element = id => elements[id] || (elements[id] = {id, style: {}, checked: false, textContent: ''});
  element('sys-config-tags').checked = true;  // the HTML default
  let boxes = [];
  const pane = element('sys-flags-pane');
  Object.defineProperty(pane, 'innerHTML', {
    get() { return this._html || ''; },
    set(html) {
      this._html = String(html);
      boxes = [...this._html.matchAll(/<input type="checkbox"[^>]* value="([^"]+)"/g)]
        .map(match => ({value: match[1], checked: false}));
    },
  });
  const requests = {};
  const batches = [];
  const window = {};
  const context = vm.createContext({
    window,
    console: {log() {}, warn() {}, error() {}},
    BigInt,
    document: {  // escapeHtml's text-node round trip
      createElement() {
        let text = '';
        return {
          set textContent(value) { text = String(value); },
          get innerHTML() { return text.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;'); },
        };
      },
    },
    hw: {
      $: element,
      qsa(selector) {
        assert(selector.startsWith('#sys-flags-pane input[type="checkbox"]'), `selector ${selector}`);
        return selector.endsWith(':checked') ? boxes.filter(box => box.checked) : boxes;
      },
      fetchJSON(url) {
        requests[url] = requests[url] || deferred();
        return requests[url].promise;
      },
      postJSON(url, body) {
        const reply = deferred();
        batches.push({url, body, reply});
        return reply.promise;
      },
    },
  });
  for (const name of ['populateLogViewerFileList', 'initBondedLogs', 'generateDefaultFilename',
                      'refreshStatus', 'generateSystemFilename', 'refreshSystemStatus']) {
    context[name] = () => {};
  }
  vm.runInContext(script, context, {filename: 'WebPage_Logging.h'});
  return {context, window, element, boxes: () => boxes, requests, batches};
}

function ticked(p) {
  return p.boxes().filter(box => box.checked).map(box => box.value);
}

const STATUS = (systemLog) => ({success: true, results: ['time', 'sensorlog', 'System logging is INACTIVE'],
                                ...(systemLog ? {systemLog} : {})});

async function load(order, systemLog) {
  const p = page();
  p.window.onload();
  p.requests['/api/settings'].resolve({user: {isAdmin: true}});
  for (const url of order) {
    p.requests[url].resolve(url === '/api/debug/flags' ? FLAG_ROWS : STATUS(systemLog));
    await flush();
  }
  return p;
}

(async () => {
  const saved = {tags: false, flags: savedFlags([2, 130])};

  // The grid and the status land in either order; the boxes and the tags
  // checkbox end up on the saved options. Bitless subs get no box.
  for (const order of [['/api/debug/flags', '/api/logging/status'],
                       ['/api/logging/status', '/api/debug/flags']]) {
    const p = await load(order, saved);
    equal(p.boxes().map(box => box.value), ['0x1', '0x4', '0x200000000', '0x' + (1n << 130n).toString(16),
                                            '0x' + (1n << 239n).toString(16)], 'one box per bit-bearing flag');
    equal(ticked(p), ['0x4', '0x' + (1n << 130n).toString(16)], `saved categories ticked (${order[0]} first)`);
    equal(p.element('sys-config-tags').checked, false, `saved tags applied (${order[0]} first)`);
  }

  // No saved options in the reply (a non-admin, or an older image): the HTML
  // defaults stay.
  {
    const p = await load(['/api/debug/flags', '/api/logging/status'], null);
    equal(ticked(p), [], 'nothing ticked without saved options');
    equal(p.element('sys-config-tags').checked, true, 'tags default kept');
  }

  // Apply saves categories and tags in one settings write.
  {
    const p = await load(['/api/debug/flags', '/api/logging/status'], saved);
    const status = p.element('sys-config-status');
    p.boxes().forEach(box => { box.checked = box.value === '0x1' || box.value === '0x' + (1n << 239n).toString(16); });
    p.element('sys-config-tags').checked = true;
    p.context.applySystemConfig();
    equal(p.batches.length, 1, 'one batch request');
    equal(p.batches[0].url, '/api/cli/batch', 'batch endpoint');
    equal(p.batches[0].body, {commands: ['beginwrite', 'systemlogflags 0x' + ((1n << 239n) | 1n).toString(16),
                                         'logcategorytags 1', 'savesettings']}, 'commands');
    equal(status.textContent, 'Saving...', 'saving shown while the request runs');
    p.batches[0].reply.resolve({ok: true, count: 4, results: ['OK', 'OK: [Settings] Configuration updated',
                                                              'OK: [Settings] Configuration updated', 'OK']});
    await flush();
    assert.match(status.textContent, /^Saved\./);
    checks++;
    equal(status.style.color, 'var(--success)', 'success colour');

    // A refused command (e.g. a non-admin) is shown once, as the device said it.
    p.context.applySystemConfig();
    p.batches[1].reply.resolve({ok: false, results: ['OK', 'Error: Admin access required', '', '']});
    await flush();
    equal(status.textContent, 'Error: Admin access required', 'refusal shown');
    equal(status.style.color, 'var(--danger)', 'refusal colour');

    // A save that did not complete is an error even with no refusal text.
    p.context.applySystemConfig();
    p.batches[2].reply.resolve({ok: false, error: 'settings save failed', results: ['OK', 'OK', 'OK']});
    await flush();
    equal(status.textContent, 'Error: settings save failed', 'unsaved batch shown');

    // A transport failure too.
    p.context.applySystemConfig();
    p.batches[3].reply.reject(new Error('HTTP 500'));
    await flush();
    equal(status.textContent, 'Error: HTTP 500', 'transport failure shown');

    // Nothing ticked: warn and send nothing.
    p.boxes().forEach(box => { box.checked = false; });
    p.context.applySystemConfig();
    equal(p.batches.length, 4, 'no request without categories');
    assert.match(status.textContent, /^Warning: No debug categories selected/);
    checks++;
  }

  console.log(`logging system config browser: ${checks} checks passed`);
})().catch(error => {
  console.error(error);
  process.exit(1);
});
