#ifndef WEBPAGE_FILES_H
#define WEBPAGE_FILES_H

#include "WebServer_Utils.h"

// Streamed inner content for files page
inline void streamFilesInner(httpd_req_t* req) {
  // Stream shared file browser scripts
  String fbScript = getFileBrowserScript();
  httpd_resp_send_chunk(req, fbScript.c_str(), fbScript.length());
  // HTML structure
  httpd_resp_send_chunk(req, R"HTML(
<h2>File Manager</h2>
<p>Browse and manage files on the device filesystem</p>
<div id='storage-stats' style='background:var(--panel-bg);padding:1rem;border-radius:8px;margin:1rem 0;color:var(--panel-fg);border:1px solid var(--border)'>
  <div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:0.5rem'>
    <strong>Storage</strong>
    <span id='storage-text' style='font-size:0.9rem'>Loading...</span>
  </div>
  <div style='width:100%;height:20px;background:rgba(255,255,255,.15);border-radius:10px;overflow:hidden;border:1px solid var(--border)'>
    <div id='storage-bar' style='height:100%;background:linear-gradient(90deg,#28a745,#20c997);width:0%;transition:width 0.3s'></div>
  </div>
</div>
<div id='file-manager-container' style='margin:1rem 0'></div>
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
  updateStorageStats();
  initFileManager();
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
    onRefresh: function() {
      updateStorageStats();
    }
  });
}
function updateStorageStats() {
  fetch('/api/files/stats').then(r => r.json()).then(d => {
    if (d.success) {
      const usedMB = (d.used / 1024 / 1024).toFixed(2);
      const totalMB = (d.total / 1024 / 1024).toFixed(2);
      const freeMB = (d.free / 1024 / 1024).toFixed(2);
      document.getElementById('storage-text').textContent = usedMB + ' MB / ' + totalMB + ' MB (' + freeMB + ' MB free)';
      document.getElementById('storage-bar').style.width = d.usagePercent + '%';
      if (d.usagePercent > 90) {
        document.getElementById('storage-bar').style.background = 'linear-gradient(90deg,#dc3545,#c82333)';
      } else if (d.usagePercent > 75) {
        document.getElementById('storage-bar').style.background = 'linear-gradient(90deg,#ffc107,#ff9800)';
      } else {
        document.getElementById('storage-bar').style.background = 'linear-gradient(90deg,#28a745,#20c997)';
      }
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
</script>
)JS", HTTPD_RESP_USE_STRLEN);

}

#endif
