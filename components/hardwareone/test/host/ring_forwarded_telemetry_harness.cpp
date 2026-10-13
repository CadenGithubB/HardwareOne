// Host harness for the production forwarded-telemetry parser
// (g2RingNoteForwardedTelemetry, G2_Ring.cpp). test_ring_forwarded_telemetry.py
// splices the production function, its local protobuf helpers, the shared
// g2Pb* readers (System_G2_Protocol.cpp), the R1TelemetryCache layout and the
// G2HealthMetric enum into the INSERT_* markers. The telemetry mutex, the
// direct-link ownership check, the Health live series and millis() are doubles.
//
// Each case runs in its own process: `harness <case>`. The frame is copied into
// an exact-size heap allocation so ASan reports any read past it, and an alarm
// turns a parser that never returns into a named failure.

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <string>
#include <vector>

#include <unistd.h>

#define DEBUG_RING_HEALTHF(...) do {} while (0)

// INSERT_TYPES

// INSERT_PROTOCOL

static R1TelemetryCache gR1Cache;

struct NotedSample {
  G2HealthMetric metric;
  int16_t value;
  uint32_t ringTs;
};
static std::vector<NotedSample> gSamples;
static int gPrepareCalls = 0;

static uint32_t millis() { return 4242u; }

static void g2HealthNoteSample(G2HealthMetric metric, int16_t value, uint32_t ringTs) {
  gSamples.push_back({metric, value, ringTs});
}

// The production guard takes the telemetry mutex; the parser only needs it to
// report success.
class RingTelemetryGuard {
 public:
  bool locked() const { return true; }
};

static bool ringPrepareForwardedTelemetryMutationLocked() {
  ++gPrepareCalls;
  return true;
}

// INSERT_PRODUCTION

// ASan calls this hook before it prints a report; symbolizing the report can
// outlast the hang alarm, so disarm it and let the report name the defect.
extern "C" void __asan_on_error() { alarm(0); }

namespace {

using Bytes = std::vector<uint8_t>;

const char* gCase = "";
int gFailures = 0;

void check(bool condition, const char* what) {
  if (condition) return;
  std::printf("FAIL %s: %s\n", gCase, what);
  ++gFailures;
}

void append(Bytes& out, std::initializer_list<uint8_t> bytes) {
  out.insert(out.end(), bytes.begin(), bytes.end());
}

void appendDelimited(Bytes& out, uint8_t tag, const Bytes& body) {
  // Bodies used here stay below 128 bytes, so the length is one varint byte.
  out.push_back(tag);
  out.push_back((uint8_t)body.size());
  out.insert(out.end(), body.begin(), body.end());
}

void onAlarm(int) {
  static const char kPrefix[] = "FAIL ";
  static const char kMessage[] =
      ": g2RingNoteForwardedTelemetry did not return within 5 s "
      "(a wrapped field length moved the parse position backwards)\n";
  (void)!write(STDOUT_FILENO, kPrefix, sizeof(kPrefix) - 1);
  (void)!write(STDOUT_FILENO, gCase, std::strlen(gCase));
  (void)!write(STDOUT_FILENO, kMessage, sizeof(kMessage) - 1);
  _exit(3);
}

void feed(const Bytes& frame) {
  // Exact-size heap copy: any read at or past frame.size() is an ASan error.
  uint8_t* heap = static_cast<uint8_t*>(std::malloc(frame.size()));
  if (!heap) std::abort();
  std::memcpy(heap, frame.data(), frame.size());
  std::signal(SIGALRM, onAlarm);
  alarm(5);
  g2RingNoteForwardedTelemetry(heap, frame.size());
  alarm(0);
  std::free(heap);
}

bool cacheUntouched() {
  return !gR1Cache.batteryValid && !gR1Cache.hrValid && !gR1Cache.spo2Valid &&
         !gR1Cache.hrvValid && !gR1Cache.tempValid && !gR1Cache.wearValid;
}

// Ten-byte varint 2^64-11: F5 then eight FF then 01.
void appendHugeLengthMinus11(Bytes& out) {
  append(out, {0xF5, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01});
}

void caseValidFull() {
  Bytes raw;
  append(raw, {0x08, 0x32});                    // 1 battery = 50
  append(raw, {0x18, 0x48});                    // 3 hr = 72
  append(raw, {0x20, 0xE8, 0x07});              // 4 hrTs = 1000
  append(raw, {0x5D, 0x01, 0x02, 0x03, 0x04});  // 11 FIXED32 (unknown wire here)
  append(raw, {0x28, 0x62});                    // 5 spo2 = 98
  append(raw, {0x30, 0xD0, 0x0F});              // 6 spo2Ts = 2000
  append(raw, {0x38, 0x2A});                    // 7 hrv = 42
  append(raw, {0x62, 0x02, 0x01, 0x02});        // 12 LEN_DELIM (skipped)
  append(raw, {0x48, 0xED, 0x02});              // 9 temp = 36.5 C
  append(raw, {0x69, 1, 2, 3, 4, 5, 6, 7, 8});  // 13 FIXED64 (skipped)
  append(raw, {0x40, 0xB8, 0x17});              // 8 hrvTs = 3000
  append(raw, {0x50, 0xA0, 0x1F});              // 10 tempTs = 4000

  Bytes frame;
  append(frame, {0x08, 0x02});                          // 1 commandId = RAW_DATA
  append(frame, {0x10, 0x05});                          // 2 VARINT (skipped)
  append(frame, {0x19, 1, 2, 3, 4, 5, 6, 7, 8});        // 3 FIXED64 (skipped)
  append(frame, {0x2D, 1, 2, 3, 4});                    // 5 FIXED32 (skipped)
  append(frame, {0x1A, 0x02, 0xAA, 0xBB});              // 3 event LEN_DELIM (skipped)
  appendDelimited(frame, 0x22, raw);                    // 4 rawData
  feed(frame);

  check(gPrepareCalls == 1, "a well-formed frame reaches the cache mutation once");
  check(gR1Cache.batteryValid && gR1Cache.battery == 50, "battery cached");
  check(gR1Cache.hrValid && gR1Cache.hr == 72 && gR1Cache.hrTs == 1000, "hr and hrTs cached");
  check(gR1Cache.spo2Valid && gR1Cache.spo2 == 98 && gR1Cache.spo2Ts == 2000, "spo2 and spo2Ts cached");
  check(gR1Cache.hrvValid && gR1Cache.hrv == 42 && gR1Cache.hrvTs == 3000, "hrv and hrvTs cached");
  check(gR1Cache.tempValid && gR1Cache.tempTenths == 365 && gR1Cache.tempTs == 4000,
        "temperature and tempTs cached");
  check(gR1Cache.batteryRxMs == 4242u && gR1Cache.hrRxMs == 4242u, "rx stamps come from millis()");
  check(gSamples.size() == 5, "five live Health samples (battery, hr, spo2, hrv, temp)");
  if (gSamples.size() == 5) {
    check(gSamples[0].metric == HEALTH_METRIC_BATTERY && gSamples[0].value == 50 &&
              gSamples[0].ringTs == 0,
          "battery sample first, without a ring stamp");
    check(gSamples[1].metric == HEALTH_METRIC_HR && gSamples[1].value == 72, "hr sample");
    check(gSamples[2].metric == HEALTH_METRIC_SPO2 && gSamples[2].value == 98, "spo2 sample");
    check(gSamples[3].metric == HEALTH_METRIC_HRV && gSamples[3].value == 42, "hrv sample");
    check(gSamples[4].metric == HEALTH_METRIC_TEMP && gSamples[4].value == 365, "temp sample");
  }
}

void caseExactBoundary() {
  // Field 4 ends exactly at the end of the frame.
  Bytes frame;
  append(frame, {0x22, 0x02, 0x08, 0x32});
  feed(frame);
  check(gPrepareCalls == 1, "a field ending at the frame end is accepted");
  check(gR1Cache.batteryValid && gR1Cache.battery == 50, "battery cached");
}

void caseTruncatedField4() {
  Bytes frame;
  append(frame, {0x22, 0x03, 0x08, 0x32});  // declares 3 bytes, 2 remain
  feed(frame);
  check(gPrepareCalls == 0, "a field 4 longer than the frame is rejected before the lock");
  check(cacheUntouched() && gSamples.empty(), "nothing cached from a truncated field 4");
}

void caseTruncatedFixed64() {
  Bytes frame;
  append(frame, {0x22, 0x02, 0x08, 0x32});
  append(frame, {0x19, 1, 2, 3, 4, 5, 6, 7});  // FIXED64 with 7 bytes left
  feed(frame);
  check(gPrepareCalls == 0, "a truncated outer FIXED64 rejects the frame");
  check(cacheUntouched() && gSamples.empty(), "nothing cached from a truncated outer field");
}

void caseTruncatedNested() {
  Bytes raw;
  append(raw, {0x08, 0x32});        // battery = 50 (applied before the bad field)
  append(raw, {0x62, 0x05, 0x01});  // LEN_DELIM declaring 5 bytes, 3 remain after it
  append(raw, {0x18, 0x48});        // hr = 72 (never reached)
  Bytes frame;
  appendDelimited(frame, 0x22, raw);
  feed(frame);
  check(gPrepareCalls == 1, "the outer walk accepts a well-bounded field 4");
  check(gR1Cache.batteryValid && gR1Cache.battery == 50, "fields before the bad nested field apply");
  check(!gR1Cache.hrValid, "the walk stops at a nested field longer than RingRawData");
}

void caseHangUnknownLen() {
  // Unknown field 2, LEN_DELIM, length 2^64-11: `pos + len` wraps to 0.
  Bytes frame;
  frame.push_back(0x12);
  appendHugeLengthMinus11(frame);
  feed(frame);
  check(gPrepareCalls == 0, "an unknown field longer than the frame rejects it");
  check(cacheUntouched() && gSamples.empty(), "nothing cached from the 11-byte hang frame");
}

void caseHangField4Len() {
  // Field 4 with length 2^64-11: the same wrap through the rawData branch.
  Bytes frame;
  frame.push_back(0x22);
  appendHugeLengthMinus11(frame);
  feed(frame);
  check(gPrepareCalls == 0, "a field 4 longer than the frame rejects it");
  check(cacheUntouched() && gSamples.empty(), "nothing cached from the field-4 hang frame");
}

void caseOobField4() {
  // 22, nine FF, 01 (length 2^64-1), then eight bytes that read as RingRawData.
  Bytes frame;
  append(frame, {0x22, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01});
  append(frame, {0x08, 0x32, 0x18, 0x48, 0x28, 0x62, 0x08, 0x32});
  feed(frame);
  check(gPrepareCalls == 0, "a field 4 of length 2^64-1 rejects the frame before the lock");
  check(cacheUntouched() && gSamples.empty(),
        "no value from past the declared span reaches the cache or the Health series");
}

void caseNestedHang() {
  // A well-bounded field 4 whose RingRawData is the 11-byte hang frame.
  Bytes raw;
  raw.push_back(0x12);
  appendHugeLengthMinus11(raw);
  Bytes frame;
  appendDelimited(frame, 0x22, raw);
  feed(frame);
  check(gPrepareCalls == 1, "the outer walk accepts the 11-byte field 4");
  check(cacheUntouched() && gSamples.empty(), "the nested walk rejects the oversized field");
}

struct Case {
  const char* name;
  void (*run)();
};

const Case kCases[] = {
    {"valid_full", caseValidFull},
    {"exact_boundary", caseExactBoundary},
    {"truncated_field4", caseTruncatedField4},
    {"truncated_fixed64", caseTruncatedFixed64},
    {"truncated_nested", caseTruncatedNested},
    {"hang_unknown_len", caseHangUnknownLen},
    {"hang_field4_len", caseHangField4Len},
    {"oob_field4", caseOobField4},
    {"nested_hang", caseNestedHang},
};

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
    for (const Case& c : kCases) std::printf("%s\n", c.name);
    return 0;
  }
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <case>|--list\n", argv[0]);
    return 2;
  }
  for (const Case& c : kCases) {
    if (std::strcmp(argv[1], c.name) != 0) continue;
    gCase = c.name;
    c.run();
    if (gFailures == 0) std::printf("PASS %s\n", c.name);
    return gFailures == 0 ? 0 : 1;
  }
  std::fprintf(stderr, "unknown case %s\n", argv[1]);
  return 2;
}
