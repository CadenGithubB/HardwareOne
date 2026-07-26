#ifndef WEBPAGE_R1_HEALTH_H
#define WEBPAGE_R1_HEALTH_H

#include "System_BuildConfig.h"

#if ENABLE_WEB_R1_HEALTH

#include "WebServer_Server.h"   // httpd_req_t / httpd_handle_t types

// GET /r1-health           — R1 Health page (vitals + Track; connect on /bluetooth)
esp_err_t handleR1HealthPage(httpd_req_t* req);
// GET /api/health/status   — same JSON as `healthstatus json`
esp_err_t handleR1HealthStatus(httpd_req_t* req);
void registerR1HealthHandlers(httpd_handle_t server);

#else

inline void registerR1HealthHandlers(httpd_handle_t) {}

#endif  // ENABLE_WEB_R1_HEALTH
#endif  // WEBPAGE_R1_HEALTH_H
