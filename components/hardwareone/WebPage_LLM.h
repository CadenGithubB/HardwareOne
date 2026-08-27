#ifndef WEBPAGE_LLM_H
#define WEBPAGE_LLM_H

#include "System_BuildConfig.h"

// Forward declare httpd_handle_t
#ifndef HW_HTTPD_TYPES_DEFINED
  #define HW_HTTPD_TYPES_DEFINED 1
  struct httpd_req;
  typedef struct httpd_req httpd_req_t;
  typedef void* httpd_handle_t;
#endif

#if ENABLE_LLM_BACKEND && ENABLE_HTTP_SERVER
void registerLLMHandlers(httpd_handle_t server);
#else
inline void registerLLMHandlers(httpd_handle_t) {}
#endif

#if ENABLE_LLM_BACKEND && ENABLE_HTTP_SERVER

inline void streamLLMInner(httpd_req_t* req, const String& username) {

  // ── CSS ──────────────────────────────────────────────────────────────────
  httpd_resp_send_chunk(req, R"CSS(
<style>
.qa-wrap {
  max-width:980px;margin:0 auto;display:flex;flex-direction:column;
  height:72vh;min-height:320px;padding:0 12px;
}

/* ── Status bar ── */
.qa-bar {
  display:flex;align-items:center;gap:8px;flex-wrap:wrap;
  background:var(--panel-bg);border:1px solid var(--border);border-radius:10px;
  padding:8px 12px;margin-bottom:10px;font-size:.82em;color:var(--muted);
}
.qa-bar-dot{width:7px;height:7px;border-radius:50%;flex-shrink:0}
.dot-ready{background:#4caf50}.dot-busy{background:#ff9800;animation:qa-pulse 1s ease-in-out infinite}.dot-off{background:#555}
@keyframes qa-pulse{0%,100%{opacity:1}50%{opacity:.3}}
.qa-bar-state{font-weight:600;color:var(--panel-fg)}
/* Measured load progress. Determinate on purpose — no indeterminate sweep
   animation, because a sweep implies motion we cannot actually observe. When
   there is no measurement the bar is hidden entirely rather than faked. */
.qa-load{display:inline-block;width:120px;height:6px;border-radius:3px;
         background:var(--border);overflow:hidden;flex-shrink:0}
.qa-load-fill{display:block;height:100%;width:0;background:var(--accent);
              transition:width .5s linear}
.qa-bar-meta{color:var(--muted);flex:1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.qa-bar select{
  background:var(--panel-bg);color:var(--panel-fg);border:1px solid var(--border);
  border-radius:5px;padding:3px 6px;font-size:.82em;max-width:160px;
  align-self:center;margin-top:2px;
}
.qa-bar .btn{padding:3px 10px;font-size:.8em}

/* ── Q&A list ── */
.qa-list {
  flex:1;overflow-y:auto;display:flex;flex-direction:column;gap:14px;
  padding:4px 2px 8px;
}
.qa-pair {
  display:flex;flex-direction:column;gap:6px;
}
.qa-q-row, .qa-a-row {
  display:flex;gap:10px;align-items:flex-start;
}
.qa-label {
  font-family:'Courier New',monospace;font-size:.82em;font-weight:700;
  padding:2px 0;min-width:18px;flex-shrink:0;line-height:1.6;
  user-select:none;
}
.qa-label-q{color:var(--panel-fg);font-size:.9em}
.qa-label-a{color:var(--panel-fg);font-size:.9em}
.qa-q-text {
  font-size:.92em;color:var(--muted);line-height:1.5;padding-top:2px;
  font-style:italic;
}
.qa-a-text {
  font-size:.95em;color:var(--panel-fg);line-height:1.6;padding-top:2px;
  flex:1;word-break:break-word;
}
.qa-a-text.streaming::after {
  content:'▌';animation:qa-blink .7s step-end infinite;color:var(--accent);
}
@keyframes qa-blink{0%,100%{opacity:1}50%{opacity:0}}
.qa-meta {
  font-size:.75em;color:var(--muted);margin-left:28px;font-family:'Courier New',monospace;
}
.qa-sys {
  font-size:.82em;color:var(--muted);font-style:italic;text-align:center;padding:6px 0;
}
.qa-err {
  font-size:.85em;color:var(--danger);margin-left:28px;
}

/* ── Input row ── */
.qa-input-wrap {
  margin-top:10px;border-top:1px solid var(--border);padding-top:10px;
}
.qa-input-row {
  display:flex;align-items:center;gap:8px;
}
.qa-q-prefix {
  font-family:'Courier New',monospace;font-size:1em;font-weight:700;
  color:var(--panel-fg);flex-shrink:0;user-select:none;
}
.qa-input-row textarea {
  flex:1;background:var(--panel-bg);color:var(--panel-fg);
  border:1px solid var(--border);border-radius:8px;
  padding:10px 12px;font-family:inherit;font-size:.95em;
  resize:none;min-height:44px;max-height:120px;line-height:1.4;
}
.qa-input-row textarea:focus{outline:none;border-color:var(--link)}
.qa-input-row textarea:disabled{opacity:.5}

/* ── Guided-ask strip ── */
.qa-guided {
  display:flex;align-items:center;gap:8px;flex-wrap:wrap;
  margin-bottom:8px;font-size:.82em;color:var(--muted);
}
.qa-guided-lbl{font-weight:600;color:var(--panel-fg);flex-shrink:0}
.qa-guided select {
  background:var(--panel-bg);color:var(--panel-fg);border:1px solid var(--border);
  border-radius:5px;padding:3px 6px;font-size:1em;max-width:260px;
}

/* ── Retry button ── */
.qa-retry{display:block;margin-left:28px;margin-top:4px;font-size:.85em;padding:4px 10px;width:fit-content}
.qa-a-old{opacity:.45;font-size:.88em;border-left:2px solid var(--border);padding-left:8px;margin-top:2px}

/* ── Advanced ── */
.qa-adv-body{display:none;flex-wrap:wrap;gap:8px;margin-top:6px;
  padding:8px 10px;border:1px solid var(--border);border-radius:6px;
  font-size:.82em;color:var(--muted);align-items:center}
.qa-adv-body.open{display:flex}
.qa-adv-body label{display:flex;align-items:center;gap:4px}
.qa-adv-body input[type=number]{width:58px;padding:3px 5px;border:1px solid var(--border);
  border-radius:4px;background:var(--panel-bg);color:var(--panel-fg);font-size:.82em}
.qa-adv-body select{padding:3px 6px;border:1px solid var(--border);border-radius:4px;
  background:var(--panel-bg);color:var(--panel-fg);font-size:.82em}
.qa-adv-hint{font-size:.73em;color:var(--muted);font-style:italic;width:100%}

/* ── Do: suggestion ── */
.qa-do-suggestion {
  display:flex;align-items:center;gap:8px;
  padding:8px 12px;background:var(--panel-bg);
  border:1px solid var(--accent);border-radius:6px;
}
.qa-do-cmd {
  font-family:'Courier New',monospace;font-size:.95em;color:var(--panel-fg);
  background:var(--bg);border:1px solid var(--border);border-radius:4px;
  padding:4px 8px;flex:1;outline:none;
}
.qa-do-cmd:focus{border-color:var(--accent)}
.qa-do-run {
  background:var(--accent);color:#fff;border:none;border-radius:4px;
  padding:6px 16px;cursor:pointer;font-size:.9em;white-space:nowrap;
}
.qa-do-run:hover{opacity:.85}
.qa-do-result {
  margin-top:6px;padding:6px 10px;background:var(--panel-bg);
  border:1px solid var(--border);border-radius:4px;
  font-family:'Courier New',monospace;font-size:.85em;color:var(--muted);
  white-space:pre-wrap;
}
</style>
)CSS", HTTPD_RESP_USE_STRLEN);

  // ── HTML ─────────────────────────────────────────────────────────────────
  httpd_resp_send_chunk(req, R"HTML(
<div class='qa-wrap'>

  <div class='qa-bar'>
    <span class='qa-bar-dot dot-off' id='qa-dot'></span>
    <span class='qa-bar-state' id='qa-state'>Connecting...</span>
    <span class='qa-bar-meta' id='qa-meta'></span>
    <!-- Measured load progress. Hidden unless the host is actually reporting a
         percentage, so it never implies knowledge we do not have (an onboard
         load has no such signal, and neither does external-server mode). -->
    <span class='qa-load' id='qa-load' style='display:none'>
      <span class='qa-load-fill' id='qa-load-fill'></span>
    </span>
    <select id='qa-model' data-guest-hide><option value=''>Loading...</option></select>
    <button class='btn' onclick='qaLoadModel()' data-guest-hide>Load</button>
    <button class='btn' onclick='qaUnloadModel()' data-guest-hide>Unload</button>
    <button class='btn' id='qa-adv-btn' onclick='qaToggleAdv()' data-guest-hide>Advanced</button>
  </div>

  <div class='qa-list' id='qa-list'>
    <div class='qa-sys' id='qa-init-msg'>Connecting to LLM engine...</div>
  </div>

  <div class='qa-input-wrap'>
    <div class='qa-guided' id='qa-guided' style='display:none' data-guest-hide>
      <span class='qa-guided-lbl'>Guided:</span>
      <select id='qa-group' title='Question group'></select>
      <select id='qa-tpl' title='Question template'></select>
      <select id='qa-ent' title='Subject' style='display:none'></select>
    </div>
    <div class='qa-input-row'>
      <span class='qa-q-prefix'>Q:</span>
      <textarea id='qa-input' rows='2'
        placeholder='Ask about Hardware One...' disabled data-guest-hide></textarea>
      <button id='qa-ask' class='btn' disabled onclick='qaAsk()' data-guest-hide>Ask</button>
      <!-- Do: mode, as a real button rather than only a typed prefix. It is the
           one path where a model's answer becomes an executed device command, so
           it must be discoverable and reachable without a pointer: this is
           tab-focusable and Enter/Space-activatable for free. Hidden entirely
           unless the LOADED MODEL declares it can suggest commands. -->
      <button id='qa-do' class='btn' style='display:none' disabled onclick='qaAsk("do")'
              title='Ask for a Hardware One command instead of an answer'
              data-guest-hide>Do:</button>
      <button id='qa-stop' class='btn' style='display:none' onclick='qaStop()' data-guest-hide>Stop</button>
    </div>
    <div class='qa-adv-body' id='qa-adv-body' data-guest-hide>
      <label title='Temperature override — blank uses the saved llmtemperature setting'>Temp:<input type='number' id='qa-temp' class='input-fit' placeholder='saved' min='0' max='2' step='0.05'></label>
      <label title='Sentence-limit override — blank uses the saved llmsentencelimit setting'>Sentences:<input type='number' id='qa-sentlimit' class='input-fit' placeholder='saved' min='0' max='20'></label>
      <label title='Rep-penalty override — blank uses the saved llmreppenalty setting'>Rep:<input type='number' id='qa-repen' class='input-fit' placeholder='saved' min='1' max='5' step='0.05'></label>
      <div class='qa-adv-hint'>Blank fields use your saved device settings. Type a value to override it for this one message.</div>
    </div>
  </div>

</div>
)HTML", HTTPD_RESP_USE_STRLEN);

  // ── JavaScript ───────────────────────────────────────────────────────────
  // The compiler never parses what follows — it is one opaque string literal to
  // the build. This page's logic is covered on the host instead, by
  // tools/webui/tests/test_llm_page.py, which extracts THIS block and runs it.
  httpd_resp_send_chunk(req, R"JS(
<script>
(function(){
  var list      = hw.$('qa-list');
  var inputEl   = hw.$('qa-input');
  var askBtn    = hw.$('qa-ask');
  var stopBtn   = hw.$('qa-stop');
  var doBtn     = hw.$('qa-do');
  var dot       = hw.$('qa-dot');
  var stateEl   = hw.$('qa-state');
  var loadEl     = hw.$('qa-load');
  var loadFillEl = hw.$('qa-load-fill');
  var metaEl    = hw.$('qa-meta');
  var modelSel  = hw.$('qa-model');
  var initMsg   = hw.$('qa-init-msg');
  var busy      = false;
  var abortCtrl = null;
  var currentCtx = null; // Track the most recent Q&A context
  var lastShownError = null; // Avoid repeating the same error message
  // fetchStatus now runs on a heartbeat (see below), so its addSys() lines are
  // EDGE-TRIGGERED off this signature. Without it a poll would reprint
  // "Model loaded: …" into the chat log every few seconds.
  var lastAnnounced  = null;
  var lastState      = '';   // drives the heartbeat cadence (see scheduleBeat)
  // Last host presence seen on a status poll. Kept so a load REJECTION can say
  // WHY — the reject arrives on its own request and carries no host context,
  // and "not available right now" alone reads as broken when the CM5 is simply
  // still starting. '' until the first poll, so neither branch fires blind.
  var lastHostMode   = '';
  var lastHostFresh  = null;
  // Whether the ACTIVE backend supports Do:-mode. Only the on-device model
  // does; see llmBackendSupportsCommandMode. Assumed true until the first
  // status lands so a fast typist is not refused on a stale default.
  var cmdModeOk      = null;   // null = no status yet; see syncDoAffordance

  // ── helpers ──
  function setDot(cls) {
    dot.className = 'qa-bar-dot ' + cls;
  }
  function setReady(isReady) {
    inputEl.disabled = !isReady || busy;
    askBtn.disabled  = !isReady || busy;
    syncDoAffordance(isReady);
  }

  // The Do: button exists only while the loaded model has declared it may
  // suggest runnable commands. Hidden rather than disabled when unavailable: a
  // greyed-out button invites "why can't I click this", and the honest answer
  // depends on which model is loaded, which is not something a tooltip conveys.
  //
  // cmdModeOk is deliberately tri-state. It starts null (no status has landed
  // yet) and only a poll makes it true or false, so the button does not flicker
  // into existence on page load before the device has said anything.
  function syncDoAffordance(isReady) {
    if (!doBtn) return;
    var allowed = (cmdModeOk === true);
    doBtn.style.display = allowed ? '' : 'none';
    doBtn.disabled = !allowed || !isReady || busy;
  }
  function scrollBottom() {
    list.scrollTop = list.scrollHeight;
  }
  function addSys(msg) {
    var d = document.createElement('div');
    d.className = 'qa-sys';
    d.textContent = msg;
    list.appendChild(d);
    scrollBottom();
  }

  // ── advanced toggle ──
  window.qaToggleAdv = function() {
    var b = hw.$('qa-adv-body');
    var t = hw.$('qa-adv-btn');
    var open = b.classList.toggle('open');
    t.textContent = open ? 'Hide Advanced' : 'Advanced';
  };

  // ── status poll ──
  function fetchStatus(afterGen, metaEl2) {
    hw.fetchJSON('/api/llm/status')
      .then(function(j){
        // Narrate only real transitions. afterGen still suppresses narration
        // entirely, but now also ADOPTS the signature it saw — it observed the
        // state and chose not to announce it, so the next heartbeat must not
        // announce it either. (Otherwise every finished answer, and every
        // explicit unload, would be trailed by a stray status line.)
        lastState = j.state;
        // One place to retire the bar: every non-LOADING state hides it, so no
        // branch can leave a stale fill behind after a load ends or fails.
        if (loadEl && j.state !== 'LOADING') loadEl.style.display = 'none';
        if (typeof j.hostMode === 'string')   lastHostMode  = j.hostMode;
        if (typeof j.hostFresh === 'boolean') lastHostFresh = j.hostFresh;
        if (typeof j.cmdMode === 'boolean' && j.cmdMode !== cmdModeOk) {
          cmdModeOk = j.cmdMode;
          syncDoAffordance(!askBtn.disabled || busy);
        }
        var sig = j.state + '|' + (j.model || '') + '|' + (j.error || '');
        var announce = !afterGen && sig !== lastAnnounced;
        lastAnnounced = sig;

        if (j.state === 'READY') {
          var modelName = (j.model||'').split('/').pop().replace(/\.bin$/i,'') || 'model';
          metaEl.textContent = modelName;
          setDot('dot-ready');
          stateEl.textContent = 'Ready';
          setReady(true);
          if (announce) {
            if (initMsg) { initMsg.remove(); initMsg = null; }
            var modelName = (j.model||'').split('/').pop() || j.model || 'model';
            var details = [];
            if (j.arch) details.push(j.arch);
            if (j.quant) details.push(j.quant);
            if (j.dim) details.push('dim=' + j.dim);
            if (j.vocab) details.push('vocab=' + j.vocab);
            if (j.layers) details.push('layers=' + j.layers);
            if (j.heads) details.push('heads=' + j.heads);
            if (j.kvHeads && j.kvHeads !== j.heads) details.push('kv_heads=' + j.kvHeads);
            if (j.seqLen) details.push('seq_len=' + j.seqLen);
            if (j.ctxMax > 0 && j.ctxMax < j.seqLen) details.push('ctx=' + j.ctxMax + ' (auto-fit)');
            if (j.psramKB) details.push(j.psramKB + 'KB PSRAM');
            var msg = 'Model loaded: ' + modelName;
            if (details.length > 0) msg += ' (' + details.join(' · ') + ')';
            addSys(msg);
            // Degraded context (PSRAM was low at load): call it out prominently on
            // its own line — the details parenthetical above is too easy to miss.
            if (j.ctxWarn && j.ctxWarning) addSys('⚠ ' + j.ctxWarning);
          }
        } else if (j.state === 'GENERATING') {
          setDot('dot-busy');
          stateEl.textContent = 'Generating';
        } else if (j.state === 'LOADING') {
          // Reachable without this page having asked for it: a remote load is a
          // host-side llama-server restart that outlives any one request, and a
          // load can equally be started from the OLED, the lens or the CLI.
          // Deliberately silent — qaLoadModel() already narrated the click, and
          // the state pill is the honest place for "still working on it".
          setDot('dot-busy');
          // loadPct is MEASURED on the host (resident bytes / model bytes), not
          // a timer and not an animation — see cm5LlmLoadPercent. It is capped
          // below 100 there because the last ~4% of wall time (KV/compute
          // buffers + warmup) is unobservable, so the bar parks near the end
          // rather than claiming a completion it cannot see.
          var pct = (typeof j.loadPct === 'number') ? j.loadPct : 0;
          if (pct > 0) {
            stateEl.textContent = 'Loading ' + pct + '%';
            hw.show(loadEl);
            if (loadFillEl) loadFillEl.style.width = pct + '%';
          } else {
            // No host signal (onboard engine, external-server mode, or the
            // first second before a sample). Indeterminate, and say so.
            stateEl.textContent = 'Loading...';
            hw.hide(loadEl);
          }
          setReady(false);
        } else if (j.state === 'UNLOADED') {
          setDot('dot-off');
          stateEl.textContent = 'No model';
          metaEl.textContent = '';
          setReady(false);
          if (announce) {
            if (initMsg) initMsg.textContent = 'No model loaded. Select a model and click Load.';
            else addSys('No model loaded. Select a model and click Load.');
          }
        } else if (j.state === 'ERROR') {
          setDot('dot-off');
          stateEl.textContent = 'Error';
          metaEl.textContent = '';
          setReady(false);
          if (announce && j.error && j.error !== lastShownError) {
            lastShownError = j.error;
            if (initMsg) { initMsg.remove(); initMsg = null; }
            var specs = [];
            if (j.arch && j.quant) specs.push(j.arch + ' ' + j.quant);
            if (j.params && j.params !== '0x0x0') specs.push(j.params);
            if (j.dim)    specs.push('dim=' + j.dim);
            if (j.layers) specs.push(j.layers + ' layers');
            if (j.vocab)  specs.push('vocab=' + j.vocab);
            var specStr = specs.length > 0 ? ' (' + specs.join(' · ') + ')' : '';
            addSys('Load failed' + specStr + ': ' + j.error);
          }
        }

        // Show/hide the guided-ask strip and refetch its menu when the model's
        // menu generation changes (model load/unload). See LLM_GUIDED_MENU_SPEC §7.
        menuSync(j.menu);

        // Append per-answer stats if we just finished generating
        if (afterGen && metaEl2 && (j.lastTokens > 0 || j.tokPerSec > 0)) {
          var parts = [];
          if (j.lastTokens > 0) parts.push(j.lastTokens + ' tok');
          if (j.tokPerSec  > 0) parts.push(j.tokPerSec.toFixed(1) + ' tok/s');
          if (j.ctxMax     > 0) parts.push(j.ctxUsed + '/' + j.ctxMax + ' ctx');
          metaEl2.textContent = parts.join(' · ');
        }
      })
      .catch(function(){
        lastState = '';
        // The device itself is unreachable, so any cached host verdict is now
        // unfounded — clear it rather than blame the CM5 for a dead web link.
        lastHostMode = ''; lastHostFresh = null;
        setDot('dot-off');
        stateEl.textContent = 'Offline';
        setReady(false);
      });
  }

  var modelSig = null;
  function fetchModels() {
    hw.fetchJSON('/api/llm/models')
      .then(function(arr){
        // Called on every heartbeat, so rebuilding unconditionally would stomp
        // the user's selection mid-scroll several times a minute. Diff first.
        var sig = arr.map(function(m){
          return m.id + '\u0001' + m.name + '\u0001' + m.size +
                 '\u0001' + (m.available === false ? 0 : 1);
        }).join('\u0002');
        if (sig === modelSig) return;
        var keep = modelSig === null ? '' : modelSel.value;   // survive a rebuild
        modelSig = sig;

        modelSel.innerHTML = '';
        if (!arr.length) {
          modelSel.innerHTML = '<option value="">No models found</option>';
          return;
        }
        for (var i = 0; i < arr.length; i++) {
          var o   = document.createElement('option');
          o.value = arr[i].id;
          // Sizes span an on-device .bin (~6MB) to a remote GGUF (~3.5GB), so a
          // flat KB figure prints "3590144KB". Scale to the unit that reads.
          var b = arr[i].size || 0, size = '';
          if (b > 0) {
            var k = b / 1024, m = k / 1024, g = m / 1024;
            size = g >= 1 ? ' (' + g.toFixed(1) + 'GB)'
                 : m >= 1 ? ' (' + m.toFixed(m < 10 ? 1 : 0) + 'MB)'
                          : ' (' + Math.round(k) + 'KB)';
          }
          var pre = arr[i].storage === 'sd' ? '[SD] '
                  : arr[i].storage === 'remote' ? '[' + arr[i].backend + '] ' : '';
          // A remote model can be listed but not selectable right now (host
          // offline). Show it disabled rather than dropping it, so the reason
          // is visible instead of the model just vanishing.
          o.textContent = pre + arr[i].name + size +
                          (arr[i].available === false ? ' - unavailable' : '');
          if (arr[i].available === false) o.disabled = true;
          modelSel.appendChild(o);
        }
        if (keep) modelSel.value = keep;                      // no-op if it went away
      })
      .catch(function(){
        // Don't let one dropped poll wipe a list we already have — only claim
        // an error when there is nothing to fall back to.
        if (modelSig === null) modelSel.innerHTML = '<option value="">Error</option>';
      });
  }

  // ── liveness heartbeat ───────────────────────────────────────────────────
  // This page used to be a snapshot: the catalog fetched once at load, the
  // status only after a user action. Both assumptions hold only for the onboard
  // engine, where the models are local files that are already there and a load
  // is synchronous with the POST that requested it. A remote source breaks both
  // — the CM5's catalog arrives over UART once the host link is up (so a model
  // can appear, or flip from unavailable to selectable, at any moment) and its
  // load is a llama-server restart taking tens of seconds, ending in a `ready`
  // the device learns about long after /api/llm/load returned. Neither showed
  // up without an F5.
  //
  // The cadence follows state, because each poll is an httpd request on a
  // device where httpd is one of the few things that can preempt the LLM task:
  //   generating → SUSPENDED. pollResult() is already running a 150ms loop;
  //                a second request stream would compete for the httpd worker
  //                during the most latency-sensitive window on the device.
  //   hidden tab → SUSPENDED, with an immediate catch-up on re-show.
  //   loading    → FAST, so a host-side switch lands on screen as it finishes.
  //   otherwise  → SLOW.
  var BEAT_FAST = 1000, BEAT_SLOW = 5000;
  var beatTimer = null;

  function scheduleBeat(ms) {
    if (beatTimer) clearTimeout(beatTimer);
    beatTimer = setTimeout(beat, ms !== undefined ? ms
                                 : (lastState === 'LOADING' ? BEAT_FAST : BEAT_SLOW));
  }
  function beat() {
    beatTimer = null;
    // Re-arm even when skipping: a suspended heartbeat must not be a stopped one.
    if (busy || document.hidden) { scheduleBeat(BEAT_SLOW); return; }
    fetchStatus(false, null);
    fetchModels();
    scheduleBeat();
  }
  document.addEventListener('visibilitychange', function(){
    if (!document.hidden) scheduleBeat(0);
  });

  // ── generate (shared by ask and retry) — async poll architecture ──
  function doGenerate(ctx) {
    busy = true;
    askBtn.style.display = 'none';
    hw.hide(doBtn);
    stopBtn.style.display = '';
    inputEl.disabled = true;
    hw.hide(ctx.retryBtn);

    ctx.aText.textContent = '';
    ctx.aText.classList.add('streaming');
    ctx.metaLine.textContent = '';
    scrollBottom();

    // Per-request overrides: only send a value the user actually typed in the
    // Advanced panel. A blank field is omitted, so the device's saved llm*
    // settings apply (llmtemperature / llmsentencelimit / llmreppenalty, plus
    // top_p / rep_window / min_p / hard_cap which the resolver pulls from
    // settings). This is why a blank panel now honours your saved settings.
    var body = { prompt: ctx.prompt };
    var tempStr = hw.$('qa-temp').value.trim();
    var sentStr = hw.$('qa-sentlimit').value.trim();
    var repStr  = hw.$('qa-repen').value.trim();
    if (tempStr !== '') body.temperature    = parseFloat(tempStr);
    if (sentStr !== '') body.sentence_limit = parseInt(sentStr);
    if (repStr  !== '') body.rep_penalty    = parseFloat(repStr);
    if (ctx.isDoMode) { body.hard_cap = 4; body.sentence_limit = 0; }
    if (ctx.prevAnswers.length > 0) { body.suppress = ctx.prevAnswers; }

    ctx.pollOffset  = 0;
    ctx.pollSession = 0;
    ctx.failCount   = 0;
    ctx.failMs      = 0;
    ctx.stopped     = false;
    // Fake AbortController so qaStop() can set stopped = true
    abortCtrl = { abort: function() { ctx.stopped = true; } };

    hw.postJSON('/api/llm/generate', body)
      .then(function(j) {
        if (!j.ok) {
          var err = document.createElement('div');
          err.className = 'qa-err';
          err.textContent = '[' + (j.error || 'error') + ']';
          ctx.pair.appendChild(err);
          finishGen(ctx);
          return;
        }
        ctx.pollSession = j.session;
        schedulePoll(ctx);
      })
      .catch(function(e) {
        if (!ctx.stopped) {
          var err = document.createElement('div');
          err.className = 'qa-err';
          err.textContent = '[request failed: ' + e.message + ']';
          ctx.pair.appendChild(err);
        }
        finishGen(ctx);
      });
  }

  // Poll cadence, and what happens when polls stop landing.
  //
  // A transport failure does NOT lose data: the retry re-polls the SAME byte
  // cursor, so the answer still assembles exactly (verified with 25 consecutive
  // dropped polls mid-answer). The problem with the old code was that it retried
  // at 150ms FOREVER with no feedback — a device that rebooted or dropped off
  // WiFi left a blinking cursor and half an answer, at ~6.7 requests/second, and
  // only Stop escaped.
  //
  // WHY THIS CANNOT FIRE ON A SLOW FIRST TOKEN: hw.fetchJSON sets no timeout and
  // no AbortController, so a device that is merely slow does not reject — the
  // poll SUCCEEDS and returns empty text, and any success resets the streak. The
  // only way to accumulate failures is transport-level: connection refused, the
  // socket dropped, or a non-2xx status. A cold model load can starve httpd into
  // that territory, which is exactly why the backoff matters: it stops hammering
  // a device that is already struggling, and the two thresholds are sized so a
  // starved-then-recovered device is a transient warning, never a lost turn.
  // ONE threshold, one outcome. An earlier version warned at 10s and kept the
  // turn alive until 60s, which created a 50-second stretch where the page
  // looked alive and was not, and then resumed a stream the user had already
  // given up on. Terminating is fewer moving parts and a clearer promise.
  //
  // The backoff stays, because a SINGLE dropped poll is normal and costs
  // nothing -- the retry re-reads the same byte cursor, and 25 consecutive
  // drops still reassemble the answer exactly. Ending a turn on the first
  // failure would kill generations during ordinary radio blips.
  var POLL_MS = 150, POLL_MAX_MS = 2000;
  var LOST_GIVEUP_MS = 20000;

  function schedulePoll(ctx, delayMs) {
    if (ctx.stopped) { finishGen(ctx); return; }
    setTimeout(function() { pollResult(ctx); }, delayMs === undefined ? POLL_MS : delayMs);
  }

  function pollSucceeded(ctx) {
    // Any success at all clears the streak. This is the whole anti-false-trigger
    // mechanism: only an UNBROKEN run of failures counts toward giving up.
    ctx.failCount = 0;
    ctx.failMs = 0;
  }

  function pollFailed(ctx) {
    ctx.failCount = (ctx.failCount || 0) + 1;
    // 150, 300, 600, 1200, then hold at 2000.
    var delay = Math.min(POLL_MS * Math.pow(2, Math.min(ctx.failCount - 1, 4)), POLL_MAX_MS);
    ctx.failMs = (ctx.failMs || 0) + delay;

    if (ctx.failMs >= LOST_GIVEUP_MS) {
      // Unbroken failure for the whole budget: the link is down, not stuttering.
      // End the turn and leave whatever text arrived on screen -- it is real
      // output. The turn is NOT resumed if the device comes back; a new question
      // is the way forward, which is what "ended" should mean.
      var gone = document.createElement('div');
      gone.className = 'qa-err';
      gone.textContent = '[connection lost]';
      ctx.pair.appendChild(gone);
      finishGen(ctx);
      return;
    }
    schedulePoll(ctx, delay);
  }

  function pollResult(ctx) {
    if (ctx.stopped) { finishGen(ctx); return; }
    hw.fetchJSON('/api/llm/result?session=' + ctx.pollSession + '&offset=' + ctx.pollOffset)
      .then(function(j) {
        pollSucceeded(ctx);
        if (j.stale) { finishGen(ctx); return; }
        if (j.text && j.text.length > 0) {
          ctx.aText.textContent += j.text;
          scrollBottom();
        }
        // OUTSIDE the text guard on purpose: a zero-byte poll must still adopt
        // the device's cursor.
        //
        // The device reports the absolute byte cursor it served up to. Prefer it
        // over any client-side measurement: it is the only party that knows how
        // many BYTES it copied, and it deliberately passes malformed bytes
        // through untouched (an invalid lead, a stray byte from a byte-fallback
        // token), for which decoded length and served length differ — the
        // browser substitutes U+FFFD, which re-encodes to 3 bytes.
        //
        // The fallback is unreachable against matched firmware, since this JS is
        // streamed from the same image that serves the endpoint. Keep it anyway:
        // without it a missing field makes pollOffset NaN, the query becomes
        // offset=NaN, atoi() reads 0, and the device re-serves from byte 0 on
        // every poll forever.
        if (typeof j.next === 'number') {
          ctx.pollOffset = j.next;
        } else {
          try { console.warn('/api/llm/result omitted next'); } catch (e) {}
          ctx.pollOffset += unescape(encodeURIComponent(j.text || '')).length;
        }
        if (j.done) {
          // The device only sends this when the turn ended badly. Render it
          // against THIS pair rather than as a global status line: it explains
          // this answer, and an empty answer with no explanation is the worst
          // outcome the streaming path can produce.
          if (j.error) {
            var why = document.createElement('div');
            why.className = 'qa-err';
            why.textContent = '[' + j.error + ']';
            ctx.pair.appendChild(why);
          }
          finishGen(ctx);
        } else {
          schedulePoll(ctx);
        }
      })
      .catch(function() {
        // Retry the SAME cursor — no data is lost by a failed poll. Backs off
        // and eventually gives up; see schedulePoll.
        if (!ctx.stopped) pollFailed(ctx);
      });
  }

  function finishGen(ctx) {
    busy = false;
    abortCtrl = null;
    ctx.aText.classList.remove('streaming');
    askBtn.style.display = '';
    stopBtn.style.display = 'none';
    syncDoAffordance(!inputEl.disabled);
    inputEl.disabled = false;
    askBtn.disabled  = false;
    inputEl.focus();
    fetchStatus(true, ctx.metaLine);

    // Do: mode — render suggestion UI instead of retry
    if (ctx.isDoMode && ctx.aText.textContent.trim()) {
      var raw = ctx.aText.textContent.trim();
      // Extract just the command — strip any trailing explanation text.
      // Commands are 1-2 words (e.g. "wifistatus", "ledcolor green").
      // Stop at punctuation or common explanation words.
      var cmd = raw.replace(/[.,;!?].*/,'')             // strip from first punctuation
                   .replace(/\s+(to|for|and|the|is|it|that|which|will|can|shows|displays|checks|reads)\b.*/i,'')  // strip explanation
                   .trim();
      if (!cmd) cmd = raw.split(/\s/)[0];  // fallback: first word
      ctx.aText.textContent = '';

      var suggestion = document.createElement('div');
      suggestion.className = 'qa-do-suggestion';

      var cmdInput = document.createElement('input');
      cmdInput.className = 'qa-do-cmd';
      cmdInput.type = 'text';
      cmdInput.value = cmd;

      var runBtn = document.createElement('button');
      runBtn.className = 'qa-do-run';
      runBtn.textContent = 'Run \u25B6';

      var resultDiv = document.createElement('div');
      resultDiv.className = 'qa-do-result';
      resultDiv.style.display = 'none';

      runBtn.onclick = function() { doRunCmd(cmdInput, resultDiv); };
      cmdInput.addEventListener('keydown', function(e) {
        if (e.key === 'Enter') { e.preventDefault(); doRunCmd(cmdInput, resultDiv); }
      });

      suggestion.appendChild(cmdInput);
      suggestion.appendChild(runBtn);
      ctx.aText.appendChild(suggestion);
      ctx.pair.appendChild(resultDiv);
      return;
    }

    // Show retry button if under 3 retries
    if (ctx.prevAnswers.length < 3 && ctx.aText.textContent.trim()) {
      if (!ctx.retryBtn) {
        ctx.retryBtn = document.createElement('button');
        ctx.retryBtn.className = 'btn qa-retry';
        ctx.retryBtn.innerHTML = '&#8635; Retry';
        ctx.retryBtn.onclick = function() {
          var curAnswer = ctx.aText.textContent.trim();
          // Guided asks re-ask the SAME composed question plain: pushing the prior
          // answer into prevAnswers would send it as suppress[], banning the
          // memorized-correct answer (LLM_GUIDED_MENU_SPEC §7 guided-retry fix).
          if (curAnswer && !ctx.isGuided) ctx.prevAnswers.push(curAnswer);

          // Dim the old answer row + meta instead of clearing
          ctx.aText.classList.add('qa-a-old');
          ctx.metaLine.classList.add('qa-a-old');
          ctx.retryBtn.style.display = 'none';

          // Create a new answer row below
          var newRow = document.createElement('div');
          newRow.className = 'qa-a-row';
          newRow.innerHTML = '<span class="qa-label qa-label-a">A:</span>';
          var newAText = document.createElement('span');
          newAText.className = 'qa-a-text';
          newRow.appendChild(newAText);
          ctx.pair.appendChild(newRow);

          var newMeta = document.createElement('div');
          newMeta.className = 'qa-meta';
          ctx.pair.appendChild(newMeta);

          // Update ctx to point to new elements
          ctx.aText = newAText;
          ctx.metaLine = newMeta;
          ctx.retryBtn = null;

          doGenerate(ctx);
        };
        ctx.metaLine.parentNode.insertBefore(ctx.retryBtn, ctx.metaLine.nextSibling);
      }
      ctx.retryBtn.style.display = '';
      ctx.retryBtn.disabled = false;
      if (ctx.prevAnswers.length >= 2) {
        ctx.retryBtn.innerHTML = '&#8635; Retry (' + (3 - ctx.prevAnswers.length) + ' left)';
      }
    } else if (ctx.retryBtn) {
      ctx.retryBtn.style.display = 'none';
    }
  }

  // ── run Do: command ──
  function doRunCmd(cmdInput, resultDiv) {
    var cmd = cmdInput.value.trim();
    if (!cmd) return;
    // Find the Run button (sibling of the input)
    var runBtn = cmdInput.parentElement.querySelector('.qa-do-run');
    // Disable input and button immediately
    cmdInput.disabled = true;
    cmdInput.style.opacity = '0.6';
    if (runBtn) {
      runBtn.disabled = true;
      runBtn.textContent = 'Running...';
      runBtn.style.opacity = '0.6';
      runBtn.style.cursor = 'default';
    }
    resultDiv.style.display = '';
    resultDiv.textContent = 'Running...';
    hw.postFormText('/api/cli', { cmd: cmd, capture: '1' })
      .then(function(t) {
        resultDiv.textContent = t || '(no output)';
        hw.setText(runBtn, 'Ran \u2713');
      })
      .catch(function(e) {
        resultDiv.textContent = 'Error: ' + e.message;
        // Re-enable on error so user can retry
        cmdInput.disabled = false;
        cmdInput.style.opacity = '';
        if (runBtn) {
          runBtn.disabled = false;
          runBtn.textContent = 'Run \u25B6';
          runBtn.style.opacity = '';
          runBtn.style.cursor = 'pointer';
        }
      });
  }

  // ── ask ──
  window.qaAsk = function(mode) {
    var q = inputEl.value.trim();
    if (!q || busy) return;
    inputEl.value = '';

    // Detect Do: prefix (case-insensitive)
    // Three ways in, one meaning. The typed "do:" prefix stays exactly as it
    // was -- it is the keyboard path, the guided strip composes through it, and
    // the device parses the same marker independently -- and the Do: button
    // selects the same mode explicitly.
    var typedDo  = /^do:\s*/i.test(q);
    var isDoMode = (mode === 'do') || typedDo;
    var intent   = typedDo ? q.replace(/^do:\s*/i, '') : q;

    // Do:-mode is on-device only. The device refuses it for a remote model too,
    // but stopping here means the user gets the reason instead of a round trip
    // that comes back looking like a failure. Say what to do about it, and put
    // the text back so a long intent is not lost to a mode they cannot use.
    if (isDoMode && cmdModeOk === false) {
      addSys('Do: mode needs the on-device model — ' +
             (metaEl.textContent || 'the current model') +
             ' does not know this device\'s commands. Load a local model, or ask ' +
             'it as a normal question.');
      inputEl.value = q;
      scrollBottom();
      return;
    }

    // Hide retry button from previous Q&A pair
    if (currentCtx && currentCtx.retryBtn) {
      currentCtx.retryBtn.style.display = 'none';
    }

    // Build Q&A pair in the list
    var pair = document.createElement('div');
    pair.className = 'qa-pair';

    var qRow = document.createElement('div');
    qRow.className = 'qa-q-row';
    qRow.innerHTML = '<span class="qa-label qa-label-q">' + (isDoMode ? 'Do:' : 'Q:') + '</span>';
    var qText = document.createElement('span');
    qText.className = 'qa-q-text';
    qText.textContent = intent;
    qRow.appendChild(qText);
    pair.appendChild(qRow);

    var aRow = document.createElement('div');
    aRow.className = 'qa-a-row';
    aRow.innerHTML = '<span class="qa-label qa-label-a">A:</span>';
    var aText = document.createElement('span');
    aText.className = 'qa-a-text';
    aRow.appendChild(aText);
    pair.appendChild(aRow);

    var metaLine = document.createElement('div');
    metaLine.className = 'qa-meta';
    pair.appendChild(metaLine);

    list.appendChild(pair);

    // Create new context and track it as current.
    //
    // Send the prompt UNFRAMED. The device frames per backend at one chokepoint
    // (llmBackendFramePrompt): the onboard engine gets its "Q: ...\nA:"
    // scaffolding, and a remote source deliberately gets none, because it
    // applies its own chat template and the local scaffolding would otherwise
    // show up verbatim in the answer.
    //
    // This page used to frame client-side. That was invisible on the onboard
    // path only because llmFramePrompt passes through anything already starting
    // with "Q:" — but nothing stripped it on the way to the CM5, so the remote
    // model received the local scaffolding despite the backend going out of its
    // way not to add any. "Do: " is kept because it is the user's INTENT
    // marker, not scaffolding: llmFramePrompt consumes that prefix and turns it
    // into the same "Q: <intent>\nDo:" this used to build by hand.
    currentCtx = {
      prompt: isDoMode ? 'Do: ' + intent : q,
      isDoMode: isDoMode,
      pair: pair,
      aText: aText,
      metaLine: metaLine,
      prevAnswers: [],
      retryBtn: null
    };
    doGenerate(currentCtx);
  };

  window.qaStop = function() {
    hw.postJSON('/api/llm/stop', {}).catch(function(){});
    if (abortCtrl) abortCtrl.abort();
  };

  window.qaLoadModel = function() {
    var path = modelSel.value;
    if (!path) return;
    var name = path.split('/').pop() || path;
    setDot('dot-busy');
    stateEl.textContent = 'Loading...';
    setReady(false);
    addSys('Loading ' + name + '...');
    hw.postJSON('/api/llm/load', {model: path})   // no max_ctx: server auto-fits to PSRAM (or the saved llmmaxcontext cap)
      .then(function(j){
        // A rejected selection comes back as {"ok":false,"error":...}
        // (WebPage_LLM.cpp handleLLMLoad). Discarding it made the page go quiet
        // with no reason given: a select that fails WITHOUT changing device
        // state leaves the status signature identical, so the edge-triggered
        // announcer below correctly stays silent too, and the only feedback the
        // user got was the pill dropping back to "No model".
        // "not available right now" is true but reads as broken when the host is
        // merely still starting — the case that looks like a bug on a cold boot,
        // because the daemon withholds readiness for the ~40s llama-server takes
        // to come up. hostMode already distinguishes the two, so say which it is
        // instead of leaving the user to guess.
        if (j && j.ok === false) {
          var why = j.error || 'unknown error';
          if (lastHostMode === 'starting') {
            why += ' — the CM5 is still starting up; try again in a moment';
          } else if (lastHostFresh === false) {
            why += ' — the CM5 is not reachable right now';
          }
          addSys('Load failed: ' + why);
        }
        fetchStatus(false, null);
        scheduleBeat(BEAT_FAST);   // a remote load resolves asynchronously; watch for it
      })
      .catch(function(){ addSys('Load request failed'); fetchStatus(false, null); });
  };

  window.qaUnloadModel = function() {
    hw.postJSON('/api/llm/unload', {})
      // Acknowledge the unload explicitly. Pass afterGen=true so fetchStatus
      // refreshes the dot/state to UNLOADED but SKIPS the generic "No model
      // loaded…" line — we show "Model unloaded" instead (matches the CLI's
      // `llmunload` result string). On failure, fall back to a normal status
      // refresh so a real error still surfaces.
      .then(function(){ addSys('Model unloaded'); fetchStatus(true, null); })
      .catch(function(){ fetchStatus(false, null); });
  };

  inputEl.addEventListener('keydown', function(e){
    if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); qaAsk(); }
  });

  // ── Guided-ask strip (menu-driven input) ────────────────────────────────
  // A model may carry a MENU: curated corpus-exact templates ("What type is {}?")
  // + entity rosters. Three chained selects compose a corpus-exact question
  // straight into the free-text box; the untouched qaAsk() then submits it
  // through the normal generate pipeline. See LLM_GUIDED_MENU_SPEC §7.
  var guidedStrip  = hw.$('qa-guided');
  var groupSel     = hw.$('qa-group');
  var tplSel       = hw.$('qa-tpl');
  var entSel       = hw.$('qa-ent');
  var guidedGen    = -1;              // cached menu generation (from /api/llm/status)
  var guidedGroups = [];             // [{i,name,mode,templates,entities}]
  var curGroup     = null;           // selected group object
  var curTemplates = [];             // display strings for the selected group
  var curEntities  = [];             // entity strings currently in #qa-ent
  var entCacheG    = -1;             // group whose entities are cached in curEntities
  var guidedComposedActive = false;  // input currently holds a strip-composed question

  function findGroup(i) {
    for (var k = 0; k < guidedGroups.length; k++) if (guidedGroups[k].i === i) return guidedGroups[k];
    return null;
  }

  // Page through /api/llm/menu (tpl/ent are windowed; groups come in one shot)
  // and hand the fully-accumulated list to done(gen, items).
  function menuFetchAll(kind, g, done) {
    var acc = [];
    (function step(off) {
      var url = '/api/llm/menu?kind=' + kind + (kind === 'groups' ? '' : '&g=' + g + '&off=' + off);
      hw.fetchJSON(url)
        .then(function(j) {
          if (kind === 'groups') { done(j.gen, j.groups || []); return; }
          var items = j.items || [];
          for (var i = 0; i < items.length; i++) acc.push(items[i]);
          var next = j.off + items.length;
          if (items.length > 0 && next < j.total) step(next);
          else done(j.gen, acc);
        })
        .catch(function() { done(-1, acc); });
    })(0);
  }

  function loadGuided() {
    menuFetchAll('groups', 0, function(gen, groups) {
      guidedGen = gen;
      guidedGroups = groups;
      entCacheG = -1;
      if (!groups.length) { guidedStrip.style.display = 'none'; return; }
      groupSel.innerHTML = '';
      for (var i = 0; i < groups.length; i++) {
        var o = document.createElement('option');
        o.value = groups[i].i;
        o.textContent = groups[i].name;
        groupSel.appendChild(o);
      }
      guidedStrip.style.display = '';
      loadTemplates(groups[0].i);
    });
  }

  function loadTemplates(g) {
    curGroup = findGroup(g);
    menuFetchAll('tpl', g, function(gen, tpls) {
      curTemplates = tpls;
      tplSel.innerHTML = '';
      for (var i = 0; i < tpls.length; i++) {
        var o = document.createElement('option');
        o.value = i;
        o.textContent = tpls[i];   // display form, slot shown as "{}"
        tplSel.appendChild(o);
      }
      onTplChange();
    });
  }

  function loadEntities(g, cb) {
    if (entCacheG === g) { cb(curEntities); return; }
    menuFetchAll('ent', g, function(gen, ents) {
      entCacheG = g;
      curEntities = ents;
      cb(ents);
    });
  }

  function onTplChange() {
    var g   = parseInt(groupSel.value, 10);
    var t   = parseInt(tplSel.value, 10);
    var tpl = curTemplates[t] || '';
    if (tpl.indexOf('{}') >= 0) {          // slot present → offer the entity picker
      loadEntities(g, function(ents) {
        entSel.innerHTML = '';
        for (var i = 0; i < ents.length; i++) {
          var o = document.createElement('option');
          o.value = i;
          o.textContent = ents[i];
          entSel.appendChild(o);
        }
        entSel.style.display = ents.length ? '' : 'none';
        compose();
      });
    } else {                                // slotless (canned) → entity pick skipped
      entSel.style.display = 'none';
      compose();
    }
  }

  function compose() {
    var t   = parseInt(tplSel.value, 10);
    var tpl = curTemplates[t];
    if (tpl == null) return;
    var q;
    if (tpl.indexOf('{}') >= 0) {
      var e   = parseInt(entSel.value, 10);
      var ent = curEntities[e] || '';
      q = tpl.split('{}').join(ent);       // split/join avoids String.replace $-patterns
    } else {
      q = tpl;
    }
    if (curGroup && curGroup.mode === 'do') q = 'do: ' + q;
    inputEl.value = q;                     // programmatic set: fires no 'input' event...
    guidedComposedActive = true;           // ...so this flag survives until the user types
  }

  // Refetch/hide the strip when the menu generation changes (model load/unload).
  function menuSync(menu) {
    var groups = (menu && menu.groups) || 0;
    var gen    = (menu && menu.gen) || 0;
    if (groups <= 0) { guidedStrip.style.display = 'none'; guidedGen = -1; return; }
    if (gen !== guidedGen) { guidedGen = gen; loadGuided(); }
  }

  groupSel.addEventListener('change', function() { loadTemplates(parseInt(groupSel.value, 10)); });
  tplSel.addEventListener('change', onTplChange);
  entSel.addEventListener('change', compose);
  // A manual keystroke clears the guided flag so a hand-typed question retries
  // normally (with suppress[]); a strip-composed one retries plain (see above).
  inputEl.addEventListener('input', function() { guidedComposedActive = false; });

  // Tag the Q&A pair qaAsk() just created as "guided" WITHOUT touching qaAsk
  // itself — the retry branch reads ctx.isGuided to skip suppress[].
  var _qaAskOrig = window.qaAsk;
  // MUST forward the mode. This wrapper used to call _qaAskOrig() with no
  // arguments, which was invisible while qaAsk took none -- and silently turned
  // the Do: button into an ordinary Ask the moment one was added, because the
  // button's mode never reached the real handler.
  window.qaAsk = function(mode) {
    var wasGuided = guidedComposedActive;
    var prevCtx   = currentCtx;
    _qaAskOrig(mode);
    if (currentCtx && currentCtx !== prevCtx) {   // an ask actually started (not an empty/busy no-op)
      guidedComposedActive = false;
      if (wasGuided) currentCtx.isGuided = true;
    }
  };

  fetchStatus(false, null);
  fetchModels();
  scheduleBeat();
})();
</script>
)JS", HTTPD_RESP_USE_STRLEN);
}

#endif // ENABLE_LLM_BACKEND && ENABLE_HTTP_SERVER
#endif // WEBPAGE_LLM_H
