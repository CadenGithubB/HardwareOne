#include "HAL_JPEG.h"
#include "jpeg_fixtures.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "soc/soc_caps.h"
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
unsigned failures = 0;
#if defined(SOC_JPEG_DECODE_SUPPORTED) && SOC_JPEG_DECODE_SUPPORTED
constexpr bool kHasHardware = true;
#else
constexpr bool kHasHardware = false;
#endif
void check(bool condition, const char* message, const char* name) {
  if (!condition) {
    ++failures;
    printf("JPEG_FAIL fixture=%s reason=%s\n", name, message);
  }
}
uint32_t hash(const uint8_t* bytes, size_t size) {
  uint32_t value = 2166136261u;
  for (size_t i = 0; i < size; ++i) value = (value ^ bytes[i]) * 16777619u;
  return value;
}
struct Difference { unsigned maximum = 0; double mean = 0; };
Difference difference(const hwjpeg::Image& a, const hwjpeg::Image& b) {
  Difference result;
  uint64_t sum = 0;
  if (a.size != b.size || !a.size) return result;
  for (size_t i = 0; i < a.size; ++i) {
    const unsigned delta = unsigned(std::abs(int(a.pixels[i]) - int(b.pixels[i])));
    result.maximum = std::max(result.maximum, delta);
    sum += delta;
  }
  result.mean = double(sum) / a.size;
  return result;
}
void checkShape(const hwjpeg::Image& image, const hwjpeg::Info& info, const char* name) {
  check(image.pixels && image.width == info.width && image.height == info.height &&
        image.stride == size_t(info.width) * 3 && image.size == info.bytes,
        "pixel dimensions or tight stride", name);
}
void colorCheck(const hwjpeg::Image& image, const char* name) {
  if (!image.pixels) return;
  if (std::strcmp(name, "red_8x8") == 0) {
    check(image.pixels[0] > 200 && image.pixels[1] < 30 && image.pixels[2] < 30,
          "red RGB ordering", name);
  }
  if (std::strcmp(name, "blue_8x8") == 0) {
    check(image.pixels[2] > 200 && image.pixels[0] < 30 && image.pixels[1] < 30,
          "blue RGB ordering", name);
  }
}
void runFixture(const JpegFixture& fixture) {
  hwjpeg::Info info;
  const char* error = nullptr;
  check(hwjpeg::inspect(fixture.bytes, fixture.size, info, {}, &error), "header inspect", fixture.name);
  hwjpeg::Image software;
  hwjpeg::DecodeOptions options;
  options.mode = hwjpeg::DecodeMode::SoftwareOnly;
  int64_t start = esp_timer_get_time();
  const bool softwareOk = hwjpeg::decode(fixture.bytes, fixture.size, software, options, &error);
  const int64_t softwareUs = esp_timer_get_time() - start;
  check(softwareOk == fixture.decodable, "software compatibility", fixture.name);
  if (softwareOk) { checkShape(software, info, fixture.name); colorCheck(software, fixture.name); }
  printf("JPEG_CASE name=%s mode=software ok=%d backend=%s us=%" PRIi64 " hash=%08" PRIx32 " error=%s\n",
         fixture.name, softwareOk, hwjpeg::backendName(software.backend), softwareUs,
         hash(software.pixels, software.size), error ? error : "none");
  for (const auto mode : {hwjpeg::DecodeMode::Auto, hwjpeg::DecodeMode::HardwareOnly}) {
    hwjpeg::Image decoded;
    options.mode = mode;
    start = esp_timer_get_time();
    const bool ok = hwjpeg::decode(fixture.bytes, fixture.size, decoded, options, &error);
    const int64_t elapsed = esp_timer_get_time() - start;
    const bool autoMode = mode == hwjpeg::DecodeMode::Auto;
    const bool hardwareExpected = kHasHardware && info.hardwareEligible &&
        info.components == 3 && info.width % 8 == 0 && info.height % 8 == 0;
    check(ok == (autoMode ? fixture.decodable : hardwareExpected), "backend outcome", fixture.name);
    if (ok) {
      checkShape(decoded, info, fixture.name);
      colorCheck(decoded, fixture.name);
      check(decoded.backend == ((autoMode && !hardwareExpected) ? hwjpeg::Backend::Software : hwjpeg::Backend::Hardware),
            "backend selection", fixture.name);
    }
    const Difference delta = difference(software, decoded);
    if (ok && decoded.backend == hwjpeg::Backend::Software) {
      check(softwareOk && software.size == decoded.size && delta.maximum == 0,
            "software fallback pixel equality", fixture.name);
    }
    // Hardware and TJpgDec use different IDCT/chroma rounding. Record their
    // differences rather than claiming bit-identical accelerated output.
    printf("JPEG_CASE name=%s mode=%s ok=%d backend=%s us=%" PRIi64
           " hash=%08" PRIx32 " max_delta=%u mean_delta=%.5f error=%s\n",
           fixture.name, autoMode ? "auto" : "hardware", ok, hwjpeg::backendName(decoded.backend),
           elapsed, hash(decoded.pixels, decoded.size), delta.maximum, delta.mean, error ? error : "none");
  }
}
void runPadding(const JpegFixture& fixture) {
  // Valid JPEG permits repeated 0xff marker fill bytes. The common layer
  // normalizes this before either the ROM/software or hardware decoder.
  uint8_t* padded = static_cast<uint8_t*>(std::malloc(fixture.size + 2));
  check(padded != nullptr, "marker-padding allocation", fixture.name);
  if (!padded) return;
  std::memcpy(padded, fixture.bytes, 2);
  padded[2] = padded[3] = 0xff;
  std::memcpy(padded + 4, fixture.bytes + 2, fixture.size - 2);
  for (const auto mode : {hwjpeg::DecodeMode::Auto, hwjpeg::DecodeMode::SoftwareOnly}) {
    hwjpeg::DecodeOptions options;
    options.mode = mode;
    hwjpeg::Image original, normalized;
    const bool a = hwjpeg::decode(fixture.bytes, fixture.size, original, options);
    const bool b = hwjpeg::decode(padded, fixture.size + 2, normalized, options);
    check(a && b && original.backend == normalized.backend && original.size == normalized.size &&
          difference(original, normalized).maximum == 0,
          "valid marker-padding compatibility", fixture.name);
    printf("JPEG_PADDING mode=%s ok=%d backend=%s hash=%08" PRIx32 "\n",
           mode == hwjpeg::DecodeMode::Auto ? "auto" : "software", b,
           hwjpeg::backendName(normalized.backend), hash(normalized.pixels, normalized.size));
  }
  std::free(padded);
}

void runRejects(const JpegFixture& fixture) {
  const uint8_t invalid[] = {0xff, 0xd8, 0xff, 0xe0, 0x00, 0x01, 0xff, 0xd9};
  const uint8_t* inputs[] = {nullptr, invalid, fixture.bytes, fixture.bytes};
  const size_t lengths[] = {0, sizeof(invalid), 10, fixture.size - 2};
  for (unsigned i = 0; i < 4; ++i) {
    hwjpeg::Image decoded;
    const char* error = nullptr;
    check(!hwjpeg::decode(inputs[i], lengths[i], decoded, {}, &error) &&
          !decoded.pixels && decoded.backend == hwjpeg::Backend::None,
          "malformed/truncated input rejection", fixture.name);
  }
  hwjpeg::Image decoded;
  hwjpeg::DecodeOptions options;
  options.maxOutputBytes = 1;
  check(!hwjpeg::decode(fixture.bytes, fixture.size, decoded, options) && !decoded.pixels,
        "output limit rejection", fixture.name);
}
struct Worker { const JpegFixture* fixture; SemaphoreHandle_t done; unsigned failed = 0; unsigned hardware = 0; unsigned software = 0; };
void stressWorker(void* argument) {
  auto& worker = *static_cast<Worker*>(argument);
  for (unsigned i = 0; i < 30; ++i) {
    hwjpeg::Image decoded;
    if (!hwjpeg::decode(worker.fixture->bytes, worker.fixture->size, decoded)) ++worker.failed;
    else if (decoded.backend == hwjpeg::Backend::Hardware) ++worker.hardware;
    else if (decoded.backend == hwjpeg::Backend::Software) ++worker.software;
  }
  xSemaphoreGive(worker.done);
  vTaskDelete(nullptr);
}
void runStress(const JpegFixture& fixture) {
  // All codec/driver lazy initialization is warmed by the corpus above.
  const size_t freeBefore = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const size_t internalBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  int64_t start = esp_timer_get_time();
  Worker workers[2] = {{&fixture, xSemaphoreCreateBinary()}, {&fixture, xSemaphoreCreateBinary()}};
  bool started[2] = {};
  for (unsigned i = 0; i < 2; ++i) {
    check(workers[i].done != nullptr, "worker semaphore", fixture.name);
    if (workers[i].done) {
      started[i] = xTaskCreatePinnedToCore(stressWorker, "jpeg-stress", 8192, &workers[i], 5,
                                         nullptr, i % portNUM_PROCESSORS) == pdPASS;
      check(started[i], "worker create", fixture.name);
    }
  }
  for (unsigned i = 0; i < 2; ++i) {
    if (started[i]) {
      // A hardware call is bounded to one second. Waiting rather than deleting
      // workers keeps their argument memory alive even if a decoder misbehaves.
      xSemaphoreTake(workers[i].done, portMAX_DELAY);
      check(workers[i].failed == 0, "concurrent decode", fixture.name);
    }
    if (workers[i].done) vSemaphoreDelete(workers[i].done);
  }
  vTaskDelay(pdMS_TO_TICKS(30));
  const int64_t elapsed = esp_timer_get_time() - start;
  const size_t freeAfter = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const size_t internalAfter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  printf("JPEG_STRESS loops=60 us=%" PRIi64 " hardware=%u software=%u heap_before=%u heap_after=%u internal_before=%u internal_after=%u\n",
         elapsed, workers[0].hardware + workers[1].hardware, workers[0].software + workers[1].software,
         unsigned(freeBefore), unsigned(freeAfter), unsigned(internalBefore), unsigned(internalAfter));
  check(freeAfter + 1024 >= freeBefore && internalAfter + 1024 >= internalBefore,
        "concurrent decode heap retention", fixture.name);
  check(heap_caps_check_integrity_all(true), "heap integrity", fixture.name);
}
} // namespace

extern "C" void app_main() {
  // Give the USB serial console time to reconnect after an application reset.
  vTaskDelay(pdMS_TO_TICKS(1500));
  printf("JPEG_START hardware=%d fixtures=%u\n", kHasHardware, unsigned(kFixtureCount));
  const JpegFixture* benchmark = nullptr;
  for (const auto& fixture : kFixtures) {
    runFixture(fixture);
    if (std::strcmp(fixture.name, "rgb420_320x240") == 0) benchmark = &fixture;
  }
  if (benchmark) {
    runPadding(*benchmark);
    runRejects(*benchmark);
    runStress(*benchmark);
  } else check(false, "benchmark fixture missing", "corpus");
  printf("JPEG_RESULT failures=%u\n", failures);
}
