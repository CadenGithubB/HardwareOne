#ifndef WEBPAGE_BOND_H
#define WEBPAGE_BOND_H

#include <Arduino.h>
#include "System_BuildConfig.h"
#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>
#endif
#include "WebServer_Utils.h"

// Register Bond device handlers
void registerBondHandlers(httpd_handle_t server);

// Stream inner content for embedding in other pages
void streamBondInner(httpd_req_t* req);

#endif // WEBPAGE_BOND_H
