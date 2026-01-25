// WebServer_Utils.h - Web server utility functions, helpers, and shared HTML/JS
// Merged from WebCore_Utils.h and WebCore_Shared.h

#ifndef WEBSERVER_UTILS_H
#define WEBSERVER_UTILS_H

#include "System_BuildConfig.h"

#if ENABLE_HTTP_SERVER

#include <Arduino.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ============================================================================
// Web Mirror Buffer - CLI output buffer for web interface
// ============================================================================

struct WebMirrorBuf {
  char* buf;
  size_t cap;  // maximum bytes stored (excluding null)
  size_t len;  // current length
  SemaphoreHandle_t mutex;  // Protects concurrent access
  
  WebMirrorBuf();
  void init(size_t capacity);
  void clear();
  void append(const String& s, bool needNewline);
  void append(const char* s, bool needNewline);
  void appendDirect(const char* s, size_t slen, bool needNewline);  // Zero-copy append
  String snapshot();
  size_t snapshotTo(char* dest, size_t destSize);  // Zero-copy snapshot
  void assignFrom(const String& s);
};

extern WebMirrorBuf gWebMirror;
extern size_t gWebMirrorCap;

// ============================================================================
// HTTP Request Utilities
// ============================================================================

// Get client IP address from request
void getClientIP(httpd_req_t* req, char* ipBuf, size_t bufSize);
void getClientIP(httpd_req_t* req, String& ipOut);

// Get header value from request
bool getHeaderValue(httpd_req_t* req, const char* name, String& out);

// Get cookie value from request
bool getCookieValue(httpd_req_t* req, const char* key, String& out);

// Get session ID from cookie
String getCookieSID(httpd_req_t* req);

// Generate a random session token
String makeSessToken();

// ============================================================================
// Navigation HTML Generation
// ============================================================================

// Generate navigation bar for authenticated users
String generateNavigation(const String& activePage, const String& username);

// Generate navigation bar for public (unauthenticated) pages
String generatePublicNavigation();

// ============================================================================
// Shared HTML/JS Utilities (inline for header-only usage)
// ============================================================================

// Forward declarations
void streamCommonCSS(httpd_req_t* req);  // Streaming CSS - no String allocation

// Render a generic two-field form with two buttons using shared classes
// title: heading for the form
// subtitle: small helper text under the title (optional)
// action, method: form target and HTTP method
// Field 1: label1, name1, value1, type1 (e.g., text, email)
// Field 2: label2, name2, value2, type2 (e.g., password)
// primaryText: primary button text
// secondaryText, secondaryHref: secondary action link
// errorMsg: optional error message to display above the form
inline String renderTwoFieldForm(
  const String& title,
  const String& subtitle,
  const String& action,
  const String& method,
  const String& label1,
  const String& name1,
  const String& value1,
  const String& type1,
  const String& label2,
  const String& name2,
  const String& value2,
  const String& type2,
  const String& primaryText,
  const String& secondaryText,
  const String& secondaryHref,
  const String& errorMsg
) {
  String html;
  html += "<div class='panel container-narrow space-top-md'>";
  html += "  <div class='text-center space-bottom-sm'>";
  html += "    <h2>" + title + "</h2>";
  if (subtitle.length()) {
    html += "    <p class='text-muted' style='margin:0'>" + subtitle + "</p>";
  }
  html += "  </div>";

  if (errorMsg.length()) {
    html += "  <div id='err' class='form-error text-danger'>" + errorMsg + "</div>";
  } else {
    html += "  <div id='err' class='form-error' style='display:none'></div>";
  }

  html += "  <form method='" + method + "' action='" + action + "'>";
  html += "    <div class='form-field'><label>" + label1 + "</label>";
  html += "      <input class='form-input' name='" + name1 + "' value='" + value1 + "' type='" + type1 + "'></div>";
  html += "    <div class='form-field'><label>" + label2 + "</label>";
  html += "      <input class='form-input' name='" + name2 + "' value='" + value2 + "' type='" + type2 + "'></div>";
  html += "    <div class='btn-row space-top-md'>";
  html += "      <button class='btn btn-primary' type='submit'>" + primaryText + "</button>";
  if (secondaryText.length()) {
    html += "      <a class='btn btn-secondary' href='" + secondaryHref + "'>" + secondaryText + "</a>";
  }
  html += "    </div>";
  html += "  </form>";
  html += "</div>";
  return html;
}

// Generic File Explorer Utility
// This provides reusable JavaScript functions for browsing and selecting files from the filesystem
// Usage: Call createFileExplorer(containerId, onSelectCallback) to create a file explorer in any element
inline String getFileBrowserScript() {
  return R"FBSCRIPT(
<script>
// Generic File Explorer Utility
// Creates an interactive file explorer with folder navigation
(function() {
  // Global function to create a file explorer in a container
  window.createFileExplorer = function(config) {
    // Config: {
    //   containerId: string - ID of container to place explorer in
    //   onSelect: function(filePath) - callback when file is selected
    //   path: string - optional root path to browse (default: '/')
    //   filter: function(file) - optional filter function for files
    //   height: string - optional height (default: '300px')
    //   mode: string - 'select' (select only), 'view' (view only), 'full' (all features, default)
    //   selectFilesOnly: boolean - if true, only files can be selected (not folders)
    // }
    
    var container = document.getElementById(config.containerId);
    if (!container) {
      console.error('[FileExplorer] Container not found:', config.containerId);
      return;
    }
    
    var currentPath = config.path || '/';
    var explorerHeight = config.height || '300px';
    var mode = config.mode || 'full';  // 'select', 'view', or 'full'
    var selectFilesOnly = config.selectFilesOnly || false;
    
    // Sanitize for JavaScript function names (no hyphens/colons allowed in JS identifiers)
    var explorerFnId = 'fexp_' + config.containerId.replace(/[^a-zA-Z0-9]/g, '_');
    // DOM IDs can use hyphens
    var explorerId = 'fexp-' + config.containerId;
    var breadcrumbId = explorerId + '-breadcrumb';
    var listId = explorerId + '-list';
    
    var html = '<div id="' + explorerId + '" style="border:1px solid var(--border);border-radius:4px;background:var(--panel-bg);color:var(--panel-fg);overflow:hidden;">';
    html += '<div id="' + breadcrumbId + '" style="padding:8px;background:var(--crumb-bg);border-bottom:1px solid var(--border);font-size:0.9em;color:var(--panel-fg);"></div>';
    html += '<div id="' + listId + '" style="height:' + explorerHeight + ';overflow-y:auto;"></div>';
    html += '</div>';
    
    container.innerHTML = html;
    
    var breadcrumbDiv = document.getElementById(breadcrumbId);
    var listDiv = document.getElementById(listId);
    
    function renderBreadcrumb() {
      var parts = currentPath.split('/').filter(function(p) { return p.length > 0; });
      var html = '<span style="cursor:pointer;color:var(--link);" onclick="' + explorerFnId + 'Navigate(\'/\')">[Root]</span>';
      
      var path = '';
      parts.forEach(function(part, idx) {
        path += '/' + part;
        var finalPath = path;
        html += ' <span style="color:var(--muted);">/</span> ';
        html += '<span style="cursor:pointer;color:var(--link);" onclick="' + explorerFnId + 'Navigate(\'' + finalPath + '\')">' + part + '</span>';
      });
      
      breadcrumbDiv.innerHTML = html;
    }
    
    var iconCache = {};
    var iconLoadFailed = {};
    
    function getFileTypeIconName(filename, isFolder) {
      if (isFolder) return 'folder';
      var ext = filename.toLowerCase().split('.').pop();
      var iconMap = {
        // code
        'js': 'file_code',
        'ts': 'file_code',
        'jsx': 'file_code',
        'tsx': 'file_code',
        'cpp': 'file_code',
        'h': 'file_code',
        'hpp': 'file_code',
        'c': 'file_code',
        'ino': 'file_code',
        'py': 'file_code',
        'sh': 'file_code',
        // structured data
        'json': 'file_json',
        // web documents
        'html': 'file_code',
        'htm': 'file_code',
        'css': 'file_code',
        // text
        'txt': 'file_text',
        'log': 'file_text',
        'md': 'file_text',
        // images
        'jpg': 'file_image',
        'jpeg': 'file_image',
        'png': 'file_image',
        'gif': 'file_image',
        'bmp': 'file_image',
        'svg': 'file_image',
        'ico': 'file_image',
        // documents
        'pdf': 'file_pdf',
        // archives
        'zip': 'file_zip',
        'gz': 'file_zip',
        'tar': 'file_zip',
        '7z': 'file_zip',
        // binaries
        'bin': 'file_bin',
        'dat': 'file_bin'
      };
      return iconMap[ext] || 'file';
    }
    
    function getFileTypeIconFallback(filename, isFolder) {
      if (isFolder) return '[DIR]';
      return '[FILE]';
    }
    
    function renderFileIcon(iconName, fallbackText) {
      function dbgIcons(){try{return !!(window.localStorage&&window.localStorage.getItem('hwDebugIcons')==='1')}catch(_){return false}}
      function logIcons(){try{if(dbgIcons())console.log.apply(console,arguments)}catch(_){}}
      if (iconLoadFailed[iconName]) {
        logIcons('[icons] cached-fail icon=', iconName, 'fallback=', fallbackText);
        return '<span style="display:inline-block;width:32px;font-family:monospace;color:var(--muted);font-size:0.85em;text-align:center;">' + fallbackText + '</span>';
      }
      
      var imgId = 'icon_' + iconName + '_' + Math.random().toString(36).substr(2, 9);
      var iconUrl = '/api/icon?name=' + iconName;
      logIcons('[icons] render icon=', iconName, 'url=', iconUrl);
      var html = '<img id="' + imgId + '" src="' + iconUrl + '" width="48" height="48" style="vertical-align:middle;image-rendering:auto;display:inline-block;background:var(--icon-bg);border-radius:6px;padding:4px;box-sizing:border-box;" ';
      html += 'onerror="this.style.display=\'none\';this.nextSibling.style.display=\'inline-block\';" />';
      html += '<span style="display:none;width:48px;font-family:monospace;color:var(--muted);font-size:0.85em;text-align:center;">' + fallbackText + '</span>';

      if (dbgIcons()) {
        setTimeout(function(){
          try {
            var img = document.getElementById(imgId);
            if (!img) {
              console.warn('[icons] element not found id=', imgId, 'icon=', iconName);
              return;
            }
            img.addEventListener('load', function(){
              console.log('[icons] load ok', iconName, 'id', imgId);
            });
            img.addEventListener('error', function(){
              console.warn('[icons] load fail', iconName, 'url', iconUrl);
              iconLoadFailed[iconName] = true;
            });
          } catch (e) {
            try { console.warn('[icons] attach listeners failed', e); } catch(_) {}
          }
        }, 0);
      }

      return html;
    }
    
    function loadDirectory(path) {
      listDiv.innerHTML = '<div style="padding:20px;text-align:center;color:var(--muted);">Loading...</div>';
      
      fetch('/api/files/list?path=' + encodeURIComponent(path))
        .then(function(r) { return r.json(); })
        .then(function(data) {
          if (!data.success || !data.files) {
            listDiv.innerHTML = '<div style="padding:20px;text-align:center;color:var(--danger);">Error loading directory</div>';
            return;
          }
          
          var files = data.files;
          
          // Apply filter if provided
          if (config.filter && typeof config.filter === 'function') {
            files = files.filter(config.filter);
          }
          
          // Sort: folders first, then files, alphabetically
          files.sort(function(a, b) {
            if (a.type === 'folder' && b.type !== 'folder') return -1;
            if (a.type !== 'folder' && b.type === 'folder') return 1;
            return a.name.localeCompare(b.name);
          });
          
          if (files.length === 0) {
            listDiv.innerHTML = '<div style="padding:20px;text-align:center;color:var(--muted);">No files found</div>';
            return;
          }
          
          var html = '<div style="padding:4px;">';
          files.forEach(function(file) {
            var isFolder = file.type === 'folder';
            var itemPath = (currentPath === '/' ? '/' : currentPath + '/') + file.name;
            var sizeInfo = file.size || '';
            
            // Format size for files
            if (!isFolder && sizeInfo.indexOf('bytes') >= 0) {
              var match = sizeInfo.match(/(\d+)/);
              if (match) {
                var bytes = parseInt(match[1]);
                if (bytes >= 1048576) {
                  sizeInfo = (bytes / 1048576).toFixed(2) + ' MB';
                } else if (bytes >= 1024) {
                  sizeInfo = (bytes / 1024).toFixed(2) + ' KB';
                } else {
                  sizeInfo = bytes + ' B';
                }
              }
            }
            
            // Determine interaction based on mode
            var canInteract = true;
            var clickAction = '';
            
            if (mode === 'select') {
              // Select mode: folders navigate, files select (if selectFilesOnly is true)
              if (isFolder) {
                clickAction = explorerFnId + 'Navigate(\'' + itemPath + '\')';
              } else {
                clickAction = explorerFnId + 'Select(\'' + itemPath + '\')';
              }
            } else if (mode === 'view') {
              // View mode: folders navigate, files do nothing (view-only)
              if (isFolder) {
                clickAction = explorerFnId + 'Navigate(\'' + itemPath + '\')';
              } else {
                canInteract = false;
              }
            } else {
              // Full mode: folders navigate, files select
              if (isFolder) {
                clickAction = explorerFnId + 'Navigate(\'' + itemPath + '\')';
              } else {
                clickAction = explorerFnId + 'Select(\'' + itemPath + '\')';
              }
            }
            
            var bgColor = 'var(--panel-bg)';
            var cursor = canInteract ? 'pointer' : 'default';
            var hoverStyle = canInteract ? 'onmouseover="this.style.background=\'var(--crumb-bg)\'" onmouseout="this.style.background=\'' + bgColor + '\'"' : '';
            
            html += '<div style="padding:8px 12px;border-bottom:1px solid var(--border);display:flex;justify-content:space-between;align-items:center;background:' + bgColor + ';" ' + hoverStyle + '>';
            
            // File/folder name (clickable)
            html += '<span style="flex:1;color:var(--panel-fg);font-size:0.95em;cursor:' + cursor + ';display:flex;align-items:center;gap:8px;"';
            if (clickAction) {
              html += ' onclick="' + clickAction + '"';
            }
            var iconName = getFileTypeIconName(file.name, isFolder);
            var fallbackText = getFileTypeIconFallback(file.name, isFolder);
            html += '>' + renderFileIcon(iconName, fallbackText) + '<span>' + file.name + '</span></span>';
            
            // Size info
            html += '<span style="color:var(--muted);font-size:0.85em;margin-left:12px;min-width:80px;text-align:right;">' + sizeInfo + '</span>';
            
            // Delete button (only in full mode) - icon-only if available, text-only fallback
            if (mode === 'full') {
              var trashIconId = 'trash_' + itemPath.replace(/[^a-zA-Z0-9]/g, '_') + '_' + Math.random().toString(36).substr(2, 9);
              html += '<button class="btn btn-small" id="' + trashIconId + '" onclick="' + explorerFnId + 'Delete(\'' + itemPath + '\',' + (isFolder ? 'true' : 'false') + ');event.stopPropagation();" ';
              html += 'style="margin-left:8px;padding:4px 8px;">';
              html += renderFileIcon('trash', 'Delete');
              html += '</button>';
            }
            
            html += '</div>';
          });
          html += '</div>';
          
          listDiv.innerHTML = html;
        })
        .catch(function(e) {
          console.error('[FileExplorer] Failed to load directory:', e);
          listDiv.innerHTML = '<div style="padding:20px;text-align:center;color:var(--danger);">Error: ' + e.message + '</div>';
        });
    }
    
    // Global navigation function (needs to be accessible from onclick)
    window[explorerFnId + 'Navigate'] = function(path) {
      currentPath = path;
      renderBreadcrumb();
      loadDirectory(path);
      
      // Notify parent if onNavigate callback provided
      if (config.onNavigate && typeof config.onNavigate === 'function') {
        config.onNavigate(path);
      }
    };
    
    // Global select function
    window[explorerFnId + 'Select'] = function(filePath) {
      if (config.onSelect && typeof config.onSelect === 'function') {
        config.onSelect(filePath);
      }
    };
    
    // Global delete function
    window[explorerFnId + 'Delete'] = function(filePath, isFolder) {
      var itemType = isFolder ? 'folder' : 'file';
      var confirmMsg = 'Delete ' + itemType + ' "' + filePath + '"?';
      if (isFolder) {
        confirmMsg += '\n\nNote: Folder must be empty to delete.';
      }
      
      if (!confirm(confirmMsg)) return;
      
      var cmd = isFolder ? 'rmdir ' + filePath : 'filedelete ' + filePath;
      
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'cmd=' + encodeURIComponent(cmd)
      })
      .then(function(r) { return r.text(); })
      .then(function(txt) {
        if (txt.indexOf('Error') >= 0 || txt.indexOf('Failed') >= 0) {
          alert('Delete failed: ' + txt);
        } else {
          // Reload directory on success
          loadDirectory(currentPath);
        }
      })
      .catch(function(e) {
        alert('Delete error: ' + e.message);
      });
    };
    
    // Initial load
    renderBreadcrumb();
    loadDirectory(currentPath);
    
    return {
      navigate: function(path) {
        window[explorerFnId + 'Navigate'](path);
      },
      getCurrentPath: function() {
        return currentPath;
      }
    };
  };
  
  // Helper: Create file explorer with auto-fill to an input field
  window.createFileExplorerWithInput = function(config) {
    // Config: {
    //   explorerContainerId: string - where to place the explorer
    //   inputId: string - ID of input field to auto-fill
    //   mode: string - 'select' (default for this helper), 'view', or 'full'
    //   selectFilesOnly: boolean - default true for this helper
    //   ... other createFileExplorer options
    // }
    
    var inputId = config.inputId;
    var originalOnSelect = config.onSelect;
    
    // Default to select mode for input helper
    if (!config.mode) config.mode = 'select';
    if (config.selectFilesOnly === undefined) config.selectFilesOnly = true;
    
    config.onSelect = function(filePath) {
      var input = document.getElementById(inputId);
      if (input) {
        input.value = filePath;
      }
      if (originalOnSelect) {
        originalOnSelect(filePath);
      }
    };
    
    // Map explorerContainerId to containerId for createFileExplorer
    config.containerId = config.explorerContainerId;
    
    return window.createFileExplorer(config);
  };
  
  // Full-featured file manager with action buttons
  window.createFileManager = function(config) {
    // Config: {
    //   containerId: string - ID of container element
    //   path: string - initial path (default: '/')
    //   height: string - explorer height (default: '400px')
    //   showActions: boolean - show action buttons (default: true)
    //   mode: string - 'select', 'view', or 'full' (default: 'full')
    //   onRefresh: function() - callback after operations
    // }
    
    var container = document.getElementById(config.containerId);
    if (!container) {
      console.error('[FileManager] Container not found:', config.containerId);
      return;
    }
    
    var currentPath = config.path || '/';
    var managerHeight = config.height || '400px';
    var showActions = config.showActions !== false;
    var mode = config.mode || 'full';
    
    var managerId = 'fmgr_' + config.containerId.replace(/[^a-zA-Z0-9]/g, '_');
    var toolbarId = managerId + '_toolbar';
    var explorerId = managerId + '_explorer';
    var statusId = managerId + '_status';
    
    // Build UI
    var html = '<div id="' + managerId + '" style="border:1px solid var(--border);border-radius:4px;background:var(--panel-bg);color:var(--panel-fg);overflow:hidden;">';
    
    // Toolbar
    if (showActions) {
      html += '<div id="' + toolbarId + '" style="padding:8px;background:var(--crumb-bg);border-bottom:1px solid var(--border);display:flex;gap:8px;flex-wrap:wrap;">';
      html += '<button class="btn" onclick="' + managerId + 'CreateFolder()">New Folder</button>';
      html += '<button class="btn" onclick="' + managerId + 'CreateFile()">New File</button>';
      html += '<button class="btn" onclick="' + managerId + 'UploadFile()">Upload</button>';
      html += '<button class="btn" onclick="' + managerId + 'Refresh()">Refresh</button>';
      html += '<input type="file" id="' + managerId + '_upload_input" style="display:none">';
      html += '</div>';
    }
    
    // Explorer area
    html += '<div id="' + explorerId + '"></div>';
    
    // Status bar
    html += '<div id="' + statusId + '" style="padding:6px 8px;background:var(--crumb-bg);border-top:1px solid var(--border);font-size:0.85em;color:var(--muted);min-height:24px;"></div>';
    html += '</div>';
    
    container.innerHTML = html;
    
    var explorerDiv = document.getElementById(explorerId);
    var statusDiv = document.getElementById(statusId);
    
    function setStatus(msg, isError) {
      statusDiv.textContent = msg;
      statusDiv.style.color = isError ? 'var(--danger)' : 'var(--muted)';
    }
    
    function loadExplorer() {
      // Create embedded explorer
      window.createFileExplorer({
        containerId: explorerId,
        path: currentPath,
        height: managerHeight,
        mode: mode,
        onSelect: function(filePath) {
          window[managerId + 'ViewFile'](filePath);
        },
        onNavigate: function(path) {
          // Update manager's current path when explorer navigates
          currentPath = path;
          setStatus('Path: ' + currentPath);
        }
      });
      setStatus('Path: ' + currentPath);
    }
    
    // Action: Create folder
    window[managerId + 'CreateFolder'] = function() {
      var name = prompt('Enter folder name:');
      if (!name) return;
      
      var fullPath = currentPath === '/' ? '/' + name : currentPath + '/' + name;
      setStatus('Creating folder...', false);
      
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'cmd=' + encodeURIComponent('mkdir ' + fullPath)
      })
      .then(r => r.text())
      .then(txt => {
        setStatus(txt, txt.indexOf('Error') >= 0);
        loadExplorer();
        if (config.onRefresh) config.onRefresh();
      })
      .catch(e => setStatus('Error: ' + e.message, true));
    };
    
    // Action: Create file
    window[managerId + 'CreateFile'] = function() {
      var name = prompt('Enter file name (with extension):');
      if (!name) return;
      
      var fullPath = currentPath === '/' ? '/' + name : currentPath + '/' + name;
      setStatus('Creating file...', false);
      
      fetch('/api/cli', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'cmd=' + encodeURIComponent('filecreate ' + fullPath)
      })
      .then(r => r.text())
      .then(txt => {
        setStatus(txt, txt.indexOf('Error') >= 0);
        loadExplorer();
        if (config.onRefresh) config.onRefresh();
      })
      .catch(e => setStatus('Error: ' + e.message, true));
    };
    
    // Action: Upload file
    window[managerId + 'UploadFile'] = function() {
      var input = document.getElementById(managerId + '_upload_input');
      input.onchange = function(e) {
        var file = e.target.files[0];
        if (!file) return;
        
        if (file.size > 3 * 1024 * 1024) {
          setStatus('Error: File too large (max 3MB)', true);
          input.value = '';
          return;
        }
        
        setStatus('Uploading ' + file.name + '...', false);
        var targetPath = currentPath === '/' ? '/' + file.name : currentPath + '/' + file.name;
        // Text files that can be safely read as text; everything else is binary
        var isText = /\.(txt|json|csv|xml|html|htm|css|js|md|log|ini|cfg|conf|yaml|yml|sh|py|c|cpp|h|hpp)$/i.test(file.name);
        var isBinary = !isText;
        
        var reader = new FileReader();
        reader.onload = function(evt) {
          var content = evt.target.result;
          
          if (isBinary) {
            content = content.split(',')[1];  // Strip data URL prefix for base64
          }
          
          fetch('/api/files/upload', {
            method: 'POST',
            headers: {'Content-Type': 'application/x-www-form-urlencoded'},
            body: 'path=' + encodeURIComponent(targetPath) + '&binary=' + (isBinary ? '1' : '0') + '&content=' + encodeURIComponent(content)
          })
          .then(r => r.json())
          .then(j => {
            if (j.success) {
              setStatus('Uploaded: ' + file.name, false);
              loadExplorer();
              if (config.onRefresh) config.onRefresh();
            } else {
              setStatus('Upload failed: ' + (j.error || 'Unknown'), true);
            }
            input.value = '';
          })
          .catch(e => {
            setStatus('Upload error: ' + e.message, true);
            input.value = '';
          });
        };
        
        if (isBinary) {
          reader.readAsDataURL(file);  // Base64 preserves binary data
        } else {
          reader.readAsText(file);     // Text files only
        }
      };
      input.click();
    };
    
    // Action: Refresh
    window[managerId + 'Refresh'] = function() {
      loadExplorer();
      if (config.onRefresh) config.onRefresh();
    };
    
    // Action: View file
    window[managerId + 'ViewFile'] = function(filePath) {
      window.open('/api/files/view?name=' + encodeURIComponent(filePath), '_blank');
    };
    
    // Initial load
    loadExplorer();
    
    return {
      refresh: function() {
        window[managerId + 'Refresh']();
      },
      navigate: function(path) {
        currentPath = path;
        loadExplorer();
      },
      getCurrentPath: function() {
        return currentPath;
      }
    };
  };
  
  console.log('[FileExplorer] Utility loaded');
})();
</script>
)FBSCRIPT";
}

// ============================================================================
// HTTP Streaming Helpers
// ============================================================================

// Stream a null-terminated C string as a chunk
esp_err_t streamChunkC(httpd_req_t* req, const char* s);

// Stream a buffer with explicit length as a chunk
esp_err_t streamChunkBuf(httpd_req_t* req, const char* buf, size_t len);

// Stream a String as a chunk
void streamChunk(httpd_req_t* req, const String& str);

// Stream a C string as a chunk
void streamChunk(httpd_req_t* req, const char* str);

// Begin HTML page with standard structure (doctype, head, nav, content wrapper)
void streamBeginHtml(httpd_req_t* req, const char* title, bool isPublic,
                     const String& username, const String& activePage);

// End HTML page (close content div, body, html; finalize chunked response)
void streamEndHtml(httpd_req_t* req);

// Stream navigation bar
void streamNav(httpd_req_t* req, const String& username, const String& activePage);

// Stream generic content with chunking (for large content)
void streamContentGeneric(httpd_req_t* req, const String& content);

// ============================================================================
// CSS Streaming
// ============================================================================

// Stream common CSS styles directly to response (no String allocation)
inline void streamCommonCSS(httpd_req_t* req) {
  if (!req) return;
  
  // Stream CSS in chunks to avoid large String allocation
  httpd_resp_send_chunk(req,
    ":root{"
    "--bg:linear-gradient(135deg,#667eea 0%,#764ba2 100%);"
    "--fg:#fff;"
    "--card-bg:rgba(255,255,255,.10);"
    "--card-border:rgba(255,255,255,.20);"
    "--menu-bg:rgba(0,0,0,.20);"
    "--menu-item-bg:rgba(255,255,255,.80);"
    "--menu-item-fg:#333;"
    "--panel-bg:rgba(255,255,255,.10);"
    "--panel-fg:#fff;"
    "--border:rgba(255,255,255,.22);"
    "--crumb-bg:rgba(255,255,255,.12);"
    "--link:#bcd0ff;"
    "--muted:rgba(255,255,255,.75);"
    "--icon-bg:rgba(0,0,0,.55);"
    "--danger:#dc3545;"
    "--danger-hover:#c82333;"
    "}"
    "html[data-theme=light]{"
    "--bg:linear-gradient(135deg,#667eea 0%,#764ba2 100%);"
    "--fg:#fff;"
    "--card-bg:rgba(255,255,255,.10);"
    "--card-border:rgba(255,255,255,.20);"
    "--menu-bg:rgba(0,0,0,.20);"
    "--menu-item-bg:rgba(255,255,255,.80);"
    "--menu-item-fg:#333;"
    "--panel-bg:rgba(255,255,255,.10);"
    "--panel-fg:#fff;"
    "--border:rgba(255,255,255,.22);"
    "--crumb-bg:rgba(255,255,255,.12);"
    "--link:#bcd0ff;"
    "--muted:rgba(255,255,255,.75);"
    "--icon-bg:rgba(0,0,0,.55);"
    "--danger:#dc3545;"
    "--danger-hover:#c82333;"
    "--success:#28a745;"
    "--success-hover:#218838;"
    "--warning-bg:#fff3cd;"
    "--warning-fg:#856404;"
    "--warning-border:#ffeeba;"
    "--warning-accent:#ffc107;"
    "--info-bg:#d1ecf1;"
    "--info-fg:#0c5460;"
    "--info-border:#bee5eb;"
    "--info-accent:#17a2b8;"
    "}"
    "html[data-theme=dark]{"
    "--bg:linear-gradient(135deg,#07070b 0%,#151520 100%);"
    "--fg:#f2f2f7;"
    "--card-bg:rgba(255,255,255,.04);"
    "--card-border:rgba(255,255,255,.12);"
    "--menu-bg:rgba(0,0,0,.55);"
    "--menu-item-bg:rgba(30,30,40,.92);"
    "--menu-item-fg:#f2f2f7;"
    "--panel-bg:rgba(18,18,26,.92);"
    "--panel-fg:#f2f2f7;"
    "--border:rgba(255,255,255,.14);"
    "--crumb-bg:rgba(30,30,40,.75);"
    "--link:#8ab4ff;"
    "--muted:rgba(242,242,247,.72);"
    "--icon-bg:rgba(255,255,255,.10);"
    "--danger:#ff5a6a;"
    "--danger-hover:#ff3b4e;"
    "--success:#4ade80;"
    "--success-hover:#22c55e;"
    "--warning-bg:rgba(118,75,162,.15);"
    "--warning-fg:#a78bfa;"
    "--warning-border:rgba(118,75,162,.3);"
    "--warning-accent:#8b5cf6;"
    "--info-bg:rgba(118,75,162,.15);"
    "--info-fg:#a78bfa;"
    "--info-border:rgba(56,189,248,.3);"
    "--info-accent:#0ea5e9;"
    "}"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{font-family:'Segoe UI',Tahoma,Geneva,Verdana,sans-serif;"
    "background:var(--bg);"
    "min-height:100vh;color:var(--fg);line-height:1.6}"
    ".content{padding:1rem;max-width:1200px;margin:0 auto}"
    ".card{background:var(--card-bg);backdrop-filter:blur(10px);"
    "border-radius:15px;padding:2rem;margin:1rem 0;border:1px solid var(--card-border);"
    "box-shadow:0 8px 32px rgba(0,0,0,.1)}"
    ".top-menu{background:var(--menu-bg);padding:1rem;display:flex;"
    "justify-content:space-between;align-items:center;flex-wrap:wrap}"
    ".menu-left{display:flex;gap:1rem;flex-wrap:wrap}"
    ".menu-item,button.menu-item{color:var(--menu-item-fg);text-decoration:none;font-weight:500;padding:8px 16px;border-radius:8px;"
    "transition:all .3s;border:1px solid var(--border);background:var(--menu-item-bg);"
    "box-shadow:0 2px 4px rgba(0,0,0,.1);display:inline-block;font-size:1rem;line-height:1.2}"
    "button.menu-item{cursor:pointer}"
    ".menu-item:hover,button.menu-item:hover{color:#222;background:rgba(255,255,255,.9);border-color:rgba(0,0,0,.3);"
    "transform:translateY(-1px);box-shadow:0 4px 8px rgba(0,0,0,.15)}"
    ".menu-item.active{color:#fff;background:rgba(255,255,255,.2);border-color:rgba(255,255,255,.4);font-weight:600}"
    ".user-info{display:flex;align-items:center;gap:1rem;flex-wrap:wrap}"
    ".username{font-weight:bold;color:var(--fg)}"
    ".login-btn{background:rgba(255,255,255,.85);color:#0f5132;text-decoration:none;"
    "padding:.4rem .8rem;border-radius:8px;font-size:.85rem;transition:all .3s ease;"
    "border:1px solid rgba(25,135,84,.4);box-shadow:0 2px 4px rgba(0,0,0,.1)}"
    ".login-btn:hover{background:rgba(255,255,255,.95);border-color:rgba(25,135,84,.6);"
    "transform:translateY(-1px);box-shadow:0 4px 8px rgba(0,0,0,.15)}"
    ".logout-btn{background:rgba(255,255,255,.85);color:#b02a37;text-decoration:none;"
    "padding:.4rem .8rem;border-radius:8px;font-size:.85rem;transition:all .3s ease;"
    "border:1px solid rgba(176,42,55,.4);box-shadow:0 2px 4px rgba(0,0,0,.1)}"
    ".logout-btn:hover{background:rgba(255,255,255,.95);border-color:rgba(176,42,55,.6);"
    "transform:translateY(-1px);box-shadow:0 4px 8px rgba(0,0,0,.15)}"
    "h1,h2,h3{margin-bottom:1rem;color:var(--fg)}"
    "p{margin-bottom:.5rem}"
    "a{color:var(--link);text-decoration:none}"
    "a:hover{text-decoration:underline}"
    "input,select,textarea{width:100%;padding:.5rem;border:1px solid #ddd;"
    "border-radius:6px;margin-bottom:.5rem;background:var(--panel-bg);color:var(--panel-fg)}"
    "body.public input,body.public select,body.public textarea{background:#fff;color:#000;border:1px solid rgba(0,0,0,.25);box-shadow:none}"
    "body.public input:focus,body.public select:focus,body.public textarea:focus{outline:none;border-color:rgba(0,0,0,.45)}"
    "body.public ::placeholder{color:rgba(0,0,0,.55)}", HTTPD_RESP_USE_STRLEN);
  
  httpd_resp_send_chunk(req,
    ".input-tall{min-height:40px;padding:.5rem .6rem}"
    "button:not(.menu-item):not(.btn){background:#007bff;color:#fff;border:none;padding:.5rem 1rem;"
    "border-radius:4px;cursor:pointer}"
    "button:not(.menu-item):not(.btn):hover{background:#0056b3}"
    "table{width:100%;border-collapse:collapse;margin:1rem 0}"
    "th,td{padding:.5rem;text-align:left;border-bottom:1px solid rgba(255,255,255,.1)}"
    "th{background:rgba(255,255,255,.1);font-weight:bold}"
    "@media(max-width:768px){"
    ".top-menu{flex-direction:column;gap:1rem}"
    ".menu-left{justify-content:center}"
    ".user-info{justify-content:center}"
    ".content{padding:.5rem}"
    ".card{padding:1rem}"
    "}"
    ".text-center{text-align:center}"
    ".text-muted{color:var(--muted)}"
    ".text-danger{color:var(--danger)}"
    ".text-primary{color:#0d6efd}"
    ".text-sm{font-size:.9rem}"
    ".link-primary{color:#0d6efd}"
    ".vis-hidden{visibility:hidden!important}"
    ".vis-gone{display:none!important}", HTTPD_RESP_USE_STRLEN);
  
  httpd_resp_send_chunk(req,
    ".space-top-sm{margin-top:8px}"
    ".space-top-md{margin-top:16px}"
    ".space-top-lg{margin-top:24px}"
    ".space-bottom-sm{margin-bottom:8px}"
    ".space-bottom-md{margin-bottom:16px}"
    ".space-bottom-lg{margin-bottom:24px}"
    ".space-left-sm{margin-left:8px}"
    ".space-left-md{margin-left:16px}"
    ".space-left-lg{margin-left:24px}"
    ".space-right-sm{margin-right:8px}"
    ".space-right-md{margin-right:16px}"
    ".space-right-lg{margin-right:24px}"
    ".panel{background:var(--panel-bg);color:var(--panel-fg);border-radius:12px;padding:1.25rem;"
    "box-shadow:0 6px 20px rgba(0,0,0,.08);border:1px solid var(--border)}"
    ".panel h1,.panel h2,.panel h3{color:var(--panel-fg)}"
    ".panel-light{background:var(--panel-bg);color:var(--panel-fg);border-radius:8px;padding:1rem;border:1px solid var(--border)}"
    ".container-narrow{max-width:520px;margin:0 auto}"
    ".pad-xl{padding:2rem}"
    ".form-field{margin-bottom:12px}"
    ".form-field label{display:block;margin-bottom:6px}"
    ".form-input{width:100%;padding:.6rem;border:1px solid var(--border);border-radius:6px;background:var(--panel-bg);color:var(--panel-fg)}"
    ".form-error{margin-bottom:.5rem}"
    ".input-medium{width:260px}"
    ".settings-panel{background:var(--panel-bg);border-radius:8px;padding:1rem 1.5rem;margin:1rem 0;color:var(--panel-fg);border:1px solid var(--border)}"
    ".settings-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:1rem}"
    ".alert{padding:12px;border-radius:8px;margin-bottom:15px;border:1px solid}"
    ".alert-warning{background:var(--warning-bg);color:var(--warning-fg);border-color:var(--warning-border);border-left:4px solid var(--warning-accent)}"
    ".alert-info{background:var(--info-bg);color:var(--info-fg);border-color:var(--info-border);border-left:4px solid var(--info-accent)}"
    ".status-dot{width:12px;height:12px;border-radius:50%;display:inline-block}"
    ".status-inactive{background:var(--muted)}"
    ".status-active{background:var(--success)}", HTTPD_RESP_USE_STRLEN);
  
  httpd_resp_send_chunk(req,
    ".btn{display:inline-flex;align-items:center;justify-content:center;min-height:40px;"
    "padding:.5rem 1rem;border-radius:8px;border:1px solid var(--border);"
    "background:var(--menu-item-bg);color:var(--menu-item-fg);text-decoration:none;cursor:pointer;transition:all .2s;"
    "font-size:1rem;line-height:1.2;font-weight:500;box-sizing:border-box}"
    "button.btn,a.btn{display:inline-flex;align-items:center;justify-content:center;min-height:40px;"
    "font-size:1rem;line-height:1.2;font-weight:500}"
    ".btn:hover{transform:translateY(-1px);box-shadow:0 2px 6px rgba(0,0,0,.12);background:var(--crumb-bg)}"
    ".btn-primary,.btn-secondary{ }"
    ".btn-small{padding:.25rem .5rem;border-radius:6px}"
    ".btn-row{display:flex;gap:.5rem;align-items:center;flex-wrap:wrap}"
    ".modal-overlay{display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.5);z-index:1000}"
    ".modal-dialog{position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);background:var(--panel-bg);color:var(--panel-fg);padding:1.25rem;border-radius:8px;min-width:320px;border:1px solid var(--border)}"
    ".table{width:100%;border-collapse:collapse}"
    ".table th,.table td{padding:.5rem;text-align:left;border-bottom:1px solid var(--border);color:var(--panel-fg)}"
    ".table-striped tr:nth-child(odd){background:rgba(255,255,255,.05)}", HTTPD_RESP_USE_STRLEN);
}

#endif // ENABLE_HTTP_SERVER
#endif // WEBSERVER_UTILS_H
