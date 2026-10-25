'use strict';
// Run the dashboard's shipping Bluetooth card scripts against `status json`
// connectivity.bluetooth objects. Only the DOM and the Dash/hw helpers are fake.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const page = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
let checks = 0;
function equal(actual, expected, label) {
  checks++;
  assert.deepEqual(actual, expected, label);
}

// `phone` selects the ENABLE_BLE_PHONE_PIPE card and its relay pipe script.
function fixture(phone) {
  const elements = new Map();
  const html = phone ? page.phoneMarkup : page.markup;
  for (const tag of html.matchAll(/<([a-z][\w-]*)\b([^>]*?)>([^<]*)/gi)) {
    const attrs = new Map([...tag[2].matchAll(/([\w-]+)(?:\s*=\s*(?:'([^']*)'|"([^"]*)"))?/g)]
      .map(match => [match[1], match[2] ?? match[3] ?? '']));
    if (!attrs.has('id')) continue;
    const style = Object.fromEntries((attrs.get('style') || '').split(';').filter(Boolean)
      .map(value => value.split(':').map(part => part.trim())));
    elements.set(attrs.get('id'), {className: attrs.get('class') || '', style, textContent: tag[3]});
  }
  const hw = {
    $: x => typeof x === 'string' ? elements.get(x) || null : x,
    setText(x, text) { const e = hw.$(x); if (e) e.textContent = text; },
    show(x) { const e = hw.$(x); if (e) e.style.display = ''; },
    hide(x) { const e = hw.$(x); if (e) e.style.display = 'none'; },
    toggle(x, shown) { (shown ? hw.show : hw.hide)(x); },
  };
  const window = {Dash: {
    setText(id, value) { hw.setText(hw.$(id), value); },
    setIndicator(id, on) {
      const e = hw.$(id);
      if (e) e.className = on ? 'status-indicator status-enabled' : 'status-indicator status-disabled';
    },
  }};
  const warnings = [];
  const context = vm.createContext({window, hw, console: {warn: (...args) => warnings.push(args)}});
  vm.runInContext(page.connectivity, context, {filename: 'WebPage_Dashboard.h'});
  if (phone) vm.runInContext(page.pipe, context, {filename: 'WebPage_Dashboard.h'});
  return {
    text: id => { assert(elements.has(id), 'card markup includes ' + id); return elements.get(id).textContent; },
    shown: id => elements.get(id).style.display !== 'none',
    has: id => elements.has(id),
    update(bluetooth) {
      window.Dash.updateSystem({connectivity: {bluetooth}});
      equal(warnings, [], 'card update raised no error');
    },
  };
}

// connectivity.bluetooth as `status json` writes it: the existing keys plus the
// bleLinkStatusJson keys. `phoneState` is the pipe line; pipe builds only.
function bluetooth(role, saved, fields = {}) {
  const text = {console: 'App console', glasses: 'Glasses', 'glasses-pipe': 'Glasses+pipe',
    'phone-pipe': 'Phone pipe', off: 'Off'};
  return {running: role !== 'off', state: 'advertising', mode: 'server', server: role === 'console',
    client: false, g2Connected: false, role, roleText: text[role], saved, savedText: text[saved],
    savedDiffers: role !== saved, console: 'not-here', consoleText: 'not available in this mode',
    consoleClients: 0, advertising: false, ...fields};
}
function withPipe(line, fields = {}) {
  return {phoneConnected: line === 'ready', phoneState: line, pipeText: line.split(':')[0],
    pipePaired: line !== 'not paired', ...fields};
}

// Builds without the pipe: no pipe row; role and console rows still follow.
{
  equal(page.markup.includes('conn-bt-pipe'), false, 'no pipe row without the pipe');
  equal(page.phoneMarkup.includes('conn-bt-pipe'), true, 'pipe row in pipe builds');
  const f = fixture(false);
  equal(f.shown('conn-bt-saved-row'), false, 'saved row hidden before the first update');
  f.update(bluetooth('console', 'console', {console: 'waiting', consoleText: 'waiting for the app',
    advertising: true}));
  equal(f.text('conn-bt-running'), 'Yes', 'running');
  equal(f.text('conn-bt-role'), 'App console', 'console role');
  equal(f.shown('conn-bt-saved-row'), false, 'no saved row while the saved mode runs');
  equal(f.text('conn-bt-console'), 'waiting for the app', 'console waits for the app');
  f.update(bluetooth('console', 'console', {console: 'signed-in', consoleText: 'app signed in',
    consoleClients: 1, state: 'connected'}));
  equal(f.text('conn-bt-console'), 'app signed in', 'console signed in');
  equal(f.text('conn-bt-state'), 'connected', 'owner state kept');
  f.update(bluetooth('off', 'glasses', {console: 'off', consoleText: 'off', state: 'uninitialized'}));
  equal(f.text('conn-bt-running'), 'No', 'nothing runs');
  equal(f.text('conn-bt-role'), 'Off', 'role off');
  equal(f.shown('conn-bt-saved-row'), true, 'saved row names the mode Off would start');
  equal(f.text('conn-bt-saved'), 'Glasses', 'saved glasses');
  equal(f.text('conn-bt-console'), 'off', 'console off');
}

// Pipe builds: the pipe row uses the pipe words; the console is not here beside
// the phone pipe alone, and rides the pipe's link beside the glasses.
{
  const f = fixture(true);
  // A pipe `phone connect` started under a saved server mode.
  f.update(bluetooth('phone-pipe', 'console', {mode: 'server', state: 'ready', ...withPipe('ready')}));
  equal(f.text('conn-bt-role'), 'Phone pipe', 'standalone pipe role');
  equal(f.shown('conn-bt-saved-row'), true, 'saved row while the pipe runs under a saved server mode');
  equal(f.text('conn-bt-saved'), 'App console', 'saved console mode');
  equal(f.text('conn-bt-pipe'), 'ready', 'pipe ready');
  equal(f.text('conn-bt-console'), 'not available in this mode', 'console n/a while the pipe runs');

  f.update(bluetooth('glasses-pipe', 'glasses-pipe', {mode: 'client', ...withPipe('fail: not found',
    {pipeReason: 'not found'})}));
  equal(f.text('conn-bt-role'), 'Glasses+pipe', 'glasses and pipe');
  equal(f.shown('conn-bt-saved-row'), false, 'saved mode runs');
  equal(f.text('conn-bt-pipe'), 'fail: not found', 'pipe failure with its reason');

  f.update(bluetooth('glasses-pipe', 'glasses-pipe', {mode: 'client', ...withPipe('not paired')}));
  equal(f.text('conn-bt-pipe'), 'not paired', 'pipe not paired');

  // Beside the glasses the app console rides the pipe's link.
  f.update(bluetooth('glasses-pipe', 'glasses-pipe', {mode: 'client', ...withPipe('searching'),
    console: 'waiting-phone', consoleText: 'waiting for the phone link'}));
  equal(f.text('conn-bt-console'), 'waiting for the phone link', 'console waits for the phone link');
  f.update(bluetooth('glasses-pipe', 'glasses-pipe', {mode: 'client', ...withPipe('ready'),
    console: 'signed-in', consoleText: 'app signed in', consoleClients: 1}));
  equal(f.text('conn-bt-pipe'), 'ready', 'pipe ready under the console');
  equal(f.text('conn-bt-console'), 'app signed in', 'console signed in on the phone link');

  f.update(bluetooth('console', 'console', {console: 'waiting', consoleText: 'waiting for the app',
    advertising: true, ...withPipe('off')}));
  equal(f.text('conn-bt-role'), 'App console', 'console role in a pipe build');
  equal(f.text('conn-bt-pipe'), 'off', 'pipe off beside the console');
  equal(f.text('conn-bt-console'), 'waiting for the app', 'console waits in a pipe build');
}
console.log(`Dashboard Bluetooth browser: ${checks} checks passed`);
