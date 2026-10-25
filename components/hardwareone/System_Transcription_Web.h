#pragma once
#include "System_BuildConfig.h"
#if ENABLE_WEB_SENSORS && ENABLE_MICROPHONE && ENABLE_DICTATION
#include <esp_http_server.h>

inline void streamTranscriptionPanel(httpd_req_t* req) {
  httpd_resp_send_chunk(req, R"STTHTML(
<details id="transcription-panel" style="margin-top:10px;border:1px solid var(--border);border-radius:4px;padding:10px">
  <summary style="cursor:pointer">Transcription</summary>
  <p id="transcription-provider">Checking provider...</p>
  <div style="display:flex;gap:6px;flex-wrap:wrap">
    <button class="btn btn-primary" id="transcription-start" disabled data-guest-hide>Start</button>
    <button class="btn" id="transcription-stop" disabled data-guest-hide>Stop</button>
    <button class="btn" id="transcription-cancel" disabled data-guest-hide>Cancel</button>
  </div>
  <p id="transcription-status" role="status">Open this panel to begin.</p>
  <label for="transcription-mic">Microphone</label>
  <select id="transcription-mic" disabled data-guest-hide>
    <option value="auto">Auto</option>
    <option value="pdm">Onboard mic</option>
    <option value="g2">G2 glasses</option>
  </select>
  <p id="transcription-mic-note" style="font-size:.85em"></p>
  <label><input type="checkbox" id="transcription-save" disabled> Save transcripts for new sessions</label>
  <p id="transcription-saving" style="font-size:.85em"></p>
  <label for="transcription-live">Recent text on this page</label>
  <textarea id="transcription-live" readonly rows="7" style="width:100%;box-sizing:border-box"></textarea>
  <p style="font-size:.85em">Completed phrases appear here, grouped by speaker when speaker labels are on (Person 1:, Person 2:; ?: for text still waiting for a label); a ~ line is words still being spoken. Vocabulary spellings are display only: the saved transcript keeps the recognized words. The view keeps the most recent 12,000 characters. Stop finishes pending phrases; Cancel discards unfinished work. Leaving this page requests cancellation of its session.</p>
)STTHTML", HTTPD_RESP_USE_STRLEN);
#if ENABLE_LOCAL_STT
  // The vocabulary exists only with the local backend (sttvocab); boards that
  // dictate through the CM5 never see this section (the script skips it).
  httpd_resp_send_chunk(req, R"STTVOCAB(
  <details id="transcription-vocab" style="margin-top:10px" hidden>
    <summary style="cursor:pointer">Vocabulary</summary>
    <p style="font-size:.85em">Names and terms the recognizer should prefer, with the spelling to show. One word each: <code>HardwareOne</code>, or <code>ESP32|esp</code> when the spoken form (lowercase letters) differs from the spelling. Changes apply to the next phrase and need an administrator.</p>
    <div style="display:flex;gap:6px;flex-wrap:wrap">
      <input id="transcription-vocab-term" type="text" maxlength="96" placeholder="HardwareOne or ESP32|esp" data-guest-hide>
      <button class="btn" id="transcription-vocab-add" data-guest-hide>Add</button>
      <button class="btn" id="transcription-vocab-refresh">Refresh</button>
    </div>
    <p id="transcription-vocab-status" role="status" style="font-size:.85em"></p>
    <div id="transcription-vocab-list"></div>
  </details>
)STTVOCAB", HTTPD_RESP_USE_STRLEN);
#endif
  httpd_resp_send_chunk(req, R"STTHTML2(
  <details id="transcription-files" style="margin-top:10px">
    <summary style="cursor:pointer">Saved transcripts</summary>
    <button class="btn" id="transcription-refresh">Refresh</button>
    <p id="transcription-file-status" role="status"></p>
    <div id="transcription-file-list"></div>
    <div style="display:flex;gap:6px;margin-top:6px">
      <button class="btn" id="transcription-prev" disabled>Previous</button>
      <button class="btn" id="transcription-next" disabled>Next</button>
    </div>
    <pre id="transcription-preview" style="white-space:pre-wrap;overflow-wrap:anywhere;max-height:18em;overflow:auto"></pre>
    <a href="/files">Open File Manager</a>
  </details>
</details>
)STTHTML2", HTTPD_RESP_USE_STRLEN);
}

inline void streamTranscriptionJS(httpd_req_t* req) {
  httpd_resp_send_chunk(req, R"STTJS(
<script>
(function(){
'use strict';
const el = id => document.getElementById('transcription-' + id);
const panel = el('panel');
if (!panel) return;
const CAP = 12000, FILE_CAP = 200, PAGE = 10;
let id = '', active = false, busy = false, ended = false, generation = 0, timer = 0;
let pendingAction = null, pendingAck = null, lastPiece = '', savedPreference = false;
let roots = [], files = [], filePage = 0, fileGeneration = 0, fileBusy = false, sessionEpoch = 0;
let capable = false, available = false, serviceBusy = false, canSave = false, saving = false;
let previewAbort = null, micSource = 'auto', micPdm = false, micG2 = false, micSaving = false;
// Live text is kept per phrase so late speaker labels can regroup it, the
// same way the lens page does: a known speaker's phrases flow into one
// paragraph, unlabelled text stays its own "?:" line, the draft is a ~ line.
let phrases = [], labels = new Map(), draft = '', speakersAvailable = false, vocabulary = false, vocabBusy = false;
function controls() {
  el('start').disabled = !capable || !available || serviceBusy || active || !!pendingAction;
  el('stop').disabled = !active || !!pendingAction;
  el('cancel').disabled = !active || !!pendingAction;
  el('save').disabled = !canSave || saving;
  // The source is claimed when a session starts, so only change it between sessions.
  el('mic').disabled = !capable || active || !!pendingAction || micSaving;
}
function renderMic() {
  const available = {auto:true, pdm:micPdm, g2:micG2};
  for (const option of el('mic').options) option.disabled = !available[option.value] && option.value !== micSource;
  if (!micSaving) el('mic').value = micSource;
  el('mic-note').textContent = active ? 'Stop the session to change the microphone.'
    : (!available[micSource] ? 'Selected microphone is not connected right now. ' : '') + 'Applies to the next session.';
}
function clearLive() { phrases = []; labels = new Map(); draft = ''; speakersAvailable = false; lastPiece = ''; el('live').value = ''; }
function renderLive() {
  const turns = [];
  let last = null;
  for (const p of phrases) {
    const speaker = labels.has(p.sequence) ? labels.get(p.sequence) : -1;
    const sameRun = last && (!speakersAvailable || (speaker >= 0 && speaker === last.speaker));
    if (sameRun) last.text += ' ' + p.text;
    else { last = {speaker:speaker, text:p.text}; turns.push(last); }
  }
  let text = turns.map(t => {
    if (!speakersAvailable) return t.text;
    // Display casing of a known speaker's turn only (raw text stays as decoded).
    if (t.speaker >= 0) return 'Person ' + (t.speaker + 1) + ': ' + t.text.charAt(0).toUpperCase() + t.text.slice(1);
    return '?: ' + t.text;
  }).join('\n');
  if (draft) text += (text ? '\n' : '') + '~ ' + draft;
  el('live').value = text.slice(-CAP);
  el('live').scrollTop = el('live').scrollHeight;
}
function clearPrivate() {
  generation++; fileGeneration++;
  id = ''; active = false; pendingAck = pendingAction = null; lastPiece = '';
  roots = []; files = []; filePage = 0; sessionEpoch = 0;
  clearLive(); el('preview').textContent = ''; el('file-list').replaceChildren();
  if (el('vocab-list')) el('vocab-list').replaceChildren();
  el('saving').textContent = ''; el('file-status').textContent = '';
  if (previewAbort) previewAbort.abort();
  controls();
}
function failure(error) {
  if (error.message === 'auth_required') error.status = 401;
  if (error.status === 401 || error.status === 403 || error.status === 410) {
    clearPrivate(); capable = available = false;
    if (error.status !== 410) ended = true;
  }
  el('status').textContent = error.message || 'Request failed; retrying.';
  controls();
}
async function request(path, form) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 12000);
  try {
    const options = {credentials:'same-origin', cache:'no-store', signal:controller.signal};
    if (form) {
      options.method = 'POST'; options.headers = {'Content-Type':'application/x-www-form-urlencoded'};
      options.body = new URLSearchParams(form).toString();
    }
    const response = await fetch(path, options);
    let data;
    try { data = await response.json(); }
    catch (_) { const error = new Error('Invalid server response'); error.status = response.status; throw error; }
    if (!response.ok || !data.success) {
      const error = new Error(data.error || 'Request failed (' + response.status + ')');
      error.status = response.status; throw error;
    }
    return data;
  } finally { clearTimeout(timeout); }
}
function schedule(delay) {
  clearTimeout(timer);
  if (!ended && (panel.open || active || pendingAction || pendingAck)) timer = setTimeout(tick, delay);
}
function apply(data) {
  if (sessionEpoch && sessionEpoch !== data.session) clearPrivate();
  sessionEpoch = data.session;
  if ('savePreference' in data) {
    capable = true; available = !!data.available; canSave = !!data.canSave;
    const firstRoots = !roots.length;
    savedPreference = !!data.savePreference; roots = data.roots || [];
    if (firstRoots && roots.length && el('files').open) loadFiles();
    if (!saving) el('save').checked = savedPreference;
    micSource = data.micSource || 'auto'; micPdm = !!data.micPdm; micG2 = !!data.micG2;
    vocabulary = !!data.vocabulary;
    if (el('vocab')) { el('vocab').hidden = !vocabulary; el('vocab-add').hidden = el('vocab-term').hidden = !canSave; }
    el('provider').textContent = data.continuous
      ? 'On-device transcription: keeps listening between completed phrases.'
      : 'Pi transcription: one recording, followed by one final result.';
  }
  serviceBusy = !!data.busy;
  if (data.id && data.id !== id) {
    id = data.id; pendingAck = null; clearLive();
  }
  active = !!data.active;
  renderMic();
  const state = data.preparing ? 'Preparing microphone...'
    : data.captureActive ? 'Listening' : data.inferenceActive ? 'Transcribing...'
    : active ? 'Finishing...' : data.busy && !data.valid ? 'In use by another app or session.'
    : data.done ? 'Finished' : 'Ready';
  el('status').textContent = data.error || (!id && !available ? (data.unavailable || 'Not available') : state);
  if (data.transcriptEnabled !== undefined) {
    const mode = data.transcriptError ? 'Saving failed: ' + data.transcriptError
      : data.transcriptComplete ? 'Saved transcript finalized.'
      : data.transcriptSaved ? 'Saving accepted text.'
      : data.transcriptEnabled ? 'Saving is on for this session; waiting for text.' : 'Saving is off for this session.';
    el('saving').textContent = mode + ' The checkbox applies to the next session.';
  } else el('saving').textContent = canSave ? 'Changes apply to the next session.' : 'An administrator can change this setting.';
  if (data.textPending && data.receipt) {
    const r = data.receipt, text = data.sttText;
    if (typeof text !== 'string' || r.length !== new TextEncoder().encode(text).length ||
        !Number.isInteger(r.sequence) || r.sequence < 1 || r.sequence > 4294967295 ||
        !Number.isInteger(r.offset) || r.offset < 0 || r.offset > 65535 ||
        !Number.isInteger(r.length) || r.length < 1 || r.length > 256) throw new Error('Invalid transcript receipt');
    const key = id + ':' + r.sequence + ':' + r.offset + ':' + r.length;
    if (lastPiece !== key) {
      // The receipt names the raw bytes; the page shows the display copy
      // (vocabulary spellings) when the device supplied one.
      const shown = typeof data.display === 'string' && data.display ? data.display : text;
      const current = phrases.length ? phrases[phrases.length - 1] : null;
      if (r.offset !== 0 && current && current.sequence === r.sequence) current.text += shown;
      else phrases.push({sequence:r.sequence, text:shown});
      let total = 0;
      for (const p of phrases) total += p.text.length + 1;
      while (phrases.length > 1 && total > 2 * CAP) total -= phrases.shift().text.length + 1;
      lastPiece = key;
    }
    pendingAck = {id:id, sequence:r.sequence, offset:r.offset, length:r.length};
  }
  if (id) {
    if (Array.isArray(data.speakers)) for (const l of data.speakers) {
      if (l && l.ready === true && Number.isInteger(l.sequence)) labels.set(l.sequence, Number.isInteger(l.speaker) && l.speaker >= 0 && l.speaker < 16 ? l.speaker : -1);
    }
    speakersAvailable = speakersAvailable || data.speakersAvailable === true;
    draft = typeof data.draft === 'string' ? data.draft.slice(0, 512) : '';
    renderLive();
  }
  controls();
}
// Vocabulary (sttvocab): a plain-text reply, "OK: N words ..." then DISPLAY<TAB>spoken lines.
function vocabStatus(text) { if (el('vocab-status')) el('vocab-status').textContent = text; }
async function vocabCommand(cmd) {
  if (!vocabulary || vocabBusy || ended) return null;
  vocabBusy = true; const mine = generation;
  try {
    const result = await hw.postFormText('/api/cli', {cmd:cmd});
    if (mine !== generation || ended) return null;
    if (!/^OK/.test(result)) throw new Error(result || 'Request failed');
    return result;
  } catch (error) { if (mine === generation && !ended) vocabStatus(error.message || 'Request failed'); return null; }
  finally { vocabBusy = false; }
}
async function loadVocab() {
  const result = await vocabCommand('sttvocab list');
  const box = el('vocab-list'); if (!result || !box) return;
  const lines = result.split('\n');
  box.replaceChildren();
  let count = 0;
  for (const line of lines.slice(1)) {
    const tab = line.indexOf('\t'); if (tab <= 0 || count >= 256) continue;
    const display = line.slice(0, tab), spoken = line.slice(tab + 1);
    const row = document.createElement('div'); row.style.margin = '4px 0';
    const name = document.createElement('span'); name.textContent = display + (spoken !== display.toLowerCase() ? ' (' + spoken + ')' : '');
    row.append(name);
    if (canSave) {
      const remove = document.createElement('button'); remove.className = 'btn'; remove.textContent = 'Remove'; remove.style.marginLeft = '8px';
      remove.onclick = async () => { if (await vocabCommand('sttvocab remove ' + spoken)) await loadVocab(); };
      row.append(remove);
    }
    box.append(row); count++;
  }
  vocabStatus(count ? count + ' term(s). Spoken form in parentheses when it differs.' : 'No terms yet.');
}
if (el('vocab')) {
  el('vocab').addEventListener('toggle', () => { if (el('vocab').open) loadVocab(); });
  el('vocab-refresh').onclick = loadVocab;
  el('vocab-add').onclick = async () => {
    const term = el('vocab-term').value.trim();
    if (!term || /\s/.test(term)) { vocabStatus('Enter one term without spaces (DISPLAY or DISPLAY|spoken).'); return; }
    if (await vocabCommand('sttvocab add ' + term)) { el('vocab-term').value = ''; await loadVocab(); }
  };
}
async function tick() {
  if (ended || busy) return;
  busy = true;
  const mine = generation;
  try {
    if (pendingAction) {
      const action = pendingAction; pendingAction = null;
      const data = await request('/api/transcription', action);
      if (mine !== generation || ended) { if (action.action === 'start' && data.id) cancelOnExit(data.id); return; }
      if (action.action === 'cancel') pendingAck = null;
      if (data.id && data.id !== id) { id = data.id; pendingAck = null; clearLive(); }
      active = true;
    } else if (pendingAck) {
      const ack = pendingAck;
      await request('/api/transcription/ack', ack);
      if (mine !== generation || ended) return;
      if (pendingAck === ack) pendingAck = null;
    } else {
      const data = await request('/api/transcription' + (id && active ? '?id=' + encodeURIComponent(id) : ''));
      if (mine !== generation || ended) return;
      apply(data);
    }
  } catch (error) {
    if (mine === generation && !ended) {
      if (error.status === 409) pendingAck = null; // Reconcile a receipt another same-session view advanced.
      failure(error);
    }
  } finally {
    busy = false;
    controls();
    schedule(pendingAck || pendingAction ? 80 : active ? 500 : 2500);
  }
}
function action(name) {
  if (ended || pendingAction || (name !== 'start' && !id)) return;
  pendingAction = {action:name};
  if (name !== 'start') pendingAction.id = id;
  controls(); schedule(0);
}
el('start').onclick = () => action('start');
el('stop').onclick = () => action('stop');
el('cancel').onclick = () => action('cancel');
panel.addEventListener('toggle', () => { if (panel.open) schedule(0); });
el('save').onchange = async function() {
  if (!canSave || saving) return;
  saving = true; controls(); const mine = generation;
  const value = this.checked;
  try {
    const result = await hw.postFormText('/api/cli', {cmd:'sttsavetranscripts ' + (value ? '1' : '0')});
    if (mine !== generation || ended) return;
    if (/^(Error|Failed)/i.test(result)) throw new Error(result);
    savedPreference = value;
    el('saving').textContent = 'Saved. This change applies to the next session.';
  } catch (error) { if (mine === generation && !ended) { this.checked = savedPreference; failure(error); } }
  finally { saving = false; controls(); }
};
el('mic').onchange = async function() {
  if (micSaving || active) return;
  micSaving = true; controls(); const mine = generation;
  const value = this.value;
  try {
    const result = await hw.postFormText('/api/cli', {cmd:'micsource ' + value});
    if (mine !== generation || ended) return;
    if (/^(Error|Failed)/i.test(result)) throw new Error(result);
    micSource = value;
  } catch (error) { if (mine === generation && !ended) failure(error); }
  finally { micSaving = false; renderMic(); controls(); }
};
async function limitedText(response, cap, signal) {
  if (!response.ok) { const error = new Error('File request failed (' + response.status + ')'); error.status = response.status; throw error; }
  if (!response.body || !response.body.getReader) throw new Error('Use Download in this browser.');
  const reader = response.body.getReader(), decoder = new TextDecoder();
  let text = '', count = 0, truncated = false;
  try {
    for (;;) {
      if (signal && signal.aborted) throw new Error('Request cancelled');
      const part = await reader.read();
      if (part.done) break;
      const take = Math.min(part.value.length, cap - count);
      text += decoder.decode(part.value.subarray(0, take), {stream:true}); count += take;
      if (take < part.value.length || count === cap) { truncated = true; break; }
    }
    text += decoder.decode();
  } finally { await reader.cancel(); }
  return {text:text, truncated:truncated};
}
function renderFiles() {
  const box = el('file-list'); box.replaceChildren();
  const start = filePage * PAGE;
  files.slice(start, start + PAGE).forEach(file => {
    const row = document.createElement('div'); row.style.margin = '8px 0';
    const name = document.createElement('div'); name.textContent = file.name + ' (' + file.tier + ')';
    const preview = document.createElement('button'); preview.className = 'btn'; preview.textContent = 'Preview';
    preview.onclick = () => previewFile(file);
    const link = document.createElement('a'); link.className = 'btn'; link.textContent = 'Download';
    link.href = '/api/files/read?name=' + encodeURIComponent(file.path); link.download = file.name;
    row.append(name, preview, link); box.append(row);
  });
  el('prev').disabled = filePage === 0; el('next').disabled = start + PAGE >= files.length;
}
async function fileSessionStillCurrent(expected) {
  const data = await request('/api/transcription?identity=1');
  if (data.session === expected) return true;
  clearPrivate(); capable = available = false;
  el('status').textContent = 'Web session changed. Reopen Transcription.';
  schedule(0); return false;
}
async function loadFiles() {
  if (ended || fileBusy || !roots.length) return;
  fileBusy = true; const mine = ++fileGeneration, session = generation, owner = sessionEpoch;
  if (previewAbort) previewAbort.abort();
  el('preview').textContent = ''; el('file-status').textContent = 'Loading...';
  const found = []; let unavailable = 0;
  try {
    for (const root of roots) {
      const response = await fetch('/api/files/list?path=' + encodeURIComponent(root), {credentials:'same-origin', cache:'no-store'});
      const body = await limitedText(response, 131072);
      if (session !== generation || mine !== fileGeneration || ended) return;
      if (body.truncated) throw new Error('Folder is too large for this panel; use File Manager.');
      const data = JSON.parse(body.text);
      if (!data.success) { unavailable++; continue; }
      for (const f of data.files || []) {
        if (found.length >= FILE_CAP) break;
        if (f.type !== 'file' || !(f.perms & 1) || typeof f.name !== 'string' ||
            !f.name.endsWith('.txt') || /[\/\\\x00-\x1f]/.test(f.name) || f.name === '..') continue;
        found.push({name:f.name, path:root + '/' + f.name, tier:root.startsWith('/sd/') ? 'SD' : 'Internal'});
      }
    }
    if (!await fileSessionStillCurrent(owner) || session !== generation || mine !== fileGeneration || ended) return;
    files = found.sort((a,b) => b.name.localeCompare(a.name)); filePage = 0; renderFiles();
    el('file-status').textContent = files.length ? files.length + ' file(s)' + (files.length === FILE_CAP ? ' shown; use File Manager for more.' : '')
      : unavailable ? 'No saved files found on available storage.' : 'No saved transcripts yet.';
  } catch (error) {
    if (session === generation && mine === fileGeneration && !ended) { el('file-status').textContent = error.message; if (error.status === 401 || error.status === 403) failure(error); }
  } finally { fileBusy = false; }
}
async function previewFile(file) {
  if (previewAbort) previewAbort.abort();
  previewAbort = new AbortController();
  const signal = previewAbort.signal, mine = ++fileGeneration, session = generation, owner = sessionEpoch;
  el('preview').textContent = 'Loading preview...';
  try {
    const response = await fetch('/api/files/read?name=' + encodeURIComponent(file.path), {credentials:'same-origin', cache:'no-store', signal:signal});
    const body = await limitedText(response, 16384, signal);
    if (!await fileSessionStillCurrent(owner) || session !== generation || mine !== fileGeneration || ended || signal.aborted) return;
    el('preview').textContent = body.text + (body.truncated ? '\n[Preview limited to 16 KiB. Download for the full file.]' : '');
  } catch (error) {
    if (session === generation && mine === fileGeneration && !ended && !signal.aborted) { el('preview').textContent = error.message; if (error.status === 401 || error.status === 403) failure(error); }
  }
}
el('refresh').onclick = loadFiles;
el('files').addEventListener('toggle', () => { if (el('files').open) loadFiles(); });
el('prev').onclick = () => { if (filePage) { filePage--; renderFiles(); } };
el('next').onclick = () => { if ((filePage + 1) * PAGE < files.length) { filePage++; renderFiles(); } };
function cancelOnExit(owner) {
  fetch('/api/transcription', {method:'POST', credentials:'same-origin', keepalive:true,
    headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:new URLSearchParams({action:'cancel',id:owner}).toString()}).catch(() => {});
}
window.addEventListener('pagehide', () => {
  const owner = id; ended = true; clearTimeout(timer); clearPrivate();
  if (owner) cancelOnExit(owner);
});
window.addEventListener('pageshow', event => { if (event.persisted) { ended = false; schedule(0); } });
schedule(0);
})();
</script>
)STTJS", HTTPD_RESP_USE_STRLEN);
}
#endif
