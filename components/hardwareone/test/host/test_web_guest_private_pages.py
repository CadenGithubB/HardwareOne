#!/usr/bin/env python3
"""The pages whose data is private from a guest say so, never a raw error.

Owner, guest privacy 1-2 (2026-11-22): the ESP-NOW message history is
admin-only and private from a guest, and the ring's health data is private
from a guest. The web gate answers those reads with a 403 whose JSON carries
code "guest_private" ("Not available to guests") or, for a user reading the
message history, "admin_required" (web_guest_gate checks the gate). This runs
the shipping page scripts on the shipping fetch helpers (hw.fetchJSON and
hw._httpError from WebServer_Utils.cpp, unchanged) against a fake DOM and a
fake fetch, and checks:

- R1 Health: a guest's page says "Health data is not available to guests.",
  starts no 2 s poll and shows no popup; an administrator's page shows the
  vitals and polls; a transient failure (500) still starts the poll;
- ESP-NOW messages: a guest's conversation logs say "Message history is not
  available to guests.", a user's say "Message history is for administrators
  only.", the 500 ms poll stops, a later start does not fetch again, and the
  panels drawn afterwards read the same sentence (window.__espnowMessagesRefused,
  which renderDevicePanel in web_assets/espnow-core.js shows in place of
  "No messages yet"); an administrator's poll renders the messages and keeps
  running.

Runs under Node.js, or JavaScriptCore's jsc on macOS when Node is absent;
exits 77 (skipped) with neither.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_web_guest_view_browser import JSC, chunk  # noqa: E402


def between(source: str, start: str, end: str, what: str) -> str:
    if source.count(start) != 1:
        raise AssertionError(f"expected one start of {what}")
    i = source.index(start)
    j = source.index(end, i)
    return source[i:j]


def r1_script(source: str) -> str:
    body = between(source, "<script>(function(){\n  function v(ok,n,suf)", "</script>", "the R1 Health script")
    return body[len("<script>"):]


def espnow_poll_script(source: str) -> str:
    body = between(source, "<script>\n(function(){\n  var lastSeqNum = 0;", "</script>",
                   "the ESP-NOW message poll script")
    return body[len("<script>"):]


def check_core(core: str) -> None:
    want = ("<div class=\"message-empty\">' + hw.esc(window.__espnowMessagesRefused || "
            "'No messages yet. Start a conversation!') + '</div>")
    if core.count(want) != 1:
        raise AssertionError("renderDevicePanel shows the refused sentence in place of "
                             "'No messages yet' (web_assets/espnow-core.js)")


DRIVER = r'''
var out = (typeof print === 'function') ? print : console.log.bind(console);
var G = (typeof globalThis !== 'undefined') ? globalThis : this;
var window = G;
G.window = G;  // the page scripts run by indirect eval, in the global scope
var quiet = function() {};
G.console = {log: quiet, warn: quiet, error: quiet, info: quiet, debug: quiet};
var failures = 0, checks = 0;
function check(ok, what) { checks++; if (!ok) { failures++; out('FAIL ' + what); } }
function ticks(n) { var p = Promise.resolve(); for (var i = 0; i < n; i++) p = p.then(function() {}); return p; }

// Fake DOM: nodes by id, made on first use; .message-log nodes for ESP-NOW.
var nodes = {}, logs = [];
function Node(id) {
  this.id = id; this.style = {display: ''}; this.textContent = ''; this.value = '';
  this.disabled = false; this.title = ''; this.children = []; this.className = '';
  this.classList = {add: quiet, remove: quiet};
}
Node.prototype.addEventListener = quiet;
Node.prototype.querySelector = function(sel) {
  if (sel === '.message-empty') {
    for (var i = 0; i < this.children.length; i++)
      if (this.children[i].className === 'message-empty') return this.children[i];
  }
  return null;
};
var document = {
  documentElement: {dataset: {}, classList: {add: quiet}},
  body: {style: {}},
  activeElement: null,
  getElementById: function(id) { return nodes[id] || (nodes[id] = new Node(id)); },
  querySelectorAll: function(sel) { return sel === '.message-log' ? logs : []; },
  querySelector: function() { return null; },
  addEventListener: quiet
};
G.document = document;
G.addEventListener = quiet;

// Timers: recorded, never fired by themselves.
var intervals = {}, nextTimer = 1;
G.setInterval = function(fn, ms) { var id = nextTimer++; intervals[id] = {fn: fn, ms: ms}; return id; };
G.clearInterval = function(id) { delete intervals[id]; };
G.setTimeout = function(fn) { return nextTimer++; };
G.clearTimeout = quiet;
function liveIntervals() { return Object.keys(intervals).length; }

var popups = [];
G.hw = {notify: function(level, msg) { popups.push(level + ':' + msg); }};

// Fake fetch, answered by URL prefix.
var routes = {}, fetched = [];
function answer(prefix, status, body) { routes[prefix] = {status: status, body: body}; }
G.fetch = function(url) {
  fetched.push(url);
  var r = null;
  for (var k in routes) if (String(url).indexOf(k) === 0) r = routes[k];
  if (!r) r = {status: 404, body: '{"error":"no route"}'};
  function make() {
    return {status: r.status, ok: r.status >= 200 && r.status < 300,
            clone: function() { return make(); },
            text: function() { return Promise.resolve(r.body); },
            json: function() { return Promise.resolve(JSON.parse(r.body)); }};
  }
  return Promise.resolve(make());
};

__HELPERS__

var hw = G.hw;
// pollJSON is streamed with the page name; this stand-in records the poll.
var polls = [];
hw.pollJSON = function(u, ms, cb) {
  var p = {url: u, ms: ms, stopped: false};
  polls.push(p);
  return function() { p.stopped = true; };
};
function livePolls() { return polls.filter(function(p) { return !p.stopped; }).length; }

var PRIVATE = '{"success":false,"code":"guest_private","error":"Not available to guests","message":"Not available to guests"}';
var ADMIN = '{"success":false,"code":"admin_required","error":"Admin access required","message":"Admin access required"}';
var R1_SRC = __R1__;
var POLL_SRC = __POLL__;

function reset() {
  nodes = {}; logs = []; intervals = {}; routes = {}; fetched = []; polls = []; popups = [];
  hw._shownErr = {};
  delete G.__espnowMessagesRefused;
  delete G.espnowStartPolling; delete G.espnowStopPolling;
}
function emptyLog(mac) {
  var log = new Node('log-' + mac); log.className = 'message-log';
  var empty = new Node(''); empty.className = 'message-empty';
  empty.textContent = 'No messages yet. Start a conversation!';
  log.children.push(empty); logs.push(log);
  return empty;
}

function r1(status, body) {
  reset();
  answer('/api/health/status', status, body);
  (0, eval)(R1_SRC);
  return ticks(20);
}
function espnow(status, body) {
  reset();
  answer('/api/espnow/messages', status, body);
  var empties = [emptyLog('AA:BB:CC:00:00:01'), emptyLog('AA:BB:CC:00:00:02')];
  var appended = [];
  G.appendLogLine = function(id, type, text) { appended.push(id + '|' + type + '|' + text); return null; };
  (0, eval)(POLL_SRC);
  G.espnowStartPolling();
  return ticks(20).then(function() { return {empties: empties, appended: appended}; });
}

r1(403, PRIVATE).then(function() {
  check(nodes['rh-status'] && nodes['rh-status'].textContent === 'Health data is not available to guests.',
        'R1 Health tells a guest the data is not theirs: ' + (nodes['rh-status'] || {}).textContent);
  check(polls.length === 0, 'R1 Health starts no poll for a guest');
  check(popups.length === 0, 'R1 Health shows no popup for a guest: ' + JSON.stringify(popups));
  check(fetched.length === 1, 'R1 Health asked once');
  return r1(200, '{"schema":1,"ringConnected":true,"hrValid":true,"hr":61,"spo2Valid":true,"spo2":97}');
}).then(function() {
  check(nodes['rh-hr'].textContent === '61' && nodes['rh-spo2'].textContent === '97%',
        'R1 Health shows an administrator the vitals');
  check(livePolls() === 1 && polls[0].url === '/api/health/status' && polls[0].ms === 2000,
        'R1 Health polls for an administrator');
  check((nodes['rh-status'] || {textContent: ''}).textContent === '', 'no status line for an administrator');
  return r1(500, '{"error":"busy"}');
}).then(function() {
  check(livePolls() === 1, 'a transient failure still starts the poll');
  check((nodes['rh-status'] || {textContent: ''}).textContent === '', 'a transient failure is no guest sentence');
  return espnow(403, PRIVATE);
}).then(function(r) {
  var guest = 'Message history is not available to guests.';
  check(r.empties.every(function(e) { return e.textContent === guest; }),
        'a guest\'s conversation logs say the history is not theirs: ' + r.empties[0].textContent);
  check(G.__espnowMessagesRefused === guest, 'panels drawn later read the guest sentence');
  check(liveIntervals() === 0, 'the message poll stops for a guest');
  check(popups.length === 0, 'no popup for a guest\'s refused history');
  var asked = fetched.length;
  G.espnowStartPolling();
  return ticks(10).then(function() {
    check(fetched.length === asked && liveIntervals() === 0, 'a later start does not ask again');
    return espnow(403, ADMIN);
  });
}).then(function(r) {
  var user = 'Message history is for administrators only.';
  check(r.empties.every(function(e) { return e.textContent === user; }),
        'a user\'s conversation logs say the history is for administrators: ' + r.empties[0].textContent);
  check(G.__espnowMessagesRefused === user, 'panels drawn later read the user sentence');
  check(liveIntervals() === 0, 'the message poll stops for a user');
  return espnow(200, '{"messages":[{"seq":4,"mac":"aa:bb:cc:00:00:01","msg":"hello","of":1,"piece":1}],"deliveries":[]}');
}).then(function(r) {
  check(r.appended.length === 1 && r.appended[0] === 'log-AA:BB:CC:00:00:01|RECEIVED|hello',
        'an administrator\'s poll renders the message: ' + JSON.stringify(r.appended));
  check(liveIntervals() === 1, 'an administrator\'s poll keeps running');
  check(G.__espnowMessagesRefused === undefined, 'nothing is refused for an administrator');
  out('checks ' + checks + ' failures ' + failures);
  out(failures === 0 ? 'RESULT ok' : 'RESULT failed');
}).catch(function(e) { out('RESULT error ' + e + ' ' + (e && e.stack)); });
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--node", default=shutil.which("node") or shutil.which("nodejs"))
    parser.add_argument("--component", type=Path, default=HERE.parents[1])
    args = parser.parse_args()
    import json
    utils = (args.component / "WebServer_Utils.cpp").read_text()
    check_core((args.component / "web_assets" / "espnow-core.js").read_text())
    script = (DRIVER.replace("__HELPERS__", chunk(utils, "hw._cliRefusal=function"))
                    .replace("__R1__", json.dumps(r1_script(
                        (args.component / "WebPage_R1_Health.cpp").read_text())))
                    .replace("__POLL__", json.dumps(espnow_poll_script(
                        (args.component / "WebPage_ESPNow.h").read_text()))))
    engine = None
    if args.node and shutil.which(args.node):
        engine = [args.node]
    elif JSC.exists():
        engine = [str(JSC)]
    if not engine:
        print("web_guest_private_pages: SKIP (no Node.js or jsc)")
        raise SystemExit(77)
    with tempfile.TemporaryDirectory(prefix="hw1-web-guest-private-") as directory:
        path = Path(directory) / "guest_private.js"
        path.write_text(script)
        run = subprocess.run(engine + [str(path)], capture_output=True, text=True, timeout=60)
    lines = (run.stdout or "").strip().splitlines()
    for line in lines:
        if line.startswith("FAIL"):
            print(line, file=sys.stderr)
    if run.returncode != 0 or not lines or lines[-1] != "RESULT ok":
        print((run.stdout or "")[-2000:], (run.stderr or "")[-2000:], file=sys.stderr)
        raise SystemExit("web_guest_private_pages: FAILED")
    print(f"web_guest_private_pages ({Path(engine[0]).name}): {lines[-2]}; the health and "
          "message pages say the data is not a guest's (or a user's) and stop polling")


if __name__ == "__main__":
    main()
