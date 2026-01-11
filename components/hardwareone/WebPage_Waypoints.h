#ifndef WEBPAGE_WAYPOINTS_H
#define WEBPAGE_WAYPOINTS_H

#include <esp_http_server.h>
#include "WebServer_Utils.h"

// Waypoint management web page
esp_err_t handleWaypointsPage(httpd_req_t* req);

// API endpoint for waypoint operations (GET/POST)
esp_err_t handleWaypointsAPI(httpd_req_t* req);

#endif // WEBPAGE_WAYPOINTS_H
