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

// `phone` selects the ENABLE_BLE_PHONE_PIPE page (script and markup).
function fixture(enabled = true, phone = false) {
  const elements = new Map();
  const html = (phone ? page.phoneMarkup : page.markup)[String(enabled)];
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
    // A command reply resolves to its text, or to {status, text} for a refusal
    // the device answered with an HTTP error (the page shows that body).
    postForm(url, form) {
      equal(url, '/api/cli', 'controls retain authorized CLI endpoint');
      const result = request(commands, {cmd: form.cmd, at: now});
      // Unrelated session read-only commands still use the production queue.
      if (form.cmd === 'g2nav' || form.cmd === 'g2verbose') {
        commands.at(-1).resolve(form.cmd + ': Off');
      }
      return result.then(reply => {
        const status = typeof reply === 'object' ? reply.status : 200;
        const text = typeof reply === 'object' ? reply.text : reply;
        return {ok: status >= 200 && status < 300, status, text: () => Promise.resolve(text)};
      });
    },
  };
  // The page's confirm dialog: records each question and answers OK (true) or
  // Cancel (null, as the shared dialog does) as `answer` says.
  const prompts = [];
  let confirmAnswer = true;
  const window = {__bluetoothG2Enabled: enabled, hwConfirm(message) {
    prompts.push(message);
    return Promise.resolve(confirmAnswer ? true : null);
  }};
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
  vm.runInContext(phone ? page.phoneScript : page.script, context, {filename: 'WebPage_Bluetooth.h'});
  const element = id => {
    assert(elements.has(id), 'production markup includes ' + id);
    return elements.get(id);
  };
  return {
    element, requests, commands, prompts,
    answer(ok) { confirmAnswer = ok; },
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

// The status response's `link` object (BLE_LinkStatus.h): the role running now,
// the saved mode's role, the relay pipe and the app console.
const ROLE_TEXT = {console: 'App console', glasses: 'Glasses', 'glasses-pipe': 'Glasses+pipe',
  'phone-pipe': 'Phone pipe', off: 'Off'};
const CONSOLE_TEXT = {'not-here': 'not available in this mode', off: 'off',
  waiting: 'waiting for the app', hidden: 'not visible (advertising off)',
  connected: 'app connected', 'signed-in': 'app signed in',
  'waiting-phone': 'waiting for the phone link', 'no-secret': 'set blesecret first'};
// Beside the glasses and the pipe the console rides the pipe's link (waiting
// until it attaches); the phone pipe alone has none. savedDiffers as the device
// sends it: Off always names the saved mode, and the client stack's roles (the
// glasses, with the pipe, the pipe alone) are one saved mode.
const CLIENT_ROLES = ['glasses', 'glasses-pipe', 'phone-pipe'];
function link(role, saved = role, fields = {}) {
  const app = fields.console || (role === 'console' ? 'waiting' : role === 'off' ? 'off'
    : role === 'glasses-pipe' ? 'waiting-phone' : 'not-here');
  const mode = r => CLIENT_ROLES.includes(r) ? 'client' : r;
  return {role, roleText: ROLE_TEXT[role], saved, savedText: ROLE_TEXT[saved],
    savedDiffers: role === 'off' || mode(role) !== mode(saved), console: app,
    consoleText: CONSOLE_TEXT[app], consoleClients: 0, advertising: app === 'waiting', ...fields};
}
const MODE_ROLE = {server: 'console', client: 'glasses'};
// A pipe build names the saved client mode (the client stack) as the glasses with
// the pipe (hw1link::roleOfMode), with or without the glasses in the build.
const PIPE_MODE_ROLE = {server: 'console', client: 'glasses-pipe'};

function status(mode = 'server', g2Reconnect = false, ringReconnect = false) {
  return {
    mode,
    link: link(MODE_ROLE[mode]),
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

// The `phone` object /api/ble/status adds in phone relay pipe builds: `text` is
// the pipe line ("fail: not found"), `pipeText` its one word.
function phone(fields = {}) {
  return {schema: 2, active: true, standalone: false, connected: true, state: 'ready',
    text: 'ready', pipe: 'ready', pipeText: 'ready', role: 'with-glasses', enrolled: true,
    autoReconnect: true, console: 'not-here', consoleText: 'not available in this mode', ...fields};
}
// A pipe that is running and searching for the phone.
const SEARCHING = {connected: false, state: 'scanning', text: 'searching', pipe: 'searching',
  pipeText: 'searching'};
// The `phoneLink` block (phone link v2) the status response adds beside
// `phone` in pipe builds: the device's own words, nothing mapped on the page.
function phoneLink(fields = {}) {
  return {state: 'ready', stateText: 'ready', paired: true, secret: 'code', secretText: 'device code',
    console: 'signed-in', consoleText: 'signed in', relay: 'off', streams: {relay: 0, console: 1},
    ended: 0, endReasons: {}, ...fields};
}
// A stopped pipe.
const PIPE_OFF = {active: false, standalone: false, connected: false, state: 'stopped', text: 'off',
  pipe: 'off', pipeText: 'off', role: 'off', console: 'off', consoleText: 'off'};

// Glasses + phone build, client mode: the pipe runs beside the glasses.
function glassesPipeStatus(fields = {}) {
  const base = status('client');
  base.link = link('glasses-pipe');
  base.phone = phone(fields);
  base.phoneLink = phoneLink();
  return base;
}

// p4-phone-only: no glasses in the build (no g2/ring keys), the pipe alone on
// the client stack. `saved` is the saved mode: a pipe `openphone` opened under
// a saved server mode runs the same way.
function phoneOnlyStatus(fields = {}, saved = 'client') {
  const base = status(saved);
  delete base.g2; delete base.g2Text; delete base.ring;
  base.ble.initialized = false;
  base.phone = phone({standalone: true, role: 'phone-only', ...fields});
  base.link = link(base.phone.active ? 'phone-pipe' : 'off', PIPE_MODE_ROLE[saved]);
  base.phoneLink = phoneLink();
  // `host` (bleHostStateWord): the stack carries the pipe, or is down with it.
  base.host = base.phone.active ? 'on' : 'off';
  return base;
}

// Glasses + phone build with the glasses closed (closeg2): the same pipe alone,
// and the device still reports the glasses runtime, not running.
function pipeAloneStatus(fields = {}, saved = 'client') {
  const base = phoneOnlyStatus(fields, saved);
  const glasses = status(saved);
  const closed = {...glasses.g2.connection, running: false, state: 'off'};
  closed.left = {...closed.left, connected: false};
  closed.right = {...closed.right, connected: false};
  base.g2 = {...glasses.g2, connection: closed};
  base.g2Text = '';
  base.ring = glasses.ring;
  return base;
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
  equal(unknown.element('bt-mode-pill').textContent, 'Glasses', 'valid poll recovers unknown mode');
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
    equal(f.element('bt-mode-pill').textContent, 'App console', 'mode stays confirmed while command is pending');
    f.event('btn-bt-mode');
    await f.advance(1500);
    equal(f.countCommand('blemode client'), 1, 'repeated mode events send one command');
    equal(f.pendingStatus().length, 2, 'background polling waits while command is pending');
    old[0].resolve(status('client', true, true));
    await flush();
    equal(f.element('bt-mode-pill').textContent, 'App console', 'pre-action response cannot change pending role');
    f.pendingCommand('blemode client').resolve(reply);
    await flush();
    const readback = f.pendingStatus().find(entry => entry !== old[1]);
    assert(readback, 'mode command completion triggers a status readback');
    equal(f.element('bt-mode-pill').textContent, 'App console', 'resolved refusal does not flip mode');
    equal(f.element('btn-bt-mode').disabled, true, 'mode remains disabled until readback');
    readback.resolve(status());
    await flush();
    equal(f.element('btn-bt-mode').textContent, 'Mode: App console', 'refused mode label restored from owner');
    equal(f.element('btn-bt-mode').disabled, false, 'confirmed readback enables mode action');
    equal(f.element('bt-action-status').textContent, reply, 'mode refusal displayed literally');
    old[1].resolve(status('client', true, true));
    await flush();
    equal(f.element('bt-mode-pill').textContent, 'App console', 'pre-action response cannot overwrite completed readback');
    f.event('btn-bt-refresh');
    await f.answerAllStatus(status());
    equal(f.element('bt-action-status').textContent, reply, 'routine poll preserves command feedback');
  }

  const success = await ready();
  success.event('btn-bt-mode');
  success.pendingCommand('blemode client').resolve('[BLE] Mode set to client (Glasses)');
  await flush();
  equal(success.element('bt-mode-pill').textContent, 'App console', 'success text alone does not confirm target role');
  await success.answerAllStatus(status('client'));
  equal(success.element('bt-mode-pill').textContent, 'Glasses', 'owner readback confirms successful role change');
  equal(success.element('btn-bt-mode').textContent, 'Mode: Glasses', 'mode label follows confirmed role');

  // A dropped request can have reached the device. Readback, not transport
  // success or failure, determines what to display and whether controls recover.
  for (const readbackFails of [false, true]) {
    const f = await ready();
    f.event('btn-bt-mode');
    f.pendingCommand('blemode client').reject(new Error('connection reset'));
    await flush();
    await f.answerAllStatus(readbackFails ? new Error('HTTP 503') : status());
    matches(f.element('bt-action-status').textContent, /connection reset/, 'mode network error stays visible');
    equal(f.element('bt-mode-pill').textContent, 'App console', 'failed request preserves confirmed role');
    equal(f.element('btn-bt-mode').disabled, readbackFails,
      'mode only becomes actionable after successful confirmation');
    if (readbackFails) {
      f.event('btn-bt-refresh');
      await f.answerAllStatus(status('client'));
      equal(f.element('bt-mode-pill').textContent, 'Glasses', 'later poll recovers actual role after transport failure');
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
  equal(ordering.element('bt-mode-pill').textContent, 'Glasses', 'older poll cannot revert confirmed mode');
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
    equal(slow.element('bt-mode-pill').textContent, mode === 'server' ? 'App console' : 'Glasses',
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
  equal(noG2.element('bt-mode-pill').textContent, 'App console', 'server-only build still reads owner mode');
  equal(noG2.element('bt-state-pill').textContent, 'Advertising', 'server-only page still renders BLE status');
  equal(noG2.commands.length, 0, 'server-only page issues no G2 startup commands');

  // Builds without the phone pipe ship no phone card and no phone commands.
  for (const g2 of ['false', 'true']) {
    equal(page.markup[g2].includes('bt-phone-panel'), false, 'no phone card without the pipe');
    equal(page.phoneMarkup[g2].includes('bt-phone-panel'), true, 'phone card in pipe builds');
  }
  equal(/'(openphone|closephone|phone stats)'/.test(page.script), false, 'no phone commands without the pipe');
  // The phone card names no placement: the mode pill shows the role.
  for (const g2 of ['false', 'true']) {
    equal(page.phoneMarkup[g2].includes('phone-role'), false, 'phone card has no role field');
  }
  equal(/phone only|with glasses|'stopped'/.test(page.phoneScript), false, 'no placement words in the phone card');
  // The card is the phone link's: its rows, and pairing by a device code made
  // on the device's own screen or serial console, never shown on the page.
  for (const g2 of ['false', 'true']) {
    const html = page.phoneMarkup[g2];
    matches(html, />Phone link</, 'the card is titled Phone link');
    for (const id of ['phone-link-secret', 'phone-link-console', 'phone-link-relay', 'phone-link-streams',
                      'phone-link-ends']) {
      equal(html.includes(`id='${id}'`), true, 'phone link row ' + id);
    }
    matches(html, /New code and pair/, 'pairing names the device screen entry');
    matches(html, /phone pair new/, 'pairing names the serial command');
    equal(/pipe passphrase|phone-link-code/.test(html), false, 'no shared passphrase or code on the page');
    equal(page.markup[g2].includes('phone-link-'), false, 'no phone link rows without the pipe');
  }
  equal(/'phone pair/.test(page.phoneScript), false, 'the page never runs a pairing command');

  // The role switch is built with a second role, one button per role in the build.
  for (const [html, buttons, label] of [
    [page.markup.false, [], 'server-only'],
    [page.markup.true, ['console', 'glasses'], 'glasses'],
    [page.phoneMarkup.false, ['console', 'phone'], 'phone-only'],
    [page.phoneMarkup.true, ['console', 'glasses', 'phone'], 'glasses + pipe'],
  ]) {
    equal(['console', 'glasses', 'phone'].filter(role => html.includes(`id='btn-bt-use-${role}'`)),
      buttons, 'role switch buttons in a ' + label + ' build');
  }
  for (const [id, text] of [['console', 'Use app console'], ['glasses', 'Use glasses'],
                            ['phone', 'Use phone pipe']]) {
    matches(page.phoneMarkup.true, new RegExp(`id='btn-bt-use-${id}'[^>]*>${text}<`), 'switch label: ' + text);
  }

  // Glasses + phone build, client mode: the card follows the device's phone object.
  {
    const f = fixture(true, true);
    f.start();
    await f.answerAllStatus(glassesPipeStatus());
    await f.advance(400);
    equal(f.element('bt-mode-pill').textContent, 'Glasses+pipe', 'role pill names the glasses and the pipe');
    equal(f.element('btn-bt-mode').textContent, 'Mode: Glasses+pipe', 'saved mode in the same words');
    equal(f.element('bt-console-text').textContent, 'waiting for the phone link',
      'the app console waits for the pipe\'s link beside the glasses');
    equal(f.element('bt-console-meta').textContent, '', 'no server clients/advertising on the phone link');
    equal(f.element('bt-phone-panel').style.display, '', 'phone card shown beside the glasses');
    equal(f.element('bt-g2-panels').style.display, '', 'glasses panels still shown');
    equal(f.element('phone-state').textContent, 'ready', 'pipe line from the device');
    equal(f.element('phone-state-summary').textContent, 'ready', 'card summary is the pipe word');
    equal(f.element('phone-paired').textContent, 'yes', 'pairing shown');
    equal(f.element('btn-phone-start').style.display, 'none', 'running pipe offers no Start');
    equal(f.element('btn-phone-stop').style.display, '', 'running pipe offers Stop');
    equal(f.element('cb-phone-autoreconnect').checked, true, 'phone reconnect read from status');
    equal(f.element('cb-phone-autoreconnect').disabled, false, 'confirmed phone reconnect is editable');

    // Reconnect uses the shared confirmed-control path with the phone peer name.
    f.element('cb-phone-autoreconnect').checked = false;
    f.event('cb-phone-autoreconnect', 'change');
    await f.advance(100);
    equal(f.countCommand('bleautoreconnect phone off'), 1, 'phone reconnect toggle sends one command');
    f.pendingCommand('bleautoreconnect phone off').resolve('Auto-reconnect disabled for phone');
    await flush();
    await f.answerAllStatus(glassesPipeStatus({autoReconnect: false, ...SEARCHING}));
    equal(f.element('cb-phone-autoreconnect').checked, false, 'readback confirms the phone setting');
    equal(f.element('phone-state').textContent, 'searching', 'pipe line while it works');
    equal(f.element('phone-state-summary').textContent, 'searching', 'summary for a searching pipe');

    // The phone link rows: the device's words for the console stream, relay
    // forwarding, live streams by class and how sessions ended.
    equal(f.element('phone-link-secret').textContent, 'device code', 'secret kind from the device');
    equal(f.element('phone-link-console').textContent, 'signed in', 'console stream words');
    equal(f.element('phone-link-relay').textContent, 'off', 'relay forwarding as the phone reports it');
    equal(f.element('phone-link-streams').textContent, 'relay 0/2, console 1/1', 'live streams by class');
    equal(f.element('phone-link-ends').textContent, '0', 'no session ended yet');
    f.event('btn-bt-refresh');
    const ended = glassesPipeStatus({autoReconnect: false, ...SEARCHING});
    ended.phoneLink = phoneLink({state: 'searching', stateText: 'searching', secret: 'passphrase',
      secretText: 'passphrase', console: 'needs-code', consoleText: 'needs a device code',
      relay: 'unknown', streams: {relay: 0, console: 0}, ended: 4, lastEnd: 'paused',
      endReasons: {cancelled: 2, 'service-off': 1, paused: 1}});
    await f.answerAllStatus(ended);
    equal(f.element('phone-link-secret').textContent, 'passphrase', 'a passphrase is named as one');
    equal(f.element('phone-link-console').textContent, 'needs a device code', 'why the console is not offered');
    equal(f.element('phone-link-relay').textContent, 'unknown', 'relay unknown until the link is ready');
    equal(f.element('phone-link-streams').textContent, 'relay 0/2, console 0/1', 'no live streams');
    equal(f.element('phone-link-ends').textContent,
      '4 (last: paused) · cancelled 2, service-off 1, paused 1', 'ended sessions with their reasons');
    // A response without the block (an older device) leaves the rows as they were.
    f.event('btn-bt-refresh');
    const older = glassesPipeStatus({autoReconnect: false, ...SEARCHING});
    delete older.phoneLink;
    await f.answerAllStatus(older);
    equal(f.element('phone-link-console').textContent, 'needs a device code', 'no block, no change');

    // The app console on the pipe's link: its words, without the server's
    // clients/advertising meta.
    f.event('btn-bt-refresh');
    const attached = glassesPipeStatus({autoReconnect: false, ...SEARCHING});
    attached.link = link('glasses-pipe', 'glasses-pipe',
                         {console: 'signed-in', consoleText: 'app signed in', consoleClients: 1});
    await f.answerAllStatus(attached);
    equal(f.element('bt-console-text').textContent, 'app signed in', 'phone-link console words');
    equal(f.element('bt-console-meta').textContent, '', 'no server clients/advertising on the phone link');

    // A failure names its reason in the line; the summary keeps the one word.
    f.event('btn-bt-refresh');
    await f.answerAllStatus(glassesPipeStatus({connected: false, state: 'phone not found',
      text: 'fail: not found', pipe: 'fail', pipeText: 'fail', pipeReason: 'not found'}));
    equal(f.element('phone-state').textContent, 'fail: not found', 'failure line with its reason');
    equal(f.element('phone-state-summary').textContent, 'fail', 'failure summary word');
    equal(f.element('phone-state').style.color, 'var(--warning,#c8a000)', 'a reason is a warning');
    equal(f.element('bt-phone-panel').style.display, '', 'a failed running pipe keeps its card');

    // Nothing paired yet.
    f.event('btn-bt-refresh');
    await f.answerAllStatus(glassesPipeStatus({connected: false, state: 'waiting to enroll',
      text: 'not paired', pipe: 'not-paired', pipeText: 'not paired', enrolled: false}));
    equal(f.element('phone-state').textContent, 'not paired', 'unpaired pipe line');
    equal(f.element('phone-state-summary').textContent, 'not paired', 'unpaired summary');
    equal(f.element('phone-paired').textContent, 'no', 'no pairing saved');
    equal(f.element('phone-state').style.color, 'var(--muted)', 'not paired is not a failure');

    for (const [id, cmd] of [['btn-phone-connect', 'openphone'],
                             ['btn-phone-disconnect', 'closephone'],
                             ['btn-phone-stop', 'closephone'],
                             ['btn-phone-start', 'openphone']]) {
      f.event(id);
      await f.advance(100);
      f.pendingCommand(cmd).resolve('OK: ' + cmd);
      await flush();
      equal(f.element('phone-conn-status').textContent, 'OK: ' + cmd, id + ' reply shown');
    }
    f.event('btn-phone-stats');
    await f.advance(100);
    f.pendingCommand('phone stats').resolve({status: 403, text: 'Error: admin required'});
    await flush();
    equal(f.element('phone-stats').textContent, 'Error: admin required', 'refused stats keep the device reply');

    // Server mode with the pipe stopped: no phone card; the console waits for the app.
    f.event('btn-bt-refresh');
    const server = status('server');
    server.phone = phone({...PIPE_OFF, console: 'waiting', consoleText: 'waiting for the app'});
    await f.answerAllStatus(server);
    equal(f.element('bt-mode-pill').textContent, 'App console', 'console role read');
    equal(f.element('btn-bt-mode').textContent, 'Mode: App console', 'saved console mode');
    equal(f.element('bt-console-text').textContent, 'waiting for the app', 'console waits for the app');
    equal(f.element('bt-console-meta').textContent, ' · clients 0/4 · advertising on',
      'console clients and advertising');
    equal(f.element('bt-phone-panel').style.display, 'none', 'no phone card in server mode');

    // An app connects and signs in.
    f.event('btn-bt-refresh');
    const signedIn = status('server');
    signedIn.ble.connections = 1;
    signedIn.link = link('console', 'console', {console: 'signed-in', consoleClients: 1});
    signedIn.phone = server.phone;
    await f.answerAllStatus(signedIn);
    equal(f.element('bt-console-text').textContent, 'app signed in', 'console signed in');
    equal(f.element('bt-console-meta').textContent, ' · clients 1/4 · advertising off',
      'one app client; advertising stopped on connect');
    equal(f.element('bt-state-pill').textContent, 'Connected', 'server state follows the client');

    // A pipe still closing as the console comes up keeps its card.
    f.event('btn-bt-refresh');
    const closing = status('server');
    closing.phone = phone({connected: false, state: 'phone close requested', text: 'closing',
      pipe: 'closing', pipeText: 'closing'});
    await f.answerAllStatus(closing);
    equal(f.element('bt-panels').style.display, '', 'console panels while the console runs');
    equal(f.element('bt-phone-panel').style.display, '', 'a running pipe keeps its card in the console view');
    equal(f.element('phone-state').textContent, 'closing', 'closing pipe line');
  }

  // A pipe `openphone` opened under a saved server mode, the glasses closed: the
  // view is the client stack's, the pill names the saved mode, and the card stays
  // shown. The client view's header follows the device's `host`: the stack is
  // up with the pipe, so it reads Running with Disable, no start card and no
  // glasses panels.
  {
    const f = fixture(true, true);
    f.start();
    await f.answerAllStatus(pipeAloneStatus({}, 'server'));
    await f.advance(400);
    equal(f.element('bt-mode-pill').textContent, 'Phone pipe (saved: App console)',
      'running pipe with the saved mode it differs from');
    equal(f.element('btn-bt-mode').textContent, 'Mode: App console', 'mode button names the saved mode');
    equal(f.element('bt-phone-panel').style.display, '', 'phone card shown while the pipe runs alone');
    equal(f.element('bt-panels').style.display, 'none', 'no console panels: the console is not up');
    equal(f.element('bt-g2-panels').style.display, 'none', 'no glasses panels: the glasses are closed');
    equal(f.element('bt-not-init').style.display, 'none', 'no start card: the client stack is up');
    equal(f.element('bt-state-pill').textContent, 'Running', 'the client view\'s pill follows the host');
    equal(f.element('btn-bt-close').style.display, '', 'header Disable while the stack is up');
    equal(f.element('bt-console-text').textContent, 'not available in this mode', 'console n/a while the pipe runs');
    equal(f.element('phone-state').textContent, 'ready', 'pipe ready');

    // The mode button switches the saved mode, not the view it shows.
    f.event('btn-bt-mode');
    await f.advance(100);
    equal(f.countCommand('blemode client'), 1, 'saved server mode switches to the client stack');
    f.pendingCommand('blemode client').resolve('Error: mode change refused');
    await flush();
    await f.answerAllStatus(pipeAloneStatus({}, 'server'));
    equal(f.element('btn-bt-mode').textContent, 'Mode: App console', 'refused switch keeps the saved mode');

    // Header Disable in the client view is the whole radio (the pipe with it),
    // never the console that does not run.
    f.event('btn-bt-close');
    await f.advance(100);
    equal(f.countCommand('closeble'), 1, 'header Disable turns Bluetooth off');
    equal(f.countCommand('closeconsole'), 0, 'header Disable does not close the absent console');
    f.pendingCommand('closeble').resolve('Bluetooth off (devices closed, host stopped, workers released)');
    await f.advance(400);
    await f.answerAllStatus(pipeAloneStatus(PIPE_OFF, 'server'));
    equal(f.element('bt-mode-pill').textContent, 'Off (saved: App console)', 'nothing runs: the saved mode');
    equal(f.element('bt-not-init-title').textContent, 'Bluetooth is not running', 'saved server start card');
    equal(f.element('btn-bt-open2').textContent, 'Enable Bluetooth', 'start card opens the console');
    equal(f.element('bt-phone-panel').style.display, 'none', 'no card once the pipe stops');
    equal(f.element('bt-console-text').textContent, 'off', 'console off');
  }

  // Phone-only build (p4-phone-only): the pipe alone is the client view with the
  // phone card; no glasses panels because the glasses are not in the build. The
  // client view's header follows `host` here too: Running while the pipe runs,
  // the client start card once the stack is down.
  {
    const f = fixture(false, true);
    f.start();
    await f.answerAllStatus(phoneOnlyStatus(SEARCHING));
    await f.advance(400);
    equal(f.element('bt-mode-pill').textContent, 'Phone pipe', 'phone pipe role is shown, not ignored');
    equal(f.element('bt-state-pill').textContent, 'Running', 'the client view\'s pill follows the host');
    equal(f.element('bt-panels').style.display, 'none', 'server panels hidden');
    equal(f.element('bt-not-init').style.display, 'none', 'no start card while the stack carries the pipe');
    equal(f.element('bt-phone-panel').style.display, '', 'phone card shown');
    equal(f.element('bt-console-text').textContent, 'not available in this mode', 'console n/a in the phone role');
    f.event('btn-bt-close');
    await f.advance(100);
    equal(f.countCommand('closeble'), 1, 'header Disable turns Bluetooth off, the pipe with it');
    f.pendingCommand('closeble').resolve('Bluetooth off (devices closed, host stopped, workers released)');
    await f.advance(400);  // runAndRefresh reads the device back after the reply
    await f.answerAllStatus(phoneOnlyStatus(PIPE_OFF));
    equal(f.element('bt-mode-pill').textContent, 'Off (saved: Glasses+pipe)', 'stopped pipe names the saved mode');
    equal(f.element('bt-state-pill').textContent, 'Off', 'nothing runs');
    equal(f.element('bt-not-init').style.display, '', 'stopped pipe shows the start card');
    equal(f.element('bt-not-init-title').textContent, 'Bluetooth client stack is off', 'the client start card\'s copy');
    equal(f.element('btn-bt-open2').textContent, 'Open Bluetooth', 'the client start card\'s button');
    equal(f.element('bt-phone-panel').style.display, 'none', 'no card while the pipe is stopped');
    f.event('btn-bt-open2');
    await f.advance(100);
    equal(f.countCommand('openble'), 1, 'start card brings the client stack up');
  }

  // Glasses + phone build: the pipe alone (glasses closed) is the same client
  // view; the glasses panels come back with the glasses.
  {
    const f = fixture(true, true);
    f.start();
    await f.answerAllStatus(pipeAloneStatus());
    await f.advance(400);
    equal(f.element('btn-bt-mode').textContent, 'Mode: Glasses+pipe', 'mode button names the saved client mode');
    equal(f.element('bt-g2-panels').style.display, 'none', 'no glasses panels while the glasses are closed');
    f.event('btn-bt-refresh');
    await f.answerAllStatus(glassesPipeStatus());
    equal(f.element('bt-mode-pill').textContent, 'Glasses+pipe', 'glasses role back');
    equal(f.element('bt-g2-panels').style.display, '', 'glasses panels back with the glasses');
    equal(f.element('btn-bt-open2').textContent, 'Open Bluetooth', 'client start card');
  }

  // Role switch (glasses build): a button for each role that is not running,
  // each behind a confirm that names the role it stops, sent through the
  // confirmed-control path. The saved-mode button stays.
  {
    const f = await ready();
    equal(f.element('btn-bt-use-console').style.display, 'none', 'the running console is not offered');
    equal(f.element('btn-bt-use-glasses').style.display, '', 'glasses offered from the console');
    equal(f.element('btn-bt-use-glasses').disabled, false, 'a confirmed role enables the switch');
    equal(f.element('btn-bt-mode').textContent, 'Mode: App console', 'saved-mode button kept');

    f.answer(false);
    f.event('btn-bt-use-glasses');
    await f.advance(100);
    equal(f.prompts, ['Use the glasses? This stops the running role (App console).'],
      'the confirm names the role it stops');
    equal(f.countCommand('openg2'), 0, 'a cancelled switch sends nothing');

    f.answer(true);
    f.event('btn-bt-use-glasses');
    f.event('btn-bt-use-glasses');
    await f.advance(100);
    equal(f.countCommand('openg2'), 1, 'a confirmed switch sends one command');
    equal(f.element('btn-bt-use-glasses').disabled, true, 'switches wait for the command');
    equal(f.element('btn-bt-mode').disabled, true, 'the mode button waits too');
    const g2Reply = 'G2: scan/connect started in background — use g2status to watch';
    f.pendingCommand('openg2').resolve(g2Reply);
    await flush();
    equal(f.element('bt-mode-pill').textContent, 'App console', 'the reply alone does not change the role');
    await f.answerAllStatus(status('client'));
    equal(f.element('bt-mode-pill').textContent, 'Glasses', 'readback confirms the glasses');
    equal(f.element('bt-action-status').textContent, g2Reply, 'switch reply shown');
    equal(f.element('btn-bt-use-glasses').style.display, 'none', 'running glasses not offered');
    equal(f.element('btn-bt-use-console').style.display, '', 'app console offered from the glasses');
    equal(f.element('btn-bt-use-console').disabled, false, 'switch usable after readback');

    // From the glasses back to the app console (the OLED's Start a week ago).
    f.event('btn-bt-use-console');
    await f.advance(100);
    equal(f.prompts.at(-1), 'Switch to server mode for a direct app connection? This stops the running role (Glasses).',
      'the console confirm names the glasses');
    f.pendingCommand('openconsole').resolve('Bluetooth started and advertising');
    await flush();
    await f.answerAllStatus(status());
    equal(f.element('bt-mode-pill').textContent, 'App console', 'readback confirms the console');
    equal(f.element('bt-action-status').textContent, 'Bluetooth started and advertising', 'console reply shown');

    // A refused switch keeps the role the device reports.
    f.event('btn-bt-use-glasses');
    await f.advance(100);
    f.pendingCommand('openg2').reject(Object.assign(new Error('HTTP 403'), {reply: 'Error: admin required'}));
    await flush();
    await f.answerAllStatus(status());
    equal(f.element('bt-mode-pill').textContent, 'App console', 'refused switch keeps the console');
    // The device's own refusal, not the transport status.
    equal(f.element('bt-action-status').textContent, 'Error: admin required', 'refusal shown');
  }

  // Nothing runs: every role is offered, and there is nothing to stop or confirm.
  {
    const f = fixture();
    f.start();
    const off = status('client');
    off.g2.connection = {...off.g2.connection, running: false, state: 'off'};
    off.link = link('off', 'glasses');
    await f.answerAllStatus(off);
    await f.advance(400);
    equal(f.element('bt-mode-pill').textContent, 'Off (saved: Glasses)', 'off names the saved mode');
    equal(f.element('btn-bt-use-console').style.display, '', 'console offered while off');
    equal(f.element('btn-bt-use-glasses').style.display, '', 'glasses offered while off');
    f.event('btn-bt-use-console');
    await f.advance(100);
    equal(f.prompts, [], 'no confirm when nothing runs');
    equal(f.countCommand('openconsole'), 1, 'off starts the app console directly');
  }

  // Pipe builds: beside the glasses only the console is a switch (`openphone`
  // would only add the pipe); from the console, the phone pipe is one.
  {
    const f = fixture(true, true);
    f.start();
    await f.answerAllStatus(glassesPipeStatus());
    await f.advance(400);
    equal(f.element('btn-bt-use-console').style.display, '', 'console offered beside the glasses');
    equal(f.element('btn-bt-use-glasses').style.display, 'none', 'running glasses not offered');
    equal(f.element('btn-bt-use-phone').style.display, 'none', 'no phone switch beside the glasses');
    f.event('btn-bt-use-console');
    await f.advance(100);
    equal(f.prompts.at(-1), 'Switch to server mode for a direct app connection? This stops the running role (Glasses+pipe).',
      'the confirm names the glasses and the pipe');
    f.pendingCommand('openconsole').resolve('Bluetooth started and advertising');
    await flush();
    const server = status('server');
    server.phone = phone(PIPE_OFF);
    await f.answerAllStatus(server);
    equal(f.element('btn-bt-use-glasses').style.display, '', 'glasses offered from the console');
    equal(f.element('btn-bt-use-phone').style.display, '', 'phone pipe offered from the console');
    f.event('btn-bt-use-phone');
    await f.advance(100);
    equal(f.prompts.at(-1), 'Use the phone pipe? This stops the running role (App console).',
      'the phone confirm names the console');
    f.pendingCommand('openphone').resolve('Phone pipe started');
    await flush();
    await f.answerAllStatus(pipeAloneStatus());
    equal(f.element('bt-mode-pill').textContent, 'Phone pipe', 'readback confirms the phone pipe');
    equal(f.element('btn-bt-use-phone').style.display, 'none', 'running pipe not offered');
    equal(f.element('btn-bt-use-console').style.display, '', 'console offered from the pipe');
    equal(f.element('btn-bt-use-glasses').style.display, '', 'glasses offered from the pipe');
  }

  // Phone-only build: the console and the pipe switch between each other.
  {
    const f = fixture(false, true);
    f.start();
    await f.answerAllStatus(phoneOnlyStatus());
    await f.advance(400);
    equal(f.element('btn-bt-use-console').style.display, '', 'console offered from the pipe');
    equal(f.element('btn-bt-use-phone').style.display, 'none', 'running pipe not offered');
    f.event('btn-bt-use-console');
    await f.advance(100);
    equal(f.prompts.at(-1), 'Switch to server mode for a direct app connection? This stops the running role (Phone pipe).',
      'the confirm names the phone pipe');
    equal(f.countCommand('openconsole'), 1, 'the console switch runs openconsole');
    equal(f.countCommand('openg2'), 0, 'no glasses command without the glasses');
  }

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
