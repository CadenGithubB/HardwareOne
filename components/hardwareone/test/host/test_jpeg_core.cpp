#include "../../HAL_JPEG_Backend.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace hwjpeg;
namespace {
int hardwareCalls = 0, softwareCalls = 0;
bool hardwarePass = false, softwarePass = true;
OutputAllocator expectedAllocator = nullptr;
const std::vector<uint8_t>* expectedCanonical = nullptr;
void checkCanonical(const uint8_t* data, size_t length) {
  if (expectedCanonical) {
    assert(length == expectedCanonical->size());
    assert(std::memcmp(data, expectedCanonical->data(), length) == 0);
  }
}
std::vector<uint8_t> readFile(const std::string& name) {
  std::ifstream file(name, std::ios::binary);
  assert(file.good());
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void resetBackend() { hardwareCalls = softwareCalls = 0; hardwarePass = false; softwarePass = true; }
bool fillImage(const Info& info, Image& image, Backend backend, bool succeed) {
  image.pixels = static_cast<uint8_t*>(std::malloc(info.bytes));
  assert(image.pixels);
  std::memset(image.pixels, backend == Backend::Hardware ? 0x53 : 0x71, info.bytes);
  image.width = info.width; image.height = info.height;
  image.size = info.bytes; image.stride = info.width * 3; image.backend = backend;
  // Deliberately dirty partial results on failure: the dispatcher must never
  // expose them or leak them before falling back or returning a failed decode.
  return succeed;
}
size_t marker(const std::vector<uint8_t>& data, uint8_t value) {
  for (size_t i = 2; i + 1 < data.size(); ++i)
    if (data[i] == 0xff && data[i + 1] == value) return i;
  assert(false); return 0;
}
void rejected(const std::vector<uint8_t>& data, const DecodeOptions& opts = {}) {
  Info info; const char* error = nullptr;
  assert(!inspect(data.data(), data.size(), info, opts, &error));
  resetBackend(); Image image;
  assert(!decode(data.data(), data.size(), image, opts, &error));
  assert(hardwareCalls == 0 && softwareCalls == 0);
  assert(!image.pixels && image.size == 0 && image.backend == Backend::None);
}
}
namespace hwjpeg { namespace detail {
bool validateSoftwareEntropy(const uint8_t*, size_t, const Info&, const char**) { return true; }
bool decodeHardware(const uint8_t* data, size_t length, const Info& info, Image& image, const char**) {
  checkCanonical(data, length);
  ++hardwareCalls; return fillImage(info, image, Backend::Hardware, hardwarePass);
}
bool decodeSoftware(const uint8_t* data, size_t length, const Info& info, Image& image, const char**, OutputAllocator allocator) {
  checkCanonical(data, length);
  assert(allocator == expectedAllocator);
  ++softwareCalls; return fillImage(info, image, Backend::Software, softwarePass);
}
}}
int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string fixtures = argv[1];
  auto jpeg = readFile(fixtures + "/rgb420_17x19.jpg");
  Info info; const char* error = nullptr;
  assert(inspect(jpeg.data(), jpeg.size(), info, {}, &error));
  assert(info.width == 17 && info.height == 19 && info.bytes == 17 * 19 * 3);
  assert(info.precision == 8 && info.components == 3 && info.sofMarker == 0xc0);
  assert(info.sampling[0] == 0x22 && info.sampling[1] == 0x11 && info.sampling[2] == 0x11);
  for (const char* name : {"gray_17x19.jpg", "progressive_17x19.jpg", "rgb444_24x24.jpg"}) {
    auto other = readFile(fixtures + "/" + name);
    assert(inspect(other.data(), other.size(), info, {}, &error));
  }
  const size_t sof = marker(jpeg, 0xc0), sos = marker(jpeg, 0xda);
  const size_t sosEnd = sos + 2 + (jpeg[sos + 2] << 8) + jpeg[sos + 3];
  for (size_t size = 0; size < sosEnd; ++size) {
    Info bad;
    assert(!inspect(jpeg.data(), size, bad));
  }
  assert(!inspect(nullptr, jpeg.size(), info));
  auto bad = jpeg; bad[0] = 0; rejected(bad);
  bad = jpeg; bad.insert(bad.begin() + 2, 1, 0xff);
  assert(inspect(bad.data(), bad.size(), info)); // one extra marker fill byte
  bad = jpeg; bad.insert(bad.begin() + 2, 2, 0xff);
  assert(inspect(bad.data(), bad.size(), info));
  bad = jpeg; bad.insert(bad.end() - 2, 2, 0xff);
  assert(inspect(bad.data(), bad.size(), info));
  bad = jpeg; const uint8_t invalidStuffing[] = {0xff, 0xff, 0x00};
  bad.insert(bad.begin() + static_cast<std::ptrdiff_t>(sosEnd), std::begin(invalidStuffing), std::end(invalidStuffing));
  rejected(bad);
  bad = jpeg; bad[sof + 5] = bad[sof + 6] = 0; rejected(bad); // zero height
  bad = jpeg; bad[sof + 7] = bad[sof + 8] = 0; rejected(bad); // zero width
  bad = jpeg; bad[sof + 2] = 0; bad[sof + 3] = 1; rejected(bad);
  bad = jpeg; bad[2] = 0; rejected(bad); // no marker prefix
  bad = jpeg; bad[3] = 0xd9; rejected(bad); // EOI before frame
  bad = jpeg; bad[3] = 0xda; rejected(bad); // scan before frame
  bad = jpeg; bad[4] = 0xff; bad[5] = 0xff; rejected(bad); // segment overrun
  bad = jpeg;
  const size_t sofEnd = sof + 2 + (jpeg[sof + 2] << 8) + jpeg[sof + 3];
  bad.insert(bad.begin() + static_cast<std::ptrdiff_t>(sofEnd), jpeg.begin() + static_cast<std::ptrdiff_t>(sof), jpeg.begin() + static_cast<std::ptrdiff_t>(sofEnd));
  rejected(bad); // conflicting or duplicate frames must not change allocation size
  bad = jpeg; bad[sos + 2] = 0; bad[sos + 3] = 1; rejected(bad);
  bad = jpeg; bad[sos + 4] = 0; rejected(bad); // empty scan
  // Fake frame markers inside length-delimited metadata must be ignored.
  bad = jpeg;
  const uint8_t app[] = {0xff, 0xe2, 0, 10, 0xff, 0xc0, 0, 2, 0xff, 0xda, 0, 2};
  bad.insert(bad.begin() + 2, std::begin(app), std::end(app));
  assert(inspect(bad.data(), bad.size(), info));
  assert(info.width == 17 && info.height == 19);
  DecodeOptions opts;
  opts.maxWidth = 16; rejected(jpeg, opts);
  opts = {}; opts.maxHeight = 18; rejected(jpeg, opts);
  opts = {}; opts.maxOutputBytes = 17 * 19 * 3 - 1; rejected(jpeg, opts);
  opts = {}; opts.maxInputBytes = jpeg.size() - 1; rejected(jpeg, opts);
  opts = {}; opts.maxWidth = 17; opts.maxHeight = 19; opts.maxOutputBytes = 17 * 19 * 3;
  assert(inspect(jpeg.data(), jpeg.size(), info, opts));
  // Large dimensions are bounded before either backend can allocate.
  bad = jpeg; bad[sof + 5] = bad[sof + 6] = bad[sof + 7] = bad[sof + 8] = 0xff;
  rejected(bad);
  opts = {}; opts.maxWidth = opts.maxHeight = 65535;
  opts.maxOutputBytes = std::numeric_limits<size_t>::max();
  rejected(bad, opts); // uint32_t decoder capacities cannot hold 65535*65535*3
  const size_t dqt = marker(jpeg, 0xdb), dht = marker(jpeg, 0xc4);
  bad = jpeg; bad[dqt + 4] = 0x20; rejected(bad); // unsupported table precision
  bad = jpeg; bad[dht + 5] = 255; rejected(bad); // oversubscribed Huffman tree
  bad = jpeg; bad[sof + 13] = bad[sof + 10]; rejected(bad); // duplicate component IDs

  opts = {}; Image image;
  resetBackend();
  assert(decode(jpeg.data(), jpeg.size(), image, opts));
  assert(hardwareCalls == 1 && softwareCalls == 1 && image.backend == Backend::Software);
  assert(image.stride == 51 && image.size == 969 && image.pixels[0] == 0x71);
  resetBackend(); hardwarePass = true;
  assert(decode(jpeg.data(), jpeg.size(), image, opts));
  assert(hardwareCalls == 1 && softwareCalls == 0 && image.backend == Backend::Hardware);
  opts.mode = DecodeMode::SoftwareOnly; resetBackend(); hardwarePass = true;
  assert(decode(jpeg.data(), jpeg.size(), image, opts));
  assert(hardwareCalls == 0 && softwareCalls == 1 && image.backend == Backend::Software);
  opts.mode = DecodeMode::HardwareOnly; resetBackend();
  assert(!decode(jpeg.data(), jpeg.size(), image, opts));
  assert(hardwareCalls == 1 && softwareCalls == 0 && !image.pixels && image.size == 0);
  opts = {}; resetBackend(); softwarePass = false;
  assert(!decode(jpeg.data(), jpeg.size(), image, opts));
  assert(hardwareCalls == 1 && softwareCalls == 1 && !image.pixels && image.backend == Backend::None);
  resetBackend(); assert(decode(jpeg.data(), jpeg.size(), image));
  assert(!decode(nullptr, 0, image));
  assert(!image.pixels && !image.width && !image.height && !image.size && !image.stride);
  // A caller's allocation policy must reach the software backend unchanged,
  // including when software is selected after a hardware failure.
  expectedAllocator = +[](size_t size) { return static_cast<uint8_t*>(std::malloc(size)); };
  opts = {}; opts.softwareAllocator = expectedAllocator; resetBackend();
  assert(decode(jpeg.data(), jpeg.size(), image, opts));
  assert(hardwareCalls == 1 && softwareCalls == 1);
  expectedAllocator = nullptr;
  expectedCanonical = &jpeg;
  for (int fill : {1, 2, 17}) {
    bad = jpeg; bad.insert(bad.end() - 2, fill, 0xff);
    bad.insert(bad.begin() + 2, fill, 0xff);
    const auto original = bad;
    resetBackend(); opts = {};
    assert(decode(bad.data(), bad.size(), image, opts));
    assert(hardwareCalls == 1 && softwareCalls == 1 && bad == original);
  }
  expectedCanonical = nullptr;
  // Normalization storage must honor a supplied allocator's failure; it may
  // not bypass a diagnostic memory policy to reach either decoder anyway.
  opts.softwareAllocator = +[](size_t) -> uint8_t* { return nullptr; };
  resetBackend();
  assert(!decode(bad.data(), bad.size(), image, opts));
  assert(hardwareCalls == 0 && softwareCalls == 0 && !image.pixels);
  image.reset(); image.reset();
  assert(std::strcmp(backendName(Backend::Software), "software") == 0);
  assert(std::strcmp(backendName(Backend::Hardware), "hardware") == 0);
  std::puts("JPEG metadata, resource limits and backend fallback tests passed");
}
