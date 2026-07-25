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
#include "System_CommandTypes.h"  // CMD_RESULT_MAX — json branch returns the document
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
#include "G2_Glasses.h"    // g2SendNativeNotificationAsync / isG2Connected — native-card sink
#include "G2_HijackCmd.h"  // g2HijackAuthContext — G2 lens viewer identity
#endif

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

// Read-side accessors for on-device config screens (the masks are file-static
// and cmd_notifydevicekind's show path prints via broadcast, so a renderer
// can't screen-scrape it). Level: 0=all, 1=admin, 2=off. No locking — same
// volatile word-read discipline as notifDeviceRuleFor. The token form is
// exactly the argument cmd_notifydevicekind accepts, so displayed text ==
// command argument (single vocabulary).
uint8_t notifDeviceKindLevel(uint8_t kind) {
  if (maskTest(gNotifOffMask, kind)) return 2;
  if (maskTest(gNotifAdminMask, kind)) return 1;
  return 0;
}

const char* notifLevelToken(uint8_t level) {
  return level == 2 ? "off" : level == 1 ? "admin" : "all";
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
  uint32_t forceMask[4];
  uint8_t  minTier;
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

// Turn a kind-name array under `key` into a 4x32 mask (bit index = kind).
static void loadKindMaskFromDoc(JsonDocument& doc, const char* key, uint32_t out[4]) {
  memset(out, 0, 4 * sizeof(uint32_t));
  JsonArrayConst arr = doc[key].as<JsonArrayConst>();
  if (arr.isNull()) return;
  for (JsonVariantConst v : arr) {
    const char* n = v.as<const char*>();
    if (!n) continue;
    int k = systemEventKindFromName(n);
    if (k > 0 && k < 128) out[k >> 5] |= (1UL << (k & 31));
  }
}

// Load a user's notification prefs: the force-off (notificationMuted) + force-on
// (notificationForced) kind masks and the importance floor (notifyLevel).
// Missing file / keys resolve to the defaults (nothing muted/forced, floor =
// NTIER_DEFAULT). One flash read serves all three.
static void loadUserNotifPrefs(const char* username, uint32_t mute[4],
                               uint32_t force[4], uint8_t* minTier) {
  memset(mute, 0, 4 * sizeof(uint32_t));
  memset(force, 0, 4 * sizeof(uint32_t));
  *minTier = NTIER_DEFAULT;
  uint32_t uid = 0;
  if (!getUserIdByUsername(String(username), uid)) return;
  PSRAM_JSON_DOC(doc);
  if (!loadUserSettings(uid, doc)) return;
  loadKindMaskFromDoc(doc, "notificationMuted", mute);
  loadKindMaskFromDoc(doc, "notificationForced", force);
  if (doc["notifyLevel"].is<int>()) {
    int lv = doc["notifyLevel"].as<int>();
    if (lv >= (int)NTIER_VERBOSE && lv <= (int)NTIER_ALERT) *minTier = (uint8_t)lv;
  }
}

void notifViewerResolve(const char* username, NotifViewer& out) {
  memset(out.muteMask, 0, sizeof(out.muteMask));
  memset(out.forceMask, 0, sizeof(out.forceMask));
  out.minTier = NTIER_DEFAULT;
  out.known = false;
  out.isAdmin = false;
  if (!username || !username[0]) return;  // anonymous surface (default floor)
  out.known = true;
  out.isAdmin = isAdminUser(String(username));  // live — roles change mid-session

  if (gUserPrefsMutex) xSemaphoreTake(gUserPrefsMutex, portMAX_DELAY);
  for (auto& e : gUserPrefsCache) {
    if (e.valid && e.username.equals(username)) {
      memcpy(out.muteMask, e.muteMask, sizeof(out.muteMask));
      memcpy(out.forceMask, e.forceMask, sizeof(out.forceMask));
      out.minTier = e.minTier;
      if (gUserPrefsMutex) xSemaphoreGive(gUserPrefsMutex);
      return;
    }
  }
  if (gUserPrefsMutex) xSemaphoreGive(gUserPrefsMutex);

  // Miss: load outside the lock (flash read), then publish.
  uint32_t mute[4], force[4];
  uint8_t minTier;
  loadUserNotifPrefs(username, mute, force, &minTier);
  if (gUserPrefsMutex) xSemaphoreTake(gUserPrefsMutex, portMAX_DELAY);
  UserPrefsCacheEntry& slot = gUserPrefsCache[gUserPrefsCacheNext];
  gUserPrefsCacheNext = (uint8_t)((gUserPrefsCacheNext + 1) % 4);
  slot.username = username;
  memcpy(slot.muteMask, mute, sizeof(slot.muteMask));
  memcpy(slot.forceMask, force, sizeof(slot.forceMask));
  slot.minTier = minTier;
  slot.valid = true;
  if (gUserPrefsMutex) xSemaphoreGive(gUserPrefsMutex);
  memcpy(out.muteMask, mute, sizeof(out.muteMask));
  memcpy(out.forceMask, force, sizeof(out.forceMask));
  out.minTier = minTier;
}

// Cross-cutting importance tier for a kind (NTIER_*) — orthogonal to family,
// and the axis the per-user floor tunes. Every kind that surfaces (has a sink
// in notifDefaultRuleFor) is tagged here; unlisted → NTIER_STANDARD. This is
// what keeps the everyday interrupt load small WITHOUT hard-limiting any one
// surface: raise your floor to cut noise, lower it (or force-on a kind) to see
// more — identically on OLED, web, and the glasses.
static uint8_t notifKindTier(uint8_t kind) {
  switch (kind) {
    // ALERT — security, safety, data-integrity faults. Always interrupts.
    case SYSEVT_BATTERY_CRITICAL:
    case SYSEVT_THERMAL_HOT_ALERT:
    case SYSEVT_LOGIN_FAIL:
    case SYSEVT_LOGIN_LOCKED:
    case SYSEVT_USER_BANNED:
    case SYSEVT_IP_BANNED:
    case SYSEVT_CRASH:
    case SYSEVT_STORAGE_FORMATTED:
    case SYSEVT_FACTORY_RESET:
    case SYSEVT_CONFIG_FILE_CORRUPT:
    case SYSEVT_AUTH_DB_FAULT:
    case SYSEVT_SECRET_DECRYPT_FAILED:
      return NTIER_ALERT;
    // VERBOSE — chatty/info; opt-in (stay in history unless you lower your floor
    // or force them on).
    case SYSEVT_PEER_PAIRED:
    case SYSEVT_WIFI_NET_ADDED:
    case SYSEVT_WIFI_NET_REMOVED:
    case SYSEVT_ESPNOW_ON:
    case SYSEVT_ESPNOW_OFF:
    case SYSEVT_USB_ON:
    case SYSEVT_USB_OFF:
    case SYSEVT_LOGIN_OK:
    case SYSEVT_SETTING_CHANGED:
    case SYSEVT_SENSOR_STARTED:
    case SYSEVT_SENSOR_STOPPED:
    case SYSEVT_FILE_DELETED:
    case SYSEVT_VOICE_WAKE:
    case SYSEVT_VOICE_COMMAND:
    case SYSEVT_BATTERY_FULL:
      return NTIER_VERBOSE;
    // STANDARD (default) — everything else that surfaces: presence, inbound,
    // connectivity, battery-low, service faults. The default user floor.
    default:
      return NTIER_STANDARD;
  }
}

// Device layers only: compiled default, then off-level, then sink masters.
// Admin-level kinds PASS here (an admin viewer may exist) — the tick uses
// this to early-out before resolving any viewer.
static NotifRule notifDeviceRuleFor(uint8_t kind) {
  NotifRule r = notifDefaultRuleFor(kind);
  if (r.sinks == NSINK_NONE) return r;
  // The G2 lens is an interrupt surface: grant it wherever the kind already
  // interrupts (OLED banner or web toast), so banner/toast/G2 fire on the same
  // kinds. The per-user importance floor (notifRuleForViewer) then curates all
  // three uniformly — nothing here hard-limits what the glasses may show.
  if (r.sinks & (NSINK_BANNER | NSINK_TOAST)) r.sinks |= NSINK_G2;
  if (maskTest(gNotifOffMask, kind)) { r.sinks = NSINK_NONE; return r; }
  if (!gSettings.notifBanners) r.sinks &= (uint8_t)~NSINK_BANNER;
  if (!gSettings.notifToasts)  r.sinks &= (uint8_t)~NSINK_TOAST;
  if (!gSettings.notifQueue)   r.sinks &= (uint8_t)~NSINK_QUEUE;
  if (!gSettings.notifG2)      r.sinks &= (uint8_t)~NSINK_G2;
  return r;
}

// The interrupt sinks enabled device-wide right now (sink masters applied).
// force-on grants these; a device that turned off a whole surface still wins.
static uint8_t notifDeviceInterruptSinks() {
  uint8_t s = NSINK_INTERRUPT;
  if (!gSettings.notifBanners) s &= (uint8_t)~NSINK_BANNER;
  if (!gSettings.notifToasts)  s &= (uint8_t)~NSINK_TOAST;
  if (!gSettings.notifG2)      s &= (uint8_t)~NSINK_G2;
  return s;
}

NotifRule notifRuleForViewer(uint8_t kind, const NotifViewer& v) {
  NotifRule r = notifDeviceRuleFor(kind);
  // Event-only kinds and device-"off" kinds (NONE here) can't be resurrected by
  // a user preference — force-on only promotes real NOTIFICATION kinds.
  if (r.sinks == NSINK_NONE) return r;
  if (maskTest(gNotifAdminMask, kind) && !v.isAdmin) { r.sinks = NSINK_NONE; return r; }
  // Force-off (personal mute): the viewer never wants this kind, on any surface.
  if (v.known && maskTest(v.muteMask, kind)) { r.sinks = NSINK_NONE; return r; }

  const uint8_t floor    = v.known ? v.minTier : NTIER_DEFAULT;
  const bool    forcedOn = v.known && maskTest(v.forceMask, kind);
  if (forcedOn) {
    // "Always show me this" — promote to every device-enabled interrupt surface,
    // even for kinds that are queue-only (history-only) by default. Overrides
    // both the tier floor and the compiled per-sink default.
    r.sinks |= notifDeviceInterruptSinks();
  } else if (notifKindTier(kind) < floor) {
    // Importance floor: interrupt surfaces fire only at/above the viewer's tier.
    // QUEUE (history) is never floor-gated — nothing is lost, it just doesn't pop.
    r.sinks &= (uint8_t)~NSINK_INTERRUPT;
  }
  return r;
}

// ============================================================================
// Settings module + notifydevicekind / notifyusermute commands
// ============================================================================
static const SettingEntry notifSettingEntries[] = {
  { "notifBanners", SETTING_BOOL, &gSettings.notifBanners, 1, 0, nullptr, 0, 1, "OLED banners", nullptr, false, nullptr, "notifydevicebanners" },
  { "notifToasts",  SETTING_BOOL, &gSettings.notifToasts,  1, 0, nullptr, 0, 1, "Web toasts", nullptr, false, nullptr, "notifydevicetoasts" },
  { "notifQueue",   SETTING_BOOL, &gSettings.notifQueue,   1, 0, nullptr, 0, 1, "Notification center", nullptr, false, nullptr, "notifydevicequeue" },
  { "notifG2",      SETTING_BOOL, &gSettings.notifG2,      1, 0, nullptr, 0, 1, "G2 lens cards", nullptr, false, nullptr, "notifydeviceg2" },
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
    String argsLower = args;
    argsLower.toLowerCase();  // the outer guard is case-insensitive; match it
    if (argWantsJson(argsLower)) {
      // SPARSE — non-default kinds only, byte-identical to what notifPolicySave()
      // writes to the policy file. "all" is the default, so a kind's ABSENCE
      // means "all"; the client merges this over the full vocabulary from
      // `events kinds json`. One shape for the file and the wire, not two.
      //
      // The old form listed every kind as {"n":..,"l":..} and reached 4362 B —
      // over CMD_RESULT_MAX, so it could not be delivered on ANY transport and
      // the settings page had been dead behind a bogus "admin login required".
      // Worst case here (every one of 134 kinds non-default) is ~3290 B; the
      // realistic case is a handful of entries.
      PSRAM_JSON_DOC(doc);
      JsonObject kinds = doc["kinds"].to<JsonObject>();
      for (int k = SYSEVT_NONE + 1; k < SYSEVT_COUNT; k++) {
        if (maskTest(gNotifOffMask, (uint8_t)k))        kinds[systemEventKindName((uint8_t)k)] = "off";
        else if (maskTest(gNotifAdminMask, (uint8_t)k)) kinds[systemEventKindName((uint8_t)k)] = "admin";
      }
      PSRAM_STATIC_BUF(jbuf, CMD_RESULT_MAX);
      size_t n = serializeJson(doc, jbuf, jbuf_SIZE);
      if (n == 0 || n >= jbuf_SIZE - 1) return "Error: notification policy outgrew the response buffer";
      return jbuf;
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
// Shared body for the per-user kind-list prefs. `key` is the user-settings JSON
// array (notificationMuted = force-off, notificationForced = force-on); `noun`
// labels the output; `hint` shows on the bare/list form. Both lists apply
// uniformly on every interface. Bare = show; "none" = clear; else set.
static const char* notifUserKindListCmd(const String& argsInput, const char* key,
                                        const char* noun, const char* hint) {
  const String& user = currentExecUser();
  uint32_t uid = 0;
  if (user.length() == 0 || !getUserIdByUsername(user, uid)) {
    return "Error: no logged-in user - personal notification prefs need a user identity";
  }
  String args = argsInput;
  args.trim();

  if (args.length() == 0) {
    PSRAM_JSON_DOC(doc);
    String cur;
    if (loadUserSettings(uid, doc)) {
      JsonArrayConst arr = doc[key].as<JsonArrayConst>();
      if (!arr.isNull()) {
        for (JsonVariantConst v : arr) {
          const char* n = v.as<const char*>();
          if (!n) continue;
          if (cur.length()) cur += ", ";
          cur += n;
        }
      }
    }
    BROADCAST_PRINTF("%s's %s kinds: %s", user.c_str(), noun, cur.length() ? cur.c_str() : "(none)");
    cliHint(hint);
    return "OK";
  }

  PSRAM_JSON_DOC(patch);
  JsonArray arr = patch[key].to<JsonArray>();
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
  systemEventPost(SYSEVT_SETTING_CHANGED, key, user.c_str());
  BROADCAST_PRINTF("%s's %s kinds updated (%d)", user.c_str(), noun, (int)arr.size());
  return "[Settings] Configuration updated";
}

// Force-OFF: kinds this user never wants notified, on any surface.
const char* cmd_notifyusermute(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return notifUserKindListCmd(argsInput, "notificationMuted", "muted",
      "mute with 'notifyusermute <kind,...>', clear with 'notifyusermute none' - list kinds with 'events kinds'");
}

// Force-ON: kinds this user always wants to interrupt them, even below their
// notifylevel floor (the complement of a mute).
const char* cmd_notifyusershow(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return notifUserKindListCmd(argsInput, "notificationForced", "always-show",
      "always-show with 'notifyusershow <kind,...>' (overrides your notifylevel), clear with 'notifyusershow none'");
}

// Per-user importance floor — the ONE knob that decides what interrupts you
// (banner/toast/G2), applied identically on every interface. Lower tiers still
// land in the notification-center history. Bare = show current.
const char* cmd_notifylevel(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const String& user = currentExecUser();
  uint32_t uid = 0;
  if (user.length() == 0 || !getUserIdByUsername(user, uid)) {
    return "Error: no logged-in user - notifylevel is per-user";
  }
  static const char* kNames[] = { "verbose", "standard", "alert" };
  String a = argsInput;
  a.trim();
  a.toLowerCase();

  if (a.length() == 0) {
    PSRAM_JSON_DOC(doc);
    uint8_t lv = NTIER_DEFAULT;
    if (loadUserSettings(uid, doc) && doc["notifyLevel"].is<int>()) {
      int v = doc["notifyLevel"].as<int>();
      if (v >= (int)NTIER_VERBOSE && v <= (int)NTIER_ALERT) lv = (uint8_t)v;
    }
    BROADCAST_PRINTF("%s's notification level: %s (interrupts at this tier and above; "
                     "lower tiers stay in history)", user.c_str(), kNames[lv]);
    cliHint("set with 'notifylevel <verbose|standard|alert>'; per-kind exceptions via notifyusershow / notifyusermute");
    return "OK";
  }

  int lv = -1;
  if (a == "verbose"  || a == "0") lv = NTIER_VERBOSE;
  else if (a == "standard" || a == "1") lv = NTIER_STANDARD;
  else if (a == "alert"    || a == "2") lv = NTIER_ALERT;
  else return "Error: level must be verbose | standard | alert";

  PSRAM_JSON_DOC(patch);
  patch["notifyLevel"] = lv;
  if (!mergeAndSaveUserSettings(uid, patch)) return "Error: failed to save user settings";
  systemEventPost(SYSEVT_SETTING_CHANGED, "notifyLevel", user.c_str());
  BROADCAST_PRINTF("%s's notification level set to %s", user.c_str(), kNames[lv]);
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
  uint32_t g2Pushed;         // native G2 cards enqueued to the lens worker
  uint32_t g2Filtered;       // G2 viewer rule denied the card (policy/mute)
  uint32_t g2Dropped;        // enqueue failed (worker queue full / disconnect race)
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
  BROADCAST_PRINTF("  Rendered: banners=%lu toasts=%lu (toast events offered to web sessions) g2-cards=%lu",
                   (unsigned long)gNotifPipe.bannersShown, (unsigned long)gNotifPipe.toastsBroadcast,
                   (unsigned long)gNotifPipe.g2Pushed);
  BROADCAST_PRINTF("  LOSS: ring_skip=%lu stale=%lu sse_drop=%lu (sse = current sessions)",
                   (unsigned long)gNotifPipe.ringSkipped,
                   (unsigned long)gNotifPipe.staleDropped,
                   (unsigned long)sseEventDropsTotal());
  BROADCAST_PRINTF("  Suppressed (benign): cooldown=%lu | Filtered (policy/mutes, intentional): banner=%lu toast-sessions=%lu g2=%lu | g2-drop(enqueue-fail)=%lu",
                   (unsigned long)gNotifPipe.cooldownSupp,
                   (unsigned long)gNotifPipe.bannersFiltered,
                   (unsigned long)gNotifPipe.toastsFiltered,
                   (unsigned long)gNotifPipe.g2Filtered,
                   (unsigned long)gNotifPipe.g2Dropped);
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
  EXT_RAM_BSS_ATTR static uint32_t sKindLastShownMs[SYSEVT_COUNT];

  SystemEvent evs[8];
  int n;
  // The OLED banner's viewer is whoever is logged in on the local display
  // (anonymous when nobody is — admin-level kinds then stay hidden).
  // Resolved lazily once per tick, only if a bannerable event shows up.
  bool oledViewerResolved = false;
  NotifViewer oledViewer;
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
  // The G2 sink's viewer is the glasses' paired owner (g2HijackAuthContext),
  // resolved lazily once per tick like the OLED viewer. isG2Connected() is a
  // cheap flag read, taken once so absent glasses cost nothing per event.
  const bool g2Connected = isG2Connected();
  bool g2ViewerResolved = false;
  NotifViewer g2Viewer;
#endif
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
      // Any NOTIFICATION kind (≥1 sink) proceeds — a user's force-on can promote
      // even a queue-only kind to an interrupt surface below. Pure event-only /
      // device-"off" kinds (NONE) are skipped cheaply here.
      if (r.sinks == NSINK_NONE) continue;
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

      // Each interrupt surface is decided by ITS viewer's effective rule
      // (notifRuleForViewer = device policy + tier floor + per-user force/mute),
      // NOT the device rule — so force-on works even where the compiled default
      // grants no interrupt sink. Viewers resolve lazily, once per tick.
      #if ENABLE_OLED_DISPLAY
      {
        if (!oledViewerResolved) {
          notifViewerResolve(gLocalDisplayAuthed ? gLocalDisplayUser.c_str() : "", oledViewer);
          oledViewerResolved = true;
        }
        if (notifRuleForViewer(e.kind, oledViewer).sinks & NSINK_BANNER) {
          oledNotificationBannerShow(msg, levelIcon(r.level), r.durMs, r.level >= 3);
          gNotifPipe.bannersShown++;
        } else if (r.sinks & NSINK_BANNER) {
          gNotifPipe.bannersFiltered++;  // device offered a banner; viewer denied
        }
      }
      #endif

      // Web toasts: broadcast whenever the surface is enabled device-wide and
      // let each session's viewer decide (the predicate applies floor/force/mute).
      if (gSettings.notifToasts) {
        char json[128];
        snprintf(json, sizeof(json), "{\"level\":\"%s\",\"msg\":\"%s\",\"ms\":%u}",
                 levelName(r.level), msg, (unsigned)r.durMs);
        broadcastEventToSessionsIf("notification", json, notifToastAllowedFor,
                                   (void*)(uintptr_t)e.kind);
        gNotifPipe.toastsBroadcast++;
      }

      #if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
      if (g2Connected) {
        // Viewer = the glasses' paired owner, so admin gating + that user's
        // floor/force/mute decide the lens exactly like the OLED/web sinks.
        if (!g2ViewerResolved) {
          const AuthContext g2ctx = g2HijackAuthContext();
          notifViewerResolve(g2ctx.user.c_str(), g2Viewer);
          g2ViewerResolved = true;
        }
        if (notifRuleForViewer(e.kind, g2Viewer).sinks & NSINK_G2) {
          // Title = event family (grouping); body = the shared one-liner.
          // Fire-and-forget enqueue onto the lens worker — never sends BLE here.
          const char* title = systemEventFamilyName(systemEventKindFamily(e.kind));
          if (g2SendNativeNotificationAsync("one.hardware", "HardwareOne",
                                            title, "", msg)) {
            gNotifPipe.g2Pushed++;
          } else {
            gNotifPipe.g2Dropped++;
          }
        } else if (r.sinks & NSINK_G2) {
          gNotifPipe.g2Filtered++;  // device offered G2; viewer denied
        }
      }
      #endif
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
          "[NOTIF] fetched=%lu banner=%lu toast=%lu g2=%lu | LOSS ring_skip=%lu stale=%lu sse_drop=%lu"
          " | supp cooldown=%lu filtered=b%lu/t%lu/g%lu g2_drop=%lu | lag_hwm=%lu/%d (%s) drain_max=%luus",
          (unsigned long)gNotifPipe.fetched,
          (unsigned long)gNotifPipe.bannersShown,
          (unsigned long)gNotifPipe.toastsBroadcast,
          (unsigned long)gNotifPipe.g2Pushed,
          (unsigned long)gNotifPipe.ringSkipped,
          (unsigned long)gNotifPipe.staleDropped,
          (unsigned long)sseEventDropsTotal(),
          (unsigned long)gNotifPipe.cooldownSupp,
          (unsigned long)gNotifPipe.bannersFiltered,
          (unsigned long)gNotifPipe.toastsFiltered,
          (unsigned long)gNotifPipe.g2Filtered,
          (unsigned long)gNotifPipe.g2Dropped,
          (unsigned long)gNotifPipe.ringLagHwm, SYSEVT_RING_SIZE,
          saturationLabel(gNotifPipe.ringLagHwm, SYSEVT_RING_SIZE),
          (unsigned long)gNotifPipe.drainMaxUs);
    }
  }
}
