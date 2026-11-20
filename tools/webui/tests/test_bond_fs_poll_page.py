"""Run the shipping BondFs helper's start-and-poll loop against a scripted device.

/api/bond/fs/list, /stat and /get no longer wait for the bonded peer inside
the web request: the first GET answers {success:true,pending:true,id:N} and
the page asks the same route again with ?id=N. These tests run the real
window.BondFs script (WebServer_Utils.h, getFileBrowserScript) with a fake
clock, fake timers and a scripted hw.fetchJSON, and check:

  - list / stat: the start, the polls every 250 ms with the id, one done()
    with the result (entries sorted folders first);
  - "bridge busy" on the start is retried and ends as an error after 7 s;
  - the peer's own "busy" as the polled answer starts the request again (the
    peer did not take it), for /get too, and ends as an error after 7 s;
  - a start error (not bonded, admin required) and a start that fails on the
    network end at once, with no poll and no second start;
  - a poll error (timeout, expired, a peer status) ends the call;
  - a poll that fails on the network is polled again, and the start is sent
    exactly once (for /get a second start would transfer the file twice);
  - a request that stays pending is given up one second after the device's
    own 7 s limit, with the start sent once;
  - a newer list() supersedes an older one: the older request is polled to
    its end but its done() is not called;
  - a superseded list() that was still waiting for the busy route is never
    sent to the device;
  - pull: the get start and its poll, then the watch on the landing folder.

Not a browser test and not an HTTP/firmware test: the reply shapes are the
ones WebPage_Bond.cpp writes, which test/host/test_bond_fs_poll.py pins on the
device side.
"""
from __future__ import annotations

from pathlib import Path
import re
import tempfile
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js

ROOT = Path(__file__).resolve().parents[3]
UTILS = ROOT / "components/hardwareone/WebServer_Utils.h"


def bond_fs_script() -> str:
    """The <script> block of getFileBrowserScript() that defines window.BondFs."""
    text = UTILS.read_text(encoding="utf-8")
    start = text.index('R"FBSCRIPT(') + len('R"FBSCRIPT(')
    body = text[start:text.index(')FBSCRIPT"', start)]
    blocks = [b for b in re.findall(r"<script>(.*?)</script>", body, re.S) if "window.BondFs = " in b]
    if len(blocks) != 1:
        raise AssertionError(f"expected one BondFs script block, found {len(blocks)}")
    return blocks[0]


HARNESS = r"""
var clock = 1000;
Date.now = function () { return clock; };
var timers = [], nextTimer = 1;
var setTimeout = function (fn, ms) { timers.push({id: nextTimer, at: clock + ms, fn: fn, every: 0}); return nextTimer++; };
var setInterval = function (fn, ms) { timers.push({id: nextTimer, at: clock + ms, fn: fn, every: ms}); return nextTimer++; };
var clearInterval = function (id) { timers = timers.filter(function (t) { return t.id !== id; }); };
var clearTimeout = clearInterval;

var fetched = [], respond = null;
var hw = {
  fetchJSON: function (url) {
    fetched.push({url: url, at: clock});
    var reply;
    try { reply = respond(url); } catch (e) { return Promise.reject(e); }
    return Promise.resolve(reply);
  },
  postFormText: function () { return Promise.resolve(''); },
  postForm: function () { return Promise.resolve({json: function () { return {success: true}; }}); }
};
var window = this;
var document = {getElementById: function () { return null; }};
var console = {log: function () {}, warn: function () {}, error: function () {}};
eval(slurp(__argv[0]));
var BondFs = window.BondFs;

var failures = [];
function check(ok, message) { if (!ok) failures.push(message); }
function settle(turns) {
  var p = Promise.resolve();
  for (var i = 0; i < turns; i++) p = p.then(function () {});
  return p;
}
// Let promises settle, then fire timers in time order until none is left or
// stop() says so.
function drive(stop, steps) {
  steps = steps === undefined ? 400 : steps;
  return settle(12).then(function () {
    if (!timers.length || steps <= 0 || (stop && stop())) return;
    timers.sort(function (a, b) { return a.at - b.at || a.id - b.id; });
    var t = timers[0];
    if (t.at > clock) clock = t.at;
    if (t.every) t.at = clock + t.every; else timers.shift();
    t.fn();
    return drive(stop, steps - 1);
  });
}
function reset() { timers = []; fetched = []; clock += 100000; }
function urls(prefix) {
  return fetched.filter(function (f) { return f.url.indexOf(prefix) === 0; });
}
function PENDING(id) { return {success: true, pending: true, id: id}; }

var calls;
function recorder() {
  calls = [];
  return function (value, err) { calls.push({value: value, err: err}); };
}

/* 1. list: start, two polls, the result once. */
reset();
var polls = 0;
respond = function (url) {
  if (url === '/api/bond/fs/list?path=%2Flogs') return PENDING(7);
  if (url === '/api/bond/fs/list?id=7') {
    polls++;
    if (polls < 3) return PENDING(7);
    return {success: true, path: '/logs', totalEntries: 3, hasMore: false, nextStartIndex: 0,
            entries: [{name: 'b.txt', isDir: false, size: 12, perms: 3},
                      {name: 'zdir', isDir: true, size: 0, perms: 3},
                      {name: 'a.txt', isDir: false, size: 5, perms: 3}]};
  }
  throw new Error('unexpected ' + url);
};
BondFs.list('/logs', recorder());
drive().then(function () {
  check(calls.length === 1 && calls[0].err === null, 'list: one done with a result: ' + JSON.stringify(calls));
  var names = calls.length && calls[0].value ? calls[0].value.map(function (e) { return e.name; }).join(',') : '';
  check(names === 'zdir,a.txt,b.txt', 'list: folders first, then by name: ' + names);
  check(fetched.length === 4, 'list: one start and three polls: ' + fetched.length);
  check(fetched[1].at - fetched[0].at === 250 && fetched[3].at - fetched[2].at === 250,
        'list: polls are 250 ms apart');
  check(timers.length === 0, 'list: no timer left running');

  /* 2. stat: busy twice on the start, then accepted. */
  reset();
  var starts = 0;
  respond = function (url) {
    if (url === '/api/bond/fs/stat?path=%2F') { starts++; return starts < 3 ? {success: false, error: 'bridge busy'} : PENDING(11); }
    if (url === '/api/bond/fs/stat?id=11') return {success: true, path: '/', total: 1000, used: 400, free: 600, usagePercent: 40.0};
    throw new Error('unexpected ' + url);
  };
  BondFs.stat('/', recorder());
  return drive();
}).then(function () {
  check(calls.length === 1 && calls[0].err === null && calls[0].value && calls[0].value.free === 600,
        'stat: result after two busy starts: ' + JSON.stringify(calls));
  check(urls('/api/bond/fs/stat?path=').length === 3 && urls('/api/bond/fs/stat?id=').length === 1,
        'stat: three starts, one poll');

  /* 3. busy for good: an error after the 7 s limit, never a poll. */
  reset();
  respond = function () { return {success: false, error: 'bridge busy'}; };
  var t0 = clock;
  BondFs.stat('/', recorder());
  return drive().then(function () {
    check(calls.length === 1 && calls[0].value === null && calls[0].err === 'bridge busy',
          'busy: ends as bridge busy: ' + JSON.stringify(calls));
    check(clock - t0 >= 7000 && clock - t0 <= 7250, 'busy: gives up at the 7 s limit: ' + (clock - t0));
    check(urls('/api/bond/fs/stat?id=').length === 0, 'busy: nothing was polled');
  });
}).then(function () {
  /* 4. start errors end at once. */
  reset();
  respond = function () { return {success: false, error: 'not bonded'}; };
  BondFs.list('/', recorder());
  return drive();
}).then(function () {
  check(calls.length === 1 && calls[0].err === 'not bonded' && fetched.length === 1,
        'not bonded: one request, error shown: ' + JSON.stringify(calls));
  reset();
  respond = function () { throw new Error('HTTP 500'); };
  BondFs.stat('/sd', recorder());
  return drive();
}).then(function () {
  check(calls.length === 1 && calls[0].err === 'HTTP 500' && fetched.length === 1,
        'start network failure: no retry of the start: ' + JSON.stringify(calls) + ' ' + fetched.length);

  /* 5. poll errors end the call. */
  reset();
  respond = function (url) {
    if (url.indexOf('?path=') > 0) return PENDING(21);
    return {success: false, error: 'timeout'};
  };
  BondFs.list('/x', recorder());
  return drive();
}).then(function () {
  check(calls.length === 1 && calls[0].err === 'timeout' && fetched.length === 2,
        'poll timeout: shown, polling stops: ' + JSON.stringify(calls));
  reset();
  respond = function (url) {
    if (url.indexOf('?path=') > 0) return PENDING(22);
    return {success: false, error: 'not_found', path: '/x'};
  };
  BondFs.list('/x', recorder());
  return drive();
}).then(function () {
  check(calls.length === 1 && calls[0].err === 'not_found', 'peer status: shown: ' + JSON.stringify(calls));

  /* 6. pending for good: given up at 8 s, the start sent once. */
  reset();
  respond = function (url) { return PENDING(31); };
  var t0 = clock;
  BondFs.stat('/', recorder());
  return drive().then(function () {
    check(calls.length === 1 && calls[0].value === null && calls[0].err === 'timeout',
          'pending for good: timeout: ' + JSON.stringify(calls));
    check(clock - t0 >= 8000 && clock - t0 <= 8250, 'pending for good: stops at 8 s: ' + (clock - t0));
    check(urls('/api/bond/fs/stat?path=').length === 1, 'pending for good: one start');
    check(timers.length === 0, 'pending for good: no timer left running');
  });
}).then(function () {
  /* 7. a poll that fails on the network is polled again; one start. */
  reset();
  var pollCount = 0;
  respond = function (url) {
    if (url === '/api/bond/status') return {bonded: true, role: 1, peerMac: 'AA:BB:CC:DD:EE:FF', peerName: 'peer'};
    if (url === '/api/bond/fs/get?path=%2Flogs%2Fa.txt') return PENDING(41);
    if (url === '/api/bond/fs/get?id=41') {
      pollCount++;
      if (pollCount < 3) throw new Error('network');
      return {success: true, path: '/logs/a.txt', size: 10, message: 'transfer initiated'};
    }
    if (url.indexOf('/api/files/list?path=') === 0) {
      return {files: urls('/api/files/list').length >= 3 ? [{name: 'a.txt'}] : []};
    }
    throw new Error('unexpected ' + url);
  };
  BondFs.checkAvailable(function () {});
  return settle(12).then(function () {
    BondFs.pull('/logs/a.txt', recorder());
    return drive();
  });
}).then(function () {
  check(calls.length === 1 && calls[0].err === null && calls[0].value &&
        calls[0].value.localPath === '/espnow/received/AABBCCDDEEFF/a.txt' && calls[0].value.size === 10,
        'pull: the file is reported where it lands: ' + JSON.stringify(calls));
  check(urls('/api/bond/fs/get?path=').length === 1, 'pull: the get start is sent exactly once: ' +
        urls('/api/bond/fs/get?path=').length);
  check(urls('/api/bond/fs/get?id=41').length === 3, 'pull: the failed polls were repeated');
  check(timers.length === 0, 'pull: no timer left running');

  /* 8. a get whose poll ends in an error is not started again. */
  reset();
  respond = function (url) {
    if (url.indexOf('/api/bond/fs/get?path=') === 0) return PENDING(51);
    if (url === '/api/bond/fs/get?id=51') return {success: false, error: 'expired'};
    throw new Error('unexpected ' + url);
  };
  BondFs.pull('/logs/b.txt', recorder());
  return drive();
}).then(function () {
  check(calls.length === 1 && calls[0].err === 'expired', 'pull: a poll error is shown: ' + JSON.stringify(calls));
  check(urls('/api/bond/fs/get?path=').length === 1, 'pull: no second start after a poll error');
  check(urls('/api/files/list').length === 0, 'pull: no watch on the landing folder after an error');

  /* 9. a newer list() supersedes an older one. */
  reset();
  var slot = null, issued = 60, firstDone = [], secondDone = [];
  respond = function (url) {
    var m;
    if ((m = /^\/api\/bond\/fs\/list\?path=(.*)$/.exec(url))) {
      if (slot) return {success: false, error: 'bridge busy'};
      slot = {id: ++issued, path: decodeURIComponent(m[1]), polls: 0};
      return PENDING(slot.id);
    }
    if ((m = /^\/api\/bond\/fs\/list\?id=(\d+)$/.exec(url))) {
      if (!slot || String(slot.id) !== m[1]) return {success: false, error: 'expired'};
      if (++slot.polls < 3) return PENDING(slot.id);
      var reply = {success: true, path: slot.path, entries: [{name: slot.path.slice(1) + '.txt', isDir: false, size: 1, perms: 1}]};
      slot = null;
      return reply;
    }
    throw new Error('unexpected ' + url);
  };
  BondFs.list('/old', function (v, e) { firstDone.push({value: v, err: e}); });
  return settle(12).then(function () {
    BondFs.list('/new', function (v, e) { secondDone.push({value: v, err: e}); });
    return drive();
  }).then(function () {
    check(firstDone.length === 0, 'superseded: the older list never calls done: ' + JSON.stringify(firstDone));
    check(secondDone.length === 1 && secondDone[0].err === null && secondDone[0].value[0].name === 'new.txt',
          'superseded: the newer list is shown: ' + JSON.stringify(secondDone));
    check(urls('/api/bond/fs/list?id=61').length === 3, 'superseded: the older request was polled to its end');
    check(slot === null && timers.length === 0, 'superseded: the device slot is free and no timer runs');
  });
}).then(function () {
  /* 10. three quick folder changes: the middle one, still waiting for the
     busy route when the third arrives, is never started on the device. */
  reset();
  var slot = null, issued = 80, accepted = [], doneA = [], doneB = [], doneC = [];
  respond = function (url) {
    var m;
    if ((m = /^\/api\/bond\/fs\/list\?path=(.*)$/.exec(url))) {
      if (slot) return {success: false, error: 'bridge busy'};
      slot = {id: ++issued, path: decodeURIComponent(m[1]), polls: 0};
      accepted.push(slot.path);
      return PENDING(slot.id);
    }
    if ((m = /^\/api\/bond\/fs\/list\?id=(\d+)$/.exec(url))) {
      if (!slot || String(slot.id) !== m[1]) return {success: false, error: 'expired'};
      if (++slot.polls < 3) return PENDING(slot.id);
      var reply = {success: true, path: slot.path, entries: [{name: slot.path.slice(1) + '.txt', isDir: false, size: 1, perms: 1}]};
      slot = null;
      return reply;
    }
    throw new Error('unexpected ' + url);
  };
  BondFs.list('/a', function (v, e) { doneA.push({value: v, err: e}); });
  return settle(12).then(function () {
    BondFs.list('/b', function (v, e) { doneB.push({value: v, err: e}); });
    return settle(12);
  }).then(function () {
    BondFs.list('/c', function (v, e) { doneC.push({value: v, err: e}); });
    return drive();
  }).then(function () {
    check(accepted.join(',') === '/a,/c', 'abandoned: the device starts /a and /c only: ' + accepted.join(','));
    check(doneA.length === 0 && doneB.length === 0, 'abandoned: the older lists never call done');
    check(doneC.length === 1 && doneC[0].err === null && doneC[0].value[0].name === 'c.txt',
          'abandoned: the newest list is shown: ' + JSON.stringify(doneC));
    check(urls('/api/bond/fs/list?path=%2Fb').length === 1,
          'abandoned: /b is not asked again once superseded: ' + urls('/api/bond/fs/list?path=%2Fb').length);
    check(slot === null && timers.length === 0, 'abandoned: the device slot is free and no timer runs');
  });
}).then(function () {
  /* 11. the peer answers busy (it holds one waiting request for all three
     routes): the request is started again and its result shown. */
  reset();
  var starts = 0;
  respond = function (url) {
    if (url === '/api/bond/fs/stat?path=%2F') { starts++; return PENDING(90 + starts); }
    if (url === '/api/bond/fs/stat?id=91') return {success: false, error: 'busy', path: '/'};
    if (url === '/api/bond/fs/stat?id=92') return {success: true, path: '/', total: 1000, used: 100, free: 900, usagePercent: 10.0};
    throw new Error('unexpected ' + url);
  };
  BondFs.stat('/', recorder());
  return drive();
}).then(function () {
  check(calls.length === 1 && calls[0].err === null && calls[0].value && calls[0].value.free === 900,
        'peer busy: the second start gives the result: ' + JSON.stringify(calls));
  check(urls('/api/bond/fs/stat?path=').length === 2 && urls('/api/bond/fs/stat?id=').length === 2,
        'peer busy: two starts, one poll each');
  check(timers.length === 0, 'peer busy: no timer left running');

  /* 12. the same for a pull: busy means the peer did not take the get, so a
     second start is not a second transfer. */
  reset();
  var starts = 0;
  respond = function (url) {
    if (url === '/api/bond/fs/get?path=%2Flogs%2Fc.txt') { starts++; return PENDING(100 + starts); }
    if (url === '/api/bond/fs/get?id=101') return {success: false, error: 'busy', path: '/logs/c.txt'};
    if (url === '/api/bond/fs/get?id=102') return {success: true, path: '/logs/c.txt', size: 4, message: 'transfer initiated'};
    if (url.indexOf('/api/files/list?path=') === 0) return {files: [{name: 'c.txt'}]};
    throw new Error('unexpected ' + url);
  };
  BondFs.pull('/logs/c.txt', recorder());
  return drive();
}).then(function () {
  check(calls.length === 1 && calls[0].err === null && calls[0].value && calls[0].value.size === 4,
        'pull after peer busy: the file is reported: ' + JSON.stringify(calls));
  check(urls('/api/bond/fs/get?path=').length === 2, 'pull after peer busy: started again once: ' +
        urls('/api/bond/fs/get?path=').length);

  /* 13. a peer that stays busy: an error after the 7 s limit. */
  reset();
  var issuedBusy = 200;
  respond = function (url) {
    if (url.indexOf('?path=') > 0) return PENDING(++issuedBusy);
    return {success: false, error: 'busy', path: '/'};
  };
  var t0 = clock;
  BondFs.stat('/', recorder());
  return drive().then(function () {
    check(calls.length === 1 && calls[0].value === null && calls[0].err === 'busy',
          'peer busy for good: ends as busy: ' + JSON.stringify(calls));
    check(clock - t0 >= 7000 && clock - t0 <= 7500, 'peer busy for good: gives up at the 7 s limit: ' + (clock - t0));
    check(timers.length === 0, 'peer busy for good: no timer left running');
  });
}).then(function () {
  if (failures.length) { __out('BOND_FS_POLL_PAGE FAIL\n' + failures.join('\n')); }
  else { __out('BOND_FS_POLL_PAGE PASS'); }
}).catch(function (e) {
  __out('BOND_FS_POLL_PAGE FAIL\nharness chain: ' + (e && e.stack ? e.stack : e));
});
undefined;
"""


class BondFsPollContractTests(unittest.TestCase):
    """Source contracts: need no JavaScript engine."""

    def test_routes_go_through_the_poll_helper(self) -> None:
        script = bond_fs_script()
        self.assertEqual(script.count("function fsCall(route, query, done, abandoned)"), 1)
        for route in ("/api/bond/fs/list", "/api/bond/fs/stat", "/api/bond/fs/get"):
            self.assertEqual(script.count(f"fsCall('{route}',"), 1, f"{route} must go through fsCall once")
            self.assertNotIn(f"fetchJSON('{route}", script, f"{route} is fetched outside fsCall")
        self.assertIn("var FS_POLL_MS = 250, FS_LIMIT_MS = 7000,", script)

    def test_callers_only_use_done(self) -> None:
        # The Files and Logging pages rely on done(result, err) and nothing else.
        for name in ("WebPage_Files.h", "WebPage_Logging.h"):
            page = (ROOT / "components/hardwareone" / name).read_text(encoding="utf-8")
            self.assertNotIn("/api/bond/fs/", page, f"{name} calls a bond fs route itself")


@unittest.skipUnless(JS_ENGINE, "a JavaScript engine is required")
class BondFsPollPageTests(unittest.TestCase):
    def test_poll_loop(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            script, harness = work / "bondfs.js", work / "test.js"
            script.write_text(bond_fs_script(), encoding="utf-8")
            harness.write_text(HARNESS, encoding="utf-8")
            result = run_js(harness, [str(script)])
        lines = result.stdout.splitlines()
        self.assertEqual(lines.count("BOND_FS_POLL_PAGE PASS"), 1, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
