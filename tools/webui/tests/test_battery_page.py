"""Run the shipping battery page against measured, unavailable and estimated data.

This checks display text and visibility transitions with a small DOM stub; it
is not a browser layout test or an HTTP/firmware integration test.
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
PAGE = ROOT / "components/hardwareone/WebPage_Battery.cpp"
HARNESS = r"""
var ids=JSON.parse(slurp(__argv[1])), nodes={}, status=null;
ids.forEach(function(id){nodes[id]={style:{},textContent:''};});
function node(x){var n=typeof x==='string'?nodes[x]:x;if(!n)throw new Error('Missing DOM node '+x);return n;}
var pending={then:function(){return this;},catch:function(){return this;}};
var hw={
  $:node,setText:function(x,v){node(x).textContent=v;},
  toggle:function(x,visible){node(x).style.display=visible?'':'none';},
  on:function(){},fetchJSON:function(){return pending;},fetchText:function(){return pending;},
  pollJSON:function(url,interval,fn){status=fn;}
};
var failures=[];
function eq(id,want){var got=node(id).textContent;if(got!==want)failures.push(id+': '+got+' != '+want);}
function shown(id,want){var got=node(id).style.display!=='none';if(got!==want)failures.push(id+' visibility '+got+' != '+want);}
eval(slurp(__argv[0]));
if(typeof status!=='function')throw new Error('Missing status poll callback');

// A P4 measures its charger BAT node; that voltage cannot prove USB, cell
// presence or charging. Keep the useful reading, label SOC as approximate.
status({voltageAvailable:true,voltageValid:true,voltage:3.8,percentageValid:true,
        percentage:50,percentageEstimated:true,percentageSource:'voltage',
        present:null,usbKnown:false,usbPresent:null,chargingKnown:false,
        charging:null,rateValid:false,status:'Good'});
eq('bat-volt','3.800');eq('bat-pct','~50.0');eq('bat-src','Unknown');
eq('bat-charging','Unknown');shown('bat-estimate',true);shown('bat-rate-wrap',false);

// A gauge estimate is not a divider estimate. USB presence does not mean full.
status({voltageAvailable:true,voltageValid:true,voltage:3.9,percentageValid:true,
        percentage:63,percentageEstimated:true,percentageSource:'fuelgauge',
        present:true,usbKnown:true,usbPresent:true,chargingKnown:true,
        charging:true,chargingEstimated:true,rateValid:true,ratePctPerHr:8,
        etaMinutes:null,status:'Charging'});
eq('bat-src','USB connected');eq('bat-charging','Yes (estimated)');
eq('bat-pct','~63.0');eq('bat-rate','8.00');shown('bat-estimate',false);
shown('bat-rate-wrap',true);shown('bat-eta-wrap',false);

// A failed/stale read must replace previously visible values with unknowns.
status({voltageAvailable:true,voltageValid:false,voltage:null,percentageValid:false,
        percentage:null,usbKnown:false,chargingKnown:false,rateValid:false,
        present:null,status:'Unknown'});
eq('bat-volt','--');eq('bat-pct','--');eq('bat-src','Unknown');
eq('bat-charging','Unknown');shown('bat-rate-wrap',false);shown('bat-estimate',false);
eq('bat-absent','Battery voltage is unavailable.');

// A measured zero is distinct from an absent sensor. It does not become 0%.
status({voltageAvailable:true,voltageValid:true,voltage:0,percentageValid:false,
        percentage:null,usbKnown:false,chargingKnown:false,rateValid:false});
eq('bat-volt','0.000');eq('bat-pct','--');eq('bat-src','Unknown');

// Conversely, a valid empty SOC is allowed to display zero.
status({voltageAvailable:true,voltageValid:true,voltage:3.2,percentageValid:true,
        percentage:0,percentageEstimated:true,percentageSource:'fuelgauge',
        present:true,usbKnown:true,usbPresent:false,chargingKnown:true,
        charging:false,chargingEstimated:true,rateValid:false});
eq('bat-pct','~0.0');eq('bat-src','Battery');

status({voltageAvailable:false,voltageValid:false,voltage:null,percentageValid:false,
        percentage:null,present:null,usbKnown:false,chargingKnown:false,rateValid:false});
eq('bat-pct','--');eq('bat-volt','--');eq('bat-src','Unknown');
eq('bat-absent','Battery monitoring is unavailable on this board.');
shown('bat-absent',true);
if(failures.length)throw new Error(failures.join('\n'));
__out('BATTERY_PAGE PASS');
"""


@unittest.skipUnless(JS_ENGINE, "a JavaScript engine is required")
class BatteryPageTests(unittest.TestCase):
    def test_validity_and_estimate_transitions(self):
        page_js, _ = extract_js.page_js(PAGE)
        self.assertIn("function applyStatus", page_js)
        ids = sorted(set(re.findall(r"\bid=['\"]([A-Za-z0-9_-]+)['\"]", PAGE.read_text())))
        self.assertIn("bat-pct", ids)
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            script, markup, harness = work / 'page.js', work / 'ids.json', work / 'test.js'
            script.write_text(page_js)
            markup.write_text(json.dumps(ids))
            harness.write_text(HARNESS)
            result = run_js(harness, [str(script), str(markup)])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.splitlines().count('BATTERY_PAGE PASS'), 1,
                         result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
