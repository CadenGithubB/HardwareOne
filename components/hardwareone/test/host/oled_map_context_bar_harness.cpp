// OLED Map context bar. test_oled_map_context_bar.py inserts System_Maps.h's
// LocationContext, OLED_Mode_Map.cpp's context-bar append helper (when the
// tree has one) and the displayGPSMap block that builds the bar text, from
// "char contextBuf[128];" up to the scroll update, unchanged.
// The libc double: inside the inserted code snprintf/vsnprintf follow the C
// contract exactly (return the full length, store min(n, size - 1) bytes plus
// the NUL) and store each byte from instrumented code, so ASan reports any
// byte written outside contextBuf. (The ESP32 newlib refuses sizes above
// INT_MAX with EOVERFLOW instead of writing; the contract allows the write.)
// Each case runs in its own process: `harness <case>`; `harness --list`.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// INSERT_TYPES

static int contractVsnprintf(char* dst, size_t size, const char* fmt, va_list args) {
  char staging[512];
  const int n = std::vsnprintf(staging, sizeof(staging), fmt, args);
  if (n < 0 || n >= static_cast<int>(sizeof(staging))) {
    std::fprintf(stderr, "harness: staging buffer too small for \"%s\"\n", fmt);
    std::abort();
  }
  if (size > 0) {
    const size_t copy = static_cast<size_t>(n) < size - 1 ? static_cast<size_t>(n) : size - 1;
    for (size_t i = 0; i < copy; ++i) dst[i] = staging[i];
    dst[copy] = '\0';
  }
  return n;
}

// Unused when the block appends only through a vsnprintf helper.
__attribute__((format(printf, 3, 4), unused))
static int contractSnprintf(char* dst, size_t size, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const int n = contractVsnprintf(dst, size, fmt, args);
  va_end(args);
  return n;
}

struct BarResult {
  int pos = 0;
  bool terminated = true;
  std::string text;
};

#define snprintf contractSnprintf
#define vsnprintf contractVsnprintf

// INSERT_HELPER

static BarResult buildContextBar(const LocationContext& ctx) {
  BarResult result;
  // INSERT_CONTEXT_BLOCK
  result.pos = pos;
  if (pos > 0) {
    const size_t len = strnlen(contextBuf, sizeof(contextBuf));
    result.terminated = len < sizeof(contextBuf);
    result.text.assign(contextBuf, len);
  }
  return result;
}

#undef snprintf
#undef vsnprintf

static const size_t kContextBufBytes = 128;  // OLED_Mode_Map.cpp: char contextBuf[128]

struct CaseSpec {
  const char* name;
  std::string road;
  std::string area;
  float areaDistanceM;
};

static std::string nameOf(char initial, size_t length) {
  std::string name(length, static_cast<char>(initial + ('a' - 'A')));
  if (length > 0) name[0] = initial;
  return name;
}

static std::vector<CaseSpec> cases() {
  return {
      {"short_names_near", "Main Street", "Central Park", 123.0f},
      {"area_only_far", "", "Lake Fen", 750.0f},
      {"road_only", "Elm Road", "", 42.0f},
      {"no_names", "", "", 10.0f},
      // 3 + 54 + 10 + 53 + 7 == 127: the longest text that fits whole.
      {"exact_fit_near", nameOf('R', 54), nameOf('A', 53), 123.0f},
      // 3 + 58 + 10 + 57 == 128: the area piece fills the buffer exactly.
      {"boundary_115_near", nameOf('R', 58), nameOf('A', 57), 42.0f},
      {"long_names_far", nameOf('R', 63), nameOf('A', 63), 900.0f},
      {"two_58_near", nameOf('R', 58), nameOf('A', 58), 123.0f},
      {"two_63_near", nameOf('R', 63), nameOf('A', 63), 499.4f},
  };
}

// The text the bar should show: the whole context, cut at the buffer's last
// usable byte.
static std::string expectedText(const CaseSpec& spec) {
  std::string full;
  if (!spec.road.empty()) full = "On " + spec.road;
  if (!spec.area.empty()) {
    full += (full.empty() ? "Near " : " \xE2\x80\xA2 Near ") + spec.area;
    if (spec.areaDistanceM < 500.0f) {
      char distance[32];
      std::snprintf(distance, sizeof(distance), " (%.0fm)", spec.areaDistanceM);
      full += distance;
    }
  }
  return full.substr(0, kContextBufBytes - 1);
}

static int failures = 0;

static void check(bool ok, const char* name, const char* what, const std::string& detail) {
  if (ok) return;
  ++failures;
  std::printf("FAIL %s: %s (%s)\n", name, what, detail.c_str());
}

static void runCase(const CaseSpec& spec) {
  LocationContext ctx;
  std::memset(&ctx, 0, sizeof(ctx));
  std::snprintf(ctx.nearestRoad, sizeof(ctx.nearestRoad), "%s", spec.road.c_str());
  std::snprintf(ctx.nearestArea, sizeof(ctx.nearestArea), "%s", spec.area.c_str());
  ctx.areaDistanceM = spec.areaDistanceM;
  ctx.valid = true;

  const BarResult bar = buildContextBar(ctx);
  const std::string expected = expectedText(spec);
  check(bar.pos >= 0 && static_cast<size_t>(bar.pos) < kContextBufBytes, spec.name,
        "pos must stay below sizeof(contextBuf)",
        "pos " + std::to_string(bar.pos) + ", buffer " + std::to_string(kContextBufBytes));
  check(bar.terminated, spec.name, "contextBuf must be NUL-terminated inside the array", "no NUL in 128 bytes");
  check(bar.pos <= 0 || static_cast<size_t>(bar.pos) == bar.text.size(), spec.name,
        "pos must equal the drawn text length",
        "pos " + std::to_string(bar.pos) + ", strlen " + std::to_string(bar.text.size()));
  check(bar.text == expected, spec.name, "drawn text must be the context cut at 127 bytes",
        "got \"" + bar.text + "\", want \"" + expected + "\"");
  if (expected.empty()) check(bar.pos == 0, spec.name, "an empty context draws nothing", "pos " + std::to_string(bar.pos));
}

int main(int argc, char** argv) {
  const std::vector<CaseSpec> all = cases();
  if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
    for (const CaseSpec& spec : all) std::printf("%s\n", spec.name);
    return 0;
  }
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <case>|--list\n", argv[0]);
    return 2;
  }
  for (const CaseSpec& spec : all) {
    if (std::strcmp(spec.name, argv[1]) != 0) continue;
    runCase(spec);
    if (failures == 0) std::printf("ok %s\n", spec.name);
    return failures == 0 ? 0 : 1;
  }
  std::fprintf(stderr, "unknown case %s\n", argv[1]);
  return 2;
}
