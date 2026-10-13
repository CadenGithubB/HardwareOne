// Host harness for the ESP-NOW V4 fragment reassembler (System_ESPNow.cpp).
// test_espnow_reassembly.py fills the INSERT_* markers with the production
// header struct, constants, reassembly entry, helpers and the fragment-accept
// path, then runs `test` (cases 1-7, 9), `test late-stride` (case 8) and
// `test null-table` (case 10), so on a pre-fix tree each over-read is its own run.
// Fragments are fed as already-unwrapped plaintext: no session, ACK or radio.
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static constexpr size_t CMD_RESULT_MAX = 4096;  // production value; V4_FRAG_MAX evaluates to 21
// INSERT_OPCODES
// INSERT_WIRE_HEADER
struct esp_now_recv_info { uint8_t* src_addr; uint8_t* des_addr; void* rx_ctrl; };

// Log doubles: format every message (so -Wformat checks the production format
// strings and the sanitizers see the arguments) and count warnings.
__attribute__((format(printf, 1, 2))) static void hostLog(const char* fmt, ...) {
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
}
static unsigned warnings = 0;
#define DEBUGF(flag, fmt, ...) hostLog(fmt, ##__VA_ARGS__)
#define WARN_ESPNOWF(fmt, ...) (++warnings, hostLog(fmt, ##__VA_ARGS__))

static uint32_t clockMs = 1000;
static uint32_t millis() { return clockMs; }
void formatMacAddressBuf(const uint8_t* mac, char* buf, size_t n) {
  std::snprintf(buf, n, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
struct RouterMetrics { uint32_t v4FragRx = 0, v4FragRxCompleted = 0, v4FragRxGc = 0; };
struct EspNowState { RouterMetrics routerMetrics; };
static EspNowState gState;
static EspNowState* gEspNow = &gState;

// INSERT_CONSTANTS
// INSERT_ENTRY
static V4ReasmEntry* gV4Reasm = nullptr;
// INSERT_OUTCOME
// INSERT_HELPERS
// INSERT_ACCEPT

static unsigned failures = 0, checks = 0;
static void check(bool ok, const char* what) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", what); }
}
struct Fed { bool complete; const uint8_t* payload; uint16_t len; };
static Fed feed(const uint8_t* src, uint8_t type, uint32_t msgId, uint8_t idx, uint8_t cnt, uint16_t len, uint8_t fill) {
  std::vector<uint8_t> data(len, fill);
  EspNowV4Header h{};
  h.type = type; h.msgId = msgId; h.fragIndex = idx; h.fragCount = cnt;
  esp_now_recv_info ri{const_cast<uint8_t*>(src), nullptr, nullptr};
  const uint8_t* p = data.data();
  uint16_t n = len;
  // INSERT_FEED
}
// A handler reads every byte it was given; ASan owns the table's block boundary.
static uint32_t consume(const Fed& f) {
  uint32_t sum = 0;
  for (uint16_t i = 0; i < f.len; i++) sum += f.payload[i];
  return sum;
}
static void release(const uint8_t* src, uint32_t msgId, uint8_t cnt) {
  EspNowV4Header h{};
  h.msgId = msgId; h.fragCount = cnt;
  esp_now_recv_info ri{const_cast<uint8_t*>(src), nullptr, nullptr};
  v4_dispatch_post_cleanup(&ri, &h);
}
// Allocate the table exactly as init does (one block of V4_REASM_MAX entries, zeroed).
static void table(bool present) {
  std::free(gV4Reasm);
  gV4Reasm = nullptr;
  if (present) {
    const size_t n = sizeof(V4ReasmEntry) * V4_REASM_MAX;
    gV4Reasm = (V4ReasmEntry*)std::malloc(n);
    std::memset(gV4Reasm, 0, n);
  }
  warnings = 0;
  clockMs = 1000;
  gState = EspNowState{};
}

static const uint8_t A[6] = {2, 0, 0, 0, 0, 1}, B[6] = {2, 0, 0, 0, 0, 2}, C[6] = {2, 0, 0, 0, 0, 3};

static void reassemblyCases() {
  // 1 in-order direct transfer: 200/200/50 -> 450 bytes, released by the dispatch cleanup
  table(true);
  check(!feed(A, ESPNOW_V4_TYPE_CMD_RESP, 10, 0, 3, 200, 1).complete, "1: frag 1/3 pending");
  check(!feed(A, ESPNOW_V4_TYPE_CMD_RESP, 10, 1, 3, 200, 2).complete, "1: frag 2/3 pending");
  Fed f = feed(A, ESPNOW_V4_TYPE_CMD_RESP, 10, 2, 3, 50, 3);
  check(f.complete && f.len == 450 && f.payload == gV4Reasm[0].buffer, "1: in-order transfer completes at 450 B");
  check(f.complete && consume(f) == 200 * 1 + 200 * 2 + 50 * 3, "1: in-order bytes land in fragment order");
  release(A, 10, 3);
  check(!gV4Reasm[0].active, "1: dispatch cleanup releases the entry");

  // 2 routed stride learned out of order: 130/130/40 arriving 2,0,1
  table(true);
  check(!feed(A, ESPNOW_V4_TYPE_CMD_RESP, 11, 2, 3, 40, 3).complete, "2: routed last-first pending");
  check(!feed(A, ESPNOW_V4_TYPE_CMD_RESP, 11, 0, 3, 130, 1).complete, "2: routed 1/3 pending");
  f = feed(A, ESPNOW_V4_TYPE_CMD_RESP, 11, 1, 3, 130, 2);
  check(f.complete && f.len == 300, "2: routed transfer reports 2*130+40");
  release(A, 11, 3);

  // 3 duplicate fragment ignored
  table(true);
  feed(A, ESPNOW_V4_TYPE_CMD_RESP, 12, 0, 3, 200, 1);
  check(!feed(A, ESPNOW_V4_TYPE_CMD_RESP, 12, 0, 3, 200, 9).complete && gV4Reasm[0].received == 1,
        "3: duplicate fragment ignored");

  // 4 a fragment whose fragCount disagrees with its entry is dropped, the entry kept
  table(true);
  feed(A, ESPNOW_V4_TYPE_CMD_RESP, 13, 0, 3, 200, 1);
  check(!feed(A, ESPNOW_V4_TYPE_CMD_RESP, 13, 1, 5, 200, 2).complete, "4: count-mismatch fragment not complete");
  check(gV4Reasm[0].received == 1 && !gV4Reasm[0].have[1] && gV4Reasm[0].active,
        "4: count-mismatch fragment (frame 2/5 into a 3-fragment entry) dropped, entry kept");
  table(true);
  feed(A, ESPNOW_V4_TYPE_CMD_RESP, 13, 0, 3, 200, 1);
  check(!feed(A, ESPNOW_V4_TYPE_CMD_RESP, 13, 5, 3, 200, 3).complete && gV4Reasm[0].received == 1,
        "4: index past the entry's fragCount dropped");

  // 5 a fragment whose type disagrees with its entry is dropped
  table(true);
  feed(A, ESPNOW_V4_TYPE_CMD_RESP, 14, 0, 3, 200, 1);
  feed(A, ESPNOW_V4_TYPE_FS_LIST_REPLY, 14, 1, 3, 200, 2);
  check(gV4Reasm[0].received == 1 && !gV4Reasm[0].have[1],
        "5: type-mismatch fragment (FS_LIST_REPLY into a CMD_RESP entry) dropped");

  // 6 a fragCount above V4_FRAG_MAX gets no entry
  table(true);
  check(!feed(A, ESPNOW_V4_TYPE_CMD_RESP, 15, 0, 30, 200, 1).complete, "6: 30-fragment message not complete");
  check(!gV4Reasm[0].active && !gV4Reasm[1].active, "6: 30-fragment message gets no reassembly entry");

  // 7 the finding: indices 0-18 at 202 B, index 20 at 160 B, then a frame claiming
  //   to be last (index 19 of 20) would complete at 20*202+202 = 4242 B > 4200 B.
  //   Slot 0 is held by another sender so the victim is slot 1 (end of the block).
  table(true);
  feed(C, ESPNOW_V4_TYPE_CMD_RESP, 99, 0, 3, 200, 7);
  for (uint8_t i = 0; i < 19; i++) feed(B, ESPNOW_V4_TYPE_CMD_RESP, 16, i, 21, 202, 1);
  feed(B, ESPNOW_V4_TYPE_CMD_RESP, 16, 20, 21, 160, 1);
  f = feed(B, ESPNOW_V4_TYPE_CMD_RESP, 16, 19, 20, 202, 1);
  check(!f.complete, "7: index-19-of-20 frame must not complete a 21-fragment entry");
  if (f.complete) {
    check(f.len <= gV4Reasm[1].bufferSize, "7: completed size within bufferSize");
    consume(f);
  }
  release(B, 16, 21);
  table(false);
}

static void lateStrideCase() {
  // 8 last fragment first (placed at the fallback stride 200), then 0-19 at 202 B:
  //   20*202+200 = 4240 B > 4200 B is refused and the entry reset.
  table(true);
  feed(C, ESPNOW_V4_TYPE_CMD_RESP, 99, 0, 3, 200, 7);
  feed(B, ESPNOW_V4_TYPE_CMD_RESP, 17, 20, 21, 200, 1);
  for (uint8_t i = 0; i < 19; i++) feed(B, ESPNOW_V4_TYPE_CMD_RESP, 17, i, 21, 202, 1);
  const Fed f = feed(B, ESPNOW_V4_TYPE_CMD_RESP, 17, 19, 21, 202, 1);
  check(!f.complete, "8: completion larger than the buffer (20*202+200 = 4240 > 4200) is refused");
  if (f.complete) {
    check(f.len <= gV4Reasm[1].bufferSize, "8: completed size within bufferSize");
    consume(f);
  }
  check(!gV4Reasm[1].active, "8: refused oversize completion releases its entry");
  table(false);
}

static void gcCase() {
  // 9 GC evicts stale entries and the slot is reused
  table(true);
  feed(A, ESPNOW_V4_TYPE_CMD_RESP, 18, 0, 3, 200, 1);
  feed(B, ESPNOW_V4_TYPE_CMD_RESP, 19, 0, 3, 200, 1);
  clockMs += V4_REASM_TIMEOUT_MS + 1;
  check(!feed(C, ESPNOW_V4_TYPE_CMD_RESP, 20, 0, 2, 200, 1).complete, "9: third sender pending after GC");
  check(gV4Reasm[0].active && gV4Reasm[0].msgId == 20 && gEspNow->routerMetrics.v4FragRxGc == 2,
        "9: GC evicted both stale entries");
  table(false);
}

// 10 init continued without the table (allocation failed): fragments are dropped
//    and the dispatch cleanup after a 2-fragment TEXT (which skips reassembly) returns.
static void nullTableCase() {
  table(false);
  check(!feed(A, ESPNOW_V4_TYPE_CMD_RESP, 21, 0, 2, 200, 1).complete, "10: no table: fragment dropped");
  EspNowV4Header t{};
  t.type = ESPNOW_V4_TYPE_TEXT; t.msgId = 21; t.fragCount = 2;
  esp_now_recv_info ri{const_cast<uint8_t*>(A), nullptr, nullptr};
  v4_dispatch_post_cleanup(&ri, &t);
  check(true, "10: no table: dispatch cleanup after a 2-fragment TEXT returns");
}

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "";
  if (!std::strcmp(mode, "null-table")) nullTableCase();
  else if (!std::strcmp(mode, "late-stride")) lateStrideCase();
  else { reassemblyCases(); gcCase(); }
  std::printf("ESP-NOW reassembly%s%s: %u checks, %u failures\n", *mode ? " " : "", mode, checks, failures);
  return failures ? 1 : 0;
}
