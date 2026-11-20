"""Run the shipping Logging page's status code against json replies.

The page reads /api/logging/status, whose three slots are json replies
('time json', 'sensorlog status json', the system log status json), and decides
from the parsed fields only. These tests run the page's own functions on a
small DOM stub and check: both status cards and the file-name stamps for the
json replies the firmware sends; that a text reply (the old wording included)
decides nothing; that a slot that is not json is tolerated; that the three
system-log states are shown and Start/Stop follow `running`; that a refresh
reads the endpoint (so it works without /api/cli) and falls back to the CLI
json forms.

Not a browser layout test and not an HTTP/firmware integration test: the reply
shapes here are copied from the firmware's formatters, whose own output is
pinned by test/host/test_sensorlog_status_rows.py and test_system_log_status.py.
"""
from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from tools.webui import extract_js
from tools.webui.js_engine import JS_ENGINE, run_js

ROOT = Path(__file__).resolve().parents[3]
PAGE = ROOT / "components/hardwareone/WebPage_Logging.h"

HARNESS = r"""
var nodes = {};
function node(id) {
  if (!nodes[id]) nodes[id] = {id: id, style: {}, textContent: '', value: '', checked: false, innerHTML: ''};
  return nodes[id];
}
var sensorBoxes = ['thermal', 'tof', 'imu', 'gamepad', 'apds', 'gps', 'presence', 'r1'].map(function (name) {
  return {value: name, checked: false};
});
var fetched = [], cli = [], endpoint = null, endpointFails = false, cliReplies = {}, cliFails = {};
var hw = {
  $: node,
  qsa: function (selector) { return selector === '#sensors-pane input[type=checkbox]' ? sensorBoxes : []; },
  fetchJSON: function (url) {
    fetched.push(url);
    if (url !== '/api/logging/status') return new Promise(function () {});
    return endpointFails ? Promise.reject(new Error('HTTP 500')) : Promise.resolve(endpoint);
  },
  postFormText: function (url, form) {
    cli.push(form.cmd);
    if (cliFails[form.cmd]) return Promise.reject(new Error(cliFails[form.cmd]));
    return Promise.resolve(cliReplies[form.cmd] || '');
  },
  postJSON: function () { return new Promise(function () {}); },
  on: function () {}, show: function () {}, hide: function () {}, toggle: function () {},
  setText: function () {}, setHTML: function () {}
};
var window = this;
var document = {addEventListener: function () {}, getElementById: node,
                createElement: function () { return {style: {}}; }, hidden: false};
var console = {log: function () {}, warn: function () {}, error: function () {}};
var alert = function () {};
eval(slurp(__argv[0]));

var failures = [];
function check(ok, message) { if (!ok) failures.push(message); }
function text(id) { return node(id).textContent; }
function settle(turns) {
  var p = Promise.resolve();
  for (var i = 0; i < turns; i++) p = p.then(function () {});
  return p;
}
function resetDom() {
  nodes = {};
  sensorBoxes.forEach(function (box) { box.checked = false; });
  fetched = []; cli = []; endpointFails = false; cliReplies = {}; cliFails = {};
}

// Replies as the firmware's formatters write them.
var TIME = '{"schema":1,"synced":true,"source":"ntp","time":"2026-11-20T14:05:09","uptime_ms":123456}';
var TIME_UNSET = '{"schema":1,"synced":false,"source":"none","time":"","uptime_ms":1000}';
var SENSOR_ACTIVE = '{"schema":1,"running":true,"file":"/logging_captures/sensors/a.csv","intervalMs":5000,"format":"csv",' +
  '"maxSizeBytes":256000,"rotations":3,"sensors":["thermal","gps"],"autoStart":true,"lastWriteAgoSec":5,"approxSizeBytes":4321}';
var SENSOR_INACTIVE = '{"schema":1,"running":false,"intervalMs":5000,"format":"track","maxSizeBytes":512000,"rotations":2,' +
  '"sensors":["gps","r1"],"autoStart":false}';
var SYS_LIMITS = ',"maxSizeKb":1024,"keep":3,"rotations":0,"reserveSkips":0,"reserveKb":100}';
var SYS_ACTIVE = '{"schema":1,"state":"active","running":true,"file":"/logging_captures/system/debug.log",' +
  '"lastWriteAgoSec":62,"outputFlags":9,"autoStart":true' + SYS_LIMITS;
var SYS_CONFIGURED = '{"schema":1,"state":"configured","running":true,"file":"/logging_captures/system/debug.log","autoStart":false' + SYS_LIMITS;
var SYS_INACTIVE = '{"schema":1,"state":"inactive","running":false,"autoStart":false' + SYS_LIMITS;
// The text forms of the same commands (read by people; the page must not match on them).
var SENSOR_TEXT = 'Sensor logging ACTIVE\n  File: /logging_captures/sensors/a.csv\n  Interval: 5000ms\n  Format: CSV\n' +
  '  Max size: 256000 bytes\n  Rotations: 3\n  Sensors: thermal gps \n  Auto-start: ON\n  Last write: 5s ago';
var SYS_TEXT = 'System logging ACTIVE\n  File: /logging_captures/system/debug.log\n  Last write: 62s ago\n' +
  '  Output flags: 0x09\n  Auto-start: ON';

/* Reply parsing. */
check(loggingParseReply(SENSOR_ACTIVE).running === true, 'a json reply parses');
check(loggingParseReply(SENSOR_TEXT) === null && loggingParseReply('') === null && loggingParseReply(undefined) === null &&
      loggingParseReply('[1]') === null && loggingParseReply('{"schema":1,"ok":false,"error":"buffer"}') === null,
      'text, empty, array and json-error replies are no data');

/* File-name stamps from 'time json'. */
resetDom();
node('config-format').value = 'csv';
generateDefaultFilename(TIME);
check(node('config-path').value === '/logging_captures/sensors/sensors-2026-11-20T14-05-09.csv',
      'sensor file name from device time: ' + node('config-path').value);
generateSystemFilename(TIME);
check(node('sys-config-path').value === '/logging_captures/system/system-2026-11-20T14-05-09.log',
      'system file name from device time: ' + node('sys-config-path').value);
['', TIME_UNSET, 'Time: 2026-11-20T14:05:09 (RTC)', 'Error: Out of memory', '{"time":"<img src=x>"}'].forEach(function (reply) {
  node('config-format').value = 'text';
  generateDefaultFilename(reply);
  check(/^\/logging_captures\/sensors\/sensors-\d{4}-\d{2}-\d{2}T\d{2}-\d{2}-\d{2}\.log$/.test(node('config-path').value) &&
        node('config-path').value.indexOf('2026-11-20T14-05-09') < 0,
        'browser clock for reply ' + JSON.stringify(reply) + ': ' + node('config-path').value);
});

/* Sensor logging card. */
resetDom();
refreshStatus(SENSOR_ACTIVE);
check(text('status-text') === 'ACTIVE' && node('btn-start').style.display === 'none' && node('btn-stop').style.display === 'inline-block',
      'active sensor card');
check(text('detail-file') === '/logging_captures/sensors/a.csv' && text('detail-interval') === '5000ms' &&
      text('detail-format') === 'CSV' && text('detail-maxsize') === (256000).toLocaleString() + ' bytes' &&
      text('detail-rotations') === '3' && text('detail-sensors') === 'thermal gps' && text('detail-lastwrite') === '5s ago',
      'active sensor details: ' + JSON.stringify([text('detail-file'), text('detail-interval'), text('detail-format'),
                                                  text('detail-maxsize'), text('detail-rotations'), text('detail-sensors'),
                                                  text('detail-lastwrite')]));
check(node('config-path').value === '/logging_captures/sensors/a.csv' && node('config-interval').value === '5000' &&
      node('config-format').value === 'csv' && node('config-maxsize').value === '256000' && node('config-rotations').value === '3',
      'active sensor config inputs');
check(text('autostart-status') === 'ON', 'sensor auto-start ON');

resetDom();
refreshStatus(SENSOR_INACTIVE);
check(text('status-text') === 'INACTIVE' && node('btn-start').style.display === 'inline-block' && node('btn-stop').style.display === 'none',
      'inactive sensor card');
check(node('config-format').value === 'track' && node('config-maxsize').value === '512000' && node('config-rotations').value === '2',
      'inactive sensor config inputs');
check(sensorBoxes.filter(function (b) { return b.checked; }).map(function (b) { return b.value; }).join(',') === 'gps,r1',
      'sensor boxes follow the json array by exact name');
check(text('autostart-status') === 'OFF', 'sensor auto-start OFF');

// The old text reply decides nothing: not ACTIVE, no details, nothing ticked.
resetDom();
refreshStatus(SENSOR_TEXT);
check(text('status-text') === 'Not Available' && node('btn-start').style.display === undefined && node('btn-stop').style.display === undefined &&
      text('detail-file') === '' && node('config-path').value === '' && text('autostart-status') === '?',
      'a text sensorlog reply is not matched: ' + text('status-text'));
// A reply that could not be read (a busy device) says so and leaves Start/Stop
// as the last good reply set them: the state is unknown, not "no sensor logging".
var BUSY = 'Error: Command queue full. Try again in a few seconds.';
resetDom();
refreshStatus(SENSOR_ACTIVE);
refreshStatus(BUSY);
check(text('status-text') === 'Not Available' && node('btn-start').style.display === 'none' && node('btn-stop').style.display === 'inline-block',
      'a busy sensorlog reply keeps the Stop button: ' + node('btn-stop').style.display);
resetDom();
refreshStatus(SENSOR_INACTIVE);
refreshStatus(BUSY);
check(text('status-text') === 'Not Available' && node('btn-start').style.display === 'inline-block' && node('btn-stop').style.display === 'none',
      'a busy sensorlog reply keeps the Start button: ' + node('btn-start').style.display);
['Unknown command: sensorlog', 'Error: admin required', '', '{"schema":1,"ok":false,"error":"response too large"}'].forEach(function (reply) {
  resetDom();
  var threw = null;
  try { refreshStatus(reply); } catch (e) { threw = e; }
  check(threw === null && text('status-text') === 'Not Available' && text('autostart-status') === '?',
        'sensor slot ' + JSON.stringify(reply) + ' is tolerated: ' + threw);
});
// Fields of the wrong type are skipped, never shown or copied.
resetDom();
refreshStatus('{"schema":1,"running":true,"file":7,"intervalMs":"x","format":null,"maxSizeBytes":"big","rotations":{},"sensors":"gps"}');
check(text('status-text') === 'ACTIVE' && text('detail-file') === '—' && text('detail-interval') === '—' && text('detail-format') === '—' &&
      text('detail-maxsize') === '—' && text('detail-rotations') === '—' && text('detail-sensors') === '—' &&
      text('detail-lastwrite') === '—' && node('config-path').value === '' && node('config-interval').value === '',
      'wrongly typed fields are skipped');

/* System logging card: three states; Start/Stop follow `running`. */
resetDom();
refreshSystemStatus(SYS_ACTIVE);
check(text('sys-status-text') === 'ACTIVE' && node('sys-btn-start').style.display === 'none' && node('sys-btn-stop').style.display === 'inline-block' &&
      text('sys-detail-file') === '/logging_captures/system/debug.log' && text('sys-detail-lastwrite') === '62s ago' &&
      text('sys-detail-flags') === '0x09' && node('sys-config-path').value === '/logging_captures/system/debug.log' &&
      text('sys-autostart-status') === 'ON', 'active system card: ' + text('sys-detail-flags'));
resetDom();
refreshSystemStatus(SYS_CONFIGURED);
check(text('sys-status-text') === 'NOT ROUTED' && node('sys-btn-start').style.display === 'none' && node('sys-btn-stop').style.display === 'inline-block' &&
      text('sys-detail-file') === '/logging_captures/system/debug.log' && text('sys-detail-lastwrite') === '—' && text('sys-detail-flags') === '—' &&
      node('sys-config-path').value === '' && text('sys-autostart-status') === 'OFF',
      'configured-but-not-routed system card (Stop, not Start: log start refuses while running)');
resetDom();
refreshSystemStatus(SYS_INACTIVE);
check(text('sys-status-text') === 'INACTIVE' && node('sys-btn-start').style.display === 'inline-block' && node('sys-btn-stop').style.display === 'none' &&
      text('sys-detail-file') === '—' && text('sys-autostart-status') === 'OFF', 'inactive system card');
resetDom();
refreshSystemStatus(SYS_TEXT);
check(text('sys-status-text') === 'Not Available' && text('sys-detail-file') === '' && text('sys-autostart-status') === '?' &&
      node('sys-btn-start').style.display === undefined && node('sys-btn-stop').style.display === undefined,
      'a text log status reply is not matched: ' + text('sys-status-text'));
resetDom();
refreshSystemStatus(SYS_ACTIVE);
refreshSystemStatus('{"schema":1,"ok":false,"error":"response too large"}');
check(text('sys-status-text') === 'Not Available' && node('sys-btn-start').style.display === 'none' &&
      node('sys-btn-stop').style.display === 'inline-block', 'an unreadable log status reply keeps the Stop button');
resetDom();
refreshSystemStatus('{"schema":1,"state":"active","running":true,"outputFlags":300}');
check(text('sys-detail-flags') === '0x12C', 'wider output flags keep their digits: ' + text('sys-detail-flags'));

/* Refresh without preloaded text: one endpoint request, no /api/cli. */
resetDom();
endpoint = {success: true, results: [TIME, SENSOR_ACTIVE, SYS_ACTIVE]};
refreshStatus();
refreshSystemStatus();
generateDefaultFilename();
settle(8).then(function () {
  check(cli.length === 0 && fetched.length === 3 && fetched.every(function (u) { return u === '/api/logging/status'; }),
        'refresh reads the endpoint only: fetched ' + fetched.join(',') + ' cli ' + cli.join(','));
  check(text('status-text') === 'ACTIVE' && text('autostart-status') === 'ON' && text('sys-status-text') === 'ACTIVE' &&
        text('sys-autostart-status') === 'ON', 'refresh applies the endpoint slots');

  /* Endpoint unavailable: the CLI json forms; a refused admin command shows an error, not a wrong state. */
  resetDom();
  endpointFails = true;
  cliReplies['sensorlog status json'] = SENSOR_INACTIVE;
  cliReplies['time json'] = TIME;
  cliFails['log status json'] = 'HTTP 400';
  node('config-format').value = 'text';
  refreshStatus();
  refreshSystemStatus();
  generateDefaultFilename();
  return settle(12);
}).then(function () {
  check(cli.slice().sort().join('|') === 'log status json|sensorlog status json|time json', 'fallback uses the json forms: ' + cli.join(','));
  check(text('status-text') === 'INACTIVE' && text('autostart-status') === 'OFF', 'fallback applies sensorlog status json');
  check(text('sys-status-text') === 'Error: HTTP 400', 'a refused log status shows the error: ' + text('sys-status-text'));
  // The extension follows the format select, which the status reply set in the same turn.
  check(/^\/logging_captures\/sensors\/sensors-2026-11-20T14-05-09\.(log|txt)$/.test(node('config-path').value),
        'fallback uses time json: ' + node('config-path').value);

  /* A reply with too few slots falls back as well. */
  resetDom();
  endpoint = {success: true, results: [TIME]};
  cliReplies['sensorlog status json'] = SENSOR_ACTIVE;
  refreshStatus();
  return settle(12);
}).then(function () {
  check(cli.join(',') === 'sensorlog status json' && text('status-text') === 'ACTIVE', 'a short endpoint reply falls back: ' + cli.join(','));
  if (failures.length) { __out('LOGGING_STATUS_PAGE FAIL\n' + failures.join('\n')); }
  else { __out('LOGGING_STATUS_PAGE PASS'); }
}).catch(function (e) {
  __out('LOGGING_STATUS_PAGE FAIL\nharness chain: ' + (e && e.stack ? e.stack : e));
});
undefined;
"""


class LoggingStatusPageContractTests(unittest.TestCase):
    """Source contracts: need no JavaScript engine."""

    def test_page_reads_json_forms_only(self) -> None:
        page = PAGE.read_text(encoding="utf-8")
        for wording in ("logging ACTIVE", "'Unknown command'", "'not found'", "/File:", "/Interval:", "/Format:",
                        "/Max size:", "/Rotations:", "/Sensors:", "/Last write:", "/Auto-start:", "/Output flags:",
                        "/Time:"):
            self.assertNotIn(wording, page, f"the Logging page still matches on text-reply wording {wording!r}")
        for command in ("time", "sensorlog status", "log status"):
            self.assertNotRegex(page, r"cmd:\s*'" + command + r"'",
                                f"the Logging page requests the text form of '{command}'")
        for call in ("loggingFetchStatusSlot(0, 'time json')", "loggingFetchStatusSlot(1, 'sensorlog status json')",
                     "loggingFetchStatusSlot(2, 'log status json')"):
            self.assertIn(call, page, f"the Logging page lost {call}")

    def test_bond_remote_panel_keeps_text(self) -> None:
        # The panel shows a peer's reply as text; an older peer has no json form.
        page = PAGE.read_text(encoding="utf-8")
        self.assertIn("window.BondFs.exec('sensorlog status', {", page)
        self.assertIn("window.BondFs.exec('log status', {", page)

    def test_endpoint_slots(self) -> None:
        server = (ROOT / "components/hardwareone/WebServer_Server.cpp").read_text(encoding="utf-8")
        start = server.index("static esp_err_t handleLoggingStatus(")
        handler = server[start:server.index("\n}\n", start)]
        self.assertIn('executeInternalStatusCommand("time json", timeBuf, sizeof(timeBuf));', handler)
        self.assertIn('executeInternalStatusCommand("sensorlog status json", sensorBuf, sizeof(sensorBuf));', handler)
        self.assertIn("formatSystemLogStatusJson(logBuf, sizeof(logBuf));", handler)
        order = [handler.index(text) for text in
                 ("results.add(timeBuf);", "results.add(sensorBuf);", "results.add(logBuf);")]
        self.assertEqual(order, sorted(order), "slot order: time, sensorlog, system log")


@unittest.skipUnless(JS_ENGINE, "a JavaScript engine is required")
class LoggingStatusPageTests(unittest.TestCase):
    def test_status_cards_read_json(self) -> None:
        page_js, _ = extract_js.page_js(PAGE)
        self.assertIn("function loggingParseReply", page_js)
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            script, harness = work / "page.js", work / "test.js"
            script.write_text(page_js, encoding="utf-8")
            harness.write_text(HARNESS, encoding="utf-8")
            result = run_js(harness, [str(script)])
        lines = result.stdout.splitlines()
        self.assertEqual(lines.count("LOGGING_STATUS_PAGE PASS"), 1, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
