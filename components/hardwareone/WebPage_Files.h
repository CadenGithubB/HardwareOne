#ifndef WEBPAGE_FILES_H
#define WEBPAGE_FILES_H

#include "WebServer_Utils.h"
#include "WebPage_AviPlayer.h"

// Streamed inner content for files page
inline void streamFilesInner(httpd_req_t* req) {
  // Stream shared file browser scripts
  String fbScript = getFileBrowserScript();
  httpd_resp_send_chunk(req, fbScript.c_str(), fbScript.length());
  // HTML structure
  httpd_resp_send_chunk(req, R"HTML(
<h2>File Manager</h2>
<p>Browse and manage files on the device filesystem</p>
<div style='display:flex;justify-content:space-between;align-items:center;margin:1rem 0 0.5rem 0'>
  <strong>Storage</strong>
  <span id='storage-text' style='font-size:0.9rem'>Loading...</span>
</div>
<div style='width:100%;height:20px;background:rgba(255,255,255,.15);border-radius:10px;overflow:hidden;border:1px solid var(--border);margin-bottom:1rem'>
  <div id='storage-bar' style='height:100%;background:linear-gradient(90deg,#28a745,#20c997);width:0%;transition:width 0.3s'></div>
</div>
<div id='fs-source-toggle' style='display:none;margin:0.75rem 0 0.25rem 0;align-items:center;gap:8px'>
  <span style='font-size:0.85rem;color:var(--muted)'>Filesystem:</span>
  <button id='fs-btn-local' class='btn' onclick='showLocalFs()'>This Device</button>
  <button id='fs-btn-bonded' class='btn' onclick='showBondedFs()'>Bonded Device</button>
</div>
<div id='file-manager-container' style='margin:1rem 0'></div>
<div id='bonded-fs-container' style='display:none;margin:1rem 0'></div>
)HTML", HTTPD_RESP_USE_STRLEN);
  // AVI player modal — injected so ViewFile can call openAviPlayer for .avi
  // files instead of letting the browser try to render the raw stream.
  streamAviPlayerModal(req);
  httpd_resp_send_chunk(req, R"HTML(
<div id='editor-modal' style='display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.5);z-index:1001'>
  <div style='position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);background:var(--panel-bg);color:var(--panel-fg);padding:1rem;border-radius:8px;min-width:720px;max-width:90vw;max-height:90vh;display:flex;flex-direction:column;border:1px solid var(--border)'>
    <div style='display:flex;align-items:center;justify-content:space-between;margin-bottom:0.5rem'>
      <h3 id='editor-title' style='margin:0;color:var(--panel-fg)'>Edit File</h3>
      <button class='btn' onclick="closeEditor()">Close</button>
    </div>
    <div style='font-size:0.9rem;color:var(--panel-fg);margin-bottom:0.25rem'><span id='editor-path'></span></div>
    <textarea id='editor-text' style='flex:1;min-height:300px;width:70vw;max-width:85vw;box-sizing:border-box;padding:0.75rem;border:1px solid var(--border);border-radius:6px;font-family:Menlo,Consolas,monospace;font-size:13px;line-height:1.4;color:var(--panel-fg);background:var(--panel-bg);'></textarea>
    <div style='margin-top:0.6rem;display:flex;gap:0.5rem;align-items:center;flex-wrap:wrap'>
      <button id='editor-save' class='btn' onclick="saveEditor()">Save</button>
      <button id='btn-pretty' class='btn' onclick="prettyJSON()" style='display:none'>Pretty JSON</button>
      <button id='btn-raw' class='btn' onclick="rawJSON()" style='display:none'>Raw JSON</button>
      <span id='editor-status' style='color:var(--panel-fg)'></span>
    </div>
  </div>
</div>
)HTML", HTTPD_RESP_USE_STRLEN);

  // Shared AVI player JS (defines window.openAviPlayer, wrapped in its own
  // IIFE). Emit before other scripts so ViewFile can rely on it being ready.
  httpd_resp_send_chunk(req, "<script>", HTTPD_RESP_USE_STRLEN);
  streamAviPlayerJs(req);
  httpd_resp_send_chunk(req, "</script>", HTTPD_RESP_USE_STRLEN);

  // JavaScript chunk 1
  httpd_resp_send_chunk(req, R"JS(
<script>console.log('[FILES] Section 1: Pre-script sentinel');</script>
<script>
console.log('[FILES] Part 1: Init starting...');
let fileManager = null;
let currentEditPath = '';
let isJsonEdit = false;

window.onload = function() {
  console.log('[FILES] Window onload');
  updateStorageStats('/');
  initFileManager();
  checkBondedFsAvailable();
};
console.log('[FILES] onload registered');

function initFileManager() {
  if (typeof window.createFileManager !== 'function') {
    console.error('[FILES] createFileManager not available');
    return;
  }
  fileManager = window.createFileManager({
    containerId: 'file-manager-container',
    path: '/',
    height: '500px',
    showActions: true,
    onEdit: editFile,
    onRefresh: function(path) {
      updateStorageStats(path || '/');
    }
  });
}
function updateStorageStats(path) {
  fetch('/api/files/stats?path=' + encodeURIComponent(path || '/')).then(r => r.json()).then(d => {
    if (d.success) {
      const usedMB = (d.used / 1024 / 1024).toFixed(2);
      const totalMB = (d.total / 1024 / 1024).toFixed(2);
      const freeMB = (d.free / 1024 / 1024).toFixed(2);
      document.getElementById('storage-text').textContent = usedMB + ' MB / ' + totalMB + ' MB (' + freeMB + ' MB free)';
      document.getElementById('storage-bar').style.width = d.usagePercent + '%';
      if (d.usagePercent > 80) {
        document.getElementById('storage-bar').style.background = 'linear-gradient(90deg,#dc3545,#c82333)';
      } else if (d.usagePercent > 45) {
        document.getElementById('storage-bar').style.background = 'linear-gradient(90deg,#ffc107,#ff9800)';
      } else {
        document.getElementById('storage-bar').style.background = 'linear-gradient(90deg,#28a745,#20c997)';
      }
    } else {
      document.getElementById('storage-text').textContent = d.error || 'Storage unavailable';
      document.getElementById('storage-bar').style.width = '0%';
      document.getElementById('storage-bar').style.background = 'linear-gradient(90deg,#666,#444)';
    }
  }).catch(e => console.error('Storage stats error:', e));
}
function editFile(filePath) {
  currentEditPath = filePath;
  document.getElementById('editor-title').textContent = 'Edit File';
  document.getElementById('editor-path').textContent = currentEditPath;
  document.getElementById('editor-status').textContent = 'Loading...';
  document.getElementById('editor-text').value = '';
  document.getElementById('editor-modal').style.display = 'block';
  isJsonEdit = currentEditPath.toLowerCase().endsWith('.json');
  document.getElementById('btn-pretty').style.display = isJsonEdit ? 'inline-block' : 'none';
  document.getElementById('btn-raw').style.display = isJsonEdit ? 'inline-block' : 'none';
  fetch('/api/files/read?name=' + encodeURIComponent(currentEditPath)).then(r=>r.text()).then(txt=>{ document.getElementById('editor-text').value = txt; document.getElementById('editor-status').textContent=''; }).catch(e=>{ document.getElementById('editor-status').textContent = 'Error: ' + e.message; });
}
function closeEditor(){ document.getElementById('editor-modal').style.display = 'none'; }
function saveEditor(){
  const content = document.getElementById('editor-text').value;
  if (isJsonEdit) {
    try { JSON.parse(content); } catch (e) { document.getElementById('editor-status').textContent = 'Invalid JSON: ' + e.message; return; }
  }
  document.getElementById('editor-status').textContent = 'Saving...';
  fetch('/api/files/write', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body: 'name=' + encodeURIComponent(currentEditPath) + '&content=' + encodeURIComponent(content) })
    .then(r=>r.json()).then(j=>{ 
      if(j && j.success){ 
        document.getElementById('editor-status').textContent='Saved.'; 
        if(fileManager) fileManager.refresh(); 
      } else { 
        document.getElementById('editor-status').textContent = 'Error: ' + (j && j.error ? j.error : 'Unknown'); 
      } 
    })
    .catch(e=>{ document.getElementById('editor-status').textContent = 'Error: ' + e.message; });
}
function prettyJSON(){ if(!isJsonEdit) return; const ta=document.getElementById('editor-text'); try{ const obj=JSON.parse(ta.value); ta.value = JSON.stringify(obj, null, 2); document.getElementById('editor-status').textContent='Pretty-printed JSON.'; } catch(e){ document.getElementById('editor-status').textContent='Invalid JSON: ' + e.message; } }
function rawJSON(){ if(!isJsonEdit) return; const ta=document.getElementById('editor-text'); try{ const obj=JSON.parse(ta.value); ta.value = JSON.stringify(obj); document.getElementById('editor-status').textContent='Minified JSON.'; } catch(e){ document.getElementById('editor-status').textContent='Invalid JSON: ' + e.message; } }

// ===========================================================================
// Bonded-device file browser (master only). Lists the bonded device's
// filesystem and pulls files back to THIS device, all over the bond session
// token (the 'remote:' command path) — no username/password. The toggle is
// revealed ONLY when this device is bonded AND is the master; otherwise it stays
// hidden (never shown as a dead/disabled control).
// ===========================================================================
var bondFs = { peerMac: '', peerName: '', localMac: '', seq: 0, path: '/' };

function fsToken(mac){ return (mac || '').replace(/:/g, '').toUpperCase(); }

function checkBondedFsAvailable(){
  fetch('/api/bond/status').then(function(r){return r.json();}).then(function(d){
    if (d && d.bonded === true && d.role === 1 && d.peerMac){
      bondFs.peerMac = d.peerMac;
      bondFs.peerName = d.peerName || 'bonded device';
      bondFs.localMac = d.localMac || '';
      var t = document.getElementById('fs-source-toggle');
      if (t) t.style.display = 'flex';
      showLocalFs();
    }
  }).catch(function(){ /* not bonded / endpoint absent — leave toggle hidden */ });
}

function setFsButtons(local){
  var bl = document.getElementById('fs-btn-local');
  var bb = document.getElementById('fs-btn-bonded');
  if (bl) bl.style.opacity = local ? '1' : '0.55';
  if (bb) bb.style.opacity = local ? '0.55' : '1';
}

function showLocalFs(){
  var lc = document.getElementById('file-manager-container');
  var bc = document.getElementById('bonded-fs-container');
  if (lc) lc.style.display = '';
  if (bc) bc.style.display = 'none';
  setFsButtons(true);
  updateStorageStats('/');
}

function showBondedFs(){
  var lc = document.getElementById('file-manager-container');
  var bc = document.getElementById('bonded-fs-container');
  if (lc) lc.style.display = 'none';
  if (bc) bc.style.display = '';
  setFsButtons(false);
  bondBrowse(bondFs.path || '/');
}

// Parse the remote 'files' text listing ("  name (N items)" / "  name (N bytes)").
function bondParseListing(lines){
  var entries = [], seen = {};
  var re = /^\s+(.+?)\s+\((\d+)\s+(items|bytes)\)(\s+\[mount\])?\s*$/;
  for (var i=0;i<lines.length;i++){
    var line = String(lines[i] || '');
    if (!line || line.indexOf('Files (') >= 0 || line.indexOf('Total:') >= 0 || line.indexOf('No files found') >= 0) continue;
    var m = line.match(re);
    if (!m) continue;
    var name = m[1].trim();
    if (!name || seen[name]) continue;
    seen[name] = true;
    var isDir = (m[3] === 'items') || !!m[4];
    entries.push({ name: name, isDir: isDir, meta: isDir ? (m[2] + ' items') : (m[2] + ' bytes') });
  }
  entries.sort(function(a,b){ if (a.isDir !== b.isDir) return a.isDir ? -1 : 1; return a.name < b.name ? -1 : (a.name > b.name ? 1 : 0); });
  return entries;
}

function bondJoin(base, name){ if (base === '/' || base === '') return '/' + name; return (base.charAt(base.length-1) === '/' ? base : base + '/') + name; }
function bondEsc(s){ return String(s).replace(/\\/g,'\\\\').replace(/'/g,"\\'"); }

function bondRender(path, entries, statusMsg){
  var c = document.getElementById('bonded-fs-container');
  if (!c) return;
  bondFs.path = path;
  var h = '';
  h += '<div style="display:flex;align-items:center;gap:8px;margin-bottom:6px;flex-wrap:wrap">';
  h += '<strong>' + (bondFs.peerName || 'Bonded device') + '</strong>';
  h += '<span style="font-size:0.8rem;color:var(--muted)">' + (statusMsg || ('Path: ' + path)) + '</span>';
  h += '<button class="btn" style="margin-left:auto;font-size:0.8em;padding:3px 10px" onclick="bondBrowse(bondFs.path)">Refresh</button>';
  h += '</div>';
  h += '<div style="font-size:0.85rem;margin-bottom:6px"><span style="cursor:pointer;color:var(--link)" onclick="bondBrowse(\'/\')">/</span>';
  var segs = path.split('/').filter(function(s){return s.length>0;}); var acc = '';
  for (var s=0;s<segs.length;s++){ acc += '/' + segs[s]; h += '<span style="color:var(--muted)"> / </span><span style="cursor:pointer;color:var(--link)" onclick="bondBrowse(\'' + bondEsc(acc) + '\')">' + segs[s] + '</span>'; }
  h += '</div>';
  h += '<div style="border:1px solid var(--border);border-radius:6px;overflow:hidden">';
  if (path !== '/'){
    var trimmed = path.replace(/\/+$/,''); var idx = trimmed.lastIndexOf('/'); var parent = idx <= 0 ? '/' : trimmed.substring(0, idx);
    h += '<div style="padding:7px 10px;border-bottom:1px solid var(--border);cursor:pointer;color:var(--link)" onclick="bondBrowse(\'' + bondEsc(parent) + '\')">[..]</div>';
  }
  if (!entries || entries.length === 0){
    h += '<div style="padding:10px;color:var(--muted);text-align:center">Empty directory</div>';
  } else {
    for (var e=0;e<entries.length;e++){
      var en = entries[e]; var full = bondEsc(bondJoin(path, en.name));
      if (en.isDir){
        h += '<div style="display:flex;gap:8px;align-items:center;padding:7px 10px;border-bottom:1px solid var(--border);cursor:pointer" onclick="bondBrowse(\'' + full + '\')">';
        h += '<span style="color:var(--link);font-family:monospace">[D]</span><span>' + en.name + '</span><span style="margin-left:auto;font-size:0.78em;color:var(--muted)">' + en.meta + '</span></div>';
      } else {
        h += '<div style="display:flex;gap:8px;align-items:center;padding:7px 10px;border-bottom:1px solid var(--border)">';
        h += '<span style="font-family:monospace">[F]</span><span>' + en.name + '</span><span style="margin-left:auto;font-size:0.78em;color:var(--muted)">' + en.meta + '</span>';
        h += '<button class="btn" style="font-size:0.75em;padding:2px 8px" onclick="bondDownload(\'' + full + '\')">Download</button></div>';
      }
    }
  }
  h += '</div>';
  c.innerHTML = h;
}

// Snapshot the peer's current max message seq, then run a command on the peer.
function bondSeqBaseline(cb){
  fetch('/api/espnow/messages?since=0&mac=' + encodeURIComponent(bondFs.peerMac))
    .then(function(r){return r.json();}).then(function(d){
      var msgs = (d && d.messages) ? d.messages : [];
      for (var i=0;i<msgs.length;i++){ if ((msgs[i].seq||0) > bondFs.seq) bondFs.seq = msgs[i].seq; }
    }).catch(function(){}).finally(cb);
}

function bondBrowse(path){
  path = path || '/';
  bondRender(path, [], 'Requesting directory…');
  bondSeqBaseline(function(){
    fetch('/api/bond/exec', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd=' + encodeURIComponent('files ' + path) })
      .then(function(r){return r.json();}).then(function(d){
        if (!d || !d.success){ bondRender(path, [], 'Error: ' + ((d && (d.result||d.error)) || 'command failed')); return; }
        var lines = [], done = false, polls = 0;
        var timer = setInterval(function(){
          polls++;
          fetch('/api/espnow/messages?since=' + bondFs.seq + '&mac=' + encodeURIComponent(bondFs.peerMac))
            .then(function(r){return r.json();}).then(function(md){
              var ms = (md && md.messages) ? md.messages : [];
              for (var i=0;i<ms.length;i++){ var m = ms[i]; if (m.seq > bondFs.seq) bondFs.seq = m.seq; if (m.msg){ var parts = String(m.msg).split('\n'); for (var p=0;p<parts.length;p++) lines.push(parts[p]); if (String(m.msg).indexOf('Total:') >= 0) done = true; } }
              if (done){ clearInterval(timer); bondRender(path, bondParseListing(lines), null); }
              else if (polls >= 20){ clearInterval(timer); bondRender(path, bondParseListing(lines), lines.length ? null : 'Timed out waiting for ' + (bondFs.peerName||'peer')); }
            }).catch(function(){});
        }, 500);
      }).catch(function(e){ bondRender(path, [], 'Error: ' + e.message); });
  });
}

// Pull a file from the bonded device onto THIS device, then download it locally.
function bondDownload(remotePath){
  if (!bondFs.localMac){ alert('Local MAC unavailable — cannot pull file.'); return; }
  var base = remotePath.split('/').pop();
  var dir = '/espnow/received/' + fsToken(bondFs.peerMac);
  var localUrl = '/api/files/read?name=' + encodeURIComponent(dir + '/' + base);
  bondRender(bondFs.path, bondParseListing([]), 'Pulling ' + base + '…');
  fetch('/api/bond/exec', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'cmd=' + encodeURIComponent('espnow sendfile ' + bondFs.localMac + ' ' + remotePath) })
    .then(function(r){return r.json();}).then(function(d){
      if (!d || !d.success){ alert('Pull failed: ' + ((d && (d.result||d.error)) || 'command failed')); bondBrowse(bondFs.path); return; }
      // Poll the LOCAL filesystem until the received file appears, then download it.
      var polls = 0;
      var timer = setInterval(function(){
        polls++;
        fetch('/api/files/list?path=' + encodeURIComponent(dir)).then(function(r){return r.json();}).then(function(ld){
          var items = (ld && ld.files) ? ld.files : (Array.isArray(ld) ? ld : []);
          var found = false;
          for (var i=0;i<items.length;i++){ var nm = items[i] && items[i].name ? items[i].name : items[i]; if (nm === base || String(nm).split('/').pop() === base){ found = true; break; } }
          if (found){ clearInterval(timer); bondBrowse(bondFs.path); window.location = localUrl; }
          else if (polls >= 30){ clearInterval(timer); bondBrowse(bondFs.path); alert('Timed out pulling ' + base + ' from ' + (bondFs.peerName||'peer')); }
        }).catch(function(){ if (polls >= 30){ clearInterval(timer); } });
      }, 600);
    }).catch(function(e){ alert('Pull error: ' + e.message); bondBrowse(bondFs.path); });
}
</script>
)JS", HTTPD_RESP_USE_STRLEN);

}

#endif
