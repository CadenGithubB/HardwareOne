#ifndef WEBSERVER_MIGRATIONTOOL_H
#define WEBSERVER_MIGRATIONTOOL_H

#include "System_BuildConfig.h"

#if ENABLE_MIGRATION_TOOL

#include <stdint.h>

#include <esp_http_server.h>

// HWBACKUP bundle format, the bundle's "formatVersion". /api/restore refuses
// a newer one before staging anything; a bundle without the field is format
// 1 (older exports). `versions` reports it as backup_format.
constexpr uint32_t kBackupFormatVersion = 1;

class String;

// Migration tool endpoints — two sub-features with independent gates:
//
//   Backup export (ENABLE_HTTP_SERVER):
//     - POST /api/backup        (authenticated, CORS) - export backup bundle
//     - OPTIONS /api/ping       (CORS preflight)
//
//   FTS restore-only server (ENABLE_MIGRATION_TOOL):
//     - GET  /                  (restore-mode splash, no auth)
//     - POST /api/restore       (unauthenticated, CORS) - stages import during FTS;
//                               apply requires on-device OLED/serial confirm
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

// True after POST /api/restore staged a valid backup and is waiting for
// physical confirm (OLED/serial). LAN alone cannot finish the restore.
bool migrationRestoreAwaitingConfirm();

// Write staged backup files to flash. Sets gRestoreComplete on success.
// Returns false if nothing was staged or the write failed.
bool applyStagedMigrationRestore();

// Discard a staged backup without writing (user cancelled on OLED/serial).
void discardStagedMigrationRestore();

// Provenance of the staged backup, for the confirm prompt; empty when nothing
// is staged. One serial line: "from <hostname> [<board>], <n> bytes,
// sha256 <first 8 hex>..., <source>", where <source> is "this device" (the
// bundle's fingerprint matched this board), "other device (credentials
// skipped)" or "no fingerprint (unverified, ...)" when the bundle carries none.
// Hostname and board are the bundle's claims; size and SHA-256 are of the
// bytes received.
String migrationStagedSummary();

// One serial line telling the operator what to compare the size and SHA-256
// with. A body carrying "force" (a cross-device upload) is not the exported
// file, so it says they will not match.
String migrationStagedCheckAdvice();

// The same, as three 21-character lines for getOLEDYesNoPrompt.
String migrationStagedOledPrompt();

#endif // ENABLE_MIGRATION_TOOL

#endif // WEBSERVER_MIGRATIONTOOL_H
