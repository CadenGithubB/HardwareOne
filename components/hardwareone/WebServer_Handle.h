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

// --- Deadlock-safe HTTP server teardown -------------------------------------
//
// httpd_stop() blocks until the httpd task exits, and the httpd task only
// notices the shutdown between requests. A handler that is mid-request sits in
// submitAndExecuteSync waiting on cmd_exec_task — so calling httpd_stop from a
// command (closewifi, closehttp, radio power-off all run on cmd_exec_task)
// while a web request is in flight is a circular wait: the handler waits for
// the command, the command waits for the handler's task to exit. It unwedges
// only via submitAndExecuteSync's 60 s timeout, and takes the whole command
// pipeline (G2, OLED, MQTT, automations) down with it for that minute.
//
// httpServerStopSafe() therefore stops inline when nobody is waiting (the
// common case), and otherwise defers the stop to the main loop, which is
// neither task and can never be part of the cycle.

// Number of httpd-task callers currently blocked waiting on cmd_exec_task.
// Maintained by submitAndExecuteSync (System_Utils.cpp).
extern volatile int gWebCmdWaiters;

// Stop the server without risking the circular wait above. Returns true if the
// stop happened inline, false if it was deferred to the main loop. Safe to call
// when the server is already stopped (no-op, returns true).
bool httpServerStopSafe();

// Runs a pending deferred stop once the last web waiter has drained. Called
// once per pass from hardwareone_loop().
void httpServerStopPendingTick();
