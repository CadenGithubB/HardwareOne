#include "HAL_JPEG_Backend.h"
#include "soc/soc_caps.h"

#if defined(SOC_JPEG_DECODE_SUPPORTED) && SOC_JPEG_DECODE_SUPPORTED && \
    defined(HW1_JPEG_DRIVER_QUALIFIED) && HW1_JPEG_DRIVER_QUALIFIED
#include <atomic>
#include <cstdlib>
#include <cstring>
#include "driver/jpeg_decode.h"
#endif

namespace hwjpeg { namespace detail {
namespace {
bool failed(const char** error, const char* message) {
  if (error) *error = message;
  return false;
}

#if defined(SOC_JPEG_DECODE_SUPPORTED) && SOC_JPEG_DECODE_SUPPORTED && \
    defined(HW1_JPEG_DRIVER_QUALIFIED) && HW1_JPEG_DRIVER_QUALIFIED
// One owner makes admission nonblocking, including engine creation/deletion.
// Auto mode can use software while another caller owns the accelerator.
std::atomic_flag hardwareBusy = ATOMIC_FLAG_INIT;
struct HardwareClaim {
  ~HardwareClaim() { hardwareBusy.clear(std::memory_order_release); }
};

struct DecoderResources {
  jpeg_decoder_handle_t engine = nullptr;
  uint8_t* input = nullptr;
  uint8_t* output = nullptr;
  ~DecoderResources() {
    if (engine) jpeg_del_decoder_engine(engine);
    std::free(input);
    std::free(output);
  }
};

// IDF 5.5.5 uses 14-bit dimensions in the 2D-DMA descriptors. Check the
// MCU-rounded dimensions as well as the visible image before allocation.
constexpr uint32_t kDmaDimensionLimit = (1u << 14) - 1u;
constexpr int kDecodeTimeoutMs = 1000;
#endif
} // namespace

bool decodeHardware(const uint8_t* data, size_t length, const Info& info,
                    Image& image, const char** error) {
  image.reset();
#if defined(SOC_JPEG_DECODE_SUPPORTED) && SOC_JPEG_DECODE_SUPPORTED && \
    defined(HW1_JPEG_DRIVER_QUALIFIED) && HW1_JPEG_DRIVER_QUALIFIED
  // inspect() establishes the bounded, single-scan baseline/header contract.
  // Grayscale, unusual components/tables and odd dimensions retain the shared
  // software path. The hardware cannot convert gray input directly to RGB.
  if (!data || length == 0 || length > UINT32_MAX || !info.hardwareEligible ||
      info.sofMarker != 0xc0 || info.precision != 8 || info.components != 3 ||
      info.width == 0 || info.height == 0 ||
      info.width > kDmaDimensionLimit || info.height > kDmaDimensionLimit ||
      (info.width % 8) != 0 || (info.height % 8) != 0 ||
      info.sampling[1] != 0x11 || info.sampling[2] != 0x11) {
    return failed(error, "JPEG is outside the hardware decoder subset");
  }
  uint32_t mcuWidth = 8, mcuHeight = 8;
  switch (info.sampling[0]) {
    case 0x11: break;                       // 4:4:4
    case 0x21: mcuWidth = 16; break;         // 4:2:2
    case 0x22: mcuWidth = mcuHeight = 16; break; // 4:2:0
    default: return failed(error, "JPEG sampling is not hardware supported");
  }
  const uint32_t paddedWidth = (info.width + mcuWidth - 1) / mcuWidth * mcuWidth;
  const uint32_t paddedHeight = (info.height + mcuHeight - 1) / mcuHeight * mcuHeight;
  if (paddedWidth > kDmaDimensionLimit || paddedHeight > kDmaDimensionLimit) {
    return failed(error, "JPEG padded dimensions exceed the hardware limit");
  }
  const uint64_t paddedBytes64 = uint64_t(paddedWidth) * paddedHeight * 3u;
  const uint64_t visibleBytes64 = uint64_t(info.width) * info.height * 3u;
  if (paddedBytes64 > UINT32_MAX || paddedBytes64 > SIZE_MAX ||
      visibleBytes64 > SIZE_MAX) {
    return failed(error, "JPEG hardware output size overflow");
  }
  if (hardwareBusy.test_and_set(std::memory_order_acquire)) {
    return failed(error, "JPEG hardware decoder is busy");
  }
  HardwareClaim claim;
  DecoderResources resources;

  // Copy caller-owned input into SDK-compatible storage. This also accepts
  // unaligned input and flash-backed input without exposing DMA requirements.
  size_t inputCapacity = 0, outputCapacity = 0;
  jpeg_decode_memory_alloc_cfg_t inputConfig = {};
  inputConfig.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
  resources.input = static_cast<uint8_t*>(
      jpeg_alloc_decoder_mem(length, &inputConfig, &inputCapacity));
  if (!resources.input || inputCapacity < length) {
    return failed(error, "JPEG hardware input allocation failed");
  }
  std::memset(resources.input, 0, inputCapacity);
  std::memcpy(resources.input, data, length);

  jpeg_decode_memory_alloc_cfg_t outputConfig = {};
  outputConfig.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
  resources.output = static_cast<uint8_t*>(jpeg_alloc_decoder_mem(
      static_cast<size_t>(paddedBytes64), &outputConfig, &outputCapacity));
  if (!resources.output || outputCapacity < paddedBytes64 || outputCapacity > UINT32_MAX) {
    return failed(error, "JPEG hardware output allocation failed");
  }

  jpeg_decode_engine_cfg_t engineConfig = {};
  engineConfig.intr_priority = 0;
  engineConfig.timeout_ms = kDecodeTimeoutMs;
  if (jpeg_new_decoder_engine(&engineConfig, &resources.engine) != ESP_OK) {
    return failed(error, "JPEG hardware engine initialization failed");
  }
  // A fresh engine avoids IDF 5.5.5's retained no_color_conversion state
  // between direct-format and converted operations. This backend uses RGB
  // conversion exclusively; no raw driver handle escapes the codec boundary.
  jpeg_decode_cfg_t decodeConfig = {};
  decodeConfig.output_format = JPEG_DECODE_OUT_FORMAT_RGB888;
  decodeConfig.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB;
  decodeConfig.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
  uint32_t decodedBytes = 0;

  // Current application has no other 2D-DMA consumer. Before adding PPA or
  // another independent user, qualify/fix IDF 5.5.5 queued-job cancellation:
  // process() calls dma2d_force_end() on timeout, but an unstarted queued job
  // has no rx_chan and is not cancelled there. Our try-lock excludes codec
  // contention, not unrelated 2D-DMA users. Do not assume timeout cleanup
  // remains safe after introducing such a competing subsystem.
  if (jpeg_decoder_process(resources.engine, &decodeConfig, resources.input,
                           static_cast<uint32_t>(length), resources.output,
                           static_cast<uint32_t>(outputCapacity), &decodedBytes) != ESP_OK) {
    return failed(error, "JPEG hardware decode failed");
  }
  if (decodedBytes != paddedBytes64 || decodedBytes > outputCapacity) {
    return failed(error, "JPEG hardware returned an unexpected output size");
  }

  const esp_err_t deleted = jpeg_del_decoder_engine(resources.engine);
  resources.engine = nullptr; // Deletion may have released sub-resources even on error.
  if (deleted != ESP_OK) {
    return failed(error, "JPEG hardware engine cleanup failed");
  }

  const size_t tightStride = static_cast<size_t>(info.width) * 3u;
  const size_t paddedStride = static_cast<size_t>(paddedWidth) * 3u;
  if (tightStride != paddedStride) {
    for (uint32_t row = 1; row < info.height; ++row) {
      std::memmove(resources.output + static_cast<size_t>(row) * tightStride,
                   resources.output + static_cast<size_t>(row) * paddedStride,
                   tightStride);
    }
  }
  image.pixels = resources.output;
  resources.output = nullptr;
  image.width = info.width;
  image.height = info.height;
  image.stride = tightStride;
  image.size = static_cast<size_t>(visibleBytes64);
  image.backend = Backend::Hardware;
  if (error) *error = nullptr;
  return true;
#else
  (void)data; (void)length; (void)info;
  return failed(error, "JPEG hardware decoder is unavailable");
#endif
}

} } // namespace hwjpeg::detail
