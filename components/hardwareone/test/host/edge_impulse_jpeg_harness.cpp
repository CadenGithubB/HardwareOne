#include "HAL_JPEG_Backend.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

static unsigned converterCalls;
static bool converterSucceeds = true;
static constexpr int PIXFORMAT_JPEG = 4;
// Only the legacy converter is mocked. The production guard and shared JPEG
// parser run unchanged; a missed capacity guard makes this full-size write
// overflow the caller's actual buffer under ASan.
static bool fmt2rgb888(const uint8_t* jpeg, size_t length, int format, uint8_t* rgb) {
  ++converterCalls;
  assert(format == PIXFORMAT_JPEG);
  hwjpeg::Info info;
  assert(hwjpeg::inspect(jpeg, length, info));
  std::memset(rgb, 0x27, info.bytes);
  return converterSucceeds;
}
// INSERT_EDGE_IMPULSE_JPEG_HELPER

namespace hwjpeg { namespace detail {
bool validateSoftwareEntropy(const uint8_t*, size_t, const Info&, const char**) { assert(false); return false; }
bool decodeHardware(const uint8_t*, size_t, const Info&, Image&, const char**) { assert(false); return false; }
bool decodeSoftware(const uint8_t*, size_t, const Info&, Image&, const char**, OutputAllocator) { assert(false); return false; }
}}
static std::vector<uint8_t> readFile(const std::string& name) {
  std::ifstream input(name, std::ios::binary); assert(input.good());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
static void rejected(const uint8_t* jpeg, size_t length, size_t capacity = 640u * 480u * 3u) {
  std::vector<uint8_t> rgb(capacity ? capacity : 1, 0x53);
  int width = 123, height = 456;
  const char* error = nullptr;
  converterCalls = 0;
  assert(!decodeEdgeImpulseJpeg(jpeg, length, rgb.data(), capacity, width, height, &error));
  assert(converterCalls == 0 && width == 0 && height == 0 && error);
  assert(std::all_of(rgb.begin(), rgb.end(), [](uint8_t v) { return v == 0x53; }));
}
static size_t marker(const std::vector<uint8_t>& jpeg, uint8_t value) {
  for (size_t i = 2; i + 1 < jpeg.size(); ++i)
    if (jpeg[i] == 0xff && jpeg[i + 1] == value) return i;
  assert(false); return 0;
}
int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string fixtures = argv[1];
  const auto small = readFile(fixtures + "/rgb420_17x19.jpg");
  const auto vga = readFile(fixtures + "/solid_640x480.jpg");
  const auto hd = readFile(fixtures + "/solid_1280x720.jpg");
  for (const auto* jpeg : {&small, &vga}) {
    hwjpeg::Info info;
    assert(hwjpeg::inspect(jpeg->data(), jpeg->size(), info));
    std::vector<uint8_t> rgb(info.bytes);
    int width = 640, height = 480;
    const char* error = "stale";
    converterCalls = 0;
    assert(decodeEdgeImpulseJpeg(jpeg->data(), jpeg->size(), rgb.data(), rgb.size(), width, height, &error));
    assert(converterCalls == 1 && !error);
    assert(width == static_cast<int>(info.width) && height == static_cast<int>(info.height));
    assert(rgb.front() == 0x27 && rgb.back() == 0x27);
    rejected(jpeg->data(), jpeg->size(), info.bytes - 1);
    rejected(jpeg->data(), jpeg->size(), 0);
  }
  rejected(hd.data(), hd.size());
  // An oversized RGB allocation must not relax the module's VGA geometry cap.
  rejected(hd.data(), hd.size(), 1280u * 720u * 3u);
  rejected(nullptr, small.size());
  const uint8_t oneByte[] = {0xff};
  rejected(oneByte, sizeof(oneByte));
  rejected(oneByte, 0);
  rejected(oneByte, static_cast<size_t>(UINT32_MAX) + 1);
  const size_t sof = marker(small, 0xc0), sos = marker(small, 0xda);
  for (size_t length = 0; length < sos + 4; ++length) {
    // Exact-size allocation detects any short-input overread under ASan.
    const std::vector<uint8_t> truncated(small.begin(), small.begin() + length);
    rejected(truncated.data(), truncated.size());
  }
  auto invalid = small; invalid[0] = 0; rejected(invalid.data(), invalid.size());
  invalid = small; invalid.pop_back(); rejected(invalid.data(), invalid.size());
  invalid = small; invalid[sof + 2] = 0; invalid[sof + 3] = 1;
  rejected(invalid.data(), invalid.size());
  invalid = small; invalid[sof + 7] = 2; invalid[sof + 8] = 129; // width641
  rejected(invalid.data(), invalid.size());
  invalid = small; invalid[sof + 5] = 1; invalid[sof + 6] = 225; // height481
  rejected(invalid.data(), invalid.size());
  int width = 100, height = 200;
  const char* error = nullptr;
  converterCalls = 0;
  assert(!decodeEdgeImpulseJpeg(small.data(), small.size(), nullptr, 921600, width, height, &error));
  assert(converterCalls == 0 && width == 0 && height == 0 && error);
  std::vector<uint8_t> rgb(17u * 19u * 3u);
  converterSucceeds = false;
  assert(!decodeEdgeImpulseJpeg(small.data(), small.size(), rgb.data(), rgb.size(), width, height, &error));
  assert(converterCalls == 1 && width == 0 && height == 0 && error);
  puts("EDGE_IMPULSE_JPEG PASS");
}
