// The real stt/stt_model_slot.h against a simulated 32 MB PSRAM heap that places blocks the way ESP-IDF's
// TLSF does in the case that matters here: the smallest free block that fits, allocation at its start,
// neighbours merged on free, a shrinking realloc kept in place with its tail returned as a free block.
//
// The control replays the field failure (portable P4X-EYE, Nov 2026: "worked once or twice and then no
// more"): a session that allocates the 22.7 MB weights, trims them, and frees them at its end, while other
// features keep blocks they allocate mid-session. The slot runs the same sessions and must never need a
// second block. Sizes are the Citrinet-384 build's.
#include "stt/stt_model_slot.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <vector>

namespace {
constexpr size_t kMB = 1000000;
constexpr size_t kRaw = 22700144, kKeep = 21383888;  // identity::kRawBytes, kResidentKeepBytes
constexpr size_t kPsram = 31750000;                  // portable boot: "PSRAM free 31.75 MB"

// ---- simulated heap ---------------------------------------------------------------------------------
struct Heap {
    std::map<size_t, size_t> freeBlocks;  // offset -> size
    std::map<size_t, size_t> used;        // offset -> size
    size_t allocs = 0, frees = 0, shrinks = 0;
    bool refuseShrink = false, moveShrink = false;
    void init() { freeBlocks = {{0, kPsram}}; used.clear(); allocs = frees = shrinks = 0; refuseShrink = moveShrink = false; }
    static size_t round(size_t n) { return (n + 15) & ~size_t(15); }
    static void* ptr(size_t off) { return reinterpret_cast<void*>(uintptr_t(0x10000000) + off); }
    static size_t off(void* p) { return size_t(reinterpret_cast<uintptr_t>(p) - uintptr_t(0x10000000)); }
    size_t largest() const { size_t m = 0; for (auto& f : freeBlocks) m = std::max(m, f.second); return m; }
    size_t freeBytes() const { size_t n = 0; for (auto& f : freeBlocks) n += f.second; return n; }
    void* alloc(size_t n) {
        n = round(n);
        auto best = freeBlocks.end();
        for (auto it = freeBlocks.begin(); it != freeBlocks.end(); ++it)
            if (it->second >= n && (best == freeBlocks.end() || it->second < best->second)) best = it;
        if (best == freeBlocks.end()) return nullptr;
        const size_t at = best->first, size = best->second;
        freeBlocks.erase(best);
        if (size > n) freeBlocks[at + n] = size - n;
        used[at] = n;
        ++allocs;
        return ptr(at);
    }
    void giveFree(size_t at, size_t n) {
        auto next = freeBlocks.find(at + n);
        if (next != freeBlocks.end()) { n += next->second; freeBlocks.erase(next); }
        auto prev = freeBlocks.lower_bound(at);
        if (prev != freeBlocks.begin()) {
            --prev;
            if (prev->first + prev->second == at) { prev->second += n; return; }
        }
        freeBlocks[at] = n;
    }
    void release(void* p) {
        if (!p) return;
        auto it = used.find(off(p));
        assert(it != used.end());  // double free / not ours
        const size_t at = it->first, n = it->second;
        used.erase(it);
        ++frees;
        giveFree(at, n);
    }
    void* shrink(void* p, size_t n) {
        ++shrinks;
        if (refuseShrink) return nullptr;
        auto it = used.find(off(p));
        assert(it != used.end());
        n = round(n);
        assert(n <= it->second);
        if (moveShrink) {  // what a shrinking realloc must never do: a new block, the old one freed
            void* moved = alloc(n);
            release(p);
            return moved;
        }
        const size_t at = it->first, tail = it->second - n;
        it->second = n;
        if (tail) giveFree(at + n, tail);
        return p;
    }
} heap;
hw1::stt::ModelSlotHeap slotHeap() {
    return {[](size_t n) { return heap.alloc(n); }, [](void* p, size_t n) { return heap.shrink(p, n); },
            [](void* p) { heap.release(p); }};
}

// ---- one session, as System_STT.cpp / System_STTLocal.cpp / citrinet_runtime.cpp allocate it ----------
// Other features' blocks that appear during a session and are never freed (G2 mic ring and LC3 decoder, LLM
// result buffer, debug pool growth, ESP-NOW peer history, Bluetooth connection blocks, a grown G2 scratch).
const size_t kPinned[][3] = {
    {65536, 8192, 28672}, {16384, 24576, 4096}, {32768, 12288, 2048}, {49152, 16384, 6144},
    {16384, 4096, 8192},  {98304, 2048, 1024},  {20480, 6144, 3072},  {16384, 16384, 16384},
};
struct Session {
    std::vector<void*> audio, transient;
};
std::vector<void*> kept;  // never freed (other features)
void startAudio(Session& s) {
    for (int i = 0; i < 5; ++i) s.audio.push_back(heap.alloc(320000));  // stt.segment, 3 x stt.queue, stt.draft
    s.audio.push_back(heap.alloc(64 * 1024));                           // segmenter
    for (void* p : s.audio) assert(p);
}
void endAudio(Session& s) { for (void* p : s.audio) heap.release(p); s.audio.clear(); }
void midSession(Session& s, int n) {
    s.transient.push_back(heap.alloc(1600000));   // the LM (meeting.lm)
    s.transient.push_back(heap.alloc(800000));    // feature pool
    s.transient.push_back(heap.alloc(3300000));   // speaker model, arena, workspace
    // First uses: each set once, in the first sessions after boot (about 0.5 MB in all).
    if (size_t(n) < std::size(kPinned))
        for (size_t bytes : kPinned[n]) {
            void* p = heap.alloc(bytes);
            assert(p);
            kept.push_back(p);
        }
    s.transient.push_back(heap.alloc(14336));     // an LLM request body, freed with the session's tail
}
void endSession(Session& s) { for (void* p : s.transient) heap.release(p); s.transient.clear(); }
void* buildResident(std::vector<void*>& residentBlocks) {
    residentBlocks.push_back(heap.alloc(1216000 + 64 * 1024));  // 10 s arena
    residentBlocks.push_back(heap.alloc(300000));               // ESP-DL metadata, prepared conv blocks
    return residentBlocks.front();
}

// ---- control: the lifecycle before the slot ----------------------------------------------------------
int controlFirstRefusal(int sessions) {
    heap.init(); kept.clear();
    kept.push_back(heap.alloc(250000));  // boot allocations
    for (int n = 0; n < sessions; ++n) {
        Session s;
        startAudio(s);
        if (heap.largest() < kRaw) { endAudio(s); return n + 1; }  // admission: "No contiguous PSRAM block"
        void* raw = heap.alloc(kRaw);
        assert(raw);
        std::vector<void*> resident;
        buildResident(resident);
        assert(heap.shrink(raw, kKeep) == raw);  // the in-place trim
        midSession(s, n);
        endSession(s);
        for (void* p : resident) heap.release(p);
        heap.release(raw);  // session end: the weights go
        endAudio(s);
    }
    return 0;
}

// ---- the slot: same sessions, weights kept for the boot ----------------------------------------------
struct Cache { hw1::stt::ModelSlot* slot; void* raw = nullptr; };
void slotLifecycle(bool reserveAtBoot, int sessions) {
    heap.init(); kept.clear();
    hw1::stt::ModelSlot slot(kRaw, kKeep);
    const auto h = slotHeap();
    kept.push_back(heap.alloc(250000));
    if (reserveAtBoot) assert(slot.reserve(h));
    void* const first = reserveAtBoot ? slot.block() : nullptr;
    int loads = 0;
    std::vector<void*> resident;
    for (int n = 0; n < sessions; ++n) {
        Session s;
        startAudio(s);
        Cache cache{&slot};
        assert(slot.lend(&cache));
        if (slot.loaded()) cache.raw = slot.block();
        else {
            // The admission check of citrinet_runtime.cpp: one free block of kRaw only without a reserved one.
            assert(slot.reserved() || heap.largest() >= kRaw);
            assert(slot.reserve(h));
            ++loads;                              // the card read and SHA-256 check
            assert(slot.setLoaded(true));
            cache.raw = slot.block();
        }
        if (resident.empty()) {
            buildResident(resident);
            assert(slot.trim(h) == hw1::stt::ModelSlot::Trim::Trimmed);
            assert(slot.trimmed() && slot.loaded());
        }
        midSession(s, n);
        endSession(s);
        slot.giveBack(&cache);                    // ModelCache::reset: never a free
        endAudio(s);
        assert(!slot.lent() && slot.loaded() && slot.reserved());
        if (first) assert(slot.block() == first);
        // A trimmed image cannot be invalidated back into a load: the block would be too short.
        assert(!slot.setLoaded(false) && slot.loaded());
    }
    assert(loads == 1);
    assert(heap.shrinks == 1);
    // Idle: the weights (trimmed) and the resident model stay; what is left is free for everything else.
    const size_t idleFree = heap.freeBytes();
    std::printf("slot boot=%d sessions=%d loads=%d idle_free=%.2f MB largest=%.2f MB\n", reserveAtBoot ? 1 : 0,
                sessions, loads, double(idleFree) / kMB, double(heap.largest()) / kMB);
    assert(idleFree > 7 * kMB);
}

// ---- unit behaviour ----------------------------------------------------------------------------------
void leases() {
    heap.init();
    hw1::stt::ModelSlot slot(kRaw, kKeep);
    int a = 0, b = 0;
    assert(!slot.lend(nullptr));
    assert(slot.lend(&a) && slot.lend(&a) && slot.lentTo(&a));
    assert(!slot.lend(&b));       // one user at a time
    slot.giveBack(&b);            // not the holder: ignored
    assert(slot.lentTo(&a));
    slot.giveBack(&a);
    assert(!slot.lent() && slot.lend(&b));
    slot.giveBack(&b);
}
void reserveOnce() {
    heap.init();
    hw1::stt::ModelSlot slot(kRaw, kKeep);
    const auto h = slotHeap();
    void* hog = heap.alloc(kPsram - kRaw + 4096);  // no single block of kRaw left
    assert(!slot.reserve(h) && !slot.reserved() && heap.used.size() == 1);
    assert(!slot.setLoaded(true));                  // nothing to load into
    heap.release(hog);
    assert(slot.reserve(h) && slot.reserve(h));
    assert(heap.allocs == 2);                       // the hog and one block: reserve never allocates twice
    void* block = slot.block();
    // A failed or cancelled read: the block stays for the next attempt.
    assert(slot.setLoaded(false) && slot.reserved() && slot.block() == block && !slot.loaded());
    assert(slot.trim(h) == hw1::stt::ModelSlot::Trim::Kept && heap.shrinks == 0);  // nothing loaded to trim
    assert(slot.setLoaded(true));
    assert(heap.frees == 1);
}
void trims() {
    const auto h = slotHeap();
    {   // the heap refuses: the whole image stays
        heap.init();
        hw1::stt::ModelSlot slot(kRaw, kKeep);
        assert(slot.reserve(h) && slot.setLoaded(true));
        heap.refuseShrink = true;
        assert(slot.trim(h) == hw1::stt::ModelSlot::Trim::Kept);
        assert(!slot.trimmed() && slot.loaded() && slot.reserved());
        heap.refuseShrink = false;
        assert(slot.trim(h) == hw1::stt::ModelSlot::Trim::Trimmed);
        assert(slot.trim(h) == hw1::stt::ModelSlot::Trim::Kept && heap.shrinks == 2);  // once per boot
        assert(heap.used.begin()->second == Heap::round(kKeep));
    }
    {   // moved: the copy is released, the slot is empty, the next call reserves again
        heap.init();
        hw1::stt::ModelSlot slot(4 * kMB, 3 * kMB);  // small enough that the heap has room for a moved copy
        assert(slot.reserve(h) && slot.setLoaded(true));
        heap.moveShrink = true;
        assert(slot.trim(h) == hw1::stt::ModelSlot::Trim::Lost);
        assert(!slot.reserved() && !slot.loaded() && !slot.trimmed() && heap.used.empty());
        heap.moveShrink = false;
        assert(slot.reserve(h) && !slot.loaded());
    }
    {   // an image with nothing to trim (kRawBytes == kResidentKeepBytes)
        heap.init();
        hw1::stt::ModelSlot slot(kRaw, kRaw);
        assert(slot.reserve(h) && slot.setLoaded(true));
        assert(slot.trim(h) == hw1::stt::ModelSlot::Trim::Kept && heap.shrinks == 0);
    }
}
void splits() {
    using hw1::stt::splitSpeech;
    constexpr size_t kRate = 16000, kMax = 10 * kRate, kMin = kRate;
    std::vector<int16_t> pcm(30 * kRate);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = int16_t((i * 7919) % 8000) - 4000;  // loud everywhere
    size_t cuts[4] = {};
    assert(splitSpeech(pcm.data(), kMax, kMax, kMin, cuts, 4) == 0);      // fits in one call
    assert(splitSpeech(pcm.data(), 15 * kRate, kMax, kMin, nullptr, 4) == 0);
    assert(splitSpeech(pcm.data(), 30 * kRate, kMax, kMin, cuts, 1) == 0);  // needs two cuts
    // 19 s with a pause at 9.4 s: one cut, inside the pause (the cut may fall anywhere from 9 to 10 s).
    std::vector<int16_t> quiet(pcm.begin(), pcm.begin() + 19 * kRate);
    for (size_t i = size_t(9.35 * kRate); i < size_t(9.45 * kRate); ++i) quiet[i] = 0;
    assert(splitSpeech(quiet.data(), quiet.size(), kMax, kMin, cuts, 4) == 1);
    assert(cuts[0] >= size_t(9.35 * kRate) && cuts[0] <= size_t(9.45 * kRate));
    // A pause the second piece could not take (cut at 4 s would leave 16 s) is not used: 20 s cuts at 10 s.
    std::vector<int16_t> early(pcm.begin(), pcm.begin() + 20 * kRate);
    for (size_t i = 4 * kRate; i < size_t(4.1 * kRate); ++i) early[i] = 0;
    assert(splitSpeech(early.data(), early.size(), kMax, kMin, cuts, 4) == 1 && cuts[0] == kMax);
    // Every length up to the runtime's 30 s: fewest pieces, each within limits, cuts increasing.
    for (size_t samples = kMax + 1; samples <= pcm.size(); samples += 777) {
        const size_t n = splitSpeech(pcm.data(), samples, kMax, kMin, cuts, 4);
        assert(n == (samples + kMax - 1) / kMax - 1);
        size_t from = 0;
        for (size_t k = 0; k <= n; ++k) {
            const size_t to = k < n ? cuts[k] : samples;
            assert(to > from && to - from <= kMax && to - from >= kMin);
            from = to;
        }
    }
}
}  // namespace

int main() {
    leases();
    reserveOnce();
    trims();
    splits();
    // The control must reproduce the field report, or the scenario proves nothing.
    const int refused = controlFirstRefusal(20);
    std::printf("control: first refused cold load at session %d\n", refused);
    assert(refused >= 2 && refused <= 4);
    slotLifecycle(true, 40);
    // Not reserved at boot (a board that could not spare it): the first session's load takes the block,
    // which then stays for every later session too.
    slotLifecycle(false, 40);
    std::puts("stt model slot: ok");
    return 0;
}
