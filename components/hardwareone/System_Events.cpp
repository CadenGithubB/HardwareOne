/**
 * System Events - in-memory event register (the "event bus")
 *
 * See System_Events.h for the design contract. Producers copy into a fixed
 * ring under a spinlock; consumers copy out under the same spinlock with
 * per-consumer cursors. No heap in the post path, no FS, no blocking.
 *
 * This file also owns the per-task source-attribution TLS context (moved
 * here from System_Notifications in the Phase-1 cutover — posts stamp
 * source+who from it, so every consumer gets attribution for free).
 */

#include "System_Events.h"

#include <string.h>

#include "System_Automation.h"  // automationOnSystemEvent (stub when disabled)
#include "System_Debug.h"       // cliHint, debugQueueLine, MSG_ROUTE_*
#include "System_Settings.h"    // gSettings.eventLogEnabled
#include "System_Filesystem.h"  // filesystemReady
#include "System_Utils.h"       // everyMs, argWantsJson
#include "System_MemUtil.h"     // PSRAM_JSON_DOC / PSRAM_STATIC_BUF — json branch returns the document
#include "System_CommandTypes.h"  // CMD_RESULT_MAX — the json branch's ceiling
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"  // vTaskSetThreadLocalStoragePointerAndDelCallback

// ============================================================================
// Source-attribution TLS context — per-task (source, subsource)
// ============================================================================
//
// Each FreeRTOS task gets its own tuple so concurrent tasks never stomp each
// other's attribution, and the RAII guard save/restores so nesting composes.
//
// Slot coordination:
//   slot 0 — ESP-IDF pthread (PTHREAD_TLS_INDEX). DO NOT use.
//   slot 1 — auth identity (System_AuthIdentity.cpp).
//   slot 2 — this module (was System_Notifications pre-cutover).
//   slot 3 — unused (CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS=4).

static constexpr BaseType_t kNotifTlsSlot = 2;

namespace {

struct NotifContext {
  uint8_t source = NOTIF_SOURCE_UNKNOWN;
  char    subsource[32] = {};
};

const NotifContext& anonSentinel() {
  static const NotifContext kAnon{};
  return kAnon;
}

void deleteNotifContext(int /*index*/, void* p) {
  delete static_cast<NotifContext*>(p);
}

NotifContext* getOrCreateSlot() {
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  if (!self) return nullptr;  // pre-scheduler
  NotifContext* slot = static_cast<NotifContext*>(
      pvTaskGetThreadLocalStoragePointer(self, kNotifTlsSlot));
  if (!slot) {
    slot = new NotifContext{};
    vTaskSetThreadLocalStoragePointerAndDelCallback(
        self, kNotifTlsSlot, slot, deleteNotifContext);
  }
  return slot;
}

const NotifContext* getSlotReadOnly() {
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  if (!self) return &anonSentinel();
  NotifContext* slot = static_cast<NotifContext*>(
      pvTaskGetThreadLocalStoragePointer(self, kNotifTlsSlot));
  return slot ? slot : &anonSentinel();
}

}  // namespace

void setNotificationContext(uint8_t source, const char* subsource) {
  NotifContext* slot = getOrCreateSlot();
  if (!slot) return;  // pre-scheduler — no-op (no task to attribute to)
  slot->source = source;
  if (subsource && subsource[0]) {
    strncpy(slot->subsource, subsource, sizeof(slot->subsource) - 1);
    slot->subsource[sizeof(slot->subsource) - 1] = '\0';
  } else {
    slot->subsource[0] = '\0';
  }
}

void clearNotificationContext() {
  NotifContext* slot = getOrCreateSlot();
  if (!slot) return;
  slot->source = NOTIF_SOURCE_UNKNOWN;
  slot->subsource[0] = '\0';
}

NotificationContextGuard::NotificationContextGuard(uint8_t source, const char* subsource)
    : savedSource_(NOTIF_SOURCE_UNKNOWN),
      savedSubsource_{} {
  NotifContext* slot = getOrCreateSlot();
  if (slot) {
    savedSource_ = slot->source;
    memcpy(savedSubsource_, slot->subsource, sizeof(savedSubsource_));
  }
  setNotificationContext(source, subsource);
}

NotificationContextGuard::~NotificationContextGuard() {
  NotifContext* slot = getOrCreateSlot();
  if (!slot) return;
  slot->source = savedSource_;
  memcpy(slot->subsource, savedSubsource_, sizeof(slot->subsource));
}

// ============================================================================
// Ring storage
// ============================================================================

static SystemEvent gEventRing[SYSEVT_RING_SIZE];
static uint32_t gEventNextSeq = 1;  // seq of the NEXT event to be posted
static portMUX_TYPE gEventMux = portMUX_INITIALIZER_UNLOCKED;

// Kind names — generated from SYSEVT_KIND_LIST so the strings can never
// drift from the enum. These are what automation event triggers use in
// their "on" field; keep them stable (see System_Events.h).
static const char* const kEventKindNames[SYSEVT_COUNT] = {
    "none",
#define SYSEVT_X(sym, name, fam) name,
    SYSEVT_KIND_LIST(SYSEVT_X)
#undef SYSEVT_X
};

// Family per kind — same X-macro, so a kind can never carry a family that
// disagrees with its declaration. Index 0 (SYSEVT_NONE) is a filler.
static const uint8_t kEventKindFamily[SYSEVT_COUNT] = {
    SYSEVT_FAM_SYSTEM,
#define SYSEVT_X(sym, name, fam) fam,
    SYSEVT_KIND_LIST(SYSEVT_X)
#undef SYSEVT_X
};

// Family display labels, generated from SYSEVT_FAMILY_LIST.
static const char* const kEventFamilyNames[SYSEVT_FAM_COUNT] = {
#define SYSEVT_FAM_X(sym, label) label,
    SYSEVT_FAMILY_LIST(SYSEVT_FAM_X)
#undef SYSEVT_FAM_X
};

const char* systemEventFamilyName(uint8_t family) {
  if (family >= SYSEVT_FAM_COUNT) return "?";
  return kEventFamilyNames[family];
}

uint8_t systemEventKindFamily(uint8_t kind) {
  if (kind >= SYSEVT_COUNT) return SYSEVT_FAM_SYSTEM;
  return kEventKindFamily[kind];
}

const char* systemEventKindName(uint8_t kind) {
  if (kind >= SYSEVT_COUNT) return "?";
  return kEventKindNames[kind];
}

int systemEventKindFromName(const char* name) {
  if (!name || !name[0]) return -1;
  // Event names are persisted by automations and notification policy. Keep the
  // former single boot event as a read-time alias for the completion edge so
  // upgrades do not silently orphan existing `on: "boot"` configuration. New
  // listings and writes use only the two canonical boot_started/boot_finished
  // names generated from SYSEVT_KIND_LIST.
  if (strcasecmp(name, "boot") == 0) return SYSEVT_BOOT_FINISHED;
  for (int i = SYSEVT_NONE + 1; i < SYSEVT_COUNT; i++) {
    if (strcasecmp(name, kEventKindNames[i]) == 0) return i;
  }
  return -1;
}

const char* systemEventSourceName(uint8_t source) {
  switch (source) {
    case NOTIF_SOURCE_CLI:    return "cli";
    case NOTIF_SOURCE_OLED:   return "oled";
    case NOTIF_SOURCE_WEB:    return "web";
    case NOTIF_SOURCE_VOICE:  return "voice";
    case NOTIF_SOURCE_REMOTE: return "remote";
    case NOTIF_SOURCE_SYSTEM: return "system";
    case NOTIF_SOURCE_G2:     return "g2";
    default:                  return "?";
  }
}

// Case-insensitive substring search (strcasestr is a GNU extension; keep a
// local bounded implementation so this module has zero libc surprises).
static bool ciContains(const char* haystack, const char* needle) {
  if (!needle || !needle[0]) return true;
  if (!haystack || !haystack[0]) return false;
  size_t nlen = strlen(needle);
  for (const char* h = haystack; *h; h++) {
    if (strncasecmp(h, needle, nlen) == 0) return true;
  }
  return false;
}

bool systemEventMatches(const SystemEvent& ev, const char* pattern) {
  if (!pattern || !pattern[0] || strcmp(pattern, "*") == 0) return true;
  return ciContains(ev.subject, pattern) || ciContains(ev.detail, pattern) ||
         ciContains(ev.who, pattern);
}

// ============================================================================
// Post / fetch
// ============================================================================

void systemEventPost(uint8_t kind, const char* subject, const char* detail,
                     uint8_t srcOverride, const char* whoOverride) {
  if (kind == SYSEVT_NONE || kind >= SYSEVT_COUNT) return;

  SystemEvent ev;
  ev.tsMs = millis();
  ev.kind = kind;
  ev.subject[0] = '\0';
  ev.detail[0] = '\0';
  ev.who[0] = '\0';
  if (subject && subject[0]) {
    strncpy(ev.subject, subject, sizeof(ev.subject) - 1);
    ev.subject[sizeof(ev.subject) - 1] = '\0';
  }
  if (detail && detail[0]) {
    strncpy(ev.detail, detail, sizeof(ev.detail) - 1);
    ev.detail[sizeof(ev.detail) - 1] = '\0';
  }

  // Attribution: explicit override wins; otherwise the calling task's TLS
  // context. A task with no context installed (sensor workers, radio tasks)
  // is firmware-initiated by definition — map UNKNOWN to SYSTEM.
  if (srcOverride != 0xFF) {
    ev.source = srcOverride;
  } else {
    const NotifContext& nctx = *getSlotReadOnly();
    ev.source = (nctx.source == NOTIF_SOURCE_UNKNOWN) ? NOTIF_SOURCE_SYSTEM : nctx.source;
    if (!whoOverride && nctx.subsource[0]) {
      strncpy(ev.who, nctx.subsource, sizeof(ev.who) - 1);
      ev.who[sizeof(ev.who) - 1] = '\0';
    }
  }
  if (whoOverride && whoOverride[0]) {
    strncpy(ev.who, whoOverride, sizeof(ev.who) - 1);
    ev.who[sizeof(ev.who) - 1] = '\0';
  }

  taskENTER_CRITICAL(&gEventMux);
  ev.seq = gEventNextSeq++;
  gEventRing[ev.seq % SYSEVT_RING_SIZE] = ev;
  taskEXIT_CRITICAL(&gEventMux);

  // Wake the automation scheduler if any enabled automation has an event
  // trigger for this kind (cheap mask check; no-op stub when automation is
  // compiled out).
  automationOnSystemEvent(kind);
}

uint32_t systemEventLatestSeq() {
  taskENTER_CRITICAL(&gEventMux);
  uint32_t latest = gEventNextSeq - 1;
  taskEXIT_CRITICAL(&gEventMux);
  return latest;
}

uint32_t systemEventTotalPosted() { return systemEventLatestSeq(); }

// Ring-overwrite loss counter (all consumers). Guarded by gEventMux.
static uint32_t gEventRingSkipped = 0;

uint32_t systemEventRingSkippedTotal() {
  taskENTER_CRITICAL(&gEventMux);
  uint32_t v = gEventRingSkipped;
  taskEXIT_CRITICAL(&gEventMux);
  return v;
}

int systemEventFetchSince(uint32_t* cursor, SystemEvent* out, int maxOut,
                          uint32_t* skippedOut) {
  if (!cursor || !out || maxOut <= 0) return 0;

  taskENTER_CRITICAL(&gEventMux);
  uint32_t latest = gEventNextSeq - 1;
  if (*cursor == UINT32_MAX) {
    // First call with the "from now" sentinel: skip everything already live.
    *cursor = latest;
    taskEXIT_CRITICAL(&gEventMux);
    return 0;
  }
  uint32_t from = *cursor + 1;
  // Oldest still-live seq: the ring holds the last SYSEVT_RING_SIZE posts.
  uint32_t oldest = (latest >= SYSEVT_RING_SIZE) ? latest - SYSEVT_RING_SIZE + 1 : 1;
  if (from < oldest) {
    // Consumer fell behind; the overwritten gap is SILENT LOSS — count it so
    // the diagnostics (`notifstats`, DEBUG_NOTIFICATIONS/[AUTO] warnings) can
    // surface what used to vanish without a trace.
    uint32_t skipped = oldest - from;
    gEventRingSkipped += skipped;
    if (skippedOut) *skippedOut += skipped;
    from = oldest;
  }
  int n = 0;
  for (uint32_t s = from; s <= latest && n < maxOut; s++) {
    out[n++] = gEventRing[s % SYSEVT_RING_SIZE];
  }
  if (n > 0) *cursor = out[n - 1].seq;
  else if (latest > *cursor) *cursor = latest;  // nothing live to copy
  taskEXIT_CRITICAL(&gEventMux);
  return n;
}

bool systemEventGetBySeq(uint32_t seq, SystemEvent* out) {
  if (!out || seq == 0) return false;
  taskENTER_CRITICAL(&gEventMux);
  const SystemEvent& slot = gEventRing[seq % SYSEVT_RING_SIZE];
  bool live = (slot.seq == seq);
  if (live) *out = slot;
  taskEXIT_CRITICAL(&gEventMux);
  return live;
}

// ============================================================================
// `events` CLI command — inspect the ring (newest first, with attribution)
// ============================================================================

const char* cmd_events(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String sub = argsInput;
  sub.trim();
  if (sub.startsWith("kinds") || sub.startsWith("Kinds") || sub.startsWith("KINDS")) {
    // List every valid kind name — the vocabulary for automation event
    // triggers ("on="), notifydevicekind levels, and notifyusermute lists. The json
    // form feeds the web editors.
    if (argWantsJson(sub)) {
      // The whole document goes in the RETURN VALUE and nothing is broadcast —
      // see the JSON contract in System_Utils.cpp. Broadcasting it meant the
      // addressed reply on every transport was a bare "OK", since only the
      // return value reaches those.
      // Per-transport reality for this multi-kilobyte payload:
      //   web /api/cli, BLE  — ExecReq::out[4096], full document. OK.
      //   ESP-NOW            — 6143 B fragmenting. OK.
      //   MQTT               — TRUNCATED. System_MQTT.cpp hands executeCommand a
      //                        2048 B buffer and System_Utils.cpp strncpy's into
      //                        it silently, so MQTT gets a torn 2047 B fragment.
      //                        Fix belongs in that buffer, not here.
      //   serial console     — still [CUT]. The return value is echoed via
      //                        broadcastOutput(), which clamps a line to 255 B.
      //                        Platform line limit, not this command's bug.
      // GROUPED ONLY — one shape, not two. Emitting a flat `kinds` array beside
      // the grouped copy would repeat every name and can push the result over
      // CMD_RESULT_MAX. A consumer that only wants the flat vocabulary can
      // flatten the groups, so the second copy buys nothing and costs headroom.
      PSRAM_JSON_DOC(doc);
      JsonArray fams = doc["families"].to<JsonArray>();
      for (int f = 0; f < SYSEVT_FAM_COUNT; f++) {
        JsonObject fo = fams.add<JsonObject>();
        fo["n"] = systemEventFamilyName((uint8_t)f);
        JsonArray fk = fo["k"].to<JsonArray>();
        for (int k = SYSEVT_NONE + 1; k < SYSEVT_COUNT; k++) {
          // static tables — ArduinoJson stores const char* by pointer, no copy
          if (systemEventKindFamily((uint8_t)k) == f) fk.add(systemEventKindName((uint8_t)k));
        }
      }
      PSRAM_STATIC_BUF(jbuf, CMD_RESULT_MAX);
      size_t n = serializeJson(doc, jbuf, jbuf_SIZE);
      // serializeJson truncates silently when it runs out of room; a short list
      // would look like a real answer, so fail loudly instead.
      if (n == 0 || n >= jbuf_SIZE - 1) return "Error: event-kind list outgrew the response buffer";
      return jbuf;
    }
    BROADCAST_PRINTF("OK: %d event kinds in %d families:",
                     (int)SYSEVT_COUNT - 1, (int)SYSEVT_FAM_COUNT);
    // One family per section, names packed into <=255 B lines at the call site
    // (there is no runtime splitter — see System_Debug.cpp).
    char line[120];
    for (int f = 0; f < SYSEVT_FAM_COUNT; f++) {
      int count = 0;
      for (int k = SYSEVT_NONE + 1; k < SYSEVT_COUNT; k++) {
        if (systemEventKindFamily((uint8_t)k) == f) count++;
      }
      if (count == 0) continue;
      BROADCAST_PRINTF("  %s (%d):", systemEventFamilyName((uint8_t)f), count);
      int len = 0;
      for (int k = SYSEVT_NONE + 1; k < SYSEVT_COUNT; k++) {
        if (systemEventKindFamily((uint8_t)k) != f) continue;
        const char* n = systemEventKindName((uint8_t)k);
        if (len > 0 && len + 1 + (int)strlen(n) > (int)sizeof(line) - 1) {
          broadcastOutput(line);
          len = 0;
        }
        len += snprintf(line + len, sizeof(line) - len, len ? " %s" : "    %s", n);
      }
      if (len > 0) broadcastOutput(line);
    }
    cliHint("drive automations with 'automation add ... type=event on=<kind>' or mute notifications with 'notifyusermute <kind,...>'");
    return "OK";
  }

  uint32_t latest = systemEventLatestSeq();
  BROADCAST_PRINTF("OK: %lu event(s) posted since boot (ring holds last %d)",
                   (unsigned long)latest, SYSEVT_RING_SIZE);
  uint32_t nowMs = millis();
  int shown = 0;
  SystemEvent e;
  for (uint32_t seq = latest; seq > 0 && shown < SYSEVT_RING_SIZE; seq--) {
    if (!systemEventGetBySeq(seq, &e)) break;  // fell off the ring
    uint32_t ageS = (nowMs - e.tsMs) / 1000;
    char attr[32];
    if (e.who[0]) {
      snprintf(attr, sizeof(attr), "  by %s:%s", systemEventSourceName(e.source), e.who);
    } else {
      snprintf(attr, sizeof(attr), "  by %s", systemEventSourceName(e.source));
    }
    if (e.detail[0]) {
      BROADCAST_PRINTF("  [%lu] %lus ago  %-17s %s (%s)%s", (unsigned long)e.seq,
                       (unsigned long)ageS, systemEventKindName(e.kind), e.subject, e.detail, attr);
    } else {
      BROADCAST_PRINTF("  [%lu] %lus ago  %-17s %s%s", (unsigned long)e.seq,
                       (unsigned long)ageS, systemEventKindName(e.kind), e.subject, attr);
    }
    shown++;
  }
  if (shown == 0) {
    broadcastOutput("  (no events yet)");
  }
  cliHint("these kinds can drive automations - add an Event trigger on the web Automations page, or 'automation add name=<n> type=event on=<kind> [match=<text>] commands=\"...\" enabled=1' (match also tests the by-who field); list all kinds with 'events kinds'");
  return "OK";
}

// ============================================================================
// Structured event-history file sink (third ring consumer)
// ============================================================================
// Drains the ring into /system/sys_logs/events.log — the complete, durable,
// machine-parseable tier alongside the curated free-text system-events.log.
// Lines go through the debug output task (the single log writer) via the
// [EVLOG] prefix tee; the route mask carries no display sinks, so nothing
// echoes to serial/web/OLED. Loss is self-recording: if this consumer ever
// falls >SYSEVT_RING_SIZE behind, a gap-marker line lands in the file where
// the missing events would have been.
// Copy src into dst replacing bytes that would break the one-line-per-event
// contract: control/non-ASCII chars (an embedded '\n' would split the record
// and confuse the rotation trim) become ' ', and the field separator '|'
// becomes '/'. subject/detail can carry remote-origin bytes (ESP-NOW chat
// text lands in SYSEVT_TEXT_RX detail verbatim) — same convention as the G2
// chat renderer's non-printable scrub.
static void sanitizeField(const char* src, char* dst, size_t dstLen) {
  size_t i = 0;
  for (; src[i] && i < dstLen - 1; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c < 0x20 || c > 0x7E) dst[i] = ' ';
    else if (c == '|') dst[i] = '/';
    else dst[i] = (char)c;
  }
  dst[i] = '\0';
}

void systemEventLogTick(bool force) {
  static uint32_t sCursor = 0;         // 0 = oldest live event (captures boot events)
  static uint32_t sSkipped = 0;        // ring overwrote events before we read them
  static uint32_t sSkippedLogged = 0;  // portion of sSkipped already gap-marked
  static uint32_t sDrainMs = 0;

  if (!force && !everyMs(&sDrainMs, 2000)) return;
  if (!filesystemReady) return;  // events wait in the ring until FS is up
  if (!gSettings.eventLogEnabled) {
    // Disabled: track the head cheaply so a later enable doesn't dump stale
    // history or count the idle period as a ring-skip gap.
    sCursor = systemEventLatestSeq();
    return;
  }

  const uint8_t route = MSG_ROUTE_ALLOW_IN_HELP;  // file tee only, no display sinks

  // Up to 3 batches (24 lines) per drain so a full-ring backlog (boot replay,
  // lagged sink) can't monopolize the shared debug-message pool in one
  // main-loop lap; the remainder stays in the ring for the next drain.
  SystemEvent evs[8];
  for (int batch = 0; batch < 3; batch++) {
    int n = systemEventFetchSince(&sCursor, evs, 8, &sSkipped);
    if (n <= 0) break;

    // Ring-overwrite gap marker, emitted before this batch's lines — the
    // batch is the first data after the gap, so the marker lands where the
    // missing events would have been. If the marker enqueue fails, the
    // counters stay unbalanced and it retries next drain (a batch late, but
    // the per-line #seq discontinuity still pins the exact spot).
    if (sSkipped != sSkippedLogged) {
      char gap[96];
      snprintf(gap, sizeof(gap),
               "[EVLOG] !gap %lu event(s) overwritten before logging (sink lagged)",
               (unsigned long)(sSkipped - sSkippedLogged));
      if (debugQueueLine(gap, route)) sSkippedLogged = sSkipped;
    }

    for (int i = 0; i < n; i++) {
      const SystemEvent& e = evs[i];
      char who[SYSEVT_WHO_LEN], subj[SYSEVT_SUBJECT_LEN], det[SYSEVT_DETAIL_LEN];
      sanitizeField(e.who, who, sizeof(who));
      sanitizeField(e.subject, subj, sizeof(subj));
      sanitizeField(e.detail, det, sizeof(det));
      // Sized to the debug-transport ceiling (DEBUG_MSG_SIZE); with the wider
      // event fields the worst-case line is ~222 chars, still under it, and
      // debugQueueLine [CUT]-marks anything that would exceed it.
      char line[DEBUG_MSG_SIZE];
      int len = snprintf(line, sizeof(line), "[EVLOG] #%lu %s | %s%s%s | %s | %s",
                         (unsigned long)e.seq,
                         systemEventKindName(e.kind),
                         systemEventSourceName(e.source),
                         who[0] ? ":" : "", who,
                         subj[0] ? subj : "-",
                         det[0] ? det : "-");
      // The tee stamps wall-clock time at WRITE time; annotate lines written
      // long after the event occurred (boot replay, FS-not-ready backlog)
      // with the event's own uptime so ordering stays reconstructible.
      uint32_t ageMs = millis() - e.tsMs;
      if (ageMs > 5000UL && len > 0 && len < (int)sizeof(line) - 20) {
        snprintf(line + len, sizeof(line) - len, " (@%lums)", (unsigned long)e.tsMs);
      }
      if (!debugQueueLine(line, route)) {
        // Queue saturated. The event is still live in the ring — rewind the
        // cursor to just before it and retry next drain, so transient
        // saturation delays logging instead of losing it. Prolonged
        // saturation eventually surfaces as a ring-overwrite gap above.
        sCursor = e.seq - 1;
        return;
      }
    }
    if (n < 8) break;  // ring drained
  }
}

// Enqueue all pending ring events to events.log immediately, ignoring the 2s
// throttle. Called before a controlled reboot so the last couple seconds of
// typed history reach the file. The background writer still needs a moment to
// drain its queue to flash — the caller's reboot flush delay covers that.
void systemEventLogFlush() { systemEventLogTick(true); }
