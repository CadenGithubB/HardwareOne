#pragma once
#include "HAL_JPEG.h"

namespace hwjpeg { namespace detail {
// Each backend owns its per-call resources. On failure image must be empty.
// HardwareOnly is a diagnostic option; Auto retains the software fallback.
bool decodeHardware(const uint8_t* data, size_t length, const Info& info,
                    Image& image, const char** error);
bool validateSoftwareEntropy(const uint8_t* data, size_t length, const Info& info,
                             const char** error);
bool decodeSoftware(const uint8_t* data, size_t length, const Info& info,
                    Image& image, const char** error, OutputAllocator allocator = nullptr);
} }
