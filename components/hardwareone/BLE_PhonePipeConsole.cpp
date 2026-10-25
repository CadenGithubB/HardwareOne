#include "BLE_PhonePipeConsole.h"
#if ENABLE_BLUETOOTH && ENABLE_BLE_PHONE_PIPE
// The app console as stream slot 2 of the phone pipe (phone link v2). The pipe
// task drives the stream under the owner mutex (L2): open and reopen, INFO,
// admitted records to cmd_exec, closes. Producers write records through
// pipeConsoleWrite under the chain mutex (L1) with short owner locks. The BLE
// row is reached only with L2 released (bind/end) and read through its leaf
// identity snapshot; Bluetooth.cpp reaches back only through the close mailbox.
// Chain paths log nothing (counters only): a line routed to BLE would come back
// here as output.
#include "BLE_CentralTx.h"
#include "BLE_PhonePairCode.h"
#include "BLE_PhonePipe.h"
#include "BLE_PhonePipeConsoleCore.h"
#include "BLE_PhonePipeCore.h"
#include "Bluetooth.h"
#include "System_SelfDevice.h"
#include "System_Settings.h"
#include "System_Utils.h"
#include <Arduino.h>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>
#include <cstring>

extern QueueHandle_t gCmdExecQ;          // HardwareOne.cpp: the executor's queue
extern TaskHandle_t gCmdExecTaskHandle;  // HardwareOne.cpp

namespace {
using hw1pipe::Session;
using hw1pipe::Stream;
using hw1pipe::StreamState;
using hw1pipe::expired;

constexpr uint32_t kBackoffMs[] = {2000, 5000, 10000};
constexpr uint32_t kPartialMs = 5000;   // a record the phone could finish and did not
constexpr uint32_t kBindMs = 3000;      // OPEN_OK to a bound row
constexpr uint32_t kRowLostMs = 1000;   // a bound row gone with no mailbox reason
constexpr uint32_t kStallMs = 10000, kCeilingMs = 15000;  // F-2: eligible / wall clock per accepted record
constexpr uint32_t kRetryMs = 10, kPushWaitMs = 20, kAbortWaitMs = 50;
constexpr unsigned kAbortTries = 20, kPoolSlots = 8, kRecordsPerPass = 32, kPendingEnds = 4;
constexpr size_t kSlotBytes = 512;
static_assert(hw1rec::kCommandMax <= kSlotBytes && hw1rec::kBinaryMax <= kSlotBytes,
              "an admitted record fits its slot");

// ---- Ingress pool --------------------------------------------------------
// One admitted COMMAND or BINARY on its way to cmd_exec. Login lines pass
// through here: the accepted plaintext-in-PSRAM exception (CONTRACT 2.11). A
// slot is all-zero unless it is being filled, queued or run; no default
// member initialisers, so the PSRAM .bss needs no constructor.
struct PoolSlot {
  uint32_t token;               // the stream it was admitted on
  TransportSessionEpoch epoch;  // the row's epoch at admission (the record's fence)
  uint16_t rowConnId, len;
  uint8_t type, reserved[3];
  uint8_t bytes[kSlotBytes];
};
static_assert(sizeof(PoolSlot) == 528, "8 x 528 B of PSRAM");
alignas(4) EXT_RAM_BSS_ATTR PoolSlot pool[kPoolSlots];
enum class Slot : uint8_t { Free, Filling, Queued, Running, Cancelled };
Slot slotState[kPoolSlots];  // poolMux (leaf)
portMUX_TYPE poolMux = portMUX_INITIALIZER_UNLOCKED;

PoolSlot* claimSlot() {
  PoolSlot* slot = nullptr;
  portENTER_CRITICAL(&poolMux);
  for (unsigned i = 0; i < kPoolSlots && !slot; ++i)
    if (slotState[i] == Slot::Free) { slotState[i] = Slot::Filling; slot = &pool[i]; }
  portEXIT_CRITICAL(&poolMux);
  return slot;
}
// The slot's holder lets it go, wiped unless cancelSlots already wiped it.
void freeSlot(unsigned i, bool wipe) {
  if (wipe) std::memset(&pool[i], 0, sizeof(PoolSlot));
  portENTER_CRITICAL(&poolMux);
  slotState[i] = Slot::Free;
  portEXIT_CRITICAL(&poolMux);
}
// cmd_exec: the record goes to its row (fenced on its epoch there), then the
// slot is wiped and freed. A cancelled slot never runs.
void runSlot(void* arg) {
  const unsigned i = unsigned(static_cast<PoolSlot*>(arg) - pool);
  if (i >= kPoolSlots) return;
  portENTER_CRITICAL(&poolMux);
  const bool run = slotState[i] == Slot::Queued;
  if (run) slotState[i] = Slot::Running;
  portEXIT_CRITICAL(&poolMux);
  if (run) blePipeConsoleInbound(pool[i].rowConnId, pool[i].epoch, pool[i].type, pool[i].bytes, pool[i].len);
  freeSlot(i, run);
}
// Records still queued for stream `token` (0: every stream) are wiped now and
// never run: they die with their stream or session. A running one finishes.
void cancelSlots(uint32_t token) {
  portENTER_CRITICAL(&poolMux);
  for (unsigned i = 0; i < kPoolSlots; ++i)
    if (slotState[i] == Slot::Queued && (!token || pool[i].token == token)) {
      std::memset(&pool[i], 0, sizeof(PoolSlot));
      slotState[i] = Slot::Cancelled;
    }
  portEXIT_CRITICAL(&poolMux);
}
bool submitSlot(PoolSlot* slot) {
  const unsigned i = unsigned(slot - pool);
  portENTER_CRITICAL(&poolMux);
  slotState[i] = Slot::Queued;
  portEXIT_CRITICAL(&poolMux);
  if (submitDeferredToCmdExec(runSlot, slot)) return true;
  freeSlot(i, true);
  return false;
}
// Two executor places: this record's job and the one it submits (the command,
// or the login job), so the second hop rarely finds the queue full (E-6).
bool executorRoom() { return gCmdExecQ && uxQueueSpacesAvailable(gCmdExecQ) >= 2; }

// ---- Driver state (pipe task, owner mutex) -------------------------------
struct Target { uint32_t generation; uint16_t streamId; };
bool same(const Target& t, uint32_t generation, uint16_t id) {
  return t.streamId && t.streamId == id && t.generation == generation;
}
// What the next OPEN waits for (CONTRACT 2.3).
enum class Hold : uint8_t { None, Backoff, FlagChange, WantedRise, Stopped };
struct Driver {
  bool session = false;
  uint32_t generation = 0;
  bool offered = false, wanted = false;  // last pass: flag edges
  Hold hold = Hold::None;
  uint32_t holdAt = 0, holdMs = 0;  // holdMs: Backoff's delay; WantedRise's quiet retry (0: the rise alone)
  uint8_t backoff = 0;      // next entry of kBackoffMs
  uint8_t quiet = 0;        // next quiet retry (kBackoffMs), apart from the error ladder
  bool wantedLow = false;   // WantedRise: CONSOLE_WANTED was seen 0
  uint16_t id = 0;          // the console stream (0: none)
  uint32_t token = 0;       // its pool token
  uint16_t closing = 0;     // the CLOSE reason this driver asked for (the first stands)
  bool info = false, bound = false, rowGone = false, partial = false;
  uint32_t infoAt = 0, rowGoneAt = 0, partialAt = 0;
  bool waiting = false;     // a whole record in rx that a gate refused (credit withheld)
  uint8_t waitGates = 0;    // the gates that held it (kGate*), each counted once
  uint32_t waitAt = 0, waitMark = 0;  // its first refused pass; withheld time counted up to
};
Driver drv;
uint32_t tokens = 0;

// Bind/end requests for pipeConsoleAfterUnlock, and its bind refusals back.
struct Pending {
  Target bind;
  Target ends[kPendingEnds];
  uint8_t endCount;
  Target bindRefused;
};
portMUX_TYPE pendingMux = portMUX_INITIALIZER_UNLOCKED;
Pending pending;

// The close mailbox: any task (the lifecycle lock may be held), consumed by the pass.
struct CloseRequest { Target target; uint16_t reason; };
portMUX_TYPE mailboxMux = portMUX_INITIALIZER_UNLOCKED;
CloseRequest mailbox;

// Status, read lock-free by pipeConsoleStatus.
std::atomic<uint8_t> shownState{uint8_t(PipeConsoleState::Off)};
std::atomic<bool> shownOffered{false}, shownWanted{false};
std::atomic<uint16_t> shownStream{0}, shownLastClose{0};
std::atomic<uint32_t> opens{0}, cardDrops{0}, mirrorDrops{0}, unknownRecords{0}, malformed{0}, aborts{0};
// Withheld ingress credit since boot (pipe task writes, status reads).
std::atomic<uint32_t> withheldMs{0}, withheldMaxMs{0}, poolFull{0}, executorBusy{0}, rowUnbound{0};

// The ingress gates. A whole record one of them refuses stays in rx, so its
// credit is withheld: one wait per record, from its first refused pass until
// it is admitted or its stream or session ends, timed as it goes (phone stats
// shows a wait still running). Each gate that held the record counts once.
constexpr uint8_t kGateRow = 1, kGateExecutor = 2, kGatePool = 4;
void waitCount(uint32_t now) {
  const uint32_t step = now - drv.waitMark, span = now - drv.waitAt;
  drv.waitMark = now;
  if (step) withheldMs.fetch_add(step);
  if (span > withheldMaxMs.load()) withheldMaxMs.store(span);  // one writer
}
void withhold(uint8_t gate, uint32_t now) {
  if (!drv.waiting) { drv.waiting = true; drv.waitGates = 0; drv.waitAt = drv.waitMark = now; }
  else waitCount(now);
  if (drv.waitGates & gate) return;
  drv.waitGates |= gate;
  (gate == kGateRow ? rowUnbound : gate == kGateExecutor ? executorBusy : poolFull).fetch_add(1);
}
void waitEnd(uint32_t now) {
  if (!drv.waiting) return;
  waitCount(now);
  drv.waiting = false; drv.waitGates = 0;
}

void requestEnd(uint32_t generation, uint16_t id) {
  portENTER_CRITICAL(&pendingMux);
  if (same(pending.bind, generation, id)) pending.bind = {};
  // The oldest end goes first: a later bind ends any earlier row anyway.
  if (pending.endCount == kPendingEnds) {
    for (unsigned i = 1; i < kPendingEnds; ++i) pending.ends[i - 1] = pending.ends[i];
    --pending.endCount;
  }
  pending.ends[pending.endCount++] = {generation, id};
  portEXIT_CRITICAL(&pendingMux);
}
void requestBind(uint32_t generation, uint16_t id) {
  portENTER_CRITICAL(&pendingMux);
  pending.bind = {generation, id};
  portEXIT_CRITICAL(&pendingMux);
}
Target takeBindRefused() {
  portENTER_CRITICAL(&pendingMux);
  const Target t = pending.bindRefused;
  pending.bindRefused = {};
  portEXIT_CRITICAL(&pendingMux);
  return t;
}
CloseRequest takeMailbox() {
  portENTER_CRITICAL(&mailboxMux);
  const CloseRequest m = mailbox;
  mailbox = {};
  portEXIT_CRITICAL(&mailboxMux);
  return m;
}

void backoffHold(uint32_t now, bool stopAfter) {
  if (drv.backoff < 3) { drv.hold = Hold::Backoff; drv.holdAt = now; drv.holdMs = kBackoffMs[drv.backoff++]; }
  else drv.hold = stopAfter ? Hold::Stopped : Hold::FlagChange;
}
// The phone refused or closed: a CONSOLE_WANTED rise reopens. Its LINK_STATE
// goes at most once a second and a 1->0->1 dip inside that sends nothing, so
// while wanted still reads 1 (the P4 never saw it fall) the stream also
// reopens after 2, 5, 10 s, then waits for the rise alone. A 0 seen meanwhile
// leaves only the rise. Its own ladder: the error backoff stays untouched. A
// step is spent when its timer reopens (the tick), not here: a wait the rise
// ended leaves it for a later dip.
void wantedRiseHold(uint32_t now) {
  drv.hold = Hold::WantedRise; drv.wantedLow = !drv.wanted; drv.holdAt = now;
  drv.holdMs = drv.wanted && drv.quiet < 3 ? kBackoffMs[drv.quiet] : 0;
}

// The stream ended inside a live session: how the next OPEN waits (CONTRACT 2.3).
void settle(const Stream& s, uint32_t now) {
  switch (drv.closing) {
    case 0: break;
    case uint16_t(hw1pipe::ConsoleClose::Error): backoffHold(now, false); return;
    // 2 renewed and 5 revoked at once; 3 once offered again; 4 once wanted
    // again (it closed because CONSOLE_WANTED fell, so 1 now is the rise, even
    // one that came while the CLOSE drained).
    default: drv.hold = Hold::None; return;
  }
  if (!s.remoteEnd) { wantedRiseHold(now); return; }    // the phone's CLOSE, any reason
  if (s.peerLimit) { backoffHold(now, false); return; } // a stream RESET after OPEN_OK
  if (s.error == hw1pipe::Error::ServiceOff) wantedRiseHold(now);
  else if (s.error == hw1pipe::Error::Unsupported) drv.hold = Hold::Stopped;
  else backoffHold(now, s.error == hw1pipe::Error::Busy);
}

// The driver lets go of its stream: queued records are wiped, the row ends
// after the unlock, and the ended slot is free for the next OPEN.
void letGo(Session& session, uint32_t now) {
  waitEnd(now);
  cancelSlots(drv.token);
  requestEnd(drv.generation, drv.id);
  session.release(drv.id);
  drv.id = 0; drv.token = 0; drv.closing = 0;
  drv.info = drv.bound = drv.rowGone = drv.partial = false;
}
// The link went or a new session began: the stream ends with it (no CLOSE)
// and the next READY starts afresh (start() has reset the session's slots).
void sessionEnded(uint32_t now) {
  waitEnd(now);
  if (drv.id) requestEnd(drv.generation, drv.id);
  cancelSlots(0);
  drv = Driver{};
}
void closeStream(Session& session, uint32_t now, uint16_t reason) {
  waitEnd(now);  // nothing more is admitted from this stream
  drv.closing = reason; drv.partial = false;
  cancelSlots(drv.token);  // its records die with the stream, as with its row
  session.closeAfterDrain(drv.id, now, reason);
}

// INFO is the stream's first record, written into the empty tx at OPEN_OK,
// before the row is bound (so before any writer can reach the stream).
bool writeInfo(Session& session) {
  char info[hw1rec::kInfoMax + 1];
  const size_t n = hw1rec::buildInfo(info, sizeof info, SelfDevice::firmwareVersion(), BOARD_NAME);
  uint8_t head[hw1rec::kHeader];
  hw1rec::encodeHeader(head, hw1rec::kInfo, 0, uint16_t(n));
  return n && session.pushRecord(drv.id, head, sizeof head, reinterpret_cast<const uint8_t*>(info), n, 0) ==
                  hw1pipe::PushResult::Ok;
}

// The row is the session's only way in and out. Not bound 3 s after INFO
// (bind refused or never run): CLOSE 1. Gone for 1 s after it was bound with
// no mailbox reason: CLOSE 2 (renewed: a fresh row at once).
uint16_t rowCheck(const BlePipeConsoleIdentity& row, uint32_t now) {
  const bool ours = row.bound && row.generation == drv.generation && row.streamId == drv.id && row.epoch;
  if (ours) { drv.bound = true; drv.rowGone = false; return 0; }
  if (!drv.bound) return expired(now, drv.infoAt, kBindMs) ? uint16_t(hw1pipe::ConsoleClose::Error) : 0;
  if (!drv.rowGone) { drv.rowGone = true; drv.rowGoneAt = now; return 0; }
  return expired(now, drv.rowGoneAt, kRowLostMs) ? uint16_t(hw1pipe::ConsoleClose::Renewed) : 0;
}

// Whole records from rx to cmd_exec. A record is consumed (and its credit
// returned) only once it is in a pool slot and submitted; a missing row, slot
// or executor place leaves it in rx, which withholds credit and drops nothing
// (timed and counted per gate: withhold).
void ingress(Session& session, const Stream& s, const BlePipeConsoleIdentity& row, uint32_t now) {
  const bool ours = drv.bound && !drv.rowGone && row.epoch;
  for (unsigned n = 0; n < kRecordsPerPass; ++n) {
    const size_t have = session.rxAvailable(drv.id);
    if (!have) { drv.partial = false; return; }
    uint8_t head[hw1rec::kScanPeek];
    session.rxPeek(drv.id, head, have < sizeof head ? have : sizeof head);
    const hw1rec::Scan r = hw1rec::scanIngress(head, have);
    if (r.verdict == hw1rec::Verdict::Malformed) {
      malformed.fetch_add(1);
      closeStream(session, now, uint16_t(hw1pipe::ConsoleClose::Error));
      return;
    }
    if (r.verdict == hw1rec::Verdict::NeedMore) {
      // Late only while the phone's credit covers the rest: withheld credit
      // (a full pool, display pressure) is ours, not the phone's.
      if (s.receiveLimit - s.received < r.size - have) { drv.partial = false; return; }
      if (!drv.partial) { drv.partial = true; drv.partialAt = now; }
      else if (expired(now, drv.partialAt, kPartialMs)) {
        malformed.fetch_add(1);
        closeStream(session, now, uint16_t(hw1pipe::ConsoleClose::Error));
      }
      return;
    }
    drv.partial = false;
    if (r.verdict == hw1rec::Verdict::Skip) {  // unknown or reserved: skipped and counted
      session.consume(drv.id, r.size);
      unknownRecords.fetch_add(1);
      continue;
    }
    if (!ours) { withhold(kGateRow, now); return; }
    if (!executorRoom()) { withhold(kGateExecutor, now); return; }
    PoolSlot* slot = claimSlot();
    if (!slot) { withhold(kGatePool, now); return; }
    slot->token = drv.token; slot->epoch = row.epoch; slot->rowConnId = row.rowConnId;
    slot->type = r.header.type; slot->len = r.header.len;
    if (session.rxPeek(drv.id, slot->bytes, r.header.len, hw1rec::kHeader) != r.header.len) {
      freeSlot(unsigned(slot - pool), true);
      return;
    }
    // Another producer took the executor's places since executorRoom().
    if (!submitSlot(slot)) { withhold(kGateExecutor, now); return; }
    session.consume(drv.id, r.size);
    waitEnd(now);
    phonePipeNoteConsoleActivity();
    // A bound stream that carried a command resets the reopen ladders.
    if (r.header.type == hw1rec::kCommand) drv.backoff = drv.quiet = 0;
  }
}

void streamPass(Session& session, uint32_t now, const CloseRequest& asked, const Target& refused) {
  Stream* s = session.find(drv.id);
  if (!s) { letGo(session, now); return; }  // restarted under us (cannot happen in one READY session)
  if (s->closeReason) shownLastClose.store(s->closeReason);
  if (s->state == StreamState::Ended) { settle(*s, now); letGo(session, now); return; }
  const bool open = s->state == StreamState::Open;
  BlePipeConsoleIdentity row;
  if (open && drv.info) blePipeConsoleIdentity(&row);
  if ((open || s->state == StreamState::Opening) && !s->draining && !drv.closing) {
    uint16_t why = 0;
    if (same(asked.target, drv.generation, drv.id)) why = asked.reason;
    else if (same(refused, drv.generation, drv.id)) why = uint16_t(hw1pipe::ConsoleClose::Error);
    else if (!drv.offered) why = uint16_t(hw1pipe::ConsoleClose::ConsoleOff);
    else if (!drv.wanted) why = uint16_t(hw1pipe::ConsoleClose::NotWanted);
    else if (open && drv.info) why = rowCheck(row, now);
    if (why) closeStream(session, now, why);
  }
  if (open && !s->draining && !drv.closing) {
    if (!drv.info) {
      if (!writeInfo(session)) closeStream(session, now, uint16_t(hw1pipe::ConsoleClose::Error));
      else { drv.info = true; drv.infoAt = now; requestBind(drv.generation, drv.id); }
    } else {
      ingress(session, *s, row, now);
    }
  }
  if (s->closeReason) shownLastClose.store(s->closeReason);  // a CLOSE queued in this pass
}

PipeConsoleState stateOf(const Session& session) {
  if (drv.id) {
    const Stream* s = session.find(drv.id);
    if (!s || s->state == StreamState::Opening) return PipeConsoleState::Opening;
    if (s->state != StreamState::Open || s->draining || drv.closing) return PipeConsoleState::Closing;
    return drv.bound && !drv.rowGone ? PipeConsoleState::Open : PipeConsoleState::Opening;
  }
  if (drv.hold == Hold::Stopped) return PipeConsoleState::Stopped;
  if (drv.hold == Hold::Backoff || drv.hold == Hold::FlagChange) return PipeConsoleState::Backoff;
  return drv.offered ? PipeConsoleState::Waiting : PipeConsoleState::NotOffered;
}

// ---- Writers (chain mutex, then short owner locks) -----------------------
StaticSemaphore_t chainStorage;
SemaphoreHandle_t chainMutex = nullptr;
portMUX_TYPE initMux = portMUX_INITIALIZER_UNLOCKED;
// The stream the last chain went to and its state, under the chain mutex. A
// chain whose ABORT could not go yet stays open here, and the next writer on
// that stream ends it before anything else.
PipeConsoleTarget chainTarget;
hw1rec::Egress chain;
// A Reliable write stalled on chainTarget in this epoch (CONC-1). The budget
// runs per episode, not per write: until a record goes there, a Reliable write
// tries once without waiting for room, so a command's batches and the queued
// commands after it do not each wait their own 10-15 s.
bool stalled = false;
uint32_t stalledEpoch = 0;

bool chainReady() {
  if (chainMutex) return true;
  portENTER_CRITICAL(&initMux);
  if (!chainMutex) chainMutex = xSemaphoreCreateMutexStatic(&chainStorage);
  portEXIT_CRITICAL(&initMux);
  return chainMutex != nullptr;
}
class ChainLock {
 public:
  explicit ChainLock(TickType_t wait) : held(xSemaphoreTake(chainMutex, wait) == pdTRUE) {}
  ~ChainLock() { if (held) xSemaphoreGive(chainMutex); }
  explicit operator bool() const { return held; }
 private:
  bool held;
};

bool onCmdExec() { return gCmdExecTaskHandle && xTaskGetCurrentTaskHandle() == gCmdExecTaskHandle; }
// The output's session is still the stream's bound row (a logout, revoke or a
// new row ends it).
bool rowLive(const PipeConsoleTarget& t, uint32_t epoch) {
  BlePipeConsoleIdentity row;
  blePipeConsoleIdentity(&row);
  return row.bound && row.generation == t.generation && row.streamId == t.streamId && row.epoch && row.epoch == epoch;
}
uint8_t recordType(PipeConsoleKind kind) {
  switch (kind) {
    case PipeConsoleKind::Text: return hw1rec::kText;
    case PipeConsoleKind::Blob: return hw1rec::kBlob;
    case PipeConsoleKind::Card: return hw1rec::kCard;
    case PipeConsoleKind::Mirror: return hw1rec::kMirror;
  }
  return 0;
}
PipeConsoleWrite dropped(uint8_t type) {
  if (type == hw1rec::kCard) cardDrops.fetch_add(1);
  else if (type == hw1rec::kMirror) mirrorDrops.fetch_add(1);
  return PipeConsoleWrite::Dropped;
}

// The Reliable writer's wait between tries, owner mutex released: false once
// 10 s of eligible time (display not busy) or 15 s of wall clock passed
// without an accepted record.
struct Stall {
  uint32_t progress, mark, eligible;
  bool busy;
  explicit Stall(uint32_t now) : progress(now), mark(now), eligible(0), busy(bleCentralTxAccessoryPressure()) {}
  void accepted(uint32_t now) { progress = mark = now; eligible = 0; }
  bool wait() {
    const uint32_t now = millis();
    if (!busy) eligible += now - mark;
    mark = now;
    if (eligible >= kStallMs || now - progress >= kCeilingMs) return false;
    busy = bleCentralTxAccessoryPressure();
    vTaskDelay(pdMS_TO_TICKS(kRetryMs));
    return true;
  }
};

enum class Put : uint8_t { Ok, Full, Gone, Stalled };
// One whole record with 4 B left over for an ABORT. With no stall it never
// waits for room; TryNow also takes the owner mutex only if free (lockWaitMs
// 0). Any record taken on chainTarget ends a stall episode there.
Put put(const PipeConsoleTarget& t, uint32_t epoch, Stall* stall, uint32_t lockWaitMs, uint8_t type,
        uint8_t flags, const uint8_t* payload, size_t n) {
  uint8_t head[hw1rec::kHeader];
  hw1rec::encodeHeader(head, type, flags, uint16_t(n));
  for (;;) {
    if (!rowLive(t, epoch)) return Put::Gone;
    const PhonePipePush r = phonePipeConsolePush(t.generation, t.streamId, head, sizeof head, payload, n,
                                                 hw1rec::kHeader, lockWaitMs);
    if (r == PhonePipePush::Ok) {
      stalled = false;
      if (stall) stall->accepted(millis());
      return Put::Ok;
    }
    if (r == PhonePipePush::Gone) return Put::Gone;
    if (!stall) return Put::Full;
    if (!stall->wait()) return Put::Stalled;
  }
}
// Ends the open chain with ABORT (no reserve: every push kept 4 B for it).
// True once the chain is settled: ABORT went, or the stream is no longer open
// (its CLOSE shows the chain as not confirmed). False while the owner mutex
// was busy: the chain stays open for the next writer.
bool endChain(const PipeConsoleTarget& t, bool wait) {
  uint8_t head[hw1rec::kHeader];
  hw1rec::encodeHeader(head, hw1rec::kAbort, 0, 0);
  for (unsigned i = 0;; ++i) {
    const PhonePipePush r = phonePipeConsolePush(t.generation, t.streamId, head, sizeof head, nullptr, 0, 0,
                                                 wait ? kAbortWaitMs : 0);
    if (r == PhonePipePush::Ok) { chain.commit(hw1rec::kAbort, 0, 0); return true; }
    if (r == PhonePipePush::Gone) { chain.reset(); return true; }
    if (!wait || i + 1 >= kAbortTries) return false;
    vTaskDelay(pdMS_TO_TICKS(kRetryMs));
  }
}
}  // namespace

PipeConsoleWrite pipeConsoleWrite(const PipeConsoleTarget& target, uint32_t epoch, PipeConsoleKind kind,
                                  const uint8_t* data, size_t len, PipeConsoleMode mode) {
  const uint8_t type = recordType(kind);
  const bool text = type == hw1rec::kText || type == hw1rec::kBlob;
  // A chain carries one whole result (the executor caps results below it);
  // a card is one record; a mirror longer than a record goes as several.
  if (!type || !data || !len || !target.streamId || (text && !hw1rec::chainFits(len)) ||
      (type == hw1rec::kCard && len > hw1rec::kCardMax)) {
    return PipeConsoleWrite::Refused;
  }
  // A caller holding the owner or the chain mutex would wait on itself (the
  // pipe task, an owner command's output, a nested write); one holding the
  // lifecycle lock would take L1 under L3, which the lock order forbids.
  if (phonePipeOwnerHeldByCurrentTask() || pipeConsoleHeldByCurrentTask() || bleLifecycleHeldByCurrentTask() ||
      !chainReady()) {
    return PipeConsoleWrite::Refused;
  }
  if (!rowLive(target, epoch)) return PipeConsoleWrite::Gone;
  // Only cmd_exec waits for room (F-2); CARD and MIRROR never do.
  const bool reliable = mode == PipeConsoleMode::Reliable && text && onCmdExec();
  Stall stall(millis());
  // TryNow never waits for a chain in progress: nothing interleaves with it.
  ChainLock lock(reliable ? pdMS_TO_TICKS(kCeilingMs) : 0);
  if (!lock) return dropped(type);
  if (chainTarget.generation != target.generation || chainTarget.streamId != target.streamId) {
    chain.reset();
    chainTarget = target;
    stalled = false;
  }
  // Stalled already this episode: one try (still the Reliable lock wait, as
  // the owner pass holds L2 every 10 ms) until a record goes, then it waits.
  Stall* waits = reliable && !(stalled && stalledEpoch == epoch) ? &stall : nullptr;
  const uint32_t lockWait = reliable ? kPushWaitMs : 0;
  if (chain.chainOpen() && !endChain(target, reliable)) return dropped(type);
  if (!text) {
    for (size_t sent = 0; sent < len;) {
      const size_t n = len - sent < hw1rec::kMirrorMax ? len - sent : hw1rec::kMirrorMax;
      const Put r = put(target, epoch, nullptr, 0, type, 0, data + sent, n);
      if (r == Put::Gone) return PipeConsoleWrite::Gone;
      if (r != Put::Ok) return dropped(type);
      sent += n;
    }
    return PipeConsoleWrite::Sent;
  }
  for (size_t sent = 0; sent < len;) {
    const size_t rest = len - sent;
    const uint16_t n = hw1rec::sliceLen(rest);
    const uint8_t flags = hw1rec::sliceFlags(rest);
    // chainFits() above keeps every slice legal; a refusal here still cuts what went.
    const bool legal = chain.check(type, flags, n) == hw1rec::Fault::None;
    const Put r = legal ? put(target, epoch, waits, lockWait, type, flags, data + sent, n) : Put::Gone;
    if (r == Put::Ok) {
      chain.commit(type, flags, n);
      phonePipeNoteConsoleActivity();
      sent += n;
      if (reliable && !waits) { stall.accepted(millis()); waits = &stall; }  // the episode is over
      continue;
    }
    if (r == Put::Stalled) { stalled = true; stalledEpoch = epoch; }
    // A stall, the ceiling, a new epoch or the stream's end cut an open chain.
    if (chain.chainOpen()) {
      aborts.fetch_add(1);
      (void)endChain(target, reliable);
      return PipeConsoleWrite::Aborted;
    }
    if (!legal) return PipeConsoleWrite::Refused;
    return r == Put::Gone ? PipeConsoleWrite::Gone : PipeConsoleWrite::Dropped;
  }
  return PipeConsoleWrite::Sent;
}

uint8_t pipeConsoleLocalFlags() {
  // The owner calls this only from tickLocked on an active pipe (publishSummary
  // reads the session's flags). The secret's writers (phone secret, pair new,
  // factory reset) stop the pipe first, then phonePipeInvalidateSecret takes
  // the owner mutex: the last read here comes before their write.
  const String& secret = gSettings.phonePipeSecret;
  // Only a device-made code offers the console: `phone pair new` sets the mark.
  const bool offered = gSettings.bleConsoleOnPipe && gSettings.phonePipeSecretGenerated &&
                       hw1code::isCode(secret.c_str(), secret.length());
  shownOffered.store(offered);
  return offered ? hw1pipe::kP4ConsoleOffered : 0;
}

void pipeConsoleTickLocked(hw1pipe::Session& session, uint32_t generation, bool ready, uint32_t now,
                           bool consoleDisplayBusy) {
  // The partial-record clock follows the phone's credit, which display
  // pressure already withholds; nothing else here yields to it.
  (void)consoleDisplayBusy;
  const bool up = ready && session.state == hw1pipe::State::Ready;
  if (drv.session && (!up || generation != drv.generation)) sessionEnded(now);
  const CloseRequest asked = takeMailbox();
  const Target refused = takeBindRefused();
  if (!up) {
    shownState.store(uint8_t(PipeConsoleState::Off));
    shownWanted.store(false);
    shownStream.store(0);
    return;
  }
  const bool offered = (session.localFlags() & hw1pipe::kP4ConsoleOffered) != 0;
  const bool wanted = (session.peerFlags() & hw1pipe::kPhoneConsoleWanted) != 0;
  if (!drv.session) {  // a new READY session: no holds, the backoff from the start
    drv = Driver{};
    drv.session = true; drv.generation = generation; drv.offered = offered; drv.wanted = wanted;
  }
  const bool changed = offered != drv.offered || wanted != drv.wanted;
  drv.offered = offered; drv.wanted = wanted;
  if (drv.hold == Hold::Backoff && expired(now, drv.holdAt, drv.holdMs)) drv.hold = Hold::None;
  else if (drv.hold == Hold::FlagChange && changed) drv.hold = Hold::None;
  else if (drv.hold == Hold::WantedRise) {
    if (!wanted) drv.wantedLow = true;
    else if (drv.wantedLow) drv.hold = Hold::None;
    else if (drv.holdMs && expired(now, drv.holdAt, drv.holdMs)) { drv.hold = Hold::None; ++drv.quiet; }
  }
  if (drv.id) streamPass(session, now, asked, refused);
  if (!drv.id && drv.hold == Hold::None && offered && wanted &&
      session.streams[hw1pipe::kConsoleSlot].state == StreamState::Free) {
    if (const uint16_t id = session.openConsole(now)) {
      drv.id = id;
      drv.token = ++tokens ? tokens : ++tokens;
      opens.fetch_add(1);
    }
  }
  shownState.store(uint8_t(stateOf(session)));
  shownWanted.store(wanted);
  shownStream.store(drv.id);
}

void pipeConsoleAfterUnlock() {
  portENTER_CRITICAL(&pendingMux);
  const Pending p = pending;
  pending.bind = {};
  pending.endCount = 0;
  portEXIT_CRITICAL(&pendingMux);
  for (unsigned i = 0; i < p.endCount; ++i) blePipeConsoleEnd(p.ends[i].generation, p.ends[i].streamId);
  if (!p.bind.streamId) return;
  // Bound only while that session is still the READY one (its link's id is for display).
  PhonePipeLinkRecord link;
  const bool live = phonePipeLinkRecord(&link) && link.generation == p.bind.generation;
  if (live && blePipeConsoleBind(p.bind.generation, p.bind.streamId, link.connId)) return;
  portENTER_CRITICAL(&pendingMux);
  pending.bindRefused = p.bind;
  portEXIT_CRITICAL(&pendingMux);
}

void pipeConsoleRequestClose(uint32_t generation, uint16_t streamId, uint16_t reason) {
  if (!streamId || reason < uint16_t(hw1pipe::ConsoleClose::Error) ||
      reason > uint16_t(hw1pipe::ConsoleClose::Revoked)) {
    return;
  }
  portENTER_CRITICAL(&mailboxMux);
  // The first reason for a stream stands, as in closeAfterDrain.
  if (!same(mailbox.target, generation, streamId)) mailbox = {{generation, streamId}, reason};
  portEXIT_CRITICAL(&mailboxMux);
}

bool pipeConsoleHeldByCurrentTask() {
  return chainMutex && xSemaphoreGetMutexHolder(chainMutex) == xTaskGetCurrentTaskHandle();
}

void pipeConsoleStatus(PipeConsoleStatus* out) {
  if (!out) return;
  PipeConsoleStatus s;
  // The owner stops driving the console when the session ends: READY is the truth.
  const bool up = phonePipeReady();
  s.state = up ? PipeConsoleState(shownState.load()) : PipeConsoleState::Off;
  s.offered = shownOffered.load();
  s.wanted = up && shownWanted.load();
  s.streamId = up ? shownStream.load() : 0;
  s.lastClose = shownLastClose.load();
  s.opens = opens.load(); s.cardDrops = cardDrops.load(); s.mirrorDrops = mirrorDrops.load();
  s.unknown = unknownRecords.load(); s.malformed = malformed.load(); s.aborts = aborts.load();
  s.withheldMs = withheldMs.load(); s.withheldMaxMs = withheldMaxMs.load();
  s.poolFull = poolFull.load(); s.executorBusy = executorBusy.load(); s.rowUnbound = rowUnbound.load();
  *out = s;
}
#endif
