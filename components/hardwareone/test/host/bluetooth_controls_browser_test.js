'use strict';
// Run the full production IIFE. Only the DOM, HTTP helpers, and clock are fake;
// tests reach production behavior through DOM events and complete real promises.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const page = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
let checks = 0;
function equal(actual, expected, label) {
  checks++;
  assert.deepEqual(actual, expected, label);
}
function matches(actual, expected, label) {
  checks++;
  assert.match(actual, expected, label);
}
const flush = () => new Promise(resolve => setImmediate(resolve));

function fixture(enabled = true) {
  const elements = new Map();
  const html = page.markup[String(enabled)];
  for (const tag of html.matchAll(/<([a-z][\w-]*)\b([^>]*?)>([^<]*)/gi)) {
    const attrs = new Map([...tag[2].matchAll(/([\w-]+)(?:\s*=\s*(?:'([^']*)'|"([^"]*)"|([^\s>]+)))?/g)]
      .map(match => [match[1], match[2] ?? match[3] ?? match[4] ?? '']));
    if (!attrs.has('id')) continue;
    const listeners = new Map();
    const style = Object.fromEntries((attrs.get('style') || '').split(';').filter(Boolean)
      .map(value => value.split(':').map(part => part.trim())));
    let text = tag[3];
    const element = {
      id: attrs.get('id'), className: attrs.get('class') || '', style,
      value: attrs.get('value') || '', disabled: attrs.has('disabled'),
      checked: attrs.has('checked'), indeterminate: false, innerHTML: '',
      get textContent() { return text; },
      set textContent(value) { text = String(value); },
      addEventListener(type, fn) {
        if (!listeners.has(type)) listeners.set(type, []);
        listeners.get(type).push(fn);
      },
      // Programmatic events deliberately bypass disabled, just as dispatchEvent
      // does in browsers, to verify the action's own repeated-request guard.
      dispatch(type) {
        for (const fn of listeners.get(type) || []) fn.call(this, {target: this});
      },
    };
    elements.set(element.id, element);
  }
  let now = 100000;
  let nextTimer = 1;
  const timers = new Map();
  const requests = [];
  const commands = [];
  const domReady = [];
  function request(collection, fields) {
    const entry = {...fields, done: false};
    entry.promise = new Promise((resolve, reject) => {
      entry.resolve = value => { assert(!entry.done); entry.done = true; resolve(value); };
      entry.reject = error => { assert(!entry.done); entry.done = true; reject(error); };
    });
    collection.push(entry);
    return entry.promise;
  }
  const hw = {
    $: id => elements.get(id) || null,
    on: (element, type, fn) => { if (element) element.addEventListener(type, fn); },
    setText: (element, value) => { if (element) element.textContent = value; },
    toggle: (element, visible) => { if (element) element.style.display = visible ? '' : 'none'; },
    hide: element => { if (element) element.style.display = 'none'; },
    fetchJSON(url) {
      equal(url, '/api/ble/status', 'page uses authenticated BLE status endpoint');
      return request(requests, {url});
    },
    postFormText(url, form) {
      equal(url, '/api/cli', 'controls retain authorized CLI endpoint');
      const result = request(commands, {cmd: form.cmd, at: now});
      // Unrelated session read-only commands still use the production queue.
      if (form.cmd === 'g2nav' || form.cmd === 'g2verbose') {
        commands.at(-1).resolve(form.cmd + ': Off');
      }
      return result;
    },
  };
  const window = {__bluetoothG2Enabled: enabled};
  class TestDate extends Date {
    constructor(...args) { super(...(args.length ? args : [now])); }
    static now() { return now; }
  }
  const context = vm.createContext({
    hw, window, console, Date: TestDate,
    document: {addEventListener(type, fn) {
      equal(type, 'DOMContentLoaded', 'page initializes after DOM ready');
      domReady.push(fn);
    }},
    setTimeout(fn, delay) {
      const id = nextTimer++;
      timers.set(id, {fn, at: now + Number(delay)});
      return id;
    },
    clearTimeout(id) { timers.delete(id); },
    clearInterval(id) { timers.delete(id); },
  });
  vm.runInContext(page.script, context, {filename: 'WebPage_Bluetooth.h'});
  const element = id => {
    assert(elements.has(id), 'production markup includes ' + id);
    return elements.get(id);
  };
  return {
    element, requests, commands,
    start() { for (const fn of domReady) fn(); },
    event(id, type = 'click') { element(id).dispatch(type); },
    pendingStatus() { return requests.filter(request => !request.done); },
    pendingCommand(cmd) {
      const entry = commands.find(entry => entry.cmd === cmd && !entry.done);
      assert(entry, 'command was dispatched: ' + cmd);
      return entry;
    },
    countCommand(cmd) { return commands.filter(entry => entry.cmd === cmd).length; },
    async advance(ms) {
      const until = now + ms;
      await flush();
      for (let count = 0; ; count++) {
        assert(count < 1000, 'timer scheduler makes progress');
        const next = [...timers].filter(([, timer]) => timer.at <= until)
          .sort((a, b) => a[1].at - b[1].at)[0];
        if (!next) break;
        const [id, timer] = next;
        timers.delete(id);
        now = timer.at;
        timer.fn();
        await flush();
      }
      now = until;
      await flush();
    },
    async answerAllStatus(value) {
      for (const entry of requests.filter(request => !request.done)) {
        if (value instanceof Error) entry.reject(value); else entry.resolve(value);
      }
      await flush();
    },
  };
}

function status(mode = 'server', g2Reconnect = false, ringReconnect = false) {
  return {
    mode,
    ble: {initialized: true, state: 'advertising', connections: 0,
      maxConnections: 4, totalConnections: 2, commandsReceived: 7,
      clients: [], deviceName: 'Hardware One', txPower: 3, autoStart: false,
      requireAuth: true},
    g2: {autoReconnect: g2Reconnect, connection: {running: true, state: 'connected',
      left: {connected: true, mtu: 244, battery: 75, tx: 4, rx: 8},
      right: {connected: true, mtu: 244, battery: 82, tx: 3, rx: 9}}},
    g2Text: 'G2 connected',
    ring: {autoReconnect: ringReconnect, connected: false, name: '', addr: '',
      mtu: 0, rx: 0, scanFound: false},
  };
}

async function ready(mode = 'server', g2Reconnect = false, ringReconnect = false) {
  const f = fixture();
  f.start();
  await f.answerAllStatus(status(mode, g2Reconnect, ringReconnect));
  await f.advance(400);
  return f;
}

// Test cases below assert visible state and requests, never internal IIFE state.
(async () => {
  // Failed bootstrap cannot claim server mode, enable mode-dependent actions,
  // or interpret a failed bare CLI query as a saved reconnect setting.
  const unknown = fixture();
  unknown.start();
  await unknown.answerAllStatus(new Error('HTTP 503'));
  await unknown.advance(400);
  equal(unknown.element('bt-mode-pill').textContent, '--', 'bootstrap failure keeps mode unknown');
  for (const id of ['btn-bt-mode', 'btn-bt-open', 'btn-bt-open2',
                    'cb-g2-autoreconnect', 'cb-ring-autoreconnect']) {
    equal(unknown.element(id).disabled, true, 'unknown owner state disables ' + id);
  }
  equal(unknown.element('cb-g2-autoreconnect').indeterminate, true,
    'unknown saved setting is visibly unconfirmed');
  equal(unknown.commands.some(entry => entry.cmd.startsWith('bleautoreconnect')), false,
    'bootstrap reads reconnect booleans from status rather than human command text');
  unknown.event('btn-bt-refresh');
  await unknown.answerAllStatus(status('client', true, false));
  equal(unknown.element('bt-mode-pill').textContent, 'G2 Client', 'valid poll recovers unknown mode');
  equal(unknown.element('cb-g2-autoreconnect').checked, true, 'valid poll reads G2 saved setting');
  equal(unknown.element('cb-ring-autoreconnect').checked, false, 'valid poll reads ring saved setting');

  for (const invalid of [null, {ble: status().ble}, {mode: 'invalid', ble: status().ble}]) {
    const f = fixture();
    f.start();
    await f.answerAllStatus(invalid);
    await f.advance(400);
    equal(f.element('bt-mode-pill').textContent, '--', 'incomplete bootstrap does not invent server mode');
    equal(f.element('btn-bt-mode').disabled, true, 'incomplete bootstrap leaves role control unavailable');
  }

  // A resolved HTTP 200 with an Error/refusal is still only diagnostic text.
  // Old requests cannot change the displayed role before or after readback.
  for (const reply of ['Error: mode change refused', 'BLE mode change refused: insufficient memory']) {
    const f = await ready();
    f.event('btn-bt-refresh');
    f.event('btn-bt-refresh');
    const old = f.pendingStatus();
    equal(old.length, 2, 'independent status requests can overlap');
    f.event('btn-bt-mode');
    equal(f.element('btn-bt-mode').disabled, true, 'mode button disabled during command');
    equal(f.element('cb-g2-autoreconnect').disabled, true, 'mode action disables overlapping reconnect edits');
    equal(f.element('bt-mode-pill').textContent, 'Server', 'mode stays confirmed while command is pending');
    f.event('btn-bt-mode');
    await f.advance(1500);
    equal(f.countCommand('blemode client'), 1, 'repeated mode events send one command');
    equal(f.pendingStatus().length, 2, 'background polling waits while command is pending');
    old[0].resolve(status('client', true, true));
    await flush();
    equal(f.element('bt-mode-pill').textContent, 'Server', 'pre-action response cannot change pending role');
    f.pendingCommand('blemode client').resolve(reply);
    await flush();
    const readback = f.pendingStatus().find(entry => entry !== old[1]);
    assert(readback, 'mode command completion triggers a status readback');
    equal(f.element('bt-mode-pill').textContent, 'Server', 'resolved refusal does not flip mode');
    equal(f.element('btn-bt-mode').disabled, true, 'mode remains disabled until readback');
    readback.resolve(status());
    await flush();
    equal(f.element('btn-bt-mode').textContent, 'Mode: Server', 'refused mode label restored from owner');
    equal(f.element('btn-bt-mode').disabled, false, 'confirmed readback enables mode action');
    equal(f.element('bt-action-status').textContent, reply, 'mode refusal displayed literally');
    old[1].resolve(status('client', true, true));
    await flush();
    equal(f.element('bt-mode-pill').textContent, 'Server', 'pre-action response cannot overwrite completed readback');
    f.event('btn-bt-refresh');
    await f.answerAllStatus(status());
    equal(f.element('bt-action-status').textContent, reply, 'routine poll preserves command feedback');
  }

  const success = await ready();
  success.event('btn-bt-mode');
  success.pendingCommand('blemode client').resolve('BLE mode: client');
  await flush();
  equal(success.element('bt-mode-pill').textContent, 'Server', 'success text alone does not confirm target role');
  await success.answerAllStatus(status('client'));
  equal(success.element('bt-mode-pill').textContent, 'G2 Client', 'owner readback confirms successful role change');
  equal(success.element('btn-bt-mode').textContent, 'Mode: G2 Client', 'mode label follows confirmed role');

  // A dropped request can have reached the device. Readback, not transport
  // success or failure, determines what to display and whether controls recover.
  for (const readbackFails of [false, true]) {
    const f = await ready();
    f.event('btn-bt-mode');
    f.pendingCommand('blemode client').reject(new Error('connection reset'));
    await flush();
    await f.answerAllStatus(readbackFails ? new Error('HTTP 503') : status());
    matches(f.element('bt-action-status').textContent, /connection reset/, 'mode network error stays visible');
    equal(f.element('bt-mode-pill').textContent, 'Server', 'failed request preserves confirmed role');
    equal(f.element('btn-bt-mode').disabled, readbackFails,
      'mode only becomes actionable after successful confirmation');
    if (readbackFails) {
      f.event('btn-bt-refresh');
      await f.answerAllStatus(status('client'));
      equal(f.element('bt-mode-pill').textContent, 'G2 Client', 'later poll recovers actual role after transport failure');
      equal(f.element('btn-bt-mode').disabled, false, 'recovered role enables mode action');
    }
  }

  // Later-started status requests win even when responses arrive in reverse.
  const ordering = await ready();
  ordering.event('btn-bt-refresh');
  ordering.event('btn-bt-refresh');
  const [older, newer] = ordering.pendingStatus();
  newer.resolve(status('client', true, false));
  await flush();
  older.resolve(status('server', false, true));
  await flush();
  equal(ordering.element('bt-mode-pill').textContent, 'G2 Client', 'older poll cannot revert confirmed mode');
  equal(ordering.element('cb-g2-autoreconnect').checked, true, 'older poll cannot revert G2 setting');
  equal(ordering.element('cb-ring-autoreconnect').checked, false, 'older poll cannot revert ring setting');
  equal(ordering.pendingStatus().length, 0, 'rendering a changed mode does not start recursive polling');

  // Poll latency may exceed the fast cadence for many successive requests.
  // Starting another poll must not starve all usable responses indefinitely.
  const slow = fixture();
  slow.start();
  slow.pendingStatus()[0].resolve(status()); // Independent config load.
  await flush();
  for (const mode of ['server', 'client', 'server']) {
    if (!slow.pendingStatus().length) await slow.advance(1500);
    const request = slow.pendingStatus()[0];
    await slow.advance(1600);
    request.resolve(status(mode));
    await flush();
    equal(slow.element('bt-mode-pill').textContent, mode === 'server' ? 'Server' : 'G2 Client',
      'slow but current status response eventually confirms owner state');
    equal(slow.element('btn-bt-mode').disabled, false, 'slow responses recover usable controls');
  }

  for (const [id, command, feedback, saved] of [
    ['cb-g2-autoreconnect', 'bleautoreconnect g2-glasses', 'g2-auto-status', false],
    ['cb-ring-autoreconnect', 'bleautoreconnect r1-ring', 'ring-auto-status', true],
  ]) {
    const f = await ready('client', false, true);
    const cb = f.element(id);
    equal(cb.checked, saved, 'reconnect starts at device-confirmed value');
    cb.checked = !saved;
    f.event(id, 'change');
    equal(cb.checked, saved, 'pending reconnect preserves last confirmed checkmark');
    equal(cb.disabled, true, 'reconnect disabled while request is pending');
    f.event(id, 'change');
    await f.advance(100);
    const requestCommand = command + (saved ? ' off' : ' on');
    equal(f.countCommand(requestCommand), 1, 'repeated reconnect events send one command');
    const reply = saved ? 'Error: change refused; auto-reconnect remains enabled'
      : 'Auto-reconnect NOT enabled: change refused';
    f.pendingCommand(requestCommand).resolve(reply);
    await flush();
    equal(cb.checked, saved, 'enabled substring in refusal never changes checkbox');
    await f.answerAllStatus(status('client', false, true));
    equal(cb.checked, saved, 'reconnect refusal leaves owner setting unchanged');
    equal(cb.disabled, false, 'reconnect readback enables another action');
    equal(f.element(feedback).textContent, reply, 'reconnect refusal displayed literally');

    // The same setting survives a command transport error and a failed readback;
    // another owner poll can subsequently confirm that the setting did change.
    cb.checked = !saved;
    f.event(id, 'change');
    await f.advance(100);
    f.pendingCommand(requestCommand).reject(new Error('network disconnected'));
    await flush();
    await f.answerAllStatus(new Error('HTTP 503'));
    equal(cb.checked, saved, 'failed reconnect request and readback retain confirmed setting');
    equal(cb.disabled, true, 'failed readback leaves reconnect unavailable');
    matches(f.element(feedback).textContent, /network disconnected/, 'reconnect network failure stays visible');
    f.event('btn-bt-refresh');
    await f.answerAllStatus(status('client', true, false));
    equal(cb.checked, !saved, 'later owner status can confirm setting after dropped reply');
    equal(cb.disabled, false, 'later owner confirmation recovers reconnect control');
  }

  const connectedRing = await ready('client', false, true);
  connectedRing.element('cb-ring-autoreconnect').checked = false;
  connectedRing.event('cb-ring-autoreconnect', 'change');
  const ringFailure = 'Error: auto-reconnect setting change refused';
  connectedRing.pendingCommand('bleautoreconnect r1-ring off').resolve(ringFailure);
  await flush();
  const ringUp = status('client', false, true);
  ringUp.ring.connected = true;
  await connectedRing.answerAllStatus(ringUp);
  connectedRing.event('btn-bt-refresh');
  await connectedRing.answerAllStatus(ringUp);
  equal(connectedRing.element('ring-auto-status').textContent, ringFailure,
    'connected ring poll preserves reconnect failure feedback');

  const noG2 = fixture(false);
  noG2.start();
  await noG2.answerAllStatus(status());
  equal(noG2.element('bt-mode-pill').textContent, 'Server', 'server-only build still reads owner mode');
  equal(noG2.element('bt-state-pill').textContent, 'Advertising', 'server-only page still renders BLE status');
  equal(noG2.commands.length, 0, 'server-only page issues no G2 startup commands');

  // The shared microphone control link appears only when every owning feature
  // is compiled. The Bluetooth page has no separate microphone toggle or command.
  for (const variant of page.featureMarkup) {
    const enabled = variant.g2 && variant.sensors && variant.microphone;
    equal(variant.html.includes("id='g2-mic-controls'"), enabled,
      `microphone link feature guards: ${variant.g2}/${variant.sensors}/${variant.microphone}`);
    equal(variant.html.includes('btn-g2-mic-toggle'), false, 'old microphone toggle removed from markup');
    if (enabled) matches(variant.html, /href=['"]\/sensors#mic-card['"]/, 'mic link opens shared controls');
  }
  equal(/\b(?:g2mic|micrecord|micstop|micState)\b/.test(page.script), false,
    'Bluetooth page does not own microphone commands or speculative recording state');
  console.log(`Bluetooth controls browser: ${checks} checks passed`);
})().catch(error => { console.error(error); process.exitCode = 1; });
