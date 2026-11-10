// Host harness for the interrupted-recording repair decision.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// INSERT_PRODUCTION

static int failures = 0;
#define CHECK(cond) \
  do { if (!(cond)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static std::vector<uint8_t> header(uint32_t dataSize, uint16_t channels = 1) {
  WavHeader h;
  h.numChannels = channels;
  h.sampleRate = 16000;
  h.bitsPerSample = 16;
  h.blockAlign = channels * 2;
  h.byteRate = h.sampleRate * h.blockAlign;
  h.dataSize = dataSize;
  h.fileSize = dataSize + sizeof(WavHeader) - 8;
  std::vector<uint8_t> bytes(sizeof(h));
  std::memcpy(bytes.data(), &h, sizeof(h));
  return bytes;
}

static MicWavRepairDecision decide(const std::vector<uint8_t>& head, size_t fileSize) {
  return micWavRepairDecide(head.data(), head.size(), fileSize);
}

int main() {
  CHECK(sizeof(WavHeader) == 44);
  const std::vector<uint8_t> placeholder = header(0);

  // Created, header never committed.
  CHECK(micWavRepairDecide(nullptr, 0, 0).action == MicWavRepairAction::REMOVE);
  // Placeholder header alone, or with less than one sample: no audio.
  CHECK(decide(placeholder, 44).action == MicWavRepairAction::REMOVE);
  CHECK(decide(placeholder, 45).action == MicWavRepairAction::REMOVE);

  // Placeholder plus checkpointed audio: the header is completed.
  MicWavRepairDecision d = decide(placeholder, 44 + 160000);
  CHECK(d.action == MicWavRepairAction::PATCH && d.dataSize == 160000);
  // A torn last sample is dropped.
  d = decide(placeholder, 44 + 160001);
  CHECK(d.action == MicWavRepairAction::PATCH && d.dataSize == 160000);
  // Stereo rounds to whole frames.
  d = decide(header(0, 2), 44 + 4098);
  CHECK(d.action == MicWavRepairAction::PATCH && d.dataSize == 4096);

  // A finished recording is never touched, whatever follows its header.
  CHECK(decide(header(32000), 44 + 32000).action == MicWavRepairAction::LEAVE);
  CHECK(decide(header(32000), 44 + 64000).action == MicWavRepairAction::LEAVE);

  // Foreign or damaged content is left alone.
  std::vector<uint8_t> foreign = placeholder;
  foreign[0] = 'X';
  CHECK(decide(foreign, 44 + 1000).action == MicWavRepairAction::LEAVE);
  std::vector<uint8_t> floatFormat = placeholder;
  floatFormat[20] = 3;  // audioFormat: IEEE float
  CHECK(decide(floatFormat, 44 + 1000).action == MicWavRepairAction::LEAVE);
  std::vector<uint8_t> oddRiff = placeholder;
  oddRiff[4] = 37;  // RIFF length that no placeholder carries
  CHECK(decide(oddRiff, 44 + 1000).action == MicWavRepairAction::LEAVE);
  std::vector<uint8_t> zeroAlign = placeholder;
  zeroAlign[32] = 0;  // blockAlign
  CHECK(decide(zeroAlign, 44 + 1000).action == MicWavRepairAction::LEAVE);
  // Shorter than a header, or a short read of a longer file.
  CHECK(micWavRepairDecide(placeholder.data(), 20, 20).action == MicWavRepairAction::LEAVE);
  CHECK(micWavRepairDecide(placeholder.data(), 20, 44 + 1000).action == MicWavRepairAction::LEAVE);

  if (failures) return 1;
  std::fprintf(stderr, "mic WAV repair decision: all checks passed\n");
  return 0;
}
