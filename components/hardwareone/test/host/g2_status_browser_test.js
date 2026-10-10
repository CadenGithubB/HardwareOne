'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(process.argv[2], 'utf8');
const baselineSource = process.argv[3] && fs.readFileSync(process.argv[3], 'utf8');
let checks = 0;
function equal(actual, expected, label) {
  checks++;
  assert.deepEqual(actual, expected, label);
}

function fixture(script = source, enabled = true) {
  const elements = new Map();
  const element = id => {
    if (!elements.has(id)) {
      let text = '';
      elements.set(id, {
        className: '', style: {}, innerHTML: '',
        get textContent() { return text; },
        set textContent(value) { text = String(value); },
      });
    }
    return elements.get(id);
  };
  let response;
  const hw = {
    $: element,
    setText: (target, value) => { if (target) target.textContent = value; },
    toggle: (target, visible) => { target.style.display = visible ? '' : 'none'; },
    hide: target => { target.style.display = 'none'; },
    fetchJSON: url => {
      equal(url, '/api/ble/status', 'refresh uses authenticated status endpoint');
      return response instanceof Error ? Promise.reject(response) : Promise.resolve(response);
    },
  };
  // Both current and baseline fixtures include the controls touched by status
  // availability; rendering snapshots intentionally compare display fields only.
  for (const id of ['btn-bt-mode', 'btn-bt-open', 'btn-bt-open2', 'btn-bt-close',
                    'btn-bt-adv', 'btn-bt-disconnect',
                    'cb-g2-autoreconnect', 'cb-ring-autoreconnect']) element(id);
  const context = vm.createContext({hw, bleMode: 'client', btState: 'unknown', G2_ENABLED: enabled,
    reconnectState: {g2: null, ring: null}, controlPending: false,
    controlsConfirmed: false, statusRequest: 0});
  vm.runInContext(script, context);
  const snapshot = () => JSON.parse(JSON.stringify({
    state: context.btState,
    elements: Object.fromEntries([...elements].map(([id, e]) => [id, {
      text: e.textContent, className: e.className, style: e.style, innerHTML: e.innerHTML,
    }])),
  }));
  return {
    element, context, snapshot,
    async poll(value) {
      response = value;
      context.refresh();
      // refresh intentionally returns void; drain its promise/catch chain.
      await new Promise(resolve => setImmediate(resolve));
    },
  };
}

function connection(state = 'idle', left = false, right = false) {
  return {
    running: state !== 'off' && state !== 'disabled' && state !== 'uninitialized', state,
    left: {connected: left, mtu: 23, battery: -1, tx: 0, rx: 0},
    right: {connected: right, mtu: 244, battery: 82, tx: 19, rx: 3},
  };
}
function legacyText(c) {
  return `state=${c.state} L=${c.left.connected ? 'up' : 'down'} R=${c.right.connected ? 'up' : 'down'}` +
    ` mtu=L${c.left.mtu}/R${c.right.mtu} batt=L${c.left.battery}/R${c.right.battery}` +
    ` tx=L${c.left.tx}/R${c.right.tx} rx=L${c.left.rx}/R${c.right.rx}`;
}
function payload(c, text = legacyText(c)) {
  return {
    mode: 'client', g2: {connection: c}, g2Text: text,
    ring: {connected: false, name: '', addr: '', mtu: 0, rx: 0, scanFound: true},
  };
}

(async () => {
  // Steady and transitional FSM states, including both/one-side overrides.
  for (const state of ['off', 'disabled', 'uninitialized', 'idle', 'scanning',
                       'connecting', 'authenticating', 'connected', 'disconnecting', 'error']) {
    for (const [left, right] of [[false, false], [true, false], [false, true], [true, true]]) {
      const c = connection(state, left, right);
      const f = fixture();
      await f.poll(payload(c));
      const overall = left && right ? 'connected' : left || right ? 'connecting' : state;
      const pill = !c.running ? 'Off' : overall === 'connected' ? 'Connected' :
        ['scanning', 'connecting', 'authenticating'].includes(overall) ? 'Connecting' : 'Running';
      equal(f.element('bt-state-pill').textContent, pill, `pill for ${state}/${left}/${right}`);
      equal(f.element('bt-g2-panels').style.display, c.running ? '' : 'none', 'panel tracks running');
      equal(f.element('bt-status-text').style.display, c.running ? 'block' : 'none', 'diagnostic visibility');
      if (c.running) {
        equal(f.element('g2-state-summary').textContent, 'state: ' + overall, 'overall summary');
        equal(f.element('g2-l-state').textContent, left ? 'connected' : 'idle', 'left transport');
        equal(f.element('g2-r-state').textContent, right ? 'connected' : 'idle', 'right transport');
        equal(f.element('g2-l-batt').textContent, '?', 'unknown battery sentinel');
        equal(f.element('g2-l-tx').textContent, '0', 'zero counter stays visible');
      }
      if (baselineSource) {
        const previous = fixture(baselineSource);
        await previous.poll(payload(c));
        equal(f.snapshot(), previous.snapshot(), `baseline DOM for ${state}/${left}/${right}`);
      }
    }
  }

  // JSON numbers retain the old parsed-string display, including 0 and uint32 max.
  for (const battery of [-2, -1, 0, 100]) {
    const c = connection('connected', true, true);
    c.left = {connected: true, mtu: 0, battery, tx: 0xffffffff, rx: 0};
    c.right = {connected: true, mtu: 0xffff, battery: 0, tx: 0, rx: 0xffffffff};
    const f = fixture();
    await f.poll(payload(c));
    equal(f.element('g2-l-batt').textContent, battery === -1 ? '?' : String(battery), 'only -1 is unknown');
    equal(f.element('g2-r-batt').textContent, '0', 'zero battery');
    equal(f.element('g2-l-mtu').textContent, '0', 'zero MTU');
    equal(f.element('g2-r-mtu').textContent, '65535', 'maximum MTU');
    equal(f.element('g2-l-tx').textContent, '4294967295', 'maximum sent count');
    equal(f.element('g2-r-rx').textContent, '4294967295', 'maximum received count');
    if (baselineSource) {
      const previous = fixture(baselineSource);
      await previous.poll(payload(c));
      equal(f.snapshot(), previous.snapshot(), 'baseline numeric DOM');
    }
  }

  // Human-readable text is display-only, never an alternate source of state.
  const f = fixture();
  const c = connection('connected', true, true);
  const diagnostic = 'state=off <img src=x onerror=alert(1)> unrelated diagnostic';
  await f.poll(payload(c, diagnostic));
  equal(f.element('bt-state-pill').textContent, 'Connected', 'structured status wins over text');
  equal(f.element('bt-status-text').textContent, diagnostic, 'diagnostic stays literal text');
  equal(f.element('bt-status-text').innerHTML, '', 'diagnostic does not use HTML insertion');
  await f.poll(payload(c, ''));
  equal(f.element('bt-state-pill').textContent, 'Connected', 'structured status works without diagnostic');
  equal(f.element('bt-status-text').style.display, 'none', 'empty diagnostic hidden');

  // Failed or incomplete polls must not invent an off transition.
  const previous = f.snapshot();
  for (const response of [new Error('HTTP 503'), new Error('HTTP 500'), null,
                          {mode: 'client'}, {mode: 'client', g2: {}},
                          {mode: 'client', g2: {connection: null}},
                          {mode: 'client', g2: {connection: {}}},
                          {mode: 'client', g2: {connection: {running: true, state: 'idle'}}},
                          {mode: 'client', g2Text: 'state=off'}]) {
    await f.poll(response);
    equal(f.snapshot(), previous, 'failed/missing structured status keeps prior display');
  }

  // Server and G2-disabled views still follow the BLE owner, ignoring G2 text.
  for (const enabled of [true, false]) {
    const server = fixture(source, enabled);
    server.context.bleMode = 'server';
    await server.poll({mode: 'server', g2Text: 'state=off', ble: {
      initialized: true, state: 'advertising', connections: 0, maxConnections: 4,
      totalConnections: 2, commandsReceived: 7, clients: [],
    }});
    equal(server.element('bt-state-pill').textContent, 'Advertising', 'server view preserved');
    equal(server.element('bt-conn-count').textContent, '0 / 4', 'server connection count preserved');
    equal(server.element('bt-status-text').textContent, 'Lifetime: 2 connections · 7 commands', 'server diagnostic');
  }
  console.log(`G2 status browser: ${checks} checks passed${baselineSource ? ' with baseline DOM comparison' : ''}`);
})().catch(error => { console.error(error); process.exitCode = 1; });
