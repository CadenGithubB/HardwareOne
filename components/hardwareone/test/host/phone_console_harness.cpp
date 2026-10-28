// Host co-simulation of the phone link v2 console driver (test_phone_console.py).
//
// BLE_PhonePipeConsole.cpp is included as shipped, so its pool and state are
// in reach. The real hw1pipe::Session stands for the pipe owner's session; a
// phone answers it frame by frame; the BLE row (Bluetooth.cpp) and the owner
// API (BLE_PhonePipe.cpp) are recording doubles. Tasks are simulated on one
// clock: the pipe task's owner pass, cmd_exec, the main loop, the debug task
// and httpd. A writer that sleeps lets the others run (the owner pass, plus
// whatever a scenario adds).
//
// Lock-order oracle, checked on every acquisition: L1 (chain) is never taken
// by the pipe task or under L2/L3; L2 (owner) never under L3; L3 (lifecycle)
// never under L1 or L2; no task re-takes a non-recursive mutex; nothing blocks
// inside a leaf portMUX; the pipe task never sleeps (never waits for room) and
// a writer sleeps with L2 released.
#include "BLE_PhonePipeConsole.cpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#define REQUIRE(c)                                                          \
  do {                                                                      \
    if (!(c)) {                                                             \
      std::fprintf(stderr, "%s:%d: REQUIRE(%s)\n", __FILE__, __LINE__, #c); \
      std::abort();                                                         \
    }                                                                       \
  } while (0)

// ---- FreeRTOS, Arduino and lock doubles ----------------------------------
struct HostTask { const char* name; };
struct HostSem { const char* name; HostTask* holder; };
struct HostQueue { size_t capacity; std::deque<std::pair<ExecReq::DeferredFn, void*>> items; };

HostTask workerTask{"phone_pipe"}, execTask{"cmd_exec"}, mainTask{"main"}, debugTask{"debug_output"},
    httpdTask{"httpd"};
HostTask* current = &mainTask;
TaskHandle_t gCmdExecTaskHandle = &execTask;
HostQueue execQueue{24, {}};
QueueHandle_t gCmdExecQ = &execQueue;
HostSem chainSem{"L1", nullptr}, ownerSem{"L2", nullptr}, lifecycleSem{"L3", nullptr};
int critical = 0;
unsigned chainCreates = 0, chainTakes = 0, ownerTakesUnderChain = 0, lifecycleTakes = 0;
uint32_t clockMs = 1000;

struct HostAs {
  HostTask* saved;
  explicit HostAs(HostTask& t) : saved(current) { current = &t; }
  ~HostAs() { current = saved; }
};
uint32_t millis() { return clockMs; }
void hostEnterCritical(portMUX_TYPE* m) { REQUIRE(!m->held); m->held = 1; ++critical; }
void hostExitCritical(portMUX_TYPE* m) { REQUIRE(m->held); m->held = 0; --critical; }
bool holds(const HostSem& s) { return s.holder == current; }
BaseType_t take(HostSem& s, TickType_t wait) {
  REQUIRE(critical == 0);         // nothing blocks inside a leaf lock
  REQUIRE(s.holder != current);   // non-recursive: its holder taking it again deadlocks
  if (&s == &chainSem) { REQUIRE(current != &workerTask && !holds(ownerSem) && !holds(lifecycleSem)); ++chainTakes; }
  if (&s == &ownerSem) { REQUIRE(!holds(lifecycleSem)); if (holds(chainSem)) ++ownerTakesUnderChain; }
  if (&s == &lifecycleSem) { REQUIRE(!holds(chainSem) && !holds(ownerSem)); ++lifecycleTakes; }
  if (!s.holder) { s.holder = current; return pdTRUE; }
  // Held by a task the simulation suspended: it cannot give it back meanwhile.
  REQUIRE(wait != portMAX_DELAY);
  clockMs += wait;
  return pdFALSE;
}
void give(HostSem& s) { REQUIRE(s.holder == current); s.holder = nullptr; }
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t*) { ++chainCreates; return &chainSem; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) { return take(*s, wait); }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { give(*s); return pdTRUE; }
TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t s) { return s->holder; }
TaskHandle_t xTaskGetCurrentTaskHandle() { return current; }

std::function<void()> whileAsleep;  // the other tasks' turn while a writer sleeps
unsigned sleeps = 0;
void vTaskDelay(TickType_t ticks) {
  REQUIRE(current != &workerTask);  // the pipe task is the only drainer: it never waits for room
  REQUIRE(!holds(ownerSem) && !holds(lifecycleSem) && critical == 0);
  ++sleeps;
  clockMs += ticks;
  if (whileAsleep) { const auto turn = whileAsleep; turn(); }
}

unsigned execDrops = 0;
UBaseType_t uxQueueSpacesAvailable(QueueHandle_t q) { return UBaseType_t(q->capacity - q->items.size()); }
bool submitDeferredToCmdExec(ExecReq::DeferredFn fn, void* arg) {
  REQUIRE(fn && critical == 0);
  if (execQueue.items.size() >= execQueue.capacity) { ++execDrops; return false; }
  execQueue.items.push_back({fn, arg});
  return true;
}
void otherProducer(void*) {}
void runExec() {
  HostAs as(execTask);
  while (!execQueue.items.empty()) {
    const auto item = execQueue.items.front();
    execQueue.items.pop_front();
    item.first(item.second);
  }
}

Settings gSettings;
namespace SelfDevice {
const char* firmwareVersion() { return "0.99.99 \"host\""; }
}

// ---- The BLE row (Bluetooth.cpp) -----------------------------------------
BlePipeConsoleIdentity row;  // the leaf snapshot
TransportSessionEpoch epochs = 500;
bool refuseBind = false, bindInvisible = false;
unsigned binds = 0, ends = 0;
uint16_t lastLink = 0;
struct Inbound { uint16_t rowConnId; TransportSessionEpoch epoch; uint8_t type; std::string bytes; };
std::vector<Inbound> inbound;
struct LifecycleGuard {
  LifecycleGuard() { REQUIRE(take(lifecycleSem, 0) == pdTRUE); }
  ~LifecycleGuard() { give(lifecycleSem); }
};
bool bleLifecycleHeldByCurrentTask() { return holds(lifecycleSem); }
bool blePipeConsoleBind(uint32_t generation, uint16_t streamId, uint16_t linkConnId) {
  REQUIRE(current == &workerTask && critical == 0);
  LifecycleGuard l3;
  ++binds;
  lastLink = linkConnId;
  if (refuseBind) return false;
  if (!bindInvisible) {
    row = BlePipeConsoleIdentity{};
    row.bound = true; row.generation = generation; row.streamId = streamId;
    row.rowConnId = blePipeConsoleRowId(generation); row.epoch = ++epochs;
  }
  return true;
}
void blePipeConsoleEnd(uint32_t generation, uint16_t streamId) {
  REQUIRE(current == &workerTask && critical == 0);
  LifecycleGuard l3;
  ++ends;
  if (row.bound && row.generation == generation && row.streamId == streamId) row = {};
}
void blePipeConsoleIdentity(BlePipeConsoleIdentity* out) { REQUIRE(out); *out = row; }
void blePipeConsoleInbound(uint16_t rowConnId, TransportSessionEpoch epoch, uint8_t type, const uint8_t* bytes,
                           size_t len) {
  REQUIRE(current == &execTask && critical == 0);
  LifecycleGuard l3;
  inbound.push_back({rowConnId, epoch, type, std::string(reinterpret_cast<const char*>(bytes), len)});
}
// bledisconnect or a revoke: the row ends under L3, then the close is posted.
void rowEndedBy(uint16_t reason) {
  HostAs as(httpdTask);
  LifecycleGuard l3;
  const BlePipeConsoleIdentity was = row;
  row = {};
  pipeConsoleRequestClose(was.generation, was.streamId, reason);
}
void signIn() {  // login (or logout) rotates the row's epoch
  HostAs as(httpdTask);
  LifecycleGuard l3;
  row.epoch = ++epochs;
  row.authed = true;
}

// ---- The pipe owner (BLE_PhonePipe.cpp) ----------------------------------
hw1pipe::Session session;
uint32_t generation = 40;
bool ready = false, displayBusy = false, ownerBusy = false;
size_t busyFrom = SIZE_MAX;  // pushes from this index on find the owner mutex busy
constexpr uint16_t kLink = 0x21;
unsigned activity = 0;
struct PushLog { uint8_t type; size_t headLen, n, reserve; uint32_t wait; PhonePipePush result; };
std::vector<PushLog> pushes;
PhonePipePush phonePipeConsolePush(uint32_t g, uint16_t id, const uint8_t* head, size_t headLen,
                                   const uint8_t* payload, size_t n, size_t reserve, uint32_t lockWaitMs) {
  REQUIRE(current != &workerTask && critical == 0 && !holds(lifecycleSem));
  PushLog log{headLen ? head[0] : uint8_t(0), headLen, n, reserve, lockWaitMs, PhonePipePush::Busy};
  if (ownerBusy || pushes.size() >= busyFrom) {
    clockMs += lockWaitMs;
    pushes.push_back(log);
    return log.result;
  }
  REQUIRE(take(ownerSem, 0) == pdTRUE);
  if (!ready || g != generation) log.result = PhonePipePush::Gone;
  else switch (session.pushRecord(id, head, headLen, payload, n, reserve)) {
    case hw1pipe::PushResult::Ok: log.result = PhonePipePush::Ok; break;
    case hw1pipe::PushResult::NoSpace: log.result = PhonePipePush::NoSpace; break;
    case hw1pipe::PushResult::Gone: log.result = PhonePipePush::Gone; break;
  }
  give(ownerSem);
  pushes.push_back(log);
  return log.result;
}
bool phonePipeOwnerHeldByCurrentTask() { return holds(ownerSem); }
void phonePipeNoteConsoleActivity() { ++activity; }
bool phonePipeLinkRecord(PhonePipeLinkRecord* out) {
  REQUIRE(out);
  if (!ready) { *out = {}; return false; }
  *out = PhonePipeLinkRecord{generation, kLink};
  return true;
}
bool phonePipeReady() { return ready; }
bool bleCentralTxAccessoryPressure() { return displayBusy; }

namespace sim {
using hw1pipe::Op;
using Bytes = std::vector<uint8_t>;
Bytes u16b(uint16_t v) { Bytes b(2); hw1pipe::put16(b.data(), v); return b; }
Bytes u32b(uint32_t v) { Bytes b(4); hw1pipe::put32(b.data(), v); return b; }
Bytes record(uint8_t type, uint8_t flags, const std::string& payload) {
  Bytes r(hw1rec::kHeader + payload.size());
  hw1rec::encodeHeader(r.data(), type, flags, uint16_t(payload.size()));
  std::copy(payload.begin(), payload.end(), r.begin() + hw1rec::kHeader);
  return r;
}
std::string otaFrame(const std::string& chunk) {
  return std::string(reinterpret_cast<const char*>(hw1rec::kOtaMagic), sizeof hw1rec::kOtaMagic) +
         std::string("\0\0\0\x10", 4) + chunk;
}

// ---- The phone -----------------------------------------------------------
struct Phone {
  uint32_t seq = 0;
  uint16_t value = 0;
  uint8_t flags = hw1pipe::kPhoneRelayOn | hw1pipe::kPhoneConsoleWanted;
  Bytes helloFlags, linkStates;
  uint16_t openError = 0;   // OPEN answer: 0 OPEN_OK, else OPEN_ERR(code)
  uint16_t console = 0;     // the open console stream
  bool credit = true;       // grants the P4 credit for what it read
  uint32_t got = 0, limit = 0, sent = 0;
  Bytes outbox, inbox;
  hw1rec::PhoneReceiver receiver;
  struct Record { uint16_t stream; uint8_t type, flags; std::string payload; };
  std::vector<Record> records;
  struct Closed { uint16_t id, reason; size_t records, linkStates; uint32_t at; };
  std::vector<Closed> closes;
  struct Opened { uint16_t id; uint32_t at; };
  std::vector<Opened> opened;
};
Phone phone;
unsigned framesPerPass = 8;
bool execPaused = false;

bool phoneSend(Op op, uint16_t id, const Bytes& body) {
  size_t offset = 0;
  do {
    size_t n = body.size() - offset;
    if (n > size_t(phone.value) - hw1pipe::kHeader) n = phone.value - hw1pipe::kHeader;
    uint8_t p[hw1pipe::kValue]{};
    p[0] = hw1pipe::kVersion; p[1] = uint8_t(op); hw1pipe::put16(p + 2, id); hw1pipe::put32(p + 4, phone.seq++);
    hw1pipe::put16(p + 8, uint16_t(n));
    p[10] = op == Op::Data ? 0
                           : uint8_t((offset == 0 ? hw1pipe::kFirst : 0) | (offset + n == body.size() ? hw1pipe::kLast : 0));
    if (n) std::memcpy(p + hw1pipe::kHeader, body.data() + offset, n);
    if (!session.receive(p, n + hw1pipe::kHeader, clockMs)) return false;
    offset += n;
  } while (offset < body.size());
  return true;
}
// The P4's console output as the app parses it: INFO first, no interleaving,
// no stray ABORT, nothing malformed.
void phoneParse() {
  while (!phone.inbox.empty()) {
    const hw1rec::Scan r = phone.receiver.next(phone.inbox.data(), phone.inbox.size());
    REQUIRE(r.verdict != hw1rec::Verdict::Malformed && r.verdict != hw1rec::Verdict::Ignore &&
            r.verdict != hw1rec::Verdict::Skip);
    if (r.verdict == hw1rec::Verdict::NeedMore) return;
    phone.records.push_back({phone.console, r.header.type, r.header.flags,
                             std::string(phone.inbox.begin() + hw1rec::kHeader, phone.inbox.begin() + long(r.size))});
    phone.inbox.erase(phone.inbox.begin(), phone.inbox.begin() + long(r.size));
  }
}
void phoneTake(const uint8_t* f, size_t size) {
  REQUIRE(size >= hw1pipe::kHeader && f[0] == hw1pipe::kVersion);
  const Op op = Op(f[1]);
  const uint16_t id = hw1pipe::get16(f + 2);
  const uint8_t* b = f + hw1pipe::kHeader;
  const size_t n = size - hw1pipe::kHeader;
  switch (op) {
    case Op::Hello: {
      REQUIRE(n == 12 && b[8] == hw1pipe::kMaxStreams);
      phone.helloFlags.push_back(b[9]);
      phone.value = hw1pipe::get16(b + 10);
      Bytes ack(20);
      std::memcpy(ack.data(), b, 8);
      ack[8] = 0x5a; ack[16] = hw1pipe::kMaxStreams; ack[17] = phone.flags;
      hw1pipe::put16(ack.data() + 18, phone.value);
      REQUIRE(phoneSend(Op::HelloAck, 0, ack));
      break;
    }
    case Op::LinkState: REQUIRE(n == 4); phone.linkStates.push_back(b[0]); break;
    case Op::Open: {
      const Bytes console{0x00, 0x10, 0x00, 0x00, 0x01, 0x00, 0x04, 0x00};  // limit 4096 | port 1 | type 4 | no host
      REQUIRE(Bytes(b, b + n) == console);
      phone.opened.push_back({id, clockMs});
      if (phone.openError) { REQUIRE(phoneSend(Op::OpenErr, id, u16b(phone.openError))); break; }
      phone.console = id; phone.got = phone.sent = 0; phone.limit = hw1pipe::get32(b);
      phone.inbox.clear(); phone.receiver.reset();
      REQUIRE(phoneSend(Op::OpenOk, id, u32b(hw1pipe::kConsoleCredit)));
      break;
    }
    case Op::Data:
      REQUIRE(id == phone.console && n);
      phone.got += uint32_t(n);
      phone.inbox.insert(phone.inbox.end(), b, b + n);
      phoneParse();
      if (phone.credit) REQUIRE(phoneSend(Op::Credit, id, u32b(phone.got + hw1pipe::kConsoleCredit)));
      break;
    case Op::Credit: if (id == phone.console) phone.limit = hw1pipe::get32(b); break;
    case Op::Close:
      phone.closes.push_back({id, hw1pipe::get16(b), phone.records.size(), phone.linkStates.size(), clockMs});
      REQUIRE(phoneSend(Op::CloseAck, id, {}));
      if (id == phone.console) phone.console = 0;
      break;
    case Op::CloseAck: if (id == phone.console) phone.console = 0; break;
    case Op::Ping: REQUIRE(phoneSend(Op::Pong, 0, Bytes(b, b + n))); break;
    default: REQUIRE(false);
  }
}
void phoneFlush() {
  while (phone.console && !phone.outbox.empty() && phone.limit > phone.sent) {
    const size_t n = std::min({phone.outbox.size(), size_t(phone.limit - phone.sent),
                               size_t(phone.value) - hw1pipe::kHeader});
    REQUIRE(phoneSend(Op::Data, phone.console, Bytes(phone.outbox.begin(), phone.outbox.begin() + long(n))));
    phone.sent += uint32_t(n);
    phone.outbox.erase(phone.outbox.begin(), phone.outbox.begin() + long(n));
  }
}
void phoneQueue(const Bytes& bytes) { phone.outbox.insert(phone.outbox.end(), bytes.begin(), bytes.end()); }
// The phone's own control frames go in through the pipe task (the owner's poll).
std::vector<std::pair<Op, Bytes>> phoneControl;
void phoneLinkState(uint8_t flags) { phone.flags = flags; phoneControl.push_back({Op::LinkState, u32b(flags)}); }
void phoneClose(uint16_t reason) { phoneControl.push_back({Op::Close, u16b(reason)}); }
void phoneReset(uint16_t code) { phoneControl.push_back({Op::Reset, u16b(code)}); }

// One poll of the owner: the session's TX and RX.
void exchange() {
  for (const auto& c : phoneControl) {
    const uint16_t id = c.first == Op::LinkState ? 0 : phone.console;
    REQUIRE(phoneSend(c.first, id, c.second));
    if (c.first == Op::Reset) phone.console = 0;
  }
  phoneControl.clear();
  phoneFlush();
  for (unsigned i = 0; i < framesPerPass; ++i) {
    uint8_t f[hw1pipe::kValue];
    const size_t n = session.peek(f);
    if (!n) break;
    REQUIRE(session.accept(clockMs));
    phoneTake(f, n);
  }
  phoneFlush();
}
// The pipe task's pass: tickLocked under the owner mutex (flags, poll, the
// console driver), then the driver's after-unlock hook.
void ownerPass() {
  HostAs as(workerTask);
  REQUIRE(take(ownerSem, 0) == pdTRUE);
  session.setLocalFlags(pipeConsoleLocalFlags());
  if (ready) {
    (void)session.tick(clockMs, false, displayBusy);
    if (session.state == hw1pipe::State::Ready) exchange();
  }
  pipeConsoleTickLocked(session, generation, ready, clockMs, displayBusy);
  if (ready && session.state != hw1pipe::State::Ready) ready = false;  // the owner's "phone link lost"
  give(ownerSem);
  pipeConsoleAfterUnlock();
}
void pass() {
  clockMs += 10;
  ownerPass();
  if (!execPaused) runExec();
}
void run(uint32_t ms) { for (uint32_t t = 0; t < ms; t += 10) pass(); }
template <class F> bool runUntil(F done, uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 10) { if (done()) return true; pass(); }
  return done();
}

char code[17];
void setGenerated(bool on) { gSettings.phonePipeSecretGenerated = on; }
bool slotZero(unsigned i) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&pool[i]);
  for (size_t j = 0; j < sizeof(PoolSlot); ++j) if (p[j]) return false;
  return true;
}
bool poolIdle() {
  for (unsigned i = 0; i < kPoolSlots; ++i) if (slotState[i] != Slot::Free || !slotZero(i)) return false;
  return true;
}
PipeConsoleStatus status() { PipeConsoleStatus s; pipeConsoleStatus(&s); return s; }

void reset() {
  REQUIRE(!chainSem.holder && !ownerSem.holder && !lifecycleSem.holder && critical == 0 && current == &mainTask);
  runExec();
  REQUIRE(poolIdle());  // every slot is wiped once it is let go
  drv = Driver{}; pending = Pending{}; mailbox = CloseRequest{}; chain.reset(); chainTarget = {};
  stalled = false; stalledEpoch = 0;
  for (auto* counter : {&opens, &cardDrops, &mirrorDrops, &unknownRecords, &malformed, &aborts, &withheldMs,
                        &withheldMaxMs, &poolFull, &executorBusy, &rowUnbound})
    counter->store(0);
  shownLastClose.store(0);
  session.down();
  row = {}; refuseBind = bindInvisible = false; binds = ends = 0; inbound.clear();
  ready = displayBusy = ownerBusy = false; busyFrom = SIZE_MAX; pushes.clear(); activity = 0;
  execQueue.items.clear(); execPaused = false; execDrops = 0;
  framesPerPass = 8; phone = Phone{}; phoneControl.clear();
  whileAsleep = [] { ownerPass(); };
  sleeps = 0;
  gSettings.bleConsoleOnPipe = true; gSettings.phonePipeSecret = String(code); setGenerated(true);
}
void connect(uint8_t flags = hw1pipe::kPhoneRelayOn | hw1pipe::kPhoneConsoleWanted) {
  ++generation;
  phone = Phone{};
  phone.flags = flags;
  HostAs as(workerTask);
  REQUIRE(take(ownerSem, 0) == pdTRUE);
  session.setLocalFlags(pipeConsoleLocalFlags());
  const uint8_t nonce[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  REQUIRE(session.start(247, nonce, clockMs));
  exchange();
  REQUIRE(session.state == hw1pipe::State::Ready);
  give(ownerSem);
  ready = true;
}
void linkLoss() {
  HostAs as(workerTask);
  REQUIRE(take(ownerSem, 0) == pdTRUE);
  session.down(hw1pipe::Error::Connect, clockMs);
  give(ownerSem);
  ready = false;
}
// Connected, the console open, bound, and its INFO delivered.
void openStream() {
  connect();
  REQUIRE(runUntil([] { return row.bound && phone.console && !phone.records.empty(); }, 1000));
  REQUIRE(status().state == PipeConsoleState::Open);
}
bool reopened(size_t before) { return phone.opened.size() > before && row.bound && phone.console; }
PipeConsoleTarget target() { return PipeConsoleTarget{row.generation, row.streamId}; }
PipeConsoleWrite write(HostTask& task, PipeConsoleKind kind, const std::string& s,
                       PipeConsoleMode mode = PipeConsoleMode::TryNow) {
  HostAs as(task);
  return pipeConsoleWrite(target(), row.epoch, kind, reinterpret_cast<const uint8_t*>(s.data()), s.size(), mode);
}
size_t txSpace() { return session.txSpace(drv.id); }
std::string recordsOf(uint8_t type) {
  std::string all;
  for (const auto& r : phone.records) if (r.type == type) all += r.payload;
  return all;
}

// ---- Scenarios ------------------------------------------------------------
void openAndInfo() {
  reset();
  REQUIRE(status().state == PipeConsoleState::Off);
  connect();
  REQUIRE(phone.helloFlags.back() == hw1pipe::kP4ConsoleOffered && status().offered);
  // The row is bound right after INFO was queued; a write that wins the race
  // to the stream still lands behind INFO.
  REQUIRE(runUntil([] { return row.bound; }, 500));
  REQUIRE(phone.records.empty() && status().state == PipeConsoleState::Opening);  // Open once the pass sees the row
  REQUIRE(write(mainTask, PipeConsoleKind::Text, "early") == PipeConsoleWrite::Sent);
  run(50);
  REQUIRE(phone.opened.size() == 1 && binds == 1 && lastLink == kLink && opens.load() == 1);
  REQUIRE(row.generation == generation && row.streamId == phone.console &&
          row.rowConnId == blePipeConsoleRowId(generation) && row.rowConnId >= 0x8000);
  char info[hw1rec::kInfoMax + 1];
  const size_t n = hw1rec::buildInfo(info, sizeof info, SelfDevice::firmwareVersion(), BOARD_NAME);
  REQUIRE(phone.records.size() == 2 && phone.records[0].type == hw1rec::kInfo && phone.records[0].flags == 0 &&
          phone.records[0].payload == std::string(info, n));
  REQUIRE(phone.records[0].payload.find("\"fw\":\"0.99.99 \\\"host\\\"\"") != std::string::npos &&
          phone.records[0].payload.find("\"model\":\"Host \\\"Board\\\"\"") != std::string::npos);
  REQUIRE(phone.records[1].type == hw1rec::kText && phone.records[1].payload == "early");
  const PipeConsoleStatus st = status();
  REQUIRE(st.state == PipeConsoleState::Open && st.offered && st.wanted && st.streamId == phone.console &&
          st.opens == 1 && st.lastClose == 0);
  REQUIRE(activity == 1);  // the TEXT; INFO is not console activity
}

void notOffered() {
  char bad[17];
  std::memcpy(bad, code, sizeof bad);
  bad[15] = bad[15] == '0' ? '1' : '0';  // a wrong check symbol
  for (int variant = 0; variant < 4; ++variant) {
    reset();
    if (variant == 0) gSettings.bleConsoleOnPipe = false;
    if (variant == 1) setGenerated(false);
    if (variant == 2) gSettings.phonePipeSecret = String(bad);
    if (variant == 3) gSettings.phonePipeSecret = String("Correct-Horse-9!");
    REQUIRE(pipeConsoleLocalFlags() == 0);
    connect();
    run(3000);
    REQUIRE(phone.helloFlags.back() == 0 && phone.opened.empty());
    REQUIRE(status().state == PipeConsoleState::NotOffered && !status().offered);
  }
  // The display form of the code is a code too.
  reset();
  char shown[20];
  hw1code::display(code, shown);
  gSettings.phonePipeSecret = String(shown);
  REQUIRE(pipeConsoleLocalFlags() == hw1pipe::kP4ConsoleOffered);
  // Offered, not wanted: it waits, then opens as soon as the phone wants it.
  reset();
  connect(hw1pipe::kPhoneRelayOn);
  run(3000);
  REQUIRE(phone.opened.empty() && status().state == PipeConsoleState::Waiting && status().offered && !status().wanted);
  phoneLinkState(hw1pipe::kPhoneRelayOn | hw1pipe::kPhoneConsoleWanted);
  run(30);
  REQUIRE(phone.opened.size() == 1);
}

void ingressRecords() {
  reset();
  openStream();
  const unsigned before = activity;
  const std::string bin = otaFrame("chunk-bytes");
  Bytes all;
  for (const Bytes& r : {record(0x90, 0, "abc"), record(hw1rec::kResume, 0, "zz"), record(hw1rec::kCommand, 0, "help"),
                         record(hw1rec::kBinary, 0, bin), record(0x44, 0, "")})
    all.insert(all.end(), r.begin(), r.end());
  phoneQueue(all);
  run(30);
  REQUIRE(inbound.size() == 2 && inbound[0].type == hw1rec::kCommand && inbound[0].bytes == "help" &&
          inbound[1].type == hw1rec::kBinary && inbound[1].bytes == bin);
  REQUIRE(inbound[0].rowConnId == blePipeConsoleRowId(generation) && inbound[0].epoch == row.epoch);
  REQUIRE(unknownRecords.load() == 3 && malformed.load() == 0 && activity == before + 2);
  REQUIRE(session.find(drv.id)->consumed == all.size() && session.rxAvailable(drv.id) == 0);
  REQUIRE(poolIdle() && execDrops == 0);
  // Display pressure defers our credit, not the records already here.
  displayBusy = true;
  phoneQueue(record(hw1rec::kCommand, 0, "status"));
  run(30);
  REQUIRE(inbound.size() == 3 && inbound[2].bytes == "status");
  displayBusy = false;
}

// Credit is withheld, and nothing is dropped or counted as unknown or
// malformed, while the row is unbound, the pool is full or cmd_exec has fewer
// than two free places (withheldCredit times and counts those waits).
void ingressGated() {
  // Pool: eight records wait in slots for cmd_exec; the ninth stays in rx.
  reset();
  openStream();
  execPaused = true;
  Bytes nine;
  for (int i = 0; i < 9; ++i) {
    const Bytes r = record(hw1rec::kCommand, 0, "cmd" + std::to_string(i) + std::string(96, 'x'));
    nine.insert(nine.end(), r.begin(), r.end());
  }
  phoneQueue(nine);
  const uint32_t consumed = session.find(drv.id)->consumed;
  run(500);
  REQUIRE(execQueue.items.size() == 8 && session.rxAvailable(drv.id) == 104 &&
          session.find(drv.id)->consumed == consumed + 8 * 104);
  for (unsigned i = 0; i < kPoolSlots; ++i) REQUIRE(slotState[i] == Slot::Queued && !slotZero(i));
  REQUIRE(execDrops == 0 && unknownRecords.load() == 0 && malformed.load() == 0 && phone.closes.empty());
  execPaused = false;
  run(30);
  REQUIRE(inbound.size() == 9 && session.rxAvailable(drv.id) == 0 && poolIdle());
  for (int i = 0; i < 9; ++i) REQUIRE(inbound[size_t(i)].bytes.compare(0, 4, "cmd" + std::to_string(i)) == 0);

  // cmd_exec: other producers left one place.
  reset();
  openStream();
  execPaused = true;
  for (int i = 0; i < 23; ++i) execQueue.items.push_back({otherProducer, nullptr});
  phoneQueue(record(hw1rec::kCommand, 0, "queued"));
  run(500);
  REQUIRE(session.rxAvailable(drv.id) == 10 && execQueue.items.size() == 23 && execDrops == 0 && poolIdle());
  execQueue.items.pop_front();
  run(10);
  REQUIRE(session.rxAvailable(drv.id) == 0 && execQueue.items.size() == 23);
  execPaused = false;
  run(10);
  REQUIRE(inbound.size() == 1 && inbound[0].bytes == "queued");

  // Row: bound late (within 3 s of INFO) the record waits, then goes.
  reset();
  bindInvisible = true;
  connect();
  REQUIRE(runUntil([] { return binds == 1 && phone.console; }, 500));
  phoneQueue(record(hw1rec::kCommand, 0, "early"));
  run(2000);
  REQUIRE(session.rxAvailable(drv.id) == 9 && inbound.empty() && phone.closes.empty() && poolIdle());
  REQUIRE(status().state == PipeConsoleState::Opening);
  row.bound = true; row.generation = generation; row.streamId = drv.id;
  row.rowConnId = blePipeConsoleRowId(generation); row.epoch = ++epochs;
  run(20);
  REQUIRE(inbound.size() == 1 && inbound[0].bytes == "early" && status().state == PipeConsoleState::Open);
  // Never bound: CLOSE 1 three seconds after INFO, then the 2 s backoff.
  reset();
  bindInvisible = true;
  connect();
  REQUIRE(runUntil([] { return binds == 1; }, 500));
  const uint32_t boundAt = clockMs;
  REQUIRE(runUntil([] { return !phone.closes.empty(); }, 4000));
  REQUIRE(phone.closes[0].reason == 1 && clockMs - boundAt >= 3000 && clockMs - boundAt <= 3050);
  REQUIRE(status().state == PipeConsoleState::Backoff);
}

// phone stats: credit withheld while a whole record waits on a gate.
bool withheld(uint32_t ms, uint32_t maxMs, uint32_t pool, uint32_t executor, uint32_t unbound) {
  const PipeConsoleStatus s = status();
  if (s.withheldMs == ms && s.withheldMaxMs == maxMs && s.poolFull == pool && s.executorBusy == executor &&
      s.rowUnbound == unbound)
    return true;
  std::fprintf(stderr, "withheld_ms=%u max_ms=%u pool_full=%u exec_busy=%u row_unbound=%u\n", unsigned(s.withheldMs),
               unsigned(s.withheldMaxMs), unsigned(s.poolFull), unsigned(s.executorBusy), unsigned(s.rowUnbound));
  return false;
}
void otherProducers(unsigned n) { for (unsigned i = 0; i < n; ++i) execQueue.items.push_back({otherProducer, nullptr}); }

// One wait per record, timed from its first refused pass to its admission, its
// stream's end or its session's end (live while it runs); each gate that held
// it counts once, not once per 10 ms pass; the longest wait is kept; the
// counters run per boot, not per session.
void withheldCredit() {
  reset();
  openStream();
  // Admitted on its first pass: nothing withheld.
  phoneQueue(record(hw1rec::kCommand, 0, "help"));
  run(30);
  REQUIRE(inbound.size() == 1 && withheld(0, 0, 0, 0, 0));
  // Pool full for 300 ms: the ninth record waits, then goes.
  execPaused = true;
  Bytes nine;
  for (int i = 0; i < 9; ++i) {
    const Bytes r = record(hw1rec::kCommand, 0, "cmd" + std::to_string(i) + std::string(96, 'x'));
    nine.insert(nine.end(), r.begin(), r.end());
  }
  phoneQueue(nine);
  pass();
  const uint32_t poolAt = clockMs;
  REQUIRE(execQueue.items.size() == 8 && session.rxAvailable(drv.id) == 104 && withheld(0, 0, 1, 0, 0));
  run(290);
  REQUIRE(session.rxAvailable(drv.id) == 104 && withheld(290, 290, 1, 0, 0));  // 29 more passes, one count
  runExec();  // cmd_exec frees the eight slots
  pass();
  REQUIRE(clockMs - poolAt == 300 && session.rxAvailable(drv.id) == 0 && withheld(300, 300, 1, 0, 0));
  execPaused = false;
  run(10);
  REQUIRE(inbound.size() == 10);
  // cmd_exec one place short for 120 ms: added to the total, the max stays 300.
  execPaused = true;
  otherProducers(23);
  phoneQueue(record(hw1rec::kCommand, 0, "queued"));
  pass();
  const uint32_t busyAt = clockMs;
  REQUIRE(session.rxAvailable(drv.id) == 10 && withheld(300, 300, 1, 1, 0));
  run(110);
  execQueue.items.pop_front();
  pass();
  REQUIRE(clockMs - busyAt == 120 && session.rxAvailable(drv.id) == 0 && withheld(420, 300, 1, 1, 0));
  execPaused = false;
  run(10);
  REQUIRE(inbound.size() == 11 && inbound.back().bytes == "queued");
  // One record held by the pool, then cmd_exec, then the pool again: one wait
  // of 610 ms (the new max), each gate counted once.
  execPaused = true;
  Bytes fill;
  for (int i = 0; i < 8; ++i) {
    const Bytes r = record(hw1rec::kCommand, 0, "fill" + std::to_string(i));
    fill.insert(fill.end(), r.begin(), r.end());
  }
  const Bytes both = record(hw1rec::kCommand, 0, "both");
  fill.insert(fill.end(), both.begin(), both.end());
  phoneQueue(fill);
  pass();
  const uint32_t bothAt = clockMs;
  REQUIRE(execQueue.items.size() == 8 && withheld(420, 300, 2, 1, 0));
  run(200);
  otherProducers(15);  // 23 queued: one place
  run(200);
  REQUIRE(withheld(820, 400, 2, 2, 0));
  execQueue.items.pop_back();  // another producer's job ran: two places, the pool refuses again
  run(200);
  REQUIRE(withheld(1020, 600, 2, 2, 0));
  runExec();
  pass();
  REQUIRE(clockMs - bothAt == 610 && session.rxAvailable(drv.id) == 0 && withheld(1030, 610, 2, 2, 0));
  execPaused = false;
  run(10);
  REQUIRE(inbound.size() == 20 && inbound.back().bytes == "both");
  // The session goes down 200 ms into a wait: it ends there.
  execPaused = true;
  otherProducers(23);
  phoneQueue(record(hw1rec::kCommand, 0, "lost"));
  pass();
  const uint32_t lostAt = clockMs;
  REQUIRE(withheld(1030, 610, 2, 3, 0));
  run(190);
  linkLoss();
  pass();
  REQUIRE(clockMs - lostAt == 200 && withheld(1230, 610, 2, 3, 0));
  run(500);
  REQUIRE(withheld(1230, 610, 2, 3, 0));
  // Per boot: the next session keeps the counts.
  execQueue.items.clear();
  execPaused = false;
  openStream();
  REQUIRE(withheld(1230, 610, 2, 3, 0));
  // The phone's stream RESET ends a wait; the record after the reopen adds nothing.
  execPaused = true;
  otherProducers(23);
  phoneQueue(record(hw1rec::kCommand, 0, "reset"));
  pass();
  const uint32_t resetAt = clockMs;
  run(90);
  const size_t before = phone.opened.size();
  phoneReset(uint16_t(hw1pipe::Error::Resources));
  REQUIRE(runUntil([] { return !drv.id; }, 100));
  const uint32_t resetWait = clockMs - resetAt;
  REQUIRE(resetWait >= 100 && resetWait <= 110 && withheld(1230 + resetWait, 610, 2, 4, 0));
  execQueue.items.clear();
  execPaused = false;
  REQUIRE(runUntil([&] { return reopened(before); }, 2500));
  phoneQueue(record(hw1rec::kCommand, 0, "after"));
  run(30);
  REQUIRE(inbound.back().bytes == "after" && withheld(1230 + resetWait, 610, 2, 4, 0));

  // Row not bound for 700 ms: the record waits, then goes once it is bound.
  reset();
  bindInvisible = true;
  connect();
  REQUIRE(runUntil([] { return binds == 1 && phone.console; }, 500));
  phoneQueue(record(hw1rec::kCommand, 0, "early"));
  pass();
  const uint32_t unboundAt = clockMs;
  REQUIRE(session.rxAvailable(drv.id) == 9 && withheld(0, 0, 0, 0, 1));
  run(690);
  row.bound = true; row.generation = generation; row.streamId = drv.id;
  row.rowConnId = blePipeConsoleRowId(generation); row.epoch = ++epochs;
  pass();
  REQUIRE(clockMs - unboundAt == 700 && inbound.size() == 1 && withheld(700, 700, 0, 0, 1));
  // Never bound: the wait ends with the stream (CLOSE 1, 3 s after INFO), not
  // when the CLOSE has drained.
  reset();
  bindInvisible = true;
  connect();
  REQUIRE(runUntil([] { return binds == 1 && phone.console; }, 500));
  const uint32_t infoAt = drv.infoAt;
  phoneQueue(record(hw1rec::kCommand, 0, "never"));
  pass();
  const uint32_t neverAt = clockMs;
  REQUIRE(runUntil([] { return drv.closing != 0; }, 4000));
  const uint32_t neverWait = infoAt + kBindMs - neverAt;
  REQUIRE(clockMs == infoAt + kBindMs && neverWait == 2990 && withheld(neverWait, neverWait, 0, 0, 1));
  run(3000);
  REQUIRE(phone.closes.size() == 1 && phone.closes[0].reason == 1 && withheld(neverWait, neverWait, 0, 0, 1));
}

void malformedRecords() {
  const std::vector<Bytes> bad = {
      {hw1rec::kCommand, 0x02, 1, 0, 'x'},             // a flag bit beyond MORE
      {hw1rec::kCommand, hw1rec::kMore, 1, 0, 'x'},    // MORE outside TEXT/BLOB
      {hw1rec::kCommand, 0, 0, 0},                     // an empty command
      {hw1rec::kCommand, 0, 0x00, 0x02},               // 512 B: longer than a line
      record(hw1rec::kText, 0, "x"),                   // a P4 type from the phone
      record(hw1rec::kBinary, 0, std::string(13, 'x')),  // BINARY without the OTA magic
      {0x90, hw1rec::kMore, 0, 0},                     // MORE on an unknown type
  };
  for (const Bytes& r : bad) {
    reset();
    openStream();
    phoneQueue(r);
    REQUIRE(runUntil([] { return !phone.closes.empty(); }, 100));
    REQUIRE(phone.closes[0].reason == 1 && malformed.load() == 1 && inbound.empty() && status().lastClose == 1);
  }
  // A record the phone could finish for 5 s and did not.
  reset();
  openStream();
  phoneQueue(Bytes{hw1rec::kCommand, 0, 100, 0});
  phoneQueue(Bytes(50, 'p'));
  run(4900);
  REQUIRE(phone.closes.empty() && malformed.load() == 0);
  REQUIRE(runUntil([] { return !phone.closes.empty(); }, 200));
  REQUIRE(phone.closes[0].reason == 1 && malformed.load() == 1);
  // A record the phone cannot finish because our credit is withheld (display
  // pressure defers console CREDIT) is never late.
  reset();
  openStream();
  displayBusy = true;
  for (int i = 0; i < 8; ++i) phoneQueue(record(hw1rec::kCommand, 0, std::string(511, char('a' + i))));
  run(20000);
  REQUIRE(inbound.size() == 7 && phone.closes.empty() && malformed.load() == 0 && session.rxAvailable(drv.id) == 491);
  displayBusy = false;
  run(50);
  REQUIRE(inbound.size() == 8 && inbound[7].bytes == std::string(511, 'h') && phone.closes.empty());
}

// CONTRACT 2.3: CLOSE 1 and stream RESET back off 2, 5, 10 s, then wait for a
// flag change; a bound stream that carried a command resets the ladder.
void reopenLadder() {
  reset();
  openStream();
  for (uint32_t gap : {2000u, 5000u, 10000u}) {
    const size_t before = phone.opened.size(), closes = phone.closes.size();
    phoneQueue(Bytes{hw1rec::kCommand, 0x02, 1, 0, 'x'});
    REQUIRE(runUntil([&] { return phone.closes.size() > closes && !drv.id; }, 100));
    REQUIRE(status().state == PipeConsoleState::Backoff);
    const uint32_t closedAt = phone.closes.back().at;
    REQUIRE(runUntil([&] { return reopened(before); }, gap + 500));
    const uint32_t took = phone.opened.back().at - closedAt;
    REQUIRE(took >= gap && took <= gap + 40);
  }
  size_t before = phone.opened.size();
  phoneQueue(Bytes{hw1rec::kCommand, 0x02, 1, 0, 'x'});
  run(30000);
  REQUIRE(phone.opened.size() == before && status().state == PipeConsoleState::Backoff);
  phoneLinkState(hw1pipe::kPhoneRelayOn);  // a flag change ends the wait; not wanted: still closed
  run(500);
  REQUIRE(phone.opened.size() == before && status().state == PipeConsoleState::Waiting);
  phoneLinkState(hw1pipe::kPhoneRelayOn | hw1pipe::kPhoneConsoleWanted);
  REQUIRE(runUntil([&] { return reopened(before); }, 100));
  phoneQueue(record(hw1rec::kCommand, 0, "ok"));
  run(30);
  REQUIRE(inbound.size() == 1);
  before = phone.opened.size();
  phoneQueue(Bytes{hw1rec::kCommand, 0x02, 1, 0, 'x'});
  REQUIRE(runUntil([] { return !drv.id; }, 100));
  const uint32_t closedAt = phone.closes.back().at;
  REQUIRE(runUntil([&] { return reopened(before); }, 3000));
  REQUIRE(phone.opened.back().at - closedAt <= 2040);

  // A phone stream RESET (any code) backs off the same way; the P4 sends no CLOSE.
  reset();
  openStream();
  before = phone.opened.size();
  phoneReset(uint16_t(hw1pipe::Error::Resources));
  run(20);
  REQUIRE(!drv.id && status().state == PipeConsoleState::Backoff);
  const uint32_t resetAt = clockMs;
  REQUIRE(runUntil([&] { return reopened(before); }, 2500));
  REQUIRE(phone.closes.empty() && clockMs - resetAt >= 2000);

  // A refused bind is CLOSE 1 on the next pass, then the backoff.
  reset();
  refuseBind = true;
  connect();
  REQUIRE(runUntil([] { return !phone.closes.empty(); }, 500));
  REQUIRE(phone.closes[0].reason == 1 && binds == 1 && !row.bound);
  refuseBind = false;
  REQUIRE(runUntil([] { return row.bound; }, 2500) && binds == 2);
}

void openErrors() {
  // 16 while CONSOLE_WANTED still reads 1 (the P4 never saw it fall): quiet
  // retries after 2, 5, 10 s, then wait for CONSOLE_WANTED to rise again.
  reset();
  connect();
  phone.openError = uint16_t(hw1pipe::Error::ServiceOff);
  run(40000);
  std::vector<uint32_t> quiet;
  for (const auto& o : phone.opened) quiet.push_back(o.at);
  REQUIRE(quiet.size() == 4 && status().state == PipeConsoleState::Waiting);
  for (size_t i = 1; i < 4; ++i) {
    const uint32_t gap = kBackoffMs[i - 1], took = quiet[i] - quiet[i - 1];
    REQUIRE(took >= gap && took <= gap + 60);
  }
  phone.openError = 0;
  phoneLinkState(hw1pipe::kPhoneRelayOn | hw1pipe::kPhoneConsoleWanted);  // no edge
  run(500);
  REQUIRE(phone.opened.size() == 4);
  phoneLinkState(hw1pipe::kPhoneRelayOn);
  run(500);
  phoneLinkState(hw1pipe::kPhoneRelayOn | hw1pipe::kPhoneConsoleWanted);
  REQUIRE(runUntil([] { return reopened(4); }, 100));
  // 17: not again this session, whatever the flags; the next session opens.
  reset();
  connect();
  phone.openError = uint16_t(hw1pipe::Error::Unsupported);
  run(1000);
  phoneLinkState(hw1pipe::kPhoneRelayOn);
  run(500);
  phoneLinkState(hw1pipe::kPhoneRelayOn | hw1pipe::kPhoneConsoleWanted);
  run(20000);
  REQUIRE(phone.opened.size() == 1 && status().state == PipeConsoleState::Stopped);
  linkLoss();
  run(20);
  REQUIRE(status().state == PipeConsoleState::Off);
  openStream();
  // 9: 2, 5, 10 s, then not again this session.
  reset();
  connect();
  phone.openError = uint16_t(hw1pipe::Error::Busy);
  run(20);
  std::vector<uint32_t> at;
  run(40000);
  for (const auto& o : phone.opened) at.push_back(o.at);
  REQUIRE(at.size() == 4);
  for (size_t i = 1; i < 4; ++i) {
    const uint32_t gap = kBackoffMs[i - 1], took = at[i] - at[i - 1];
    REQUIRE(took >= gap && took <= gap + 60);
  }
  phoneLinkState(hw1pipe::kPhoneRelayOn);
  run(500);
  phoneLinkState(hw1pipe::kPhoneRelayOn | hw1pipe::kPhoneConsoleWanted);
  run(20000);
  REQUIRE(phone.opened.size() == 4 && status().state == PipeConsoleState::Stopped);
}

// The phone's CLOSE (any reason) after its LINK_STATE(0): wait for
// CONSOLE_WANTED to rise again. With wanted still at 1 (its re-want coalesced
// into no LINK_STATE): the quiet retry after 2 s.
void phoneCloses() {
  for (uint16_t reason : {uint16_t(0), uint16_t(4), uint16_t(1)}) {
    reset();
    openStream();
    phoneLinkState(hw1pipe::kPhoneRelayOn);
    phoneClose(reason);
    run(30);
    REQUIRE(!drv.id && phone.closes.empty() && status().state == PipeConsoleState::Waiting);
    run(20000);
    REQUIRE(phone.opened.size() == 1);
    phoneLinkState(hw1pipe::kPhoneRelayOn | hw1pipe::kPhoneConsoleWanted);
    REQUIRE(runUntil([] { return reopened(1); }, 200));
  }
  for (uint16_t reason : {uint16_t(0), uint16_t(4), uint16_t(1)}) {
    reset();
    openStream();
    phoneClose(reason);
    const uint32_t sentAt = clockMs + 10;  // the next pass carries it
    run(30);
    REQUIRE(!drv.id && phone.closes.empty() && status().state == PipeConsoleState::Waiting);
    REQUIRE(runUntil([] { return reopened(1); }, 2500));
    const uint32_t took = phone.opened.back().at - sentAt;
    REQUIRE(took >= 2000 && took <= 2040 && phone.closes.empty());
  }
}

// F2: the phone sends LINK_STATE at most once a second, so a 1->0->1 dip of
// CONSOLE_WANTED inside that second reaches the P4 as nothing. An OPEN that met
// the dip is refused with 16 while wanted still reads 1 here: the quiet retry
// reopens it after 2 s instead of waiting for an edge that never comes. A 0
// seen meanwhile leaves only the rise. The quiet retries keep their own
// ladder (CLOSE 1 still backs off 2 s), and a command resets it.
void coalescedDip() {
  reset();
  connect();
  phone.openError = uint16_t(hw1pipe::Error::ServiceOff);  // the OPEN lands inside the dip
  REQUIRE(runUntil([] { return phone.opened.size() == 1; }, 100));
  phone.openError = 0;  // wanted is back at 1; the coalesced LINK_STATE never went
  const uint32_t refusedAt = phone.opened[0].at;
  run(1500);
  REQUIRE(phone.opened.size() == 1 && status().state == PipeConsoleState::Waiting);
  REQUIRE(runUntil([] { return reopened(1); }, 1000));
  REQUIRE(phone.opened[1].at - refusedAt >= 2000 && phone.opened[1].at - refusedAt <= 2040);
  // CLOSE 1 after a quiet retry: the error ladder starts at 2 s, untouched.
  phoneQueue(Bytes{hw1rec::kCommand, 0x02, 1, 0, 'x'});
  REQUIRE(runUntil([] { return !drv.id; }, 100));
  REQUIRE(status().state == PipeConsoleState::Backoff);
  const uint32_t closedAt = phone.closes.back().at;
  REQUIRE(runUntil([] { return reopened(2); }, 2500));
  REQUIRE(phone.opened.back().at - closedAt <= 2040);
  // A phone CLOSE with wanted still at 1: the next quiet retry, 5 s.
  phoneClose(0);
  uint32_t sentAt = clockMs + 10;
  REQUIRE(runUntil([] { return !drv.id; }, 100) && status().state == PipeConsoleState::Waiting);
  REQUIRE(runUntil([] { return reopened(3); }, 5500));
  uint32_t took = phone.opened.back().at - sentAt;
  REQUIRE(took >= 5000 && took <= 5040);
  // A command on the bound stream resets the quiet ladder: 2 s again.
  phoneQueue(record(hw1rec::kCommand, 0, "ok"));
  run(30);
  REQUIRE(inbound.size() == 1);
  phoneClose(0);
  sentAt = clockMs + 10;
  REQUIRE(runUntil([] { return reopened(4); }, 2500));
  took = phone.opened.back().at - sentAt;
  REQUIRE(took >= 2000 && took <= 2040);

  // The phone's LINK_STATE(0) arrives inside the quiet wait: the rise alone.
  reset();
  connect();
  phone.openError = uint16_t(hw1pipe::Error::ServiceOff);
  REQUIRE(runUntil([] { return phone.opened.size() == 1; }, 100));
  phone.openError = 0;
  run(500);
  phoneLinkState(hw1pipe::kPhoneRelayOn);
  run(5000);
  REQUIRE(phone.opened.size() == 1 && status().state == PipeConsoleState::Waiting);
  phoneLinkState(hw1pipe::kPhoneRelayOn | hw1pipe::kPhoneConsoleWanted);
  REQUIRE(runUntil([] { return reopened(1); }, 100));

  // A quiet step is spent only when its timer reopens: three phone CLOSEs
  // whose LINK_STATE(0) lands inside the quiet wait (each reopened by the
  // rise, no command between) leave the ladder at 2 s, so a later dip still
  // gets its retry instead of the edge wait alone.
  reset();
  openStream();
  for (size_t i = 1; i <= 3; ++i) {
    phoneClose(0);
    REQUIRE(runUntil([] { return !drv.id; }, 100) && status().state == PipeConsoleState::Waiting);
    run(500);
    phoneLinkState(hw1pipe::kPhoneRelayOn);
    run(500);
    phoneLinkState(hw1pipe::kPhoneRelayOn | hw1pipe::kPhoneConsoleWanted);
    REQUIRE(runUntil([&] { return reopened(i); }, 100));
  }
  phone.openError = uint16_t(hw1pipe::Error::ServiceOff);  // the renewed OPEN meets a dip
  rowEndedBy(2);
  REQUIRE(runUntil([] { return phone.opened.size() == 5; }, 200));
  phone.openError = 0;
  const uint32_t dipAt = phone.opened[4].at;
  REQUIRE(runUntil([] { return reopened(5); }, 2500));
  REQUIRE(phone.opened[5].at - dipAt >= 2000 && phone.opened[5].at - dipAt <= 2040);
}

// Mailbox 2 and 5 (the row already ended): CLOSE after the queued output, then
// a fresh stream and row at once. 3 and 4 follow the flags.
void closeReasons() {
  for (uint16_t reason : {uint16_t(2), uint16_t(5)}) {
    reset();
    openStream();
    framesPerPass = 0;
    REQUIRE(write(mainTask, PipeConsoleKind::Text, "last words") == PipeConsoleWrite::Sent);
    const BlePipeConsoleIdentity old = row;
    pipeConsoleRequestClose(old.generation, uint16_t(old.streamId + 7), 2);  // another stream's: ignored
    pipeConsoleRequestClose(old.generation, old.streamId, 6);                // not a console reason: ignored
    rowEndedBy(reason);
    pass();
    REQUIRE(drv.closing == reason && session.find(old.streamId)->draining);
    REQUIRE(status().state == PipeConsoleState::Closing);
    framesPerPass = 8;
    REQUIRE(runUntil([] { return row.bound && phone.console; }, 200));
    REQUIRE(phone.closes.size() == 1 && phone.closes[0].id == old.streamId && phone.closes[0].reason == reason);
    REQUIRE(phone.records[phone.closes[0].records - 1].payload == "last words");
    REQUIRE(phone.opened.size() == 2 && phone.opened[1].at - phone.closes[0].at <= 20);
    REQUIRE(row.epoch != old.epoch && row.streamId != old.streamId && status().lastClose == reason);
  }
  // A mailbox for an old stream or session closes nothing.
  reset();
  openStream();
  pipeConsoleRequestClose(row.generation - 1, row.streamId, 2);
  run(200);
  REQUIRE(phone.closes.empty());
  // 3: bleconsole off. LINK_STATE goes first, CLOSE 3 follows; reopen once offered.
  reset();
  openStream();
  gSettings.bleConsoleOnPipe = false;
  REQUIRE(runUntil([] { return !phone.closes.empty(); }, 100));
  REQUIRE(phone.closes[0].reason == 3 && phone.closes[0].linkStates == 1 && phone.linkStates[0] == 0);
  run(5000);
  REQUIRE(phone.opened.size() == 1 && status().state == PipeConsoleState::NotOffered);
  gSettings.bleConsoleOnPipe = true;
  REQUIRE(runUntil([] { return reopened(1); }, 1500));
  // 4: the phone no longer wants it; reopen when it does again.
  reset();
  openStream();
  phoneLinkState(hw1pipe::kPhoneRelayOn);
  REQUIRE(runUntil([] { return !phone.closes.empty(); }, 100));
  REQUIRE(phone.closes[0].reason == 4);
  run(5000);
  REQUIRE(phone.opened.size() == 1 && status().state == PipeConsoleState::Waiting);
  phoneLinkState(hw1pipe::kPhoneRelayOn | hw1pipe::kPhoneConsoleWanted);
  REQUIRE(runUntil([] { return reopened(1); }, 100));
  // Wanted again while our CLOSE 4 is still queued: that is the rise, it reopens.
  reset();
  openStream();
  framesPerPass = 0;
  phoneLinkState(hw1pipe::kPhoneRelayOn);
  run(20);
  REQUIRE(drv.closing == 4 && phone.closes.empty());
  phoneLinkState(hw1pipe::kPhoneRelayOn | hw1pipe::kPhoneConsoleWanted);
  run(20);
  framesPerPass = 8;
  REQUIRE(runUntil([] { return reopened(1); }, 100));
  REQUIRE(phone.closes.size() == 1 && phone.closes[0].reason == 4);
  // A row that vanished with no mailbox reason: CLOSE 2 after 1 s; an epoch
  // rotation (sign-in) in between closes nothing.
  reset();
  openStream();
  row.epoch = 0;
  run(300);
  signIn();
  run(2000);
  REQUIRE(phone.closes.empty());
  row = {};
  run(900);
  REQUIRE(phone.closes.empty());
  REQUIRE(runUntil([] { return !phone.closes.empty(); }, 200));
  REQUIRE(phone.closes[0].reason == 2);
  REQUIRE(runUntil([] { return reopened(1); }, 100));
}

// Link loss: the row ends, nothing is sent, queued records are wiped and
// never run, and the next READY starts with no holds and a fresh backoff.
void linkLossEnds() {
  reset();
  openStream();
  phoneQueue(Bytes{hw1rec::kCommand, 0x02, 1, 0, 'x'});
  REQUIRE(runUntil([] { return !drv.id; }, 100));
  REQUIRE(runUntil([] { return reopened(1); }, 2500));
  phoneQueue(Bytes{hw1rec::kCommand, 0x02, 1, 0, 'x'});
  REQUIRE(runUntil([] { return !drv.id; }, 100));
  REQUIRE(runUntil([] { return reopened(2); }, 5500));  // the ladder is at 10 s now
  execPaused = true;
  const std::string frame = otaFrame("pending");  // a BINARY: only a command resets the ladder
  phoneQueue(record(hw1rec::kBinary, 0, frame));
  run(20);
  unsigned queued = kPoolSlots;
  for (unsigned i = 0; i < kPoolSlots; ++i) if (slotState[i] == Slot::Queued) queued = i;
  REQUIRE(queued < kPoolSlots && pool[queued].len == frame.size() &&
          std::string(reinterpret_cast<const char*>(pool[queued].bytes), frame.size()) == frame);
  const unsigned endsBefore = ends;
  const size_t closes = phone.closes.size();
  linkLoss();
  pass();
  REQUIRE(!row.bound && ends == endsBefore + 1 && phone.closes.size() == closes);
  REQUIRE(slotState[queued] == Slot::Cancelled && slotZero(queued));
  REQUIRE(status().state == PipeConsoleState::Off && status().streamId == 0);
  execPaused = false;
  runExec();
  REQUIRE(inbound.empty() && poolIdle());
  openStream();
  phoneQueue(Bytes{hw1rec::kCommand, 0x02, 1, 0, 'x'});
  REQUIRE(runUntil([] { return !drv.id; }, 100));
  const uint32_t closedAt = phone.closes.back().at;
  const size_t before = phone.opened.size();
  REQUIRE(runUntil([&] { return reopened(before); }, 2500));
  REQUIRE(phone.opened.back().at - closedAt <= 2040);
}

// Fills tx (nothing on air) with TryNow TEXT so exactly `left` bytes (>= 4) stay free.
void fillTx(size_t left) {
  framesPerPass = 0;
  for (size_t rest; (rest = txSpace() - left) > 0;) {
    size_t n = rest - hw1rec::kHeader;  // the record that leaves exactly `left`
    if (n > hw1rec::kTextMax) n = std::min<size_t>(hw1rec::kTextMax, rest - hw1rec::kHeader - 5);
    REQUIRE(n >= 1 && write(mainTask, PipeConsoleKind::Text, std::string(n, 'f')) == PipeConsoleWrite::Sent);
  }
  REQUIRE(txSpace() == left);
}

void writersReserveAndModes() {
  // Whole records with 4 B kept for an ABORT: 1012 free takes 1004 B, not 1005.
  reset();
  openStream();
  fillTx(1012);
  REQUIRE(txSpace() == 1012);
  REQUIRE(write(mainTask, PipeConsoleKind::Text, std::string(1005, 'r')) == PipeConsoleWrite::Dropped);
  REQUIRE(write(mainTask, PipeConsoleKind::Text, std::string(1004, 'r')) == PipeConsoleWrite::Sent);
  REQUIRE(txSpace() == hw1rec::kHeader);
  REQUIRE(write(mainTask, PipeConsoleKind::Card, "{}") == PipeConsoleWrite::Dropped && cardDrops.load() == 1);
  REQUIRE(write(debugTask, PipeConsoleKind::Mirror, "m") == PipeConsoleWrite::Dropped && mirrorDrops.load() == 1);
  REQUIRE(write(mainTask, PipeConsoleKind::Text, "notice") == PipeConsoleWrite::Dropped);
  REQUIRE(cardDrops.load() == 1 && mirrorDrops.load() == 1 && sleeps == 0);
  // Reliable off cmd_exec is TryNow: no waiting, dropped.
  REQUIRE(write(mainTask, PipeConsoleKind::Text, "x", PipeConsoleMode::Reliable) == PipeConsoleWrite::Dropped);
  REQUIRE(write(execTask, PipeConsoleKind::Card, "{}", PipeConsoleMode::Reliable) == PipeConsoleWrite::Dropped);
  REQUIRE(sleeps == 0 && pushes.back().wait == 0 && cardDrops.load() == 2);
  // Reliable on cmd_exec waits (owner mutex released) until the phone drains.
  const uint32_t t0 = clockMs;
  whileAsleep = [t0] { if (clockMs - t0 >= 300) framesPerPass = 8; ownerPass(); };
  REQUIRE(write(execTask, PipeConsoleKind::Text, std::string(3000, 'w'), PipeConsoleMode::Reliable) ==
          PipeConsoleWrite::Sent);
  REQUIRE(sleeps > 0 && clockMs - t0 >= 300 && clockMs - t0 < 1000);
  whileAsleep = [] { ownerPass(); };
  for (const auto& p : pushes) {
    REQUIRE(p.headLen == hw1rec::kHeader);
    REQUIRE(p.type == hw1rec::kAbort ? p.reserve == 0 && p.n == 0 : p.reserve == hw1rec::kHeader);
  }
  REQUIRE(pushes.back().wait == kPushWaitMs);
  run(100);
  REQUIRE(recordsOf(hw1rec::kText).find(std::string(3000, 'w')) != std::string::npos);
  // The owner mutex busy: TryNow drops (counted); then it goes.
  ownerBusy = true;
  REQUIRE(write(mainTask, PipeConsoleKind::Card, "{\"k\":1}") == PipeConsoleWrite::Dropped && cardDrops.load() == 3);
  ownerBusy = false;
  REQUIRE(write(mainTask, PipeConsoleKind::Card, "{\"k\":1}") == PipeConsoleWrite::Sent);
  // A long mirror goes as 512 B records, without MORE; cards and mirrors are not activity.
  const unsigned before = activity;
  REQUIRE(write(debugTask, PipeConsoleKind::Mirror, std::string(1100, 'm')) == PipeConsoleWrite::Sent);
  run(50);
  REQUIRE(activity == before);
  std::vector<size_t> mirrors;
  for (const auto& r : phone.records) if (r.type == hw1rec::kMirror) { mirrors.push_back(r.payload.size()); REQUIRE(r.flags == 0); }
  REQUIRE((mirrors == std::vector<size_t>{512, 512, 76}));
  REQUIRE(phone.records.back().type == hw1rec::kMirror && recordsOf(hw1rec::kCard) == "{\"k\":1}");
  // TEXT and BLOB records are activity, one per record.
  REQUIRE(write(execTask, PipeConsoleKind::Blob, std::string(2500, 'b'), PipeConsoleMode::Reliable) ==
          PipeConsoleWrite::Sent);
  REQUIRE(activity == before + 3);
  run(50);
  REQUIRE(recordsOf(hw1rec::kBlob) == std::string(2500, 'b'));
  const PipeConsoleStatus st = status();
  REQUIRE(st.cardDrops == 3 && st.mirrorDrops == 1 && st.aborts == 0);
}

void refusals() {
  reset();
  openStream();
  REQUIRE(chainReady());
  const std::string s = "abc";
  const auto* p = reinterpret_cast<const uint8_t*>(s.data());
  HostAs as(execTask);
  REQUIRE(pipeConsoleWrite(target(), row.epoch, PipeConsoleKind::Text, p, 0, PipeConsoleMode::Reliable) == PipeConsoleWrite::Refused);
  REQUIRE(pipeConsoleWrite(target(), row.epoch, PipeConsoleKind::Text, nullptr, 3, PipeConsoleMode::Reliable) == PipeConsoleWrite::Refused);
  REQUIRE(pipeConsoleWrite(PipeConsoleTarget{row.generation, 0}, row.epoch, PipeConsoleKind::Text, p, 3, PipeConsoleMode::Reliable) == PipeConsoleWrite::Refused);
  REQUIRE(pipeConsoleWrite(target(), row.epoch, PipeConsoleKind(9), p, 3, PipeConsoleMode::Reliable) == PipeConsoleWrite::Refused);
  const std::string big(4609, 'x'), card(513, 'c'), chain4608(4608, 'y');
  REQUIRE(pipeConsoleWrite(target(), row.epoch, PipeConsoleKind::Text, reinterpret_cast<const uint8_t*>(big.data()), big.size(), PipeConsoleMode::Reliable) == PipeConsoleWrite::Refused);
  REQUIRE(pipeConsoleWrite(target(), row.epoch, PipeConsoleKind::Card, reinterpret_cast<const uint8_t*>(card.data()), card.size(), PipeConsoleMode::TryNow) == PipeConsoleWrite::Refused);
  // Not this row's session: gone, nothing sent.
  REQUIRE(pipeConsoleWrite(target(), row.epoch + 1, PipeConsoleKind::Text, p, 3, PipeConsoleMode::Reliable) == PipeConsoleWrite::Gone);
  REQUIRE(pipeConsoleWrite(PipeConsoleTarget{row.generation + 1, row.streamId}, row.epoch, PipeConsoleKind::Text, p, 3, PipeConsoleMode::Reliable) == PipeConsoleWrite::Gone);
  const size_t pushed = pushes.size();
  // The caller holds the owner mutex (the pipe task, an owner command) or the chain mutex.
  REQUIRE(take(ownerSem, 0) == pdTRUE);
  REQUIRE(pipeConsoleWrite(target(), row.epoch, PipeConsoleKind::Text, p, 3, PipeConsoleMode::Reliable) == PipeConsoleWrite::Refused);
  give(ownerSem);
  REQUIRE(take(chainSem, 0) == pdTRUE && pipeConsoleHeldByCurrentTask());
  REQUIRE(pipeConsoleWrite(target(), row.epoch, PipeConsoleKind::Card, p, 3, PipeConsoleMode::TryNow) == PipeConsoleWrite::Refused);
  give(chainSem);
  REQUIRE(!pipeConsoleHeldByCurrentTask() && pushes.size() == pushed);
  {
    HostAs worker(workerTask);
    REQUIRE(take(ownerSem, 0) == pdTRUE);
    REQUIRE(pipeConsoleWrite(target(), row.epoch, PipeConsoleKind::Text, p, 3, PipeConsoleMode::TryNow) == PipeConsoleWrite::Refused);
    give(ownerSem);
  }
  // The caller holds the lifecycle lock (L3: a Bluetooth.cpp path): refused
  // before L1, which the oracle forbids under L3, from cmd_exec or any task.
  const unsigned chainTaken = chainTakes;
  for (HostTask* task : {&execTask, &httpdTask}) {
    HostAs caller(*task);
    LifecycleGuard l3;
    REQUIRE(bleLifecycleHeldByCurrentTask());
    REQUIRE(pipeConsoleWrite(target(), row.epoch, PipeConsoleKind::Text, p, 3, PipeConsoleMode::Reliable) == PipeConsoleWrite::Refused);
    REQUIRE(pipeConsoleWrite(target(), row.epoch, PipeConsoleKind::Card, p, 3, PipeConsoleMode::TryNow) == PipeConsoleWrite::Refused);
  }
  REQUIRE(!bleLifecycleHeldByCurrentTask() && chainTakes == chainTaken && pushes.size() == pushed && sleeps == 0);
  // A 4608 B chain is the most one write carries.
  REQUIRE(pipeConsoleWrite(target(), row.epoch, PipeConsoleKind::Text, reinterpret_cast<const uint8_t*>(chain4608.data()), chain4608.size(), PipeConsoleMode::Reliable) == PipeConsoleWrite::Sent);
}

// F-2: a chain gives up after 10 s of eligible time (display pressure not
// counted) or 15 s of wall clock without an accepted record. ABORT only when
// a chain is open.
void stalls() {
  struct Case { uint32_t busyMs; uint32_t expect; };
  for (const Case c : {Case{0, 10000}, Case{3000, 13000}, Case{UINT32_MAX, 15000}}) {
    reset();
    openStream();
    framesPerPass = 0;
    const uint32_t t0 = clockMs;
    displayBusy = c.busyMs != 0;
    whileAsleep = [t0, c] { if (clockMs - t0 >= c.busyMs) displayBusy = false; ownerPass(); };
    REQUIRE(write(execTask, PipeConsoleKind::Text, std::string(4608, 's'), PipeConsoleMode::Reliable) ==
            PipeConsoleWrite::Aborted);
    const uint32_t took = clockMs - t0;
    REQUIRE(took >= c.expect && took <= c.expect + 100);
    REQUIRE(aborts.load() == 1 && pushes.back().type == hw1rec::kAbort && pushes.back().reserve == 0 &&
            pushes.back().result == PhonePipePush::Ok);
    whileAsleep = [] { ownerPass(); };
    displayBusy = false;
    framesPerPass = 8;
    run(100);
    std::vector<uint8_t> types;
    for (const auto& r : phone.records) types.push_back(r.type);
    REQUIRE((types == std::vector<uint8_t>{hw1rec::kInfo, hw1rec::kText, hw1rec::kText, hw1rec::kText, hw1rec::kAbort}));
    for (size_t i = 1; i < 4; ++i) REQUIRE(phone.records[i].flags == hw1rec::kMore);
    REQUIRE(status().aborts == 1);
    // The stream and the row survive: the next chain goes through.
    REQUIRE(write(execTask, PipeConsoleKind::Text, "after", PipeConsoleMode::Reliable) == PipeConsoleWrite::Sent);
    run(30);
    REQUIRE(phone.records.back().payload == "after" && phone.closes.empty());
  }
  // One record that never fits: no chain was open, so no ABORT.
  reset();
  openStream();
  fillTx(1012);
  const size_t pushed = pushes.size();
  const uint32_t t0 = clockMs;
  REQUIRE(write(execTask, PipeConsoleKind::Text, std::string(1010, 'z'), PipeConsoleMode::Reliable) ==
          PipeConsoleWrite::Dropped);
  REQUIRE(clockMs - t0 >= 10000 && aborts.load() == 0);
  for (size_t i = pushed; i < pushes.size(); ++i) REQUIRE(pushes[i].type == hw1rec::kText);
}

// CONC-1: the stall budget runs per episode, not per write. Once a Reliable
// write stalled, the next ones to that stream and epoch try once (Reliable
// lock wait, no wait for room): three back-to-back writes to a full stream
// that never drains cost one budget, not three. A record that goes ends the
// episode: the rest of that write, and the writes after it, wait again.
void stallEpisodes() {
  for (const bool busy : {false, true}) {
    reset();
    openStream();
    fillTx(4);  // full, and nothing drains (framesPerPass 0)
    displayBusy = busy;
    const uint32_t budget = busy ? kCeilingMs : kStallMs, t0 = clockMs;
    REQUIRE(write(execTask, PipeConsoleKind::Text, std::string(500, 'a'), PipeConsoleMode::Reliable) ==
            PipeConsoleWrite::Dropped);
    REQUIRE(clockMs - t0 >= budget && clockMs - t0 <= budget + 100 && stalled);
    const unsigned slept = sleeps;
    const size_t pushed = pushes.size();
    REQUIRE(write(execTask, PipeConsoleKind::Text, std::string(500, 'b'), PipeConsoleMode::Reliable) ==
            PipeConsoleWrite::Dropped);
    REQUIRE(write(execTask, PipeConsoleKind::Blob, std::string(3000, 'c'), PipeConsoleMode::Reliable) ==
            PipeConsoleWrite::Dropped);
    REQUIRE(clockMs - t0 <= budget + 100 && sleeps == slept && aborts.load() == 0 && stalled);
    REQUIRE(pushes.size() == pushed + 2);
    for (size_t i = pushed; i < pushes.size(); ++i)
      REQUIRE(pushes[i].wait == kPushWaitMs && pushes[i].result == PhonePipePush::NoSpace);
    // The phone drains. The next write's first record goes, which ends the
    // episode: its fourth record waits for room as a Reliable write does.
    displayBusy = false;
    framesPerPass = 8;
    run(100);
    REQUIRE(txSpace() == hw1pipe::kConsoleCredit && stalled);
    framesPerPass = 0;
    const uint32_t t1 = clockMs;
    whileAsleep = [t1] { if (clockMs - t1 >= 300) framesPerPass = 8; ownerPass(); };
    REQUIRE(write(execTask, PipeConsoleKind::Text, std::string(4608, 'd'), PipeConsoleMode::Reliable) ==
            PipeConsoleWrite::Sent);
    REQUIRE(!stalled && sleeps > slept && clockMs - t1 >= 300 && clockMs - t1 < 1000);
    whileAsleep = [] { ownerPass(); };
    run(100);
    REQUIRE(recordsOf(hw1rec::kText).find(std::string(4608, 'd')) != std::string::npos);
    // A new episode waits its full budget again.
    fillTx(4);
    const uint32_t t2 = clockMs;
    REQUIRE(write(execTask, PipeConsoleKind::Text, std::string(500, 'e'), PipeConsoleMode::Reliable) ==
            PipeConsoleWrite::Dropped);
    REQUIRE(clockMs - t2 >= kStallMs && clockMs - t2 <= kStallMs + 100 && stalled);
    // A TryNow record that goes ends it too.
    framesPerPass = 8;
    run(100);
    REQUIRE(stalled && write(mainTask, PipeConsoleKind::Card, "{}") == PipeConsoleWrite::Sent && !stalled);
    REQUIRE(phone.closes.empty() && status().state == PipeConsoleState::Open);
  }
}

// A chain cut mid-way: ABORT while the stream is open, none once it drains.
void chainCuts() {
  // The epoch changes (sign-in, logout): ABORT.
  reset();
  openStream();
  fillTx(2040);
  bool signedIn = false;
  uint32_t t0 = clockMs;
  whileAsleep = [&signedIn, t0] { if (!signedIn && clockMs - t0 >= 100) { signIn(); signedIn = true; } ownerPass(); };
  REQUIRE(write(execTask, PipeConsoleKind::Text, std::string(3000, 'c'), PipeConsoleMode::Reliable) ==
          PipeConsoleWrite::Aborted);
  REQUIRE(signedIn && clockMs - t0 < 300);  // every record is fenced, not just the first
  REQUIRE(aborts.load() == 1 && pushes.back().type == hw1rec::kAbort && pushes.back().result == PhonePipePush::Ok);
  whileAsleep = [] { ownerPass(); };
  framesPerPass = 8;
  run(100);
  REQUIRE(phone.records.back().type == hw1rec::kAbort);
  REQUIRE(phone.records[phone.records.size() - 2].flags == hw1rec::kMore && phone.closes.empty());
  // The stream drains to its CLOSE (mailbox 2): no ABORT.
  reset();
  openStream();
  fillTx(2040);
  bool ended = false;
  t0 = clockMs;
  whileAsleep = [&ended, t0] { if (!ended && clockMs - t0 >= 100) { rowEndedBy(2); ended = true; } ownerPass(); };
  REQUIRE(write(execTask, PipeConsoleKind::Text, std::string(3000, 'd'), PipeConsoleMode::Reliable) ==
          PipeConsoleWrite::Aborted);
  for (const auto& p : pushes) REQUIRE(p.type != hw1rec::kAbort || p.result == PhonePipePush::Gone);
  whileAsleep = [] { ownerPass(); };
  framesPerPass = 8;
  REQUIRE(runUntil([] { return !phone.closes.empty(); }, 200));
  REQUIRE(phone.closes[0].reason == 2 && phone.records[phone.closes[0].records - 1].flags == hw1rec::kMore);
  for (const auto& r : phone.records) REQUIRE(r.type != hw1rec::kAbort);
  // The session goes down mid-chain: the writer returns, nothing hangs.
  reset();
  openStream();
  fillTx(2040);
  bool lost = false;
  t0 = clockMs;
  whileAsleep = [&lost, t0] { if (!lost && clockMs - t0 >= 100) { linkLoss(); lost = true; } ownerPass(); };
  REQUIRE(write(execTask, PipeConsoleKind::Text, std::string(3000, 'e'), PipeConsoleMode::Reliable) ==
          PipeConsoleWrite::Aborted);
  REQUIRE(lost && clockMs - t0 < 1000 && !row.bound);
  whileAsleep = [] { ownerPass(); };
  // A TryNow chain cut while the owner is busy owes its ABORT; the next writer
  // on that stream sends it first.
  reset();
  openStream();
  fillTx(2040);
  busyFrom = pushes.size() + 2;  // TEXT 1 goes, TEXT 2 finds no room, the ABORT finds the owner busy
  REQUIRE(write(mainTask, PipeConsoleKind::Text, std::string(2048, 'o')) == PipeConsoleWrite::Aborted);
  REQUIRE(chain.chainOpen() && pushes.back().type == hw1rec::kAbort && pushes.back().result == PhonePipePush::Busy);
  REQUIRE(write(mainTask, PipeConsoleKind::Card, "{}") == PipeConsoleWrite::Dropped && chain.chainOpen());
  busyFrom = SIZE_MAX;
  REQUIRE(write(mainTask, PipeConsoleKind::Card, "{\"c\":1}") == PipeConsoleWrite::Sent && !chain.chainOpen());
  framesPerPass = 8;
  run(100);
  const size_t n = phone.records.size();
  REQUIRE(phone.records[n - 3].flags == hw1rec::kMore && phone.records[n - 2].type == hw1rec::kAbort &&
          phone.records[n - 1].type == hw1rec::kCard);
}

// Two writers: cards and mirrors from other tasks never land inside a chain.
void noInterleaving() {
  reset();
  openStream();
  framesPerPass = 0;
  unsigned tries = 0, sent = 0;
  const uint32_t t0 = clockMs;
  whileAsleep = [&tries, &sent, t0] {
    if (clockMs - t0 >= 1000) framesPerPass = 8;
    ++tries;
    if (write(mainTask, PipeConsoleKind::Card, "{\"k\":1}") == PipeConsoleWrite::Sent) ++sent;
    if (write(debugTask, PipeConsoleKind::Mirror, "mirror") == PipeConsoleWrite::Sent) ++sent;
    if (write(mainTask, PipeConsoleKind::Text, "notice") == PipeConsoleWrite::Sent) ++sent;
    ownerPass();
  };
  REQUIRE(write(execTask, PipeConsoleKind::Text, std::string(4608, 't'), PipeConsoleMode::Reliable) ==
          PipeConsoleWrite::Sent);
  REQUIRE(tries >= 90 && sent == 0 && cardDrops.load() == tries && mirrorDrops.load() == tries);
  whileAsleep = [] { ownerPass(); };
  run(100);
  REQUIRE(write(mainTask, PipeConsoleKind::Card, "{\"k\":2}") == PipeConsoleWrite::Sent);
  run(30);
  REQUIRE(recordsOf(hw1rec::kText) == std::string(4608, 't'));
  REQUIRE(phone.records.back().type == hw1rec::kCard && phone.records.back().payload == "{\"k\":2}");
  REQUIRE(ownerTakesUnderChain > 0);
}

void poolWiping() {
  reset();
  openStream();
  execPaused = true;
  phoneQueue(record(hw1rec::kCommand, 0, "login admin secret"));
  run(20);
  unsigned i = kPoolSlots;
  for (unsigned j = 0; j < kPoolSlots; ++j) if (slotState[j] == Slot::Queued) i = j;
  REQUIRE(i < kPoolSlots && !slotZero(i));
  execPaused = false;
  runExec();
  REQUIRE(inbound.size() == 1 && slotState[i] == Slot::Free && slotZero(i));
  // Our close cancels (and wipes) what is still queued for the stream.
  execPaused = true;
  phoneQueue(record(hw1rec::kCommand, 0, "queued then closed"));
  run(20);
  for (unsigned j = 0; j < kPoolSlots; ++j) if (slotState[j] == Slot::Queued) i = j;
  framesPerPass = 0;  // the CLOSE waits: the stream has not ended yet
  gSettings.bleConsoleOnPipe = false;
  pass();
  REQUIRE(drv.closing == 3 && drv.id && slotState[i] == Slot::Cancelled && slotZero(i));
  framesPerPass = 8;
  run(20);
  REQUIRE(!drv.id);
  execPaused = false;
  runExec();
  REQUIRE(inbound.size() == 1 && poolIdle());
}

void statusAndOracle() {
  reset();
  REQUIRE(status().state == PipeConsoleState::Off);
  connect();
  pass();
  REQUIRE(status().state == PipeConsoleState::Opening && status().streamId == drv.id);
  run(50);
  REQUIRE(status().state == PipeConsoleState::Open);
  ready = false;  // the owner stopped driving the console
  REQUIRE(status().state == PipeConsoleState::Off && !status().wanted && status().streamId == 0);
  ready = true;
  REQUIRE(chainCreates == 1 && chainTakes > 0 && lifecycleTakes > 0);
}

void runAll() {
  openAndInfo();
  notOffered();
  ingressRecords();
  ingressGated();
  withheldCredit();
  malformedRecords();
  reopenLadder();
  openErrors();
  phoneCloses();
  coalescedDip();
  closeReasons();
  linkLossEnds();
  writersReserveAndModes();
  refusals();
  stalls();
  stallEpisodes();
  chainCuts();
  noInterleaving();
  poolWiping();
  statusAndOracle();
}

// A code that `phone pair new` did not make (typed with `phone secret`): the
// console is never offered, even with bleconsole on.
void neverOffered() {
  reset();
  setGenerated(false);
  REQUIRE(pipeConsoleLocalFlags() == 0 && !status().offered);
  connect();
  run(5000);
  REQUIRE(phone.helloFlags.back() == 0 && phone.opened.empty() && status().state == PipeConsoleState::NotOffered);
}
}  // namespace sim

int main() {
  const uint8_t values[15] = {3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9};
  hw1code::fromValues(values, sim::code);
  REQUIRE(hw1code::isCode(sim::code, 16));
  sim::runAll();
  sim::neverOffered();
  std::printf("phone console: open/reopen table, quiet retries after a coalesced dip, INFO first, ingress gating, "
              "withheld credit timed and counted per gate, malformed and partial records, close reasons 1-5, link loss, reserve, Reliable/TryNow, stall and "
              "ceiling, one stall budget per episode, ABORT rules, no interleaving, pool wiping, refusal under L3 "
              "and the lock order passed (%u L1, %u L3 acquisitions); "
              "a code not made by phone pair new is never offered\n",
              chainTakes, lifecycleTakes);
  return 0;
}
