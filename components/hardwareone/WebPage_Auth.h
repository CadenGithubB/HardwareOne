#ifndef WEBPAGE_AUTH_H
#define WEBPAGE_AUTH_H

// ============================================================================
// WebPage_Auth.h - Authentication-related web pages
// ============================================================================
// Merged from WebPage_Login.h, WebPage_LoginSuccess.h, and WebPage_AuthRequired.h

// Forward declaration - implemented in main file
String getLogoutReasonForAuthPage(httpd_req_t* req);

// ============================================================================
// Login Page - Inner content only (handler wraps with streamBeginHtml/streamEndHtml)
// ============================================================================
inline void streamLoginInner(httpd_req_t* req, const String& username, const String& errorMsg, const String& logoutReason) {
  // Form opening with title
  httpd_resp_send_chunk(req, R"LOGIN1(<div class='panel container-narrow space-top-md'>
  <div class='text-center space-bottom-sm'>
    <h2>Sign In</h2>
    <p class='text-muted' style='margin:0'>Use your HardwareOne credentials</p>
  </div>
)LOGIN1", HTTPD_RESP_USE_STRLEN);
  
  // Error message section (combined error + logout reason)
  if (errorMsg.length() > 0 || logoutReason.length() > 0) {
    httpd_resp_send_chunk(req, "  <div id='err' class='form-error text-danger'>", HTTPD_RESP_USE_STRLEN);
    
    if (errorMsg.length() > 0) {
      httpd_resp_send_chunk(req, errorMsg.c_str(), HTTPD_RESP_USE_STRLEN);
    }
    
    if (logoutReason.length() > 0) {
      if (errorMsg.length() > 0) {
        httpd_resp_send_chunk(req, "<br>", HTTPD_RESP_USE_STRLEN);
      }
      httpd_resp_send_chunk(req, "<div class='alert alert-warning mb-3' style='background:#fff3cd;border:1px solid #ffeaa7;color:#856404;padding:12px;border-radius:4px;'><strong>Session Terminated:</strong> ", HTTPD_RESP_USE_STRLEN);
      httpd_resp_send_chunk(req, logoutReason.c_str(), HTTPD_RESP_USE_STRLEN);
      httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
    }
    
    httpd_resp_send_chunk(req, "</div>\n", HTTPD_RESP_USE_STRLEN);
  } else {
    httpd_resp_send_chunk(req, "  <div id='err' class='form-error' style='display:none'></div>\n", HTTPD_RESP_USE_STRLEN);
  }
  
  // Form with username field
  httpd_resp_send_chunk(req, R"LOGIN2(  <form method='POST' action='/login'>
    <div class='form-field'><label>Username</label>
      <input class='form-input' name='username' value=')LOGIN2", HTTPD_RESP_USE_STRLEN);
  
  // Dynamic username value
  if (username.length() > 0) {
    httpd_resp_send_chunk(req, username.c_str(), HTTPD_RESP_USE_STRLEN);
  }
  
  // Password field and buttons
  httpd_resp_send_chunk(req, R"LOGIN3(' type='text'></div>
    <div class='form-field'><label>Password</label>
      <input class='form-input' name='password' value='' type='password'></div>
    <div class='btn-row space-top-md'>
      <button class='btn btn-primary' type='submit'>Sign In</button>
      <a class='btn btn-secondary' href='/register'>Request Account</a>
    </div>
  </form>
</div>
<script>console.log('[LOGIN] Section 1: Pre-script sentinel');</script>
<script>
console.log('[LOGIN] Page loaded');
window.addEventListener('load', function(){ 
  console.log('[LOGIN] Window onload event');
  setTimeout(function(){ 
    try{ 
      var msg = sessionStorage.getItem('revokeMsg'); 
      if(msg){ 
        console.log('[LOGIN] Found revoke message:',msg);
        sessionStorage.removeItem('revokeMsg'); 
        alert(msg); 
      } else {
        console.log('[LOGIN] No revoke message found');
      }
    }catch(e){
      console.error('[LOGIN] Error checking revoke message:',e);
    } 
  }, 500); 
});
console.log('[LOGIN] Script complete');
</script>
)LOGIN3", HTTPD_RESP_USE_STRLEN);
}

// ============================================================================
// Login Success Page - Full page with redirect
// ============================================================================
inline void streamLoginSuccessContent(httpd_req_t* req, const String& sessionId) {
  httpd_resp_set_type(req, "text/html");
  
  // HTML head with meta tags and CSS
  httpd_resp_send_chunk(req, R"LOGINSUCCESS1(<!DOCTYPE html><html><head>
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
</head><body>
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

// ============================================================================
// Auth Required Page - Inner content only (handler wraps with streamBeginHtml/streamEndHtml)
// ============================================================================
inline void streamAuthRequiredInner(httpd_req_t* req, const String& logoutReason) {
  // Opening div
  httpd_resp_send_chunk(req, R"AUTHREQ(
<div class='text-center pad-xl'>
  <h2>Authentication Required</h2>
)AUTHREQ", HTTPD_RESP_USE_STRLEN);
  
  // Show logout reason if present
  if (logoutReason.length() > 0) {
    httpd_resp_send_chunk(req, R"LOGOUT(
  <div class='alert alert-warning mb-3' style='background:#fff3cd;border:1px solid #ffeaa7;color:#856404;padding:12px;border-radius:4px;'>
    <strong>Session Terminated:</strong> )LOGOUT", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, logoutReason.c_str(), HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "\n  </div>\n", HTTPD_RESP_USE_STRLEN);
  }
  
  // Main content and closing
  httpd_resp_send_chunk(req, R"AUTHCONTENT(
  <p>You need to sign in to access this page.</p>
  <p class='text-sm' style='color:#fff'>Don't have an account? <a class='link-primary' href='/register' style='text-decoration:none'>Request Access</a></p>
</div>
</div>
<script>console.log('[AUTH_REQUIRED] Section 1: Pre-script sentinel');</script>
<script>
console.log('[AUTH_REQUIRED] Page loaded');
window.addEventListener('load', function(){ 
  console.log('[AUTH_REQUIRED] Window onload event');
  setTimeout(function(){ 
    try{ 
      var msg = sessionStorage.getItem('revokeMsg'); 
      if(msg){ 
        console.log('[AUTH_REQUIRED] Found revoke message:',msg);
        sessionStorage.removeItem('revokeMsg'); 
        alert(msg); 
      } else {
        console.log('[AUTH_REQUIRED] No revoke message found');
      }
    }catch(e){
      console.error('[AUTH_REQUIRED] Error checking revoke message:',e);
    } 
  }, 500); 
});
console.log('[AUTH_REQUIRED] Script complete');
</script>
)AUTHCONTENT", HTTPD_RESP_USE_STRLEN);
}

#endif // WEBPAGE_AUTH_H
