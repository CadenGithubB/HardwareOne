#include "HAL_JPEG_Backend.h"
#include "jpeg_decoder.h"
#include "esp_heap_caps.h"
#include <stdlib.h>

namespace hwjpeg { namespace detail {
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
  esp_jpeg_image_cfg_t config = {};
  config.indata = const_cast<uint8_t*>(data);
  config.indata_size = static_cast<uint32_t>(length);
  config.outbuf = pixels;
  config.outbuf_size = static_cast<uint32_t>(info.bytes);
  config.out_format = JPEG_IMAGE_FORMAT_RGB888;
  config.out_scale = JPEG_IMAGE_SCALE_0;
  config.flags.swap_color_bytes = 0;
  // Leave working_buffer null: esp_jpeg allocates a private workspace per call.
  // The old fmt2rgb888 wrapper shares static scratch and promises UINT32_MAX
  // output capacity. Neither contract is safe for concurrent image consumers.
  esp_jpeg_image_output_t result = {};
  const esp_err_t status = esp_jpeg_decode(&config, &result);
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
