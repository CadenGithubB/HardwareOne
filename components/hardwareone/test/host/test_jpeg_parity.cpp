#include "../../HAL_JPEG.h"
#include "esp_heap_caps.h"
#include "img_converters.h"
#include "sdkconfig.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
using namespace hwjpeg;
extern "C" void* heap_caps_malloc(size_t size, uint32_t) { return std::malloc(size); }
namespace {
std::vector<uint8_t> readFile(const std::string& name) {
 std::ifstream f(name, std::ios::binary); assert(f.good());
 return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
struct Fixture { std::vector<uint8_t> jpeg, expected; uint32_t width, height; };
}
int main(int argc, char** argv) {
 assert(argc == 2); std::string path = argv[1];
 DecodeOptions options; options.mode = DecodeMode::SoftwareOnly;
 std::vector<Fixture> fixtures; unsigned legacyCompatible = 0, expandedWorkspace = 0;
 for (const char* name : {"rgb420_17x19.jpg", "rgb422_17x19.jpg", "rgb444_17x19.jpg",
       "rgb420_24x24.jpg", "rgb422_24x24.jpg", "rgb444_24x24.jpg", "gray_17x19.jpg",
       "rgb420_320x240.jpg", "rgb422_320x240.jpg", "rgb444_320x240.jpg", "red_8x8.jpg", "blue_8x8.jpg", "rgb444_48x48.jpg", "red_48x48.jpg", "blue_48x48.jpg"}) {
   auto jpeg = readFile(path + "/" + name); Info info;
   assert(inspect(jpeg.data(), jpeg.size(), info));
   std::vector<uint8_t> legacy(info.bytes);
   const bool legacyOk = fmt2rgb888(jpeg.data(), jpeg.size(), PIXFORMAT_JPEG, legacy.data());
   Image image; assert(decode(jpeg.data(), jpeg.size(), image, options));
   assert(image.width == info.width && image.height == info.height && image.size == info.bytes);
   assert(image.stride == info.width * 3 && image.backend == Backend::Software);
   if (legacyOk) {
     assert(std::memcmp(image.pixels, legacy.data(), info.bytes) == 0);
     ++legacyCompatible;
   } else {
     // esp32-camera's fixed 3100-byte scratch cannot hold fast-mode MCU/LUT
     // data. The new private workspace must still decode every baseline fixture.
     assert(CONFIG_JD_FASTDECODE > 0);
     std::memcpy(legacy.data(), image.pixels, info.bytes);
     ++expandedWorkspace;
   }
   if (std::strcmp(name, "red_8x8.jpg") == 0) {
     assert(image.pixels[0] > 245 && image.pixels[1] < 5 && image.pixels[2] < 5);
   } else if (std::strcmp(name, "blue_8x8.jpg") == 0) {
     assert(image.pixels[0] < 5 && image.pixels[1] < 5 && image.pixels[2] > 245);
   }
   fixtures.push_back({std::move(jpeg), std::move(legacy), info.width, info.height});
 }
 // The old decoder does not support progressive JPEG. This remains a clean
 // failure, not a newly promised feature or an accidental successful black image.
 auto progressive = readFile(path + "/progressive_17x19.jpg");
 std::vector<uint8_t> progOut(17 * 19 * 3); Image image;
 assert(!fmt2rgb888(progressive.data(), progressive.size(), PIXFORMAT_JPEG, progOut.data()));
 assert(!decode(progressive.data(), progressive.size(), image, options));
 assert(!image.pixels);
 // Repeated marker-prefix regression: normalize legal marker fill bytes
 // before either software implementation (including the older S3 ROM decoder).
 auto padded = fixtures[0].jpeg;
 padded.insert(padded.begin() + 2, 1, 0xff);
 assert(decode(padded.data(), padded.size(), image, options));
 assert(image.size == fixtures[0].expected.size());
 assert(std::memcmp(image.pixels, fixtures[0].expected.data(), image.size) == 0);
 for (int fill : {2, 3, 17}) {
   padded = fixtures[0].jpeg; padded.insert(padded.begin() + 2, fill, 0xff);
   auto original = padded;
   assert(decode(padded.data(), padded.size(), image, options));
   assert(image.size == fixtures[0].expected.size());
   assert(std::memcmp(image.pixels, fixtures[0].expected.data(), image.size) == 0 && padded == original);
   padded = fixtures[0].jpeg; padded.insert(padded.end() - 2, fill, 0xff);
   original = padded;
   assert(decode(padded.data(), padded.size(), image, options));
   assert(image.size == fixtures[0].expected.size());
   assert(std::memcmp(image.pixels, fixtures[0].expected.data(), image.size) == 0 && padded == original);
 }
 for (size_t length = 0; length < fixtures[0].jpeg.size(); ++length) {
   assert(!decode(fixtures[0].jpeg.data(), length, image, options));
   assert(!image.pixels);
 }
 // Independent calls must have private JPEG workspaces. Compare every pixel
 // while alternating input dimensions/subsampling from eight concurrent callers.
 std::atomic<int> completed{0}; std::vector<std::thread> workers;
 for (int worker = 0; worker < 8; ++worker) workers.emplace_back([&, worker] {
   for (int round = 0; round < 24; ++round) {
     const auto& fixture = fixtures[(worker + round) % 7];
     Image decoded; assert(decode(fixture.jpeg.data(), fixture.jpeg.size(), decoded, options));
     assert(decoded.width == fixture.width && decoded.height == fixture.height);
     assert(decoded.size == fixture.expected.size());
     assert(std::memcmp(decoded.pixels, fixture.expected.data(), decoded.size) == 0);
     ++completed;
   }
 });
 for (auto& worker : workers) worker.join();
 assert(completed == 8 * 24);
 assert(legacyCompatible + expandedWorkspace == 15);
 assert(expandedWorkspace == (CONFIG_JD_FASTDECODE == 0 ? 0u : CONFIG_JD_FASTDECODE == 1 ? 3u : 15u));
 std::printf("JPEG actual TJpgDec FASTDECODE=%d: legacy parity=%u workspace-repaired=%u; 192 concurrent decodes passed\n",
             CONFIG_JD_FASTDECODE, legacyCompatible, expandedWorkspace);
}
