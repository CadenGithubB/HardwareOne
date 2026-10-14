import test from 'node:test';
import assert from 'node:assert/strict';
import { createDeviceApi } from '../device-api.mjs';
const SESSION = 'hwo_' + 'a'.repeat(64), OTHER = 'hwo_' + 'b'.repeat(64);
function fixture(history = []) {
  let instance = '11111111-1111-1111-1111-111111111111', cursor = 0, number = 1;
  const events = [], calls = [], snapshot = { sessionId: SESSION, runId: null, state: 'idle', uncertain: false, cursor: 0 };
  const provider = {
    getInstanceId: () => instance,
    listSessions: async (limit, _, offset) => Array.from({ length: Math.min(limit, 12 - offset) }, (_, i) => ({ id: SESSION, title: '😀'.repeat(30), status: 'idle' })),
    createSession: async () => { calls.push(['new']); return { sessionId: OTHER }; },
    getDeviceHistorySnapshot: async id => ({ history, snapshot: { ...snapshot, sessionId: id, cursor }, pendingEvents: [], recentToolEvents: [], liveText: '' }),
    getDeviceSnapshot: id => ({ ...snapshot, sessionId: id, cursor }),
    getDeviceEvents: (id, after, max) => ({ reset: after < (events[0]?.id ?? 1) - 1 || after > cursor, cursor, events: events.filter(e => e.id > after).slice(0, max) }),
    prompt: async (id, text, _, requestId) => { calls.push(['prompt', id, text, requestId]); return { runId: requestId }; },
    interrupt: async (id, runId) => { calls.push(['interrupt', id, runId]); return { aborted: true }; },
    respondPermission: async (...args) => { calls.push(['permission', ...args]); return true; },
    respondQuestion: async (...args) => { calls.push(['question', ...args]); return true; },
  };
  const handler = createDeviceApi(provider);
  const request = (op, extra = {}, id) => ({ schema: 1, deviceId: 'p4', requestId: id || (number++).toString(16).padStart(32, '0'), op, ...extra });
  const call = async (op, extra, id) => { const r = await handler(request(op, extra, id)); assert.ok(Buffer.byteLength(JSON.stringify(r)) <= 4096); return r; };
  const emit = e => { events.push({ id: ++cursor, ...e }); };
  const open = async () => { const r = await call('open', { sessionId: SESSION }); assert.equal(r.ok, true); return { view: r.view, instance: r.instance }; };
  const drain = async view => {
    let after = 0; const all = [];
    for (let i = 0; i < 1000; ++i) {
      const r = await call('poll', { ...view, after }); assert.equal(r.ok, true, JSON.stringify(r));
      assert.ok(r.events.length <= 3); all.push(...r.events); after = r.cursor;
      if (!r.more) return { events: all, after };
    }
    throw Error('poll did not drain');
  };
  return { call, open, drain, emit, calls, provider, request, handler, rotate: () => { instance = '22222222-2222-2222-2222-222222222222'; } };
}

test('recent history is paged without truncating UTF-8 or escaped JSON; list has correct next page', async () => {
  const raw = ('😀\n"\\\t'.repeat(700));
  const f = fixture([{ role: 'assistant', text: raw }]);
  const page = await f.call('list', { offset: 0, limit: 7 });
  assert.equal(page.sessions.length, 7); assert.equal(page.nextOffset, 7);
  assert.ok(Buffer.byteLength(page.sessions[0].title) <= 64);
  const final = await f.call('list', { offset: 7, limit: 7 }); assert.equal(final.nextOffset, null);
  const view = await f.open(), result = await f.drain(view), items = result.events.filter(e => e.type === 'content');
  assert.equal(items.map(x => x.text).join(''), raw);
  assert.ok(items.every(x => Buffer.byteLength(x.text) <= 512));
});

test('streaming tail upserts preserve all output; replacement retires obsolete tails', async () => {
  const f = fixture(), view = await f.open(); let { after } = await f.drain(view);
  const raw = '😀'.repeat(1500);
  f.emit({ type: 'text_delta', runId: 'run1', text: raw });
  const items = new Map();
  async function consume() {
    for (let i = 0; i < 100; ++i) {
      const r = await f.call('poll', { ...view, after }); assert.equal(r.ok, true); after = r.cursor;
      for (const e of r.events) if (e.type === 'content') items.set(e.itemId, e.text);
      if (!r.more) return;
    }
  }
  await consume(); assert.equal([...items.values()].join(''), raw);
  f.emit({ type: 'text_delta', runId: 'run1', text: ' END' }); await consume();
  assert.equal([...items.values()].join(''), raw + ' END');
  f.emit({ type: 'text_replace', runId: 'run1', text: 'Revised.' }); await consume();
  assert.equal([...items.values()].join(''), 'Revised.');
});

test('one large event drains before later events can evict the undelivered projection', async () => {
  const f = fixture(), view = await f.open(); await f.drain(view);
  for (let n = 0; n < 4; ++n) f.emit({ type: 'text_replace', runId: 'run', text: String(n).repeat(64 * 1024) });
  // Each provider event expands to128 chunks. Eight-at-once reaches the ring
  // boundary; final reconstruction must be complete, not skip text silently.
  const result = await f.drain(view), items = new Map();
  for (const e of result.events) if (e.type === 'content') items.set(e.itemId, e.text);
  assert.equal([...items.values()].join(''), '3'.repeat(64 * 1024));
});

test('prompt ID is idempotent and selected view plus instance fence every mutation', async () => {
  const f = fixture(), view = await f.open(), id = '8'.repeat(32), args = { ...view, text: 'Hello' };
  const a = await f.call('prompt', args, id), b = await f.call('prompt', args, id);
  assert.deepEqual(a, b); assert.equal(f.calls.length, 1);
  assert.equal((await f.call('prompt', { ...args, text: 'Changed' }, id)).error.code, 'request_conflict');
  await f.open(); assert.equal((await f.call('prompt', args, id)).error.code, 'reopen_required');
  const current = await f.open(); f.rotate();
  assert.equal((await f.call('interrupt', { ...current, runId: 'run' })).error.code, 'reopen_required');
  assert.equal(f.calls.length, 1);
});

test('new reserves a conversation without a model call', async () => {
  const f = fixture(); const result = await f.call('new');
  assert.equal(result.ok, true); assert.equal(result.sessionId, OTHER); assert.deepEqual(f.calls, [['new']]);
});

test('approval renders complete safe facts, accepts only exact offers, and cannot migrate to another view', async () => {
  const f = fixture(), view = await f.open();
  f.emit({ type: 'permission_request', requestId: 'gateway-a', expiresAtMs: Date.now() + 60000,
    presentation: { kind: 'exec', commandText: 'echo hello', host: 'sandbox', allowedDecisions: ['allow-once', 'deny'] } });
  const events = (await f.drain(view)).events, q = events.find(x => x.type === 'query');
  assert.match(q.question, /host: sandbox/u); assert.ok(q.expiresInMs > 0 && q.expiresInMs <= 60000);
  assert.equal((await f.call('permission', { ...view, pendingId: q.pendingId, decision: 'allow-always' })).ok, false);
  assert.equal((await f.call('permission', { ...view, pendingId: q.pendingId, decision: 'allow-once' })).applied, true);
  assert.deepEqual(f.calls, [['permission', SESSION, 'allow-once', 'gateway-a']]);
  assert.equal((await f.call('permission', { ...view, pendingId: q.pendingId, decision: 'deny' })).error.code, 'stale_request');
});

test('oversized or externally verified approvals cannot receive an actionable truncated card', async () => {
  const f = fixture(), view = await f.open();
  f.emit({ type: 'permission_request', requestId: 'gateway-a', expiresAtMs: Date.now() + 60000,
    presentation: { kind: 'exec', commandText: 'x'.repeat(2000), allowedDecisions: ['deny', 'allow-once'] } });
  const events = (await f.drain(view)).events;
  assert.ok(events.some(x => x.type === 'host_required')); assert.ok(!events.some(x => x.type === 'query')); assert.deepEqual(f.calls, []);
});

test('multi-panel questions keep exact choices, stage locally, then resolve once', async () => {
  const f = fixture(), view = await f.open();
  f.emit({ type: 'user_question', requestId: 'gateway-q', expiresAtMs: Date.now() + 60000, questions: [
    { questionId: 'color', question: 'Pick colors', multiSelect: true, options: [{ label: 'Red' }, { label: 'Blue' }] },
    { questionId: 'name', question: 'Name?', isOther: true, options: [{ label: 'Default' }] },
  ] });
  let result = await f.drain(view), q = result.events.find(x => x.type === 'query');
  assert.equal((await f.call('question', { ...view, pendingId: q.pendingId, questionId: 'q0', optionIds: ['o0', 'o1'] })).complete, false);
  assert.equal(f.calls.length, 0);
  assert.equal((await f.call('question', { ...view, pendingId: q.pendingId, questionId: 'q0', optionIds: ['o0'] })).error.code, 'stale_request');
  assert.equal((await f.call('question', { ...view, pendingId: q.pendingId, questionId: 'q1', text: 'A voice answer' })).complete, true);
  assert.deepEqual(f.calls, [['question', SESSION, { color: ['Red', 'Blue'], name: ['A voice answer'] }, 'gateway-q']]);
});

for (const toolName of ['ask_user', 'openclaw:core:ask_user']) {
test(`${toolName} internals stay out of glasses text while cards and other JSON remain intact`, async () => {
  const f = fixture(), view = await f.open();
  const internal = '{"status":"answered","answers":{"answers":{"choice":["PRIVATE_ANSWER_FIXTURE"]}},"secret":"PRIVATE_SECRET_FIXTURE"}';
  let itemId;
  for (const [type, isError, expected] of [
    ['tool_start', false, 'Waiting'], ['tool_update', false, 'Update'],
    ['tool_end', false, 'Finished'], ['tool_end', true, 'Failed'],
  ]) {
    f.emit({ type, runId: 'run', toolId: 'ask1', name: toolName, isError,
      text: internal, result: internal, detail: { input: { private: internal }, output: internal } });
    const events = (await f.drain(view)).events;
    assert.ok(!JSON.stringify(events).includes('PRIVATE_'));
    const item = events.filter(e => e.type === 'content').at(-1);
    assert.equal(item.text, `Question\n${expected}`);
    if (itemId === undefined) itemId = item.itemId;
    else assert.equal(item.itemId, itemId);
  }
  f.emit({ type: 'user_question', requestId: 'gateway-q', expiresAtMs: Date.now() + 60000, questions: [
    { questionId: 'choice', question: 'Choose one', options: [{ label: 'Alpha' }, { label: 'Beta' }] },
  ] });
  const card = (await f.drain(view)).events.find(e => e.type === 'query');
  assert.deepEqual(card.options, [{ id: 'o0', label: 'Alpha' }, { id: 'o1', label: 'Beta' }]);
  assert.equal((await f.call('question', { ...view, pendingId: card.pendingId, questionId: card.questionId, optionIds: ['o1'] })).complete, true);
  assert.deepEqual(f.calls, [['question', SESSION, { choice: ['Beta'] }, 'gateway-q']]);
  const json = '{"example":"keep this JSON"}';
  f.emit({ type: 'result', runId: 'run', text: json });
  f.emit({ type: 'tool_end', runId: 'run', toolId: 'other', name: 'hardwareone_cli', text: json });
  const result = (await f.drain(view)).events;
  assert.ok(result.some(e => e.type === 'content' && e.kind === 2 && e.text === json));
  assert.ok(result.some(e => e.type === 'content' && e.kind === 3 && e.text === `hardwareone_cli\nFinished\n${json}`));
  assert.ok(!JSON.stringify(result).includes('PRIVATE_'));

  const replay = fixture();
  replay.provider.getDeviceHistorySnapshot = async () => ({ history: [],
    snapshot: { runId: 'run', state: 'idle', cursor: 0 },
    recentToolEvents: [{ type: 'tool_end', runId: 'run', toolId: 'ask1', name: toolName, text: internal }] });
  const replayed = (await replay.drain(await replay.open())).events;
  assert.deepEqual(replayed.filter(e => e.type === 'content').map(e => e.text), ['Question\nFinished']);
  assert.ok(!JSON.stringify(replayed).includes('PRIVATE_'));
});
}

test('unknown routes, extra authority, malformed IDs and stale event cursors fail closed', async () => {
  const f = fixture();
  assert.equal((await f.handler(f.request('list', { offset: 0, limit: 7, agentId: 'devlab' }))).ok, false);
  assert.equal((await f.call('open', { sessionId: 'agent:main:main' })).ok, false);
  const view = await f.open(); assert.equal((await f.call('poll', { ...view, after: 9000 })).error.code, 'reopen_required');
  assert.equal((await f.call('chat.send')).ok, false);
});

test('uncertain final question resolution retires the card and never exposes provider diagnostics', async () => {
  const f = fixture(), view = await f.open();
  f.provider.respondQuestion = async () => { throw Error('private provider token and diagnostic'); };
  f.emit({ type: 'user_question', requestId: 'gateway-q', expiresAtMs: Date.now() + 60000, questions: [
    { questionId: 'only', question: 'Proceed?', options: [{ label: 'Yes' }] },
  ] });
  const q = (await f.drain(view)).events.find(x => x.type === 'query');
  const args = { ...view, pendingId: q.pendingId, questionId: 'q0', optionIds: ['o0'] };
  const result = await f.call('question', args);
  assert.equal(result.ok, false); assert.ok(!JSON.stringify(result).includes('private provider'));
  assert.equal((await f.call('question', args)).error.code, 'stale_request');
});

test('retiring a view releases action cache and stale selection IDs cannot reopen old views', async () => {
  const f = fixture();
  const firstId = 'f'.repeat(32);
  const first = await f.call('open', { sessionId: SESSION }, firstId);
  const view = { view: first.view, instance: first.instance };
  for (let n = 0; n < 256; ++n) assert.equal((await f.call('prompt', { ...view, text: 'New question' })).ok, true);
  assert.equal((await f.call('prompt', { ...view, text: 'At limit' })).error.code, 'reopen_required');
  const next = await f.open(); assert.equal((await f.call('prompt', { ...next, text: 'Fresh view' })).ok, true);
  for (let n = 0; n < 35; ++n) await f.open();
  assert.equal((await f.call('open', { sessionId: SESSION }, firstId)).error.code, 'reopen_required');
});

test('explicit empty final clears a streamed silent sentinel rather than redisplaying it', async () => {
  const f = fixture(), view = await f.open(); let { after } = await f.drain(view);
  f.emit({ type: 'text_delta', runId: 'silent', text: 'NO_REPLY' });
  let r = await f.call('poll', { ...view, after }); after = r.cursor;
  const old = r.events.find(e => e.type === 'content'); assert.equal(old.text, 'NO_REPLY');
  f.emit({ type: 'result', runId: 'silent', text: '', outcome: 'no_visible_reply' });
  r = await f.call('poll', { ...view, after });
  assert.deepEqual(r.events.filter(e => e.type === 'content').map(e => [e.itemId, e.text]), [[old.itemId, '']]);
});

test('adjacent live token deltas are compacted without crossing a tool event', async () => {
  const f = fixture(), view = await f.open(); let { after } = await f.drain(view);
  for (let i = 0; i < 30; ++i) f.emit({ type: 'text_delta', runId: 'run', text: '😀' });
  f.emit({ type: 'tool_start', runId: 'run', toolId: 'tool1', name: 'Check' });
  f.emit({ type: 'text_delta', runId: 'run', text: ' after tool' });
  let r = await f.call('poll', { ...view, after }); after = r.cursor;
  assert.equal(r.events.length, 1); assert.equal(r.events[0].text, '😀'.repeat(30));
  r = await f.call('poll', { ...view, after }); after = r.cursor; assert.equal(r.events[0].kind, 3);
  r = await f.call('poll', { ...view, after }); assert.equal(r.events[0].text, '😀'.repeat(30) + ' after tool');
});

test('question descriptions are preserved and every later panel is checked before an offer', async () => {
  const f = fixture(), view = await f.open();
  f.emit({ type: 'user_question', requestId: 'q-safe', expiresAtMs: Date.now() + 60000, questions: [
    { questionId: 'delete', header: 'Files', question: 'Proceed?', options: [{ label: 'Yes', description: 'Delete the temporary files' }] },
  ] });
  const q = (await f.drain(view)).events.find(x => x.type === 'query');
  assert.match(q.question, /Yes: Delete the temporary files/u);
  await f.call('question', { ...view, pendingId: q.pendingId, questionId: 'q0', cancel: true });
  f.emit({ type: 'user_question', requestId: 'q-large', expiresAtMs: Date.now() + 60000, questions: [
    { questionId: 'first', question: 'Small first panel', options: [{ label: 'Yes' }] },
    { questionId: 'later', question: '\u0001'.repeat(1024), options: [{ label: 'Yes' }] },
  ] });
  const result = await f.drain(view);
  assert.ok(result.events.some(x => x.type === 'host_required'));
  assert.equal(result.events.filter(x => x.type === 'query').length, 1); // only the original retained card
});

for (const id of ['ask_user', 'openclaw:core:ask_user']) {
  test(`deferred ${id} hides schema failures and answers, including replay`, async () => {
    const f = fixture(), view = await f.open();
    const detail = { input: { id, args: { questions: [] } } };
    for (const [type, isError, label] of [['tool_start', false, 'Waiting'], ['tool_update', false, 'Update'], ['tool_end', true, 'Failed'], ['tool_end', false, 'Finished']]) {
      f.emit({ type, runId: 'r', toolId: 'wrapper', name: 'tool_call', isError, detail,
        text: '{"error":"PRIVATE_SCHEMA_FIXTURE", "answers":"PRIVATE_ANSWER_FIXTURE"}' });
      const events = (await f.drain(view)).events;
      assert(!JSON.stringify(events).includes('PRIVATE_'));
      assert(events.some(e => e.type === 'content' && e.text === `Question\n${label}`));
    }
    const replay = fixture();
    replay.provider.getDeviceHistorySnapshot = async () => ({ history: [],
      snapshot: { runId: 'r', state: 'idle', cursor: 0 }, recentToolEvents: [
        { type: 'tool_end', runId: 'r', toolId: 'wrapper', name: 'tool_call', isError: true, detail,
          text: '{"error":"PRIVATE_SCHEMA_FIXTURE"}' }] });
    const events = (await replay.drain(await replay.open())).events;
    assert(!JSON.stringify(events).includes('PRIVATE_'));
    assert(events.some(e => e.type === 'content' && e.text === 'Question\nFailed'));
  });
}
test('only exact wrapper envelope is suppressed when start arguments are absent', async () => {
  const f = fixture(), view = await f.open();
  const envelope = JSON.stringify({ tool: { id:'openclaw:core:ask_user',name:'ask_user',source:'openclaw' },
    result:{ content:[{type:'text',text:'PRIVATE_ANSWER_FIXTURE'}] } });
  f.emit({ type:'tool_end',name:'tool_call',toolId:'question',text:envelope });
  const events = (await f.drain(view)).events;
  assert(!JSON.stringify(events).includes('PRIVATE_'));
  assert(events.some(e=>e.type==='content' && e.text==='Question\nFinished'));
  for (const [name, value] of [['tool_call','{"tool":{"id":"other","name":"ask_user","source":"openclaw"},"result":{}}'],
    ['other',envelope],['tool_call','ask_user: keep this ordinary text']]) {
    f.emit({ type:'tool_end',name,toolId:name+value,text:value });
    const result = (await f.drain(view)).events;
    assert(result.some(e=>e.type==='content' && e.text.includes(value)));
  }
  f.emit({type:'result',runId:'r',text:envelope});
  assert((await f.drain(view)).events.some(e=>e.type==='content' && e.kind===2 && e.text===envelope));
});

test('accepted question answers render exact readable choices and voice text in bounded UTF-8 items', async () => {
  const f = fixture(), view = await f.open();
  const answers = [
    { question: 'Pick colors', values: ['Red', 'Blue'] },
    { question: 'Describe the place ' + '😀'.repeat(160), values: ['🌲'.repeat(120)] },
  ];
  const receipt = { type: 'question_answer', requestId: 'accepted-question', status: 'answered', answers };
  f.emit(receipt);
  const first = (await f.drain(view)).events.filter(e => e.type === 'content' && e.kind === 5);
  assert.equal(first.map(e => e.text).join(''), answers.map(a => `${a.question}\nYou: ${a.values.join(', ')}`).join(''));
  assert.ok(first.length > answers.length);
  assert.ok(first.every(e => Buffer.byteLength(e.text) <= 512 && !e.text.includes('\ufffd')));
  // An event/RPC replay uses the same content identities, not another answer.
  f.emit(receipt);
  const repeated = (await f.drain(view)).events.filter(e => e.type === 'content' && e.kind === 5);
  assert.deepEqual(repeated, first);
  assert.deepEqual(f.calls, []);
});

test('retained accepted answers return on reopen without exposing internal tool result JSON', async () => {
  const f = fixture();
  const receipt = { type: 'question_answer', requestId: 'retained-answer', status: 'answered',
    answers: [{ question: 'Which route?', values: ['The wooded path'] }] };
  f.provider.getDeviceHistorySnapshot = async () => ({ history: [], snapshot: { runId: 'run', state: 'idle', cursor: 0 },
    recentAnswerEvents: [receipt], recentToolEvents: [{ type: 'tool_end', name: 'openclaw:core:ask_user', toolId: 'ask1',
      text: '{"status":"answered","private":"PRIVATE_INTERNAL_FIXTURE"}' }] });
  const view = await f.open();
  let events = (await f.drain(view)).events;
  const answer = events.filter(e => e.type === 'content' && e.kind === 5);
  assert.deepEqual(answer.map(e => e.text), ['Which route?\nYou: The wooded path']);
  assert.ok(!JSON.stringify(events).includes('PRIVATE_INTERNAL_FIXTURE'));
  f.emit(receipt);
  events = (await f.drain(view)).events;
  assert.deepEqual(events.filter(e => e.type === 'content' && e.kind === 5), answer);
  const reopened = (await f.drain(await f.open())).events;
  assert.deepEqual(reopened.filter(e => e.type === 'content' && e.kind === 5).map(e => e.text), ['Which route?\nYou: The wooded path']);
});

test('initial answer replay preserves the event window, pending query and newest complete receipts', async () => {
  const f = fixture(), historyText = 'H'.repeat(128 * 1024);
  const receipts = Array.from({ length: 8 }, (_, i) => ({
    type: 'question_answer', requestId: `receipt-${i}`, status: 'answered',
    answers: Array.from({ length: 3 }, (_, panel) => ({
      question: `Receipt ${i} panel ${panel}: `.padEnd(1000, 'Q'),
      values: ['A', 'B', 'C', 'D'].map(value => value.repeat(128)),
    })),
  }));
  let replay = false;
  f.provider.getDeviceHistorySnapshot = async () => ({
    history: [{ role: 'assistant', text: historyText }],
    snapshot: { runId: 'run', state: 'awaiting', cursor: 0 },
    recentToolEvents: Array.from({ length: 24 }, (_, i) => ({
      type: 'tool_end', name: 'read_file', toolId: `tool-${i}`, text: 'T'.repeat(4048),
    })),
    pendingEvents: [{ type: 'user_question', requestId: 'next-question', expiresAtMs: Date.now() + 60000,
      questions: [{ questionId: 'next', question: 'Keep this pending question', options: [{ label: 'Continue' }] }] }],
    recentAnswerEvents: replay ? receipts : [],
  });
  // This valid initial projection fits before adding retained answer receipts.
  const baseline = (await f.drain(await f.open())).events;
  assert.equal(baseline.filter(e => e.type === 'content' && e.kind === 2).map(e => e.text).join(''), historyText);
  assert.ok(baseline.length < 512);
  replay = true;
  const result = await f.drain(await f.open()), events = result.events;
  assert.ok(events.length <= 512);
  assert.deepEqual(events.map(e => e.id), Array.from({ length: events.length }, (_, i) => i + 1));
  assert.equal(events.filter(e => e.type === 'content' && e.kind === 2).map(e => e.text).join(''), historyText);
  assert.deepEqual(events.filter(e => e.type === 'content' && e.kind === 3).map(e => e.text),
    baseline.filter(e => e.type === 'content' && e.kind === 3).map(e => e.text));
  const pending = events.filter(e => e.type === 'query');
  assert.equal(pending.length, 1);
  assert.equal(pending[0].question, 'Keep this pending question');
  assert.ok(!events.some(e => e.type === 'query_clear' && e.pendingId === pending[0].pendingId));
  assert.equal(events.at(-1).type, 'state');
  const echoed = events.filter(e => e.type === 'content' && e.kind === 5).map(e => e.text).join('');
  const receiptText = receipt => receipt.answers.map(a => `${a.question}\nYou: ${a.values.join(', ')}`).join('');
  assert.ok(echoed.endsWith(receiptText(receipts.at(-1))), 'the newest complete receipt must fit');
  assert.ok(!echoed.includes('Receipt 0 panel 0:'), 'older receipts yield capacity to baseline content and controls');
  assert.deepEqual(f.calls, []);
});

test('unaccepted, cancelled and externally resolved questions never invent an answer echo', async () => {
  const f = fixture(), view = await f.open();
  for (const status of ['pending', 'cancelled', 'expired', 'rejected'])
    f.emit({ type: 'question_answer', requestId: status, status, answers: [{ question: 'Proceed?', values: ['Yes'] }] });
  f.emit({ type: 'question_answer', requestId: 'external', status: 'answered' });
  assert.deepEqual((await f.drain(view)).events.filter(e => e.type === 'content' && e.kind === 5), []);
  for (const outcome of ['rejected', 'throw', 'cancel']) {
    const run = fixture(), selected = await run.open();
    run.provider.respondQuestion = async () => {
      if (outcome === 'throw') throw Error('Uncertain question resolution');
      return outcome === 'cancel';
    };
    run.emit({ type: 'user_question', requestId: outcome, expiresAtMs: Date.now() + 60000,
      questions: [{ questionId: 'q', question: 'Proceed?', options: [{ label: 'Yes' }] }] });
    const q = (await run.drain(selected)).events.find(e => e.type === 'query');
    const response = await run.call('question', { ...selected, pendingId: q.pendingId, questionId: q.questionId,
      ...(outcome === 'cancel' ? { cancel: true } : { optionIds: ['o0'] }) });
    if (outcome === 'throw') assert.equal(response.ok, false);
    else assert.equal(response.applied, outcome === 'cancel');
    assert.deepEqual((await run.drain(selected)).events.filter(e => e.type === 'content' && e.kind === 5), []);
  }
});
