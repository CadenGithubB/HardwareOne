#include "System_BuildConfig.h"

#if ENABLE_WEB_SPEECH

#include <Arduino.h>

#include "WebPage_Speech.h"
#include "WebServer_Server.h"
#include "WebServer_Utils.h"
#include "System_User.h"

static void streamSpeechContent(httpd_req_t* req) {
  String u;
  isAuthed(req, u);
  streamBeginHtml(req, "Speech", false, u, "speech");
  httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
  streamSpeechInner(req);
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
  streamEndHtml(req);
}

esp_err_t handleSpeechPage(httpd_req_t* req) {
  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) return ESP_OK;

  streamPageWithContent(req, "speech", ctx.user, streamSpeechContent);
  return ESP_OK;
}

void registerSpeechPageHandlers(httpd_handle_t server) {
  static httpd_uri_t speechPage = { 
    .uri = "/speech", 
    .method = HTTP_GET, 
    .handler = handleSpeechPage, 
    .user_ctx = NULL 
  };
  httpd_register_uri_handler(server, &speechPage);
}

#endif // ENABLE_HTTP_SERVER
