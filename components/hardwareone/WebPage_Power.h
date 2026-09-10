#ifndef WEBPAGE_POWER_H
#define WEBPAGE_POWER_H

#include "System_BuildConfig.h"

#if ENABLE_WEB_POWER

#include "WebServer_Server.h"   // httpd_req_t / httpd_handle_t types

// GET /power            — the power page: live status, CPU/power-mode presets,
//                         idle power-save + sleep-cooldown tuning, and the
//                         restart / RAM-flush / sleep actions. Web parity with
//                         the OLED Power menu and the G2 Power page.
esp_err_t handlePowerPage(httpd_req_t* req);
// GET /api/power/status — live snapshot (JSON). Same schema as `power json`;
//                         both call buildPowerJson() in System_Power.cpp.
esp_err_t handlePowerStatus(httpd_req_t* req);
// Registers both routes; called from startWebServer() under #if ENABLE_WEB_POWER.
void registerPowerHandlers(httpd_handle_t server);

#endif // ENABLE_WEB_POWER
#endif // WEBPAGE_POWER_H
