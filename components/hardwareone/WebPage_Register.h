#ifndef WEBPAGE_REGISTER_H
#define WEBPAGE_REGISTER_H

// Registration form page
static void streamRegisterFormInner(httpd_req_t* req) {
  httpd_resp_send_chunk(req, R"REGFORM(
<script>console.log('[REGISTER] Section 1: Pre-script sentinel');</script>
<h2>Request Account</h2>
<form method='POST' action='/register/submit'>
  <label>Username<br><input name='username'></label><br><br>
  <label>Password<br><input type='password' name='password'></label><br><br>
  <label>Confirm Password<br><input type='password' name='confirm_password'></label><br><br>
  <button class='menu-item' type='submit'>Submit</button>
  <a class='menu-item' href='/login' style='margin-left:.5rem'>Back to Sign In</a>
</form>
<script>
console.log('[REGISTER] Page loaded');
console.log('[REGISTER] Form ready for submission');
</script>
)REGFORM", HTTPD_RESP_USE_STRLEN);
}

void streamRegisterFormContent(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  streamBeginHtml(req, "Request Account", /*isPublic=*/true, "", "");
  streamRegisterFormInner(req);
  streamEndHtml(req);
}

// Registration result pages
static void streamRegisterResultInner(httpd_req_t* req, bool success, const String& message, const String& details = "") {
  httpd_resp_send_chunk(req, "<script>console.log('[REGISTER_RESULT] Section 1: Pre-script sentinel');</script><div style='text-align:center;padding:2rem'>", HTTPD_RESP_USE_STRLEN);
  
  if (success) {
    httpd_resp_send_chunk(req, R"REGSUCCESS(<script>console.log('[REGISTER_RESULT] Success page loaded');</script><h2 style='color:#28a745'>Request Submitted</h2>
<div style='background:#d4edda;border:1px solid #c3e6cb;border-radius:8px;padding:1.5rem;margin:1rem 0'>
<p style='color:#155724;margin-bottom:1rem'>Your account request has been submitted successfully!</p>
<p style='color:#155724;font-size:0.9rem'>An administrator will review your request and approve access to the system.</p>
</div>
<p><a class='menu-item' href='/login'>Return to Sign In</a></p>
)REGSUCCESS", HTTPD_RESP_USE_STRLEN);
  } else {
    httpd_resp_send_chunk(req, "<script>console.log('[REGISTER_RESULT] Error page loaded');</script><h2 style='color:#dc3545'>Registration Failed</h2><p>", HTTPD_RESP_USE_STRLEN);
    
    if (message.length() > 0) {
      httpd_resp_send_chunk(req, message.c_str(), HTTPD_RESP_USE_STRLEN);
    } else {
      httpd_resp_send_chunk(req, "An error occurred.", HTTPD_RESP_USE_STRLEN);
    }
    
    if (details.length() > 0) {
      httpd_resp_send_chunk(req, "<br>", HTTPD_RESP_USE_STRLEN);
      httpd_resp_send_chunk(req, details.c_str(), HTTPD_RESP_USE_STRLEN);
    }
    
    httpd_resp_send_chunk(req, "</p><p><a class='menu-item' href='/register'>Try Again</a></p>", HTTPD_RESP_USE_STRLEN);
  }
  
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
}

void streamRegisterResultContent(httpd_req_t* req, bool success, const String& message, const String& details = "") {
  httpd_resp_set_type(req, "text/html");
  streamBeginHtml(req, success ? "Request Submitted" : "Registration Failed", /*isPublic=*/true, "", "");
  streamRegisterResultInner(req, success, message, details);
  streamEndHtml(req);
}

#endif
