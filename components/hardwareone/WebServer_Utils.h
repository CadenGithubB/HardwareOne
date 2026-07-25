// WebServer_Utils.h - Web server utility functions, helpers, and shared HTML/JS
// Merged from WebCore_Utils.h and WebCore_Shared.h

#ifndef WEBSERVER_UTILS_H
#define WEBSERVER_UTILS_H

#include "System_BuildConfig.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ============================================================================
// Web Mirror Buffer - CLI output buffer for web interface
// (always available for type-safe references)
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
  void appendDirect(const char* s, size_t slen, bool needNewline);
  String snapshot();
  size_t snapshotTo(char* dest, size_t destSize);
  void assignFrom(const String& s);
};

extern WebMirrorBuf gWebMirror;
extern size_t gWebMirrorCap;

#if ENABLE_HTTP_SERVER

#include <esp_http_server.h>

// CACHE_MUTEX_TIMEOUT_MS lives in System_Mutex.h (single canonical home).
// Callers that need it should include System_Mutex.h directly or via System_I2C.h.

// ============================================================================
// HTTP Response Helpers  (inline — avoids an extra function call)
// ============================================================================

// Send a complete JSON response in one call.  Equivalent to the common pair:
//   httpd_resp_set_type(req, "application/json");
//   httpd_resp_send(req, body, len);
// Saves two lines at every JSON endpoint.
inline esp_err_t sendJsonResponse(httpd_req_t* req, const char* body, ssize_t len = HTTPD_RESP_USE_STRLEN) {
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body, len);
}

// Guest role HTTP gate. After auth succeeds: guests may only hit an allowlist
// of status/list GET APIs, HTML view pages, and logout. Everything else
// (CLI, files, mutate POSTs, camera capture, admin, …) gets 403.
// Non-guests always pass. Returns false after sending the 403 response.
// Forward-declared here (defined in System_User.h) so this header parses
// without pulling in the full user subsystem; the reference param only needs
// the incomplete type. Callers that expand WEB_AUTH_OR_RETURN already have
// the complete type in scope.
struct AuthContext;
bool webGuestAccessAllowed(httpd_req_t* req, const AuthContext& ctx);

// Authenticate the request and return ESP_OK (unauthenticated) if auth fails.
// Usage:  WEB_AUTH_OR_RETURN(req, ctx);
//         ... use ctx.user, ctx.ip etc below ...
// The ctx variable name is a macro parameter so callers can choose it.
#define WEB_AUTH_OR_RETURN(req, ctx) \
  AuthContext ctx = makeWebAuthCtx(req); \
  if (!tgRequireAuth(ctx)) return ESP_OK; \
  if (!webGuestAccessAllowed(req, ctx)) return ESP_OK

// JSON-response variant: auth + set Content-Type: application/json in one
// step. Equivalent to WEB_AUTH_OR_RETURN followed by httpd_resp_set_type,
// which is the prologue ~30 JSON-returning handlers share. Use this when
// the handler emits JSON (which is most /api/* endpoints).
#define WEB_AUTH_JSON_OR_RETURN(req, ctx) \
  AuthContext ctx = makeWebAuthCtx(req); \
  if (!tgRequireAuth(ctx)) return ESP_OK; \
  if (!webGuestAccessAllowed(req, ctx)) return ESP_OK; \
  httpd_resp_set_type(req, "application/json")

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
String generateNavigation(const String& activePage, const String& username, const char* initialTheme = "light");

// Generate navigation bar for public (unauthenticated) pages
String generatePublicNavigation();

// ============================================================================
// Shared HTML/JS Utilities (inline for header-only usage)
// ============================================================================

// Forward declarations
void streamCommonCSS(httpd_req_t* req);     // Streaming CSS - no String allocation
void streamCommonDialogs(httpd_req_t* req); // Streaming dialog HTML+JS - no String allocation

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
  // ==========================================================================
  // window.FileBrowser — shared rendering helpers used by BOTH the local file
  // explorer (window.createFileExplorer below) and the bonded-device file
  // browser (window.BondFs.renderExplorer further down). Keep all icon-mapping,
  // sizing, and per-file rendering helpers here so the two views stay in
  // lockstep — adding a new file extension or tweaking icon styling here
  // updates both renderers automatically.
  // ==========================================================================
  var iconLoadFailed = {};

  window.FileBrowser = {
    // Map a filename extension (or folder flag) to the icon name served by
    // /api/icon. Unknown extensions fall back to the generic 'file' icon.
    iconName: function(filename, isDir) {
      if (isDir) return 'folder';
      var ext = (filename || '').toLowerCase().split('.').pop();
      var map = {
        // code
        'js':'file_code','ts':'file_code','jsx':'file_code','tsx':'file_code',
        'cpp':'file_code','h':'file_code','hpp':'file_code','c':'file_code','ino':'file_code',
        'py':'file_code','sh':'file_code',
        // web documents
        'html':'file_code','htm':'file_code','css':'file_code',
        // structured data
        'json':'file_json',
        // text
        'txt':'file_text','log':'file_text','md':'file_text',
        // images
        'jpg':'file_image','jpeg':'file_image','png':'file_image','gif':'file_image',
        'bmp':'file_image','svg':'file_image','ico':'file_image',
        // documents
        'pdf':'file_pdf',
        // archives
        'zip':'file_zip','gz':'file_zip','tar':'file_zip','7z':'file_zip',
        // binaries
        'bin':'file_bin','dat':'file_bin'
      };
      return map[ext] || 'file';
    },

    // Fallback text used when /api/icon can't serve the image.
    iconFallback: function(isDir) {
      return isDir ? '[DIR]' : '[FILE]';
    },

    // 48×48 themed file/folder icon. iconLoadFailed caches per-name 404s so we
    // don't keep refetching missing icons within the same page session.
    renderIcon: function(iconName, fallbackText) {
      function dbgIcons(){try{return !!(window.localStorage&&window.localStorage.getItem('hwDebugIcons')==='1')}catch(_){return false}}
      function logIcons(){try{if(dbgIcons())console.log.apply(console,arguments)}catch(_){}}
      if (iconLoadFailed[iconName]) {
        logIcons('[icons] cached-fail icon=', iconName, 'fallback=', fallbackText);
        return '<span style="display:inline-block;width:32px;font-family:monospace;color:var(--muted);font-size:0.85em;text-align:center;">' + fallbackText + '</span>';
      }
      var imgId = 'icon_' + iconName + '_' + Math.random().toString(36).substr(2, 9);
      var iconUrl = '/api/icon?name=' + iconName;
      logIcons('[icons] render icon=', iconName, 'url=', iconUrl);
      var html = '<img id="' + imgId + '" src="' + iconUrl + '" width="48" height="48" style="vertical-align:middle;image-rendering:auto;display:inline-block;background:var(--icon-bg);border-radius:6px;padding:4px;box-sizing:border-box;filter:var(--icon-filter);" ';
      html += 'onerror="this.style.display=\'none\';this.nextSibling.style.display=\'inline-block\';" />';
      html += '<span style="display:none;width:48px;font-family:monospace;color:var(--muted);font-size:0.85em;text-align:center;">' + fallbackText + '</span>';
      if (dbgIcons()) {
        setTimeout(function(){
          try {
            var img = document.getElementById(imgId);
            if (!img) { console.warn('[icons] element not found id=', imgId, 'icon=', iconName); return; }
            img.addEventListener('load', function(){ console.log('[icons] load ok', iconName, 'id', imgId); });
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
    },

    // 20×20 action-button icon (Download / Edit / Delete buttons).
    renderActionIcon: function(iconName, fallbackText) {
      return '<img src="/api/icon?name=' + iconName + '" width="20" height="20" style="vertical-align:middle;filter:var(--icon-filter);" onerror="this.style.display=\'none\';this.nextSibling.style.display=\'inline\'"><span style="display:none;font-size:0.8em;">' + fallbackText + '</span>';
    },

    // Convert a raw size string ("107 bytes") into a friendlier form
    // ("107 B" / "1.50 KB" / "2.30 MB"). Folder meta strings ("10 items") and
    // already-formatted sizes pass through unchanged.
    formatSize: function(meta, isDir) {
      var s = String(meta || '');
      if (isDir) return s;
      // Only reformat when the input is clearly a raw byte count — either the
      // "<N> bytes" form the local API returns, or a bare integer.
      if (s.indexOf('bytes') < 0 && !/^\d+$/.test(s)) return s;
      var m = s.match(/(\d+)/);
      if (!m) return s;
      var bytes = parseInt(m[1], 10);
      if (bytes >= 1048576) return (bytes / 1048576).toFixed(2) + ' MB';
      if (bytes >= 1024) return (bytes / 1024).toFixed(2) + ' KB';
      return bytes + ' B';
    },

    // Build the breadcrumb inner HTML ("[Root] / system / users") for either
    // renderer. The caller decides which container wraps it. `navigateExpr`
    // is a function(targetPath) → JS expression string (e.g. for the local
    // explorer that's `fnId + "Navigate('" + p + "')"`; for BondFs it's
    // `"window['" + navName + "']('" + p + "')"`). opts.lockToPath, when set,
    // pins the displayed root to a sub-folder and disables navigation above it
    // (used by file-pickers that lock to /system/automations etc.).
    breadcrumbHtml: function(currentPath, navigateExpr, opts) {
      opts = opts || {};
      var parts = String(currentPath || '/').split('/').filter(function(p){ return p.length > 0; });
      var html = '';
      if (opts.lockToPath) {
        var lockedParts = opts.lockToPath.split('/').filter(function(p){ return p.length > 0; });
        var displayRoot = lockedParts.length > 0 ? lockedParts[lockedParts.length - 1] : 'Root';
        html = '<span style="color:var(--muted);">[' + displayRoot + ']</span>';
      } else {
        html = '<span style="cursor:pointer;color:var(--link);" onclick="' + navigateExpr('/') + '">[Root]</span>';
      }
      var path = '';
      var skipCount = opts.lockToPath
        ? opts.lockToPath.split('/').filter(function(p){ return p.length > 0; }).length
        : 0;
      parts.forEach(function(part, idx){
        if (idx < skipCount) return;
        path += '/' + part;
        html += ' <span style="color:var(--muted);">/</span> ';
        html += '<span style="cursor:pointer;color:var(--link);" onclick="' + navigateExpr(path) + '">' + part + '</span>';
      });
      return html;
    },

    // Build one full file/folder row. Both renderers emitted the same outer
    // <div> + name span + size span + optional action-buttons span — this
    // collapses ~15 lines of duplicated layout into a single call. The caller
    // pre-builds `actionsHtml` because the action set is renderer-specific
    // (local uses perms + mode; bond uses a fileActions[] array).
    //
    // Required: name, isDir, sizeInfo, iconName, iconFallback
    // Optional: clickExpr (empty = non-clickable name), hoverable (default true),
    //           actionsHtml (default '')
    rowHtml: function(o) {
      var hoverable = (o.hoverable !== false);
      var cursor = o.clickExpr ? 'pointer' : 'default';
      var bg = 'var(--panel-bg)';
      var hover = hoverable
        ? ' onmouseover="this.style.background=\'var(--crumb-bg)\'" onmouseout="this.style.background=\'' + bg + '\'"'
        : '';
      var h = '';
      h += '<div style="padding:8px 12px;border-bottom:1px solid var(--border);display:flex;justify-content:space-between;align-items:center;background:' + bg + ';"' + hover + '>';
      h += '<span style="flex:1;color:var(--panel-fg);font-size:0.95em;cursor:' + cursor + ';display:flex;align-items:center;gap:8px;"';
      if (o.clickExpr) h += ' onclick="' + o.clickExpr + '"';
      h += '>' + this.renderIcon(o.iconName, o.iconFallback) + '<span>' + o.name + '</span></span>';
      h += '<span style="color:var(--muted);font-size:0.85em;margin-left:12px;min-width:80px;text-align:right;">' + (o.sizeInfo || '') + '</span>';
      if (o.actionsHtml) h += o.actionsHtml;
      h += '</div>';
      return h;
    }
  };

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
    //   lockToPath: string - optional path to lock navigation to (prevents browsing outside this directory)
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
    var lockToPath = config.lockToPath || null;  // Lock navigation to this path
    
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
      breadcrumbDiv.innerHTML = window.FileBrowser.breadcrumbHtml(currentPath,
        function(p) { return explorerFnId + "Navigate('" + p + "')"; },
        { lockToPath: lockToPath });
    }
    
    // Icon-name lookup, theme-aware <img> rendering, and size formatting all
    // live in window.FileBrowser so the bonded-device file browser uses the
    // exact same helpers as this view. See definition at the top of the IIFE.

    function loadDirectory(path) {
      listDiv.innerHTML = '<div style="padding:20px;text-align:center;color:var(--muted);">Loading...</div>';
      
      fetch('/api/files/list?path=' + encodeURIComponent(path))
        .then(function(r) {
          return r.json().then(function(data) {
            data.__httpStatus = r.status;
            return data;
          });
        })
        .then(function(data) {
          if (!data.success || !data.files) {
            var msg = 'Error loading directory';
            var color = 'var(--danger)';
            var errStr = ('' + (data.error || '')).toLowerCase();
            if (errStr.indexOf('guest') >= 0) {
              // Guest gate (webGuestSendForbidden -> error:"guest_forbidden"):
              // the whole files surface is off-limits to view-only guests, not
              // an admin-vs-user thing — say so instead of "Admin required".
              msg = 'File browsing is not available in a view-only guest session';
              color = 'var(--muted)';
            } else if ((data.__httpStatus === 403) || errStr.indexOf('admin') >= 0) {
              msg = 'Admin access required to view this directory';
              color = 'var(--muted)';
            } else if (data.error) {
              msg = '' + data.error;
            }
            listDiv.innerHTML = '<div style="padding:20px;text-align:center;color:' + color + ';">' + msg + '</div>';
            if (config.onNavigate) config.onNavigate(currentPath, 0);
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
            if (config.onNavigate) config.onNavigate(currentPath, data.dirPerms || 0);
            return;
          }
          
          var html = '<div style="padding:4px;">';
          files.forEach(function(file) {
            var isFolder = file.type === 'folder';
            var itemPath = (currentPath === '/' ? '/' : currentPath + '/') + file.name;
            var sizeInfo = window.FileBrowser.formatSize(file.size || '', isFolder);
            
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
            
            // Action buttons (only in full mode, respecting per-file
            // permissions from API). Built here as a pre-rendered string so
            // FileBrowser.rowHtml can drop it in without knowing the rules.
            var actionsHtml = '';
            if (mode === 'full') {
              var perms = file.perms || 0;
              var hasRead   = (perms & 0x01) !== 0;
              var hasWrite  = (perms & 0x02) !== 0;
              var hasDelete = (perms & 0x04) !== 0;
              var hasRename = (perms & 0x08) !== 0;
              var btns = '';
              if (!isFolder && hasRead) {
                btns += '<button class="btn btn-small" onclick="' + explorerFnId + 'Download(\'' + itemPath + '\');event.stopPropagation();" style="padding:4px 6px;" title="Download file">' + window.FileBrowser.renderActionIcon('download', 'DL') + '</button>';
              }
              if (!isFolder && config.onEdit && hasWrite) {
                btns += '<button class="btn btn-small" onclick="' + explorerFnId + 'Edit(\'' + itemPath + '\');event.stopPropagation();" style="padding:4px 6px;" title="Edit file">' + window.FileBrowser.renderActionIcon('edit', 'Edit') + '</button>';
              }
              if (hasRename) {
                btns += '<button class="btn btn-small" onclick="' + explorerFnId + 'Rename(\'' + itemPath + '\');event.stopPropagation();" style="padding:4px 8px;font-size:0.8em;" title="Rename">Rename</button>';
              }
              if (hasDelete) {
                btns += '<button class="btn btn-small" onclick="' + explorerFnId + 'Delete(\'' + itemPath + '\',' + (isFolder ? 'true' : 'false') + ');event.stopPropagation();" style="padding:4px 6px;" title="Delete">' + window.FileBrowser.renderActionIcon('trash', 'Del') + '</button>';
              }
              if (btns) actionsHtml = '<span style="display:inline-flex;gap:2px;margin-left:8px;align-items:center;">' + btns + '</span>';
            }

            html += window.FileBrowser.rowHtml({
              name: file.name,
              isDir: isFolder,
              sizeInfo: sizeInfo,
              iconName: window.FileBrowser.iconName(file.name, isFolder),
              iconFallback: window.FileBrowser.iconFallback(isFolder),
              clickExpr: canInteract ? clickAction : '',
              hoverable: canInteract,
              actionsHtml: actionsHtml
            });
          });
          html += '</div>';
          
          listDiv.innerHTML = html;
          
          // Notify parent with directory permissions from API
          if (config.onNavigate && typeof config.onNavigate === 'function') {
            config.onNavigate(currentPath, data.dirPerms || 0);
          }
        })
        .catch(function(e) {
          console.error('[FileExplorer] Failed to load directory:', e);
          listDiv.innerHTML = '<div style="padding:20px;text-align:center;color:var(--danger);">Error: ' + e.message + '</div>';
        });
    }
    
    // Global navigation function (needs to be accessible from onclick)
    window[explorerFnId + 'Navigate'] = function(path) {
      // If locked to a path, prevent navigation outside that path
      if (lockToPath && !path.startsWith(lockToPath)) {
        console.warn('[FileExplorer] Navigation blocked: locked to ' + lockToPath);
        return;
      }
      currentPath = path;
      renderBreadcrumb();
      loadDirectory(path);
      // onNavigate is called from within loadDirectory after dirPerms are available
    };
    
    // Global select function
    window[explorerFnId + 'Select'] = function(filePath) {
      if (config.onSelect && typeof config.onSelect === 'function') {
        config.onSelect(filePath);
      }
    };
    
    // Global delete function.
    // Folders use rmdir which is one-shot (single /api/cli call). Files use
    // filedelete which is two-step (Phase 3+4+5 CLIMode confirm framework):
    // hw.cliConfirm gates with the themed dialog, then sends
    // [filedelete /path, yes] atomically via /api/cli/batch so the worker's
    // confirm-mode state resolves on the same web session. Without the
    // paired 'yes' the file would NOT actually delete (the first call only
    // arms the prompt). See System_Filesystem.cpp:596 for the CLI contract.
    window[explorerFnId + 'Delete'] = function(filePath, isFolder) {
      var itemType = isFolder ? 'folder' : 'file';
      var confirmMsg = 'Delete ' + itemType + ' "' + filePath + '"?';
      if (isFolder) {
        confirmMsg += '\n\nNote: Folder must be empty to delete.';
      }

      if (isFolder) {
        if (!confirm(confirmMsg)) return;
        hw.postFormText('/api/cli', { cmd: 'rmdir "' + filePath + '"' })
        .then(function(txt) {
          if (txt.indexOf('Error') >= 0 || txt.indexOf('Failed') >= 0) {
            alert(txt);  /* txt already leads with its own status word */
          } else {
            loadDirectory(currentPath);
          }
        })
        .catch(function(e) {
          alert('Delete error: ' + e.message);
        });
        return;
      }

      // File path: two-step confirm flow.
      hw.cliConfirm('filedelete "' + filePath + '"', confirmMsg).then(function(r) {
        if (r.cancelled) return;
        if (!r.ok) { alert(r.result || 'Error: no response'); return; }
        loadDirectory(currentPath);
      }).catch(function(e) {
        alert('Delete error: ' + e.message);
      });
    };
    
    // Global edit function (calls onEdit callback)
    window[explorerFnId + 'Edit'] = function(filePath) {
      if (config.onEdit && typeof config.onEdit === 'function') {
        config.onEdit(filePath);
      }
    };
    
    // Global download function
    window[explorerFnId + 'Download'] = function(filePath) {
      var filename = filePath.split('/').pop() || 'download';
      var a = document.createElement('a');
      a.href = '/api/files/read?name=' + encodeURIComponent(filePath);
      a.download = filename;
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
    };
    
    // Global rename function
    window[explorerFnId + 'Rename'] = async function(filePath) {
      var oldName = filePath.split('/').pop();
      var newName = await hwPrompt('Rename "' + oldName + '" to:', oldName);
      if (!newName || newName === oldName) return;
      
      hw.postFormText('/api/cli', { cmd: 'filerename "' + filePath + '" "' + newName + '"' })
      .then(function(t) {
        if (!t || t.startsWith('Error')) {
          hwAlert('Rename failed: ' + (t || 'Unknown error'));
        } else {
          loadDirectory(currentPath);
        }
      })
      .catch(function(e) {
        hwAlert('Rename error: ' + e.message);
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
  
  // Shared upload utility — reads file, encodes, and POSTs with XHR progress
  // opts: { onProgress(pct, label), onDone(ok, msg) }
  window.hwUploadFile = function(file, targetPath, opts) {
    opts = opts || {};
    var isText = /\.(txt|json|csv|xml|html|htm|css|js|md|log|ini|cfg|conf|yaml|yml|sh|py|c|cpp|h|hpp|crt|pem|key|pub)$/i.test(file.name);
    var isBinary = !isText;
    if (opts.onProgress) opts.onProgress(0, 'Reading ' + file.name + '...');
    var reader = new FileReader();
    reader.onload = function(evt) {
      var content = evt.target.result;
      if (isBinary) content = content.split(',')[1]; // strip data URL prefix
      var body = 'path=' + encodeURIComponent(targetPath) + '&binary=' + (isBinary ? '1' : '0') + '&content=' + encodeURIComponent(content);
      var xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/files/upload?path=' + encodeURIComponent(targetPath), true);
      xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');
      xhr.upload.onprogress = function(e) {
        if (e.lengthComputable && opts.onProgress) {
          var pct = Math.round(e.loaded / e.total * 100);
          opts.onProgress(pct, 'Uploading ' + file.name + '... (' + pct + '%)');
        }
      };
      xhr.onload = function() {
        try {
          var j = JSON.parse(xhr.responseText);
          if (j.success) {
            if (opts.onProgress) opts.onProgress(100, 'Done');
            if (opts.onDone) opts.onDone(true, 'Uploaded: ' + file.name);
          } else {
            if (opts.onDone) opts.onDone(false, 'Upload failed: ' + (j.error || 'Unknown'));
          }
        } catch(e) {
          if (opts.onDone) opts.onDone(false, 'Upload error: bad response');
        }
      };
      xhr.onerror = function() {
        if (opts.onDone) opts.onDone(false, 'Upload error: network failure');
      };
      xhr.send(body);
    };
    reader.onerror = function() {
      if (opts.onDone) opts.onDone(false, 'Error reading file');
    };
    if (isBinary) {
      reader.readAsDataURL(file);
    } else {
      reader.readAsText(file);
    }
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
    var mode = config.mode || 'full';  // 'select', 'view', or 'full'
    
    var managerId = 'fmgr_' + config.containerId.replace(/[^a-zA-Z0-9]/g, '_');
    var toolbarId = managerId + '_toolbar';
    var explorerId = managerId + '_explorer';
    var statusId = managerId + '_status';
    
    // Build UI
    var html = '<div id="' + managerId + '" style="border:1px solid var(--border);border-radius:4px;background:var(--panel-bg);color:var(--panel-fg);overflow:hidden;">';
    
    // Toolbar
    if (showActions) {
      html += '<div id="' + toolbarId + '" style="padding:8px;background:var(--crumb-bg);border-bottom:1px solid var(--border);display:flex;gap:8px;flex-wrap:wrap;">';
      html += '<button id="' + managerId + '_back_btn" class="btn" onclick="' + managerId + 'GoBack()" style="display:none">\u2190 Back</button>';
      html += '<button id="' + managerId + '_folder_btn" class="btn" onclick="' + managerId + 'CreateFolder()">New Folder</button>';
      html += '<button id="' + managerId + '_file_btn" class="btn" onclick="' + managerId + 'CreateFile()">New File</button>';
      html += '<button id="' + managerId + '_upload_btn" class="btn" onclick="' + managerId + 'UploadFile()">Upload</button>';
      html += '<button class="btn" onclick="' + managerId + 'Refresh()">Refresh</button>';
      html += '<input type="file" id="' + managerId + '_upload_input" style="display:none">';
      html += '</div>';
    }

    // Upload progress bar (hidden by default)
    var progressId = managerId + '_progress';
    html += '<div id="' + progressId + '" style="display:none;padding:4px 8px;background:var(--crumb-bg);border-bottom:1px solid var(--border)">';
    html += '<div style="display:flex;align-items:center;gap:8px;font-size:0.82em;color:var(--muted)">';
    html += '<span id="' + progressId + '_label">Uploading...</span>';
    html += '<div style="flex:1;height:6px;background:var(--border);border-radius:3px;overflow:hidden">';
    html += '<div id="' + progressId + '_bar" style="width:0%;height:100%;background:var(--accent, #4dabf7);border-radius:3px;transition:width 0.15s"></div>';
    html += '</div>';
    html += '<span id="' + progressId + '_pct">0%</span>';
    html += '</div></div>';

    // Explorer area
    html += '<div id="' + explorerId + '"></div>';

    // Status bar
    html += '<div id="' + statusId + '" style="padding:6px 8px;background:var(--crumb-bg);border-top:1px solid var(--border);font-size:0.85em;color:var(--muted);min-height:24px;"></div>';
    html += '</div>';
    
    container.innerHTML = html;
    
    var explorerDiv = document.getElementById(explorerId);
    var statusDiv = document.getElementById(statusId);
    
    // Permission flags (must match FilePermission enum in System_Filesystem.h)
    var PERM_CREATE = 0x10;
    var PERM_IMPORT = 0x20;
    
    function updateToolbar(dirPerms) {
      var bb = document.getElementById(managerId + '_back_btn');
      var fb = document.getElementById(managerId + '_folder_btn');
      var fi = document.getElementById(managerId + '_file_btn');
      var ub = document.getElementById(managerId + '_upload_btn');
      if (bb) bb.style.display = (currentPath !== '/') ? '' : 'none';
      if (fb) fb.style.display = (dirPerms & PERM_CREATE) ? '' : 'none';
      if (fi) fi.style.display = (dirPerms & PERM_CREATE) ? '' : 'none';
      if (ub) ub.style.display = (dirPerms & PERM_IMPORT) ? '' : 'none';
    }
    
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
        onEdit: config.onEdit || null,
        onNavigate: function(path, dirPerms) {
          currentPath = path;
          setStatus('Path: ' + currentPath);
          updateToolbar(dirPerms);
          if (config.onRefresh) config.onRefresh(currentPath);
        }
      });
      setStatus('Path: ' + currentPath);
      // Toolbar buttons will be updated by onNavigate callback after directory loads
    }
    
    // Action: Go back to parent directory
    window[managerId + 'GoBack'] = function() {
      if (currentPath === '/') return;
      var parent = currentPath.replace(/\/[^\/]+\/?$/, '') || '/';
      currentPath = parent;
      loadExplorer();
    };

    // Action: Create folder
    window[managerId + 'CreateFolder'] = function() {
      hwPrompt('Enter folder name:').then(function(name) {
        if (!name) return;
        var fullPath = currentPath === '/' ? '/' + name : currentPath + '/' + name;
        setStatus('Creating folder...', false);
        hw.postFormText('/api/cli', { cmd: 'mkdir "' + fullPath + '"' })
        .then(function(txt) {
          setStatus(txt, txt.indexOf('Error') >= 0);
          loadExplorer();
          if (config.onRefresh) config.onRefresh(currentPath);
        })
        .catch(function(e) { setStatus('Error: ' + e.message, true); });
      });
    };

    // Action: Create file
    window[managerId + 'CreateFile'] = function() {
      hwPrompt('Enter file name (with extension):').then(function(name) {
        if (!name) return;
        var fullPath = currentPath === '/' ? '/' + name : currentPath + '/' + name;
        setStatus('Creating file...', false);
        hw.postFormText('/api/cli', { cmd: 'filecreate "' + fullPath + '"' })
        .then(function(txt) {
          setStatus(txt, txt.indexOf('Error') >= 0);
          loadExplorer();
          if (config.onRefresh) config.onRefresh(currentPath);
        })
        .catch(function(e) { setStatus('Error: ' + e.message, true); });
      });
    };
    
    // Action: Upload file
    window[managerId + 'UploadFile'] = function() {
      var input = document.getElementById(managerId + '_upload_input');
      input.onchange = function(e) {
        var file = e.target.files[0];
        if (!file) return;
        
        setStatus('Checking storage...', false);
        hw.fetchJSON('/api/files/stats?path=' + encodeURIComponent(currentPath)).then(function(d) {
          if (!d.success) { setStatus('Upload failed: ' + (d.error || 'Cannot check storage'), true); input.value=''; return; }
          var maxUpload = Math.floor(d.free * 0.9);
          if (file.size > maxUpload) {
            var freeMB = (d.free / 1024 / 1024).toFixed(1);
            var fileMB = (file.size / 1024 / 1024).toFixed(1);
            setStatus('Upload failed: File too large (' + fileMB + 'MB, max ~' + freeMB + 'MB free)', true);
            input.value = '';
            return;
          }
          doUpload(file);
        }).catch(function(err) {
          doUpload(file);
        });
        
        function doUpload(file) {
          setStatus('Uploading ' + file.name + '...', false);
          var targetPath = currentPath === '/' ? '/' + file.name : currentPath + '/' + file.name;
          hwUploadFile(file, targetPath, {
            onProgress: function(pct, label) {
              var wrap = document.getElementById(progressId);
              var bar = document.getElementById(progressId + '_bar');
              var pctEl = document.getElementById(progressId + '_pct');
              var lblEl = document.getElementById(progressId + '_label');
              if (wrap) wrap.style.display = '';
              if (bar) bar.style.width = pct + '%';
              if (pctEl) pctEl.textContent = pct + '%';
              if (lblEl) lblEl.textContent = label || ('Uploading ' + file.name + '...');
            },
            onDone: function(ok, msg) {
              var wrap = document.getElementById(progressId);
              if (wrap) wrap.style.display = 'none';
              setStatus(ok ? 'Uploaded: ' + file.name : msg, !ok);
              if (ok) { loadExplorer(); if (config.onRefresh) config.onRefresh(currentPath); }
              input.value = '';
            }
          });
        } // end doUpload
      };
      input.click();
    };
    
    // Action: Refresh
    window[managerId + 'Refresh'] = function() {
      loadExplorer();
      if (config.onRefresh) config.onRefresh(currentPath);
    };
    
    // Action: View file
    window[managerId + 'ViewFile'] = function(filePath) {
      // AVI recordings can't be rendered natively by browsers. If the shared
      // AVI player is present on this page, open it in the modal instead of
      // navigating to the raw file URL.
      var lower = filePath.toLowerCase();
      if (lower.endsWith('.avi') && typeof window.openAviPlayer === 'function') {
        var slash = filePath.lastIndexOf('/');
        var base = slash >= 0 ? filePath.substring(slash + 1) : filePath;
        window.openAviPlayer(base);
        return;
      }
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
<script>
// ===========================================================================
// Shared bonded-device filesystem helper (window.BondFs)
// Lets the bonded MASTER reach the peer's filesystem over the bond session
// token (the 'remote:' command path) — no username/password. Used by the Files
// page (browse + download) and the Logging page (peer log retrieval + control).
// All requests are gated by /api/bond/status reporting bonded + role===master.
// ===========================================================================
window.BondFs = (function(){
  var st = { ok:false, peerMac:'', peerName:'', localMac:'', seq:0 };
  function token(mac){ return (mac||'').replace(/:/g,'').toUpperCase(); }
  function join(base,name){ if(base==='/'||base===''){return '/'+name;} return (base.charAt(base.length-1)==='/'?base:base+'/')+name; }
  function esc(s){ return String(s).replace(/\\/g,'\\\\').replace(/'/g,"\\'"); }
  // The CLI-text parseListing() that used to translate `files /path` output
  // into entry objects has been removed. All peer-FS operations now go
  // through structured opcodes (FS_LIST / FS_STAT / FS_GET).
  // cb(available:bool, state). Reveals nothing on its own — caller decides.
  function checkAvailable(cb){
    hw.fetchJSON('/api/bond/status').then(function(d){
      if(d&&d.bonded===true&&d.role===1&&d.peerMac){ st.ok=true; st.peerMac=d.peerMac; st.peerName=d.peerName||'bonded device'; st.localMac=d.localMac||''; cb(true,st); }
      else { st.ok=false; cb(false,st); }
    }).catch(function(){ st.ok=false; cb(false,st); });
  }
  // Run a command on the peer over the bond token; collect streamed output lines.
  // opts: { doneMarker, maxPolls, intervalMs, onResult:function(lines,err) }
  //
  // CONCURRENCY: each exec() tracks its OWN seq baseline in `mySeq`. Earlier
  // versions used a shared st.seq, which caused brutal races when two execs
  // ran concurrently (e.g., the file listing and the storage stats fire
  // together when the user clicks "Bonded Device"). Whichever exec's polling
  // fired first would advance st.seq past messages the other still needed,
  // and the loser would either time out or fire its done callback on the
  // WRONG command's output. Per-call seq is the only correct design — both
  // execs read the same global message stream but each filters its own
  // completion via doneMarker. The extra `since=0` baseline GET per exec is
  // cheap (no messages returned past the head).
  function exec(cmd, opts){
    opts=opts||{}; var doneMarker=opts.doneMarker; var maxPolls=opts.maxPolls||20; var iv=opts.intervalMs||500;
    var mySeq=0;
    // Baseline: discover current head seq so polling only sees NEW responses.
    hw.fetchJSON('/api/espnow/messages?since=0&mac='+encodeURIComponent(st.peerMac)).then(function(d){
      var m=(d&&d.messages)?d.messages:[]; for(var i=0;i<m.length;i++){ if((m[i].seq||0)>mySeq) mySeq=m[i].seq; }
      return hw.postForm('/api/bond/exec', { cmd: cmd });
    }).then(function(r){return r.json();}).then(function(d){
      if(!d||!d.success){ if(opts.onResult) opts.onResult(null,(d&&(d.result||d.error))||'command failed'); return; }
      var lines=[], done=false, polls=0, lastNew=0, gotAny=false; var grace=opts.gracePolls||6;
      var t=setInterval(function(){
        polls++;
        hw.fetchJSON('/api/espnow/messages?since='+mySeq+'&mac='+encodeURIComponent(st.peerMac)).then(function(md){
          var ms=(md&&md.messages)?md.messages:[]; var newCount=0;
          for(var i=0;i<ms.length;i++){ var m=ms[i]; if(m.seq>mySeq) mySeq=m.seq; if(m.msg){ var ps=String(m.msg).split('\n'); for(var p=0;p<ps.length;p++) lines.push(ps[p]); newCount++; if(doneMarker&&String(m.msg).indexOf(doneMarker)>=0) done=true; } }
          if(newCount>0){ lastNew=polls; gotAny=true; }
          if(done){ clearInterval(t); if(opts.onResult) opts.onResult(lines,null); }
          else if(!doneMarker && gotAny && (polls-lastNew)>=grace){ clearInterval(t); if(opts.onResult) opts.onResult(lines,null); }  // settled after last output
          else if(polls>=maxPolls){ clearInterval(t); if(opts.onResult) opts.onResult(lines, lines.length?null:'timed out'); }
        }).catch(function(){});
      }, iv);
    }).catch(function(e){ if(opts.onResult) opts.onResult(null,e.message); });
  }
  // List a directory on the peer. done(entries|null, err).
  // entries shape: [{name, isDir, size, perms}] sorted folders-first, alpha.
  function list(path, done){
    var qp = '/api/bond/fs/list?path=' + encodeURIComponent(path||'/');
    hw.fetchJSON(qp).then(function(d){
      if (!d || d.success !== true) {
        done(null, (d && d.error) || 'failed');
        return;
      }
      var entries = (d.entries || []).map(function(e){
        return { name: e.name, isDir: !!e.isDir, size: e.size|0, perms: e.perms|0 };
      });
      entries.sort(function(a,b){
        if (a.isDir !== b.isDir) return a.isDir ? -1 : 1;
        return a.name < b.name ? -1 : (a.name > b.name ? 1 : 0);
      });
      done(entries, null);
    }).catch(function(e){
      done(null, (e && e.message) || 'network');
    });
  }
  // Storage stats for a path on the peer. done({total,used,free,usagePercent}|null, err).
  // Replaces the prior `exec('fsusage')` 4-line text scrape.
  function stat(path, done){
    var qp = '/api/bond/fs/stat?path=' + encodeURIComponent(path||'/');
    hw.fetchJSON(qp).then(function(d){
      if (!d || d.success !== true) { done(null, (d && d.error) || 'failed'); return; }
      done({
        total: d.total, used: d.used, free: d.free,
        usagePercent: d.usagePercent,
        path: d.path
      }, null);
    }).catch(function(e){ done(null, (e && e.message) || 'network'); });
  }
  // Pull a file from the peer onto THIS device.
  // done({localPath, localUrl, base, size}|null, err).
  //
  // Two-stage: structured FS_GET_REQ kicks the transfer (returns size + ack
  // status synchronously), then we poll the local landing directory for the
  // file to appear. File content rides the existing FILE_START/DATA/END
  // pipeline so behavior is identical to the legacy path — just a
  // deterministic trigger instead of a CLI scrape.
  function pull(remotePath, done){
    if (!st.peerMac){ done(null,'no bonded peer'); return; }
    var base = remotePath.split('/').pop();
    var dir = '/espnow/received/' + token(st.peerMac);
    var localPath = dir + '/' + base;
    var localUrl = '/api/files/read?name=' + encodeURIComponent(localPath);
    // Pre-create the landing dir so the polling list() below doesn't spam
    // [STORAGE] Cannot open directory until the first chunk lands.
    function mk(p, next){ hw.postFormText('/api/cli', { cmd: 'mkdir "'+p+'"' }).then(function(){next();}).catch(function(){next();}); }
    mk('/espnow/received', function(){ mk(dir, function(){
      var qp = '/api/bond/fs/get?path=' + encodeURIComponent(remotePath);
      hw.fetchJSON(qp).then(function(d){
        if (!d || d.success !== true) {
          done(null, (d && d.error) || 'get failed');
          return;
        }
        // ACK was OK — peer is now sending the file. Poll local FS for it
        // to land. Same pattern the legacy implementation used; not a wire
        // concern, just async observation.
        var expectedSize = d.size|0;
        var polls = 0;
        var t = setInterval(function(){
          polls++;
          hw.fetchJSON('/api/files/list?path=' + encodeURIComponent(dir)).then(function(ld){
            var items = (ld && ld.files) ? ld.files : [];
            for (var i = 0; i < items.length; i++) {
              var nm = items[i] && items[i].name ? items[i].name : items[i];
              if (nm === base || String(nm).split('/').pop() === base) {
                clearInterval(t);
                done({localPath:localPath, localUrl:localUrl, base:base, size:expectedSize}, null);
                return;
              }
            }
            if (polls >= 30) { clearInterval(t); done(null,'timed out pulling '+base); }
          }).catch(function(){ if (polls >= 30) clearInterval(t); });
        }, 600);
      }).catch(function(e){ done(null, (e && e.message) || 'network'); });
    }); });
  }
  // Render a directory explorer into containerId.
  // Visually matches createFileExplorer/createFileManager from the local file
  // manager: bordered panel, toolbar w/ Back+Refresh, [Root]-style breadcrumb,
  // icons from /api/icon, formatted sizes, hover rows, status bar.
  // opts: { onNavigate:function(path), fileActions:[{label, fn:function(fullPath)}], status }
  function renderExplorer(containerId, path, entries, opts){
    opts=opts||{}; var c=document.getElementById(containerId); if(!c) return;
    var key=containerId.replace(/[^A-Za-z0-9_]/g,'_');
    var navName='__bondNav_'+key, actName='__bondAct_'+key;
    window[navName]=opts.onNavigate||function(){};
    window[actName]=opts.fileActions||[];

    // Icon / size / fallback rendering is shared with the local file explorer
    // via window.FileBrowser (defined at the top of the createFileExplorer
    // IIFE). Adding a new file extension or tweaking icon styling there
    // automatically updates this view too.

    var peerLabel=opts.peerLabel||(st.peerName||'Bonded device');
    var h='';
    // Outer bordered container (matches createFileManager)
    h+='<div style="border:1px solid var(--border);border-radius:4px;background:var(--panel-bg);color:var(--panel-fg);overflow:hidden;">';

    // Toolbar: Back (when not at root), Refresh, peer label right-aligned
    h+='<div style="padding:8px;background:var(--crumb-bg);border-bottom:1px solid var(--border);display:flex;gap:8px;flex-wrap:wrap;align-items:center;">';
    if(path!=='/'){
      var trimmed=path.replace(/\/+$/,''); var idx=trimmed.lastIndexOf('/');
      var parent=idx<=0?'/':trimmed.substring(0,idx);
      h+='<button class="btn" onclick="window[\''+navName+'\'](\''+esc(parent)+'\')">← Back</button>';
    }
    h+='<button class="btn" onclick="window[\''+navName+'\'](\''+esc(path)+'\')">Refresh</button>';
    h+='<span style="margin-left:auto;font-size:0.85em;color:var(--muted);">'+peerLabel+'</span>';
    h+='</div>';

    // Breadcrumb (shared with createFileExplorer via FileBrowser.breadcrumbHtml)
    h+='<div style="padding:8px;background:var(--crumb-bg);border-bottom:1px solid var(--border);font-size:0.9em;color:var(--panel-fg);">';
    h+=window.FileBrowser.breadcrumbHtml(path, function(p){ return "window['"+navName+"']('"+esc(p)+"')"; });
    h+='</div>';

    // List
    h+='<div style="overflow-y:auto;">';
    if(!entries||entries.length===0){
      var emptyMsg=opts.status||'No files found';
      h+='<div style="padding:20px;text-align:center;color:var(--muted);">'+emptyMsg+'</div>';
    } else {
      h+='<div style="padding:4px;">';
      for(var e=0;e<entries.length;e++){
        var en=entries[e]; var full=esc(join(path,en.name));
        // Build per-file action buttons (only for files) from fileActions[].
        // Each entry may carry an optional `title` for the tooltip — falls
        // back to the visible label when omitted.
        var actionsHtml='';
        if(!en.isDir){
          var acts=window[actName];
          if(acts&&acts.length){
            var btns='';
            for(var a=0;a<acts.length;a++){
              var ttl=acts[a].title || acts[a].label;
              btns+='<button class="btn btn-small" onclick="window[\''+actName+'\']['+a+'].fn(\''+full+'\');event.stopPropagation();" style="padding:4px 8px;font-size:0.8em;" title="'+ttl+'">'+acts[a].label+'</button>';
            }
            if(btns) actionsHtml='<span style="display:inline-flex;gap:2px;margin-left:8px;align-items:center;">'+btns+'</span>';
          }
        }
        // Make file rows click-to-trigger-first-action (typically View). The
        // action buttons themselves stopPropagation so e.g. clicking Download
        // doesn't also fire View. Folders keep navigate-into-dir click.
        var rowClick = '';
        if (en.isDir) {
          rowClick = "window['"+navName+"']('"+full+"')";
        } else {
          var rcActs = window[actName];
          if (rcActs && rcActs.length) {
            rowClick = "window['"+actName+"'][0].fn('"+full+"')";
          }
        }
        h+=window.FileBrowser.rowHtml({
          name: en.name,
          isDir: en.isDir,
          // en.size is the bonded-fs entry's byte count (post-FS_LIST_REPLY).
          // formatSize already accepts bare integers, so passing the number
          // through works without changing that helper.
          sizeInfo: en.isDir ? '' : window.FileBrowser.formatSize(en.size, false),
          iconName: window.FileBrowser.iconName(en.name, en.isDir),
          iconFallback: window.FileBrowser.iconFallback(en.isDir),
          clickExpr: rowClick,
          hoverable: true,
          actionsHtml: actionsHtml
        });
      }
      h+='</div>';
    }
    h+='</div>';

    // Status bar at bottom — Path: ... (matches createFileManager)
    h+='<div style="padding:6px 8px;background:var(--crumb-bg);border-top:1px solid var(--border);font-size:0.85em;color:var(--muted);min-height:24px;">Path: '+path+'</div>';

    h+='</div>';
    c.innerHTML=h;
  }
  // Public surface. parseListing was removed when peer FS moved to structured
  // opcodes (FS_LIST/FS_STAT/FS_GET). stat() was added in the same pass to
  // replace the fsusage CLI scrape — exposed here so the Files page can call
  // it from updateBondedStorageStats().
  return { state:st, token:token, join:join, esc:esc, checkAvailable:checkAvailable, exec:exec, list:list, stat:stat, pull:pull, renderExplorer:renderExplorer };
})();
console.log('[BondFs] shared helper loaded');
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

// Resolve a user's saved theme preference: "light", "dark" or "system".
// Pages built by streamBeginHtml() get the real preference re-applied by
// hw.initTheme() after load, but standalone pages that don't ship that script
// have only this to go on, so they must render the right colours server-side.
const char* resolveUserThemePref(const String& username);

// Stream text into an HTML context with &, <, >, ", and ' escaped (attribute-
// safe in both quote styles). Required for any file content emitted inside
// <pre>: unescaped, a file containing "</pre><script>" runs against the
// viewing user's session.
void streamHtmlEscaped(httpd_req_t* req, const char* buf, size_t len);

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
    "--bg:linear-gradient(135deg,#667eea 0%,#764ba2 60%);"
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
    "--icon-bg:transparent;"
    "--code-bg:#f8f9fa;"
    "--code-fg:#212529;"
    "--icon-filter:none;"
    "--danger:#dc3545;"
    "--danger-hover:#c82333;"
    "--accent:#667eea;"
    "--success:#28a745;"
    "--success-hover:#218838;"
    "--terminal-bg:#12121c;"
    "--terminal-fg:#d4d4d4;"
    "--placeholder:rgba(255,255,255,.65);"
    "}"
    "html[data-theme=light]{"
    "--bg:linear-gradient(135deg,#667eea 0%,#764ba2 60%);"
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
    "--icon-bg:transparent;"
    "--code-bg:#f8f9fa;"
    "--code-fg:#212529;"
    "--icon-filter:none;"
    "--danger:#dc3545;"
    "--danger-hover:#c82333;"
    "--accent:#667eea;"
    "--placeholder:rgba(255,255,255,.65);"
    "--success:#28a745;"
    "--success-hover:#218838;"
    "--terminal-bg:#12121c;"
    "--terminal-fg:#d4d4d4;"
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
    "--code-bg:#1e1e1e;"
    "--code-fg:#d4d4d4;"
    "--icon-filter:invert(1);"
    "--danger:#ff5a6a;"
    "--danger-hover:#ff3b4e;"
    "--accent:#818cf8;"
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
    "--terminal-bg:#12121c;"
    "--terminal-fg:#d4d4d4;"
    "--placeholder:rgba(242,242,247,.5);"
    "}"
    "input::placeholder,textarea::placeholder{color:var(--placeholder);opacity:1}"
    "input::-webkit-input-placeholder,textarea::-webkit-input-placeholder{color:var(--placeholder);opacity:1}"
    "input::-moz-placeholder,textarea::-moz-placeholder{color:var(--placeholder);opacity:1}"
    "input:-ms-input-placeholder,textarea:-ms-input-placeholder{color:var(--placeholder);opacity:1}"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{font-family:'Segoe UI',Tahoma,Geneva,Verdana,sans-serif;"
    "background:var(--bg);"
    "min-height:100vh;color:var(--fg);line-height:1.6}"
    ".content{padding:1rem;max-width:1600px;margin:0 auto}"
    ".card{background:var(--card-bg);backdrop-filter:blur(10px);"
    "border-radius:15px;padding:2rem;margin:1rem 0;border:1px solid var(--card-border);"
    "box-shadow:0 8px 32px rgba(0,0,0,.1)}"
    ".top-menu{background:var(--menu-bg);padding:0.5rem 0.75rem;display:flex;"
    "justify-content:space-between;align-items:center;flex-wrap:wrap;gap:0.4rem}"
    ".menu-left{display:flex;gap:0.4rem;flex-wrap:wrap}"
    ".menu-item,button.menu-item{color:var(--menu-item-fg);text-decoration:none;font-weight:500;padding:.4rem .8rem;border-radius:8px;"
    "transition:all .3s;border:1px solid var(--border);background:var(--menu-item-bg);"
    "box-shadow:0 2px 4px rgba(0,0,0,.1);display:inline-block;line-height:1.2}"
    "button.menu-item{cursor:pointer}"
    ".menu-item:hover,button.menu-item:hover{color:#222;background:rgba(255,255,255,.9);border-color:rgba(0,0,0,.3);"
    "transform:translateY(-1px);box-shadow:0 4px 8px rgba(0,0,0,.15)}"
    ".menu-item.active{color:#fff;background:rgba(255,255,255,.2);border-color:rgba(255,255,255,.4);font-weight:600}"
    ".user-info{display:flex;align-items:center;gap:0.4rem;flex-wrap:wrap}"
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
    ".icon-invert{filter:var(--icon-filter)}"
    "img.icon-invert{filter:var(--icon-filter)}"
    ".menu-item img,.btn img,.settings-panel img:not(.no-invert){filter:var(--icon-filter)}"
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
    ".sys-card{background:rgba(255,255,255,0.08);border-radius:8px;padding:0.75rem;border:1px solid rgba(255,255,255,0.15)}"
    ".sys-card-tall{grid-row:span 2;display:flex;flex-direction:column;gap:0.5rem}"
    ".sys-card-row{display:flex;justify-content:space-between;align-items:center;gap:0.5rem;min-width:0}"
    ".sys-card-row>strong{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}"
    ".input-medium{width:260px}"
    ".settings-panel{background:var(--panel-bg);border-radius:8px;padding:1rem 1.5rem;margin:1rem 0;color:var(--panel-fg);border:1px solid var(--border)}"
    ".settings-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:1rem}"
    ".alert{padding:12px;border-radius:8px;margin-bottom:15px;border:1px solid}"
    ".alert-warning{background:var(--warning-bg);color:var(--warning-fg);border-color:var(--warning-border)}"
    ".alert-info{background:var(--info-bg);color:var(--info-fg);border-color:var(--info-border)}"
    ".status-dot{width:12px;height:12px;border-radius:50%;display:inline-block}"
    ".status-inactive{background:var(--muted)}"
    ".status-active{background:var(--success)}"
    "@keyframes pulse{0%{opacity:1}50%{opacity:.5}100%{opacity:1}}"
    "@keyframes pulse-fast{0%{opacity:1}50%{opacity:.3}100%{opacity:1}}"
    "@keyframes blink{0%{opacity:1}50%{opacity:.3}100%{opacity:1}}"
    "@keyframes slideIn{from{opacity:0;transform:translateY(10px)}to{opacity:1;transform:translateY(0)}}"
    ".status-indicator{display:inline-block;width:12px;height:12px;min-width:12px;min-height:12px;flex:0 0 12px;border-radius:50%;margin-right:8px;box-sizing:content-box;vertical-align:middle}"
    ".status-enabled{background:var(--success);animation:pulse 2s infinite}"
    ".status-disabled{background:var(--danger)}"
    ".status-recording{background:#e74c3c;animation:blink 1s infinite}"
    ".status-running{background:var(--success);animation:pulse 2s infinite}"
    ".status-wake{background:var(--warning-accent,#ffc107);animation:pulse-fast .5s infinite}"
    ".text-accent{color:var(--accent)}", HTTPD_RESP_USE_STRLEN);
  
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
    ".modal-dialog{position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);background:var(--panel-bg);color:var(--panel-fg);padding:1.25rem;border-radius:8px;min-width:320px;max-width:min(420px,calc(100vw - 2rem));box-sizing:border-box;border:1px solid var(--border)}"
    ".table{width:100%;border-collapse:collapse}"
    ".table th,.table td{padding:.5rem;text-align:left;border-bottom:1px solid var(--border);color:var(--panel-fg)}"
    ".table-striped tr:nth-child(odd){background:rgba(255,255,255,.05)}", HTTPD_RESP_USE_STRLEN);
}

// Stream global themed dialog system (hwAlert/hwConfirm/hwPrompt + window.alert override).
// Called once per authenticated page from streamBeginHtml(); zero heap allocation.
inline void streamCommonDialogs(httpd_req_t* req) {
  if (!req) return;
  httpd_resp_send_chunk(req,
    "<div id='hw-dlg' style='display:none;position:fixed;inset:0;background:rgba(0,0,0,0.55);z-index:99999;align-items:center;justify-content:center'>"
    "<div style='background:var(--panel-bg);color:var(--panel-fg);border:1px solid var(--border);border-radius:8px;padding:1.5rem;min-width:280px;max-width:420px;box-shadow:0 8px 32px rgba(0,0,0,0.4)'>"
    "<p id='hw-dlg-msg' style='margin-bottom:0.75rem;font-weight:500;white-space:pre-wrap'></p>"
    "<input id='hw-dlg-inp' class='form-input' style='width:100%;margin-bottom:0.75rem;display:none' type='text'>"
    "<div style='display:flex;gap:0.5rem;justify-content:flex-end'>"
    "<button id='hw-dlg-cancel' class='btn' style='display:none'>Cancel</button>"
    "<button id='hw-dlg-ok' class='btn'>OK</button>"
    "</div></div></div>"
    "<script>(function(){"
    "var d=document.getElementById('hw-dlg');"
    "var m=document.getElementById('hw-dlg-msg');"
    "var inp=document.getElementById('hw-dlg-inp');"
    "var ok=document.getElementById('hw-dlg-ok');"
    "var ca=document.getElementById('hw-dlg-cancel');"
    "var res=null;"
    "function show(msg,mode,def){"
      "return new Promise(function(resolve){"
        "res=resolve;m.textContent=msg;"
        "var ip=(mode==='prompt');var al=(mode==='alert');"
        "inp.style.display=ip?'':'none';"
        "inp.value=(ip&&def!=null)?def:'';"
        "ca.style.display=al?'none':'';"
        "d.style.display='flex';"
        "if(ip)setTimeout(function(){inp.focus();inp.select();},50);else ok.focus();"
      "});"
    "}"
    "function closeD(v){d.style.display='none';if(res){res(v);res=null;}}"
    "ok.addEventListener('click',function(){closeD(inp.style.display!=='none'?inp.value:true);});"
    "ca.addEventListener('click',function(){closeD(null);});"
    "inp.addEventListener('keydown',function(e){if(e.key==='Enter')closeD(inp.value);if(e.key==='Escape')closeD(null);});"
    "d.addEventListener('click',function(e){if(e.target===d)closeD(null);});"
    "window.hwAlert=function(msg){return show(String(msg),'alert',null);};"
    "window.hwConfirm=function(msg){return show(String(msg),'confirm',null);};"
    "window.hwPrompt=function(msg,def){return show(String(msg),'prompt',def!=null?String(def):'');};"
    "window.alert=function(msg){hwAlert(msg);};"
    "})();</script>", HTTPD_RESP_USE_STRLEN);
}

#else  // !ENABLE_HTTP_SERVER

// No-op stub implementations (real implementations in WebServer_Utils.cpp)
inline WebMirrorBuf::WebMirrorBuf() : buf(nullptr), cap(0), len(0), mutex(nullptr) {}
inline void WebMirrorBuf::init(size_t) {}
inline void WebMirrorBuf::clear() {}
inline void WebMirrorBuf::append(const String&, bool) {}
inline void WebMirrorBuf::append(const char*, bool) {}
inline void WebMirrorBuf::appendDirect(const char*, size_t, bool) {}
inline String WebMirrorBuf::snapshot() { return String(); }
inline size_t WebMirrorBuf::snapshotTo(char*, size_t) { return 0; }
inline void WebMirrorBuf::assignFrom(const String&) {}

#endif // ENABLE_HTTP_SERVER

#endif // WEBSERVER_UTILS_H
