// WebServer_Utils.cpp - Web server utility functions and helpers
// Stateless utilities for HTTP request handling, navigation, tokens, etc.

#include "WebServer_Utils.h"
#include "System_BuildConfig.h"

#if ENABLE_HTTP_SERVER

#include "System_Debug.h"
#include "System_MemUtil.h"
#include "System_User.h"
#include "System_UserSettings.h"
#include "System_Settings.h"
#include <lwip/sockets.h>
#include <esp_random.h>
#include <memory>

// ============================================================================
// Web Mirror Buffer Implementation
// ============================================================================

// Global instance
WebMirrorBuf gWebMirror;
size_t gWebMirrorCap = 8192;  // 8KB default capacity (reduced from 14KB)

// Constructor
WebMirrorBuf::WebMirrorBuf()
  : buf(nullptr), cap(0), len(0), mutex(nullptr) {}

// Initialize buffer with given capacity
void WebMirrorBuf::init(size_t capacity) {
  cap = capacity;
  len = 0;
  buf = (char*)ps_alloc(cap, AllocPref::PreferPSRAM, "gWebMirror.buf");
  if (buf) buf[0] = '\0';
  // Create mutex for thread-safe access
  if (!mutex) {
    mutex = xSemaphoreCreateMutex();
  }
}

// Clear buffer content
void WebMirrorBuf::clear() {
  if (mutex && xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    len = 0;
    if (buf) buf[0] = '\0';
    xSemaphoreGive(mutex);
  }
}

// Append String with optional newline
void WebMirrorBuf::append(const String& s, bool needNewline) {
  if (!buf || cap == 0) return;
  if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
  
  size_t addNL = (needNewline && len > 0) ? 1 : 0;
  size_t slen = s.length();
  size_t need = addNL + slen;
  
  // If overflow, drop oldest to keep only last cap bytes
  if (need >= cap) {
    // Only last portion of s will remain
    const char* src = s.c_str() + (slen - (cap - 1));
    memcpy(buf, src, cap - 1);
    len = cap - 1;
    buf[len] = '\0';
    xSemaphoreGive(mutex);
    return;
  }
  
  // Ensure space by trimming from front if required
  while (len + need > cap) {
    // remove up to and including first '\n' or at least 1 char
    int nl = -1;
    for (size_t i = 0; i < len; ++i) {
      if (buf[i] == '\n') {
        nl = (int)i;
        break;
      }
    }
    size_t drop = (nl >= 0 ? (size_t)(nl + 1) : (size_t)1);
    memmove(buf, buf + drop, len - drop);
    len -= drop;
    buf[len] = '\0';
  }
  
  if (addNL) { buf[len++] = '\n'; }
  memcpy(buf + len, s.c_str(), slen);
  len += slen;
  buf[len] = '\0';
  
  xSemaphoreGive(mutex);
}

// Append const char* with optional newline
void WebMirrorBuf::append(const char* s, bool needNewline) {
  if (!s) return;
  appendDirect(s, strlen(s), needNewline);
}

// Zero-copy append with pre-calculated length (hot path optimization)
void WebMirrorBuf::appendDirect(const char* s, size_t slen, bool needNewline) {
  if (!buf || cap == 0 || !s) return;
  if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
  
  size_t addNL = (needNewline && len > 0) ? 1 : 0;
  size_t need = addNL + slen;
  
  // If overflow, drop oldest to keep only last cap bytes
  if (need >= cap) {
    // Only last portion of s will remain
    const char* src = s + (slen - (cap - 1));
    memcpy(buf, src, cap - 1);
    len = cap - 1;
    buf[len] = '\0';
    xSemaphoreGive(mutex);
    return;
  }
  
  // Ensure space by trimming from front if required
  while (len + need > cap) {
    // remove up to and including first '\n' or at least 1 char
    int nl = -1;
    for (size_t i = 0; i < len; ++i) {
      if (buf[i] == '\n') {
        nl = (int)i;
        break;
      }
    }
    size_t drop = (nl >= 0 ? (size_t)(nl + 1) : (size_t)1);
    memmove(buf, buf + drop, len - drop);
    len -= drop;
    buf[len] = '\0';
  }
  
  if (addNL) { buf[len++] = '\n'; }
  memcpy(buf + len, s, slen);
  len += slen;
  buf[len] = '\0';
  
  xSemaphoreGive(mutex);
}

// Replace buffer content with string
void WebMirrorBuf::assignFrom(const String& s) {
  if (!buf || cap == 0) return;
  if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
  
  size_t sl = s.length();
  if (sl >= cap) {
    memcpy(buf, s.c_str() + (sl - (cap - 1)), cap - 1);
    len = cap - 1;
    buf[len] = '\0';
  } else {
    memcpy(buf, s.c_str(), sl);
    len = sl;
    buf[len] = '\0';
  }
  
  xSemaphoreGive(mutex);
}

// Return snapshot of current buffer content (legacy - allocates String)
String WebMirrorBuf::snapshot() {
  if (!buf) return String("");
  if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) != pdTRUE) return String("");
  
  String result(buf);
  xSemaphoreGive(mutex);
  return result;
}

// Zero-copy snapshot to caller-provided buffer (hot path optimization)
// Returns actual bytes copied (excluding null terminator)
size_t WebMirrorBuf::snapshotTo(char* dest, size_t destSize) {
  if (!dest || destSize == 0) return 0;
  if (!buf) {
    dest[0] = '\0';
    return 0;
  }
  if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    dest[0] = '\0';
    return 0;
  }
  
  size_t copyLen = (len < destSize - 1) ? len : (destSize - 1);
  memcpy(dest, buf, copyLen);
  dest[copyLen] = '\0';
  
  xSemaphoreGive(mutex);
  return copyLen;
}

// ============================================================================
// HTTP Request Utilities
// ============================================================================

// Client IP extraction
void getClientIP(httpd_req_t* req, char* ipBuf, size_t bufSize) {
  if (!ipBuf || bufSize < 2) return;
  ipBuf[0] = '-';
  ipBuf[1] = '\0';

  int sockfd = httpd_req_to_sockfd(req);
  if (sockfd < 0) { return; }
  struct sockaddr_storage addr;
  socklen_t len = sizeof(addr);
  if (getpeername(sockfd, (struct sockaddr*)&addr, &len) == 0) {
    if (addr.ss_family == AF_INET) {
      struct sockaddr_in* a = (struct sockaddr_in*)&addr;
      inet_ntop(AF_INET, &(a->sin_addr), ipBuf, bufSize);
      return;
    }
#if LWIP_IPV6
    else if (addr.ss_family == AF_INET6) {
      struct sockaddr_in6* a6 = (struct sockaddr_in6*)&addr;
      inet_ntop(AF_INET6, &(a6->sin6_addr), ipBuf, bufSize);
      return;
    }
#endif
  }
}

// Legacy String version for compatibility (calls zero-churn version)
void getClientIP(httpd_req_t* req, String& ipOut) {
  char buf[64];
  getClientIP(req, buf, sizeof(buf));
  ipOut = buf;
}

// Get header value from request
bool getHeaderValue(httpd_req_t* req, const char* name, String& out) {
  size_t len = httpd_req_get_hdr_value_len(req, name);
  if (!len) {
    BROADCAST_PRINTF("[auth] header missing: %s", name);
    return false;
  }
  std::unique_ptr<char, void (*)(void*)> buf((char*)ps_alloc(len + 1, AllocPref::PreferPSRAM, "http.header"), free);
  if (httpd_req_get_hdr_value_str(req, name, buf.get(), len + 1) != ESP_OK) return false;
  out = String(buf.get());
  BROADCAST_PRINTF("[auth] got header %s: %s", name, out.c_str());
  return true;
}

// Get cookie value from request
bool getCookieValue(httpd_req_t* req, const char* key, String& out) {
  char buf[256];
  size_t len = sizeof(buf);
  if (httpd_req_get_cookie_val(req, key, buf, &len) == ESP_OK) {
    out = String(buf);
    BROADCAST_PRINTF("[auth] cookie %s=\"%s\"", key, out.c_str());
    return true;
  }
  // Do not fall back to manual parsing to avoid misreads; simply report absence.
  BROADCAST_PRINTF("[auth] cookie key not found: %s", key);
  return false;
}

// Get session ID from cookie
String getCookieSID(httpd_req_t* req) {
  size_t hdr_len = httpd_req_get_hdr_value_len(req, "Cookie");
  if (hdr_len == 0) {
    // Limit URI length in logs to avoid format-truncation warnings
    DEBUG_AUTHF("No Cookie header for URI: %.96s", req->uri);
    return "";
  }
  static const size_t COOKIE_BUF_SIZE = 512;  // 512 bytes (empirically sized: actual usage is ~40 bytes, with headroom)

  if (hdr_len > COOKIE_BUF_SIZE) {
    DEBUG_AUTHF("Cookie header unusually large (%d bytes) – capping read to %u", hdr_len, (unsigned)COOKIE_BUF_SIZE);
    hdr_len = COOKIE_BUF_SIZE;
  }

  // Read into a PSRAM buffer to avoid heap allocations and also avoid large stack usage
  static char* cookieBuf = nullptr;
  if (!cookieBuf) {
    cookieBuf = (char*)ps_alloc(COOKIE_BUF_SIZE, AllocPref::PreferPSRAM, "cookie.buf");
    if (!cookieBuf) return "";
  }

  if (httpd_req_get_hdr_value_str(req, "Cookie", cookieBuf, COOKIE_BUF_SIZE) != ESP_OK) {
    // Limit URI length in logs to avoid format-truncation warnings
    WARN_SESSIONF("Failed to get Cookie header for URI: %.96s", req->uri);
    return "";
  }

  // DEBUG: Log cookie buffer usage (only if unusually large)
  size_t cookieLen = strlen(cookieBuf);
  if (cookieLen > 100) {  // Only log if cookie is larger than expected
    int usagePct = (cookieLen * 100) / COOKIE_BUF_SIZE;
    // Limit URI length in logs to avoid format-truncation warnings
    DEBUG_MEMORYF("[COOKIE_BUF] Used %u/%u bytes (%d%%) for %.96s",
                  (unsigned)cookieLen, (unsigned)COOKIE_BUF_SIZE, usagePct, req->uri);
  }
  // Trim leading spaces
  char* p = cookieBuf;
  while (*p == ' ' || *p == '\t') ++p;
  // Find "session=" token
  const char key[] = "session=";
  const size_t klen = sizeof(key) - 1;
  char* s = p;
  char* sidStart = nullptr;
  while (*s) {
    if (strncmp(s, key, klen) == 0) {
      sidStart = s + klen;
      break;
    }
    // advance to next token (semicolon-separated), but also check every position for robustness
    ++s;
  }
  if (!sidStart) {
    DEBUG_AUTHF("No session cookie found");
    return "";
  }
  // Find end of value (semicolon or end)
  char* sidEnd = sidStart;
  while (*sidEnd && *sidEnd != ';') ++sidEnd;
  // Create String from the exact slice by temporarily NUL-terminating
  char saved = *sidEnd;
  *sidEnd = '\0';
  String sid = String(sidStart);
  *sidEnd = saved;
  return sid;
}

// Generate a random session token
String makeSessToken() {
  // Hex-only token: 96 bits random + 32 bits time (approx)
  uint32_t r1 = esp_random();
  uint32_t r2 = esp_random();
  uint32_t r3 = esp_random();
  uint32_t t = millis();
  char buf[(8 * 4) + 1];  // 8 hex chars * 4 values + NUL
  // Cast to unsigned long and use %08lx to satisfy GCC's format checking on this platform
  snprintf(buf, sizeof(buf), "%08lx%08lx%08lx%08lx",
           (unsigned long)r1, (unsigned long)r2,
           (unsigned long)r3, (unsigned long)t);
  return String(buf);
}

// ============================================================================
// Navigation HTML Generation
// ============================================================================

String generatePublicNavigation() {
  String nav =
    "<div class=\"top-menu\">"
    "<div class=\"menu-left\">";
  nav += "</div>";
  nav += "<div class=\"user-info\">";
  nav += "<a href=\"/login\" class=\"login-btn\">Login</a>";
  nav += "</div></div>";
  return nav;
}

String generateNavigation(const String& activePage, const String& username, const char* initialTheme) {
  String nav =
    "<div class=\"top-menu\">"
    "<div class=\"menu-left\">";
  auto link = [&](const char* href, const char* id, const char* text) {
    nav += "<a href=\"";
    nav += href;
    nav += "\" class=\"menu-item";
    if (activePage == id) nav += " active";
    nav += "\">";
    nav += text;
    nav += "</a>";
  };
  link("/dashboard", "dashboard", "Dashboard");
  link("/cli", "cli", "Command Line");
#if ENABLE_WEB_SENSORS
  link("/sensors", "sensors", "Sensors");
#endif
#if ENABLE_WEB_MAPS
  link("/maps", "maps", "Maps");
#endif
#if ENABLE_WEB_GAMES
  link("/games", "games", "Games");
#endif
#if ENABLE_WEB_BLUETOOTH
  link("/bluetooth", "bluetooth", "Bluetooth");
#endif
#if ENABLE_WEB_ESPNOW
  link("/espnow", "espnow", "ESP-NOW");
#if ENABLE_WEB_PAIR
  if (gSettings.bondModeEnabled) {
    link("/bond", "bond", "Bond");
  }
#endif
#endif
#if ENABLE_WEB_MQTT
  link("/mqtt", "mqtt", "MQTT");
#endif
  link("/files", "files", "Files");
  link("/logging", "logging", "Logging");
#if ENABLE_WEB_SPEECH
  link("/speech", "speech", "Speech");
#endif
#if ENABLE_AUTOMATION
  link("/automations", "automations", "Automations");
#endif
  link("/settings", "settings", "Settings");
  nav += "</div>";
  nav += "<div class=\"user-info\">";
  if (username == "guest") {
    nav += "<a href=\"/login\" class=\"login-btn\">Login</a>";
  } else {
    nav += "<div class=\"username\">" + username + "</div>";
    nav += "<button type=\"button\" class=\"menu-item\" id=\"theme-toggle-icon\" onclick=\"(function(){var t=document.documentElement.dataset.theme||'light';var n=(t==='dark')?'light':'dark';if(window.hw&&window.hw.applyTheme){window.hw.applyTheme(n);window.hw.saveThemePref(n)}else{document.documentElement.dataset.theme=n}})()\" style=\"padding:0.4rem 0.8rem;min-width:auto;font-size:1.2rem;\">"; nav += (initialTheme && strcmp(initialTheme, "dark") == 0) ? "\U0001F319" : "\u2600\uFE0F"; nav += "</button>";
    nav += "<a href=\"/logout\" class=\"logout-btn\">Logout</a>";
  }
  nav += "</div>";
  nav += "</div>";
  return nav;
}

// ============================================================================
// HTTP Streaming Helpers
// ============================================================================

// External debug helpers
extern void streamDebugRecord(size_t bytes, size_t chunkSize);
extern void streamDebugFlush();

esp_err_t streamChunkC(httpd_req_t* req, const char* s) {
  if (!req || !s) return ESP_FAIL;
  size_t len = strlen(s);
  esp_err_t ret = httpd_resp_send_chunk(req, s, len);
  if (ret != ESP_OK) {
    WARN_WEBF("Failed to send chunk of %d bytes, error: %d", len, ret);
  }
  return ret;
}

esp_err_t streamChunkBuf(httpd_req_t* req, const char* buf, size_t len) {
  if (!req || !buf) return ESP_FAIL;
  return httpd_resp_send_chunk(req, buf, len);
}

void streamChunk(httpd_req_t* req, const String& str) {
  httpd_resp_send_chunk(req, str.c_str(), str.length());
}

void streamChunk(httpd_req_t* req, const char* str) {
  httpd_resp_send_chunk(req, str, strlen(str));
}

void streamBeginHtml(httpd_req_t* req,
                     const char* title,
                     bool isPublic,
                     const String& username,
                     const String& activePage) {
  if (!req) return;
  httpd_resp_set_type(req, "text/html");

  const char* initialTheme = "light";
  if (!isPublic && username.length()) {
    uint32_t uid = 0;
    if (getUserIdByUsername(username, uid) && uid > 0) {
      JsonDocument settings;
      if (loadUserSettings(uid, settings)) {
        const char* t = settings["theme"] | "light";
        if (t && strcmp(t, "dark") == 0) {
          initialTheme = "dark";
        } else {
          initialTheme = "light";
        }
      }
    }
  }

  // Basic head start
  streamChunkC(req, "<!DOCTYPE html>\n<html data-theme=\"");
  streamChunkC(req, initialTheme);
  streamChunkC(req, "\"><head><meta charset=\"utf-8\">");
  streamChunkC(req, "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  {
    char tb[160];
    const char* t = (title && title[0]) ? title : "HardwareOne";
    snprintf(tb, sizeof(tb), "<title>%s</title>", t);
    streamChunkC(req, tb);
  }

  // Stream CSS directly - no String allocation
  streamChunkC(req, "<style>");
  streamCommonCSS(req);
  streamChunkC(req, "</style>");

  // Add inline background style to prevent flash of unstyled content (FOUC)
  // The CSS variables may not be parsed immediately, so we set the background directly
  if (isPublic) {
    streamChunkC(req, "</head><body class=\"public\" style=\"background:linear-gradient(135deg,#667eea 0%,#764ba2 100%)\">");
  } else {
    if (strcmp(initialTheme, "dark") == 0) {
      streamChunkC(req, "</head><body class=\"auth\" style=\"background:linear-gradient(135deg,#07070b 0%,#151520 100%)\">");
    } else {
      streamChunkC(req, "</head><body class=\"auth\" style=\"background:linear-gradient(135deg,#667eea 0%,#764ba2 100%)\">");
    }
  }

  // Navigation
#ifdef ARDUINO
  if (isPublic) {
    String nav = generatePublicNavigation();
    if (nav.length()) httpd_resp_send_chunk(req, nav.c_str(), nav.length());
  } else {
    String nav = generateNavigation(activePage, username, initialTheme);
    if (nav.length()) httpd_resp_send_chunk(req, nav.c_str(), nav.length());
  }
#endif

  // Shared lightweight client helpers (available as window.hw)
  if (!isPublic) {
    streamChunkC(req, "<script>(function(w){'use strict';var hw=w.hw||(w.hw={});function sysTheme(){try{return (w.matchMedia&&w.matchMedia('(prefers-color-scheme: dark)').matches)?'dark':'light'}catch(_){return 'light'}}function dbg(){try{return !!(w.localStorage&&w.localStorage.getItem('hwDebugTheme')==='1')}catch(_){return false}}function log(){try{if(dbg())console.log.apply(console,arguments)}catch(_){}}hw.updateThemeIcon=function(){var btn=document.getElementById('theme-toggle-icon');if(btn){var t=document.documentElement.dataset.theme||'light';btn.textContent=(t==='dark')?'🌙':'☀️'}};hw.applyTheme=function(pref){var v=(pref==='system'||!pref)?sysTheme():pref;document.documentElement.dataset.theme=v;hw._themePref=pref||'light';if(document.body){document.body.style.background=(v==='dark')?'linear-gradient(135deg,#07070b 0%,#151520 100%)':'linear-gradient(135deg,#667eea 0%,#764ba2 100%)'}hw.updateThemeIcon();log('[theme] apply pref=',pref,'->',v)};hw.loadThemePref=function(){log('[theme] load pref from /api/user/settings');return (hw.fetchJSON?hw.fetchJSON('/api/user/settings') : fetch('/api/user/settings',{credentials:'include',cache:'no-store',headers:{'Accept':'application/json'}}).then(function(r){return r.json()})).then(function(d){var pref=(d&&d.settings&&d.settings.theme)?d.settings.theme:'light';log('[theme] loaded',pref,'raw=',d);return pref}).catch(function(e){log('[theme] load failed',e);return 'light'})};hw.saveThemePref=function(pref){var body={theme:pref};log('[theme] save',body);return (hw.postJSON?hw.postJSON('/api/user/settings',body) : fetch('/api/user/settings',{method:'POST',credentials:'include',headers:{'Content-Type':'application/json','Accept':'application/json'},body:JSON.stringify(body)}).then(function(r){return r.json()})).then(function(d){log('[theme] save resp',d);return d}).catch(function(e){log('[theme] save failed',e);return null})};hw.initTheme=function(){var initial=document.documentElement.dataset.theme||'light';document.documentElement.dataset.theme=initial;log('[theme] init initial=',initial);hw.loadThemePref().then(function(pref){hw.applyTheme(pref)});try{var mq=w.matchMedia('(prefers-color-scheme: dark)');if(mq&&mq.addEventListener){mq.addEventListener('change',function(){if(hw._themePref==='system')hw.applyTheme('system')})}}catch(_){}};hw.cycleTheme=function(){var cur=hw._themePref||'light';var next=(cur==='light')?'dark':((cur==='dark')?'system':'light');hw.applyTheme(next);hw.saveThemePref(next)};try{hw.initTheme();}catch(_){}})(window);</script>");
  }
  streamChunkC(req, "<script>(function(w){'use strict';var hw=w.hw||(w.hw={});hw.qs=function(s,c){return (c||document).querySelector(s)};hw.qsa=function(s,c){return (c||document).querySelectorAll(s)};hw.on=function(e,v,f){if(e)e.addEventListener(v,f)};hw._ge=function(x){return typeof x==='string'?document.getElementById(x):x};hw.setText=function(x,t){var el=hw._ge(x);if(el)el.textContent=t};hw.setHTML=function(x,h){var el=hw._ge(x);if(el)el.innerHTML=h};hw.show=function(x){var el=hw._ge(x);if(el)el.style.display=''};hw.hide=function(x){var el=hw._ge(x);if(el)el.style.display='none'};hw.toggle=function(x,sh){(sh?hw.show:hw.hide)(x)};hw.fetchJSON=function(u,o){o=o||{};if(!o.credentials)o.credentials='include';if(!o.cache)o.cache='no-store';if(!o.headers)o.headers={};o.headers['Accept']='application/json';return fetch(u,o).then(function(r){if(r.status===401){return r.json().then(function(d){if(d&&d.error==='auth_required'&&d.reload){w.location.href='/login'}throw new Error('auth_required')}).catch(function(){w.location.href='/login';throw new Error('auth_required')})}if(!r.ok)throw new Error('HTTP '+r.status);return r.json()})};hw.postJSON=function(u,b,o){o=o||{};o.method='POST';o.headers=Object.assign({'Content-Type':'application/json'},o.headers||{});o.body=JSON.stringify(b||{});return hw.fetchJSON(u,o)};hw.postForm=function(u,form,o){o=o||{};o.method='POST';o.headers=Object.assign({'Content-Type':'application/x-www-form-urlencoded'},o.headers||{});var b=[];for(var k in (form||{})){if(Object.prototype.hasOwnProperty.call(form,k)){b.push(encodeURIComponent(k)+'='+encodeURIComponent(form[k]))}};o.body=b.join('&');if(!o.credentials)o.credentials='include';if(!o.cache)o.cache='no-store';return fetch(u,o)};try{console.log('[HW] helpers ready');}catch(_){} })(window);</script>");
  streamChunkC(req, "<script>(function(w){var hw=w.hw||(w.hw={});hw.pollJSON=function(u,ms,cb){try{cb=cb||function(){};ms=ms||1000;var h=setInterval(function(){hw.fetchJSON(u).then(cb).catch(function(e){if(e&&e.message==='auth_required'){clearInterval(h)}})},ms);return function(){clearInterval(h)};}catch(_){return function(){}}};try{console.log('[HW] page=\"");
  streamChunkC(req, activePage.c_str());
  streamChunkC(req, "\"');}catch(_){}})(window);</script>");

  // Global themed dialog system (hwAlert / hwConfirm / hwPrompt + window.alert override)
  if (!isPublic) {
    streamCommonDialogs(req);
  }

  // Shared notification toast system (CSS + container + JS)
  if (!isPublic) {
    // Toast CSS - minimal, matches OLED ribbon feel (slides from top-right)
    streamChunkC(req,
      "<style>"
      "#hw-toast-wrap{position:fixed;top:60px;right:12px;z-index:9999;display:flex;flex-direction:column;gap:8px;pointer-events:none;max-width:calc(100vw - 24px)}"
      ".hw-toast{pointer-events:auto;display:flex;align-items:center;gap:8px;padding:10px 16px;border-radius:8px;"
      "background:rgba(30,30,40,0.92);color:#fff;font:600 13px/1.3 -apple-system,sans-serif;"
      "box-shadow:0 4px 12px rgba(0,0,0,0.3);backdrop-filter:blur(8px);"
      "animation:hwToastIn .3s ease-out;max-width:480px;overflow-x:auto;white-space:nowrap}"
      ".hw-toast.out{animation:hwToastOut .25s ease-in forwards}"
      ".hw-toast-icon{flex-shrink:0;width:18px;text-align:center;font-size:14px}"
      ".hw-toast-msg{overflow-x:auto;white-space:nowrap;scrollbar-width:thin;scrollbar-color:rgba(255,255,255,0.3) transparent}"
      ".hw-toast-msg::-webkit-scrollbar{height:4px}"
      ".hw-toast-msg::-webkit-scrollbar-thumb{background:rgba(255,255,255,0.3);border-radius:2px}"
      "@keyframes hwToastIn{from{opacity:0;transform:translateY(-12px)}to{opacity:1;transform:translateY(0)}}"
      "@keyframes hwToastOut{to{opacity:0;transform:translateY(-12px)}}"
      "[data-theme=dark] .hw-toast{background:rgba(255,255,255,0.12);border:1px solid rgba(255,255,255,0.15)}"
      "</style>");

    // Toast container
    streamChunkC(req, "<div id=\"hw-toast-wrap\"></div>");

    // Toast JS - hw.notify(level, msg, durationMs) + SSE auto-listener
    streamChunkC(req,
      "<script>(function(w){'use strict';"
      "var hw=w.hw||(w.hw={});"
      "var icons={success:'\\u2714',error:'\\u2716',warning:'\\u26A0',info:'\\u2139'};"
      "var wrap=null;"
      "hw.notify=function(level,msg,ms){"
        "if(!wrap)wrap=document.getElementById('hw-toast-wrap');"
        "if(!wrap)return;"
        "ms=ms||4000;"
        "var el=document.createElement('div');"
        "el.className='hw-toast';"
        "var ic=icons[level]||icons.info;"
        "el.innerHTML='<span class=\"hw-toast-icon\">'+ic+'</span><span class=\"hw-toast-msg\">'+hw._esc(msg)+'</span>';"
        "wrap.appendChild(el);"
        "var t=setTimeout(function(){el.classList.add('out');setTimeout(function(){if(el.parentNode)el.parentNode.removeChild(el)},300)},ms);"
        "el.onclick=function(){clearTimeout(t);el.classList.add('out');setTimeout(function(){if(el.parentNode)el.parentNode.removeChild(el)},300)};"
        "if(wrap.children.length>5){var old=wrap.children[0];if(old&&old.parentNode)old.parentNode.removeChild(old)}"
      "};"
      "hw._esc=function(s){var d=document.createElement('div');d.textContent=s;return d.innerHTML};"
      // SSE auto-connect for notifications (optional transport - page works without it)
      // Reuses window.__es if Dashboard already created one; otherwise creates and stores it
      "function sseNotify(){"
        "if(!w.EventSource)return;"
        "try{"
          "var es=w.__es;"
          "if(!es||es.readyState===2){"
            "es=new EventSource('/api/events',{withCredentials:true});"
            "w.__es=es;"
            "es.onerror=function(){try{es.close()}catch(_){};w.__es=null;setTimeout(sseNotify,10000)}"
          "}"
          "es.addEventListener('notification',function(e){"
            "try{var d=JSON.parse(e.data);hw.notify(d.level||'info',d.msg||'',d.ms||4000)}catch(_){}"
          "})"
        "}catch(_){}"
      "}"
      "if(document.readyState==='loading'){document.addEventListener('DOMContentLoaded',sseNotify)}else{sseNotify()}"
      "})(window);</script>");
  }

  // Open content container
  streamChunkC(req, "<div class=\"content\">");
}

void streamEndHtml(httpd_req_t* req) {
  if (!req) return;
  streamChunkC(req, "</div></body></html>");
  httpd_resp_send_chunk(req, nullptr, 0);  // End chunked response
}

void streamNav(httpd_req_t* req, const String& username, const String& activePage) {
  if (!req) return;
#ifdef ARDUINO
  String nav = generateNavigation(username, activePage);
  if (nav.length()) httpd_resp_send_chunk(req, nav.c_str(), nav.length());
#endif
}

void streamContentGeneric(httpd_req_t* req, const String& content) {
  const char* contentStr = content.c_str();
  size_t contentLen = content.length();
  size_t chunkSize = 5119;  // 5KB buffer size - 1 for null terminator

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  DEBUG_HTTPF("streamContentGeneric: content_len=%u", (unsigned)contentLen);

  for (size_t i = 0; i < contentLen; i += chunkSize) {
    size_t remainingBytes = contentLen - i;
    size_t currentChunkSize = (remainingBytes > chunkSize) ? chunkSize : remainingBytes;
    streamDebugRecord(currentChunkSize, chunkSize);
    httpd_resp_send_chunk(req, contentStr + i, currentChunkSize);
  }

  httpd_resp_send_chunk(req, NULL, 0);  // End chunked response
  streamDebugFlush();
}

#endif // ENABLE_HTTP_SERVER
