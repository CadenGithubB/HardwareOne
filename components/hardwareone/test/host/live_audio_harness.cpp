// Real HAL and live-audio implementation; mock only hardware, time and RTOS.
#include <atomic>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <sstream>
#include <string>
#include <vector>
#define ENABLE_UART_HOST_LINK 1
#define ENABLE_MICROPHONE 1
#define ENABLE_MICROPHONE_SENSOR 0
#define ENABLE_G2_GLASSES 1
#define EXT_RAM_BSS_ATTR
#define RETURN_VALID_IF_VALIDATE_CSTR() do {} while (0)
#define portMUX_INITIALIZER_UNLOCKED 0
using portMUX_TYPE = int;
void portENTER_CRITICAL(int*) {}
void portEXIT_CRITICAL(int*) {}
void logStub(const char*, ...) {}
#define WARN_SYSTEMF(...) logStub(__VA_ARGS__)
#define INFO_SYSTEMF(...) logStub(__VA_ARGS__)
#define DEBUG_SYSTEMF(...) logStub(__VA_ARGS__)
using StaticSemaphore_t = int;
using SemaphoreHandle_t = int*;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(int* p) { return p; }
void xSemaphoreTake(int* p, uint32_t) { assert(!*p); *p = 1; }
void xSemaphoreGive(int* p) { assert(*p); *p = 0; }
using TickType_t = uint32_t;
using BaseType_t = int;
using TaskHandle_t = void*;
constexpr uint32_t portMAX_DELAY = UINT32_MAX, pdTRUE = 1;
constexpr int pdPASS = 1, TASK_PRIORITY_LOW = 1, PRO_CORE = 0;
constexpr uint32_t LIVE_AUDIO_TX_STACK_WORDS = 4096;
uint32_t clockMs = 100, epoch = 1;
bool linkUp = true, blockedTx = false;
int baud = 2000000;
uint32_t millis() { return clockMs; }
uint32_t pdMS_TO_TICKS(uint32_t x) { return x; }
uint32_t xTaskGetTickCount() { return clockMs; }
void vTaskDelay(uint32_t x) { clockMs += x; }
void vTaskDelayUntil(uint32_t* wake, uint32_t x) { *wake += x; clockMs = *wake; }
struct WorkerDone {};
unsigned workerWakes = 0;
std::function<void()> waitHook;
uint32_t ulTaskNotifyTake(uint32_t, uint32_t delay) {
  if (delay == portMAX_DELAY) { if (workerWakes++) throw WorkerDone{}; }
  else { clockMs += delay; if (waitHook) waitHook(); }
  return 1;
}
void xTaskNotifyGive(void*) {}
int xTaskCreateLogged(void (*)(void*), const char*, uint32_t, void*, int,
                     void** out, const char*, int) { *out = reinterpret_cast<void*>(1); return pdPASS; }
enum class AllocPref { RequirePSRAM };
void* ps_alloc(size_t size, AllocPref, const char*) { return malloc(size); }
void heap_caps_free(void* p) { free(p); }
uint32_t esp_random() { return 0x12345678; }
uint32_t esp_crc32_le(uint32_t crc, const uint8_t* p, size_t n) {
  crc = ~crc;
  while (n--) { crc ^= *p++; for (int b=0; b<8; ++b) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0); }
  return ~crc;
}
using String = std::string;
class CommandArgs {
  std::vector<String> parts;
 public:
  explicit CommandArgs(const String& s) { std::istringstream in(s); String t; while (in >> t) parts.push_back(t); }
  size_t count() const { return parts.size(); }
  String arg(size_t n) const { return parts.at(n); }
};
struct CommandEntry { const char* name; const char* description; bool admin; const char* (*fn)(const String&); const char* help; };
enum { SOURCE_UART, SOURCE_INTERNAL, SOURCE_WEB };
struct AuthContext { int transport = SOURCE_UART; String user = "test"; String path; } auth;
const AuthContext& currentAuthContext() { return auth; }
uint32_t uartLinkSessionEpoch() { return epoch; }
bool uartLinkIsRunning() { return linkUp; }
int uartLinkEffectiveBaud() { return baud; }
constexpr size_t UARTLINK_FRAME_MAX_PAYLOAD = 1024;
constexpr uint8_t UARTLINK_FRAME_LIVE_BEGIN=0x10, UARTLINK_FRAME_LIVE_PCM=0x11,
                  UARTLINK_FRAME_LIVE_END=0x12, UARTLINK_FRAME_LIVE_ABORT=0x13;
struct Frame { uint8_t type; uint16_t seq; std::vector<uint8_t> bytes; };
std::vector<Frame> frames;
bool uartLinkTryWriteFrameForSession(uint32_t e, uint8_t type, uint16_t seq, const uint8_t* p, size_t n) {
  if (!linkUp || e != epoch || blockedTx) return false;
  assert(n <= UARTLINK_FRAME_MAX_PAYLOAD);
  frames.push_back({type,seq,{p,p+n}}); return true;
}
bool g2EvenAiExchangeBoundToUartSession(uint64_t, uint32_t) { return false; }
bool g2MicCaptureDegraded() { return false; }
bool leftUp = true, feed = false, paused = false;
uint32_t leftGeneration = 7, integrityErrors = 0;
unsigned e0Writes = 0, fastReleases = 0;
std::deque<int16_t> ring;
std::function<void()> readHook, startHook;
bool g2LeftConnected() { return leftUp; }
bool g2MicStreamEnable(bool) { ++e0Writes; return leftUp; }
bool g2MicSetAfeFeedActive(bool on, uint32_t gen = 0) {
  if (on && gen && (!leftUp || gen != leftGeneration)) return false;
  feed = on; paused = false; ring.clear(); integrityErrors = 0;
  if (on && startHook) startHook();
  return true;
}
void g2MicPauseAfeFeed(bool p) { paused = p; }
uint32_t g2MicAfeIntegrityErrors() { return integrityErrors; }
size_t g2MicAfeRingDepth() { return ring.size(); }
void g2MicLinkFastRelease() { ++fastReleases; }
size_t g2MicReadPcmSamples(int16_t* out, size_t cap, uint32_t wait) {
  if (readHook) readHook();
  if (!feed) return 0;
  size_t n = 0;
  while (n < cap && !ring.empty()) { out[n++] = ring.front(); ring.pop_front(); }
  if (!n) { clockMs += wait; if (waitHook) waitHook(); }
  return n;
}
size_t g2MicTrimAfeRingToNewest(size_t keep) {
  size_t n = 0; while (ring.size() > keep) { ring.pop_front(); ++n; } return n;
}
// INSERT_HAL_HEADER
// INSERT_HAL_SOURCE
// INSERT_LIVE_HEADER
// INSERT_LIVE_SOURCE

uint32_t le32(const uint8_t* p) { return p[0] | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24; }
void runJob() {
  workerWakes = 0;
  try { liveAudioTxTask(nullptr); } catch (WorkerDone&) {}
  assert(!sStream.active && !audioCaptureBusy());
}
void ready() {
  assert(strncmp(cmd_liveaudio("ready 1 abcdef0100000001"), "OK:", 3) == 0);
  assert(strncmp(cmd_liveaudio("conversate 1 abcdef0100000001 on"), "OK:", 3) == 0);
}
uint64_t begin() {
  uint64_t id = 0; ready();
  assert(liveAudioConversateBegin(7,&id) == LiveAudioConversateAdmission::Started);
  assert(id && audioCaptureOwnedBy("g2-conversate") && e0Writes == 0);
  return id;
}
void replenish(size_t count) { for (size_t i=0; i<count; ++i) ring.push_back(int16_t(i)); }

int main() {
  // Native HAL cannot fallback, steal an owner or send legacy mic-off.
  leftUp = false; assert(!audioCaptureStartG2Native("native",7)); leftUp = true;
  assert(!audioCaptureStartG2Native("native",8));
  assert(audioCaptureStartG2Native("native",7));
  assert(!audioCaptureStart("mic") && !audioCaptureStartG2Native("native",8));
  audioCaptureStop("other"); assert(audioCaptureBusy());
  audioCapturePauseG2Native("other",true); assert(!paused);
  audioCapturePauseG2Native("native",true); assert(paused);
  audioCaptureStop("native"); assert(!audioCaptureBusy() && !e0Writes && !fastReleases);
  assert(audioGetSource() == AUDIO_SRC_NONE); // native mode preserves selected source
  startHook = [] { audioCaptureStop(nullptr); };
  assert(!audioCaptureStartG2Native("native",7)); assert(!feed && !audioCaptureBusy()); startHook = {};
  assert(audioCaptureStart("mic")); audioCaptureStop("mic");
  assert(e0Writes == 2 && fastReleases == 1); e0Writes = fastReleases = 0;

  uint64_t id = 99;
  assert(liveAudioConversateBegin(7,&id) == LiveAudioConversateAdmission::Disabled && !id);
  auth.transport = SOURCE_WEB;
  assert(strncmp(cmd_liveaudio("conversate 1 abcdef0100000001 on"), "Error:",6) == 0);
  auth.transport = SOURCE_UART;
  assert(strncmp(cmd_liveaudio("conversate 1 abcdef0100000001 on"), "Error:",6) == 0);
  id = begin(); assert(!liveAudioTryBeginBulkTransfer());
  uint64_t ignored = 0;
  assert(liveAudioConversateBegin(7,&ignored) == LiveAudioConversateAdmission::Failed && !ignored);
  liveAudioConversatePause(id+1,true); assert(!paused);
  assert(strstr(cmd_liveaudio("status"), "paused=0"));
  liveAudioConversatePause(id,true); assert(paused);
  assert(strstr(cmd_liveaudio("status"), "paused=1"));
  liveAudioConversatePause(id,false); assert(!paused);
  assert(strstr(cmd_liveaudio("status"), "paused=0"));
  liveAudioConversateFinish(id+1,true); assert(!sStream.endRequested);
  replenish(1234); liveAudioConversateFinish(id,true); assert(paused);
  assert(liveAudioConversatePending(id) && !liveAudioConversateRunning(id));
  runJob(); assert(sLast.ended && sLast.sentSamples == 1234 && !sLast.droppedSamples);
  assert(!liveAudioConversatePending(id));
  assert(frames.front().type == 0x10 && frames.front().bytes[1] == 2 && frames.front().bytes[2] == 2);
  uint32_t offset=0, crc=0; uint16_t seq=0;
  for (const auto& f:frames) {
    assert(f.seq == seq++);
    if (f.type == 0x11) {
      assert(le32(f.bytes.data()+18) == offset);
      size_t n = f.bytes[22] | unsigned(f.bytes[23])<<8;
      assert(f.bytes.size() == 24+2*n); offset += n;
      crc = esp_crc32_le(crc,f.bytes.data()+24,2*n);
    }
  }
  assert(frames.back().type == 0x12 && le32(frames.back().bytes.data()+18) == offset);
  assert(le32(frames.back().bytes.data()+22) == crc);

  // END arriving during an empty read must cause another read of the frozen
  // final tail, not omit it. HAL input is paused before endRequested publishes.
  frames.clear(); id = begin();
  waitHook = [&] { replenish(321); liveAudioConversateFinish(id,true); waitHook = {}; };
  runJob(); assert(sLast.ended && sLast.sentSamples == 321);

  // Stop wins even if auth is lost during asynchronous native backend startup.
  ready(); startHook = [] { ++epoch; };
  assert(liveAudioConversateBegin(7,&id) == LiveAudioConversateAdmission::Failed);
  startHook = {}; runJob(); assert(!sLast.ended && sLast.reason == ABORT_AUTH_LOST);

  // Long-lived native path has no 30/60-second recorder cap; exercise actual
  // TX framing across a sequence wrap and a wrap of millis().
  frames.clear(); clockMs = UINT32_MAX-500; const uint64_t oldId = id; id = begin(); assert(id != oldId);
  uint32_t produced=0;
  readHook = [&] {
    clockMs += 40;
    sLease.deadlineMs = clockMs + kLeaseTtlMs;
    if (produced < 16000*120) { replenish(500); produced += 500; }
    else liveAudioConversateFinish(id,true);
  };
  uint16_t wrapSeq = 65530; uint32_t sent=0, runCrc=0, count=0; uint8_t reason=0;
  runConversate(sStream,wrapSeq,sent,runCrc,count,reason);
  assert(reason == 0 && sent == 16000*120 && wrapSeq == uint16_t(65530+count));
  readHook = {}; runJob(); // finish remaining lifecycle (direct run tested framing)

  // Lease expiration, replacement login, backpressure, decoder/ring failure,
  // explicit disarm, source disconnect: never advertise a successful END.
  for (int fault=0; fault<6; ++fault) {
    frames.clear(); id = begin(); replenish(800);
    if (fault == 0) clockMs += 3001;
    if (fault == 1) ++epoch;
    if (fault == 2) blockedTx = true;
    if (fault == 3) integrityErrors = 1;
    if (fault == 4) cmd_liveaudio("conversate 1 abcdef0100000001 off");
    if (fault == 5) audioCaptureStop(nullptr);
    runJob(); assert(!sLast.ended);
    if (!frames.empty()) assert(frames.back().type == 0x13);
    blockedTx = false;
  }
  // v1 offset overflow is explicit, never a wrapped "clean" recording.
  id = begin(); replenish(500); sent = UINT32_MAX-100; reason=0;
  runConversate(sStream,wrapSeq,sent,runCrc,count,reason);
  assert(reason == ABORT_SAMPLE_LIMIT);
  liveAudioConversateFinish(id,false); runJob();
  assert(!e0Writes && !fastReleases);
  if (sShadowStorage) free(sShadowStorage);
  puts("Live audio: production HAL, UART admission/framing, native tail drain, pause, long capture and failure fencing passed");
}
