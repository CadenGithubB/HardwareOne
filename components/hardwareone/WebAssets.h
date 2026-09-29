#pragma once
#include "System_BuildConfig.h"
#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>
void registerWebAssetHandlers(httpd_handle_t server);
#endif
