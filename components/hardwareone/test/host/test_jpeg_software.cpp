#include "HAL_JPEG_Backend.h"
#include "jpeg_decoder.h"
#include "esp_heap_caps.h"
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
enum class Failure { None, Psram, AllMemory, Workspace, Decoder, Width, Height, OutputLength };
Failure failure = Failure::None;
int calls = 0, allocations = 0, customCalls = 0, hardwareCalls = 0, workspaceCalls = 0;
bool bypassEnabled = false;
bool customFails = false;
uint8_t* customAllocate(size_t size) {
 ++customCalls; assert(size == 969);
 return customFails ? nullptr : static_cast<uint8_t*>(std::malloc(size));
}
std::vector<uint32_t> requestedCaps;
}
extern "C" void* heap_caps_malloc(size_t size, uint32_t caps) {
 if (size == 969) {
   ++allocations; requestedCaps.push_back(caps);
   if (failure == Failure::AllMemory || (failure == Failure::Psram && (caps & MALLOC_CAP_SPIRAM))) return nullptr;
 } else {
   ++workspaceCalls;
   assert(size >= 3100 && caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
   if (failure == Failure::Workspace) return nullptr;
 }
 return std::malloc(size);
}
extern "C" esp_err_t esp_jpeg_decode(esp_jpeg_image_cfg_t* config, esp_jpeg_image_output_t* result) {
 ++calls; assert(config->out_format == JPEG_IMAGE_FORMAT_RGB888 && config->out_scale == JPEG_IMAGE_SCALE_0);
 assert(config->flags.swap_color_bytes == 0);
 assert(config->advanced.working_buffer != nullptr && config->advanced.working_buffer_size >= 3100);
 Info info; assert(inspect(config->indata, config->indata_size, info));
 assert(config->outbuf_size == info.bytes);
 result->width = info.width; result->height = info.height; result->output_len = info.bytes;
 std::memset(config->outbuf, 0x37, info.bytes);
 if (failure == Failure::Width) ++result->width;
 if (failure == Failure::Height) ++result->height;
 if (failure == Failure::OutputLength) --result->output_len;
 return failure == Failure::Decoder ? ESP_FAIL : ESP_OK;
}
namespace hwjpeg { namespace detail {
bool decodeHardware(const uint8_t*, size_t, const Info&, Image&, const char**) { ++hardwareCalls; return false; }
}}
enum class AllocPref { PreferPSRAM };
void* ps_alloc(size_t size, AllocPref preference, const char* tag) {
 assert(preference == AllocPref::PreferPSRAM);
 assert(std::strcmp(tag, "g2.jpeg.rgb") == 0);
 return customAllocate(size);
}
bool psramBypassEnabled() { return bypassEnabled; }
// INSERT_G2_JPEG_HELPER
int main(int argc, char** argv) {
 assert(argc == 2); std::ifstream file(std::string(argv[1]) + "/rgb420_17x19.jpg", std::ios::binary); assert(file.good());
 std::vector<uint8_t> jpeg{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
 DecodeOptions options; options.mode = DecodeMode::SoftwareOnly;
 Image image;
 for (Failure f : {Failure::None, Failure::Psram, Failure::AllMemory, Failure::Workspace, Failure::Decoder, Failure::Width, Failure::Height, Failure::OutputLength}) {
   failure = f; calls = allocations = workspaceCalls = 0; requestedCaps.clear();
   bool ok = decode(jpeg.data(), jpeg.size(), image, options);
   bool expected = f == Failure::None || f == Failure::Psram;
   assert(ok == expected);
   assert(requestedCaps[0] == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
   if (f == Failure::Psram || f == Failure::AllMemory) {
     assert(allocations == 2 && requestedCaps[1] == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
   } else assert(allocations == 1);
   assert(calls == (f == Failure::AllMemory || f == Failure::Workspace ? 0 : 1));
   assert(workspaceCalls == (f == Failure::AllMemory ? 0 : 1));
   if (ok) assert(image.backend == Backend::Software && image.size == 969 && image.stride == 51 && image.pixels[0] == 0x37);
   else assert(!image.pixels && image.size == 0 && image.backend == Backend::None);
 }
 options.softwareAllocator = customAllocate;
 for (bool failAllocation : {false, true}) {
   failure = Failure::None; calls = allocations = customCalls = 0;
   requestedCaps.clear(); customFails = failAllocation;
   assert(decode(jpeg.data(), jpeg.size(), image, options) == !failAllocation);
   assert(customCalls == 1 && allocations == 0 && requestedCaps.empty());
   assert(calls == (failAllocation ? 0 : 1));
   if (failAllocation) assert(!image.pixels && image.backend == Backend::None);
   else assert(image.backend == Backend::Software && image.size == 969);
 }
 failure = Failure::Decoder; customFails = false; calls = allocations = customCalls = 0;
 assert(!decode(jpeg.data(), jpeg.size(), image, options));
 assert(customCalls == 1 && allocations == 0 && calls == 1 && !image.pixels);
 assert(hardwareCalls == 0); // Every direct check above requested software.
 failure = Failure::None; customFails = false;
 for (bool bypass : {false, true}) {
   bypassEnabled = bypass; calls = allocations = customCalls = hardwareCalls = 0;
   assert(decodeJpegForG2(jpeg.data(), jpeg.size(), image, nullptr));
   assert(customCalls == 1 && allocations == 0 && calls == 1);
   assert(hardwareCalls == (bypass ? 0 : 1));
   assert(image.backend == Backend::Software && image.size == 969);
 }
 std::puts("JPEG software capacity, allocator policy, G2 bypass, PSRAM fallback and failure cleanup tests passed");
}
