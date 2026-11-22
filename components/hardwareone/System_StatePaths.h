#pragma once

// The state files the device keeps under /system, one owner each.
//
// Every module that opens, backs up, restores, wipes or grants access to one
// of these files takes its path from here: the settings, debug, account and
// automation modules, the ESP-NOW device registry, the path permission table
// (System_Filesystem.cpp sPathRules), the backup/restore tool
// (WebServer_MigrationTool.cpp) and factory reset (System_Utils.cpp). They
// used to spell each path two or three times over, so one edit could split
// the file a module writes from the one the backup copies or the permission
// table guards.
//
// Macros, not variables: the permission table is a static constant array, and
// reply and log texts quote a path by string-literal concatenation
// ("Created " HW1_AUTOMATIONS_JSON_PATH). The older names stay as aliases of
// these where other code or host harnesses use them (SETTINGS_JSON_FILE,
// AUTOMATIONS_JSON_FILE, USERS_JSON_FILE, BOOT_ANCHORS_FILE, DEBUG_JSON_FILE,
// PENDING_USERS_FILE, ESPNOW_DEVICES_FILE).
//
// Changing a value moves a file that devices in the field already hold: a
// renamed path needs a migration, not just an edit here.

#define HW1_SETTINGS_JSON_PATH        "/system/settings.json"
#define HW1_DEBUG_JSON_PATH           "/system/debug.json"  // debug flags, split out of settings.json
#define HW1_USERS_JSON_PATH           "/system/users/users.json"
#define HW1_PENDING_USERS_JSON_PATH   "/system/users/pending_users.json"
#define HW1_USER_SETTINGS_DIR         "/system/users/user_settings"  // <id>.json per account
#define HW1_BOOT_ANCHORS_JSON_PATH    "/system/boot_anchors.json"  // NTP anchors, split out of users.json
#define HW1_AUTOMATIONS_JSON_PATH     "/system/automations.json"
#define HW1_ESPNOW_DEVICES_JSON_PATH  "/system/espnow/devices.json"
#define HW1_NOTIFICATIONS_JSON_PATH   "/system/notifications.json"  // device notification policy
