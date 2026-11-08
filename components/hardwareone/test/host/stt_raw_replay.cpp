// Replay an `sttraw` recording (raw_<id>.pcm, s16le mono 16 kHz) through the
// production speech segmenter (stt/stt_segmenter.h) with the production
// continuous settings (stt/stt_continuous_config.h), optionally overriding
// the detector floors, and print the resulting phrases as JSON.
//
// With no overrides the segments must equal the device's `TL boundary` log
// lines for that session (start/end are segmenter session samples, which are
// file samples when the sidecar reports no gaps).
//
//   stt_raw_replay raw.pcm [--sidecar raw.json] [--speech-floor N]
//       [--silence-floor N] [--noise-percentile N] [--blip-ms N]
//       [--no-short-close] [--single] [--chunk N]
//       [--trace frames.csv] [--cut-dir DIR]
//
// --trace writes one row per 10 ms frame: sample,rms,noise,threshold,open.
// --cut-dir writes each phrase's PCM as seg_<n>.pcm for host decoding.
#include "stt/stt_continuous_config.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace hw1::stt;

namespace {

constexpr size_t kRate = 16000;
constexpr size_t kPhraseSamples = 10 * kRate;  // System_STT kStreamSamples

[[noreturn]] void usage(const char* why) {
  std::fprintf(stderr, "stt_raw_replay: %s\n", why);
  std::fprintf(stderr,
               "usage: stt_raw_replay raw.pcm [--sidecar raw.json] [--speech-floor N] [--silence-floor N]\n"
               "       [--noise-percentile N] [--blip-ms N] [--decision-pre N] [--no-short-close] [--single] [--chunk N]\n"
               "       [--trace frames.csv] [--cut-dir DIR]\n");
  std::exit(2);
}

long number(const char* text, long lo, long hi, const char* name) {
  char* end = nullptr;
  errno = 0;
  const long v = std::strtol(text, &end, 10);
  if (errno || !end || *end || v < lo || v > hi) usage((std::string("bad ") + name).c_str());
  return v;
}

// The sidecar is small, flat JSON written by System_STTRawCapture; only
// samples_dropped and rate matter here.
long sidecarField(const std::string& text, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const size_t at = text.find(needle);
  if (at == std::string::npos) return -1;
  return std::strtol(text.c_str() + at + needle.size(), nullptr, 10);
}

const char* endName(SegmentEnd end) {
  switch (end) {
    case SegmentEnd::Pause: return "pause";
    case SegmentEnd::HardLimit: return "hard_limit";
    case SegmentEnd::SessionEnd: return "session_end";
  }
  return "?";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) usage("missing raw.pcm");
  const char* pcmPath = argv[1];
  const char* sidecarPath = nullptr;
  const char* tracePath = nullptr;
  const char* cutDir = nullptr;
  long speechFloor = -1, silenceFloor = -1, percentile = -1, blipMs = -1, decisionPre = -1;
  bool shortClose = true, single = false;
  size_t chunk = 1024;  // the capture task's read size
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&]() -> const char* { if (i + 1 >= argc) usage("missing value"); return argv[++i]; };
    if (a == "--sidecar") sidecarPath = value();
    else if (a == "--speech-floor") speechFloor = number(value(), 1, 32767, "--speech-floor");
    else if (a == "--silence-floor") silenceFloor = number(value(), 1, 32767, "--silence-floor");
    else if (a == "--noise-percentile") percentile = number(value(), 0, 50, "--noise-percentile");
    else if (a == "--blip-ms") blipMs = number(value(), 0, 1000, "--blip-ms");
    else if (a == "--decision-pre") decisionPre = number(value(), 0, 99, "--decision-pre");
    else if (a == "--no-short-close") shortClose = false;
    else if (a == "--single") single = true;
    else if (a == "--chunk") chunk = size_t(number(value(), 1, 1 << 20, "--chunk"));
    else if (a == "--trace") tracePath = value();
    else if (a == "--cut-dir") cutDir = value();
    else usage(("unknown option " + a).c_str());
  }

  long dropped = 0;
  if (sidecarPath) {
    std::ifstream in(sidecarPath);
    if (!in) usage("cannot read the sidecar");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    dropped = sidecarField(text, "samples_dropped");
    const long rate = sidecarField(text, "rate");
    if (rate != long(kRate)) usage("sidecar rate is not 16000");
    if (dropped < 0) usage("sidecar has no samples_dropped");
  }

  std::ifstream in(pcmPath, std::ios::binary);
  if (!in) usage("cannot read raw.pcm");
  std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (bytes.size() % 2) usage("raw.pcm has an odd byte count");
  std::vector<int16_t> pcm(bytes.size() / 2);
  for (size_t i = 0; i < pcm.size(); ++i)  // explicit little-endian decode
    pcm[i] = int16_t(uint16_t(uint8_t(bytes[2 * i])) | uint16_t(uint8_t(bytes[2 * i + 1])) << 8);

  SegmenterConfig config = continuousSegmenterConfig(kRate, kPhraseSamples, shortClose, single);
  if (speechFloor > 0) config.adaptive_speech_floor = uint16_t(speechFloor);
  if (silenceFloor > 0) config.adaptive_silence_floor = uint16_t(silenceFloor);
  if (percentile >= 0) config.noise_percentile = uint8_t(percentile);
  if (blipMs >= 0) config.silence_blip_samples = size_t(blipMs) * kRate / 1000;
  if (decisionPre >= 0) config.decision_pre_emphasis = uint8_t(decisionPre);
  std::vector<int16_t> work(config.max_segment_samples);
  Segmenter seg;
  if (!seg.reset(work.data(), work.size(), config)) usage("invalid segmenter settings (silence floor must be <= speech floor)");

  std::FILE* trace = nullptr;
  if (tracePath) {
    trace = std::fopen(tracePath, "w");
    if (!trace) usage("cannot write the trace");
    std::fputs("sample,rms,noise,threshold,open\n", trace);
    chunk = kSegmentAnalysisSamples;  // one row per analysis frame
  }

  std::string out;
  char line[512];
  size_t count = 0;
  uint64_t covered = 0;
  auto emit = [&]() {
    const SpeechSegment* s = seg.segment();
    ++count;
    covered += s->samples;
    std::snprintf(line, sizeof(line),
                  "%s{\"n\":%zu,\"start\":%llu,\"end\":%llu,\"end_kind\":\"%s\",\"continuation\":%s,"
                  "\"voiced_end\":%llu,\"new_voiced\":%zu}",
                  count > 1 ? ",\n  " : "\n  ", count, (unsigned long long)s->start_sample,
                  (unsigned long long)s->end_sample, endName(s->end), s->continuation ? "true" : "false",
                  (unsigned long long)(s->start_sample + s->voiced_end_samples), s->new_voiced_samples);
    out += line;
    if (cutDir) {
      const std::string path = std::string(cutDir) + "/seg_" + std::to_string(count) + ".pcm";
      std::FILE* f = std::fopen(path.c_str(), "wb");
      if (!f) usage("cannot write into --cut-dir");
      for (size_t i = 0; i < s->samples; ++i) {
        const uint16_t v = uint16_t(s->pcm[i]);
        std::fputc(v & 0xff, f);
        std::fputc(v >> 8, f);
      }
      std::fclose(f);
    }
    seg.release();
  };

  size_t at = 0;
  while (at < pcm.size()) {
    size_t left = std::min(chunk, pcm.size() - at);
    while (left) {
      const SegmentPushResult r = seg.push(pcm.data() + at, left);
      at += r.consumed;
      left -= r.consumed;
      if (r.status == SegmentStatus::Ready) emit();
      else if (r.status != SegmentStatus::Ok || !r.consumed) usage("segmenter rejected the input");
    }
    if (trace) {
      const int16_t* openPcm = nullptr;
      size_t openSamples = 0;
      std::fprintf(trace, "%zu,%u,%u,%u,%d\n", at, seg.current_rms(), seg.noise_rms(), seg.threshold_rms(),
                   seg.in_progress(&openPcm, &openSamples) ? 1 : 0);
    }
  }
  if (seg.finish() == SegmentStatus::Ready) emit();
  if (trace) std::fclose(trace);

  std::printf("{\"file\":\"%s\",\"samples\":%zu,\"seconds\":%.2f,\"sidecar_dropped\":%ld,"
              "\"config\":{\"speech_floor\":%u,\"silence_floor\":%u,\"noise_percentile\":%u,"
              "\"blip_samples\":%zu,\"min_segment\":%zu,\"short_close\":%zu,\"end_silence\":%zu,"
              "\"pre_roll\":%zu,\"min_voice\":%zu,\"max_segment\":%zu},"
              "\"segments_covered_samples\":%llu,\"segments\":[%s\n]}\n",
              pcmPath, pcm.size(), double(pcm.size()) / kRate, dropped, config.adaptive_speech_floor,
              config.adaptive_silence_floor, unsigned(config.noise_percentile), config.silence_blip_samples,
              config.min_segment_samples, config.short_close_silence_samples, config.end_silence_samples,
              config.pre_roll_samples, config.min_voice_samples, config.max_segment_samples,
              (unsigned long long)covered, out.c_str());
  if (dropped > 0)
    std::fprintf(stderr, "warning: the device dropped %ld samples; offsets after the first gap do not match the "
                         "device's session clock\n", dropped);
  return 0;
}
