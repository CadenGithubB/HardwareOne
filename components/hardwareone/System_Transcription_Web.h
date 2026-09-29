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
  <label><input type="checkbox" id="transcription-save" disabled> Save transcripts for new sessions</label>
  <p id="transcription-saving" style="font-size:.85em"></p>
  <label for="transcription-live">Recent text on this page</label>
  <textarea id="transcription-live" readonly rows="7" style="width:100%;box-sizing:border-box"></textarea>
  <p style="font-size:.85em">Completed phrases appear here; this is not word-by-word streaming. The view keeps the most recent 12,000 characters. Stop finishes pending phrases; Cancel discards unfinished work. Leaving this page requests cancellation of its session.</p>
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
)STTHTML", HTTPD_RESP_USE_STRLEN);
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
let previewAbort = null;
function controls() {
  el('start').disabled = !capable || !available || serviceBusy || active || !!pendingAction;
  el('stop').disabled = !active || !!pendingAction;
  el('cancel').disabled = !active || !!pendingAction;
  el('save').disabled = !canSave || saving;
}
function clearPrivate() {
  generation++; fileGeneration++;
  id = ''; active = false; pendingAck = pendingAction = null; lastPiece = '';
  roots = []; files = []; filePage = 0; sessionEpoch = 0;
  el('live').value = ''; el('preview').textContent = ''; el('file-list').replaceChildren();
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
    el('provider').textContent = data.continuous
      ? 'On-device transcription: keeps listening between completed phrases.'
      : 'Pi transcription: one recording, followed by one final result.';
  }
  serviceBusy = !!data.busy;
  if (data.id && data.id !== id) {
    id = data.id; lastPiece = ''; pendingAck = null; el('live').value = '';
  }
  active = !!data.active;
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
      el('live').value = (el('live').value + (r.offset === 0 && el('live').value ? '\n' : '') + text).slice(-CAP);
      el('live').scrollTop = el('live').scrollHeight;
      lastPiece = key;
    }
    pendingAck = {id:id, sequence:r.sequence, offset:r.offset, length:r.length};
  }
  controls();
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
      if (data.id && data.id !== id) { id = data.id; lastPiece = ''; pendingAck = null; el('live').value = ''; }
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
