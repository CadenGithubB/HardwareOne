#include "BLE_PhoneStatus.h"

#if ENABLE_BLUETOOTH && ENABLE_BLE_PHONE_PIPE

#include <string.h>
#include "BLE_Events.h"      // CompactJson + blePushEvent
#include "BLE_LinkStatus.h"
#include "BLE_PhonePipe.h"
#include "System_CommandRegistry.h"  // argWantsJson
#include "System_MemUtil.h"  // PSRAM_JSON_DOC
#include "G2_Glasses.h"      // isG2ClientInitialized: the pipe's place beside the glasses

// The pipe alone on the shared stack: no glasses runtime beside it (the ring
// does not count). The host belongs to the stack either way.
static bool phonePipeAlone() { return phonePipeActive() && !isG2ClientInitialized(); }

PhoneStatusSnapshot phoneStatusSnapshot() {
  const BleLinkStatus link = bleLinkStatus();
  PhoneStatusSnapshot s;
  s.active = phonePipeActive();
  s.standalone = phonePipeAlone();
  s.ready = link.pipe.phase == hw1link::Pipe::Ready;
  s.state = link.pipeLabel[0] ? link.pipeLabel : "stopped";
  s.enrolled = link.pipePaired;
  s.autoReconnect = link.pipeAutoReconnect;
  s.pipe = link.pipe;
  return s;
}

// The pipe's own place: beside the glasses, or alone on the shared stack
// (phone-only; the ring does not count). The host belongs to the stack in
// both cases. The device role (BleLinkStatus::role) is a separate axis.
static const char* pipeRoleName(bool active, bool standalone) {
  if (!active) return "off";
  return standalone ? "phone-only" : "with-glasses";
}

bool phoneBuildStatusJson(JsonObject out) {
  return phoneBuildStatusJson(out, bleLinkStatus());
}

bool phoneBuildStatusJson(JsonObject out, const BleLinkStatus& link) {
  if (out.isNull()) return false;
  // One link reading for every key; active/standalone are the pipe's atomics.
  const bool active = phonePipeActive();
  const bool standalone = phonePipeAlone();
  char line[24];  // pipeLine: "fail: " plus a reason of at most 9 characters
  out["schema"] = 2;
  out["active"] = active;
  out["standalone"] = standalone;
  out["connected"] = link.pipe.phase == hw1link::Pipe::Ready;
  out["state"] = link.pipeLabel[0] ? link.pipeLabel : "stopped";  // diagnostics
  out["text"] = hw1link::pipeLine(link.pipe, line, sizeof(line));  // copied
  out["role"] = pipeRoleName(active, standalone);
  out["enrolled"] = link.pipePaired;
  out["autoReconnect"] = link.pipeAutoReconnect;
  out["pipe"] = hw1link::pipeId(link.pipe.phase);
  out["pipeText"] = hw1link::pipeText(link.pipe.phase);
  if (link.pipe.reason) out["pipeReason"] = link.pipe.reason;
  out["console"] = hw1link::consoleId(link.console);
  out["consoleText"] = hw1link::consoleLongText(link.console);
  return true;
}

// Compact keys: the SSE data field is capped at 128 chars (see BLE_Events.h).
//   u  link up (bool)       on  pipe running (bool)   own  phone-only (bool)
//   en enrolled (bool)      ar  autoReconnect (bool)
//   p  the pipe id, as the JSON "pipe" ("ready", "not-paired", ...)
//   r  its short reason, as "pipeReason", only when there is one
// Worst case 89 of 127 characters: every flag false with the longest id and
// reason, {"u":false,"on":false,"own":false,"en":false,"ar":false,
// "p":"not-paired","r":"forgotten"}. The pipe's own text (up to 50 characters,
// the JSON "state") would overflow beside them, so it stays out of the event.
static void phonePushStatusEvent(const PhoneStatusSnapshot& s) {
  char buf[128];
  CompactJson j(buf, sizeof(buf));
  j.kv("u", s.ready).kv("on", s.active).kv("own", s.standalone)
   .kv("en", s.enrolled).kv("ar", s.autoReconnect).kv("p", hw1link::pipeId(s.pipe.phase));
  if (s.pipe.reason) j.kv("r", s.pipe.reason);
  blePushEvent("phone-status", j);
}

void phoneStatusTick() {
  static uint32_t lastCheckMs = 0;
  static bool seeded = false;
  static uint8_t lastFlags = 0;
  static const char* lastState = nullptr;
  const uint32_t now = millis();
  if (seeded && (uint32_t)(now - lastCheckMs) < 250) return;
  lastCheckMs = now;
  // Atomic reads only; the peer registry is read just when a push goes out.
  // The setup step (hw1link::PipeStep, 0-6) fills the high nibble, so a new
  // phase (searching -> linking, pairing) is a change even when the text stays.
  const uint8_t flags = uint8_t((phonePipeActive() ? 1 : 0) |
                                (phonePipeAlone() ? 2 : 0) |
                                (phonePipeReady() ? 4 : 0) |
                                (phonePipeEnrolling() ? 8 : 0) |
                                ((phonePipeStep() & 0x0f) << 4));
  const char* state = phonePipeStatus();
  if (seeded && flags == lastFlags && state == lastState) return;
  const bool push = seeded;
  seeded = true;
  lastFlags = flags;
  lastState = state;
  if (push) phonePushStatusEvent(phoneStatusSnapshot());
}

const char* phoneStatusCommand(const String& args) {
  EXT_RAM_BSS_ATTR static char buf[384];
  if (argWantsJson(args)) {
    PSRAM_JSON_DOC(doc);
    const bool available = phoneBuildStatusJson(doc.to<JsonObject>());
    if (doc.overflowed()) return "{\"error\":\"oom\"}";
    if (!available) return "{\"error\":\"unavailable\"}";
    if (measureJson(doc) >= sizeof(buf)) return "{\"error\":\"status too large\"}";
    serializeJson(doc, buf, sizeof(buf));
    return buf;
  }
  const BleLinkStatus link = bleLinkStatus();
  char line[24];
  // role= is the pipe's place, as in the JSON form; `blestatus` shows the device role.
  const int n = snprintf(buf, sizeof(buf),
                         "pipe=%s role=%s paired=%s auto=%s console=%s state='%s'",
                         hw1link::pipeLine(link.pipe, line, sizeof(line)),
                         pipeRoleName(phonePipeActive(), phonePipeAlone()),
                         link.pipePaired ? "yes" : "no",
                         link.pipeAutoReconnect ? "on" : "off",
                         hw1link::consoleId(link.console), link.pipeLabel);
  if (n < 0 || (size_t)n >= sizeof(buf)) return "Error: phone status too large";
  return buf;
}

#endif  // ENABLE_BLUETOOTH && ENABLE_BLE_PHONE_PIPE
