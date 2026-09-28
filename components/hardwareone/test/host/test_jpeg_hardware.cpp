#include "../../HAL_JPEG_Backend.h"
#include "driver/jpeg_decode.h"
#include "soc/soc_caps.h"
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
enum class Failure { None, Input, InputShort, Output, OutputShort, OutputHuge, Engine, Process, ShortResult, LongResult, Delete };
Failure failure = Failure::None;
int allocations = 0, engines = 0, created = 0, deleted = 0, processed = 0, software = 0;
uint32_t paddedW = 32, paddedH = 32;
size_t inputAllocationCapacity = 0;
bool probeBusy = false;
std::vector<uint8_t> expectedInput;
void reset(Failure f = Failure::None) { assert(engines == 0); failure = f; allocations = created = deleted = processed = software = 0; }
uint8_t channel(uint32_t x, uint32_t y, uint32_t c) { return static_cast<uint8_t>((x * 19 + y * 41 + c * 73) & 255); }
std::vector<uint8_t> readFile(const std::string& name) {
 std::ifstream f(name, std::ios::binary); assert(f.good());
 return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
}
void* jpeg_alloc_decoder_mem(size_t bytes, const jpeg_decode_memory_alloc_cfg_t* config, size_t* capacity) {
 ++allocations;
 const bool input = config->buffer_direction == JPEG_DEC_ALLOC_INPUT_BUFFER;
 if ((input && failure == Failure::Input) || (!input && failure == Failure::Output)) return nullptr;
 *capacity = input ? bytes + 16 : bytes;
 if (input) inputAllocationCapacity = *capacity;
 if ((input && failure == Failure::InputShort) || (!input && failure == Failure::OutputShort)) *capacity = bytes - 1;
 if (!input && failure == Failure::OutputHuge) *capacity = size_t(UINT32_MAX) + 1;
 const size_t actual = bytes + (input ? 16 : 0);
 void* p = std::malloc(actual); assert(p); std::memset(p, 0xcd, actual); return p;
}
esp_err_t jpeg_new_decoder_engine(const jpeg_decode_engine_cfg_t* config, jpeg_decoder_handle_t* engine) {
 assert(config->timeout_ms > 0 && config->timeout_ms <= 1000);
 if (failure == Failure::Engine) return ESP_FAIL;
 ++engines; ++created; *engine = reinterpret_cast<void*>(1); return ESP_OK;
}
esp_err_t jpeg_del_decoder_engine(jpeg_decoder_handle_t engine) {
 assert(engine && engines == 1); --engines; ++deleted;
 return failure == Failure::Delete ? ESP_FAIL : ESP_OK;
}
esp_err_t jpeg_decoder_process(jpeg_decoder_handle_t engine, const jpeg_decode_cfg_t* config,
 const uint8_t* input, uint32_t length, uint8_t* output, uint32_t capacity, uint32_t* outSize) {
 assert(engine && engines == 1); ++processed;
 assert(config->output_format == JPEG_DECODE_OUT_FORMAT_RGB888);
 assert(config->rgb_order == JPEG_DEC_RGB_ELEMENT_ORDER_RGB);
 assert(config->conv_std == JPEG_YUV_RGB_CONV_STD_BT601);
 assert(length == expectedInput.size() && std::memcmp(input, expectedInput.data(), length) == 0);
 assert(input != expectedInput.data()); // caller ownership is not passed to DMA
 for (size_t i = length; i < inputAllocationCapacity; ++i) assert(input[i] == 0);
 assert(capacity >= paddedW * paddedH * 3);
 if (probeBusy) {
   Info info; assert(inspect(expectedInput.data(), expectedInput.size(), info));
   Image nested; const int before = allocations;
   assert(!detail::decodeHardware(expectedInput.data(), expectedInput.size(), info, nested, nullptr));
   assert(!nested.pixels && allocations == before && engines == 1);
 }
 for (uint32_t y = 0; y < paddedH; ++y)
   for (uint32_t x = 0; x < paddedW; ++x)
     for (uint32_t c = 0; c < 3; ++c) output[(y * paddedW + x) * 3 + c] = channel(x, y, c);
 *outSize = paddedW * paddedH * 3;
 if (failure == Failure::ShortResult) --*outSize;
 if (failure == Failure::LongResult) ++*outSize;
 return failure == Failure::Process ? ESP_FAIL : ESP_OK;
}
namespace hwjpeg { namespace detail {
bool decodeSoftware(const uint8_t*, size_t, const Info& info, Image& image, const char**, OutputAllocator) {
 ++software; image.pixels = static_cast<uint8_t*>(std::malloc(info.bytes)); assert(image.pixels);
 std::memset(image.pixels, 0xee, info.bytes); image.width = info.width; image.height = info.height;
 image.size = info.bytes; image.stride = info.width * 3; image.backend = Backend::Software; return true;
}
}}
int main(int argc, char** argv) {
 assert(argc == 2); std::string fixtures = argv[1];
 expectedInput = readFile(fixtures + "/rgb420_24x24.jpg");
 reset(); Image image; DecodeOptions only; only.mode = DecodeMode::HardwareOnly;
#if !SOC_JPEG_DECODE_SUPPORTED || !HW1_JPEG_DRIVER_QUALIFIED
 assert(decode(expectedInput.data(), expectedInput.size(), image));
 assert(image.backend == Backend::Software && software == 1 && allocations == 0 && created == 0);
 assert(!decode(expectedInput.data(), expectedInput.size(), image, only));
 assert(!image.pixels && software == 1 && allocations == 0);
 std::puts("JPEG hardware-absent fallback tests passed"); return 0;
#else
 for (const char* sampling : {"420", "422", "444"}) {
   expectedInput = readFile(fixtures + "/rgb" + sampling + "_24x24.jpg");
   paddedW = std::strcmp(sampling, "444") ? 32 : 24;
   paddedH = std::strcmp(sampling, "420") ? 24 : 32;
   reset(); probeBusy = true;
   assert(decode(expectedInput.data(), expectedInput.size(), image, only));
   assert(image.backend == Backend::Hardware && image.width == 24 && image.height == 24);
   assert(image.stride == 72 && image.size == 1728 && engines == 0 && created == 1 && deleted == 1);
   assert(software == 0 && processed == 1);
   for (uint32_t y = 0; y < 24; ++y)
     for (uint32_t x = 0; x < 24; ++x)
       for (uint32_t c = 0; c < 3; ++c) assert(image.pixels[(y * 24 + x) * 3 + c] == channel(x, y, c));
 }
 probeBusy = false; expectedInput = readFile(fixtures + "/rgb420_24x24.jpg"); paddedW = paddedH = 32;
 for (Failure f : {Failure::Input, Failure::InputShort, Failure::Output, Failure::OutputShort,
                   Failure::OutputHuge, Failure::Engine, Failure::Process, Failure::ShortResult,
                   Failure::LongResult, Failure::Delete}) {
   reset(f); assert(!decode(expectedInput.data(), expectedInput.size(), image, only));
   assert(!image.pixels && engines == 0 && software == 0 && created == deleted);
   reset(f); assert(decode(expectedInput.data(), expectedInput.size(), image));
   assert(image.backend == Backend::Software && software == 1 && engines == 0 && created == deleted);
   reset(); assert(decode(expectedInput.data(), expectedInput.size(), image, only)); // failure must release admission
 }
 for (const char* file : {"rgb420_17x19.jpg", "gray_17x19.jpg", "progressive_17x19.jpg"}) {
   expectedInput = readFile(fixtures + "/" + file); reset();
   assert(decode(expectedInput.data(), expectedInput.size(), image));
   assert(image.backend == Backend::Software && software == 1 && allocations == 0 && created == 0);
 }
 expectedInput = readFile(fixtures + "/rgb420_24x24.jpg");
 for (int i = 0; i < 100; ++i) { reset(); assert(decode(expectedInput.data(), expectedInput.size(), image, only)); }
 std::puts("JPEG hardware resource, RGB order, padded-stride and failure fallback tests passed");
#endif
}
