#ifndef WEBSERVER_MIGRATION_TOOL_H
#define WEBSERVER_MIGRATION_TOOL_H

#include <esp_http_server.h>

// Migration tool endpoints (CORS-enabled)
// - GET  /api/ping          (unauthenticated, CORS) - updated in WebServer_Server.cpp
// - POST /api/backup         (authenticated, CORS)   - export backup bundle
// - POST /api/restore        (unauthenticated, CORS) - import during first-time setup only

// Register /api/backup and its OPTIONS preflight (always available, requires auth)
void registerMigrationBackupHandler(httpd_handle_t server);

// Register /api/restore and its OPTIONS preflight (call only during first-time setup restore)
void registerMigrationRestoreHandler(httpd_handle_t server);

// Unregister /api/restore (call after restore completes, before reboot)
void unregisterMigrationRestoreHandler(httpd_handle_t server);

// Register OPTIONS handler for /api/ping (CORS preflight)
void registerPingOptionsHandler(httpd_handle_t server);

// Start a minimal restore-only HTTP server (only /api/ping and /api/restore).
// Called during "Import from Backup" first-time setup instead of startHttpServer().
void startRestoreOnlyHttpServer();

// Stop the restore-only HTTP server after restore completes.
void stopRestoreOnlyHttpServer();

#endif // WEBSERVER_MIGRATION_TOOL_H
