#ifndef WEBPAGE_FILES_H
#define WEBPAGE_FILES_H

#include "WebServer_Utils.h"
#include "WebPage_AviPlayer.h"

// Streamed inner content for files page
inline void streamFilesInner(httpd_req_t* req) {
  // Stream shared file browser scripts
  httpd_resp_send_chunk(req, getFileBrowserScript(), HTTPD_RESP_USE_STRLEN);
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
    <textarea id='editor-text' style='flex:1;min-height:300px;width:70vw;max-width:85vw;box-sizing:border-box;padding:0.75rem;border:1px solid var(--border);border-radius:6px;font-family:Menlo,Consolas,monospace;font-size:13px;line-height:1.4;color:var(--panel-fg);background:var(--panel-bg);' data-guest-hide></textarea>
    <div style='margin-top:0.6rem;display:flex;gap:0.5rem;align-items:center;flex-wrap:wrap'>
      <button id='editor-save' class='btn' onclick="saveEditor()" data-guest-hide>Save</button>
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
// Apply storage-bar styling for either local or bonded stats. Shared so the
// bar reflects whichever device the user is currently looking at — switching
// between This Device and Bonded Device repaints it.
function applyStorageStats(used, total, free, percent, prefix) {
  const usedMB = (used / 1024 / 1024).toFixed(2);
  const totalMB = (total / 1024 / 1024).toFixed(2);
  const freeMB = (free / 1024 / 1024).toFixed(2);
  hw.$('storage-text').textContent = (prefix || '') + usedMB + ' MB / ' + totalMB + ' MB (' + freeMB + ' MB free)';
  hw.$('storage-bar').style.width = percent + '%';
  if (percent > 80) {
    hw.$('storage-bar').style.background = 'linear-gradient(90deg,#dc3545,#c82333)';
  } else if (percent > 45) {
    hw.$('storage-bar').style.background = 'linear-gradient(90deg,#ffc107,#ff9800)';
  } else {
    hw.$('storage-bar').style.background = 'linear-gradient(90deg,#28a745,#20c997)';
  }
}

function setStorageError(msg) {
  hw.$('storage-text').textContent = msg || 'Storage unavailable';
  hw.$('storage-bar').style.width = '0%';
  hw.$('storage-bar').style.background = 'linear-gradient(90deg,#666,#444)';
}

function updateStorageStats(path) {
  hw.fetchJSON('/api/files/stats?path=' + encodeURIComponent(path || '/')).then(d => {
    if (d.success) {
      applyStorageStats(d.used, d.total, d.free, d.usagePercent, '');
    } else {
      setStorageError(d.error);
    }
  }).catch(e => console.error('Storage stats error:', e));
}

// Bonded storage stats — structured FS_STAT_REQ via BondFs.stat. Replaces a
// fragile 4-line CLI-text scrape (Total / Used / Free / Usage) that had to
// fight doneMarker collisions with concurrent commands. Typed JSON now;
// "Worker: " prefix kept so the storage bar shows which device's numbers
// are being displayed.
function updateBondedStorageStats() {
  if (!window.BondFs) { setStorageError('BondFs unavailable'); return; }
  setStorageError('Fetching worker storage…');
  window.BondFs.stat('/', function(s, err){
    if (err || !s) { setStorageError('Worker storage: ' + (err || 'unknown')); return; }
    applyStorageStats(s.used, s.total, s.free, s.usagePercent, 'Worker: ');
  });
}
function editFile(filePath) {
  currentEditPath = filePath;
  hw.$('editor-title').textContent = 'Edit File';
  hw.$('editor-path').textContent = currentEditPath;
  hw.$('editor-status').textContent = 'Loading...';
  hw.$('editor-text').value = '';
  hw.$('editor-modal').style.display = 'block';
  isJsonEdit = currentEditPath.toLowerCase().endsWith('.json');
  hw.$('btn-pretty').style.display = isJsonEdit ? 'inline-block' : 'none';
  hw.$('btn-raw').style.display = isJsonEdit ? 'inline-block' : 'none';
  hw.fetchText('/api/files/read?name=' + encodeURIComponent(currentEditPath)).then(txt=>{ hw.$('editor-text').value = txt; hw.$('editor-status').textContent=''; }).catch(e=>{ hw.$('editor-status').textContent = 'Error: ' + e.message; });
}
function closeEditor(){ hw.$('editor-modal').style.display = 'none'; }
function saveEditor(){
  const content = hw.$('editor-text').value;
  if (isJsonEdit) {
    try { JSON.parse(content); } catch (e) { hw.$('editor-status').textContent = 'Invalid JSON: ' + e.message; return; }
  }
  hw.$('editor-status').textContent = 'Saving...';
  hw.postForm('/api/files/write', { name: currentEditPath, content: content })
    .then(r=>r.json()).then(j=>{
      if(j && j.success){ 
        hw.$('editor-status').textContent='Saved.'; 
        if(fileManager) fileManager.refresh(); 
      } else { 
        hw.$('editor-status').textContent = 'Error: ' + (j && j.error ? j.error : 'Unknown'); 
      } 
    })
    .catch(e=>{ hw.$('editor-status').textContent = 'Error: ' + e.message; });
}
function prettyJSON(){ if(!isJsonEdit) return; const ta=hw.$('editor-text'); try{ const obj=JSON.parse(ta.value); ta.value = JSON.stringify(obj, null, 2); hw.$('editor-status').textContent='Pretty-printed JSON.'; } catch(e){ hw.$('editor-status').textContent='Invalid JSON: ' + e.message; } }
function rawJSON(){ if(!isJsonEdit) return; const ta=hw.$('editor-text'); try{ const obj=JSON.parse(ta.value); ta.value = JSON.stringify(obj); hw.$('editor-status').textContent='Minified JSON.'; } catch(e){ hw.$('editor-status').textContent='Invalid JSON: ' + e.message; } }

// ===========================================================================
// Bonded-device file browser (master only). Browse + download the peer's FS
// over the bond session token. The heavy lifting lives in window.BondFs (shared,
// defined in getFileBrowserScript). The toggle is revealed ONLY when this device
// is bonded AND is the master; otherwise it stays hidden (never disabled).
// ===========================================================================
var bondCurPath = '/';

function checkBondedFsAvailable(){
  if (!window.BondFs) return;
  window.BondFs.checkAvailable(function(ok){
    if (!ok) return;
    var t = hw.$('fs-source-toggle');
    if (t) t.style.display = 'flex';
    showLocalFs();
  });
}

function setFsButtons(local){
  var bl = hw.$('fs-btn-local');
  var bb = hw.$('fs-btn-bonded');
  if (bl) bl.style.opacity = local ? '1' : '0.55';
  if (bb) bb.style.opacity = local ? '0.55' : '1';
}

function showLocalFs(){
  var lc = hw.$('file-manager-container');
  var bc = hw.$('bonded-fs-container');
  hw.show(lc);
  hw.hide(bc);
  setFsButtons(true);
  updateStorageStats('/');
}

function showBondedFs(){
  var lc = hw.$('file-manager-container');
  var bc = hw.$('bonded-fs-container');
  hw.hide(lc);
  hw.show(bc);
  setFsButtons(false);
  // Do NOT also call updateBondedStorageStats() here. bondBrowse's onResult
  // already calls it after the listing completes — serializing the two execs
  // (files-listing first, fsusage second) is what makes the page reliable.
  // Firing both in parallel triggered races that produced inconsistent
  // "No files found" / "parse failed" / "timed out" outcomes (see commit
  // notes for the three-way race diagnosis).
  bondBrowse(bondCurPath || '/');
}

function bondBrowse(path){
  path = path || '/';
  bondCurPath = path;
  if (!window.BondFs) return;
  window.BondFs.renderExplorer('bonded-fs-container', path, [], { onNavigate: bondBrowse, fileActions: [], status: 'Requesting directory…' });
  window.BondFs.list(path, function(entries, err){
    if (entries === null){
      window.BondFs.renderExplorer('bonded-fs-container', path, [], { onNavigate: bondBrowse, fileActions: [], status: 'Error: ' + (err || 'failed') });
      return;
    }
    // Each successful directory listing also refreshes worker storage stats.
    // Matches the local view's behavior of updating the bar on every nav, and
    // catches storage shifts caused by remote downloads or peer-side edits.
    updateBondedStorageStats();
    window.BondFs.renderExplorer('bonded-fs-container', path, entries, {
      onNavigate: bondBrowse,
      // Both actions go through BondFs.pull which uses the existing bond
      // file-transfer pipeline (master sends `remote:espnowsendfile`, worker
      // chunks the file back, master caches at /espnow/received/<MAC>/).
      // The same cached copy is reused for both Download and View — only the
      // browser-side disposition differs (force-save vs. inline render).
      fileActions: [
        {
          label: 'View',
          title: 'Pull this file from the worker via bond, then open it in a new tab for inline viewing (text/JSON/images render natively; AVI opens in the player modal)',
          fn: bondView
        },
        {
          label: 'Download',
          title: 'Pull this file from the worker via bond, then save it to your computer (also cached on master at /espnow/received/<MAC>/)',
          fn: bondDownload
        }
      ]
    });
  });
}

function bondDownload(remotePath){
  if (!window.BondFs) return;
  window.BondFs.pull(remotePath, function(res, err){
    if (err || !res){ alert('Pull failed: ' + (err || 'unknown')); return; }
    // window.location = url would NAVIGATE to the file, which for JSON/text/
    // image types makes the browser display it inline instead of downloading
    // it. Use a temporary <a download="..."> click so the browser saves the
    // file to the user's Downloads folder — matches what the local file
    // browser does for its Download button.
    var a = document.createElement('a');
    a.href = res.localUrl;
    a.download = res.base || 'download';
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
  });
}

// View action — same pull, different open. Mirrors the local file browser's
// ViewFile path (WebServer_Utils.h:914): AVI files open in the AVI player
// modal if it's on the page; everything else opens via /api/files/view which
// gives a Pretty/Raw text viewer with proper Content-Type for text/JSON and
// inline image rendering. The cached file at /espnow/received/<MAC>/<name>
// is served by the standard local file-view endpoint, so the worker's file
// renders identically to one that lives on the master natively.
function bondView(remotePath){
  if (!window.BondFs) return;
  window.BondFs.pull(remotePath, function(res, err){
    if (err || !res){ alert('Pull failed: ' + (err || 'unknown')); return; }
    var lower = (res.base || '').toLowerCase();
    if (lower.endsWith('.avi') && typeof window.openAviPlayer === 'function') {
      // openAviPlayer takes just the basename — it knows the camera dir. The
      // cached AVI lives under /espnow/received/<MAC>/, not where the player
      // expects, so for now bail out with a hint; future work could teach the
      // player to accept a full path or move the cached AVI into the camera
      // dir before play.
      alert('AVI playback for bonded files is not yet wired up.\nFile cached at: ' + res.localPath);
      return;
    }
    window.open('/api/files/view?name=' + encodeURIComponent(res.localPath), '_blank');
  });
}
</script>
)JS", HTTPD_RESP_USE_STRLEN);

}

#endif
