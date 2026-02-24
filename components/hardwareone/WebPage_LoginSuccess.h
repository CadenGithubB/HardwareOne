#ifndef WEBPAGE_LOGINSUCCESS_H
#define WEBPAGE_LOGINSUCCESS_H

// ============================================================================
// WebPage_LoginSuccess.h - Login success page with redirect
// ============================================================================

// Forward declaration for streamCommonCSS (defined in WebServer_Utils.h)
void streamCommonCSS(httpd_req_t* req);

// ============================================================================
// Login Success Page - Full page with redirect
// ============================================================================
inline void streamLoginSuccessContent(httpd_req_t* req, const String& sessionId, const String& theme = "light") {
  httpd_resp_set_type(req, "text/html");

  bool isDark = (theme == "dark");
  const char* bg = isDark
    ? "linear-gradient(135deg,#07070b 0%,#151520 100%)"
    : "linear-gradient(135deg,#667eea 0%,#764ba2 100%)";

  // HTML head with data-theme so CSS variables resolve correctly
  httpd_resp_send_chunk(req, "<!DOCTYPE html><html data-theme='", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, isDark ? "dark" : "light", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, R"LOGINSUCCESS1('><head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width, initial-scale=1'>
<title>Login Successful - HardwareOne</title>
<style>)LOGINSUCCESS1", HTTPD_RESP_USE_STRLEN);
  
  // Common CSS - streaming version (no String allocation)
  streamCommonCSS(req);
  
  // Spinner animation CSS and meta refresh
  httpd_resp_send_chunk(req, R"LOGINSUCCESS2(
@keyframes spin{0%{transform:rotate(0deg)}100%{transform:rotate(360deg)}}
</style>
<meta http-equiv='refresh' content='2;url=/dashboard'>
</head>)LOGINSUCCESS2", HTTPD_RESP_USE_STRLEN);

  // Apply correct background inline (matches streamBeginHtml behaviour)
  httpd_resp_send_chunk(req, "<body style='background:", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, bg, HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, "'>", HTTPD_RESP_USE_STRLEN);

  httpd_resp_send_chunk(req, R"LOGINSUCCESS2(
<div class='content'>
<div class='text-center'>
<div class='card container-narrow'>
<h2 style='color:#fff;margin-bottom:1.5rem'>Login Successful</h2>
<div style='background:rgba(40,167,69,0.1);border:1px solid rgba(40,167,69,0.3);border-radius:8px;padding:1.5rem;margin:1rem 0'>
<p style='color:#fff;margin-bottom:1rem;font-size:1.1rem'>Welcome! You are being redirected to the dashboard...</p>
<div style='display:flex;align-items:center;justify-content:center;gap:0.5rem;color:#87ceeb'>
<div style='width:20px;height:20px;border:2px solid #87ceeb;border-top:2px solid transparent;border-radius:50%;animation:spin 1s linear infinite'></div>
<span>Loading dashboard</span>
</div>
</div>
<p style='font-size:0.9rem;color:#87ceeb;margin-top:1rem'>If you are not redirected automatically, <a href='/dashboard' style='color:#fff;text-decoration:underline'>click here</a>.</p>
</div>
</div>
</div>
<script>console.log('[LOGIN_SUCCESS] Section 1: Pre-script sentinel');</script>
<script>
console.log('[LOGIN_SUCCESS] Page loaded');
try { 
  console.log('[LOGIN_SUCCESS] Setting session cookie');
  document.cookie = 'session=)LOGINSUCCESS2", HTTPD_RESP_USE_STRLEN);
  
  // Dynamic session ID
  httpd_resp_send_chunk(req, sessionId.c_str(), HTTPD_RESP_USE_STRLEN);
  
  // JavaScript cookie polling and closing tags
  httpd_resp_send_chunk(req, R"LOGINSUCCESS3(; Path=/'; 
  console.log('[LOGIN_SUCCESS] Cookie set successfully');
} catch(e) { 
  console.error('[LOGIN_SUCCESS] Cookie set error:', e); 
}
console.log('[LOGIN_SUCCESS] Starting cookie polling...');
(function(){
  var checks = 0; var maxChecks = 10; var timer = setInterval(function(){
    checks++;
    console.log('[LOGIN_SUCCESS] Cookie check #' + checks);
    if (document.cookie && document.cookie.indexOf('session=') >= 0) {
      console.log('[LOGIN_SUCCESS] Session cookie detected; redirecting to /dashboard');
      clearInterval(timer); window.location.href = '/dashboard'; return;
    }
    if (checks >= maxChecks) {
      console.warn('[LOGIN_SUCCESS] Session cookie not detected after ' + maxChecks + ' checks; navigating to /login');
      clearInterval(timer); window.location.href = '/login'; return;
    }
  }, 300);
})();
console.log('[LOGIN_SUCCESS] Script complete');
</script>
</body></html>
)LOGINSUCCESS3", HTTPD_RESP_USE_STRLEN);
  
  httpd_resp_send_chunk(req, nullptr, 0); // End chunked response
}

#endif // WEBPAGE_LOGINSUCCESS_H
