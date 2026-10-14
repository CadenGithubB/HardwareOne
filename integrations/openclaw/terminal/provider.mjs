import { createHash, randomUUID } from 'node:crypto';
import { Gateway } from './gateway.mjs';

const ID = /^hwo_[a-f0-9]{64}$/u;
const MAX_TEXT = 128 * 1024;
// Exact v2026.9.4 abort-primitives.ts bare-command vocabulary. The bridge has
// a separate, run-bound Interrupt action; these inputs must not enter chat.send
// as a cancellation that lacks an ordinary newly admitted runId.
const ABORT_COMMANDS = new Set([
  'stop','esc','abort','exit','interrupt','detente','deten','detén','arrete','arrête',
  '停止','停下来','暂停','やめて','止めて','रुको','توقف','стоп','остановись','останови',
  'остановить','прекрати','halt','anhalten','aufhören','hoer auf','stopp','pare',
  'stop openclaw','openclaw stop','stop action','stop current action','stop run',
  'stop current run','stop agent','stop the agent',"stop don't do anything",'stop dont do anything',
  'stop do not do anything','stop doing anything','do not do that','please stop','stop please',
]);
const error = (message, statusCode = 400) => Object.assign(new Error(message), { statusCode });
const opaque = (key) => `hwo_${createHash('sha256').update(key).digest('hex')}`;
const textOf = (message) => typeof message?.content === 'string' ? message.content :
  Array.isArray(message?.content) ? message.content.filter(x => x?.type === 'text' && typeof x.text === 'string').map(x => x.text).join('\n') : '';
const boundedText = (text, limit = MAX_TEXT) => { const n = Buffer.byteLength(text); if (n <= limit) return text;
  const bytes = Buffer.from(text); let end = limit; while ((bytes[end] & 0xc0) === 0x80) --end;
  return bytes.subarray(0, end).toString('utf8'); };

// Implements the official Even Terminal ProviderHandlers shape. Its routes and
// SSE/message cursor store stay official; Gateway remains the history owner.
export function createOpenClawProvider(emit, { client = new Gateway(JSON.parse(process.env.HW1_GATEWAY_CONFIG || '{}')) } = {}) {
  const sessions = new Map(), byKey = new Map(), requests = new Map();
  // Retired IDs may never trigger another send in this process. A bounded Bloom
  // set can reject a fresh colliding ID, but cannot accidentally replay an old one.
  const retired = new Uint8Array(8192);
  const requestBits = id => { const h = createHash('sha256').update(id).digest(); return [0,2,4].map(n => h.readUInt16LE(n)); };
  let instanceId = randomUUID();
  function state(s) {
    for (const offers of [s.approvals, s.questions]) for (const [id, offer] of offers)
      if (offer.expiresAtMs <= Date.now()) offers.delete(id);
    return s.uncertain ? 'uncertain' : s.approvals.size || s.questions.size ? 'awaiting' : s.busy ? 'busy' : 'idle';
  }
  function retireIdleSession() {
    const s = [...sessions.values()].filter(x => !x.busy && !x.uncertain && !x.readers && !x.subscribing && state(x) === 'idle')
      .sort((a, b) => a.touched - b.touched)[0];
    if (!s) throw error('All cached conversations are active; close an active run before opening more', 503);
    sessions.delete(s.id); byKey.delete(s.key);
    if (s.subscribed) client.request('sessions.messages.unsubscribe', {key:s.key, agentId:'main'}).catch(() => {});
  }
  function publish(s, msg) {
    const value = { ...msg, sessionId: s.id, provider: 'openclaw', ...(s.runId ? { runId: s.runId } : {}) };
    const id = ++s.cursor;
    s.events.push({ id, ...value });
    s.eventBytes += Buffer.byteLength(JSON.stringify(s.events.at(-1)));
    while (s.events.length > 500 || s.eventBytes > 256 * 1024) s.eventBytes -= Buffer.byteLength(JSON.stringify(s.events.shift()));
    emit(s.id, value);
  }
  function resetRun(s, runId) {
    s.runId = runId; s.text = ''; s.tools.clear(); s.chatSeq = s.agentSeq = -1;
    s.activity = undefined; s.startedAt = s.endedAt = s.outputTokens = undefined;
    s.planMessage = s.statsMessage = null; s.statsAt = 0;
  }
  const activityFields = s => state(s) === 'busy' && s.activity ? {activity:s.activity} : {};
  function activity(s, next) {
    if (!s.busy || s.activity === next) return;
    const previous = {thinking:'think_end', responding:'text_end'}[s.activity];
    if (previous) publish(s, {type:'status', state:previous});
    s.activity = next;
    if (next) publish(s, {type:'status', state:{thinking:'think_start',responding:'text_start'}[next] || 'busy', activity:next});
  }
  function stats(s, force = false) {
    const now = Date.now(), value = {};
    if (s.startedAt !== undefined && (s.endedAt !== undefined || s.busy && !s.uncertain))
      value.durationMs = Math.max(0, (s.endedAt ?? now) - s.startedAt);
    if (s.outputTokens !== undefined) value.outputTokens = s.outputTokens;
    if (Object.keys(value).length && (force || !s.statsMessage || now - s.statsAt >= 10000)) {
      s.statsAt = now;
      const message = {type:'running_stats', runId:s.runId, ...value};
      if (JSON.stringify(message) !== JSON.stringify(s.statsMessage)) { s.statsMessage = message; publish(s, message); }
    }
    return value;
  }
  function plan(s, data) {
    if (!Array.isArray(data?.steps) || data.steps.length > 100) return;
    const steps = data.steps.map(x => typeof x === 'string' ? {step:x,status:'pending'} : x);
    if (steps.some(x => typeof x?.step !== 'string' || !x.step.trim() || !['pending','in_progress','completed'].includes(x.status))) return;
    const completed = steps.filter(x => x.status === 'completed').length;
    const current = steps.find(x => x.status === 'in_progress')?.step || (steps.length && completed === steps.length ? 'All done' : '');
    const message = {type:'task_progress', runId:s.runId, completed, total:steps.length,
      current:Buffer.byteLength(current) > 480 ? boundedText(current, 480) + '…' : current};
    if (JSON.stringify(message) !== JSON.stringify(s.planMessage)) { s.planMessage = message; publish(s, message); }
  }
  function agentEvent(s, p) {
    if (p.runId !== s.runId || !Number.isSafeInteger(p.seq) || p.seq <= s.agentSeq) return;
    s.agentSeq = p.seq;
    const d = p.data;
    if (!d || typeof d !== 'object') return;
    if (p.stream === 'thinking') activity(s, 'thinking');
    else if (p.stream === 'assistant' && (typeof d.text === 'string' && d.text || typeof d.delta === 'string' && d.delta)) activity(s, 'responding');
    else if (p.stream === 'plan' && d.phase === 'update') plan(s, d);
    else if (p.stream === 'usage' && Number.isSafeInteger(d.outputTokens) && d.outputTokens >= 0) {
      // v2026.9.4 emits a cumulative run total after each completed model call.
      // It does not report per-run input tokens, cost or turn counts here.
      if (s.outputTokens === undefined || d.outputTokens >= s.outputTokens) { s.outputTokens = d.outputTokens; stats(s, true); }
    } else if (p.stream === 'lifecycle') {
      if (Number.isSafeInteger(d.startedAt) && d.startedAt > 0) s.startedAt = d.startedAt;
      if (['end','error'].includes(d.phase) && Number.isSafeInteger(d.endedAt) && d.endedAt >= (s.startedAt ?? 0)) s.endedAt = d.endedAt;
      stats(s, true);
    } else if (p.stream === 'tool') {
      if (typeof d.toolCallId !== 'string' || typeof d.name !== 'string') return;
      const type = {start:'tool_start', update:'tool_update', result:'tool_end'}[d.phase];
      if (!type) return;
      if (d.phase !== 'result') activity(s, 'tool');
      // Preserve the pinned worker's textual previews, not opaque attachments.
      const value = d.phase === 'update' ? d.partialResult : d.result;
      const output = typeof value === 'string' ? value : textOf(value);
      const preview = Buffer.byteLength(output) > 4096 ? boundedText(output, 4048) + '\n[Truncated; see OpenClaw dashboard.]' : output;
      const detail = {...s.tools.get(d.toolCallId)?.detail};
      if (d.args && typeof d.args === 'object' && Buffer.byteLength(JSON.stringify(d.args)) <= 4096) detail.input = d.args;
      if (output) detail.output = preview;
      const msg = {type, name:d.name, toolId:d.toolCallId, runId:p.runId, detail,
        ...(output ? {text:preview} : {}), ...(typeof d.isError === 'boolean' ? {isError:d.isError} : {})};
      s.tools.set(d.toolCallId, msg);
      if (s.tools.size > 32) s.tools.delete(s.tools.keys().next().value);
      publish(s, msg);
    }
  }
  function remember(key, row = {}) {
    if (typeof key !== 'string' || !key.startsWith('agent:main:') || key.length > 512) return null;
    const id = opaque(key);
    let s = sessions.get(id);
    if (!s) {
      if (sessions.size >= 200) retireIdleSession();
      s = { id, key, row, runId: null, busy: false, uncertain: false, subscribed: false,
        touched: Date.now(), readers: 0, subscribing: null, text: '', chatSeq: -1, agentSeq: -1, cursor: 0, eventBytes: 0, events: [], approvals: new Map(), questions: new Map(), tools: new Map() };
      sessions.set(id, s); byKey.set(key, s);
    }
    s.row = { ...s.row, ...row }; s.touched = Date.now(); return s;
  }
  async function listSessions(limit = 10, cwd, offset = 0) {
    if (cwd) throw error('OpenClaw does not accept a host working directory');
    limit = Math.max(1, Math.min(100, Number(limit) || 10));
    if (!Number.isSafeInteger(offset) || offset < 0 || offset > 10000) throw error('Invalid session offset');
    const result = await client.request('sessions.list', { agentId: 'main', limit, offset,
      includeDerivedTitles: true, includeLastMessage: false, archived: false, sortBy: 'lastInteractionAt' });
    if (!Array.isArray(result?.sessions)) throw error('Invalid Gateway session list', 502);
    return result.sessions.map(row => remember(row.key, row)).filter(Boolean).map(s => ({
      id: s.id, title: String(s.row.label || s.row.displayName || s.row.derivedTitle || 'Mingus conversation').slice(0, 128),
      timestamp: new Date(Number(s.row.lastInteractionAt || s.row.updatedAt || 0)).toISOString(),
      cwd: '', provider: 'openclaw', status: s.subscribed ? state(s) : null,
    }));
  }
  async function resolve(id) {
    if (!ID.test(id ?? '')) throw error('Unknown OpenClaw session', 404);
    if (!sessions.has(id)) await listSessions(100);
    const s = sessions.get(id);
    if (!s) throw error('Session is not in the current Mingus session list', 404);
    s.touched = Date.now(); return s;
  }
  const approvalMessage = a => {
    const p = a.presentation;
    return { type: 'permission_request', toolUseId: a.id, requestId: a.id, toolName: p.toolName || p.kind,
      description: p.title || p.commandText || p.description || 'Gateway approval',
      detail: [p.detail, p.warningText, p.scope ? JSON.stringify(p.scope) : ''].filter(Boolean).join('\n'),
      options: p.allowedDecisions.filter(k => ['allow-once', 'allow-always', 'deny'].includes(k)).map(key => ({ key, label: key })),
      expiresAtMs: a.expiresAtMs, presentation: p, externalResolution: p.externalResolution };
  };
  const questionMessage = q => ({ type:'user_question', toolUseId:q.id, requestId:q.id, questions:q.questions, expiresAtMs:q.expiresAtMs });
  function approval(s, a) {
    if (!a || typeof a.id !== 'string' || !['exec', 'plugin', 'system-agent'].includes(a.presentation?.kind)) return;
    if (a.status !== 'pending') {
      if (s.approvals.delete(a.id)) publish(s, { type: 'permission_result', toolUseId: a.id, decision: a.decision || a.status });
      return;
    }
    if (a.sourceSessionKey && a.sourceSessionKey !== s.key) return;
    const p = a.presentation;
    if (!p || !Array.isArray(p.allowedDecisions) || a.expiresAtMs <= Date.now()) return;
    if (s.approvals.size >= 8 || Buffer.byteLength(JSON.stringify(a)) > 16384) {
      publish(s, {type:'notification', text:'An additional or large approval needs the OpenClaw dashboard.'}); return;
    }
    if (s.approvals.has(a.id)) return;
    s.approvals.set(a.id, a);
    publish(s, approvalMessage(a));
  }
  function question(q) {
    const s = byKey.get(q?.sessionKey);
    if (!s?.subscribed || q.agentId && q.agentId !== 'main' || q.status !== 'pending' ||
        !Array.isArray(q.questions) || q.expiresAtMs <= Date.now() || s.questions.has(q.id) || s.questions.size >= 8) return;
    // Passwords, external URLs and secret-store writes belong on the trusted
    // Gateway UI; they must not become spoken text or a glasses option list.
    if (Buffer.byteLength(JSON.stringify(q)) > 16384 || q.questions.some(x => x.isSecret || x.secretStore || x.url)) {
      publish(s, { type: 'notification', text: 'A private or external question needs the OpenClaw dashboard.' }); return;
    }
    s.questions.set(q.id, q);
    publish(s, questionMessage(q));
  }
  async function subscribe(s) {
    if (s.subscribing) return s.subscribing;
    if (s.subscribed) return;
    s.subscribing = (async () => {
    const hello = await client.connect();
    const includeApprovals = hello.auth?.scopes?.includes('operator.approvals') === true;
    s.subscribed = true; // Accept events which arrive before the snapshot response.
    try {
      await client.request('sessions.subscribe', { agentId: 'main', limit: 1, archived: false });
      const result = await client.request('sessions.messages.subscribe', { key: s.key, agentId: 'main', includeApprovals });
      if (!result?.subscribed || result.key !== s.key) throw error('Gateway did not subscribe to the selected session', 502);
      for (const a of result.approvalReplay?.approvals ?? []) approval(s, a);
      if (hello.auth?.scopes?.includes('operator.questions') && hello.features?.methods?.includes('question.list')) {
        for (const q of (await client.request('question.list', {})).questions ?? []) question(q);
      }
    } catch (e) { s.subscribed = false; throw e; }
    })();
    try { await s.subscribing; } finally { s.subscribing = null; }
  }
  async function history(s, limit = 10) {
    ++s.readers; s.touched = Date.now();
    try {
    await subscribe(s);
    const seq = s.chatSeq, agentSeq = s.agentSeq, runId = s.runId, wasUncertain = s.uncertain;
    const result = await client.request('chat.history', { sessionKey: s.key, agentId: 'main',
      limit: Math.max(1, Math.min(10, Number(limit) || 10)), maxBytes: 128 * 1024 });
    if (!Array.isArray(result?.messages)) throw error('Invalid Gateway history', 502);
    if (s.chatSeq === seq && s.runId === runId) {
      s.uncertain = false;
      if (result.inFlightRun?.runId) {
        const changed = s.runId !== result.inFlightRun.runId;
        if (changed) resetRun(s, result.inFlightRun.runId);
        s.busy = true;
        s.text = typeof result.inFlightRun.text === 'string' ? boundedText(result.inFlightRun.text) : '';
        s.sessionAbortable = result.inFlightRun.sessionAbortable === true;
        if (Number.isSafeInteger(result.inFlightRun.startedAt) && result.inFlightRun.startedAt > 0) s.startedAt = result.inFlightRun.startedAt;
        // An older snapshot cannot replace metadata received while history was
        // loading. Replayed events still pass the exact run/sequence fence.
        if (changed || agentSeq === s.agentSeq) plan(s, result.inFlightRun.plan);
        for (const p of Array.isArray(result.inFlightRun.events) ? result.inFlightRun.events.slice(-100) : []) agentEvent(s, p);
      }
      else if (wasUncertain || !s.busy) { s.busy = false; resetRun(s, null); }
    }
    return result.messages.filter(m => ['user', 'assistant'].includes(m.role)).map(m => ({
      ...(typeof (m.id ?? m.messageId) === 'string' ? { id: m.id ?? m.messageId } : {}),
      ...(typeof m.runId === 'string' ? { runId: m.runId } : {}), role: m.role, text: textOf(m),
    })).filter(m => m.text).slice(-limit);
    } finally { --s.readers; }
  }
  async function prompt(id, text, cwd, requestId = randomUUID().replaceAll('-', '')) {
    if (cwd) throw error('OpenClaw does not accept a host working directory');
    if (typeof text !== 'string' || !text.trim() || Buffer.byteLength(text) > 4096 || /\u0000/u.test(text)) throw error('Prompt must contain 1–4096 UTF-8 bytes');
    // The Gateway's suppressCommandInterpretation field requires admin scope.
    // Keep this ordinary-user route unprivileged and refuse command prefixes
    // locally instead of changing the agent's permissions or rewriting text.
    const commandText = text.normalize('NFC').replace(/[\u0001-\u0008\u000B\u000C\u000E-\u001F\u007F]/gu, '').trim();
    if (!commandText) throw error('Prompt must contain visible chat text');
    const abortText = commandText.toLowerCase().replace(/[’`]/gu, "'").replace(/\s+/gu, ' ')
      .replace(/[.!?！？…,，。;；:：'"’”)\]}]+$/u, '').trim();
    if (/^[!/]/u.test(commandText) || ABORT_COMMANDS.has(abortText))
      throw Object.assign(error('Terminal accepts chat text only; use its Interrupt action to stop a run.'), {code:'command_not_supported'});
    if (!/^[a-f0-9]{32}$/u.test(requestId)) throw error('Invalid request ID');
    const prior = requests.get(requestId);
    if (prior) {
      if (prior.id !== id || prior.text !== text) throw error('Request ID already used for another prompt', 409);
      return prior.promise;
    }
    if (requestBits(requestId).every(bit => retired[bit >>> 3] & (1 << (bit & 7))))
      throw error('Request ID has retired; inspect history and use a new ID for a new prompt', 409);
    if (requests.size >= 200) {
      const oldest = [...requests.entries()].find(([, value]) => value.settled);
      if (!oldest) throw error('Too many concurrent prompt requests', 503);
      for (const bit of requestBits(oldest[0])) retired[bit >>> 3] |= 1 << (bit & 7);
      requests.delete(oldest[0]);
    }
    const task = (async () => {
      const s = id ? await resolve(id) : remember(`agent:main:openai-user:hw1:terminal:${randomUUID().replaceAll('-', '')}`);
      if (id) await history(s, 1);
      if (s.busy || s.uncertain) throw error('Selected conversation has an active or uncertain run', 409);
      // Bind before sending: the first stream event may race the RPC reply.
      resetRun(s, requestId); s.busy = true; s.sessionAbortable = false;
      try { await subscribe(s); } catch (e) { s.busy = false; s.runId = null; throw e; }
      const cursor = s.cursor;
      publish(s, { type: 'user_prompt', text }); publish(s, { type: 'status', state: 'busy' });
      try {
        const result = await client.request('chat.send', { sessionKey: s.key, agentId: 'main', message: text,
          idempotencyKey: requestId, deliver: false });
        if (result?.runId !== requestId) throw error('Gateway returned a different run identity', 502);
        return { sessionId: s.id, provider: 'openclaw', runId: requestId, cursor, instanceId };
      } catch (e) {
        s.uncertain = true;
        publish(s, { type: 'error', message: 'Send outcome is uncertain; inspect history before another prompt. No automatic resend.' });
        throw e;
      }
    })();
    const record = { id, text, promise: task, settled: false };
    requests.set(requestId, record);
    task.then(() => { record.settled = true; }, () => { record.settled = true; });
    return task;
  }
  client.onEvent(({ event, payload: p }) => {
    if (event === 'question.requested') { question(p); return; }
    if (event === 'question.resolved') {
      for (const s of sessions.values()) if (s.questions.delete(p?.id)) publish(s, { type: 'question_answer', requestId: p.id, status: p.status });
      return;
    }
    if (event === 'session.approval') { const s = byKey.get(p?.sessionKey); if (s?.subscribed) approval(s, p.approval); return; }
    if (event === 'agent' || event === 'session.tool') {
      const s = byKey.get(p?.sessionKey);
      if (!s?.subscribed || p.agentId && p.agentId !== 'main' || typeof p.runId !== 'string') return;
      if ((!s.runId || !s.busy && !s.uncertain && p.runId !== s.runId) && p.stream === 'lifecycle' && p.data?.phase === 'start') {
        resetRun(s, p.runId); s.busy = true; publish(s, {type:'status',state:'busy'});
      }
      agentEvent(s, p);
      return;
    }
    if (event !== 'chat' || !p || p.agentId && p.agentId !== 'main') return;
    const s = byKey.get(p.sessionKey);
    if (!s?.subscribed || typeof p.runId !== 'string' || !Number.isSafeInteger(p.seq)) return;
    if (!s.runId || p.runId !== s.runId && !s.busy && !s.uncertain) {
      resetRun(s, p.runId); s.busy = true;
    }
    if (p.runId !== s.runId || p.seq <= s.chatSeq) return;
    s.chatSeq = p.seq;
    if (p.state === 'delta') {
      if (typeof p.deltaText !== 'string') return;
      if (p.deltaText) activity(s, 'responding');
      const previous = s.text;
      s.text = textOf(p.message) || (p.replace ? p.deltaText : s.text + p.deltaText);
      if (Buffer.byteLength(s.text) > MAX_TEXT) { s.text = boundedText(s.text); s.uncertain = true; publish(s, { type: 'error', message: 'Reply exceeds local display limit; inspect Gateway history.' }); return; }
      publish(s, s.text.startsWith(previous) ? { type: 'text_delta', text: s.text.slice(previous.length) } : { type: 'text_replace', text: s.text });
    } else if (['final', 'aborted', 'error'].includes(p.state)) {
      const rawFinal = textOf(p.message) || s.text;
      const noVisibleReply = p.state === 'final' && (!rawFinal.trim() || rawFinal.trim() === 'NO_REPLY');
      const final = noVisibleReply ? '' : rawFinal;
      s.text = boundedText(final);
      if (noVisibleReply) publish(s, {type:'notification', text:'Mingus finished without a visible reply.'});
      if (Buffer.byteLength(final) > MAX_TEXT) publish(s, {type:'error', message:'Reply exceeds local display limit; full text remains in Gateway history.'});
      activity(s, undefined);
      s.busy = false; s.uncertain = false; s.approvals.clear(); s.questions.clear();
      publish(s, { type: 'result', success: p.state === 'final', text: s.text, outcome: noVisibleReply ? 'no_visible_reply' : p.state, truncated: Buffer.byteLength(final) > MAX_TEXT, ...stats(s, true) });
      publish(s, { type: 'status', state: 'idle' });
    }
  });
  client.onDisconnect(() => {
    instanceId = randomUUID();
    for (const s of sessions.values()) {
      s.subscribed = false; s.approvals.clear(); s.questions.clear();
      if (s.busy) { s.uncertain = true; publish(s, { type: 'error', message: 'Gateway disconnected. The run may continue; reopen history to reconcile.' }); }
    }
  });
  return {
    listSessions, prompt,
    createSession: async () => ({ sessionId: remember(`agent:main:openai-user:hw1:terminal:${randomUUID().replaceAll('-', '')}`).id }),
    getInfo: async () => { const hello = await client.connect(); return { account: {}, model: 'Mingus / main', version: hello.server.version, provider: 'openclaw',
      capabilities: { sessions: true, history: true, interrupt: true, approvals: hello.auth?.scopes?.includes('operator.approvals') === true, questions: hello.auth?.scopes?.includes('operator.questions') === true } }; },
    getHistory: async (id, limit) => history(await resolve(id), limit),
    getSessionStatus: async (id) => { const s = await resolve(id); await history(s, 1); return state(s); },
    getStatus: id => { const s = sessions.get(id); if (!s) return null; stats(s); return { state: state(s), provider: 'openclaw', ...activityFields(s) }; },
    isBusy: id => Boolean(sessions.get(id)?.busy),
    interrupt: async (id, runId) => {
      const s = await resolve(id);
      if (!s.runId || runId && runId !== s.runId) throw error('No matching selected run', 409);
      if (s.sessionAbortable) throw error('This recovered run requires Stop in the OpenClaw dashboard', 409);
      return client.request('chat.abort', { sessionKey: s.key, agentId: 'main', runId: s.runId, preserveSideRuns: true });
    },
    respondPermission: async (id, decision, requestId) => {
      const s = await resolve(id), a = requestId ? s.approvals.get(requestId) : [...s.approvals.values()][0];
      if (!a || a.expiresAtMs <= Date.now() || !a.presentation.allowedDecisions.includes(decision)) return false;
      const current = (await client.request('approval.get', {id:a.id})).approval;
      if (current?.status !== 'pending' || current.expiresAtMs <= Date.now() ||
          JSON.stringify(current.presentation) !== JSON.stringify(a.presentation)) { if (current) approval(s,current); return false; }
      const result = await client.request('approval.resolve', { id: a.id, kind: a.presentation.kind, decision });
      approval(s, result.approval); return result.applied === true;
    },
    respondQuestion: async (id, answer, requestId) => {
      const s = await resolve(id), q = requestId ? s.questions.get(requestId) : [...s.questions.values()][0];
      if (!q || q.expiresAtMs <= Date.now()) return false;
      let params = { id: q.id, cancel: true };
      if (answer !== 'skip') {
        const answers = typeof answer === 'string' ? JSON.parse(answer) : answer;
        if (!answers || typeof answers !== 'object' || Array.isArray(answers)) return false;
        if (Object.keys(answers).some(k => !q.questions.some(x => x.questionId === k))) return false;
        for (const item of q.questions) {
          const values = answers[item.questionId];
          if (!Array.isArray(values) || !values.length || !item.multiSelect && values.length !== 1 || values.some(v => typeof v !== 'string' || Buffer.byteLength(v) > 512 || !item.isOther && !item.options.some(o => o.label === v))) return false;
        }
        params = { id: q.id, answers: { answers }, resolutionId: randomUUID() };
      }
      const result = await client.request('question.resolve', params);
      s.questions.delete(q.id); publish(s, { type: 'question_answer', requestId: q.id, status: result.status }); return true;
    },
    getInstanceId: () => instanceId,
    getDeviceHistorySnapshot: async (id, limit = 10) => {
      const s = await resolve(id), items = await history(s, limit);
      const last = items.at(-1);
      // Seed the live accumulator even when history already contains the partial
      // reply. Give that matching history entry the same upsert identity.
      if (s.runId && s.text && last?.role === 'assistant' && last.text === s.text && !last.runId) last.runId = s.runId;
      stats(s);
      return { history: items, snapshot: { sessionId: id, runId: s.runId, state: state(s), uncertain: s.uncertain, cursor: s.cursor, ...activityFields(s) },
        liveText: s.text,
        pendingEvents: [...s.approvals.values()].map(approvalMessage).concat([...s.questions.values()].map(questionMessage)),
        recentToolEvents: [...s.tools.values()], recentProgressEvents: [s.planMessage, s.statsMessage].filter(Boolean) };
    },
    getDeviceSnapshot: id => { const s = sessions.get(id); if (!s) throw error('Session not found', 404); stats(s); return { sessionId: id, runId: s.runId, state: state(s), uncertain: s.uncertain, cursor: s.cursor, ...activityFields(s) }; },
    getDeviceEvents: (id, after = 0, max = 4) => { const s = sessions.get(id); if (!s) throw error('Session not found', 404); return { reset: after > s.cursor || after < (s.events[0]?.id ?? 1) - 1, events: s.events.filter(e => e.id > after).slice(0, max), cursor: s.cursor }; },
  };
}
