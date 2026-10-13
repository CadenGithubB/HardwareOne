"""Run the shipping battery page against measured, unavailable and estimated data.

This checks display text and visibility transitions with a small DOM stub, and
that the history table renders /battery.csv as text rather than markup; it is
not a browser layout test or an HTTP/firmware integration test.
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

# /battery.csv is a file a signed-in user can replace, so whatever it contains
# must reach the admin's page as text. This DOM stub records element trees and
# treats any innerHTML assignment as a failure; it is still not a browser.
LOG_CSV = (
    "time,<img src=x onerror=1>pct[%],event\n"
    "1,50,<script>alert(1)</script>\n"
    "2,,<b onmouseover=1>plug</b>\n"
)
LOG_HARNESS = r"""
var ids=JSON.parse(slurp(__argv[1])), nodes={}, loaded=null, innerHtmlWrites=[];
function makeNode(tag){
  var n={tag:tag,style:{},children:[]};
  Object.defineProperty(n,'textContent',{get:function(){return this._text||'';},
    set:function(v){this._text=(v===null||v===undefined)?'':String(v);this.children=[];}});
  Object.defineProperty(n,'innerHTML',{get:function(){return '';},
    set:function(v){innerHtmlWrites.push(tag+' <- '+String(v).slice(0,60));}});
  n.appendChild=function(child){this.children.push(child);};
  return n;
}
ids.forEach(function(id){nodes[id]=makeNode('#'+id);});
function node(x){var n=typeof x==='string'?nodes[x]:x;if(!n)throw new Error('Missing DOM node '+x);return n;}
var document={createElement:function(tag){return makeNode(String(tag).toLowerCase());}};
var pending={then:function(){return this;},catch:function(){return this;}};
var hw={
  $:node,setText:function(x,v){node(x).textContent=v;},
  toggle:function(x,visible){node(x).style.display=visible?'':'none';},
  on:function(){},fetchJSON:function(){return pending;},pollJSON:function(){},
  fetchText:function(){return {then:function(fn){loaded=fn;return pending;}};}
};
var console={log:function(){},warn:function(){},error:function(){}};
eval(slurp(__argv[0]));
if(typeof loaded!=='function')throw new Error('Missing log fetch callback');
loaded(slurp(__argv[2]));

var failures=[];
function check(ok,msg){if(!ok)failures.push(msg);}
var tags=[];
function walk(n){tags.push(n.tag);(n.children||[]).forEach(walk);}
var table=node('bat-table');walk(table);
check(innerHtmlWrites.length===0,'innerHTML assigned: '+innerHtmlWrites.join(' | '));
var allowed={thead:1,tbody:1,tr:1,th:1,td:1};
var stray=tags.slice(1).filter(function(t){return !allowed[t];});
check(stray.length===0,'markup from the log became elements: '+stray.join(','));
var thead=table.children[0],tbody=table.children[1];
var ths=(thead&&thead.children[0]&&thead.children[0].children)||[];
check(ths.length===3,'header cells: '+ths.length+' != 3');
if(ths.length===3){
  check(ths[1].textContent==='<img src=x onerror=1>pct[%]','header text: '+ths[1].textContent);
  check(String(ths[0].style.cssText||'').indexOf('position:sticky')!==-1,'header lost its sticky style');
}
var rows=(tbody&&tbody.children)||[];
check(rows.length===2,'body rows: '+rows.length+' != 2');
if(rows.length===2){
  check(rows[0].children[2].textContent==='<script>alert(1)</script>','cell text: '+rows[0].children[2].textContent);
  check(rows[1].children[1].textContent==='','empty cell: '+JSON.stringify(rows[1].children[1].textContent));
  check(rows[1].children[2].textContent==='<b onmouseover=1>plug</b>','cell text: '+rows[1].children[2].textContent);
}
check(node('bat-log-info').textContent==='2 samples','log info: '+node('bat-log-info').textContent);
if(failures.length)throw new Error(failures.join('\n'));
__out('BATTERY_LOG_TABLE PASS');
"""


@unittest.skipUnless(JS_ENGINE, "a JavaScript engine is required")
class BatteryPageTests(unittest.TestCase):
    def _run_page(self, harness_js, sentinel, extra_files=()):
        page_js, _ = extract_js.page_js(PAGE)
        self.assertIn("function applyStatus", page_js)
        ids = sorted(set(re.findall(r"\bid=['\"]([A-Za-z0-9_-]+)['\"]", PAGE.read_text())))
        self.assertIn("bat-pct", ids)
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            script, markup, harness = work / 'page.js', work / 'ids.json', work / 'test.js'
            script.write_text(page_js)
            markup.write_text(json.dumps(ids))
            harness.write_text(harness_js)
            args = [str(script), str(markup)]
            for name, text in extra_files:
                (work / name).write_text(text)
                args.append(str(work / name))
            result = run_js(harness, args)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.splitlines().count(sentinel), 1,
                         result.stdout + result.stderr)

    def test_validity_and_estimate_transitions(self):
        self._run_page(HARNESS, 'BATTERY_PAGE PASS')

    def test_log_table_renders_csv_as_text(self):
        self._run_page(LOG_HARNESS, 'BATTERY_LOG_TABLE PASS',
                       extra_files=[('battery.csv', LOG_CSV)])


if __name__ == '__main__':
    unittest.main()
