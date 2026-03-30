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

#if ENABLE_ONDEVICE_LLM && ENABLE_HTTP_SERVER
void registerLLMHandlers(httpd_handle_t server);
#else
inline void registerLLMHandlers(httpd_handle_t) {}
#endif

#if ENABLE_ONDEVICE_LLM && ENABLE_HTTP_SERVER

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
.qa-bar-meta{color:var(--muted);flex:1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.qa-bar select{
  background:var(--panel-bg);color:var(--panel-fg);border:1px solid var(--border);
  border-radius:5px;padding:3px 6px;font-size:.82em;max-width:160px;
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
  display:flex;align-items:flex-end;gap:8px;
}
.qa-q-prefix {
  font-family:'Courier New',monospace;font-size:1em;font-weight:700;
  color:var(--panel-fg);padding-bottom:11px;flex-shrink:0;user-select:none;
}
.qa-input-row textarea {
  flex:1;background:var(--panel-bg);color:var(--panel-fg);
  border:1px solid var(--border);border-radius:8px;
  padding:10px 12px;font-family:inherit;font-size:.95em;
  resize:none;min-height:44px;max-height:120px;line-height:1.4;
}
.qa-input-row textarea:focus{outline:none;border-color:var(--link)}
.qa-input-row textarea:disabled{opacity:.5}

/* ── Retry button ── */
.qa-retry{margin-left:8px;font-size:.85em;padding:4px 10px}
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
</style>
)CSS", HTTPD_RESP_USE_STRLEN);

  // ── HTML ─────────────────────────────────────────────────────────────────
  httpd_resp_send_chunk(req, R"HTML(
<div class='qa-wrap'>

  <div class='qa-bar'>
    <span class='qa-bar-dot dot-off' id='qa-dot'></span>
    <span class='qa-bar-state' id='qa-state'>Connecting...</span>
    <span class='qa-bar-meta' id='qa-meta'></span>
    <select id='qa-model'><option value=''>Loading...</option></select>
    <button class='btn' onclick='qaLoadModel()'>Load</button>
    <button class='btn' onclick='qaUnloadModel()'>Unload</button>
    <button class='btn' id='qa-adv-btn' onclick='qaToggleAdv()'>Advanced</button>
  </div>

  <div class='qa-list' id='qa-list'>
    <div class='qa-sys' id='qa-init-msg'>Connecting to LLM engine...</div>
  </div>

  <div class='qa-input-wrap'>
    <div class='qa-input-row'>
      <span class='qa-q-prefix'>Q:</span>
      <textarea id='qa-input' rows='2'
        placeholder='Ask about Hardware One...' disabled></textarea>
      <button id='qa-ask' class='btn' disabled onclick='qaAsk()'>Ask</button>
      <button id='qa-stop' class='btn' style='display:none;background:var(--danger);color:#fff' onclick='qaStop()'>Stop</button>
    </div>
    <div class='qa-adv-body' id='qa-adv-body'>
      <label title='Temperature — lower is more focused, higher is more varied'>Temp:<input type='number' id='qa-temp' value='0.5' min='0' max='2' step='0.05'></label>
      <label title='Stop after N sentences (0=off)'>Sentences:<input type='number' id='qa-sentlimit' value='2' min='0' max='10'></label>
      <label title='Repetition penalty'>Rep:<input type='number' id='qa-repen' value='1.3' min='1' max='5' step='0.05'></label>
      <div class='qa-adv-hint'>Temp 0.5 works best for this model. Lower = more focused, higher = more varied.</div>
    </div>
  </div>

</div>
)HTML", HTTPD_RESP_USE_STRLEN);

  // ── JavaScript ───────────────────────────────────────────────────────────
  httpd_resp_send_chunk(req, R"JS(
<script>
(function(){
  var list      = document.getElementById('qa-list');
  var inputEl   = document.getElementById('qa-input');
  var askBtn    = document.getElementById('qa-ask');
  var stopBtn   = document.getElementById('qa-stop');
  var dot       = document.getElementById('qa-dot');
  var stateEl   = document.getElementById('qa-state');
  var metaEl    = document.getElementById('qa-meta');
  var modelSel  = document.getElementById('qa-model');
  var initMsg   = document.getElementById('qa-init-msg');
  var busy      = false;
  var abortCtrl = null;
  var currentCtx = null; // Track the most recent Q&A context
  var lastShownError = null; // Avoid repeating the same error message

  // ── helpers ──
  function setDot(cls) {
    dot.className = 'qa-bar-dot ' + cls;
  }
  function setReady(isReady) {
    inputEl.disabled = !isReady || busy;
    askBtn.disabled  = !isReady || busy;
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
    var b = document.getElementById('qa-adv-body');
    var t = document.getElementById('qa-adv-btn');
    var open = b.classList.toggle('open');
    t.textContent = open ? 'Hide Advanced' : 'Advanced';
  };

  // ── status poll ──
  function fetchStatus(afterGen, metaEl2) {
    fetch('/api/llm/status', {credentials:'same-origin'})
      .then(function(r){ return r.json(); })
      .then(function(j){
        var tps     = j.tokPerSec > 0 ? j.tokPerSec.toFixed(1) + ' tok/s' : '';
        var arch    = j.arch ? j.arch + ' ' + (j.quant||'') : '';
        var meta    = [arch, j.params||'', j.psramKB ? j.psramKB+'KB' : '', tps].filter(Boolean).join(' · ');
        metaEl.textContent = meta;

        if (j.state === 'READY') {
          setDot('dot-ready');
          stateEl.textContent = 'Ready';
          setReady(true);
          if (!afterGen) {
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
            if (j.psramKB) details.push(j.psramKB + 'KB PSRAM');
            var msg = 'Model loaded: ' + modelName;
            if (details.length > 0) msg += ' (' + details.join(' · ') + ')';
            addSys(msg);
          }
        } else if (j.state === 'GENERATING') {
          setDot('dot-busy');
          stateEl.textContent = 'Generating';
        } else if (j.state === 'UNLOADED') {
          setDot('dot-off');
          stateEl.textContent = 'No model';
          setReady(false);
          if (!afterGen) {
            if (initMsg) initMsg.textContent = 'No model loaded. Select a model and click Load.';
            else addSys('No model loaded. Select a model and click Load.');
          }
        } else if (j.state === 'ERROR') {
          setDot('dot-off');
          stateEl.textContent = 'Error';
          setReady(false);
          if (!afterGen && j.error && j.error !== lastShownError) {
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
        setDot('dot-off');
        stateEl.textContent = 'Offline';
        setReady(false);
      });
  }

  function fetchModels() {
    fetch('/api/llm/models', {credentials:'same-origin'})
      .then(function(r){ return r.json(); })
      .then(function(arr){
        modelSel.innerHTML = '';
        if (!arr.length) {
          modelSel.innerHTML = '<option value="">No models found</option>';
          return;
        }
        for (var i = 0; i < arr.length; i++) {
          var o   = document.createElement('option');
          o.value = arr[i].path || arr[i].name;
          var kb  = Math.round(arr[i].size / 1024);
          var pre = arr[i].storage === 'sd' ? '[SD] ' : '';
          o.textContent = pre + arr[i].name + ' (' + kb + 'KB)';
          modelSel.appendChild(o);
        }
      })
      .catch(function(){
        modelSel.innerHTML = '<option value="">Error</option>';
      });
  }

  // ── generate (shared by ask and retry) ──
  function doGenerate(ctx) {
    busy = true;
    askBtn.style.display = 'none';
    stopBtn.style.display = '';
    inputEl.disabled = true;
    if (ctx.retryBtn) ctx.retryBtn.style.display = 'none';

    ctx.aText.textContent = '';
    ctx.aText.classList.add('streaming');
    ctx.metaLine.textContent = '';
    scrollBottom();

    var temp    = parseFloat(document.getElementById('qa-temp').value)   || 0.5;
    var sent    = parseInt(document.getElementById('qa-sentlimit').value);
    var repen   = parseFloat(document.getElementById('qa-repen').value)  || 1.3;

    var body = {
      prompt:         ctx.prompt,
      temperature:    temp,
      top_p:          0.8,
      sentence_limit: isNaN(sent) ? 2 : sent,
      rep_penalty:    repen,
      rep_window:     32
    };
    if (ctx.prevAnswers.length > 0) { body.suppress = ctx.prevAnswers; }

    abortCtrl = new AbortController();
    fetch('/api/llm/generate', {
      method:'POST',
      headers:{'Content-Type':'application/json'},
      credentials:'same-origin',
      body: JSON.stringify(body),
      signal: abortCtrl.signal
    }).then(function(r){
      var reader  = r.body.getReader();
      var decoder = new TextDecoder();
      function read() {
        reader.read().then(function(res){
          if (res.done) { finishGen(ctx); return; }
          ctx.aText.textContent += decoder.decode(res.value, {stream:true});
          scrollBottom();
          read();
        }).catch(function(e){
          if (e.name !== 'AbortError') {
            var err = document.createElement('div');
            err.className = 'qa-err';
            err.textContent = '[stream error]';
            ctx.pair.appendChild(err);
          }
          finishGen(ctx);
        });
      }
      read();
    }).catch(function(e){
      if (e.name !== 'AbortError') {
        var err = document.createElement('div');
        err.className = 'qa-err';
        err.textContent = '[request failed: ' + e.message + ']';
        ctx.pair.appendChild(err);
      }
      finishGen(ctx);
    });
  }

  function finishGen(ctx) {
    busy = false;
    abortCtrl = null;
    ctx.aText.classList.remove('streaming');
    askBtn.style.display = '';
    stopBtn.style.display = 'none';
    inputEl.disabled = false;
    askBtn.disabled  = false;
    inputEl.focus();
    fetchStatus(true, ctx.metaLine);

    // Show retry button if under 3 retries
    if (ctx.prevAnswers.length < 3 && ctx.aText.textContent.trim()) {
      if (!ctx.retryBtn) {
        ctx.retryBtn = document.createElement('button');
        ctx.retryBtn.className = 'btn qa-retry';
        ctx.retryBtn.innerHTML = '&#8635; Retry';
        ctx.retryBtn.onclick = function() {
          var curAnswer = ctx.aText.textContent.trim();
          if (curAnswer) ctx.prevAnswers.push(curAnswer);

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

  // ── ask ──
  window.qaAsk = function() {
    var q = inputEl.value.trim();
    if (!q || busy) return;
    inputEl.value = '';

    // Hide retry button from previous Q&A pair
    if (currentCtx && currentCtx.retryBtn) {
      currentCtx.retryBtn.style.display = 'none';
    }

    // Build Q&A pair in the list
    var pair = document.createElement('div');
    pair.className = 'qa-pair';

    var qRow = document.createElement('div');
    qRow.className = 'qa-q-row';
    qRow.innerHTML = '<span class="qa-label qa-label-q">Q:</span>';
    var qText = document.createElement('span');
    qText.className = 'qa-q-text';
    qText.textContent = q;
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

    // Create new context and track it as current
    currentCtx = {
      prompt: 'Q: ' + q + '\nA:',
      pair: pair,
      aText: aText,
      metaLine: metaLine,
      prevAnswers: [],
      retryBtn: null
    };
    doGenerate(currentCtx);
  };

  window.qaStop = function() {
    fetch('/api/llm/stop', {method:'POST', credentials:'same-origin'});
    if (abortCtrl) abortCtrl.abort();
  };

  window.qaLoadModel = function() {
    var path = modelSel.value;
    if (!path) return;
    var ctx = parseInt(document.getElementById('qa-ctx').value) || 64;
    var name = path.split('/').pop() || path;
    setDot('dot-busy');
    stateEl.textContent = 'Loading...';
    setReady(false);
    addSys('Loading ' + name + ' (ctx=' + ctx + ')...');
    fetch('/api/llm/load', {
      method:'POST', headers:{'Content-Type':'application/json'},
      credentials:'same-origin',
      body: JSON.stringify({model: path, max_ctx: 64})
    }).then(function(r){ return r.json(); })
      .then(function(j){
        if (j.ok) { fetchStatus(false, null); }
        else { fetchStatus(false, null); }
      })
      .catch(function(){ addSys('Load request failed'); });
  };

  window.qaUnloadModel = function() {
    fetch('/api/llm/unload', {method:'POST', credentials:'same-origin'})
      .then(function(){ fetchStatus(false, null); });
  };

  inputEl.addEventListener('keydown', function(e){
    if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); qaAsk(); }
  });

  fetchStatus(false, null);
  fetchModels();
})();
</script>
)JS", HTTPD_RESP_USE_STRLEN);
}

#endif // ENABLE_ONDEVICE_LLM && ENABLE_HTTP_SERVER
#endif // WEBPAGE_LLM_H
