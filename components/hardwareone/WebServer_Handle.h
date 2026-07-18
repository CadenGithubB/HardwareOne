#pragma once

#include <esp_http_server.h>

// The single HTTP server handle, defined in WebServer_Server.cpp. Non-web
// modules that only need to check/stop the server (WiFi disconnect, OLED
// network/system menus, first-time-setup, etc.) include this lightweight header
// instead of the heavy WebServer_Server.h — a single source of truth for the
// handle, replacing the ad-hoc `extern httpd_handle_t server;` that was inlined
// across ~8 files.
extern httpd_handle_t server;

// Live "is the HTTP server running right now" state, as distinct from
// gSettings.httpAutoStart, which is the persisted intent to start it at boot.
// The two diverge the moment someone runs closehttp/openhttp at runtime, so
// callers reporting status — or capturing live state — must use this, never the
// setting.
inline bool isHttpServerRunning() { return server != nullptr; }
