#ifndef WEBPAGE_LOGIN_H
#define WEBPAGE_LOGIN_H

// ============================================================================
// WebPage_Login.h - Login page content
// ============================================================================

// Forward declaration - implemented in WebServer_Server.cpp
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

#endif // WEBPAGE_LOGIN_H
