#pragma once

#include <esp_http_server.h>

// The single HTTP server handle, defined in WebServer_Server.cpp. Non-web
// modules that only need to check/stop the server (WiFi disconnect, OLED
// network/system menus, first-time-setup, etc.) include this lightweight header
// instead of the heavy WebServer_Server.h — a single source of truth for the
// handle, replacing the ad-hoc `extern httpd_handle_t server;` that was inlined
// across ~8 files.
extern httpd_handle_t server;
