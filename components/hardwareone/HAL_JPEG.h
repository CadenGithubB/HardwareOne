#pragma once

#include <stddef.h>
#include <stdint.h>

// Portable JPEG decoding. Pixels are top-to-bottom, tightly packed RGB888,
// matching the existing esp32-camera JPEG conversion path. Input remains caller-owned.
namespace hwjpeg {
enum class Backend { None, Software, Hardware };
enum class DecodeMode { Auto, SoftwareOnly, HardwareOnly };

struct Info {
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t precision = 0;
  uint8_t components = 0;
  uint8_t sofMarker = 0;
  uint8_t sampling[3] = {};
  size_t bytes = 0;
  uint32_t scanCount = 0;
  bool hardwareEligible = false;
  bool needsMarkerNormalization = false;
};

// Optional application allocator for software pixels and normalized input; storage must be
// releasable with free(). A failed custom allocation is not bypassed.
using OutputAllocator = uint8_t* (*)(size_t);

struct DecodeOptions {
  DecodeMode mode = DecodeMode::Auto;
  OutputAllocator softwareAllocator = nullptr;
  uint32_t maxWidth = 1600;
  uint32_t maxHeight = 1600;
  size_t maxInputBytes = UINT32_MAX;
  size_t maxOutputBytes = 1600u * 1600u * 3u;
};

class Image {
public:
  Image() = default;
  ~Image();
  Image(const Image&) = delete;
  Image& operator=(const Image&) = delete;
  void reset();
  uint8_t* pixels = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  size_t stride = 0;
  size_t size = 0;
  Backend backend = Backend::None;
};

// Bounded header validation; success does not promise a supported entropy codec.
bool inspect(const uint8_t* data, size_t length, Info& info,
             const DecodeOptions& options = {}, const char** error = nullptr);
// Validate the complete entropy stream with the compiled software decoder.
// Supports that decoder's baseline, single-scan formats; progressive/other
// unsupported formats return false, even if structurally valid. Always uses
// software (options.mode is ignored), bounded input/geometry checks and the
// same marker normalization as decode(). No full-image RGB buffer is allocated.
// info is populated by structural inspection; true additionally means all
// entropy blocks were decoded. It is not a pixel-level reference comparison.
bool validateSoftware(const uint8_t* data, size_t length, Info& info,
                      const DecodeOptions& options = {}, const char** error = nullptr);
// On failure image is empty. Auto tries eligible hardware then software.
bool decode(const uint8_t* data, size_t length, Image& image,
            const DecodeOptions& options = {}, const char** error = nullptr);
const char* backendName(Backend backend);

} // namespace hwjpeg
