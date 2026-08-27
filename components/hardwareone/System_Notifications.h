#ifndef SYSTEM_NOTIFICATIONS_H
#define SYSTEM_NOTIFICATIONS_H

/**
 * System Notifications - the human-facing view over the event register.
 *
 * Phase-1 cutover (2026-07-13): the old notify*() entry-point layer is gone.
 * Subsystems post events (systemEventPost, System_Events.h) and this module
 * renders the ones whose per-kind rule says so — OLED banner and web toast
 * from the main-loop tick below, and the persistent notification-center view
 * (OLED bell/list, future web panel) derives straight from the ring via the
 * rule helpers here. One rules table replaces ~21 hand-rolled fan-out
 * bodies; per-kind cooldowns replace the old scattered statics.
 *
 * Source attribution (NotificationSource, setNotificationContext,
 * NotificationContextGuard) now lives in System_Events.h — included here so
 * existing includers keep compiling unchanged.
 */

#include "System_Events.h"

// ============================================================================
// Per-kind notification rules
// ============================================================================

// Sink bits for a kind's rule row.
enum : uint8_t {
  NSINK_NONE   = 0,
  NSINK_BANNER = 1 << 0,  // transient OLED banner/ribbon
  NSINK_QUEUE  = 1 << 1,  // persistent notification-center view (ring-backed)
  NSINK_TOAST  = 1 << 2,  // web toast via SSE
  NSINK_G2     = 1 << 3,  // native notification card on the G2 lens (EFS push)
  NSINK_APP    = 1 << 4,  // Android companion app, over the BLE command link
};

// The "interrupt" surfaces — transient pop-ups that grab attention. The per-user
// importance floor gates these four UNIFORMLY (banner/toast/G2/app fire on the
// same kinds for a given user). QUEUE is silent history and is never floor-gated.
constexpr uint8_t NSINK_INTERRUPT = NSINK_BANNER | NSINK_TOAST | NSINK_G2 | NSINK_APP;

// Cross-cutting importance tiers — orthogonal to event family, and the axis
// users actually tune. A per-user "minimum tier" (NotifViewer.minTier) decides
// which interrupt surfaces fire, identically across every interface. Tag a
// kind's tier in notifKindTier(); the default floor keeps the everyday load
// small without hard-coding what any one surface (e.g. the glasses) may show.
enum : uint8_t {
  NTIER_VERBOSE  = 0,  // chatty/info — opt-in (setting changes, sensor start/stop, gestures)
  NTIER_STANDARD = 1,  // genuinely useful — the default floor (presence, inbound, wifi, battery low)
  NTIER_ALERT    = 2,  // must-know — security, safety, faults
};
constexpr uint8_t NTIER_DEFAULT = NTIER_STANDARD;

struct NotifRule {
  uint8_t sinks;       // NSINK_* mask
  uint8_t level;       // 0=info 1=success 2=warning 3=error
  uint16_t durMs;      // banner/toast duration
  uint16_t cooldownMs; // min gap between renders of this kind (0 = none)
};

// ============================================================================
// Viewer-aware rule resolution
// ============================================================================
// A rule stacks four layers, most global first:
//   1. compiled default   — which sinks a kind renders to at all
//   2. device policy      — per-kind level from /system/notifications.json:
//                           all (default) / admin (admin viewers only) /
//                           off (hidden for everyone)
//   3. sink masters       — gSettings.notifBanners/notifToasts/notifQueue/
//                           notifG2/notifApp
//   4. personal prefs     — the viewer's importance floor (interrupt only
//                           at/above minTier) plus per-kind force-on/force-off,
//                           from their per-user settings file. This one
//                           preference applies IDENTICALLY on every interface.
// None of it touches the event ring or automations — display only.

// Width of every per-kind bitmask in this module (per-user mute/force AND the
// device-wide off/admin policy masks). Kinds at or past NOTIF_KIND_MASK_BITS
// are silently unmaskable — they can be neither muted nor forced — so this
// MUST stay ahead of SYSEVT_COUNT.
//
// It didn't. The masks were 4 words / 128 bits while the catalog grew past
// 140, so the tail of the catalog (everything from SYSEVT_FILE_RX_FAILED
// on: hijack enter/exit, thermal alert, ToF, FM, IMU walking, voice disarm,
// automation CRUD, ...) quietly ignored every mute and force request. Nothing
// failed loudly; those kinds just weren't tunable. 8 words matches the
// automation subscription mask's 256 bits and leaves the same headroom.
//
// The static_assert below is the point: adding kinds past the width is now a
// build error instead of a silent behavioural hole.
#define NOTIF_KIND_MASK_WORDS 8
#define NOTIF_KIND_MASK_BITS  (NOTIF_KIND_MASK_WORDS * 32)
static_assert(SYSEVT_COUNT <= NOTIF_KIND_MASK_BITS,
              "System Event catalog outgrew the notification per-kind masks — "
              "raise NOTIF_KIND_MASK_WORDS");

struct NotifViewer {
  bool known;            // false = anonymous surface (no login): non-admin view
  bool isAdmin;
  uint8_t minTier;       // personal importance floor (NTIER_*); default NTIER_DEFAULT
  uint32_t muteMask[NOTIF_KIND_MASK_WORDS];  // force-OFF: kinds this viewer never wants (any sink)
  uint32_t forceMask[NOTIF_KIND_MASK_WORDS]; // force-ON: interrupt even for kinds below minTier
};

// Resolve a viewer once per render pass. Per-user prefs come from a small
// cache over the user-settings files; the admin check is live (roles can
// change mid-session). username may be nullptr/"" for anonymous.
void notifViewerResolve(const char* username, NotifViewer& out);

// Effective rule for a kind as seen by a resolved viewer.
NotifRule notifRuleForViewer(uint8_t kind, const NotifViewer& v);

// Load the device policy file into RAM. Called once at boot (after the
// filesystem is up) and by cmd_notifydevicekind after edits.
void notifPolicyLoad();

// Monotonic counter bumped whenever the device policy or any user's prefs
// change — cache key for viewer-dependent views (OLED queue rebuild).
uint32_t notifPrefsGeneration();

// Device-wide per-kind level for config screens: 0=all, 1=admin, 2=off.
// notifLevelToken maps a level to the exact token cmd_notifydevicekind
// accepts ("all"/"admin"/"off") — display it and you can send it back.
uint8_t notifDeviceKindLevel(uint8_t kind);
const char* notifLevelToken(uint8_t level);

// Flush the per-user prefs cache. Hooked into saveUserSettings() so every
// write path (web /api/user/settings, notifyusermute, password ops) invalidates.
void notifUserPrefsInvalidate();

// CLI commands (registered in the settings-editor command table):
//   notifydevicekind — admin: per-kind device visibility level (all|admin|off)
//   notifyusermute — any logged-in user: personal mute list for the EXECUTING user
const char* cmd_notifydevicekind(const String& argsInput);
const char* cmd_notifyusermute(const String& argsInput);
const char* cmd_notifyusershow(const String& argsInput);
const char* cmd_notifylevel(const String& argsInput);

// `notifstats` diagnostics command (registered in the debug command table):
// pipeline loss/suppression/saturation counters; 'reset' zeroes them.
const char* cmd_notifstats(const String& argsInput);

// Render an event's human-facing message ("WiFi: 192.168.1.5", "Batt low:
// 15%"). Used by the banner/toast renderer and the queue view.
void notifFormatEvent(const SystemEvent& e, char* out, size_t outLen);

// ============================================================================
// Main-loop renderer
// ============================================================================

// Drain the event register and render banner/toast for every event whose
// rule enables those sinks. Called once per main-loop iteration; producers
// on other tasks never render UI themselves. Events older than 10s render
// nothing transient (the queue view has no such cut — it reads the ring
// directly).
void systemEventsNotifyTick();

#endif // SYSTEM_NOTIFICATIONS_H
