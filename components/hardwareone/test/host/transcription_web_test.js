'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(process.argv[2], 'utf8');
class Element {
  constructor(tag='div') { this.tagName=tag; this.value='';this.textContent='';this.children=[];this.style={};this.listeners={};this.disabled=false;this.open=false;this.checked=false; }
  set innerHTML(_) { throw new Error('Untrusted transcription must not reach innerHTML'); }
  addEventListener(name,fn){this.listeners[name]=fn;}
  dispatch(name){return this.listeners[name]?.({});}
  append(...children){this.children.push(...children);}
  replaceChildren(...children){this.children=children;}
}
function fixture(options={}) {
  const elements={};
  for(const name of ['panel','start','stop','cancel','status','provider','save','saving','live','files','refresh','file-status','file-list','prev','next','preview','mic','mic-note','vocab','vocab-term','vocab-add','vocab-refresh','vocab-status','vocab-list']) elements['transcription-'+name]=new Element();
  elements['transcription-mic'].options=['auto','pdm','g2'].map(value=>Object.assign(new Element('option'),{value}));
  const timers=new Map(), events={}; let serial=0;
  const state={id:'',capture:false,pieces:[],lastAck:'',starts:0,acks:0,cancels:0,continuous:true,canSave:true,save:false,calls:[],list:[],fileText:'',fileCancels:0,epoch:42,micSource:'auto',micPdm:true,micG2:false,vocabulary:true,vocab:[['HardwareOne','hardwareone'],['ESP32','esp']],speakers:null,speakersAvailable:false,draft:'',...options};
  const base=()=>({success:true,session:state.epoch,available:true,canSave:state.canSave,continuous:state.continuous,savePreference:state.save,roots:['/stt/u7','/sd/stt/u7'],id:state.id,busy:state.capture||state.pieces.length>0,active:state.capture||state.pieces.length>0,done:!!state.id&&!state.capture&&!state.pieces.length,captureActive:state.capture,transcriptEnabled:state.save,transcriptSaved:false,micSource:state.micSource,micPdm:state.micPdm,micG2:state.micG2,vocabulary:state.vocabulary});
  const response=(data,status=200)=>({ok:status>=200&&status<300,status,json:async()=>data});
  const stream=(text,status=200)=>{
    let offset=0;const bytes=new TextEncoder().encode(text);
    return {ok:status===200,status,body:{getReader(){return {async read(){if(offset>=bytes.length)return {done:true};const value=bytes.subarray(offset,offset+1024);offset+=value.length;return {done:false,value};},async cancel(){state.fileCancels++;}};}}};
  };
  async function fetcher(url, opts={}) {
    const form=Object.fromEntries(new URLSearchParams(opts.body||''));
    state.calls.push({url,form,opts});
    if(state.fetchOverride){const result=state.fetchOverride(url,opts,form);if(result!==undefined)return result;}
    if(url==='/api/transcription?identity=1')return response({success:true,session:state.epoch});
    if(url.startsWith('/api/files/list'))return stream(JSON.stringify({success:true,files:state.list}));
    if(url.startsWith('/api/files/read'))return stream(state.fileText);
    if(url==='/api/transcription/ack') {
      state.acks++;const key=[form.id,form.sequence,form.offset,form.length].join(':');
      if(key===state.lastAck)return response({success:true,id:state.id});
      const piece=state.pieces[0];
      if(!piece||piece.sequence!==Number(form.sequence)||piece.offset!==Number(form.offset))return response({success:false,error:'Receipt changed'},409);
      state.lastAck=key;state.pieces.shift();
      if(state.loseAck){state.loseAck=false;throw new Error('Lost ACK response');}
      return response({success:true,id:state.id});
    }
    if(opts.method==='POST') {
      if(form.action==='start'){state.starts++;state.id='1122334455667788';state.capture=true;if(state.loseStart){state.loseStart=false;throw new Error('Lost start reply');}}
      if(form.action==='stop')state.capture=false;
      if(form.action==='cancel'){state.cancels++;state.capture=false;state.pieces=[];}
      return response({success:true,id:state.id});
    }
    const data=base();
    if(state.pieces.length){const p=state.pieces[0];data.textPending=true;data.sttText=p.text;if(p.display)data.display=p.display;data.receipt={sequence:p.sequence,offset:p.offset,length:new TextEncoder().encode(p.text).length};}
    if(state.speakers){data.speakers=state.speakers;data.speakersAvailable=state.speakersAvailable;}
    if(state.draft){data.draft=state.draft;data.draftVersion=1;}
    return response(data);
  }
  const context={document:{getElementById:id=>elements[id]||null,createElement:tag=>new Element(tag)},window:{addEventListener:(name,fn)=>events[name]=fn},fetch:fetcher,
    hw:{postFormText:async(url,form)=>{state.calls.push({url,form});
      if(form.cmd.startsWith('micsource '))state.micSource=form.cmd.slice(10);
      else if(form.cmd==='sttvocab list')return 'OK: '+state.vocab.length+' words in /sd/STT Models/user_vocab.tsv (DISPLAY<TAB>spoken)'+state.vocab.map(([d,s])=>'\n'+d+'\t'+s).join('');
      else if(form.cmd.startsWith('sttvocab add ')){const term=form.cmd.slice(13);if(/\s|bad/.test(term))return 'Error: Display must be one printable ASCII term without | or spaces; the previous vocabulary remains active';const bar=term.indexOf('|');state.vocab.push([bar>=0?term.slice(0,bar):term,bar>=0?term.slice(bar+1):term.toLowerCase()]);return 'OK: added '+term;}
      else if(form.cmd.startsWith('sttvocab remove ')){state.vocab=state.vocab.filter(([,s])=>s!==form.cmd.slice(16));return 'OK: removed';}
      else state.save=form.cmd.endsWith('1');return 'OK';}},
    setTimeout:(fn,delay)=>{const id=++serial;timers.set(id,{fn,delay});return id;},clearTimeout:id=>timers.delete(id),
    AbortController,TextEncoder,TextDecoder,URLSearchParams,console};
  vm.runInNewContext(source,context);
  const el=name=>elements['transcription-'+name];
  const tick=async()=>{const entry=[...timers].filter(([,t])=>t.delay<12000).sort((a,b)=>a[1].delay-b[1].delay)[0];assert(entry,'expected scheduled poll');timers.delete(entry[0]);await entry[1].fn();};
  const settle=async()=>{for(let i=0;i<12;i++)await new Promise(resolve=>setImmediate(resolve));};
  return {state,el,timers,tick,settle,events,response,stream,async open(){el('panel').open=true;el('panel').dispatch('toggle');await tick();},async start(){el('start').onclick();await tick();}};
}
(async()=>{
  // No unsolicited start; same session is recovered after reload.
  let f=fixture();assert.equal(f.state.calls.length,0);await f.open();assert.equal(f.el('start').disabled,false);assert.match(f.el('provider').textContent,/On-device/);assert.equal(f.state.starts,0);
  await f.start();assert.equal(f.state.starts,1);
  f.state.pieces=[{sequence:1,offset:0,text:'hello '},{sequence:1,offset:6,text:'world'},{sequence:2,offset:0,text:'second phrase'}];f.state.loseAck=true;
  await f.tick();assert.equal(f.el('live').value,'hello ');
  await f.tick();assert.match(f.el('status').textContent,/Lost ACK/);assert.equal(f.el('live').value,'hello ');
  await f.tick();assert.equal(f.state.acks,2);assert.equal(f.el('live').value,'hello ');
  await f.tick();assert.equal(f.el('live').value,'hello world');await f.tick();
  f.el('panel').open=false;f.el('panel').dispatch('toggle');await f.tick();assert.equal(f.el('live').value,'hello world second phrase');await f.tick();assert.equal(f.state.pieces.length,0);
  f.el('stop').onclick();await f.tick();await f.tick();assert.equal(f.el('stop').disabled,true);assert.equal([...f.timers.values()].filter(t=>t.delay<12000).length,0,'closed finished panel stops polling');
  // Recover an admitted start when its HTTP reply was lost; never start twice.
  f=fixture({loseStart:true});await f.open();await f.start();assert.equal(f.state.starts,1);await f.tick();assert.equal(f.el('stop').disabled,false);assert.equal(f.state.starts,1);
  // A finite view trims old preview text, yet ACKs every accepted piece.
  f=fixture();await f.open();await f.start();for(let i=1;i<=55;i++){f.state.pieces.push({sequence:i,offset:0,text:'x'.repeat(256)});await f.tick();await f.tick();}assert.equal(f.el('live').value.length,12000);assert.equal(f.state.acks,55);
  // Another consumer advanced the receipt; reconcile instead of retrying forever.
  f=fixture();await f.open();await f.start();f.state.pieces=[{sequence:1,offset:0,text:'accepted'}];await f.tick();f.state.pieces=[];await f.tick();await f.tick();assert.equal(f.state.acks,1);
  // Late text after page exit is discarded, and cancellation names the old lease.
  f=fixture();await f.open();await f.start();let resolve;
  f.state.fetchOverride=(url,opts)=>!opts.method?new Promise(r=>resolve=r):undefined;
  const inflight=f.tick();await Promise.resolve();f.events.pagehide();resolve(f.response({success:true,id:f.state.id,active:true,textPending:true,sttText:'secret',receipt:{sequence:1,offset:0,length:6}}));await inflight;assert.equal(f.el('live').value,'');assert.equal(f.state.cancels,1);
  // Admission may finish after pagehide: cancel its returned exact lease too.
  f=fixture();await f.open();let startResolve;f.state.fetchOverride=(url,opts,form)=>form.action==='start'?new Promise(r=>startResolve=r):undefined;
  f.el('start').onclick();const pending=f.tick();await Promise.resolve();f.events.pagehide();startResolve(f.response({success:true,id:'1122334455667788'}));await pending;assert.equal(f.el('live').value,'');assert.equal(f.state.cancels,1);
  // Expiry never leaves text or file previews behind; malformed error JSON too.
  f=fixture();await f.open();await f.start();f.el('live').value='private';f.el('preview').textContent='private file';f.state.fetchOverride=()=>({ok:false,status:401,json:async()=>{throw new Error('Not JSON');}});await f.tick();assert.equal(f.el('live').value,'');assert.equal(f.el('preview').textContent,'');assert.equal(f.el('start').disabled,true);
  // Provider label is truthful, and non-admins cannot mutate the preference.
  f=fixture({continuous:false,canSave:false});await f.open();assert.match(f.el('provider').textContent,/one recording/);assert.equal(f.el('save').disabled,true);f.el('save').checked=true;await f.el('save').onchange();assert.equal(f.state.calls.filter(c=>c.url==='/api/cli').length,0);
  // Microphone choice: unreachable sources are disabled, a change goes through micsource, and it locks while a session runs.
  f=fixture();await f.open();const mic=f.el('mic');assert.equal(mic.disabled,false);assert.equal(mic.value,'auto');
  assert.deepEqual(mic.options.map(o=>o.disabled),[false,false,true]);
  mic.value='pdm';await mic.onchange.call(mic);assert.equal(f.state.micSource,'pdm');assert.equal(f.state.calls.filter(c=>c.url==='/api/cli'&&c.form.cmd==='micsource pdm').length,1);
  await f.start();await f.tick();assert.equal(mic.disabled,true);assert.match(f.el('mic-note').textContent,/Stop the session/);
  // Rendering uses DOM text only; entries and previews have explicit bounds.
  f=fixture();await f.open();f.state.list=[{name:'../escape.txt',type:'file',perms:1},{name:'<img onerror=boom>.txt',type:'file',perms:1},...Array.from({length:205},(_,i)=>({name:`2026-${String(i).padStart(3,'0')}.txt`,type:'file',perms:1}))];
  f.el('files').open=true;f.el('files').dispatch('toggle');await f.settle();assert.equal(f.el('file-list').children.length,10);assert.match(f.el('file-status').textContent,/200/);assert.equal(f.el('next').disabled,false);assert(!f.el('file-list').children.some(row=>row.children[0].textContent.includes('escape')));
  f.state.fileText='A'.repeat(18000);await f.el('file-list').children[0].children[1].onclick();assert.match(f.el('preview').textContent,/limited to 16 KiB/);assert(f.el('preview').textContent.length<16500);assert(f.state.fileCancels>0);
  const first=f.el('file-list').children[0].children[0].textContent;f.el('next').onclick();assert.notEqual(f.el('file-list').children[0].children[0].textContent,first);
  // A cookie epoch change during a saved-file read cannot republish old text.
  f=fixture();await f.open();f.state.list=[{name:'one.txt',type:'file',perms:1}];f.el('files').open=true;f.el('files').dispatch('toggle');await f.settle();
  f.state.fileText='old account private text';f.state.fetchOverride=(url)=>{if(url.startsWith('/api/files/read')){f.state.epoch=43;return f.stream(f.state.fileText);}return undefined;};
  await f.el('file-list').children[0].children[1].onclick();assert.equal(f.el('preview').textContent,'');assert.equal(f.el('file-list').children.length,0);
  // The panel can expose files before initial metadata arrives; it loads once roots arrive.
  f=fixture();f.el('files').open=true;f.el('files').dispatch('toggle');await f.open();await f.settle();assert(f.state.calls.some(c=>c.url.startsWith('/api/files/list')));
  // Display copies are shown while receipts acknowledge the raw bytes; the draft is a ~ line that a final replaces.
  f=fixture();await f.open();await f.start();f.state.pieces=[{sequence:1,offset:0,text:'hardwareone said ',display:'HardwareOne said '},{sequence:1,offset:17,text:'hello',display:'hello'}];f.state.draft='more wor';
  await f.tick();assert.equal(f.el('live').value,'HardwareOne said \n~ more wor');assert.deepEqual(f.state.calls.filter(c=>c.url==='/api/transcription/ack').length,0);
  await f.tick();assert.equal(f.state.acks,1);assert.equal(f.state.pieces[0].offset,17);await f.tick();assert.equal(f.el('live').value,'HardwareOne said hello\n~ more wor');
  f.state.draft='';await f.tick();await f.tick();assert.equal(f.el('live').value,'HardwareOne said hello');
  // Speaker turns group like the lens page: known speakers flow into one capitalized paragraph, unlabelled text keeps its own ?: line, late labels regroup.
  f=fixture();await f.open();await f.start();f.state.speakersAvailable=true;f.state.speakers=[];
  f.state.pieces=[{sequence:1,offset:0,text:'alpha'},{sequence:2,offset:0,text:'beta'},{sequence:3,offset:0,text:'gamma'}];
  for(let i=0;i<6;i++)await f.tick();assert.equal(f.el('live').value,'?: alpha\n?: beta\n?: gamma');
  f.state.speakers=[{sequence:1,speaker:0,ready:true},{sequence:3,speaker:0,ready:true}];await f.tick();assert.equal(f.el('live').value,'Person 1: Alpha\n?: beta\nPerson 1: Gamma');
  f.state.speakers=[{sequence:2,speaker:0,ready:true}];await f.tick();assert.equal(f.el('live').value,'Person 1: Alpha beta gamma');
  f.state.speakers=[{sequence:2,speaker:1,ready:true}];await f.tick();assert.equal(f.el('live').value,'Person 1: Alpha\nPerson 2: Beta\nPerson 1: Gamma');
  f.state.speakers=[{sequence:4,speaker:-1,ready:true},{sequence:5,speaker:99,ready:true},{sequence:6,speaker:2,ready:false}];await f.tick();assert.equal(f.el('live').value,'Person 1: Alpha\nPerson 2: Beta\nPerson 1: Gamma');
  // A different session id clears the grouped text (unsafe speaker values above were never shown as a person).
  f.state.fetchOverride=(url,opts)=>!opts.method?f.response({...f.state,success:true,session:f.state.epoch,id:'8877665544332211',active:true,valid:true}):undefined;await f.tick();assert.equal(f.el('live').value,'');
  // Vocabulary editor: list, add and remove go through sttvocab; invalid terms report the device's reason; non-admins only read.
  f=fixture();await f.open();assert.equal(f.el('vocab').hidden,false);f.el('vocab').open=true;f.el('vocab').dispatch('toggle');await f.settle();
  assert.equal(f.el('vocab-list').children.length,2);assert.equal(f.el('vocab-list').children[1].children[0].textContent,'ESP32 (esp)');assert.match(f.el('vocab-status').textContent,/2 term/);
  f.el('vocab-term').value='OpenAI';await f.el('vocab-add').onclick();await f.settle();assert.equal(f.state.calls.filter(c=>c.form&&c.form.cmd==='sttvocab add OpenAI').length,1);assert.equal(f.el('vocab-list').children.length,3);assert.equal(f.el('vocab-term').value,'');
  f.el('vocab-term').value='bad term';await f.el('vocab-add').onclick();await f.settle();assert.match(f.el('vocab-status').textContent,/without spaces/);assert.equal(f.state.calls.filter(c=>c.form&&c.form.cmd&&c.form.cmd.startsWith('sttvocab add bad')).length,0);
  f.el('vocab-term').value='badword';await f.el('vocab-add').onclick();await f.settle();assert.match(f.el('vocab-status').textContent,/Error: Display must/);
  await f.el('vocab-list').children[1].children[1].onclick();await f.settle();assert.equal(f.state.calls.filter(c=>c.form&&c.form.cmd==='sttvocab remove esp').length,1);assert.equal(f.el('vocab-list').children.length,2);
  f=fixture({canSave:false});await f.open();f.el('vocab').open=true;f.el('vocab').dispatch('toggle');await f.settle();assert.equal(f.el('vocab-add').hidden,true);assert.equal(f.el('vocab-list').children[0].children.length,1);
  f=fixture({vocabulary:false});await f.open();assert.equal(f.el('vocab').hidden,true);
  console.log('Transcription JS: actual script passed retries, bounded views, speaker turns, drafts, vocabulary, collapse, exit, auth and file rendering');
})().catch(error=>{console.error(error);process.exitCode=1;});
