#pragma once
// Single-producer/single-consumer PCM ring for the STT raw-input recorder
// (System_STTRawCapture). The real-time capture task pushes; one drain task
// reads contiguous spans and writes them to storage. Neither side blocks or
// allocates. A push that does not fit is dropped whole and recorded as a gap
// in session-sample coordinates, so the saved file plus its gap list still
// describes exactly what the segmenter saw.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hw1::stt {

struct RawRingGap { uint64_t at; uint32_t samples; };

class RawRing {
public:
  static constexpr size_t kMaxGaps = 16;

  // memory must hold capacity samples and outlive the ring's use. capacity
  // must be a power of two so the free-running counters stay correct across
  // size_t wrap-around; any other value leaves the ring unusable (every push
  // is dropped).
  void reset(int16_t* memory, size_t capacity) {
    const bool valid = memory && capacity && !(capacity & (capacity - 1));
    buffer_ = valid ? memory : nullptr; capacity_ = valid ? capacity : 0;
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    seen_ = 0; dropped_ = 0; gapCount_ = 0; gapsLost_ = 0;
  }

  // Producer. Returns false when the block was dropped (ring full).
  bool push(const int16_t* pcm, size_t samples) {
    const uint64_t at = seen_;
    seen_ += samples;
    if (!samples) return true;
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    if (!buffer_ || samples > capacity_ - (head - tail)) {
      dropped_ += samples;
      if (gapCount_ && gaps_[gapCount_ - 1].at + gaps_[gapCount_ - 1].samples == at)
        gaps_[gapCount_ - 1].samples += static_cast<uint32_t>(samples);
      else if (gapCount_ < kMaxGaps) gaps_[gapCount_++] = {at, static_cast<uint32_t>(samples)};
      else ++gapsLost_;
      return false;
    }
    const size_t start = head % capacity_;
    const size_t first = samples < capacity_ - start ? samples : capacity_ - start;
    memcpy(buffer_ + start, pcm, first * sizeof(int16_t));
    if (first < samples) memcpy(buffer_, pcm + first, (samples - first) * sizeof(int16_t));
    head_.store(head + samples, std::memory_order_release);
    return true;
  }

  // Consumer: the oldest unread contiguous span (may be shorter than the
  // total available when it wraps). Returns its length; 0 when empty.
  size_t peek(const int16_t** span, size_t maxSamples) const {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_acquire);
    size_t n = head - tail;
    if (!n) return 0;
    const size_t start = tail % capacity_;
    if (n > capacity_ - start) n = capacity_ - start;
    if (n > maxSamples) n = maxSamples;
    *span = buffer_ + start;
    return n;
  }
  void consume(size_t samples) { tail_.fetch_add(samples, std::memory_order_release); }
  size_t pending() const {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_relaxed);
  }

  // Producer-side counters; read them only after the producer has stopped.
  uint64_t seen() const { return seen_; }
  uint64_t dropped() const { return dropped_; }
  size_t gapCount() const { return gapCount_; }
  const RawRingGap& gap(size_t i) const { return gaps_[i]; }
  uint32_t gapsLost() const { return gapsLost_; }

private:
  int16_t* buffer_ = nullptr;
  size_t capacity_ = 0;
  std::atomic<size_t> head_{0}, tail_{0};  // monotonically increasing sample counts
  uint64_t seen_ = 0, dropped_ = 0;
  RawRingGap gaps_[kMaxGaps] = {};
  size_t gapCount_ = 0;
  uint32_t gapsLost_ = 0;
};

}  // namespace hw1::stt
