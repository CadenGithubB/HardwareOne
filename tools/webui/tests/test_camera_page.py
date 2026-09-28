"""Execute the shipping camera controls against different backend capabilities.

The DOM is minimal: this checks selectable resolutions, disabled controls,
command dispatch and error feedback, not browser rendering or real hardware.
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
PAGE = ROOT / "components/hardwareone/System_Camera_DVP_Web.h"
HARNESS = r"""
var nodes={}, ids=JSON.parse(slurp(__argv[1])), sent=[], response=null;
function makeNode(id) {
  var item={id:id,style:{},type:'range',disabled:false,children:[],parentElement:{style:{}},value:'',title:''};
  Object.defineProperty(item,'textContent',{get:function(){return this._text||'';},set:function(v){this._text=String(v);this.children=[];}});
  item.appendChild=function(child){this.children.push(child);};
  item.addEventListener=function(){};
  return item;
}
ids.forEach(function(id){nodes[id]=makeNode(id);});
function node(x){var n=typeof x==='string'?nodes[x]:x;if(!n)throw new Error('Missing DOM node '+x);return n;}
var pending={then:function(){return this;},catch:function(){return this;}};
var document={activeElement:null,createElement:function(){return makeNode('option');},addEventListener:function(){}};
var window={};
var console={log:function(){},warn:function(){},error:function(){}};
var hw={
  $:node,setText:function(x,v){node(x).textContent=v;},
  hide:function(x){node(x).style.display='none';},on:function(){},
  fetchJSON:function(){return pending;},
  postFormText:function(url,body){sent.push(body.cmd);return {then:function(fn){response=fn;return pending;}};}
};
var failures=[];
function check(ok,message){if(!ok)failures.push(message);}
function disabled(id,want){check(node(id).disabled===want,id+' disabled='+node(id).disabled+' expected '+want);}
function optionIds(){return node('camera-framesize').children.map(function(o){return Number(o.value);}).join(',');}
eval(slurp(__argv[0]));

// Before the capability reply, even a synthetic click cannot send a control.
applyCameraAdjustment('camerahmirror','toggle');
check(sent.length===0,'unknown capabilities dispatched a command');

var p4={enabled:false,model:'OV2710',backend:'p4-csi',sourceWidth:1280,sourceHeight:720,
        controls:[],resolutions:[{id:0,name:'qvga',width:320,height:240},{id:11,name:'hd',width:1280,height:720}],
        requestedFramesize:11,framesize:0,quality:12};
__cameraApplyCapabilities(p4);
check(optionIds()==='0,11','P4 picker leaked unsupported resolutions: '+optionIds());
check(node('camera-framesize').value==='11','stopped camera did not select saved resolution');
disabled('camera-brightness',true);disabled('camera-exposure',true);disabled('camera-quality',false);
disabled('btn-hmirror',true);disabled('btn-vflip',true);disabled('btn-rotate',true);
check(node('camera-description').textContent.indexOf('1280x720')!==-1,'source dimensions missing');
// A stopped camera can have a new saved size and stale last-capture geometry.
// A running camera reports actual geometry, even if a portable saved preference
// differs. Unsupported preferences must not become selectable or pick a fallback.
p4.requestedFramesize=0;p4.framesize=11;__cameraApplyCapabilities(p4);
check(node('camera-framesize').value==='0','stopped resolution edit reverted to old capture size');
p4.enabled=true;__cameraApplyCapabilities(p4);
check(node('camera-framesize').value==='11','running camera did not select actual resolution');
p4.enabled=false;p4.requestedFramesize=5;__cameraApplyCapabilities(p4);
check(node('camera-framesize').value==='','unsupported saved size appeared selected');
check(optionIds()==='0,11','unsupported saved size was added to picker');
check(node('camera-framesize').title.indexOf('unavailable')!==-1,'unsupported saved size lacks explanation');
p4.enabled=true;p4.framesize=5;__cameraApplyCapabilities(p4);
check(node('camera-framesize').value==='','unsupported actual size appeared selected');
p4.enabled=false;p4.requestedFramesize=11;p4.framesize=0;__cameraApplyCapabilities(p4);
check(node('camera-framesize').title==='','supported resolution kept stale warning');
document.activeElement=node('camera-framesize');node('camera-framesize').value='0';
__cameraApplyCapabilities(p4);
check(node('camera-framesize').value==='0','refresh interrupted resolution selection');
document.activeElement=null;__cameraApplyCapabilities(p4);

applyCameraAdjustment('camerabrightness',1);
applyCameraAdjustment('cameraframesize',5);
applyCameraAdjustment('camerarotate','toggle');
check(sent.length===0,'unsupported controls or resolution reached CLI');
applyCameraAdjustment('cameraframesize',11);
check(sent.join(',')==='cameraframesize 11','supported P4 resolution was not dispatched');
response('Error: capture is busy');
check(node('camera-adjustment-result').textContent==='Error: capture is busy','CLI failure was hidden');

// A backend with only mirror cannot truthfully offer rotate. Rebuild the same
// DOM on a capability change instead of keeping stale P4 options/disabled state.
var s3={enabled:true,model:'OV3660',controls:['brightness','contrast','exposureLevel','hmirror'],
        resolutions:[{id:7,name:'qqvga',width:160,height:120},{id:0,name:'qvga',width:320,height:240},{id:5,name:'uxga',width:1600,height:1200}],
        requestedFramesize:7,framesize:0,quality:20};
__cameraApplyCapabilities(s3);
check(optionIds()==='7,0,5','S3 picker did not replace P4 options');
check(node('camera-framesize').value==='0','S3 running camera selected saved size instead of actual');
disabled('camera-brightness',false);disabled('camera-exposure',false);
disabled('camera-saturation',true);disabled('btn-hmirror',false);disabled('btn-rotate',true);
applyCameraAdjustment('camerahmirror','toggle');
check(sent[sent.length-1]==='camerahmirror on','supported mirror did not dispatch');
s3.controls.push('vflip');__cameraApplyCapabilities(s3);disabled('btn-rotate',false);

// Refresh does not jump an actively dragged quality slider.
document.activeElement=node('camera-quality');node('camera-quality').value=33;
__cameraApplyCapabilities(s3);check(node('camera-quality').value===33,'refresh interrupted quality edit');
document.activeElement=null;__cameraApplyCapabilities(s3);
check(node('camera-quality').value===20,'quality did not refresh after edit');

__cameraApplyCapabilities(null);
check(optionIds()==='','lost capability data left stale options');
disabled('camera-quality',true);disabled('camera-framesize',true);disabled('btn-hmirror',true);
var before=sent.length;applyCameraAdjustment('cameraquality',12);
check(sent.length===before,'unknown capabilities dispatched quality');
if(failures.length)throw new Error(failures.join('\n'));
__out('CAMERA_PAGE PASS');
"""


@unittest.skipUnless(JS_ENGINE, "a JavaScript engine is required")
class CameraPageTests(unittest.TestCase):
    def test_backend_capabilities_and_commands(self):
        regions = extract_js.concat_regions(PAGE)
        self.assertEqual(len(regions), 1)
        script_body = regions[0].text
        self.assertIn("function __cameraApplyCapabilities", script_body)
        ids = sorted(set(re.findall(r"\bid=['\"]([A-Za-z0-9_-]+)['\"]", PAGE.read_text())))
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            script, markup, harness = work / 'page.js', work / 'ids.json', work / 'test.js'
            script.write_text(script_body)
            markup.write_text(json.dumps(ids))
            harness.write_text(HARNESS)
            result = run_js(harness, [str(script), str(markup)])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.splitlines().count('CAMERA_PAGE PASS'), 1,
                         result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
