#ifndef WEBPAGE_LOGINREQUIRED_H
#define WEBPAGE_LOGINREQUIRED_H

// ============================================================================
// WebPage_LoginRequired.h - Login required page content
// ============================================================================

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

#endif // WEBPAGE_LOGINREQUIRED_H
