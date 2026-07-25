#include "System_BuildConfig.h"

#if ENABLE_WEB_GAME_DARKROOM

#include <Arduino.h>

#include "System_User.h"
#include "WebPage_DarkRoom.h"
#include "WebServer_Server.h"
#include "WebServer_Utils.h"

// /games — a launcher card inside the normal site shell (nav + header). Keeps
// "A Dark Room" reachable from the Games nav entry, then hands off to the
// full-screen game document at /darkroom.
static void streamDarkRoomLauncher(httpd_req_t* req, const String& username) {
  streamBeginHtml(req, "Games", false, username, "games");
  httpd_resp_send_chunk(req,
    "<div class='card'>"
    "<h2>A Dark Room</h2>"
    "<p class='text-muted'>A minimalist text adventure, running entirely in your "
    "browser. Your progress is saved locally in this browser (Settings &rarr; "
    "export/import makes a portable backup).</p>"
    "<p class='space-top-md'><a class='btn btn-primary' href='/darkroom'>Play A Dark Room</a></p>"
    "</div>",
    HTTPD_RESP_USE_STRLEN);
  streamEndHtml(req);
}

static esp_err_t handleGamesLauncherPage(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  streamPageWithContent(req, "games", ctx.user, streamDarkRoomLauncher);
  return ESP_OK;
}

// /darkroom — the complete self-contained game document, served raw (no site
// shell) so the game's own full-screen layout is preserved.
static esp_err_t handleDarkRoomPage(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
  streamDarkRoomDoc(req);
  httpd_resp_send_chunk(req, NULL, 0);  // terminate the chunked response
  return ESP_OK;
}

void registerDarkRoomHandlers(httpd_handle_t server) {
  static const httpd_uri_t gamesPage = { .uri = "/games", .method = HTTP_GET, .handler = handleGamesLauncherPage, .user_ctx = NULL };
  httpd_register_uri_handler(server, &gamesPage);
  static const httpd_uri_t darkroomPage = { .uri = "/darkroom", .method = HTTP_GET, .handler = handleDarkRoomPage, .user_ctx = NULL };
  httpd_register_uri_handler(server, &darkroomPage);
}

#endif // ENABLE_WEB_GAME_DARKROOM
