#include "HAL_JPEG.h"
#include "HAL_JPEG_Backend.h"
#include <stdlib.h>
#include <limits.h>
#include <string.h>

namespace hwjpeg {
namespace {
bool fail(const char** error, const char* message) {
  if (error) *error = message;
  return false;
}
uint16_t be16(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
bool isSof(uint8_t m) {
  return m >= 0xc0 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc;
}
}

Image::~Image() { reset(); }
void Image::reset() {
  free(pixels);
  pixels = nullptr;
  width = height = 0;
  stride = size = 0;
  backend = Backend::None;
}
const char* backendName(Backend backend) {
  switch (backend) {
    case Backend::Hardware: return "hardware";
    case Backend::Software: return "software";
    default: return "none";
  }
}

bool inspect(const uint8_t* data, size_t length, Info& info,
             const DecodeOptions& options, const char** error) {
  info = {};
  if (error) *error = nullptr;
  if (!data || length < 4 || length > UINT32_MAX || length > options.maxInputBytes)
    return fail(error, "invalid JPEG input size");
  if (data[0] != 0xff || data[1] != 0xd8) return fail(error, "missing JPEG SOI");
  Info parsed;
  uint8_t componentIds[3] = {};
  bool seenFrame = false, entropy = false, hardwareSafe = true;
  uint8_t quantTables = 0, dcTables = 0, acTables = 0, neededQuant = 0;
  size_t pos = 2;
  while (pos < length) {
    // Skip entropy bytes, stuffed FF00 and restart markers without interpreting
    // them as segment lengths. Every header segment is checked before access.
    if (entropy) {
      while (pos < length && data[pos] != 0xff) ++pos;
      if (pos == length) break;
    } else if (data[pos] != 0xff) {
      return fail(error, "invalid JPEG marker");
    }
    const bool wasEntropy = entropy;
    const size_t fillStart = pos;
    while (pos < length && data[pos] == 0xff) ++pos;
    if (pos == length) break;
    const uint8_t marker = data[pos++];
    const size_t fillBytes = pos - fillStart - 1;
    if (wasEntropy && marker == 0 && fillBytes != 1)
      return fail(error, "invalid JPEG entropy stuffing");
    // Older ROM and bundled TJpgDec variants disagree about marker fill runs.
    // Canonicalize valid fill before either vendor decoder sees the input.
    if (fillBytes > 1) parsed.needsMarkerNormalization = true;
    if (entropy && (marker == 0 || (marker >= 0xd0 && marker <= 0xd7))) continue;
    entropy = false;
    if (marker == 0xd9) {
      if (!seenFrame || !parsed.scanCount) return fail(error, "JPEG has no image scan");
      parsed.hardwareEligible = hardwareSafe && parsed.sofMarker == 0xc0 &&
          parsed.components == 3 && parsed.scanCount == 1 &&
          (quantTables == 1 || quantTables == 3);
      info = parsed;
      return true;
    }
    if (marker == 0 || marker == 0xd8 || marker == 0x01 ||
        (marker >= 0xd0 && marker <= 0xd7)) return fail(error, "unexpected JPEG marker");
    if (length - pos < 2) return fail(error, "truncated JPEG segment");
    const size_t segmentLength = be16(data + pos);
    if (segmentLength < 2 || segmentLength > length - pos)
      return fail(error, "invalid JPEG segment length");
    const uint8_t* payload = data + pos + 2;
    const size_t count = segmentLength - 2;

    if (isSof(marker)) {
      if (seenFrame || parsed.scanCount) return fail(error, "duplicate JPEG frame");
      if (count < 6) return fail(error, "truncated JPEG frame");
      parsed.precision = payload[0];
      parsed.height = be16(payload + 1);
      parsed.width = be16(payload + 3);
      parsed.components = payload[5];
      parsed.sofMarker = marker;
      if (parsed.precision != 8 || (parsed.components != 1 && parsed.components != 3))
        return fail(error, "unsupported JPEG pixel format");
      if (count != 6u + 3u * parsed.components) return fail(error, "invalid JPEG frame length");
      if (!parsed.width || !parsed.height || parsed.width > options.maxWidth ||
          parsed.height > options.maxHeight) return fail(error, "JPEG dimensions exceed limit");
      const uint64_t bytes = uint64_t(parsed.width) * parsed.height * 3u;
      if (bytes > SIZE_MAX || bytes > UINT32_MAX || bytes > options.maxOutputBytes)
        return fail(error, "JPEG output exceeds limit");
      parsed.bytes = size_t(bytes);
      for (uint8_t i = 0; i < parsed.components; ++i) {
        const uint8_t* component = payload + 6 + 3 * i;
        for (uint8_t j = 0; j < i; ++j)
          if (componentIds[j] == component[0]) return fail(error, "duplicate JPEG component");
        componentIds[i] = component[0];
        parsed.sampling[i] = component[1];
        const unsigned h = component[1] >> 4, v = component[1] & 15;
        if (!h || h > 4 || !v || v > 4 || component[2] > 3)
          return fail(error, "invalid JPEG component");
        neededQuant |= uint8_t(1u << component[2]);
        // Restrict hardware to ordinary Y/Cb/Cr IDs and table selectors.
        if (component[0] != i + 1 || component[2] > 1) hardwareSafe = false;
      }
      seenFrame = true;
    } else if (marker == 0xdb) { // DQT: one or more complete quantization tables.
      size_t off = 0;
      while (off < count) {
        const uint8_t spec = payload[off++];
        const unsigned precision = spec >> 4, id = spec & 15;
        if (precision > 1 || id > 3) return fail(error, "invalid JPEG quantization table");
        const size_t bytes = precision ? 128 : 64;
        if (bytes > count - off) return fail(error, "truncated JPEG quantization table");
        if (quantTables & (1u << id)) hardwareSafe = false;
        quantTables |= uint8_t(1u << id);
        if (precision || id > 1) hardwareSafe = false;
        for (size_t q = 0; q < bytes; ++q)
          if (!payload[off + q]) hardwareSafe = false;
        off += bytes;
      }
      if (!count) return fail(error, "empty JPEG quantization table");
    } else if (marker == 0xc4) { // DHT: selector, 16 code counts, symbols.
      size_t off = 0;
      while (off < count) {
        if (count - off < 17) return fail(error, "truncated JPEG Huffman table");
        const uint8_t spec = payload[off++];
        if ((spec >> 4) > 1 || (spec & 15) > 3) return fail(error, "invalid JPEG Huffman selector");
        unsigned symbols = 0;
        int slots = 1;
        for (unsigned j = 0; j < 16; ++j) {
          const unsigned n = payload[off++];
          symbols += n;
          slots = slots * 2 - int(n);
          if (slots < 0) return fail(error, "invalid JPEG Huffman code lengths");
        }
        if (!symbols || symbols > 256 || symbols > count - off)
          return fail(error, "invalid JPEG Huffman table length");
        if (spec & 0x10) {
          if ((acTables & (1u << (spec & 15))) || symbols > 162) hardwareSafe = false;
          acTables |= uint8_t(1u << (spec & 15));
        } else {
          if ((dcTables & (1u << (spec & 15))) || symbols > 12) hardwareSafe = false;
          dcTables |= uint8_t(1u << (spec & 15));
        }
        if (!slots) hardwareSafe = false;
        if ((spec & 15) > 1) hardwareSafe = false;
        for (unsigned j = 0; j < symbols; ++j) {
          const uint8_t symbol = payload[off + j];
          if ((spec & 0x10) ? ((symbol & 15) > 10 ||
                  (!(symbol & 15) && symbol != 0 && symbol != 0xf0)) : symbol > 11)
            hardwareSafe = false;
        }
        off += symbols;
      }
      if (!count) return fail(error, "empty JPEG Huffman table");
    } else if (marker == 0xda) {
      if (!seenFrame || count < 4) return fail(error, "JPEG scan precedes frame");
      const unsigned n = payload[0];
      if (!n || n > parsed.components || count != 4 + n * 2)
        return fail(error, "invalid JPEG scan header");
      uint8_t used = 0;
      for (unsigned i = 0; i < n; ++i) {
        unsigned index = 0;
        while (index < parsed.components && componentIds[index] != payload[1 + 2 * i]) ++index;
        if (index == parsed.components || (used & (1u << index)))
          return fail(error, "invalid JPEG scan component");
        used |= uint8_t(1u << index);
        const uint8_t selector = payload[2 + 2 * i];
        const unsigned dc = selector >> 4, ac = selector & 15;
        if (dc > 3 || ac > 3) return fail(error, "invalid JPEG scan table selector");
        if (index != i || dc > 1 || ac > 1 || !(dcTables & (1u << dc)) ||
            !(acTables & (1u << ac))) hardwareSafe = false;
      }
      if (n != parsed.components || payload[count - 3] != 0 ||
          payload[count - 2] != 63 || payload[count - 1] != 0 ||
          (quantTables & neededQuant) != neededQuant) hardwareSafe = false;
      ++parsed.scanCount;
      entropy = true;
    } else if (marker == 0xdd) {
      if (count != 2) return fail(error, "invalid JPEG restart interval");
    } else if (!((marker >= 0xe0 && marker <= 0xef) || marker == 0xfe)) {
      hardwareSafe = false;
    }
    pos += segmentLength;
  }
  return fail(error, "truncated JPEG (missing EOI)");
}

namespace {
// Input has passed inspect(). Repeat bounds checks to keep this copying boundary
// self-contained. Segment payload and entropy bytes remain bit-for-bit unchanged.
bool normalizeMarkers(const uint8_t* input, size_t length, uint8_t* output, size_t& used) {
  if (length < 2) return false;
  output[0] = 0xff; output[1] = 0xd8;
  size_t pos = 2;
  used = 2;
  bool entropy = false;
  while (pos < length) {
    if (entropy) {
      const size_t start = pos;
      while (pos < length && input[pos] != 0xff) ++pos;
      memcpy(output + used, input + start, pos - start);
      used += pos - start;
    }
    if (pos >= length || input[pos] != 0xff) return false;
    while (pos < length && input[pos] == 0xff) ++pos;
    if (pos == length) return false;
    const uint8_t marker = input[pos++];
    output[used++] = 0xff;
    output[used++] = marker;
    if (entropy && (marker == 0 || (marker >= 0xd0 && marker <= 0xd7))) continue;
    entropy = false;
    if (marker == 0xd9) {
      // Preserve permitted trailing data; it is not part of the JPEG decode.
      memcpy(output + used, input + pos, length - pos);
      used += length - pos;
      return true;
    }
    if (length - pos < 2) return false;
    const size_t segment = be16(input + pos);
    if (segment < 2 || segment > length - pos) return false;
    memcpy(output + used, input + pos, segment);
    pos += segment;
    used += segment;
    entropy = marker == 0xda;
  }
  return false;
}
struct InputCopy {
  uint8_t* bytes = nullptr;
  ~InputCopy() { free(bytes); }
};
}

bool decode(const uint8_t* data, size_t length, Image& image,
            const DecodeOptions& options, const char** error) {
  image.reset();
  Info info;
  if (!inspect(data, length, info, options, error)) return false;
  InputCopy normalized;
  if (info.needsMarkerNormalization) {
    normalized.bytes = options.softwareAllocator ? options.softwareAllocator(length) :
        static_cast<uint8_t*>(malloc(length));
    if (!normalized.bytes) return fail(error, "out of memory normalizing JPEG");
    size_t normalizedLength = 0;
    if (!normalizeMarkers(data, length, normalized.bytes, normalizedLength))
      return fail(error, "JPEG normalization failed");
    data = normalized.bytes;
    length = normalizedLength;
  }
  if (options.mode != DecodeMode::SoftwareOnly) {
    if (detail::decodeHardware(data, length, info, image, error)) {
      if (error) *error = nullptr;
      return true;
    }
    image.reset();
    if (options.mode == DecodeMode::HardwareOnly) return false;
  }
  if (detail::decodeSoftware(data, length, info, image, error, options.softwareAllocator)) {
    if (error) *error = nullptr;
    return true;
  }
  image.reset();
  return false;
}
} // namespace hwjpeg
