#ifndef WEBSERVER_MIGRATION_TOOL_H
#define WEBSERVER_MIGRATION_TOOL_H

#include "System_BuildConfig.h"

#if ENABLE_MIGRATION_TOOL

#include <esp_http_server.h>

// Migration tool endpoints — two sub-features with independent gates:
//
//   Backup export (ENABLE_HTTP_SERVER):
//     - POST /api/backup        (authenticated, CORS) - export backup bundle
//     - OPTIONS /api/ping       (CORS preflight)
//
//   FTS restore-only server (ENABLE_MIGRATION_TOOL):
//     - GET  /                  (restore-mode splash, no auth)
//     - POST /api/restore       (unauthenticated, CORS) - import during FTS only
//     - OPTIONS+POST /api/ping  (CORS preflight + connection test)

#if ENABLE_HTTP_SERVER
// Register /api/backup and its OPTIONS preflight (always available, requires auth)
void registerMigrationBackupHandler(httpd_handle_t server);

// Register OPTIONS handler for /api/ping (CORS preflight)
void registerPingOptionsHandler(httpd_handle_t server);
#endif // ENABLE_HTTP_SERVER

// Register /api/restore and its OPTIONS preflight (call only during first-time setup restore)
void registerMigrationRestoreHandler(httpd_handle_t server);

// Unregister /api/restore (call after restore completes, before reboot)
void unregisterMigrationRestoreHandler(httpd_handle_t server);

// Start a minimal restore-only HTTP server (only /api/ping and /api/restore).
// Called during "Import from Backup" first-time setup instead of startHttpServer().
void startRestoreOnlyHttpServer();

// Stop the restore-only HTTP server after restore completes.
void stopRestoreOnlyHttpServer();

#endif // ENABLE_MIGRATION_TOOL

#endif // WEBSERVER_MIGRATION_TOOL_H
