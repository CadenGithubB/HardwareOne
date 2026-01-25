#include "System_BuildConfig.h"

#if ENABLE_HTTP_SERVER

#include <Arduino.h>

#include "WebPage_Bluetooth.h"
#include "WebServer_Server.h"
#include "WebServer_Utils.h"
#include "System_User.h"

static void streamBluetoothContent(httpd_req_t* req) {
  String u;
  isAuthed(req, u);
  streamBeginHtml(req, "Bluetooth", false, u, "bluetooth");
  httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
  streamBluetoothInner(req);
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
  streamEndHtml(req);
}

esp_err_t handleBluetoothPage(httpd_req_t* req) {
  AuthContext ctx;
  ctx.transport = SOURCE_WEB;
  ctx.opaque = req;
  ctx.path = req ? req->uri : "/bluetooth";
  getClientIP(req, ctx.ip);
  if (!tgRequireAuth(ctx)) return ESP_OK;

  streamPageWithContent(req, "bluetooth", ctx.user, streamBluetoothContent);
  return ESP_OK;
}

void registerBluetoothHandlers(httpd_handle_t server) {
  static httpd_uri_t bluetoothPage = { 
    .uri = "/bluetooth", 
    .method = HTTP_GET, 
    .handler = handleBluetoothPage, 
    .user_ctx = NULL 
  };
  httpd_register_uri_handler(server, &bluetoothPage);
}

#endif // ENABLE_HTTP_SERVER
