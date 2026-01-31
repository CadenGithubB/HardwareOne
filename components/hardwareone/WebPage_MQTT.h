// WebPage_MQTT.h - MQTT status and control webpage
// Provides web interface for MQTT client management

#ifndef WEBPAGE_MQTT_H
#define WEBPAGE_MQTT_H

#include "System_BuildConfig.h"

#if ENABLE_HTTP_SERVER && ENABLE_MQTT

#include <esp_http_server.h>

// Register MQTT web page handlers with the HTTP server
void registerMqttHandlers(httpd_handle_t server);

// Stream the inner MQTT page content (for embedding)
void streamMqttInner(httpd_req_t* req);

#else

// Stubs when MQTT or HTTP server is disabled
inline void registerMqttHandlers(httpd_handle_t server) { (void)server; }

#endif // ENABLE_HTTP_SERVER && ENABLE_MQTT

#endif // WEBPAGE_MQTT_H
