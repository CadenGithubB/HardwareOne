#ifndef WEBPAGE_PAIR_H
#define WEBPAGE_PAIR_H

#include <Arduino.h>
#include "System_BuildConfig.h"
#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>
#endif
#include "WebServer_Utils.h"

// Register Paired device handlers
void registerPairHandlers(httpd_handle_t server);

// Stream inner content for embedding in other pages
void streamPairInner(httpd_req_t* req);

#endif // WEBPAGE_PAIR_H
