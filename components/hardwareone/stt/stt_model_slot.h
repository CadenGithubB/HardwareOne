#pragma once
// The boot-lifetime home of the local speech model's weights (Citrinet-384: one 22,700,144-byte block).
//
// Until 19 Nov 2026 every session that could not adopt a parked model allocated the weights afresh and freed
// them when it ended. On the 32 MB P4X-EYE that leaves about 9 MB of slack: once any long-lived block (a G2
// mic ring, an LLM buffer, a Bluetooth connection, a log buffer) landed inside the old model's region, which
// the in-place trim (kResidentKeepBytes) made likely by returning its last 1.3 MB as a free block mid-session,
// the freed region was about 21.4 MB and every later load was refused ("worked once or twice, then no more").
//
// Now the block is allocated once per boot, as early as possible (sttLocalReserveModel at boot, else the first
// call), and never freed: sessions borrow it (lend / giveBack, one user at a time, as the broker runs one
// worker at a time) and only the first load reads the card. The trim still returns the tail once the resident
// model is built; that tail is ordinary free memory, since the block never has to grow back. Optional memory
// (the language model, the speaker model, the feature pool) is what a session sheds, never the weights.
//
// Header-only and free of ESP-IDF so host tests drive it against a simulated heap (test_stt_model_slot.py).
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace hw1::stt {

// The heap operations the slot uses; the firmware passes explicit PSRAM requests (heap_caps_*, MALLOC_CAP_SPIRAM).
struct ModelSlotHeap {
    void* (*alloc)(size_t bytes);              // one block of `bytes`, 16-byte aligned; nullptr when none is free
    void* (*shrink)(void* block, size_t bytes);  // realloc to fewer bytes; nullptr: refused, block unchanged
    void (*release)(void* block);
};

class ModelSlot {
 public:
    constexpr ModelSlot(size_t bytes, size_t keepBytes) : bytes_(bytes), keep_(keepBytes < bytes ? keepBytes : bytes) {}
    ModelSlot(const ModelSlot&) = delete;
    ModelSlot& operator=(const ModelSlot&) = delete;

    // Allocates the block once; true while the slot holds one. The block is never released again (trim() only
    // returns its tail), so later fragmentation cannot take the model's place.
    bool reserve(const ModelSlotHeap& heap) {
        if (!block_.load()) block_.store(heap.alloc(bytes_));
        return block_.load() != nullptr;
    }
    bool reserved() const { return block_.load() != nullptr; }
    void* block() const { return block_.load(); }
    size_t bytes() const { return bytes_; }
    // The block holds the whole verified image (until trim()) or the resident model's part of it (after).
    bool loaded() const { return loaded_.load(); }
    bool trimmed() const { return trimmed_.load(); }
    // Set after the image is read and its SHA-256 matched; cleared when the contents can no longer be trusted
    // (a failed or cancelled read, a checksum mismatch): the block stays, the next load reads into it again.
    // A trimmed image cannot be read back into its shortened block, so clearing it is refused.
    bool setLoaded(bool loaded) {
        if (!block_.load() || (trimmed_.load() && !loaded)) return false;
        loaded_.store(loaded);
        return true;
    }

    // One user at a time. True when `user` holds the slot afterwards (taken now, or already its holder).
    bool lend(const void* user) {
        if (!user) return false;
        const void* expected = nullptr;
        return user_.compare_exchange_strong(expected, user) || expected == user;
    }
    bool lentTo(const void* user) const { return user && user_.load() == user; }
    bool lent() const { return user_.load() != nullptr; }
    // Only the holder can give it back; anyone else is ignored.
    void giveBack(const void* user) {
        const void* expected = user;
        (void)user_.compare_exchange_strong(expected, nullptr);
    }

    enum class Trim : uint8_t {
        Kept,     // nothing to trim, not loaded, or the heap refused: the whole image stays
        Trimmed,  // shrunk in place to keepBytes; the tail is free memory now
        Lost,     // the heap moved the block (a shrinking realloc never should): the moved copy is released and
                  // the slot is empty; every pointer into the old block is stale, and the next call reserves again
    };
    // Once, after the resident model is built: the image's tail (build-only data) is returned.
    Trim trim(const ModelSlotHeap& heap) {
        void* const block = block_.load();
        if (!block || !loaded_.load() || trimmed_.load() || keep_ >= bytes_) return Trim::Kept;
        void* kept = heap.shrink(block, keep_);
        if (!kept) return Trim::Kept;
        if (kept == block) {
            trimmed_.store(true);
            return Trim::Trimmed;
        }
        heap.release(kept);
        loaded_.store(false);
        trimmed_.store(false);
        block_.store(nullptr);
        return Trim::Lost;
    }

 private:
    const size_t bytes_, keep_;
    // Changed only at boot or by the slot's holder; atomic because other tasks read them (singleCallSamples,
    // reserved) while a holder may be loading or trimming.
    std::atomic<void*> block_{nullptr};
    std::atomic<bool> loaded_{false}, trimmed_{false};
    std::atomic<const void*> user_{nullptr};
};

// Where to cut `samples` of 16 kHz PCM into the fewest pieces of at most `maxPiece` samples, each at least
// `minPiece`: near equal lengths, each cut at the quietest 20 ms within half a second of its ideal place, so
// a cut rarely falls inside a word. Writes the cut positions (strictly increasing) and returns how many
// (0: one piece; also 0 when maxCuts is too small or the limits cannot be met).
inline size_t splitSpeech(const int16_t* pcm, size_t samples, size_t maxPiece, size_t minPiece,
                          size_t* cuts, size_t maxCuts) {
    constexpr size_t kFrame = 320, kHop = 160, kWindow = 8000;  // 20 ms frames, 10 ms hop, +-0.5 s
    if (!pcm || !cuts || !maxPiece || samples <= maxPiece || minPiece > maxPiece) return 0;
    const size_t pieces = (samples + maxPiece - 1) / maxPiece;
    if (pieces - 1 > maxCuts || samples < pieces * minPiece) return 0;
    size_t start = 0;
    for (size_t k = 1; k < pieces; ++k) {
        const size_t left = pieces - k;  // pieces after this cut
        // The rest must fit in `left` pieces of at most maxPiece and at least minPiece.
        const size_t lo = std::max(start + minPiece, samples > left * maxPiece ? samples - left * maxPiece : 0);
        const size_t hi = std::min(start + maxPiece, samples - left * minPiece);
        if (lo > hi) return 0;
        const size_t ideal = start + (samples - start) / (left + 1);
        const size_t from = std::max(lo, ideal > kWindow ? ideal - kWindow : 0);
        const size_t to = std::min(hi, ideal + kWindow);
        size_t best = std::min(std::max(ideal, lo), hi);
        if (from <= to) {
            uint64_t quietest = UINT64_MAX;
            // Cut at the centre of each candidate frame; frames reaching past the audio are not scored.
            for (size_t at = from; at <= to; at += kHop) {
                if (at < kFrame / 2 || at + kFrame / 2 > samples) continue;
                uint64_t energy = 0;
                for (size_t i = at - kFrame / 2; i < at + kFrame / 2; ++i)
                    energy += uint64_t(int64_t(pcm[i]) * pcm[i]);
                if (energy < quietest) { quietest = energy; best = at; }
            }
        }
        cuts[k - 1] = best;
        start = best;
    }
    return pieces - 1;
}

}  // namespace hw1::stt
