#ifndef SYSTEM_EVENTS_H
#define SYSTEM_EVENTS_H

/**
 * System Events - in-memory event register (the "event bus")
 *
 * A fixed ring of discrete, semantic system events (peer came online, text
 * received, battery low, login failed, ...) that any subsystem can post to
 * from any task, and that multiple consumers drain independently via
 * sequence-number cursors. Since the Phase-1 notification cutover this is
 * ALSO the notification pipeline: the old notify*() layer is gone, and
 * System_Notifications' main-loop renderer + the OLED notification-center
 * view are just consumers of this ring (per-kind rules decide which events
 * become banners/toasts/queue entries).
 *
 * It is deliberately NOT a log. logSystemEvent() remains the durable,
 * file-backed audit trail; this ring holds only the last SYSEVT_RING_SIZE
 * events, in RAM, for immediate reaction.
 *
 * Attribution: every event carries WHO caused it — a source interface byte
 * (NotificationSource, defined here) plus a short who[] identity (username /
 * device / IP prefix), stamped automatically at post time from the calling
 * task's TLS context (see setNotificationContext below). Consumers surface
 * it ("by web:hub"), and automation event-trigger `match` patterns test it.
 *
 * Task-safety: systemEventPost() is a bounded copy under a spinlock —
 * callable from any task (espnow_task, sensor workers, sr_task, cmd_exec,
 * BLE callbacks). Not for ISRs. Consumers each keep their own cursor and
 * call systemEventFetchSince() from their own task.
 */

#include <Arduino.h>
#include <stdint.h>

#include "System_EventCatalog.h"

// ============================================================================
// Source attribution (moved here from System_Notifications.h — events are
// the base layer now; notifications consume them)
// ============================================================================

enum NotificationSource : uint8_t {
  NOTIF_SOURCE_UNKNOWN = 0,
  NOTIF_SOURCE_CLI     = 1,
  NOTIF_SOURCE_OLED    = 2,
  NOTIF_SOURCE_WEB     = 3,
  NOTIF_SOURCE_VOICE   = 4,
  NOTIF_SOURCE_REMOTE  = 5,
  NOTIF_SOURCE_SYSTEM  = 6,  // Firmware-generated: sensor starts, WiFi events, etc.
  NOTIF_SOURCE_G2      = 7   // BLE-attached lens — mirrors SOURCE_G2_GLASSES
};

// Per-task source context. Set before executing user-attributable work so
// events (and therefore notifications) carry who caused them.
// source: NotificationSource enum value; subsource: username/device/IP.
void setNotificationContext(uint8_t source, const char* subsource = nullptr);
void clearNotificationContext();

// RAII guard — installs the context on construction, restores the PRIOR
// context on destruction (save/restore, so nested guards compose).
struct NotificationContextGuard {
  NotificationContextGuard(uint8_t source, const char* subsource = nullptr);
  ~NotificationContextGuard();
  NotificationContextGuard(const NotificationContextGuard&) = delete;
  NotificationContextGuard& operator=(const NotificationContextGuard&) = delete;
  NotificationContextGuard(NotificationContextGuard&&) = delete;
  NotificationContextGuard& operator=(NotificationContextGuard&&) = delete;

 private:
  uint8_t savedSource_;
  char    savedSubsource_[32];
};

// Immutable event-family and kind metadata, typed enumeration, and legacy
// lookup compatibility are owned by System_EventCatalog.h.
#define SYSEVT_RING_SIZE 48
// Field widths grown 2026-07 for longer names/descriptions. Each byte of
// per-slot growth is multiplied across BOTH the PSRAM-designated event ring and
// the PSRAM-preferred automation drain buffer (each can be internal on a build
// without usable external RAM). Keep modest.
#define SYSEVT_SUBJECT_LEN 48
#define SYSEVT_DETAIL_LEN 80
#define SYSEVT_WHO_LEN 24

struct SystemEvent {
  uint32_t seq;     // monotonically increasing, never 0 for a live event
  uint32_t tsMs;    // millis() at post time
  uint8_t kind;     // SystemEventKind
  uint8_t source;   // NotificationSource — which interface caused it
  char who[SYSEVT_WHO_LEN];          // username / device / IP prefix ('\0' = n/a)
  char subject[SYSEVT_SUBJECT_LEN];  // who/what (peer name, sensor, username, key)
  char detail[SYSEVT_DETAIL_LEN];    // extra (MAC, text preview, filename, value)
};

// Post an event from any task (not ISR). subject/detail may be nullptr; both
// truncate to their field sizes. source/who are stamped from the calling
// task's notification TLS context (UNKNOWN maps to SYSTEM — a task with no
// installed context is firmware-initiated by definition); pass srcOverride /
// whoOverride to stamp explicitly instead. Also nudges the automation
// scheduler if any enabled automation subscribes to this kind.
void systemEventPost(uint8_t kind, const char* subject = nullptr, const char* detail = nullptr,
                     uint8_t srcOverride = 0xFF, const char* whoOverride = nullptr);

// Copy out every event with seq > *cursor (oldest first), up to maxOut, and
// advance *cursor past the newest copied. A consumer more than
// SYSEVT_RING_SIZE behind skips the overwritten gap. Pass
// *cursor = UINT32_MAX to start "from now"; 0 to start from the oldest
// live event. Returns the number copied.
// skippedOut (optional): ACCUMULATES the size of any skipped gap — pass a
// persistent per-consumer counter to make the loss observable. The aggregate
// across all consumers is systemEventRingSkippedTotal().
int systemEventFetchSince(uint32_t* cursor, SystemEvent* out, int maxOut,
                          uint32_t* skippedOut = nullptr);

// Total ring-overwrite skips across every consumer since boot. Nonzero means
// a consumer fell >SYSEVT_RING_SIZE events behind and lost events silently
// (for the automation cursor that means event triggers that never fired).
uint32_t systemEventRingSkippedTotal();

// Copy one event by exact sequence number. Returns false if that seq has
// been overwritten (fell off the ring) or never existed. Lets viewers walk
// the ring newest-first without a 5KB snapshot buffer.
bool systemEventGetBySeq(uint32_t seq, SystemEvent* out);

// Newest sequence number posted so far (0 = nothing yet).
uint32_t systemEventLatestSeq();

// Total events ever posted (== latest seq).
uint32_t systemEventTotalPosted();

// Short source-interface name ("cli", "web", "system", ...).
const char* systemEventSourceName(uint8_t source);

// Match helper shared by the automation event matcher: true when `pattern`
// is empty, "*", or a case-insensitive substring of the event's subject,
// detail, OR who.
bool systemEventMatches(const SystemEvent& ev, const char* pattern);

// `events` CLI command (registered in the system module table).
const char* cmd_events(const String& argsInput);

// Structured event-history file sink — the third ring consumer. Drains the
// ring into /system/sys_logs/events.log (one machine-parseable line per
// event) via the debug output task's [EVLOG] tee. Called once per main-loop
// iteration; internally throttled and gated on gSettings.eventLogEnabled.
// The curated free-text system-events.log is a separate, untouched pipeline.
void systemEventLogTick(bool force = false);  // force=true drains the ring ignoring the 2s throttle
void systemEventLogFlush();                    // enqueue all pending events to events.log NOW (e.g. before a reboot)

#endif // SYSTEM_EVENTS_H
