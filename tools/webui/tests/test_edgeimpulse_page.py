"""Execute the shipping Edge Impulse sensor-card script against detect replies.

Covers the detection overlay on the /sensors Edge Impulse card
(System_EdgeImpulse_Web.h): boxes are scaled per axis from the reply's
modelInputWidth/modelInputHeight (a non-square model is drawn with its own
height, not its width), an older reply carrying only modelInputSize still
scales square, a "busy" reply (another inference holds the model) keeps the
last boxes and detection list, and any other failure still clears the boxes
and shows the error. The busy text is read from System_EdgeImpulse.cpp so the
page and the owner cannot drift apart.

Not covered: rendering, real canvas drawing, the polling cadence and the
camera stream element itself (stubbed with a fixed 320x240 rectangle).

Run from the repository root:

    python3 -m unittest tools.webui.tests.test_edgeimpulse_page
"""
from __future__ import annotations

import json
from pathlib import Path
import re
import tempfile
import unittest

from tools.webui import extract_js
from tools.webui.js_engine import JS_ENGINE, run_js

ROOT = Path(__file__).resolve().parents[3]
PAGE = ROOT / "components/hardwareone/System_EdgeImpulse_Web.h"
OWNER = ROOT / "components/hardwareone/System_EdgeImpulse.cpp"

HARNESS = r"""
var cfg = JSON.parse(slurp(__argv[1]));
var nodes = {}, ops = [];
function makeNode(id) {
  var n = {id: id, style: {}, children: [], disabled: false, value: '', innerHTML: '', textContent: '',
           className: '', files: null};
  n.appendChild = function (c) { this.children.push(c); c.parentElement = this; };
  n.addEventListener = function () {};
  return n;
}
var camWrapper = makeNode('wrapper');
var camImg = makeNode('camera-stream-img');
camImg.parentElement = camWrapper;
camImg.getBoundingClientRect = function () { return {width: 320, height: 240}; };
nodes['camera-stream-img'] = camImg;
function node(x) {
  if (typeof x !== 'string') return x;
  if (!nodes[x]) nodes[x] = makeNode(x);
  return nodes[x];
}
var ctx2d = {
  clearRect: function () { ops.push('clear'); },
  strokeRect: function (x, y, w, h) { ops.push([x, y, w, h]); },
  fillRect: function () {}, fillText: function () {},
  measureText: function () { return {width: 10}; }
};
var pending = {then: function () { return this; }, catch: function () { return this; }};
var document = {
  createElement: function (tag) {
    var n = makeNode('_' + tag);
    n.getContext = function () { return ctx2d; };
    return n;
  },
  addEventListener: function () {}
};
var window = {};
var console = {log: function () {}, warn: function () {}, error: function () {}};
function setTimeout() { return 0; }
function setInterval() { return 0; }
function clearInterval() {}
function alert() {}
var hw = {
  $: node,
  setText: function (x, v) { node(x).textContent = v; },
  setHTML: function (x, v) { node(x).innerHTML = v; },
  show: function (x) { node(x).style.display = ''; },
  hide: function (x) { node(x).style.display = 'none'; },
  on: function () {},
  fetchJSON: function () { return pending; },
  postJSON: function () { return pending; },
  postForm: function () { return pending; },
  postFormText: function () { return pending; }
};
eval(slurp(__argv[0]));

var failures = [];
function check(ok, msg) { if (!ok) failures.push(msg); }
function near(a, b) { return Math.abs(a - b) < 0.01; }
function rects() { return ops.filter(function (o) { return typeof o !== 'string'; }); }
function clears() { return ops.filter(function (o) { return o === 'clear'; }).length; }
function det(label, x, y, w, h) { return {label: label, confidence: 0.9, x: x, y: y, width: w, height: h}; }

// 1. A non-square 96x48 model: x scales by 320/96, y by 240/48.
ops = [];
window._eiShowDetections({success: true, inferenceTimeMs: 5, modelInputSize: 96,
                          modelInputWidth: 96, modelInputHeight: 48,
                          detections: [det('cat', 40, 20, 8, 4)]});
var r = rects();
check(r.length === 1, 'non-square: expected one box, got ' + JSON.stringify(r));
if (r.length === 1) {
  // centre (44*10/3, 22*5) = (146.67, 110); size (8*10/3*4, 4*5*4) = (106.67, 80)
  check(near(r[0][0], 146.6667 - 53.3333) && near(r[0][2], 106.6667),
        'non-square: x axis scaled wrong ' + JSON.stringify(r[0]));
  check(near(r[0][1], 110 - 40) && near(r[0][3], 80),
        'non-square: y axis scaled by the width instead of the height ' + JSON.stringify(r[0]));
}
var listAfterHit = node('ei-detections').innerHTML;
check(/cat/.test(listAfterHit), 'non-square: detection list not shown: ' + listAfterHit);

// 2. Busy: another inference holds the model. Keep the boxes and the list.
ops = [];
window._eiShowDetections({success: false, error: cfg.busy, inferenceTimeMs: 0,
                          modelInputSize: 96, modelInputWidth: 96, modelInputHeight: 48,
                          detections: []});
check(clears() === 0 && rects().length === 0,
      'busy: the overlay was redrawn or cleared ' + JSON.stringify(ops));
check(node('ei-detections').innerHTML === listAfterHit,
      'busy: the detection list was replaced with ' + node('ei-detections').innerHTML);
check(window._eiLastDetections.length === 1, 'busy: the last detections were dropped');

// 3. Any other failure still clears the boxes and shows the reason.
ops = [];
window._eiShowDetections({success: false, error: 'No model loaded', detections: []});
check(clears() === 1 && rects().length === 0, 'error: boxes not cleared ' + JSON.stringify(ops));
check(/No model loaded/.test(node('ei-detections').innerHTML),
      'error: reason not shown: ' + node('ei-detections').innerHTML);

// 4. An older reply with only modelInputSize scales square.
ops = [];
window._eiShowDetections({success: true, inferenceTimeMs: 5, modelInputSize: 160,
                          detections: [det('dog', 0, 0, 16, 16)]});
r = rects();
check(r.length === 1 && near(r[0][2], 16 * 2 * 4) && near(r[0][3], 16 * 1.5 * 4),
      'square fallback: ' + JSON.stringify(r));

if (failures.length) { __out('FAIL ' + failures.join('\nFAIL ')); }
else { __out('EI_PAGE PASS'); }
"""


def busy_message() -> str:
    m = re.search(r'kEIBusyMessage\s*=\s*"([^"]+)"', OWNER.read_text())
    if not m:
        raise AssertionError("kEIBusyMessage not found in System_EdgeImpulse.cpp")
    return m.group(1)


@unittest.skipUnless(JS_ENGINE, "a JavaScript engine is required")
class EdgeImpulsePageTests(unittest.TestCase):
    def test_overlay_geometry_and_busy_replies(self):
        regions = extract_js.concat_regions(PAGE)
        self.assertEqual(len(regions), 1)
        script_body = regions[0].text
        self.assertIn("window._eiShowDetections", script_body)
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            script, config, harness = work / "page.js", work / "cfg.json", work / "test.js"
            script.write_text(script_body)
            config.write_text(json.dumps({"busy": busy_message()}))
            harness.write_text(HARNESS)
            result = run_js(harness, [str(script), str(config)])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.splitlines().count("EI_PAGE PASS"), 1,
                         result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
