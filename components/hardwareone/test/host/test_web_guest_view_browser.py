#!/usr/bin/env python3
"""Run the shipping page helpers (window.hw) as a guest sees them.

Owner decision B6 and the 2026-11-21 rule for refused controls: a guest sees
every control, and pressing one that changes the device answers with the
refusal popup; nothing is hidden. The two helper scripts every signed-in page
gets (the theme/role script holding hw.applyGuestViewOnly, and the fetch
helpers holding hw.fetchJSON, hw.postForm and hw._cliRefusal) are taken from
WebServer_Utils.cpp unchanged and run against a fake DOM and a fake fetch:

- applyGuestViewOnly marks the page and shows the guest banner but leaves every
  [data-guest-hide] control visible, and the page CSS hides none of them;
- the command line's guest refusal (a 403 from POST /api/cli) is shown once as
  the refusal popup and rejects with the reply's reason;
- the web gate's guest refusal (403, code "guest_forbidden") of a POST is shown
  as the popup with its sentence, not raw JSON, and rejects with that sentence;
- the same refusal of a GET (a page loading data) is not a popup: the page
  shows its own state from the rejection, which carries the sentence and 403;
- another 403 on a non-command POST, and a CLI read sent with hwQuietRefusal,
  are not popups.

Runs under Node.js, or JavaScriptCore's jsc on macOS when Node is absent;
exits 77 (skipped) with neither.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
JSC = Path("/System/Library/Frameworks/JavaScriptCore.framework/Versions/Current/Helpers/jsc")
LITERAL = re.compile(r'streamChunkC\(req,\s*("(?:\\.|[^"\\])*")\);')


def c_string(literal: str) -> str:
    """The text of one C string literal (the escapes these chunks use)."""
    body = literal[1:-1]
    out, i = [], 0
    simple = {"n": "\n", "t": "\t", "\\": "\\", '"': '"', "'": "'", "r": "\r"}
    while i < len(body):
        ch = body[i]
        if ch != "\\":
            out.append(ch)
            i += 1
            continue
        nxt = body[i + 1]
        if nxt in simple:
            out.append(simple[nxt])
            i += 2
        elif nxt == "u":
            out.append(chr(int(body[i + 2:i + 6], 16)))
            i += 6
        else:
            raise AssertionError(f"unhandled C escape \\{nxt}")
    return "".join(out)


def chunk(source: str, marker: str) -> str:
    found = [c_string(m.group(1)) for m in LITERAL.finditer(source) if marker in m.group(1)]
    if len(found) != 1:
        raise AssertionError(f"expected one helper chunk with {marker}, found {len(found)}")
    text = found[0]
    if not (text.startswith("<script>") and text.endswith("</script>")):
        raise AssertionError(f"the chunk with {marker} is not one script")
    return text[len("<script>"):-len("</script>")]


def css_hides_guest_controls(source: str) -> bool:
    return re.search(r"\[data-guest-hide\]\s*\{[^}]*display\s*:\s*none", source) is not None


DRIVER = r'''
var out = (typeof print === 'function') ? print : function(s) { console.log(s); };
var window = (typeof globalThis !== 'undefined') ? globalThis : this;
var failures = 0, checks = 0;
function check(ok, what) { checks++; if (!ok) { failures++; out('FAIL ' + what); } }

function El(id, attrs) { this.id = id; this.style = {display: ''}; this.attrs = attrs || {}; this.textContent = ''; this.children = []; }
var guestControl = new El('ctl', {'data-guest-hide': ''});
var banner = new El('hw-guest-banner'); banner.style.display = 'none';
var classes = [];
var document = {
  documentElement: {dataset: {}, classList: {add: function(c) { classes.push(c); }, contains: function(c) { return classes.indexOf(c) >= 0; }}},
  body: {style: {}},
  getElementById: function(id) { return id === 'hw-guest-banner' ? banner : null; },
  querySelectorAll: function(sel) { return sel === '[data-guest-hide]' ? [guestControl] : []; },
  querySelector: function() { return null; },
  addEventListener: function() {}
};
window.document = document;
window.__hwUser = {username: 'gina', isAdmin: false, isGuest: true, roleRank: 0};
var popups = [];
window.hw = {notify: function(level, msg) { popups.push(level + ':' + msg); }};

// Fake fetch: each test queues the next response.
var nextResponse = null;
function respond(status, body) {
  nextResponse = {status: status, body: body};
}
window.fetch = function(url, opts) {
  var r = nextResponse || {status: 200, body: '{"settings":{"theme":"light"}}'};
  nextResponse = null;
  function make() {
    return {
      status: r.status, ok: r.status >= 200 && r.status < 300,
      clone: function() { return make(); },
      text: function() { return Promise.resolve(r.body); },
      json: function() { return Promise.resolve(JSON.parse(r.body)); }
    };
  }
  return Promise.resolve(make());
};
function settle(p) {
  return p.then(function(v) { return {ok: true, value: v}; }, function(e) { return {ok: false, error: e}; });
}
function ticks(n) { var p = Promise.resolve(); for (var i = 0; i < n; i++) p = p.then(function() {}); return p; }

__THEME__
__HELPERS__

var hw = window.hw;
var GUEST_CLI = 'Error: Guest accounts are view-only. This command is not available to a guest.';
var GATE = '{"success":false,"code":"guest_forbidden","error":"Guest accounts are view-only","message":"Guest accounts are view-only"}';

hw.applyGuestViewOnly();
check(hw.isGuest() === true, 'the page knows the viewer is a guest');
check(guestControl.style.display === '', 'a [data-guest-hide] control stays visible');
check(banner.style.display === 'block', 'the guest banner is shown');
check(classes.indexOf('guest-view') >= 0, 'the page is marked guest-view');

function fresh() { popups = []; hw._shownErr = {}; }

ticks(4).then(function() {
  fresh();
  respond(403, GUEST_CLI);
  return settle(hw.postFormText('/api/cli', {cmd: 'g2nav on'}));
}).then(function(r) {
  return ticks(6).then(function() {
    check(!r.ok && r.error.status === 403, 'a refused command line rejects with 403');
    check(!r.ok && r.error.message === 'Guest accounts are view-only. This command is not available to a guest.',
          'the rejection carries the refusal reason');
    check(popups.length === 1 && popups[0] === 'error:Guest accounts are view-only. This command is not available to a guest.',
          'the command refusal is one popup: ' + JSON.stringify(popups));
    fresh();
    respond(403, GATE);
    return settle(hw.postJSON('/api/cli/batch', {commands: ['beginwrite', 'savesettings']}));
  });
}).then(function(r) {
  return ticks(6).then(function() {
    check(!r.ok && r.error.status === 403 && r.error.message === 'Guest accounts are view-only',
          'a gate refusal of a POST rejects with the sentence');
    check(popups.length === 1 && popups[0] === 'error:Guest accounts are view-only',
          'a gate refusal of a POST is one popup with the sentence: ' + JSON.stringify(popups));
    fresh();
    respond(403, GATE);
    return settle(hw.postForm('/api/llm/load', {model: 'x'}));
  });
}).then(function(r) {
  return ticks(6).then(function() {
    check(r.ok && r.value.status === 403, 'postForm hands the page the refused response');
    check(popups.length === 1 && popups[0] === 'error:Guest accounts are view-only',
          'postForm shows the gate refusal of a control: ' + JSON.stringify(popups));
    fresh();
    respond(403, GATE);
    return settle(hw.fetchJSON('/api/files/list?path=/maps'));
  });
}).then(function(r) {
  return ticks(6).then(function() {
    check(!r.ok && r.error.status === 403 && r.error.message === 'Guest accounts are view-only',
          'a refused page load rejects with the sentence for the page to show');
    check(popups.length === 0, 'a refused GET is not a popup: ' + JSON.stringify(popups));
    fresh();
    respond(403, '{"success":false,"error":"Admin access required"}');
    return settle(hw.postJSON('/api/llm/unload', {}));
  });
}).then(function(r) {
  return ticks(6).then(function() {
    check(!r.ok && r.error.status === 403, 'another refusal still rejects');
    check(popups.length === 0, 'only the guest refusal of a non-command POST is a helper popup');
    fresh();
    respond(403, GUEST_CLI);
    return settle(hw.postFormText('/api/cli', {cmd: 'camerafps'}, {hwQuietRefusal: true}));
  });
}).then(function(r) {
  return ticks(6).then(function() {
    check(!r.ok && r.error.status === 403, 'a quiet read still rejects');
    check(popups.length === 0, 'a quiet read is not a popup');
    out('checks ' + checks + ' failures ' + failures);
    out(failures === 0 ? 'RESULT ok' : 'RESULT failed');
  });
}).catch(function(e) { out('RESULT error ' + e); });
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--node", default=shutil.which("node") or shutil.which("nodejs"))
    parser.add_argument("--component", type=Path, default=HERE.parents[1])
    args = parser.parse_args()
    source = (args.component / "WebServer_Utils.cpp").read_text()
    if css_hides_guest_controls(source):
        raise SystemExit("web_guest_view_browser: the page CSS still hides [data-guest-hide] controls")
    script = (DRIVER.replace("__THEME__", chunk(source, "hw.applyGuestViewOnly=function"))
                    .replace("__HELPERS__", chunk(source, "hw._cliRefusal=function")))
    engine = None
    if args.node and shutil.which(args.node):
        engine = [args.node]
    elif JSC.exists():
        engine = [str(JSC)]
    if not engine:
        print("web_guest_view_browser: SKIP (no Node.js or jsc)")
        raise SystemExit(77)
    with tempfile.TemporaryDirectory(prefix="hw1-web-guest-view-") as directory:
        path = Path(directory) / "guest_view.js"
        path.write_text(script)
        run = subprocess.run(engine + [str(path)], capture_output=True, text=True, timeout=60)
    lines = (run.stdout or "").strip().splitlines()
    for line in lines:
        if line.startswith("FAIL"):
            print(line, file=sys.stderr)
    if run.returncode != 0 or not lines or lines[-1] != "RESULT ok":
        print((run.stdout or "")[-2000:], (run.stderr or "")[-2000:], file=sys.stderr)
        raise SystemExit("web_guest_view_browser: FAILED")
    print(f"web_guest_view_browser ({Path(engine[0]).name}): {lines[-2]}; a guest sees every "
          "control, refused presses are one popup, refused loads are the page's own state")


if __name__ == "__main__":
    main()
