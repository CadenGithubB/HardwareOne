#include "System_BuildConfig.h"

#if ENABLE_HTTP_SERVER && ENABLE_GAMES

#include <Arduino.h>

#include "System_User.h"
#include "WebPage_Games.h"
#include "WebServer_Server.h"
#include "WebServer_Utils.h"

static void streamGamesContent(httpd_req_t* req) {
  String u;
  isAuthed(req, u);
  streamBeginHtml(req, "Games", false, u, "games");
  httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
  streamGamesInner(req);
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
  streamEndHtml(req);
}

esp_err_t handleGamesPage(httpd_req_t* req) {
  AuthContext ctx;
  ctx.transport = SOURCE_WEB;
  ctx.opaque = req;
  ctx.path = req ? req->uri : "/games";
  getClientIP(req, ctx.ip);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  logAuthAttempt(true, req->uri, ctx.user, ctx.ip, "");

  streamPageWithContent(req, "games", ctx.user, streamGamesContent);
  return ESP_OK;
}

void registerGamesHandlers(httpd_handle_t server) {
  static httpd_uri_t gamesPage = { .uri = "/games", .method = HTTP_GET, .handler = handleGamesPage, .user_ctx = NULL };
  httpd_register_uri_handler(server, &gamesPage);
}

#endif
