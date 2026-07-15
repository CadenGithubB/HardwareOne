/**
 * System Notifications - the human-facing view over the event register.
 *
 * See System_Notifications.h. This file holds the per-kind rules table, the
 * message formatter, and the main-loop renderer. Producers post events; this
 * module decides which become banners/toasts, with per-kind cooldowns. The
 * persistent notification-center view (OLED_Utils.cpp) reads the same rules
 * via notifRuleFor()/notifFormatEvent().
 */

#include "System_Notifications.h"

#include <Arduino.h>
#include <cstdio>
#include <cstring>

#include "System_BuildConfig.h"
#include "System_Settings.h"      // gSettings.notif* + SettingEntry/SettingsModule
#include "System_Debug.h"         // BROADCAST_PRINTF, broadcastOutput, cliHint
#include "System_Utils.h"         // RETURN_VALID_IF_VALIDATE_CSTR, readText/writeTextAtomic
#include "System_User.h"          // isAdminUser, getUserIdByUsername, gLocalDisplay*
#include "System_UserSettings.h"  // loadUserSettings / mergeAndSaveUserSettings
#include "System_AuthIdentity.h"  // currentExecUser (notifyusermute acts on the caller)
#include "System_MemUtil.h"       // PSRAM_JSON_DOC

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"  // esp_timer_get_time — drain timing in the tick

#if ENABLE_OLED_DISPLAY
#include "OLED_UI.h"
#endif

// Real impl in WebServer_Server.cpp; inline no-op stub when HTTP server disabled
#if ENABLE_HTTP_SERVER
#include "WebServer_Server.h"
#else
#include "System_SensorStubs.h"
#endif

// ============================================================================
// Rules table — compiled defaults, one row per kind
// ============================================================================
// Defaults preserve the pre-cutover behavior: the kinds the old notify*()
// bodies rendered keep their sinks/levels/durations/cooldowns; everything
// else is event-only. notifRuleFor() overlays the user's notification
// settings on top of these defaults.

static NotifRule notifDefaultRuleFor(uint8_t kind) {
  constexpr uint8_t ALL = NSINK_BANNER | NSINK_QUEUE | NSINK_TOAST;
  switch (kind) {
    // Mesh / bond (rendered by the tick since the register landed)
    case SYSEVT_PEER_ONLINE:      return {ALL, 1, 2500, 0};
    case SYSEVT_PEER_OFFLINE:     return {ALL, 2, 2500, 0};
    case SYSEVT_PEER_PAIRED:      return {ALL, 1, 2500, 0};
    case SYSEVT_TEXT_RX:          return {ALL, 0, 2500, 0};
    case SYSEVT_FILE_RX:          return {(uint8_t)(NSINK_QUEUE | NSINK_TOAST), 1, 2500, 0};  // receive site shows its own OLED toast
    case SYSEVT_BOND_ONLINE:      return {ALL, 1, 2500, 0};
    case SYSEVT_BOND_OFFLINE:     return {ALL, 2, 2500, 0};
    case SYSEVT_BLE_CONNECTED:    return {ALL, 1, 2500, 0};
    case SYSEVT_BLE_DISCONNECTED: return {ALL, 0, 2000, 0};
    // Connectivity
    case SYSEVT_WIFI_CONNECTED:    return {ALL, 1, 3000, 0};
    case SYSEVT_WIFI_DISCONNECTED: return {ALL, 0, 2000, 0};
    case SYSEVT_WIFI_NET_ADDED:    return {ALL, 1, 2000, 0};
    case SYSEVT_WIFI_NET_REMOVED:  return {ALL, 2, 2000, 0};
    case SYSEVT_ESPNOW_ON:         return {ALL, 1, 2000, 0};
    case SYSEVT_ESPNOW_OFF:        return {ALL, 0, 2000, 0};
    // Power (30s cooldowns replace the old shared statics; posts are
    // never gated, so automations see every edge)
    case SYSEVT_USB_ON:           return {ALL, 1, 2000, 30000};
    case SYSEVT_USB_OFF:          return {ALL, 2, 2000, 30000};
    case SYSEVT_BATTERY_LOW:      return {ALL, 2, 3000, 30000};
    case SYSEVT_BATTERY_CRITICAL: return {ALL, 3, 4000, 30000};
    // Auth
    case SYSEVT_LOGIN_OK:   return {ALL, 1, 2000, 0};
    case SYSEVT_LOGIN_FAIL: return {ALL, 3, 2000, 30000};
    // System
    case SYSEVT_SETTING_CHANGED:     return {NSINK_QUEUE, 0, 1500, 0};  // queue-only — no banner/toast on every config change (setSetting now fires this broadly)
    case SYSEVT_SENSOR_STARTED:      return {ALL, 1, 1500, 0};
    case SYSEVT_SENSOR_STOPPED:      return {ALL, 0, 1500, 0};
    case SYSEVT_SENSOR_START_FAILED: return {ALL, 3, 1500, 0};
    case SYSEVT_FILE_DELETED:        return {ALL, 2, 2000, 0};
    case SYSEVT_VOICE_WAKE:          return {ALL, 0, 2000, 0};
    case SYSEVT_VOICE_COMMAND:       return {ALL, 1, 2000, 0};
    // Coverage additions — security alerts + faults get sinks; the rest stay event-only.
    case SYSEVT_CRASH:             return {ALL, 3, 4000, 0};
    case SYSEVT_USER_BANNED:       return {ALL, 3, 2000, 0};
    case SYSEVT_IP_BANNED:         return {ALL, 3, 2000, 0};
    case SYSEVT_LOGIN_LOCKED:      return {ALL, 3, 2000, 30000};
    case SYSEVT_STORAGE_FORMATTED: return {ALL, 3, 4000, 0};
    case SYSEVT_MQTT_START_FAILED: return {ALL, 2, 3000, 0};
    case SYSEVT_LLM_LOAD_FAILED:   return {ALL, 2, 3000, 0};
    case SYSEVT_LLM_STATE_CORRUPT: return {ALL, 2, 3000, 0};
    case SYSEVT_RTC_POWER_LOSS:    return {(uint8_t)(NSINK_QUEUE | NSINK_TOAST), 2, 3000, 0};
    // Tier 2 — faults + safety alerts get sinks; everything else is event-only.
    case SYSEVT_FACTORY_RESET:        return {ALL, 3, 4000, 0};
    case SYSEVT_CONFIG_FILE_CORRUPT:  return {ALL, 3, 4000, 0};
    case SYSEVT_AUTH_DB_FAULT:        return {ALL, 3, 4000, 0};
    case SYSEVT_SECRET_DECRYPT_FAILED:return {ALL, 2, 3000, 0};
    case SYSEVT_THERMAL_HOT_ALERT:    return {ALL, 3, 3000, 10000};
    case SYSEVT_DISPLAY_INIT_FAILED:  return {(uint8_t)(NSINK_QUEUE | NSINK_TOAST), 2, 3000, 0};
    case SYSEVT_FILE_RX_FAILED:       return {(uint8_t)(NSINK_QUEUE | NSINK_TOAST), 2, 2500, 0};
    case SYSEVT_FIRMWARE_CHANGED:     return {(uint8_t)(NSINK_QUEUE | NSINK_TOAST), 1, 3000, 0};
    case SYSEVT_SD_WRITE_RECOVERED:   return {(uint8_t)(NSINK_QUEUE | NSINK_TOAST), 1, 2500, 0};
    case SYSEVT_BATTERY_FULL:         return {(uint8_t)(NSINK_QUEUE | NSINK_TOAST), 1, 2500, 0};
    // Everything else: event-only (automations/`events`/queue-off).
    default: return {NSINK_NONE, 0, 0, 0};
  }
}

// ============================================================================
// Device policy + viewer resolution (see System_Notifications.h for layers)
// ============================================================================
// Two 4x32 masks hold the per-kind device levels (bit index = kind value,
// same layout as the automation subscription mask). The persisted form is
// name-keyed JSON — kind numbers are internal-only and free to move between
// builds, so the masks are rebuilt from names at boot and on every edit.
#define NOTIF_POLICY_FILE "/system/notifications.json"

static volatile uint32_t gNotifOffMask[4]   = {0, 0, 0, 0};  // level: off
static volatile uint32_t gNotifAdminMask[4] = {0, 0, 0, 0};  // level: admin
static volatile uint32_t gNotifPrefsGen = 1;
// Guards the per-user prefs cache below (resolvers run on the main loop AND
// the OLED task). Created in notifPolicyLoad() before render tasks contend.
static SemaphoreHandle_t gUserPrefsMutex = nullptr;

static inline bool maskTest(const volatile uint32_t* m, uint8_t kind) {
  return kind < 128 && (m[kind >> 5] & (1UL << (kind & 31)));
}
static inline void maskSet(volatile uint32_t* m, uint8_t kind, bool on) {
  if (kind >= 128) return;
  if (on) m[kind >> 5] |= (1UL << (kind & 31));
  else    m[kind >> 5] &= ~(1UL << (kind & 31));
}

uint32_t notifPrefsGeneration() { return gNotifPrefsGen; }

void notifPolicyLoad() {
  if (!gUserPrefsMutex) gUserPrefsMutex = xSemaphoreCreateMutex();
  uint32_t off[4] = {0, 0, 0, 0}, adm[4] = {0, 0, 0, 0};
  String json;
  if (readText(NOTIF_POLICY_FILE, json) && json.length() > 0) {
    PSRAM_JSON_DOC(doc);
    if (deserializeJson(doc, json) == DeserializationError::Ok) {
      JsonObjectConst kinds = doc["kinds"].as<JsonObjectConst>();
      for (JsonPairConst kv : kinds) {
        int k = systemEventKindFromName(kv.key().c_str());
        if (k <= 0 || k >= 128) continue;  // unknown name (renamed kind) — skip
        const char* lvl = kv.value().as<const char*>();
        if (!lvl) continue;
        if (strcmp(lvl, "off") == 0)   off[k >> 5] |= (1UL << (k & 31));
        if (strcmp(lvl, "admin") == 0) adm[k >> 5] |= (1UL << (k & 31));
      }
    }
  }
  for (int i = 0; i < 4; i++) { gNotifOffMask[i] = off[i]; gNotifAdminMask[i] = adm[i]; }
  gNotifPrefsGen++;
}

// Serialize the current masks back to the policy file (non-default kinds only).
static bool notifPolicySave() {
  PSRAM_JSON_DOC(doc);
  JsonObject kinds = doc["kinds"].to<JsonObject>();
  for (int k = SYSEVT_NONE + 1; k < SYSEVT_COUNT; k++) {
    if (maskTest(gNotifOffMask, (uint8_t)k))        kinds[systemEventKindName((uint8_t)k)] = "off";
    else if (maskTest(gNotifAdminMask, (uint8_t)k)) kinds[systemEventKindName((uint8_t)k)] = "admin";
  }
  String out;
  serializeJson(doc, out);
  return writeTextAtomic(NOTIF_POLICY_FILE, out);
}

// --- Per-user prefs cache -------------------------------------------------
// Small cache over /system/users/user_settings/<id>.json so render passes
// don't re-read flash per event. Flushed by notifUserPrefsInvalidate() from
// the saveUserSettings() chokepoint (covers web POST, notifyusermute, password
// ops). Guarded by a mutex: resolvers run on the main loop AND the OLED task.
struct UserPrefsCacheEntry {
  String username;
  uint32_t muteMask[4];
  bool valid;
};
static UserPrefsCacheEntry gUserPrefsCache[4];
static uint8_t gUserPrefsCacheNext = 0;

void notifUserPrefsInvalidate() {
  if (gUserPrefsMutex) xSemaphoreTake(gUserPrefsMutex, portMAX_DELAY);
  for (auto& e : gUserPrefsCache) e.valid = false;
  gNotifPrefsGen++;
  if (gUserPrefsMutex) xSemaphoreGive(gUserPrefsMutex);
}

// Load a user's notificationMuted array into a mask. Missing file / missing
// key / unknown names all resolve to "nothing muted".
static void loadUserMuteMask(const char* username, uint32_t out[4]) {
  memset(out, 0, 4 * sizeof(uint32_t));
  uint32_t uid = 0;
  if (!getUserIdByUsername(String(username), uid)) return;
  PSRAM_JSON_DOC(doc);
  if (!loadUserSettings(uid, doc)) return;
  JsonArrayConst arr = doc["notificationMuted"].as<JsonArrayConst>();
  if (arr.isNull()) return;
  for (JsonVariantConst v : arr) {
    const char* n = v.as<const char*>();
    if (!n) continue;
    int k = systemEventKindFromName(n);
    if (k > 0 && k < 128) out[k >> 5] |= (1UL << (k & 31));
  }
}

void notifViewerResolve(const char* username, NotifViewer& out) {
  memset(out.muteMask, 0, sizeof(out.muteMask));
  out.known = false;
  out.isAdmin = false;
  if (!username || !username[0]) return;  // anonymous surface
  out.known = true;
  out.isAdmin = isAdminUser(String(username));  // live — roles change mid-session

  if (gUserPrefsMutex) xSemaphoreTake(gUserPrefsMutex, portMAX_DELAY);
  for (auto& e : gUserPrefsCache) {
    if (e.valid && e.username.equals(username)) {
      memcpy(out.muteMask, e.muteMask, sizeof(out.muteMask));
      if (gUserPrefsMutex) xSemaphoreGive(gUserPrefsMutex);
      return;
    }
  }
  if (gUserPrefsMutex) xSemaphoreGive(gUserPrefsMutex);

  // Miss: load outside the lock (flash read), then publish.
  uint32_t mask[4];
  loadUserMuteMask(username, mask);
  if (gUserPrefsMutex) xSemaphoreTake(gUserPrefsMutex, portMAX_DELAY);
  UserPrefsCacheEntry& slot = gUserPrefsCache[gUserPrefsCacheNext];
  gUserPrefsCacheNext = (uint8_t)((gUserPrefsCacheNext + 1) % 4);
  slot.username = username;
  memcpy(slot.muteMask, mask, sizeof(slot.muteMask));
  slot.valid = true;
  if (gUserPrefsMutex) xSemaphoreGive(gUserPrefsMutex);
  memcpy(out.muteMask, mask, sizeof(out.muteMask));
}

// Device layers only: compiled default, then off-level, then sink masters.
// Admin-level kinds PASS here (an admin viewer may exist) — the tick uses
// this to early-out before resolving any viewer.
static NotifRule notifDeviceRuleFor(uint8_t kind) {
  NotifRule r = notifDefaultRuleFor(kind);
  if (r.sinks == NSINK_NONE) return r;
  if (maskTest(gNotifOffMask, kind)) { r.sinks = NSINK_NONE; return r; }
  if (!gSettings.notifBanners) r.sinks &= (uint8_t)~NSINK_BANNER;
  if (!gSettings.notifToasts)  r.sinks &= (uint8_t)~NSINK_TOAST;
  if (!gSettings.notifQueue)   r.sinks &= (uint8_t)~NSINK_QUEUE;
  return r;
}

NotifRule notifRuleForViewer(uint8_t kind, const NotifViewer& v) {
  NotifRule r = notifDeviceRuleFor(kind);
  if (r.sinks == NSINK_NONE) return r;
  if (maskTest(gNotifAdminMask, kind) && !v.isAdmin) { r.sinks = NSINK_NONE; return r; }
  if (v.known && maskTest(v.muteMask, kind)) r.sinks = NSINK_NONE;
  return r;
}

// ============================================================================
// Settings module + notifydevicekind / notifyusermute commands
// ============================================================================
static const SettingEntry notifSettingEntries[] = {
  { "notifBanners", SETTING_BOOL, &gSettings.notifBanners, 1, 0, nullptr, 0, 1, "OLED banners", nullptr, false, nullptr, "notifydevicebanners" },
  { "notifToasts",  SETTING_BOOL, &gSettings.notifToasts,  1, 0, nullptr, 0, 1, "Web toasts", nullptr, false, nullptr, "notifydevicetoasts" },
  { "notifQueue",   SETTING_BOOL, &gSettings.notifQueue,   1, 0, nullptr, 0, 1, "Notification center", nullptr, false, nullptr, "notifydevicequeue" },
};
extern const SettingsModule notifSettingsModule = {
  "notifications", "system.notifications", notifSettingEntries,
  sizeof(notifSettingEntries) / sizeof(notifSettingEntries[0]),
  nullptr, "Notification banners, toasts, and per-event-kind muting"
};

// notifydevicekind — admin: device-wide per-kind visibility level.
const char* cmd_notifydevicekind(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsInput;
  args.trim();

  // Bare / "list" / "list json": show levels. Bare shows only non-default
  // kinds; list shows every kind; json is the machine form the web uses.
  if (args.length() == 0 || args.equalsIgnoreCase("list") || args.equalsIgnoreCase("list json")) {
    bool all = args.length() > 0;
    if (args.indexOf("json") >= 0) {
      String out;
      out.reserve(SYSEVT_COUNT * 28);
      out = "{\"kinds\":[";
      for (int k = SYSEVT_NONE + 1; k < SYSEVT_COUNT; k++) {
        if (k > SYSEVT_NONE + 1) out += ',';
        out += "{\"n\":\"";
        out += systemEventKindName((uint8_t)k);
        out += "\",\"l\":\"";
        out += maskTest(gNotifOffMask, (uint8_t)k) ? "off"
             : maskTest(gNotifAdminMask, (uint8_t)k) ? "admin" : "all";
        out += "\"}";
      }
      out += "]}";
      broadcastOutput(out);
      return "OK";
    }
    int shown = 0;
    for (int k = SYSEVT_NONE + 1; k < SYSEVT_COUNT; k++) {
      bool off = maskTest(gNotifOffMask, (uint8_t)k);
      bool adm = maskTest(gNotifAdminMask, (uint8_t)k);
      if (!all && !off && !adm) continue;
      BROADCAST_PRINTF("  %-22s %s", systemEventKindName((uint8_t)k),
                       off ? "off" : adm ? "admin" : "all");
      shown++;
    }
    if (shown == 0) broadcastOutput("  (every kind at level 'all')");
    if (!all) cliHint("'notifydevicekind list' shows every kind - set one with 'notifydevicekind <kind> <all|admin|off>'");
    return "OK";
  }

  // "<kind>" shows one level; "<kind> <level>" sets it.
  int sp = args.indexOf(' ');
  String kindName = (sp < 0) ? args : args.substring(0, sp);
  String level = (sp < 0) ? String() : args.substring(sp + 1);
  kindName.trim();
  kindName.toLowerCase();
  level.trim();
  level.toLowerCase();

  int k = systemEventKindFromName(kindName.c_str());
  if (k <= 0 || k >= SYSEVT_COUNT) {
    static char err[80];
    snprintf(err, sizeof(err), "Error: unknown event kind '%.40s'", kindName.c_str());
    cliHint("list valid kinds with 'events kinds'");
    return err;
  }
  if (level.length() == 0) {
    BROADCAST_PRINTF("%s = %s", kindName.c_str(),
                     maskTest(gNotifOffMask, (uint8_t)k) ? "off"
                   : maskTest(gNotifAdminMask, (uint8_t)k) ? "admin" : "all");
    return "OK";
  }

  bool off, adm;
  if (level == "all")        { off = false; adm = false; }
  else if (level == "admin") { off = false; adm = true; }
  else if (level == "off")   { off = true;  adm = false; }
  else return "Error: level must be all, admin, or off";

  maskSet(gNotifOffMask, (uint8_t)k, off);
  maskSet(gNotifAdminMask, (uint8_t)k, adm);
  gNotifPrefsGen++;
  if (!notifPolicySave()) return "Error: failed to write " NOTIF_POLICY_FILE;
  systemEventPost(SYSEVT_SETTING_CHANGED, kindName.c_str(), level.c_str());
  BROADCAST_PRINTF("%s = %s", kindName.c_str(), level.c_str());
  return "[Settings] Configuration updated";
}

// notifyusermute — personal mute list for the EXECUTING user (per-task identity),
// stored as a JSON array in the user's settings file next to the dashboard
// layout prefs. Bare = show, 'none' = clear, else a validated kind list.
const char* cmd_notifyusermute(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const String& user = currentExecUser();
  uint32_t uid = 0;
  if (user.length() == 0 || !getUserIdByUsername(user, uid)) {
    return "Error: no logged-in user - personal mutes need a user identity";
  }

  String args = argsInput;
  args.trim();

  if (args.length() == 0) {
    PSRAM_JSON_DOC(doc);
    String cur;
    if (loadUserSettings(uid, doc)) {
      JsonArrayConst arr = doc["notificationMuted"].as<JsonArrayConst>();
      if (!arr.isNull()) {
        for (JsonVariantConst v : arr) {
          const char* n = v.as<const char*>();
          if (!n) continue;
          if (cur.length()) cur += ", ";
          cur += n;
        }
      }
    }
    BROADCAST_PRINTF("%s's muted kinds: %s", user.c_str(), cur.length() ? cur.c_str() : "(none)");
    cliHint("mute with 'notifyusermute <kind,kind,...>', clear with 'notifyusermute none' - list valid kinds with 'events kinds'");
    return "OK";
  }

  PSRAM_JSON_DOC(patch);
  JsonArray arr = patch["notificationMuted"].to<JsonArray>();
  if (!args.equalsIgnoreCase("none")) {
    bool seen[SYSEVT_COUNT] = {false};
    int start = 0;
    while (start <= (int)args.length()) {
      int comma = args.indexOf(',', start);
      String tok = (comma < 0) ? args.substring(start) : args.substring(start, comma);
      tok.trim();
      tok.toLowerCase();
      if (tok.length() > 0) {
        int k = systemEventKindFromName(tok.c_str());
        if (k <= 0 || k >= SYSEVT_COUNT) {
          static char err[80];
          snprintf(err, sizeof(err), "Error: unknown event kind '%.40s'", tok.c_str());
          cliHint("list valid kinds with 'events kinds'");
          return err;
        }
        // Store the canonical name (static X-macro string) — dedup via bitmap.
        if (!seen[k]) {
          seen[k] = true;
          arr.add(systemEventKindName((uint8_t)k));
        }
      }
      if (comma < 0) break;
      start = comma + 1;
    }
    if (arr.size() == 0) return "Error: no event kinds given";
  }

  if (!mergeAndSaveUserSettings(uid, patch)) return "Error: failed to save user settings";
  // saveUserSettings() flushed the prefs cache via notifUserPrefsInvalidate().
  systemEventPost(SYSEVT_SETTING_CHANGED, "notificationMuted", user.c_str());
  BROADCAST_PRINTF("%s's muted kinds updated (%d muted)", user.c_str(), (int)arr.size());
  return "[Settings] Configuration updated";
}

// ============================================================================
// Message formatter — one place instead of ~21 snprintf bodies
// ============================================================================

void notifFormatEvent(const SystemEvent& e, char* out, size_t outLen) {
  switch (e.kind) {
    case SYSEVT_PEER_ONLINE:  snprintf(out, outLen, "Peer online: %s", e.subject); return;
    case SYSEVT_PEER_OFFLINE: snprintf(out, outLen, "Peer offline: %s", e.subject); return;
    case SYSEVT_PEER_PAIRED:  snprintf(out, outLen, "Paired: %s", e.subject); return;
    case SYSEVT_TEXT_RX:      snprintf(out, outLen, "Msg from %s", e.subject); return;
    case SYSEVT_FILE_RX:      snprintf(out, outLen, "File from %s: %s", e.subject, e.detail); return;
    case SYSEVT_BOND_ONLINE:  snprintf(out, outLen, "Bond online: %s", e.subject); return;
    case SYSEVT_BOND_OFFLINE: snprintf(out, outLen, "Bond offline: %s", e.subject); return;
    case SYSEVT_BLE_CONNECTED:    snprintf(out, outLen, "BLE: %s", e.subject[0] ? e.subject : "connected"); return;
    case SYSEVT_BLE_DISCONNECTED: snprintf(out, outLen, "BLE device off"); return;
    case SYSEVT_WIFI_CONNECTED:    snprintf(out, outLen, "WiFi: %s", e.subject[0] ? e.subject : "connected"); return;
    case SYSEVT_WIFI_DISCONNECTED: snprintf(out, outLen, "WiFi off"); return;
    case SYSEVT_WIFI_NET_ADDED:    snprintf(out, outLen, "WiFi saved: %s", e.subject[0] ? e.subject : "network"); return;
    case SYSEVT_WIFI_NET_REMOVED:  snprintf(out, outLen, "WiFi removed: %s", e.subject[0] ? e.subject : "network"); return;
    case SYSEVT_ESPNOW_ON:  snprintf(out, outLen, "ESP-NOW: on"); return;
    case SYSEVT_ESPNOW_OFF: snprintf(out, outLen, "ESP-NOW: off"); return;
    case SYSEVT_USB_ON:  snprintf(out, outLen, "USB connected"); return;
    case SYSEVT_USB_OFF: snprintf(out, outLen, "USB disconnected"); return;
    case SYSEVT_BATTERY_LOW:      snprintf(out, outLen, "Batt low: %s%%", e.subject); return;
    case SYSEVT_BATTERY_CRITICAL: snprintf(out, outLen, "Battery: %s%%!", e.subject); return;
    case SYSEVT_LOGIN_OK:   snprintf(out, outLen, "Login: %s", e.subject[0] ? e.subject : "user"); return;
    case SYSEVT_LOGIN_FAIL: snprintf(out, outLen, "Login failed: %s", e.subject[0] ? e.subject : "user"); return;
    case SYSEVT_SETTING_CHANGED: snprintf(out, outLen, "Set: %s=%s", e.subject, e.detail); return;
    case SYSEVT_SENSOR_STARTED:      snprintf(out, outLen, "%s: started", e.subject[0] ? e.subject : "Sensor"); return;
    case SYSEVT_SENSOR_STOPPED:      snprintf(out, outLen, "%s: stopped", e.subject[0] ? e.subject : "Sensor"); return;
    case SYSEVT_SENSOR_START_FAILED: snprintf(out, outLen, "%s: failed", e.subject[0] ? e.subject : "Sensor"); return;
    case SYSEVT_FILE_DELETED: snprintf(out, outLen, "Deleted: %s", e.subject[0] ? e.subject : "file"); return;
    case SYSEVT_VOICE_WAKE:    snprintf(out, outLen, "Listening..."); return;
    case SYSEVT_VOICE_COMMAND: snprintf(out, outLen, "Voice: %s", e.subject[0] ? e.subject : "cmd"); return;
    default:
      // Generic fallback for any kind promoted to a visible sink later.
      if (e.subject[0]) snprintf(out, outLen, "%s: %s", systemEventKindName(e.kind), e.subject);
      else snprintf(out, outLen, "%s", systemEventKindName(e.kind));
      return;
  }
}

// ============================================================================
// Main-loop renderer
// ============================================================================

static const char* levelName(uint8_t level) {
  switch (level) {
    case 1: return "success";
    case 2: return "warning";
    case 3: return "error";
    default: return "info";
  }
}

#if ENABLE_OLED_DISPLAY
static PairingRibbonIcon levelIcon(uint8_t level) {
  switch (level) {
    case 1: return PairingRibbonIcon::SUCCESS;
    case 2: return PairingRibbonIcon::WARNING_ICON;
    case 3: return PairingRibbonIcon::ERROR_ICON;
    default: return PairingRibbonIcon::INFO_ICON;
  }
}
#endif

// ============================================================================
// Pipeline diagnostics (DEBUG_NOTIFICATIONS + `notifstats`)
// ============================================================================
// Composes the shared primitives (everyMs/observeHwm/saturationLabel from
// System_Utils.h) per the System_ESPNow_Tx recipe: plain counters + HWMs,
// best-effort under concurrency (same tolerance as gDebugDropped). The three
// TRUE-LOSS counters (ringSkipped / staleDropped / SSE drops) are reported
// separately from benign suppression (cooldown) and INTENTIONAL filtering
// (device policy / per-user mutes) so filtering is never read as a fault.
struct NotifPipeStats {
  uint32_t fetched;          // events the tick pulled from the ring
  uint32_t bannersShown;
  uint32_t bannersFiltered;  // viewer rule denied the banner (policy/mute)
  uint32_t toastsBroadcast;  // events offered to the SSE layer
  uint32_t toastsFiltered;   // per-session denials by the toast predicate
  uint32_t staleDropped;     // waited >10s in the ring; transient render skipped
  uint32_t cooldownSupp;     // suppressed by per-kind cooldown
  uint32_t ringSkipped;      // ring overwrote events before the tick read them
  uint32_t ringLagHwm;       // worst backlog drained in one tick (48 = the cliff)
  uint32_t drainMaxUs;       // slowest full drain
};
static NotifPipeStats gNotifPipe = {};

// Per-session toast predicate for broadcastEventToSessionsIf: resolve the
// session's user as a viewer and check the TOAST bit. Session count is tiny
// (MAX_SESSIONS) and visible events are rare, so per-call resolution is
// fine — the per-user mask is cached; only the admin check reads flash.
static bool notifToastAllowedFor(const char* username, void* arg) {
  NotifViewer v;
  notifViewerResolve(username, v);
  if (notifRuleForViewer((uint8_t)(uintptr_t)arg, v).sinks & NSINK_TOAST) return true;
  gNotifPipe.toastsFiltered++;
  return false;
}

// `notifstats` — on-demand snapshot of the same counters the periodic [NOTIF]
// line reports (registered in the debug command table, System_Debug.cpp).
const char* cmd_notifstats(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsInput;
  args.trim();
  if (args.equalsIgnoreCase("reset")) {
    memset(&gNotifPipe, 0, sizeof(gNotifPipe));
    return "OK: notification pipeline counters reset";
  }
  BROADCAST_PRINTF("OK: notification pipeline (since boot or last reset)");
  BROADCAST_PRINTF("  Ring: %d deep | tick backlog hwm %lu (%s) | skipped: tick=%lu all-consumers=%lu",
                   SYSEVT_RING_SIZE,
                   (unsigned long)gNotifPipe.ringLagHwm,
                   saturationLabel(gNotifPipe.ringLagHwm, SYSEVT_RING_SIZE),
                   (unsigned long)gNotifPipe.ringSkipped,
                   (unsigned long)systemEventRingSkippedTotal());
  BROADCAST_PRINTF("  Tick: fetched=%lu, slowest drain %lu us",
                   (unsigned long)gNotifPipe.fetched, (unsigned long)gNotifPipe.drainMaxUs);
  BROADCAST_PRINTF("  Rendered: banners=%lu toasts=%lu (toast events offered to web sessions)",
                   (unsigned long)gNotifPipe.bannersShown, (unsigned long)gNotifPipe.toastsBroadcast);
  BROADCAST_PRINTF("  LOSS: ring_skip=%lu stale=%lu sse_drop=%lu (sse = current sessions)",
                   (unsigned long)gNotifPipe.ringSkipped,
                   (unsigned long)gNotifPipe.staleDropped,
                   (unsigned long)sseEventDropsTotal());
  BROADCAST_PRINTF("  Suppressed (benign): cooldown=%lu | Filtered (policy/mutes, intentional): banner=%lu toast-sessions=%lu",
                   (unsigned long)gNotifPipe.cooldownSupp,
                   (unsigned long)gNotifPipe.bannersFiltered,
                   (unsigned long)gNotifPipe.toastsFiltered);
  cliHint("watch these live with 'debugnotifications 1' (one [NOTIF] line per 10s); zero them with 'notifstats reset'");
  return "OK";
}

void systemEventsNotifyTick() {
  // Cursor starts at 0 (not "from now"): boot-time events replay on the
  // first pass so they land in consumers' view of history; the 10s staleness
  // gate below keeps them from toasting late. The queue view reads the ring
  // directly and is unaffected by this cursor.
  static uint32_t sCursor = 0;
  // Per-kind last-render stamp for cooldowns (0 = never rendered).
  static uint32_t sKindLastShownMs[SYSEVT_COUNT] = {};

  SystemEvent evs[8];
  int n;
  // The OLED banner's viewer is whoever is logged in on the local display
  // (anonymous when nobody is — admin-level kinds then stay hidden).
  // Resolved lazily once per tick, only if a bannerable event shows up.
  bool oledViewerResolved = false;
  NotifViewer oledViewer;
  // Diagnostics: timing starts lazily on the first non-empty fetch, so idle
  // main-loop laps (the overwhelmingly common case) pay nothing.
  uint32_t drainedThisTick = 0;
  int64_t drainStartUs = 0;
  while ((n = systemEventFetchSince(&sCursor, evs, 8, &gNotifPipe.ringSkipped)) > 0) {
    if (drainedThisTick == 0) drainStartUs = esp_timer_get_time();
    drainedThisTick += (uint32_t)n;
    gNotifPipe.fetched += (uint32_t)n;
    uint32_t nowMs = millis();
    for (int i = 0; i < n; i++) {
      const SystemEvent& e = evs[i];
      // Device layers only (default + off-level + masters) — cheap early-out
      // before any viewer resolution. Admin-level kinds pass through here.
      NotifRule r = notifDeviceRuleFor(e.kind);
      if (!(r.sinks & (NSINK_BANNER | NSINK_TOAST))) continue;
      if (nowMs - e.tsMs > 10000UL) {  // stale — transient sinks only
        gNotifPipe.staleDropped++;
        continue;
      }
      if (r.cooldownMs) {
        uint32_t last = sKindLastShownMs[e.kind];
        if (last != 0 && (nowMs - last) < r.cooldownMs) {
          gNotifPipe.cooldownSupp++;
          continue;
        }
        sKindLastShownMs[e.kind] = nowMs ? nowMs : 1;
      }

      char msg[64];
      notifFormatEvent(e, msg, sizeof(msg));

      #if ENABLE_OLED_DISPLAY
      if (r.sinks & NSINK_BANNER) {
        if (!oledViewerResolved) {
          notifViewerResolve(gLocalDisplayAuthed ? gLocalDisplayUser.c_str() : "", oledViewer);
          oledViewerResolved = true;
        }
        if (notifRuleForViewer(e.kind, oledViewer).sinks & NSINK_BANNER) {
          oledNotificationBannerShow(msg, levelIcon(r.level), r.durMs, r.level >= 3);
          gNotifPipe.bannersShown++;
        } else {
          gNotifPipe.bannersFiltered++;
        }
      }
      #endif

      if (r.sinks & NSINK_TOAST) {
        char json[128];
        snprintf(json, sizeof(json), "{\"level\":\"%s\",\"msg\":\"%s\",\"ms\":%u}",
                 levelName(r.level), msg, (unsigned)r.durMs);
        // Per-session delivery: each web session's viewer decides.
        broadcastEventToSessionsIf("notification", json, notifToastAllowedFor,
                                   (void*)(uintptr_t)e.kind);
        gNotifPipe.toastsBroadcast++;
      }
    }
    if (n < 8) break;  // ring drained
  }
  if (drainedThisTick > 0) {
    // Backlog drained in one tick == how far behind the cursor was at entry;
    // approaching SYSEVT_RING_SIZE means the next stall starts losing events.
    observeHwm(&gNotifPipe.ringLagHwm, drainedThisTick);
    observeHwm(&gNotifPipe.drainMaxUs,
               (uint32_t)(esp_timer_get_time() - drainStartUs));
  }

  // Periodic one-liner under the flag. The everyMs stamp stays zero while the
  // flag is off, so the first line prints immediately on enable.
  if (isDebugFlagSet(DEBUG_NOTIFICATIONS)) {
    static uint32_t sReportMs = 0;
    if (everyMs(&sReportMs, 10000)) {
      DEBUG_NOTIFICATIONSF(
          "[NOTIF] fetched=%lu banner=%lu toast=%lu | LOSS ring_skip=%lu stale=%lu sse_drop=%lu"
          " | supp cooldown=%lu filtered=b%lu/t%lu | lag_hwm=%lu/%d (%s) drain_max=%luus",
          (unsigned long)gNotifPipe.fetched,
          (unsigned long)gNotifPipe.bannersShown,
          (unsigned long)gNotifPipe.toastsBroadcast,
          (unsigned long)gNotifPipe.ringSkipped,
          (unsigned long)gNotifPipe.staleDropped,
          (unsigned long)sseEventDropsTotal(),
          (unsigned long)gNotifPipe.cooldownSupp,
          (unsigned long)gNotifPipe.bannersFiltered,
          (unsigned long)gNotifPipe.toastsFiltered,
          (unsigned long)gNotifPipe.ringLagHwm, SYSEVT_RING_SIZE,
          saturationLabel(gNotifPipe.ringLagHwm, SYSEVT_RING_SIZE),
          (unsigned long)gNotifPipe.drainMaxUs);
    }
  }
}
