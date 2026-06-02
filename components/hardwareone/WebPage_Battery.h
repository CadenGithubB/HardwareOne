#ifndef WEBPAGE_BATTERY_H
#define WEBPAGE_BATTERY_H

#include "System_BuildConfig.h"

#if ENABLE_WEB_BATTERY

#include "WebServer_Server.h"   // httpd_req_t / httpd_handle_t types

// GET /battery            — the battery page (live status + discharge chart + log table)
esp_err_t handleBatteryPage(httpd_req_t* req);
// GET /api/battery/status — capability-flagged live snapshot (JSON)
esp_err_t handleBatteryStatus(httpd_req_t* req);
// Registers both routes; called from startWebServer() under #if ENABLE_WEB_BATTERY.
void registerBatteryHandlers(httpd_handle_t server);

#endif // ENABLE_WEB_BATTERY
#endif // WEBPAGE_BATTERY_H
