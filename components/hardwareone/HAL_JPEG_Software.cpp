#include "HAL_JPEG_Backend.h"
#include "jpeg_decoder.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include <stdlib.h>
#include <string.h>
#if defined(CONFIG_JD_USE_ROM) && CONFIG_JD_USE_ROM
#include "rom/tjpgd.h"
#else
#include "tjpgd.h"
#endif

namespace hwjpeg { namespace detail {
namespace {
// esp_jpeg 1.3.1 defaults to 3100 bytes for both BASIC and 32BIT TJpgDec.
// The latter stores MCU samples as int16_t, so ordinary 4:2:0 needs more than
// that default. Size a private pool for its input buffer, four quantization
// tables, both DC/AC Huffman tables (up to 256 symbols each), and largest MCU.
#if defined(CONFIG_JD_USE_ROM) && CONFIG_JD_USE_ROM
constexpr size_t kWorkspaceBytes = 3100; // Original ROM decoder contract.
#else
constexpr size_t kTableBytes = 4u * (16u + 256u * 3u) + 4u * 64u * 4u;
constexpr size_t kMcuBytes = 4u * 64u * 2u + 64u + 6u * 64u *
    (CONFIG_JD_FASTDECODE >= 1 ? 2u : 1u);
constexpr size_t kBaseWorkspace = CONFIG_JD_SZBUF + kTableBytes + kMcuBytes + 64u;
#if CONFIG_JD_FASTDECODE == 2
// Retain the upstream recommendation for its Huffman lookup-table variant.
constexpr size_t kWorkspaceBytes = kBaseWorkspace + 6u * 1024u > 65472u ?
    kBaseWorkspace + 6u * 1024u : 65472u;
#else
constexpr size_t kWorkspaceBytes = kBaseWorkspace;
#endif
#endif

// Keep codec ABI differences inside this backend: IDF ROM uses the original
// unsigned-int callbacks, while bundled TJpgDec uses size_t/int callbacks.
#if defined(CONFIG_JD_USE_ROM) && CONFIG_JD_USE_ROM
using ReadCount = unsigned int;
using WriteResult = unsigned int;
#else
using ReadCount = size_t;
using WriteResult = int;
#endif
struct ValidationInput {
  const uint8_t* data;
  size_t length;
  size_t offset = 0;
};
ReadCount validationRead(JDEC* decoder, uint8_t* buffer, ReadCount requested) {
  auto* input = static_cast<ValidationInput*>(decoder->device);
  const size_t remaining = input->length - input->offset;
  const size_t amount = size_t(requested) < remaining ? size_t(requested) : remaining;
  if (buffer && amount) memcpy(buffer, input->data + input->offset, amount);
  input->offset += amount;
  return static_cast<ReadCount>(amount);
}
WriteResult validationDiscard(JDEC*, void*, JRECT*) { return 1; }
}
bool validateSoftwareEntropy(const uint8_t* data, size_t length, const Info& info,
                             const char** error) {
  void* workspace = heap_caps_malloc(kWorkspaceBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!workspace) {
    if (error) *error = "out of memory for JPEG validation workspace";
    return false;
  }
  ValidationInput input{data, length};
  JDEC decoder = {};
  const JRESULT prepared = jd_prepare(&decoder, validationRead, workspace, kWorkspaceBytes, &input);
  const bool dimensionsMatch = prepared == JDR_OK && decoder.width == info.width && decoder.height == info.height;
  // Every coefficient is still Huffman-decoded. At 1/8 scale TJpgDec skips
  // the expensive IDCT and only produces disposable MCU-sized output.
#if JD_USE_SCALE
  constexpr uint8_t scale = 3;
#else
  constexpr uint8_t scale = 0;
#endif
  const JRESULT decoded = dimensionsMatch ? jd_decomp(&decoder, validationDiscard, scale) : JDR_FMT1;
  free(workspace);
  if (!dimensionsMatch || decoded != JDR_OK) {
    if (error) *error = "software JPEG entropy validation failed";
    return false;
  }
  return true;
}
bool decodeSoftware(const uint8_t* data, size_t length, const Info& info,
                    Image& image, const char** error, OutputAllocator allocator) {
  image.reset();
  uint8_t* pixels = nullptr;
  if (allocator) {
    pixels = allocator(info.bytes);
  } else {
    pixels = static_cast<uint8_t*>(heap_caps_malloc(info.bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pixels) pixels = static_cast<uint8_t*>(heap_caps_malloc(info.bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (!pixels) {
    if (error) *error = "out of memory decoding JPEG";
    return false;
  }
  void* workspace = heap_caps_malloc(kWorkspaceBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!workspace) {
    free(pixels);
    if (error) *error = "out of memory for JPEG workspace";
    return false;
  }
  esp_jpeg_image_cfg_t config = {};
  config.indata = const_cast<uint8_t*>(data);
  config.indata_size = static_cast<uint32_t>(length);
  config.outbuf = pixels;
  config.outbuf_size = static_cast<uint32_t>(info.bytes);
  config.out_format = JPEG_IMAGE_FORMAT_RGB888;
  config.out_scale = JPEG_IMAGE_SCALE_0;
  config.flags.swap_color_bytes = 0;
  config.advanced.working_buffer = workspace;
  config.advanced.working_buffer_size = kWorkspaceBytes;
  // Keep independent scratch per call and a checked output capacity. The old
  // camera wrapper shares static scratch and advertises UINT32_MAX capacity.
  esp_jpeg_image_output_t result = {};
  const esp_err_t status = esp_jpeg_decode(&config, &result);
  free(workspace);
  if (status != ESP_OK || result.width != info.width || result.height != info.height ||
      result.output_len != info.bytes) {
    free(pixels);
    if (error) *error = "software JPEG decode failed";
    return false;
  }
  image.pixels = pixels;
  image.width = info.width;
  image.height = info.height;
  image.stride = size_t(info.width) * 3;
  image.size = info.bytes;
  image.backend = Backend::Software;
  return true;
}
} }
