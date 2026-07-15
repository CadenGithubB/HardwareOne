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
};

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
//   3. sink masters       — gSettings.notifBanners/notifToasts/notifQueue
//   4. personal mutes     — the viewer's notificationMuted array in their
//                           per-user settings file (the same store the web
//                           dashboard layout preferences live in)
// None of it touches the event ring or automations — display only.

struct NotifViewer {
  bool known;            // false = anonymous surface (no login): non-admin view
  bool isAdmin;
  uint32_t muteMask[4];  // viewer's personal muted kinds (bit index = kind)
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

// Flush the per-user prefs cache. Hooked into saveUserSettings() so every
// write path (web /api/user/settings, notifyusermute, password ops) invalidates.
void notifUserPrefsInvalidate();

// CLI commands (registered in the settings-editor command table):
//   notifydevicekind — admin: per-kind device visibility level (all|admin|off)
//   notifyusermute — any logged-in user: personal mute list for the EXECUTING user
const char* cmd_notifydevicekind(const String& argsInput);
const char* cmd_notifyusermute(const String& argsInput);

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
