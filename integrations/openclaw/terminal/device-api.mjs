import { createHash, randomBytes } from 'node:crypto';

const MAX_WIRE = 4096, MAX_EVENTS = 512, MAX_CONTENT = 128 * 1024;
const HEX = /^[a-f0-9]{32}$/u, SESSION = /^hwo_[a-f0-9]{64}$/u;
const bytes = s => Buffer.byteLength(s, 'utf8');
const nonce = () => randomBytes(16).toString('hex');
const fail = (code, message) => { throw Object.assign(new Error(message), { code }); };
const object = v => v && typeof v === 'object' && !Array.isArray(v);
const allowed = (v, keys) => object(v) && Object.keys(v).every(k => keys.includes(k));
const uint = (v, max = 0xffffffff) => Number.isInteger(v) && v >= 0 && v <= max;
const text = (v, max, empty = false) => typeof v === 'string' && (empty || v.length > 0) &&
  bytes(v) <= max && !/[\u0000\ud800-\udfff]/u.test(v.replace(/[\ud800-\udbff][\udc00-\udfff]/gu, ''));
const fields = {
  list: ['offset', 'limit'], open: ['sessionId'], new: [], poll: ['view', 'instance', 'after'],
  prompt: ['view', 'instance', 'text'], interrupt: ['view', 'instance', 'runId'],
  permission: ['view', 'instance', 'pendingId', 'decision'],
  question: ['view', 'instance', 'pendingId', 'questionId', 'optionIds', 'text', 'cancel'],
};
const ERROR_CATEGORIES = new Set(['invalid_request','capacity','host_required','reopen_required','stale_request','request_conflict','command_not_supported']);
const GATEWAY_CODES = new Set(['INVALID_REQUEST','UNAVAILABLE','NOT_LINKED','NOT_PAIRED','FORBIDDEN','UNAUTHORIZED']);
function reportFailure(op, error) {
  // Metadata only: never log an exception message, body, identity or credential.
  const operation = Object.hasOwn(fields, op) ? op : 'invalid';
  const category = ERROR_CATEGORIES.has(error?.code) ? error.code : 'provider_error';
  const gateway = error?.gatewayCode === undefined ? 'none' : GATEWAY_CODES.has(error.gatewayCode) ? error.gatewayCode : 'OTHER';
  console.error(`[Hardware One Terminal] op=${operation} error=${category} gateway=${gateway}`);
}
function chunks(value, limit = 512) {
  const result = []; let part = '', n = 0;
  for (const cp of value) {
    const size = bytes(cp);
    if (n + size > limit) { result.push(part); part = ''; n = 0; }
    part += cp; n += size;
  }
  if (part) result.push(part);
  return result;
}
function title(value) { return chunks(String(value || 'Mingus conversation'), 64)[0] || 'Mingus'; }
const activityFields = value => value.state === 'busy' && ['thinking','tool','responding'].includes(value.activity) ? {activity:value.activity} : {};

// One selected, bounded presentation per authenticated relay identity. This is
// presentation state only: OpenClaw remains the transcript/run/approval owner.
export function createDeviceApi(provider) {
  const devices = new Map();
  let instance = provider.getInstanceId();
  function device(id) {
    if (!/^[a-z0-9][a-z0-9_-]{0,31}$/u.test(id ?? '')) fail('invalid_request', 'Invalid device identity');
    if (!devices.has(id)) {
      if (devices.size >= 8) fail('capacity', 'Too many device identities');
      devices.set(id, { view: null, requests: new Map(), retired: new Uint8Array(8192), serial: Promise.resolve() });
    }
    return devices.get(id);
  }
  function syncInstance() {
    const current = provider.getInstanceId();
    if (instance !== current) {
      instance = current;
      for (const d of devices.values()) { d.view = null; d.requests.clear(); d.retired.fill(0); }
    }
  }
  function event(v, value) {
    const item = { id: ++v.cursor, ...value };
    // A single control must fit even when every character needs JSON escaping.
    if (bytes(JSON.stringify(item)) > 3400) fail('host_required', 'This item needs the OpenClaw dashboard');
    v.events.push(item);
    if (v.events.length > MAX_EVENTS) v.events.shift();
  }
  function content(v, key, kind, value) {
    if (!text(value, MAX_CONTENT, true)) {
      event(v, { type: 'host_required', text: 'This output exceeds the device view limit. Open the OpenClaw dashboard.' }); return;
    }
    if (!v.items.has(key) && v.items.size >= 256) fail('reopen_required', 'This view reached its item limit; reopen recent history');
    const parts = chunks(value), old = v.items.get(key) || [];
    for (let i = 0; i < Math.max(old.length, parts.length); ++i) {
      const part = parts[i] || '';
      if (!old[i]) old[i] = { id: ++v.nextItem, text: null };
      if (old[i].text !== part) { old[i].text = part; event(v, { type: 'content', itemId: old[i].id, kind, text: part }); }
    }
    v.items.set(key, old);
  }
  function clearPending(v, gatewayId) {
    const active = v.pending.keys().next().value;
    for (const [id, p] of v.pending) if (!gatewayId || p.gatewayId === gatewayId) {
      v.pending.delete(id); event(v, { type: 'query_clear', pendingId: id });
    }
    if (active && !v.pending.has(active) && v.pending.size) event(v, queryEvent(v, v.pending.values().next().value));
  }
  function queryEvent(v, p) {
    if (p.kind === 'permission') return { type: 'query', pendingId: p.id, queryType: p.kind, questionId: '',
      question: p.question, options: p.options, multiSelect: false, allowText: false, expiresAtMs: p.expiresAtMs };
    const q = p.questions[p.index];
    return { type: 'query', pendingId: p.id, queryType: p.kind, questionId: `q${p.index}`, question: q.displayQuestion,
      options: q.options.map((x, i) => ({ id: `o${i}`, label: x.label })), multiSelect: Boolean(q.multiSelect),
      allowText: Boolean(q.isOther), expiresAtMs: p.expiresAtMs };
  }
  function offer(v, raw, kind) {
    const gatewayId = raw.requestId || raw.toolUseId;
    if (typeof gatewayId !== 'string' || !Number.isFinite(raw.expiresAtMs) || raw.expiresAtMs <= Date.now()) return;
    if ([...v.pending.values()].some(p => p.gatewayId === gatewayId)) return;
    const p = { id: nonce(), gatewayId, kind, expiresAtMs: raw.expiresAtMs };
    let representable = true;
    if (kind === 'permission') {
      const a = raw.presentation;
      if (!object(a) || !Array.isArray(a.allowedDecisions) || a.externalResolution) representable = false;
      else {
        // Preserve all reviewer-safe facts. Do not use commandPreview in place
        // of commandText or drop host/scope/severity/proposalHash to make it fit.
        p.question = Object.entries(a).filter(([k]) => k !== 'allowedDecisions').map(([k, x]) =>
          `${k}: ${typeof x === 'string' ? x : JSON.stringify(x)}`).join('\n');
        p.options = a.allowedDecisions.map(x => ({ id: x, label: x }));
        representable = text(p.question, 1024) && p.options.length >= 1 && p.options.length <= 3 &&
          p.options.every(x => ['allow-once', 'allow-always', 'deny'].includes(x.id)) && p.options.some(x => x.id === 'deny');
      }
    } else {
      p.questions = raw.questions; p.answers = {}; p.index = 0;
      representable = Array.isArray(p.questions) && p.questions.length >= 1 && p.questions.length <= 3 && p.questions.every(q =>
        object(q) && text(q.questionId, 128) && text(q.question, 1024) && !q.isSecret && !q.secretStore && !q.url &&
        Array.isArray(q.options) && q.options.length <= 4 && (q.options.length || q.isOther === true) &&
        q.options.every(o => text(o.label, 128) && (o.description === undefined || text(o.description, 1024, true))));
      if (representable) {
        p.questions = p.questions.map(q => ({ ...q, displayQuestion: [q.header, q.question,
          ...q.options.filter(o => o.description).map(o => `${o.label}: ${o.description}`)].filter(Boolean).join('\n') }));
        representable = p.questions.every((q, index) => text(q.displayQuestion, 1024) &&
          bytes(JSON.stringify({ id: 0xffffffff, ...queryEvent(v, { ...p, index }), expiresInMs: 0xffffffff })) <= 3400);
      }
    }
    if (representable && v.pending.size < 8) {
      const value = queryEvent(v, p);
      representable = bytes(JSON.stringify({ id: 0xffffffff, ...value })) <= 3400;
      if (representable) { v.pending.set(p.id, p); if (v.pending.size === 1) event(v, value); return; }
    }
    event(v, { type: 'host_required', text: 'A pending request needs the OpenClaw dashboard; its complete details cannot be shown safely here.' });
  }
  function project(v, e) {
    const run = typeof e.runId === 'string' ? e.runId : 'external';
    switch (e.type) {
      case 'user_prompt': content(v, `user:${run}`, 1, e.text); break;
      case 'text_delta': {
        const next = (v.live.get(run) || '') + (typeof e.text === 'string' ? e.text : '');
        if (bytes(next) <= MAX_CONTENT) v.live.set(run, next);
        content(v, `assistant:${run}`, 2, next); break;
      }
      case 'text_replace': if (text(e.text, MAX_CONTENT, true)) v.live.set(run, e.text); content(v, `assistant:${run}`, 2, e.text); break;
      case 'result': content(v, `assistant:${run}`, 2, typeof e.text === 'string' ? e.text : v.live.get(run) || ''); v.live.delete(run); break;
      case 'tool_start': case 'tool_end': case 'tool_update': {
        const name = e.name || e.toolName || 'Tool';
        // ask_user has its own choice cards. Its tool result also contains an
        // internal answer envelope; that must not become another glasses item.
        const summary = (name === 'ask_user' || name === 'openclaw:core:ask_user')
          ? ['Question', e.type === 'tool_start' ? 'Waiting' : e.type === 'tool_end' ? e.isError ? 'Failed' : 'Finished' : 'Update']
          : [name, e.type === 'tool_start' ? 'Running' : e.type === 'tool_end' ? e.isError ? 'Failed' : 'Finished' : 'Update', e.text || e.result || ''];
        content(v, `tool:${run}:${e.toolId || e.toolUseId || e.name || 'tool'}`, 3,
          summary.filter(x => typeof x === 'string' && x).join('\n')); break;
      }
      case 'task_progress':
        if (uint(e.completed, 100) && uint(e.total, 100) && e.completed <= e.total)
          content(v, `plan:${run}`, 3, e.total ? [`Tasks: ${e.completed}/${e.total} complete`, typeof e.current === 'string' ? e.current : ''].filter(Boolean).join('\n') : '');
        break;
      case 'running_stats': {
        const lines = [];
        if (uint(e.durationMs, Number.MAX_SAFE_INTEGER)) lines.push(`Elapsed: ${Math.floor(e.durationMs / 1000)}s`);
        if (uint(e.outputTokens, Number.MAX_SAFE_INTEGER)) lines.push(`Reported output tokens: ${e.outputTokens}`);
        if (lines.length) content(v, `stats:${run}`, 3, lines.join('\n'));
        break;
      }
      case 'status':
        if (['think_end','text_end'].includes(e.state)) break;
        if (e.state === 'idle') clearPending(v);
        { const state = ['busy', 'idle', 'awaiting', 'uncertain'].includes(e.state) ? e.state : 'busy';
          event(v, {type:'state', state, runId:e.runId || null, ...activityFields({...e,state})}); } break;
      case 'permission_request': offer(v, e, 'permission'); break;
      case 'user_question': offer(v, e, 'question'); break;
      case 'permission_result': case 'question_answer': clearPending(v, e.requestId || e.toolUseId); break;
      case 'error': case 'notification': event(v, { type: 'host_required', text: title(e.message || e.text || 'Open the OpenClaw dashboard.') }); break;
      default: break;
    }
  }
  function pump(v) {
    const batch = provider.getDeviceEvents(v.sessionId, v.sourceCursor, 32);
    if (batch.reset) fail('reopen_required', 'The live event window changed; reopen this conversation');
    const first = batch.events[0];
    if (first && ['text_delta', 'text_replace'].includes(first.type)) {
      // Coalesce adjacent deltas for one run into one absolute tail update.
      // Preserve tool/query/state ordering and never materialize several full
      // 128KiB answers before the three-event device page can drain.
      let value = v.live.get(first.runId || 'external') || '', consumed = false;
      for (const e of batch.events) {
        if (e.runId !== first.runId || !['text_delta', 'text_replace'].includes(e.type) || typeof e.text !== 'string') break;
        value = e.type === 'text_replace' ? e.text : value + e.text;
        v.sourceCursor = e.id; consumed = true;
        if (bytes(value) > MAX_CONTENT) break;
      }
      if (!consumed) { project(v, first); v.sourceCursor = first.id; }
      else project(v, { ...first, type: 'text_replace', text: value });
    } else if (first) { project(v, first); v.sourceCursor = first.id; }
    for (const p of v.pending.values()) if (p.expiresAtMs <= Date.now()) clearPending(v, p.gatewayId);
    return batch.events.length > 0;
  }
  async function open(d, id) {
    const captured = instance;
    const initial = await provider.getDeviceHistorySnapshot(id, 10);
    if (provider.getInstanceId() !== captured) fail('reopen_required', 'Gateway reconnected; reopen this conversation');
    const snapshot = initial.snapshot;
    const v = { id: nonce(), sessionId: id, cursor: 0, nextItem: 0, sourceCursor: snapshot.cursor,
      events: [], items: new Map(), live: new Map(), pending: new Map() };
    for (const [i, m] of initial.history.entries()) {
      const key = m.runId ? `${m.role === 'user' ? 'user' : 'assistant'}:${m.runId}` : `history:${m.id || i}`;
      content(v, key, m.role === 'user' ? 1 : m.role === 'tool' ? 3 : 2, m.text);
    }
    if (snapshot.runId && typeof initial.liveText === 'string' && initial.liveText) {
      v.live.set(snapshot.runId, initial.liveText); content(v, `assistant:${snapshot.runId}`, 2, initial.liveText);
    }
    for (const p of initial.pendingEvents || []) project(v, p);
    for (const p of initial.recentToolEvents || []) project(v, p);
    for (const p of initial.recentProgressEvents || []) project(v, p);
    event(v, { type: 'state', state: snapshot.uncertain ? 'uncertain' : snapshot.state, runId: snapshot.runId, ...activityFields(snapshot) });
    d.view = v;
    // Retired view actions carry an invalid view token, so their response cache
    // can go immediately. Keep selection responses separately and bounded.
    for (const [requestId, prior] of d.requests) if (prior.viewBound) d.requests.delete(requestId);
    return { view: v.id, sessionId: id, cursor: 0, state: snapshot.uncertain ? 'uncertain' : snapshot.state, runId: snapshot.runId, ...activityFields(snapshot) };
  }
  function selected(d, r) {
    if (r.instance !== instance || !d.view || d.view.id !== r.view) fail('reopen_required', 'This view has expired; reopen the selected conversation');
    return d.view;
  }
  function pending(v, r, kind) {
    const p = v.pending.get(r.pendingId);
    if (!p || p.kind !== kind || p.expiresAtMs <= Date.now() || v.pending.keys().next().value !== p.id) fail('stale_request', 'This request is no longer pending');
    return p;
  }
  async function perform(d, r, base) {
    if (r.op === 'list') {
      if (!uint(r.offset, 10000) || !uint(r.limit, 10) || r.limit < 1) fail('invalid_request', 'Invalid page');
      const rows = await provider.listSessions(r.limit + 1, undefined, r.offset);
      return { sessions: rows.slice(0, r.limit).map(s => ({ id: s.id, title: title(s.title), state: s.status || 'idle' })),
        nextOffset: rows.length > r.limit ? r.offset + r.limit : null };
    }
    if (r.op === 'open') {
      if (!SESSION.test(r.sessionId ?? '')) fail('invalid_request', 'Invalid session handle');
      return open(d, r.sessionId);
    }
    if (r.op === 'new') return open(d, (await provider.createSession()).sessionId);
    const v = selected(d, r);
    if (r.op === 'poll') {
      if (!uint(r.after) || r.after > v.cursor || r.after < (v.events[0]?.id ?? 1) - 1) fail('reopen_required', 'The retained event window changed; reopen this conversation');
      // Drain the existing presentation first. One large provider delta can
      // expand into hundreds of small native items; pumping ahead would evict
      // those before a three-event poll has a chance to deliver them.
      if (r.after === v.cursor) pump(v);
      const s = provider.getDeviceSnapshot(v.sessionId), events = [];
      let cursor = r.after;
      for (const stored of v.events.filter(e => e.id > r.after)) {
        const e = stored.type === 'query' ? { ...stored, expiresInMs: Math.max(0, stored.expiresAtMs - Date.now()) } : stored;
        if (events.length === 3 || bytes(JSON.stringify({ ...base, events: [...events, e], cursor: e.id, more: true, state: s.state, runId: s.runId, ...activityFields(s) })) > MAX_WIRE) break;
        events.push(e); cursor = e.id;
      }
      return { events, cursor, more: cursor < v.cursor || provider.getDeviceEvents(v.sessionId, v.sourceCursor, 1).events.length > 0,
        state: s.uncertain ? 'uncertain' : s.state, runId: s.runId, ...activityFields(s) };
    }
    pump(v);
    if (r.op === 'prompt') {
      if (!text(r.text, 2047) || !r.text.trim()) fail('invalid_request', 'Prompt must contain 1–2047 UTF-8 bytes');
      const result = await provider.prompt(v.sessionId, r.text, undefined, r.requestId);
      return { accepted: true, runId: result.runId };
    }
    if (r.op === 'interrupt') {
      if (!text(r.runId, 128)) fail('invalid_request', 'Invalid run identity');
      const result = await provider.interrupt(v.sessionId, r.runId);
      return { accepted: result?.aborted === true || result?.ok === true || (Array.isArray(result?.runIds) && result.runIds.includes(r.runId)) };
    }
    if (r.op === 'permission') {
      const p = pending(v, r, 'permission');
      if (!p.options.some(o => o.id === r.decision)) fail('invalid_request', 'Decision was not offered');
      try { return { applied: await provider.respondPermission(v.sessionId, r.decision, p.gatewayId) === true }; }
      finally { clearPending(v, p.gatewayId); }
    }
    const p = pending(v, r, 'question'), q = p.questions[p.index];
    if (r.questionId !== `q${p.index}`) fail('stale_request', 'Question panel changed');
    const forms = ['optionIds', 'text', 'cancel'].filter(k => Object.hasOwn(r, k));
    if (forms.length !== 1) fail('invalid_request', 'Supply one answer form');
    if (r.cancel === true) {
      try { return { applied: await provider.respondQuestion(v.sessionId, 'skip', p.gatewayId) === true, complete: true }; }
      finally { clearPending(v, p.gatewayId); }
    }
    let values;
    if (Object.hasOwn(r, 'text')) {
      if (!q.isOther || !text(r.text, 512)) fail('invalid_request', 'Free text was not offered or exceeds its limit');
      values = [r.text];
    } else {
      if (!Array.isArray(r.optionIds) || !r.optionIds.length || r.optionIds.length > (q.multiSelect ? q.options.length : 1) ||
          new Set(r.optionIds).size !== r.optionIds.length || r.optionIds.some(id => !/^o[0-3]$/u.test(id) || !q.options[Number(id.slice(1))])) fail('invalid_request', 'Answer was not offered');
      values = r.optionIds.map(id => q.options[Number(id.slice(1))].label);
    }
    p.answers[q.questionId] = values;
    if (++p.index < p.questions.length) { event(v, queryEvent(v, p)); return { applied: false, complete: false }; }
    try { return { applied: await provider.respondQuestion(v.sessionId, p.answers, p.gatewayId) === true, complete: true }; }
    finally { clearPending(v, p.gatewayId); }
  }
  return async function handle(request) {
    const r = request;
    let base = { schema: 1, ok: false, requestId: typeof r?.requestId === 'string' && HEX.test(r.requestId) ? r.requestId : '', instance };
    try {
      syncInstance(); base.instance = instance;
      if (!object(r) || r.schema !== 1 || !Object.hasOwn(fields, r.op) || !HEX.test(r.requestId ?? '') ||
          !allowed(r, ['schema', 'op', 'requestId', 'deviceId', ...fields[r.op]]) || bytes(JSON.stringify(r)) > MAX_WIRE + 64) fail('invalid_request', 'Invalid Terminal request');
      const d = device(r.deviceId);
      const action = async () => {
        syncInstance(); base.instance = instance;
        if (['poll', 'prompt', 'interrupt', 'permission', 'question'].includes(r.op)) selected(d, r);
        const mutation = !['list', 'poll'].includes(r.op);
        const fingerprint = JSON.stringify(Object.fromEntries(Object.keys(r).sort().map(key => [key, r[key]])));
        if (mutation && d.requests.has(r.requestId)) {
          const prior = d.requests.get(r.requestId);
          if (prior.fingerprint !== fingerprint) fail('request_conflict', 'Request ID was already used for another action');
          if (!prior.viewBound && prior.response.view && prior.response.view !== d.view?.id) fail('reopen_required', 'This selection request belongs to a retired view');
          return prior.response;
        }
        const viewBound = !['open', 'new'].includes(r.op);
        const hash = createHash('sha256').update(r.requestId).digest();
        const bits = [hash.readUInt16LE(0), hash.readUInt16LE(2), hash.readUInt16LE(4)];
        if (mutation && bits.every(bit => d.retired[bit >>> 3] & (1 << (bit & 7)))) fail('reopen_required', 'This request ID has retired; choose a fresh selection');
        if (mutation && viewBound && [...d.requests.values()].filter(x => x.viewBound).length >= 256) fail('reopen_required', 'This view reached its action limit; reopen the selected conversation');
        let response;
        try {
          const result = await perform(d, r, { ...base, ok: true });
          if (provider.getInstanceId() !== instance) { syncInstance(); base.instance = instance; fail('reopen_required', 'Gateway reconnected; inspect history before retrying an action'); }
          response = { ...base, ok: true, ...result };
        }
        catch (e) {
          reportFailure(r.op, e);
          response = { ...base, error: { code: e.code || 'provider_error', message: e.code ? String(e.message).slice(0, 180) : 'Terminal operation failed; inspect host status before retrying an action' } };
        }
        if (bytes(JSON.stringify(response)) > MAX_WIRE) response = { ...base, error: { code: 'host_required', message: 'This response exceeds the device limit; use the OpenClaw dashboard' } };
        if (mutation) {
          d.requests.set(r.requestId, { fingerprint, response, viewBound });
          const selections = [...d.requests.entries()].filter(([, x]) => !x.viewBound);
          for (const [old] of selections.slice(0, Math.max(0, selections.length - 32))) {
            const digest = createHash('sha256').update(old).digest();
            for (const off of [0, 2, 4]) { const bit = digest.readUInt16LE(off); d.retired[bit >>> 3] |= 1 << (bit & 7); }
            d.requests.delete(old);
          }
        }
        return response;
      };
      const task = d.serial.then(action, action); d.serial = task.catch(() => {}); return await task;
    } catch (e) {
      reportFailure(r?.op, e);
      return { ...base, error: { code: e.code || 'invalid_request', message: String(e.message || 'Invalid Terminal request').slice(0, 180) } };
    }
  };
}
