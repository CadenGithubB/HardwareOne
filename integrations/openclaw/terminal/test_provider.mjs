// One focused integration fixture: real provider + real serialized Gateway
// client against an in-process fake WebSocket peer. No installs or network.
import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createPublicKey, verify } from 'node:crypto';
import { Gateway, createIdentity } from './gateway.mjs';
import { createOpenClawProvider } from './provider.mjs';
import { createDeviceApi } from './device-api.mjs';

test('official provider: main-only resume, exact runs, offered decisions and uncertain disconnect', async () => {
  const dir = mkdtempSync(join(tmpdir(), 'hw1-terminal-test-'));
  const tokenFile = join(dir, 'token'), identityFile = join(dir, 'identity');
  writeFileSync(tokenFile, 'fixture-token', { mode: 0o600 }); createIdentity(identityFile);
  const calls = [], emitted = [], key = 'agent:main:main';
  let socket, inFlight = null, historyHook = null, listRows = null, historyText = 'Earlier answer', rejectSend = false;
  const approval = { id:'exec:fixture', status:'pending', expiresAtMs:Date.now()+60000,
    presentation:{kind:'exec',commandText:'echo fixture',host:'sandbox',allowedDecisions:['allow-once','deny']} };
  class FakeSocket extends EventTarget {
    readyState = 1;
    constructor(url) {
      super(); assert.equal(url,'ws://127.0.0.1:18789'); socket = this;
      queueMicrotask(() => this.event('connect.challenge',{nonce:'fixture-nonce',ts:Date.now()}));
    }
    event(event,payload) { this.dispatchEvent(new MessageEvent('message',{data:JSON.stringify({type:'event',event,payload})})); }
    send(raw) {
      const r = JSON.parse(raw); calls.push(r);
      let payload;
      switch (r.method) {
        case 'connect': {
          assert.equal(r.params.minProtocol,4); assert.equal(r.params.maxProtocol,4);
          assert.deepEqual(r.params.scopes,['operator.read','operator.write','operator.approvals','operator.questions']);
          assert.equal(r.params.client.mode,'ui'); assert.equal(r.params.auth.token,'fixture-token');
          const d = r.params.device;
          const signed = ['v3',d.id,'gateway-client','ui','operator',r.params.scopes.join(','),String(d.signedAt),'fixture-token',d.nonce,process.platform,''].join('|');
          assert(verify(null,Buffer.from(signed),createPublicKey({key:{kty:'OKP',crv:'Ed25519',x:d.publicKey},format:'jwk'}),Buffer.from(d.signature,'base64url')));
          payload={type:'hello-ok',protocol:4,server:{version:'2026.9.4'},auth:{scopes:r.params.scopes},features:{methods:[
            'sessions.list','sessions.subscribe','sessions.messages.subscribe','chat.history','chat.send','chat.abort','approval.get','approval.resolve','question.list','question.resolve']}}; break;
        }
        case 'sessions.list':
          assert.equal(r.params.agentId,'main'); payload={sessions:listRows || [{key,label:'Mingus',updatedAt:1},{key:'agent:devlab:main',label:'Forbidden'}]}; break;
        case 'sessions.subscribe': payload={subscribed:true}; break;
        case 'sessions.messages.unsubscribe': payload={subscribed:false,key:r.params.key}; break;
        case 'sessions.messages.subscribe': payload={subscribed:true,key:r.params.key,approvalReplay:{approvals:[]}}; break;
        case 'question.list': payload={questions:[]}; break;
        case 'chat.history':
          assert.equal(r.params.sessionKey,key); payload={messages:[{id:'m1',role:'assistant',content:[{type:'text',text:historyText}]}],...(inFlight?{inFlightRun:inFlight}:{})};
          if(historyHook) {const hook=historyHook;historyHook=null;hook();} break;
        case 'chat.send':
          // Match the pinned Gateway normalizeChatSendRequest admission rule:
          // system/suppression fields are forbidden without operator.admin.
          if (rejectSend || r.params.suppressCommandInterpretation) {
            queueMicrotask(()=>this.dispatchEvent(new MessageEvent('message',{data:JSON.stringify({type:'res',id:r.id,ok:false,
              error:{code:'INVALID_REQUEST',message:'PRIVATE UPSTREAM DETAILS MUST NOT BE LOGGED'}})})));
            return;
          }
          assert.equal(r.params.sessionKey,key); assert.equal(r.params.agentId,'main');
          assert.equal(r.params.deliver,false);
          assert.deepEqual(Object.keys(r.params).sort(),['sessionKey','agentId','message','idempotencyKey','deliver'].sort());
          payload={runId:r.params.idempotencyKey,status:'started'};break;
        case 'chat.abort': payload={aborted:true};break;
        case 'approval.get': payload={approval};break;
        case 'approval.resolve': assert.equal(r.params.decision,'allow-once');payload={applied:true,approval:{...approval,status:'allowed',decision:'allow-once'}};break;
        case 'question.resolve': payload={status:'answered'};break;
        default: throw new Error(`Unexpected method ${r.method}`);
      }
      queueMicrotask(()=>this.dispatchEvent(new MessageEvent('message',{data:JSON.stringify({type:'res',id:r.id,ok:true,payload})})));
    }
    close() { if(this.readyState===3)return;this.readyState=3;this.dispatchEvent(new Event('close')); }
  }
  const client = new Gateway({tokenFile,identityFile,Socket:FakeSocket});
  const provider = createOpenClawProvider((id,msg)=>emitted.push({id,...msg}),{client});
  try {
    const list = await provider.listSessions(); assert.equal(list.length,1);
    const id = list[0].id; assert.match(id,/^hwo_[a-f0-9]{64}$/);
    const histories = await Promise.all([provider.getHistory(id,10), provider.getHistory(id,10)]);
    assert.equal(histories[0][0].text,'Earlier answer');
    assert.equal(calls.filter(c=>c.method==='sessions.messages.subscribe').length,1);
    await assert.rejects(provider.prompt('agent:main:main','bad'),/Unknown/);
    await assert.rejects(provider.prompt(id,'bad','/tmp'),/working directory/);
    for (const text of [' /status','\n!whoami','\u0001 /status',' Please   stop!! ','停止。',"Stop don’t do anything!"])
      await assert.rejects(provider.prompt(id,text),/chat text only/);
    assert.equal(calls.filter(c=>c.method==='chat.send').length,0);
    const requestId = 'a'.repeat(32);
    const [a,b] = await Promise.all([provider.prompt(id,'Hello',undefined,requestId),provider.prompt(id,'Hello',undefined,requestId)]);
    assert.equal(a.runId,requestId); assert.deepEqual(a,b); assert.equal(calls.filter(c=>c.method==='chat.send').length,1);
    socket.event('chat',{sessionKey:key,runId:'wrong-run',seq:1,state:'delta',deltaText:'wrong'});
    assert(!emitted.some(e=>e.text==='wrong'));
    socket.event('chat',{sessionKey:key,runId:requestId,seq:1,state:'delta',deltaText:'Hello',message:{content:[{type:'text',text:'Hello'}]}});
    socket.event('chat',{sessionKey:key,runId:requestId,seq:1,state:'delta',deltaText:'duplicate'});
    assert.equal(emitted.filter(e=>e.type==='text_delta').length,1);
    const tool={sessionKey:key,runId:requestId,seq:2,stream:'tool',data:{phase:'start',name:'hardwareone_cli',toolCallId:'tool1'}};
    socket.event('agent',tool);socket.event('session.tool',tool);
    assert.equal(emitted.filter(e=>e.type==='tool_start').length,1);
    socket.event('agent',{...tool,seq:3,data:{...tool.data,phase:'update',partialResult:{content:[{type:'text',text:'Progress'}]}}});
    socket.event('agent',{...tool,seq:4,data:{...tool.data,phase:'result',isError:true,result:{content:[{type:'text',text:'Tool failed'}]}}});
    assert.equal(emitted.findLast(e=>e.type==='tool_end').text,'Tool failed');
    assert.equal(emitted.findLast(e=>e.type==='tool_end').isError,true);
    assert.equal((await provider.getDeviceHistorySnapshot(id,10)).recentToolEvents[0].runId,requestId);
    // Only exact run facts become activity, plan progress and statistics. No
    // reasoning body, session aggregate cost, or inferred input tokens leak in.
    const startedAt=Date.now()-6000;
    const agent=(seq,stream,data,runId=requestId)=>socket.event('agent',{sessionKey:key,runId,seq,stream,data});
    agent(5,'lifecycle',{phase:'start',startedAt});
    agent(6,'thinking',{delta:'private reasoning text'});
    assert.equal(provider.getDeviceSnapshot(id).activity,'thinking');
    agent(7,'plan',{phase:'update',steps:[{step:'Read status',status:'completed'},{step:'Report result',status:'in_progress'}]});
    agent(8,'usage',{outputTokens:123,inputTokens:999,costUsd:7});
    agent(9,'assistant',{delta:'Answer'});
    assert.equal(provider.getDeviceSnapshot(id).activity,'responding');
    agent(20,'usage',{outputTokens:99999},'wrong-run');
    agent(8,'usage',{outputTokens:88888}); // Duplicate/older sequence.
    assert.deepEqual(emitted.findLast(e=>e.type==='task_progress').current,'Report result');
    const usage=emitted.findLast(e=>e.type==='running_stats');
    assert.equal(usage.outputTokens,123); assert.equal(usage.inputTokens,undefined); assert.equal(usage.costUsd,undefined);
    assert(!JSON.stringify(emitted).includes('private reasoning text'));
    socket.event('session.approval',{sessionKey:key,phase:'pending',approval});
    assert.equal(await provider.respondPermission(id,'allow-always',approval.id),false);
    assert.equal(calls.filter(c=>c.method==='approval.resolve').length,0);
    assert.equal(await provider.respondPermission(id,'allow-once',approval.id),true);
    const q={id:'question1',agentId:'main',sessionKey:key,status:'pending',expiresAtMs:Date.now()+60000,
      questions:[{questionId:'choice',header:'Choose',question:'Which?',options:[{label:'A'},{label:'B'}]}]};
    socket.event('question.requested',q);
    assert.equal(await provider.respondQuestion(id,{choice:['C']},q.id),false);
    assert.equal(await provider.respondQuestion(id,{choice:['A']},q.id),true);
    assert.equal(await provider.respondQuestion(id,{choice:['A']},q.id),false);
    agent(10,'lifecycle',{phase:'end',startedAt,endedAt:startedAt+2500});
    socket.event('chat',{sessionKey:key,runId:requestId,seq:3,state:'final',message:{content:[{type:'text',text:'Hello complete'}]}});
    assert.equal(provider.getStatus(id).state,'idle');
    assert.equal(provider.getDeviceSnapshot(id).activity,undefined);
    const finished=emitted.findLast(e=>e.type==='result');
    assert.equal(finished.durationMs,2500); assert.equal(finished.outputTokens,123); assert.equal(finished.turns,undefined);
    // Completion racing history remains visible in the atomic device snapshot.
    inFlight={runId:'existing-run',text:'Existing partial'};
    await provider.getHistory(id,10);
    historyHook=()=>socket.event('chat',{sessionKey:key,runId:'existing-run',seq:4,state:'final',message:{content:'Completed during history'}});
    const device = createDeviceApi(provider);
    async function openAndPoll(requestId) {
      const opened = await device({schema:1, op:'open', requestId, deviceId:'p4', sessionId:id});
      assert.equal(opened.ok,true);
      let after=0; const events=[];
      for(let n=0;n<20;++n) {
        const polled=await device({schema:1,op:'poll',requestId:'e'.repeat(32),deviceId:'p4',view:opened.view,instance:opened.instance,after});
        assert.equal(polled.ok,true); events.push(...polled.events); after=polled.cursor;
        if(!polled.more) return {opened,events};
      }
      assert.fail('Device view failed to drain');
    }
    const raced=await openAndPoll('d'.repeat(32));
    assert(raced.events.some(e=>e.type==='content'&&e.text==='Completed during history'));
    assert.equal(raced.opened.state,'idle');
    // Pending replay is independent of the bounded event ring and is projected
    // through the real device API, even when it arrives during history loading.
    historyHook=()=>socket.event('question.requested',{...q,id:'pending-after-history'});
    const pending=await openAndPoll('c'.repeat(32));
    assert(pending.events.some(e=>e.type==='query'&&e.question==='Choose\nWhich?'));
    assert.equal(pending.opened.state,'awaiting');
    socket.event('question.resolved',{id:'pending-after-history',status:'answered'});
    inFlight={runId:'prefix-run',text:'Hello',startedAt,
      plan:{steps:[{step:'Already done',status:'completed'},{step:'Continue',status:'in_progress'}]},
      events:[{runId:'prefix-run',seq:1,stream:'thinking',data:{delta:'not displayed'}},
        {runId:'prefix-run',seq:2,stream:'usage',data:{outputTokens:45}}]}; historyText='Hello';
    const prefix=await openAndPoll('f'.repeat(32));
    assert.equal(prefix.opened.activity,'thinking');
    assert(prefix.events.some(e=>e.kind===3&&e.text==='Tasks: 1/2 complete\nContinue'));
    assert(prefix.events.some(e=>e.kind===3&&e.text.includes('Reported output tokens: 45')));
    const original=prefix.events.filter(e=>e.type==='content'&&e.text==='Hello');
    assert.equal(original.length,1);
    historyHook=()=>agent(3,'plan',{phase:'update',steps:[{step:'Already done',status:'completed'},{step:'Continue',status:'completed'}]},'prefix-run');
    const metadataRace=await provider.getDeviceHistorySnapshot(id,10);
    assert.equal(metadataRace.recentProgressEvents.find(e=>e.type==='task_progress').completed,2);
    socket.event('chat',{sessionKey:key,runId:'prefix-run',seq:1,state:'delta',deltaText:' world',message:{content:'Hello world'}});
    let after=prefix.events.at(-1).id, updated;
    for(let n=0;n<8&&!updated;++n) {
      const continued=await device({schema:1,op:'poll',requestId:'e'.repeat(32),deviceId:'p4',view:prefix.opened.view,
        instance:prefix.opened.instance,after});
      assert.equal(continued.activity,'responding'); after=continued.cursor;
      updated=continued.events.find(e=>e.type==='content'&&e.kind===2);
    }
    assert(updated);
    assert.equal(updated.text,'Hello world'); assert.equal(updated.itemId,original[0].itemId);
    socket.event('chat',{sessionKey:key,runId:'prefix-run',seq:2,state:'final',message:{content:'NO_REPLY'}});
    // Terminal timing may arrive after chat.final; it refines statistics without
    // reactivating the run or flooding the ring with unchanged idle snapshots.
    agent(4,'lifecycle',{phase:'end',startedAt,endedAt:startedAt+4000},'prefix-run');
    assert.equal(emitted.findLast(e=>e.type==='running_stats').durationMs,4000);
    const frozen=provider.getDeviceSnapshot(id).cursor;
    agent(5,'usage',{outputTokens:45},'prefix-run');
    assert.equal(provider.getDeviceSnapshot(id).cursor,frozen);
    assert.equal(provider.getStatus(id).state,'idle');
    historyText='Earlier answer';
    assert(emitted.some(e=>e.type==='result'&&e.outcome==='no_visible_reply'&&e.text===''));
    // A new exact lifecycle owner arriving during an old history read wins even
    // before the first chat delta changes chatSeq.
    inFlight={runId:'old-snapshot',text:'Stale'};
    historyHook=()=>agent(1,'lifecycle',{phase:'start',startedAt},'fresh-owner');
    const newOwner=await provider.getDeviceHistorySnapshot(id,10);
    assert.equal(newOwner.snapshot.runId,'fresh-owner'); assert.equal(newOwner.liveText,'');
    socket.event('chat',{sessionKey:key,runId:'fresh-owner',seq:1,state:'final',message:{content:'Done'}});
    inFlight=null;
    // Actual failed provider admission is inspectable without leaking the text
    // or the Gateway's arbitrary diagnostic message to the Studio console.
    const failedView=await openAndPoll('6'.repeat(32)), logs=[], consoleError=console.error;
    console.error=(...args)=>logs.push(args.join(' ')); rejectSend=true;
    try {
      const failed=await device({schema:1,op:'prompt',requestId:'7'.repeat(32),deviceId:'p4',
        view:failedView.opened.view,instance:failedView.opened.instance,text:'PRIVATE PROMPT MUST NOT BE LOGGED'});
      assert.equal(failed.ok,false); assert.equal(failed.error.code,'provider_error');
      assert.deepEqual(logs,['[Hardware One Terminal] op=prompt error=provider_error gateway=INVALID_REQUEST']);
      assert(!JSON.stringify(failed).includes('PRIVATE'));
    } finally { console.error=consoleError; rejectSend=false; }
    await provider.getHistory(id,10); // Explicit reconciliation; no automatic resend.
    // More than 200 completed requests stay usable. An evicted ID is rejected
    // rather than executing it again; active and uncertain requests never evict.
    for(let n=1;n<=201;++n) {
      const rid=n.toString(16).padStart(32,'0');
      await provider.prompt(id,'Cache lifetime',undefined,rid);
      socket.event('chat',{sessionKey:key,runId:rid,seq:1,state:'final',message:{content:'Done'}});
    }
    await assert.rejects(provider.prompt(id,'Hello',undefined,requestId),/retired/);
    // Browsing another page retires only idle sessions, without requiring a restart.
    for(let page=0;page<3;++page) {
      listRows=Array.from({length:100},(_,n)=>({key:`agent:main:fixture:${page}:${n}`,updatedAt:1}));
      assert.equal((await provider.listSessions(100)).length,100);
    }
    listRows=null; await provider.listSessions();
    await provider.prompt(id,'Next',undefined,'b'.repeat(32));
    const before=provider.getInstanceId(), oldSocket=socket;
    let disconnects=0; client.onDisconnect(()=>++disconnects);
    oldSocket.dispatchEvent(new Event('error'));
    assert.equal(provider.getStatus(id).state,'uncertain'); assert.notEqual(before,provider.getInstanceId());
    assert.equal(disconnects,1);
    const subscriptions=calls.filter(c=>c.method==='sessions.messages.subscribe').length;
    await provider.getHistory(id,10);
    assert.equal(calls.filter(c=>c.method==='sessions.messages.subscribe').length,subscriptions+1);
    const reconnected=provider.getInstanceId(); oldSocket.dispatchEvent(new Event('close'));
    assert.equal(provider.getInstanceId(),reconnected); assert.equal(disconnects,1);
    assert.equal(provider.getStatus(id).state,'idle');
    assert(!JSON.stringify(emitted).includes('fixture-token'));
    assert(!calls.some(c=>/exec\.run|config|pair.*approve/u.test(c.method)));
  } finally { client.close(); rmSync(dir,{recursive:true,force:true}); }
});
