#include "HAL_JPEG_Backend.h"
#include "esp_heap_caps.h"
#include "jpeg_decoder.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
using namespace hwjpeg;
namespace {
bool noMemory = false;
size_t maxAllocation = 0;
unsigned allocations = 0;
std::vector<uint8_t> read(const std::string& path) {
  std::ifstream f(path, std::ios::binary); assert(f.good());
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
size_t marker(const std::vector<uint8_t>& data, uint8_t marker) {
  for (size_t i = 0; i + 1 < data.size(); ++i) if (data[i] == 0xff && data[i + 1] == marker) return i;
  assert(false); return 0;
}
void clean(const std::vector<uint8_t>& jpeg) {
  const auto original = jpeg; Info info; const char* error = "stale";
  assert(validateSoftware(jpeg.data(), jpeg.size(), info, {}, &error));
  assert(!error && info.width && info.height && jpeg == original);
}
}
extern "C" void* heap_caps_malloc(size_t bytes, uint32_t caps) {
  assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  assert(bytes >= 3100 && bytes <= 65536); // scratch only, never frame-sized RGB
  ++allocations; if (bytes > maxAllocation) maxAllocation = bytes;
  return noMemory ? nullptr : malloc(bytes);
}
extern "C" esp_err_t esp_jpeg_decode(esp_jpeg_image_cfg_t*, esp_jpeg_image_output_t*) {
  assert(false); return ESP_FAIL; // validator uses callbacks, never full RGB decoding
}
namespace hwjpeg { namespace detail {
bool decodeHardware(const uint8_t*, size_t, const Info&, Image&, const char**) { assert(false); return false; }
}}
int main(int argc, char** argv) {
  assert(argc == 2 || argc == 3); const std::string directory = argv[1];
  for (const char* name : {"rgb420_17x19.jpg", "rgb422_17x19.jpg", "rgb444_17x19.jpg", "gray_17x19.jpg", "rgb420_320x240.jpg"}) clean(read(directory + "/" + name));
  auto jpeg = read(directory + "/rgb420_320x240.jpg");
  const size_t scan = marker(jpeg, 0xda);
  const size_t entropy = scan + 2 + (size_t(jpeg[scan + 2]) << 8) + jpeg[scan + 3];
  // Public synthetic corruption keeps all headers and EOI intact: inspect
  // alone accepts it, while complete entropy validation must reject it.
  for (size_t keep : {size_t(0), size_t(1), size_t(8), (jpeg.size() - entropy - 2) / 2}) {
    auto broken = jpeg;
    broken.erase(broken.begin() + entropy + keep, broken.end() - 2);
    Info info; const char* error = nullptr;
    assert(inspect(broken.data(), broken.size(), info));
    assert(!validateSoftware(broken.data(), broken.size(), info, {}, &error));
    assert(error);
  }
  auto padded = jpeg; padded.insert(padded.begin() + 2, 3, 0xff); clean(padded);
  padded = jpeg; padded.insert(padded.end() - 2, 3, 0xff); clean(padded);
  Info info; const char* error = nullptr;
  DecodeOptions limits; limits.maxInputBytes = jpeg.size() - 1;
  unsigned before = allocations;
  assert(!validateSoftware(jpeg.data(), jpeg.size(), info, limits)); assert(allocations == before);
  limits = {}; limits.maxWidth = 319;
  assert(!validateSoftware(jpeg.data(), jpeg.size(), info, limits)); assert(allocations == before);
  limits = {}; limits.maxOutputBytes = 320 * 240 * 3 - 1;
  assert(!validateSoftware(jpeg.data(), jpeg.size(), info, limits)); assert(allocations == before);
  auto progressive = read(directory + "/progressive_17x19.jpg");
  assert(!validateSoftware(progressive.data(), progressive.size(), info, {}, &error));
  assert(error && strstr(error, "baseline") && allocations == before);
  noMemory = true;
  assert(!validateSoftware(jpeg.data(), jpeg.size(), info, {}, &error));
  assert(error && strstr(error, "memory")); noMemory = false;
  if (argc == 3) {
    auto bad = read(argv[2]); assert(inspect(bad.data(), bad.size(), info));
    assert(!validateSoftware(bad.data(), bad.size(), info, {}, &error));
    puts("Retained private corrupt camera JPEG rejected (fixture not copied to repository)");
  }
  printf("JPEG entropy validation PASS; maximum scratch allocation=%zu; no RGB frame allocation\n", maxAllocation);
}
