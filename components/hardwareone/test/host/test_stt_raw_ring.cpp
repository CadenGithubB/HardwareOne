// Production stt/stt_raw_ring.h: the SPSC ring behind the STT raw-input
// recorder (System_STTRawCapture). Checks wrap-around, whole-block drops,
// gap coalescing in session-sample coordinates, the gap cap, an invalid
// capacity, a long wrap-heavy run, and a real two-thread producer/consumer run.
#include "stt/stt_raw_ring.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

using hw1::stt::RawRing;

static std::vector<int16_t> drainAll(RawRing& ring) {
  std::vector<int16_t> out;
  const int16_t* span = nullptr;
  while (size_t n = ring.peek(&span, 5)) {  // small spans exercise partial reads
    out.insert(out.end(), span, span + n);
    ring.consume(n);
  }
  return out;
}

static std::vector<int16_t> ramp(int16_t start, size_t n) {
  std::vector<int16_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = static_cast<int16_t>(start + int16_t(i));
  return v;
}

static void testWrapAndOrder() {
  std::vector<int16_t> mem(16);
  RawRing ring;
  ring.reset(mem.data(), mem.size());
  std::vector<int16_t> expect;
  int16_t next = 0;
  for (int round = 0; round < 20; ++round) {
    auto block = ramp(next, 7);
    next = static_cast<int16_t>(next + 7);
    assert(ring.push(block.data(), block.size()));
    expect.insert(expect.end(), block.begin(), block.end());
    auto got = drainAll(ring);
    assert(got == std::vector<int16_t>(expect.end() - 7, expect.end()));
  }
  assert(ring.seen() == 140 && ring.dropped() == 0 && ring.gapCount() == 0);
}

static void testDropsAndGaps() {
  std::vector<int16_t> mem(16);
  RawRing ring;
  ring.reset(mem.data(), mem.size());
  auto a = ramp(0, 10), b = ramp(100, 10), c = ramp(200, 3), d = ramp(300, 6);
  assert(ring.push(a.data(), a.size()));    // samples 0..9 kept (10/16 used)
  assert(!ring.push(b.data(), b.size()));   // 10..19 dropped whole
  assert(!ring.push(b.data(), 7));          // 20..26 dropped, coalesces with the gap
  assert(ring.push(c.data(), c.size()));    // 27..29 kept (13/16)
  assert(!ring.push(d.data(), d.size()));   // 30..35 dropped: a new gap
  assert(ring.seen() == 36 && ring.dropped() == 23);
  assert(ring.gapCount() == 2);
  assert(ring.gap(0).at == 10 && ring.gap(0).samples == 17);
  assert(ring.gap(1).at == 30 && ring.gap(1).samples == 6);
  auto got = drainAll(ring);
  std::vector<int16_t> expect(a);
  expect.insert(expect.end(), c.begin(), c.end());
  assert(got == expect);
  // Zero-length pushes are accepted and do not move the session clock.
  assert(ring.push(nullptr, 0) && ring.seen() == 36);
}

static void testGapCap() {
  std::vector<int16_t> mem(4);
  RawRing ring;
  ring.reset(mem.data(), mem.size());
  auto fill = ramp(0, 4), one = ramp(9, 1);
  assert(ring.push(fill.data(), fill.size()));
  for (size_t i = 0; i < RawRing::kMaxGaps + 3; ++i) {
    assert(!ring.push(one.data(), 1));           // dropped
    (void)drainAll(ring);                        // free space, then refill so the next drop is separate
    assert(ring.push(fill.data(), fill.size()));
  }
  assert(ring.gapCount() == RawRing::kMaxGaps && ring.gapsLost() == 3);
}

static void testInvalidCapacity() {
  std::vector<int16_t> mem(12);
  RawRing ring;
  ring.reset(mem.data(), mem.size());  // not a power of two
  auto a = ramp(0, 2);
  assert(!ring.push(a.data(), a.size()));
  const int16_t* span = nullptr;
  assert(ring.peek(&span, 8) == 0 && ring.dropped() == 2);
  ring.reset(nullptr, 16);
  assert(!ring.push(a.data(), a.size()));
}

static void testThreads() {
  constexpr size_t kTotal = 3'000'000, kBlock = 1024;
  std::vector<int16_t> mem(size_t(1) << 13);
  RawRing ring;
  ring.reset(mem.data(), mem.size());
  std::atomic<bool> done{false};
  std::vector<int16_t> got;
  got.reserve(kTotal);
  std::thread consumer([&] {
    const int16_t* span = nullptr;
    for (;;) {
      const size_t n = ring.peek(&span, 4096);
      if (n) { got.insert(got.end(), span, span + n); ring.consume(n); continue; }
      if (done.load(std::memory_order_acquire) && !ring.pending()) break;
      std::this_thread::yield();
    }
  });
  std::vector<int16_t> block(kBlock);
  std::vector<bool> keptBlocks;
  for (size_t at = 0; at < kTotal; at += kBlock) {
    for (size_t i = 0; i < kBlock; ++i) block[i] = static_cast<int16_t>((at + i) * 7919u);
    keptBlocks.push_back(ring.push(block.data(), kBlock));
    if (!keptBlocks.back()) std::this_thread::yield();
  }
  done.store(true, std::memory_order_release);
  consumer.join();
  // The consumer saw exactly the kept blocks, intact and in order.
  size_t pos = 0;
  uint64_t droppedFromFlags = 0;
  for (size_t b = 0; b < keptBlocks.size(); ++b) {
    if (!keptBlocks[b]) { droppedFromFlags += kBlock; continue; }
    for (size_t i = 0; i < kBlock; ++i)
      assert(got[pos + i] == static_cast<int16_t>((b * kBlock + i) * 7919u));
    pos += kBlock;
  }
  assert(pos == got.size() && droppedFromFlags == ring.dropped() && ring.seen() == keptBlocks.size() * kBlock);
  // When the gap list was not truncated, it names exactly the dropped blocks.
  if (!ring.gapsLost()) {
    uint64_t sum = 0;
    for (size_t g = 0; g < ring.gapCount(); ++g) {
      sum += ring.gap(g).samples;
      for (uint64_t at = ring.gap(g).at; at < ring.gap(g).at + ring.gap(g).samples; at += kBlock)
        assert(!keptBlocks[at / kBlock]);
    }
    assert(sum == ring.dropped());
  }
  std::printf("threads: %zu blocks, %llu samples dropped\n", keptBlocks.size(), (unsigned long long)ring.dropped());
}

static void testLongRun() {
  // Many wraps of a tiny ring (the counters run far past its capacity).
  std::vector<int16_t> mem(8);
  RawRing ring;
  ring.reset(mem.data(), mem.size());
  auto a = ramp(0, 3);
  for (size_t i = 0; i < 100000; ++i) {
    assert(ring.push(a.data(), a.size()));
    assert(drainAll(ring) == a);
  }
}

int main() {
  testWrapAndOrder();
  testDropsAndGaps();
  testGapCap();
  testInvalidCapacity();
  testLongRun();
  testThreads();
  std::puts("stt_raw_ring: OK");
  return 0;
}
