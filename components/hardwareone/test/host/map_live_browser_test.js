'use strict';
// Exercise the production live-track handlers with deferred HTTP replies. Only
// DOM, transport, rendering, and timers are fake; no live-track logic is copied.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const sourcePath = process.argv[2] || path.resolve(__dirname, '../../WebPage_Maps.h');
const source = fs.readFileSync(sourcePath, 'utf8');
const start = source.indexOf('// Live tracking\n');
const end = source.indexOf('// Initialize\n', start);
assert(start >= 0 && end > start, 'production live-track section is present');
const script = source.slice(start, end);
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

function deferred(fields = {}) {
  const request = {...fields, done: false};
  request.promise = new Promise((resolve, reject) => {
    request.resolve = value => {
      assert(!request.done, 'request completes only once');
      request.done = true;
      resolve(value);
    };
    request.reject = error => {
      assert(!request.done, 'request completes only once');
      request.done = true;
      reject(error);
    };
  });
  return request;
}

function fixture() {
  let content = 'Existing track';
  const info = {
    writes: 0,
    get textContent() { return content; },
    set textContent(value) { content = String(value); this.writes++; },
    get innerHTML() { return content; },
    set innerHTML(value) { content = String(value); this.writes++; },
  };
  const button = {disabled: false};
  const commands = [];
  const polls = [];
  const alerts = [];
  const errors = [];
  const authResponses = [];
  const timers = new Map();
  let nextTimer = 1;
  let renders = 0;
  const context = vm.createContext({
    gpsTrack: [],
    hw: {
      $: id => ({'track-info': info, 'btn-live-track': button})[id] || null,
      _auth401(response) {
        authResponses.push(response.status);
        if (response.status === 401) throw new Error('Authentication required');
        return response;
      },
      fetchJSON(url) {
        const request = deferred({url});
        polls.push(request);
        return request.promise;
      },
    },
    fetch(url, options) {
      const request = deferred({url, options});
      commands.push(request);
      return request.promise;
    },
    alert(message) { alerts.push(message); },
    console: {error(...args) { errors.push(args); }},
    renderMap() { renders++; },
    setInterval(fn, delay) {
      const id = nextTimer++;
      timers.set(id, {fn, delay});
      return id;
    },
    clearInterval(id) { timers.delete(id); },
  });
  vm.runInContext(script, context, {filename: sourcePath});
  return {
    info, button, commands, polls, alerts, errors, authResponses, timers,
    get renders() { return renders; },
    get track() { return JSON.parse(JSON.stringify(context.gpsTrack)); },
    toggle: () => context.toggleLiveTrack(),
    poll: () => context.pollLiveTrack(),
    tick() {
      assert.equal(timers.size, 1, 'one active polling interval');
      return timers.values().next().value.fn();
    },
    answerCommand(data, status = 200) {
      commands.at(-1).resolve({status, ok: status >= 200 && status < 300,
        json: async () => data});
    },
    async answerPoll(data) {
      polls.at(-1).resolve(data);
      await flush();
    },
  };
}

function liveData(overrides = {}) {
  return {live: true, lastUpdate: 1, points: [{lat: 10, lon: 20}],
    count: 1, distance: 12.4, duration: 65, speed: 1.25, ...overrides};
}

async function running() {
  const f = fixture();
  const start = f.toggle();
  f.answerCommand({live: true, started: true});
  await start;
  await f.answerPoll(liveData());
  return f;
}

(async () => {
  const f = fixture();
  await f.poll();
  equal(f.polls.length, 0, 'inactive page does not poll');
  const starting = f.toggle();
  equal(f.button.disabled, true, 'start disables the control while request is pending');
  equal(f.commands.length, 1, 'start sends one command');
  equal(f.commands[0].url, '/api/gps/tracks?live=start', 'start uses the shared endpoint');
  equal(JSON.parse(JSON.stringify(f.commands[0].options)), {
    credentials: 'include', cache: 'no-store', headers: {Accept: 'application/json'},
  }, 'mutation retains session credentials and bypasses cache');
  await f.toggle();
  await f.poll();
  equal(f.commands.length, 1, 'duplicate start is ignored even when called directly');
  equal(f.timers.size, 0, 'pending start does not schedule polling');
  equal(f.polls.length, 0, 'pending start does not send a poll');
  equal(f.info.textContent, 'Existing track', 'pending start preserves displayed track');

  const body = deferred();
  f.commands[0].resolve({status: 200, ok: true, json: () => body.promise});
  await flush();
  equal(f.authResponses, [200], 'command response passes through the shared auth helper');
  equal(f.button.disabled, true, 'HTTP headers alone do not finish start');
  equal(f.timers.size, 0, 'start waits for a confirmed JSON body');
  body.resolve({live: true, started: true});
  await starting;
  equal(f.button.disabled, false, 'confirmed start releases the control');
  equal(f.timers.size, 1, 'confirmed start creates one interval');
  equal(f.timers.values().next().value.delay, 1000, 'live polling remains once per second');
  equal(f.polls.length, 1, 'confirmed start immediately requests a reading');
  equal(f.polls[0].url, '/api/gps/tracks?live=1', 'poll uses existing live status endpoint');
  matches(f.info.innerHTML, /LIVE.*Connecting/, 'confirmed start displays connecting state');

  await f.answerPoll(liveData());
  equal(f.track, [{lat: 10, lon: 20}], 'valid live points update the rendered track');
  equal(f.renders, 1, 'first new reading renders the map');
  matches(f.info.innerHTML, /1 pts \| 12m \| 1:05 \| 1\.3m\/s/, 'live stats retain their units and formatting');
  let poll = f.tick();
  await f.answerPoll(liveData({points: [{lat: 30, lon: 40}]}));
  await poll;
  equal(f.renders, 1, 'unchanged timestamp does not redraw');
  equal(f.track, [{lat: 10, lon: 20}], 'unchanged timestamp preserves the existing track');
  poll = f.tick();
  await f.answerPoll(liveData({lastUpdate: 2, distance: 1250, duration: 120,
    points: [{lat: 30, lon: 40}]}));
  await poll;
  equal(f.renders, 2, 'changed timestamp redraws');
  matches(f.info.innerHTML, /1\.25km \| 2:00/, 'long distance uses kilometers');

  const visible = f.info.innerHTML;
  for (const payload of [{live: true, points: []}, {}, {error: 'Temporarily unavailable'}]) {
    poll = f.tick();
    await f.answerPoll(payload);
    await poll;
    equal(f.timers.size, 1, 'unavailable points do not pretend the owner stopped');
    equal(f.info.innerHTML, visible, 'unavailable points preserve last confirmed stats');
  }
  poll = f.tick();
  f.polls.at(-1).reject(new Error('Poll network failure'));
  await poll;
  equal(f.errors.length, 1, 'poll failures use the existing diagnostic path');
  equal(f.timers.size, 1, 'poll failure retains the active interval');
  equal(f.alerts.length, 0, 'background poll failure does not interrupt with an alert');
  poll = f.tick();
  await f.answerPoll({live: false});
  await poll;
  equal(f.timers.size, 0, 'explicit owner stop clears polling');
  equal(f.info.textContent, 'Live tracking stopped', 'explicit owner stop updates visible state');
  const pollCount = f.polls.length;
  await f.poll();
  equal(f.polls.length, pollCount, 'no polls remain after the owner stops');

  // Both HTTP failure and a successful HTTP response with unconfirmed state
  // must leave the prior track visible and allow the user to try again.
  const failures = [
    {label: 'conflict', status: 409, data: {error: 'Error: logger already active'}, message: 'Error: logger already active'},
    {label: 'server text', status: 500, data: {error: 'Error: path "capture"\nunavailable'}, message: 'Error: path "capture"\nunavailable'},
    {label: 'failed HTTP with success body', status: 409, data: {live: true, started: true}, message: 'Live track start failed'},
    {label: 'missing confirmation', data: {live: true}, message: 'Live track start failed'},
    {label: 'false live', data: {live: false, started: true}, message: 'Live track start failed'},
    {label: 'truthy non-boolean', data: {live: 1, started: 1}, message: 'Live track start failed'},
    {label: 'null response', data: null, message: 'Live track start failed'},
    {label: 'network', reject: new Error('Network disconnected'), message: 'Network disconnected'},
    {label: 'invalid JSON', jsonError: new Error('Invalid JSON'), message: 'Invalid JSON'},
    {label: 'expired session', status: 401, data: {}, message: 'Authentication required'},
  ];
  for (const failure of failures) {
    const failed = fixture();
    const pending = failed.toggle();
    if (failure.reject) failed.commands[0].reject(failure.reject);
    else if (failure.jsonError) failed.commands[0].resolve({status: 200, ok: true,
      json: async () => { throw failure.jsonError; }});
    else failed.answerCommand(failure.data, failure.status || 200);
    await pending;
    equal(failed.alerts, [failure.message], failure.label + ' is surfaced to the user');
    equal(failed.button.disabled, false, failure.label + ' releases the control');
    equal(failed.timers.size, 0, failure.label + ' does not start polling');
    equal(failed.polls.length, 0, failure.label + ' does not request live readings');
    equal(failed.info.textContent, 'Existing track', failure.label + ' preserves displayed state');
    const retry = failed.toggle();
    equal(failed.commands.length, 2, failure.label + ' can be retried');
    failed.answerCommand({live: true, started: true});
    await retry;
    equal(failed.timers.size, 1, failure.label + ' retry can start');
    await failed.answerPoll({live: true, points: []});
  }

  for (const failure of [
    {data: {error: 'Error: stop refused'}, status: 409, message: 'Error: stop refused'},
    {data: {live: true, stopped: true}, message: 'Live track stop failed'},
    {data: {live: false}, message: 'Live track stop failed'},
    {reject: new Error('Stop network failure'), message: 'Stop network failure'},
  ]) {
    const active = await running();
    const timer = [...active.timers.keys()][0];
    const content = active.info.innerHTML;
    const stopping = active.toggle();
    equal(active.commands.at(-1).url, '/api/gps/tracks?live=stop', 'active control sends stop');
    equal(active.button.disabled, true, 'pending stop disables the control');
    await active.toggle();
    await active.tick();
    equal(active.commands.length, 2, 'pending stop suppresses duplicate actions');
    equal(active.polls.length, 1, 'pending stop pauses requests without discarding the interval');
    equal([...active.timers.keys()], [timer], 'pending stop retains the current interval');
    equal(active.info.innerHTML, content, 'pending stop preserves displayed live state');
    if (failure.reject) active.commands.at(-1).reject(failure.reject);
    else active.answerCommand(failure.data, failure.status || 200);
    await stopping;
    equal(active.alerts, [failure.message], 'failed stop surfaces its error');
    equal(active.button.disabled, false, 'failed stop releases the control');
    equal([...active.timers.keys()], [timer], 'failed stop retains its original interval');
    equal(active.info.innerHTML, content, 'failed stop retains displayed live state');
    const next = active.tick();
    equal(active.polls.length, 2, 'polling resumes after failed stop');
    await active.answerPoll(liveData({lastUpdate: 2}));
    await next;
    equal(active.renders, 2, 'new readings render after failed stop');
    const retry = active.toggle();
    active.answerCommand({live: false, stopped: true});
    await retry;
    equal(active.timers.size, 0, 'confirmed stop clears the interval');
    equal(active.info.textContent, 'Live tracking stopped', 'confirmed stop updates displayed state');
    equal(active.button.disabled, false, 'confirmed stop releases the control');
  }

  // A poll already in flight cannot overwrite pending, stopped, or restarted
  // state. Keep individual replies pending so ordering is explicit.
  const pendingStop = await running();
  const oldPoll = pendingStop.tick();
  const oldRequest = pendingStop.polls.at(-1);
  const stopRequest = pendingStop.toggle();
  const contentBefore = pendingStop.info.innerHTML;
  oldRequest.resolve(liveData({lastUpdate: 9, points: [{lat: 90, lon: 90}]}));
  await oldPoll;
  equal(pendingStop.renders, 1, 'in-flight reading is ignored while stop is pending');
  equal(pendingStop.info.innerHTML, contentBefore, 'pending stop is not overwritten by a stale reading');
  pendingStop.answerCommand({live: false, stopped: true});
  await stopRequest;

  const restarted = await running();
  const beforeStop = restarted.tick();
  const beforeStopReply = restarted.polls.at(-1);
  const beforeRestart = restarted.tick();
  const beforeRestartReply = restarted.polls.at(-1);
  const originalTimer = [...restarted.timers.keys()][0];
  const stop = restarted.toggle();
  restarted.answerCommand({live: false, stopped: true});
  await stop;
  beforeStopReply.resolve(liveData({lastUpdate: 3, points: [{lat: 90, lon: 90}]}));
  await beforeStop;
  equal(restarted.renders, 1, 'in-flight reading is ignored after confirmed stop');
  equal(restarted.info.textContent, 'Live tracking stopped', 'stale reading cannot revive stopped UI');
  const restart = restarted.toggle();
  restarted.answerCommand({live: true, started: true});
  await restart;
  const replacementTimer = [...restarted.timers.keys()][0];
  equal(replacementTimer === originalTimer, false, 'restart replaces the polling generation');
  beforeRestartReply.resolve({live: false});
  await beforeRestart;
  equal([...restarted.timers.keys()], [replacementTimer], 'old stopped reply cannot stop the new session');
  matches(restarted.info.innerHTML, /LIVE.*Connecting/, 'old stopped reply cannot overwrite restarted UI');
  await restarted.answerPoll(liveData());
  equal(restarted.renders, 2, 'restart accepts its first reading even with the previous timestamp');
  equal(restarted.track, [{lat: 10, lon: 20}], 'restart displays its own confirmed points');

  console.log('map_live_browser_test: ' + checks + ' assertions passed');
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
