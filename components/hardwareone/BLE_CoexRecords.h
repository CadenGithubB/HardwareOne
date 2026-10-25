#pragma once
// Coexistence measurement records for BLE scans and connect attempts by the G2
// glasses, the R1 ring and the phone pipe, each tagged with the device's own
// Wi-Fi state (docs/WIFI_BLE_COEXISTENCE_STUDY.md, lever 1). They record what
// happened; nothing here changes radio behaviour.
//
// The types and formatting have no Arduino, ESP-IDF or project dependencies,
// so host tests include this header as is. Records hold no addresses or names.
// The runtime functions at the bottom live in BLE_CoexRecords.cpp. With
// ENABLE_BLE_COEX_RECORDS=0 (and in builds without Bluetooth) they are no-ops
// and the readers report that the records are not in the build.
#include "System_WiFiCoex.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace hw1coex {

enum class Peer : uint8_t { None = 0, Glasses, GlassesLeft, GlassesRight, Ring, Phone };
enum class Purpose : uint8_t { None = 0, Discovery, Saved, Repair, Reconnect, Enroll };
enum class ScanOutcome : uint8_t { Found = 1, Partial, Missed, Aborted, StartFailed };
enum class ConnectOutcome : uint8_t { Connected = 1, Failed, Cancelled };
// Up to Complete, numbered like Arduino's BLEClientConnectStage (G2 and ring;
// BLE_CoexRecords.cpp checks). The phone's setup steps follow: the step an
// attempt reached, named like the `last setup_ms` columns.
enum class ConnectStage : uint8_t {
  None = 0, Precheck, RegisterApi, RegisterEvent, RegisterTimeout, OpenApi, Open, OpenTimeout, Complete,
  Discovery, Mtu, Search, Subscribe, Cccd, Secure, Hello, Ready
};
// Who ended a cancelled attempt, or a failure's notable trigger.
enum class CauseDetail : uint8_t { None = 0, ServiceChanged, Stopped, Replaced, Expired };

inline const char* name(Peer p) {
  switch (p) {
    case Peer::Glasses: return "g2";
    case Peer::GlassesLeft: return "g2-left";
    case Peer::GlassesRight: return "g2-right";
    case Peer::Ring: return "ring";
    case Peer::Phone: return "phone";
    default: return "-";
  }
}
inline const char* name(Purpose p) {
  switch (p) {
    case Purpose::Discovery: return "discovery";
    case Purpose::Saved: return "saved";
    case Purpose::Repair: return "repair";
    case Purpose::Reconnect: return "reconnect";
    case Purpose::Enroll: return "enroll";
    default: return "-";
  }
}
inline const char* name(ScanOutcome o) {
  switch (o) {
    case ScanOutcome::Found: return "found";
    case ScanOutcome::Partial: return "partial";
    case ScanOutcome::Missed: return "missed";
    case ScanOutcome::Aborted: return "aborted";
    case ScanOutcome::StartFailed: return "start-failed";
  }
  return "-";
}
inline const char* name(ConnectOutcome o) {
  return o == ConnectOutcome::Connected ? "connected" : o == ConnectOutcome::Failed ? "failed" : "cancelled";
}
inline const char* name(ConnectStage s) {
  switch (s) {
    case ConnectStage::Precheck: return "precheck";
    case ConnectStage::RegisterApi: case ConnectStage::RegisterEvent: return "register";
    case ConnectStage::RegisterTimeout: return "register-timeout";
    case ConnectStage::OpenApi: return "open-api";
    case ConnectStage::Open: return "open";
    case ConnectStage::OpenTimeout: return "open-timeout";
    case ConnectStage::Complete: return "complete";
    case ConnectStage::Discovery: return "discovery";
    case ConnectStage::Mtu: return "mtu";
    case ConnectStage::Search: return "search";
    case ConnectStage::Subscribe: return "subscribe";
    case ConnectStage::Cccd: return "cccd";
    case ConnectStage::Secure: return "secure";
    case ConnectStage::Hello: return "hello";
    case ConnectStage::Ready: return "ready";
    default: return "-";
  }
}
inline const char* name(CauseDetail d) {
  switch (d) {
    case CauseDetail::ServiceChanged: return "service-changed";
    case CauseDetail::Stopped: return "stopped";
    case CauseDetail::Replaced: return "replaced";
    case CauseDetail::Expired: return "expired";
    default: return "";
  }
}
// The disconnect reason Bluedroid reports in the DISCONNECT that precedes a
// failed OPEN (whose own status is only the generic 0x85), or that ends a
// link. Bluedroid silently retries a failed establishment up to
// BT_GATTC_CONNECT_RETRY_COUNT times (3 on every lane), so 0x3e means every
// attempt failed; on success the retries are unseen. 0x100 is not an HCI code:
// it is Bluedroid's L2CAP "connection cancelled", used for its 30 s connect
// timeout, a local cancel and a controller refusal; the elapsed time tells
// them apart (about 30 s, in between, a few ms).
inline const char* reasonName(uint16_t r) {
  switch (r) {
    case 0: return "-";
    case 0x08: return "supervision-timeout";
    case 0x13: return "peer-ended";
    case 0x16: return "local-ended";
    case 0x22: return "ll-response-timeout";
    case 0x3e: return "failed-to-establish";
    case 0x0100: return "conn-cancelled";
    default: return "other";
  }
}

// Written by BLE callbacks during one scan (atomics only), read by a task
// after it. Times are the callback's millis(), so queueing doesn't skew them.
struct Tally {
  std::atomic<uint32_t> reports{0}, matches{0}, startedAt{0}, firstAt{0}, endSpan{0};
  std::atomic<int> bestRssi{-128};
  std::atomic<bool> queued{false}, started{false}, heard{false};
  void reset() {
    reports = 0; matches = 0; startedAt = 0; firstAt = 0; endSpan = 0; bestRssi = -128;
    queued = false; started = false; heard = false;
  }
  void start(uint32_t now) { startedAt = now; started = true; }
  // The controller ended this scan (its timer, or a stop it confirmed). Only
  // after this scan's own start: a late stop of the previous scan arrives
  // before it. The first end wins. Kept as the span from the start plus one,
  // so zero means "not seen".
  void end(uint32_t now) {
    if (!started.load()) return;
    uint32_t none = 0;
    endSpan.compare_exchange_strong(none, uint32_t(now - startedAt.load()) + 1u);
  }
  void report() { reports.fetch_add(1, std::memory_order_relaxed); }
  // Milliseconds from the scan's start to the first match; kNone if none was
  // heard after a confirmed start.
  static constexpr uint32_t kNone = UINT32_MAX;
  uint32_t firstMatchMs() const {
    if (!heard || !started) return kNone;
    const int32_t d = int32_t(firstAt - startedAt);
    return d > 0 ? uint32_t(d) : 0;
  }
  // Counts a matching advertisement. True only for the first usable one in a
  // scan, so a scan without the duplicate filter queues one candidate.
  bool match(int rssi, bool usable, uint32_t now) {
    if (!heard.exchange(true)) firstAt = now;
    matches.fetch_add(1, std::memory_order_relaxed);
    int best = bestRssi.load(std::memory_order_relaxed);
    while (rssi > best && !bestRssi.compare_exchange_weak(best, rssi, std::memory_order_relaxed)) {}
    return usable && !queued.exchange(true);
  }
};

// How long a scan listened, and what hearing nothing means. Radio time runs
// from the controller's start to its end when both were seen, else the task's
// span. A scan that heard nothing was never confirmed by the
// controller (start-failed), or ended well before its length (aborted: cut
// short by someone else), or ran its course (missed).
struct ScanTiming { uint32_t durationMs; bool controller; ScanOutcome outcome; };
inline ScanTiming concludeScan(const Tally& t, ScanOutcome caller, uint32_t beganAt, uint32_t now,
                               uint8_t seconds) {
  const bool started = t.started.load();
  const uint32_t from = started ? t.startedAt.load() : beganAt, span = t.endSpan.load();
  const bool controller = started && span;
  ScanTiming s{controller ? span - 1 : now - from, controller, caller};
  if (caller == ScanOutcome::Missed && !started) s.outcome = ScanOutcome::StartFailed;
  else if (caller == ScanOutcome::Missed && s.durationMs + 500 < seconds * 1000u) s.outcome = ScanOutcome::Aborted;
  return s;
}

// The first disconnect reason seen for the armed target; later copies (one
// goes to every registered GATT app) are ignored.
struct ReasonLatch {
  std::atomic<uint16_t> reason{0};
  bool offer(uint16_t r) { uint16_t none = 0; return r && reason.compare_exchange_strong(none, r); }
  uint16_t take() { return reason.exchange(0); }
};

// The device's Wi-Fi around one record: state and flags at its start and end,
// the last STA disconnect before it, and Wi-Fi activity during it.
struct WifiSpan {
  WifiCoex start, end;
  uint8_t flagsStart, flagsEnd;
  uint8_t reason, ageS;                  // last STA disconnect at the start (ageS 255: none)
  uint8_t scans, connects, disconnects;  // during the record, capped at 255
  uint16_t scansAt, connectsAt, disconnectsAt;
  void begin(const WifiCoexTag& t) {
    start = end = t.state; flagsStart = flagsEnd = t.flags; reason = t.lastReason; ageS = t.lastDiscAgeS;
    scansAt = t.scans; connectsAt = t.connects; disconnectsAt = t.disconnects;
    scans = connects = disconnects = 0;
  }
  void finish(const WifiCoexTag& t) {
    end = t.state; flagsEnd = t.flags;
    scans = during(t.scans, scansAt); connects = during(t.connects, connectsAt);
    disconnects = during(t.disconnects, disconnectsAt);
  }
  static uint8_t during(uint16_t now, uint16_t at) {
    const uint16_t d = uint16_t(now - at);
    return d > 255 ? 255 : uint8_t(d);
  }
};

// Zero-initialized storage is "no record", so logs may live in PSRAM .bss.
struct ScanRecord {
  uint32_t seq, endedAt, durationMs, reports, matches, firstMatchMs;
  int8_t rssi;
  Peer peer; Purpose purpose; ScanOutcome outcome;
  bool controllerTimed;       // durationMs from controller events, not task ticks
  uint16_t interval, window;  // BLE units of 0.625 ms
  bool active, duplicateFilter; uint8_t seconds;
  WifiSpan wifi;
};
// G2 and ring: one record per Arduino BLEClient connect. Phone: one per
// attempt that found the phone, from GATT registration to READY or the first
// stop: connected means READY, stage is the step reached, gattStatus the first
// non-OK GATT status, reason the first disconnect reason for the phone's own
// address (the failed open's, or a live link's).
struct ConnectRecord {
  uint32_t seq, endedAt, elapsedMs;
  Peer peer; Purpose purpose; ConnectOutcome outcome; ConnectStage stage;
  int16_t gattStatus;  // -1: not reported
  uint16_t reason;     // disconnect reason (reasonName); 0: none observed
  uint8_t cause;       // the owner's end cause (phone: hw1pipe::Error, as in `phone stats`); 0: none
  CauseDetail detail;
  // The wait for the setup gate, capped at kWaitCap (printed "65534+").
  // Phone: from admission. Glasses and ring: from submission to the connect
  // worker, per job, so both temples' records of one job repeat it.
  // kNoWait: not measured.
  uint16_t waitMs;
  WifiSpan wifi;
  static constexpr uint16_t kNoWait = 0xffff;
  static constexpr uint16_t kWaitCap = 0xfffe;
  static uint16_t capWait(uint32_t ms) { return ms >= kWaitCap ? kWaitCap : uint16_t(ms); }
};

constexpr size_t kPeers = 6, kScanOutcomes = 6, kConnectOutcomes = 4, kKept = 16;

inline size_t clampWrite(int n, size_t cap) {
  return n < 0 ? 0 : size_t(n) < cap ? size_t(n) : cap ? cap - 1 : 0;
}
inline unsigned long ms100(uint16_t units) { return units * 625ul / 10; }  // hundredths of ms

// " wifi=a>b wflags=0x..>0x.. wlast=reason@age wscans=.. wconns=.. wdiscs=.."
inline size_t formatWifi(char* out, size_t cap, const WifiSpan& w) {
  char last[16] = "-";
  if (w.ageS != 255)
    std::snprintf(last, sizeof(last), "%u@%s%us", unsigned(w.reason), w.ageS == 254 ? ">" : "", unsigned(w.ageS));
  return clampWrite(std::snprintf(out, cap, " wifi=%s>%s wflags=0x%02x>0x%02x wlast=%s wscans=%u wconns=%u wdiscs=%u",
      wifiCoexName(w.start), wifiCoexName(w.end), unsigned(w.flagsStart), unsigned(w.flagsEnd), last,
      unsigned(w.scans), unsigned(w.connects), unsigned(w.disconnects)), cap);
}

// Newest-first rings of the last kKept records, with totals that cover the
// whole boot. Owners serialize add() and the readers.
struct Log {
  uint32_t scanTotal, connectTotal;
  uint32_t scans[kPeers][kScanOutcomes];        // [peer][outcome]
  uint32_t connects[kPeers][kConnectOutcomes];  // [peer][outcome]
  ScanRecord scan[kKept];
  ConnectRecord connect[kKept];

  ScanRecord& addScan(ScanRecord r) {
    r.seq = ++scanTotal;
    if (unsigned(r.peer) < kPeers && unsigned(r.outcome) < kScanOutcomes) ++scans[unsigned(r.peer)][unsigned(r.outcome)];
    ScanRecord& slot = scan[(r.seq - 1) % kKept]; slot = r; return slot;
  }
  ConnectRecord& addConnect(ConnectRecord r) {
    r.seq = ++connectTotal;
    if (unsigned(r.peer) < kPeers && unsigned(r.outcome) < kConnectOutcomes)
      ++connects[unsigned(r.peer)][unsigned(r.outcome)];
    ConnectRecord& slot = connect[(r.seq - 1) % kKept]; slot = r; return slot;
  }
  // "scans total=.. found=.. partial=.. missed=.. aborted=.. start_failed=.." for one peer.
  size_t scanCounters(char* out, size_t cap, Peer p) const {
    const uint32_t* c = scans[unsigned(p) < kPeers ? unsigned(p) : 0];
    return clampWrite(std::snprintf(out, cap, "scans total=%lu found=%lu partial=%lu missed=%lu aborted=%lu start_failed=%lu",
        (unsigned long)(c[1] + c[2] + c[3] + c[4] + c[5]), (unsigned long)c[1], (unsigned long)c[2],
        (unsigned long)c[3], (unsigned long)c[4], (unsigned long)c[5]), cap);
  }
  // "connects total=.. connected=.. failed=.. cancelled=.." for one peer.
  size_t connectCounters(char* out, size_t cap, Peer p) const {
    const uint32_t* c = connects[unsigned(p) < kPeers ? unsigned(p) : 0];
    return clampWrite(std::snprintf(out, cap, "connects total=%lu connected=%lu failed=%lu cancelled=%lu",
        (unsigned long)(c[1] + c[2] + c[3]), (unsigned long)c[1], (unsigned long)c[2], (unsigned long)c[3]), cap);
  }
  // Newest first, at most `max` whole lines, optionally one peer. A line that
  // would not fit is left out with the older ones, and a short note says so
  // when it fits: never a cut line, never past cap.
  size_t scanLines(char* out, size_t cap, uint32_t now, Peer only = Peer::None, size_t max = kKept) const {
    size_t used = 0, shown = 0;
    if (cap) out[0] = 0;
    for (uint32_t seq = scanTotal; seq && seq + kKept > scanTotal && shown < max; --seq) {
      const ScanRecord& r = scan[(seq - 1) % kKept];
      if (r.seq != seq) break;
      if (only != Peer::None && r.peer != only) continue;
      if (!fits(out, cap, used, formatScan(out + used, cap - used, r, now))) return used + omitted(out + used, cap - used);
      ++shown;
    }
    return used;
  }
  size_t connectLines(char* out, size_t cap, uint32_t now, Peer only = Peer::None, size_t max = kKept) const {
    size_t used = 0, shown = 0;
    if (cap) out[0] = 0;
    for (uint32_t seq = connectTotal; seq && seq + kKept > connectTotal && shown < max; --seq) {
      const ConnectRecord& r = connect[(seq - 1) % kKept];
      if (r.seq != seq) break;
      if (only != Peer::None && r.peer != only) continue;
      if (!fits(out, cap, used, formatConnect(out + used, cap - used, r, now))) return used + omitted(out + used, cap - used);
      ++shown;
    }
    return used;
  }
  // A line written at out+used reached the end of the buffer: it may be cut,
  // so take it back.
  static bool fits(char* out, size_t cap, size_t& used, size_t n) {
    if (used + n + 1 >= cap) { if (used < cap) out[used] = 0; return false; }
    used += n;
    return true;
  }
  static size_t omitted(char* out, size_t cap) {
    static const char kNote[] = "\n(older records not shown)";
    if (cap <= sizeof(kNote) - 1) return 0;
    std::memcpy(out, kNote, sizeof(kNote));
    return sizeof(kNote) - 1;
  }

  // "\n#seq -Ns " then the body; the BLE_COEX log line uses the same body.
  static size_t formatScan(char* out, size_t cap, const ScanRecord& r, uint32_t now) {
    size_t used = clampWrite(std::snprintf(out, cap, "\n#%lu -%lus ", (unsigned long)r.seq,
                                           (unsigned long)(uint32_t(now - r.endedAt) / 1000)), cap);
    return used + scanBody(out + used, cap - used, r);
  }
  static size_t scanBody(char* out, size_t cap, const ScanRecord& r, bool wifi = true) {
    char rssi[8] = "-", first[12] = "-";
    if (r.matches) std::snprintf(rssi, sizeof(rssi), "%d", int(r.rssi));
    if (r.firstMatchMs != Tally::kNone) std::snprintf(first, sizeof(first), "%lu", (unsigned long)r.firstMatchMs);
    const unsigned long i = ms100(r.interval), w = ms100(r.window);
    size_t used = clampWrite(std::snprintf(out, cap,
        "%s %s %s dur_ms=%lu(%s) first_ms=%s reports=%lu matches=%lu rssi=%s"
        " scan=%lu.%02lu/%lu.%02lu %s dupfilter=%s up_to_s=%u",
        name(r.peer), name(r.purpose), name(r.outcome), (unsigned long)r.durationMs,
        r.controllerTimed ? "ctl" : "task", first, (unsigned long)r.reports, (unsigned long)r.matches, rssi,
        i / 100, i % 100, w / 100, w % 100, r.active ? "active" : "passive", r.duplicateFilter ? "on" : "off",
        unsigned(r.seconds)), cap);
    return wifi ? used + formatWifi(out + used, cap - used, r.wifi) : used;
  }
  static size_t formatConnect(char* out, size_t cap, const ConnectRecord& r, uint32_t now) {
    size_t used = clampWrite(std::snprintf(out, cap, "\n#%lu -%lus ", (unsigned long)r.seq,
                                           (unsigned long)(uint32_t(now - r.endedAt) / 1000)), cap);
    return used + connectBody(out + used, cap - used, r);
  }
  static size_t connectBody(char* out, size_t cap, const ConnectRecord& r, bool wifi = true) {
    char gatt[8] = "-", cause[24] = "-", wait[8] = "-";
    if (r.gattStatus >= 0) std::snprintf(gatt, sizeof(gatt), "0x%02x", unsigned(r.gattStatus));
    if (r.cause || r.detail != CauseDetail::None)
      std::snprintf(cause, sizeof(cause), r.detail != CauseDetail::None ? "%u(%s)" : "%u%s", unsigned(r.cause), name(r.detail));
    if (r.waitMs != ConnectRecord::kNoWait)
      std::snprintf(wait, sizeof(wait), r.waitMs >= ConnectRecord::kWaitCap ? "%u+" : "%u", unsigned(r.waitMs));
    size_t used = clampWrite(std::snprintf(out, cap,
        "%s %s %s ms=%lu stage=%s gatt=%s reason=0x%02x(%s) cause=%s wait_ms=%s",
        name(r.peer), name(r.purpose), name(r.outcome), (unsigned long)r.elapsedMs, name(r.stage), gatt,
        unsigned(r.reason), reasonName(r.reason), cause, wait), cap);
    return wifi ? used + formatWifi(out + used, cap - used, r.wifi) : used;
  }
};

}  // namespace hw1coex

// ---- Runtime (BLE_CoexRecords.cpp; no-ops when compiled out) -------------

// Scans by the shared Arduino scanner (G2, ring). Task context: Begin just
// before the radio scan starts, End once its callbacks have quiesced.
void bleCoexScanBegin(hw1coex::Peer peer, hw1coex::Purpose purpose, uint16_t interval,
                      uint16_t window, bool active, bool duplicateFilter, uint8_t seconds);
// The outcome is decided by the caller; StartFailed is used when the
// controller never confirmed the start.
void bleCoexScanEnd(hw1coex::ScanOutcome outcome);
// Raw scan events from the custom GAP handler (BTC task; atomics only). The
// arguments are the esp_gap_ble_cb_event_t and its esp_ble_gap_cb_param_t.
void bleCoexOnGapEvent(int event, const void* param);
// A matching advertisement seen by the G2 or ring scan callback (BTC task).
void bleCoexOnScanMatch(int rssi);
// Did any match land in the current scan (task context, after quiescence)?
uint32_t bleCoexScanMatches();

// Connects through Arduino's BLEClient (G2 temples, ring). Begin publishes the
// target for the passive GATTC observer that captures the HCI reason; End is
// called with the client's outcome from the shared connect chokepoint.
void bleCoexConnectBegin(hw1coex::Peer peer, hw1coex::Purpose purpose, const uint8_t address[6]);
void bleCoexConnectEnd(bool ok, uint8_t stage, int32_t gattStatus, uint32_t elapsedMs);
// The connect worker's wait for the setup gate before its current G2 or ring
// job, from submission (worker task only). That job's connect records carry
// it as wait_ms; Clear ends it when the job does. LogSetupWait prints one line
// for a job another setup held up (50 ms or more), with or without a connect
// record (a missed re-seek makes none).
void bleCoexSetAccessoryWait(uint32_t waitMs);
void bleCoexClearAccessoryWait();
void bleCoexLogSetupWait(const char* job, uint32_t waitMs);
// Installs the passive GATTC observer once (Arduino's unused custom slot).
void bleCoexInstallGattcObserver();

// Records built by their owners (the phone pipe). Task context.
void bleCoexRecordScan(const hw1coex::ScanRecord& record);
void bleCoexRecordConnect(const hw1coex::ConnectRecord& record);
// The device's Wi-Fi state now (any task; no RPC).
WifiCoexTag bleCoexWifiNow();

// Readers (task context). Each writes at most cap-1 bytes and terminates.
size_t bleCoexScanCounters(char* out, size_t cap, hw1coex::Peer peer);
size_t bleCoexScanLines(char* out, size_t cap, uint32_t now, hw1coex::Peer only, size_t max);
size_t bleCoexConnectCounters(char* out, size_t cap, hw1coex::Peer peer);
size_t bleCoexConnectLines(char* out, size_t cap, uint32_t now, hw1coex::Peer only, size_t max);
// `blerecords [scans|connects|clear]`.
const char* bleCoexCommand(const char* args);
