// Host harness for the g2micwav power-cut checkpoint (G2_Glasses.cpp).
//
// Runs the production packet writer, checkpoint, header patch and close over a
// file double that separates what was written from what is durable: flush()
// and close() copy the written bytes to the "disk", and a power cut keeps only
// the disk. Each cut must leave a WAV that parses, whose RIFF and data sizes
// match the bytes on disk, and that holds every packet up to the checkpoint.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

// ---- boundary doubles -------------------------------------------------------
// The storage behind one capture; it outlives the File the recorder deletes.
struct Store {
  std::vector<uint8_t> live;                 // bytes written so far
  std::vector<uint8_t> disk;                 // bytes a power cut keeps
  std::vector<std::vector<uint8_t>> syncs;   // disk after every flush/close (keepSyncs)
  bool keepSyncs = false;
  int syncCount = 0;
  int headerSeeksWhileOpen = 0;              // seeks into the header before close
  int failSeekTo = -1;                       // seek(pos) to this offset fails once
  bool closed = false;
};

class File {
 public:
  explicit File(Store* store) : st(store) {}
  explicit operator bool() const { return !st->closed; }
  size_t write(const uint8_t* buf, size_t n) {
    if (st->closed) return 0;
    if (st->live.size() < pos + n) st->live.resize(pos + n);
    std::memcpy(st->live.data() + pos, buf, n);
    pos += n;
    return n;
  }
  bool seek(uint32_t to) {
    if (st->closed) return false;
    if (st->failSeekTo >= 0 && static_cast<uint32_t>(st->failSeekTo) == to) {
      st->failSeekTo = -1;
      return false;
    }
    if (to > st->live.size()) return false;
    if (to < 44) ++st->headerSeeksWhileOpen;
    pos = to;
    return true;
  }
  void flush() {
    st->disk = st->live;
    ++st->syncCount;
    if (st->keepSyncs) st->syncs.push_back(st->disk);
  }
  void close() {
    if (st->closed) return;
    flush();
    st->closed = true;
  }

 private:
  Store* st;
  size_t pos = 0;
};

// FS lock double: reentrant like the real guard (a nested guard in the owning
// task is a no-op). The one-argument form waits forever (portMAX_DELAY); a
// take that may wait is counted, because the packet writer runs in the BLE
// host task and must never wait for the FS lock.
static int fsDepth = 0;
static int blockingFsTakes = 0;
struct FsLockGuard {
  bool held = false;
  explicit FsLockGuard(const char* owner) : FsLockGuard(owner, ~0u) {}
  FsLockGuard(const char*, uint32_t timeoutTicks) {
    if (fsDepth > 0) return;
    if (timeoutTicks != 0) ++blockingFsTakes;
    held = true;
    ++fsDepth;
  }
  ~FsLockGuard() {
    if (held) --fsDepth;
  }
};
static bool isFsLockedByCurrentTask() { return fsDepth > 0; }

struct String {
  std::string s;
  const char* c_str() const { return s.c_str(); }
};

typedef void* lc3_decoder_t;
enum lc3_pcm_format { LC3_PCM_FORMAT_S16 };
// Deterministic "decode": every sample carries its frame's first two bytes, so
// the harness can check which packets reached the disk, in order.
static int lc3_decode(lc3_decoder_t, const void* in, int, lc3_pcm_format, void* out, int) {
  const uint8_t* b = static_cast<const uint8_t*>(in);
  int16_t* pcm = static_cast<int16_t*>(out);
  for (int i = 0; i < 160; ++i) pcm[i] = static_cast<int16_t>(b[0] | (b[1] << 8));
  return 0;
}

static int closes = 0;
static std::string lastCloseReason;
#define DEBUG_G2F(fmt, ...)                                   \
  do {                                                        \
    ++closes;                                                 \
    lastCloseReason = harnessCloseReason(__VA_ARGS__);        \
  } while (0)
static std::string harnessCloseReason(const char*, unsigned, unsigned, unsigned, unsigned,
                                      const char* reason) {
  return reason ? reason : "";
}

// ---- production state (G2_Glasses.cpp statics) ------------------------------
static File*         gMicWavFile    = nullptr;
static String        gMicWavPath;
static uint32_t      gMicWavBytes   = 0;
static uint32_t      gMicWavPackets = 0;
static lc3_decoder_t gMicWavDecoder = nullptr;
static void*         gMicWavDecMem  = nullptr;
static uint32_t      gMicWavDecodeFails = 0;
static uint32_t      gMicWavPlcFrames   = 0;
static uint32_t      gMicWavCheckpointBytes = 0;
static bool          gMicWavPatchHeader     = false;

static constexpr int kMicLc3FrameUs   = 10000;
static constexpr int kMicLc3SampleHz  = 16000;
static constexpr int kMicLc3FrameBytes = 40;
static constexpr int kMicLc3FramesPerPkt = 5;
static constexpr int kMicLc3SamplesPerFrame = kMicLc3SampleHz * kMicLc3FrameUs / 1000000;

// INSERT_PRODUCTION

// ---- checks -----------------------------------------------------------------
static int failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond);           \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static uint32_t le32(const std::vector<uint8_t>& b, size_t at) {
  return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
}
static uint16_t le16(const std::vector<uint8_t>& b, size_t at) {
  return static_cast<uint16_t>(b[at] | (b[at + 1] << 8));
}

// Parses `disk` as the 16 kHz mono 16-bit WAV the recorder writes. Returns the
// data size, or -1 when it is not a valid WAV. `exact`: the sizes must match
// the file length exactly (after a completed checkpoint); otherwise the header
// may describe less than the disk holds, never more.
static long parseWav(const std::vector<uint8_t>& disk, bool exact) {
  if (disk.size() < 44) return -1;
  if (std::memcmp(disk.data(), "RIFF", 4) || std::memcmp(disk.data() + 8, "WAVE", 4) ||
      std::memcmp(disk.data() + 12, "fmt ", 4) || std::memcmp(disk.data() + 36, "data", 4)) {
    return -1;
  }
  if (le32(disk, 16) != 16 || le16(disk, 20) != 1 || le16(disk, 22) != 1 ||
      le32(disk, 24) != 16000 || le32(disk, 28) != 32000 || le16(disk, 32) != 2 ||
      le16(disk, 34) != 16) {
    return -1;
  }
  const uint32_t riff = le32(disk, 4), data = le32(disk, 40);
  if (riff != 36 + data) return -1;
  if (exact ? 44 + static_cast<size_t>(data) != disk.size() : 44 + static_cast<size_t>(data) > disk.size()) {
    return -1;
  }
  return data;
}

// Packet i's PCM samples all equal (i & 0xff) | ((i >> 8) & 0xff) << 8.
static bool pcmPrefixOk(const std::vector<uint8_t>& disk, uint32_t dataBytes) {
  for (uint32_t off = 0; off + 1 < dataBytes; off += 2) {
    const uint32_t pkt = off / 1600;
    const uint16_t want = static_cast<uint16_t>((pkt & 0xff) | (((pkt >> 8) & 0xff) << 8));
    if (le16(disk, 44 + off) != want) return false;
  }
  return true;
}

static Store* startCapture(bool onSd, bool keepSyncs = false) {
  Store* st = new Store();
  st->keepSyncs = keepSyncs;
  File* f = new File(st);
  micWavWriteHeader(*f, 0);
  f->flush();  // as cmd_g2micwav start does
  gMicWavFile = f;
  gMicWavPath.s = onSd ? "/sd/g2_mic-1.wav" : "/g2_mic-1.wav";
  gMicWavBytes = 0;
  gMicWavPackets = 0;
  gMicWavCheckpointBytes = 0;
  gMicWavPatchHeader = onSd;
  gMicWavDecoder = reinterpret_cast<lc3_decoder_t>(1);
  gMicWavDecMem = std::malloc(16);
  return st;
}

// The BLE notify path (handleAudioNotify): writes while under the cap. It
// must not take the FS lock with a wait anywhere in here.
static void writePackets(uint32_t first, uint32_t count) {
  uint8_t pkt[205] = {};
  const int blockingBefore = blockingFsTakes;
  for (uint32_t i = first; i < first + count && gMicWavFile; ++i) {
    for (int fr = 0; fr < 5; ++fr) {
      pkt[fr * 40] = static_cast<uint8_t>(i & 0xff);
      pkt[fr * 40 + 1] = static_cast<uint8_t>((i >> 8) & 0xff);
    }
    if (gMicWavBytes < kMicWavMaxBytes && gMicWavDecoder) micWavWritePacketLocked(pkt);
    if (fsDepth != 0) {
      std::fprintf(stderr, "FAIL: the packet writer returned with the FS lock held\n");
      std::exit(1);
    }
  }
  if (blockingFsTakes != blockingBefore) {
    std::fprintf(stderr, "FAIL: the BLE notify path waited for the FS lock %d time(s)\n",
                 blockingFsTakes - blockingBefore);
    std::exit(1);
  }
}

static void endCapture(Store* st) {
  if (gMicWavFile) {  // power cut: nothing closes, only the disk survives
    std::free(gMicWavDecMem);
    gMicWavDecMem = nullptr;
    delete gMicWavFile;
    gMicWavFile = nullptr;
  }
  delete st;
}

int main(int argc, char** argv) {
  const unsigned seed = argc > 1 ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10)) : 1u;
  std::mt19937 rng(seed);
  const uint32_t interval = kMicWavCheckpointBytes;
  CHECK(interval % 1600 == 0);
  CHECK(interval >= 32000 && interval <= 5u * 32000u);  // "every few seconds"

  // 1. Random cut points on SD: the surviving file is a WAV whose sizes match
  //    the disk, and it holds every packet up to the last checkpoint.
  int cuts = 0;
  for (int trial = 0; trial < 300; ++trial) {
    const uint32_t packets = 1 + rng() % 600;  // up to 30 s of audio
    Store* f = startCapture(true);
    writePackets(0, packets);
    const std::vector<uint8_t> disk = f->disk;
    const uint32_t written = packets * 1600;
    const uint32_t expected = (written / interval) * interval;
    const long data = parseWav(disk, /*exact=*/true);
    CHECK(data >= 0);
    CHECK(data == static_cast<long>(expected));
    CHECK(written - static_cast<uint32_t>(data < 0 ? 0 : data) < interval);
    if (data > 0) CHECK(pcmPrefixOk(disk, static_cast<uint32_t>(data)));
    CHECK(f->headerSeeksWhileOpen == 2 * static_cast<int>(written / interval));
    endCapture(f);
    ++cuts;
  }

  // 2. A cut between any two syncs, including inside a checkpoint, never
  //    leaves a header that claims more than the disk holds, and every
  //    completed checkpoint is exact.
  {
    Store* f = startCapture(true, /*keepSyncs=*/true);
    writePackets(0, 400);
    CHECK(f->syncs.size() == 1 + 2 * (400u * 1600u / interval));
    for (size_t i = 0; i < f->syncs.size(); ++i) {
      const long loose = parseWav(f->syncs[i], /*exact=*/false);
      CHECK(loose >= 0);
      if (loose > 0) CHECK(pcmPrefixOk(f->syncs[i], static_cast<uint32_t>(loose)));
      if (i % 2 == 0) CHECK(parseWav(f->syncs[i], /*exact=*/true) == static_cast<long>((i / 2) * interval));
    }
    endCapture(f);
  }

  // 3. A normal stop still completes the header with everything written.
  {
    Store* f = startCapture(true);
    writePackets(0, 123);
    micWavCloseLocked("user stop");
    CHECK(!gMicWavFile && closes > 0 && lastCloseReason == "user stop");
    CHECK(parseWav(f->disk, /*exact=*/true) == 123 * 1600);
    CHECK(pcmPrefixOk(f->disk, 123 * 1600));
    endCapture(f);
  }

  // 4. LittleFS: a checkpoint syncs but never seeks back into the header
  //    (a header rewrite there copies the file); the PCM still reaches disk.
  {
    Store* f = startCapture(false);
    writePackets(0, 200);
    CHECK(f->headerSeeksWhileOpen == 0);
    CHECK(f->disk.size() == 44 + (200u * 1600u / interval) * interval);
    CHECK(parseWav(f->disk, /*exact=*/false) == 0);
    micWavCloseLocked("user stop");
    CHECK(parseWav(f->disk, /*exact=*/true) == 200 * 1600);
    endCapture(f);
  }

  // 5. A header rewrite that cannot return to the end stops the capture
  //    instead of writing the next packet over the data.
  {
    Store* f = startCapture(true);
    f->failSeekTo = static_cast<int>(44 + interval);
    writePackets(0, interval / 1600 + 10);
    CHECK(!gMicWavFile);
    CHECK(lastCloseReason == "checkpoint failed");
    CHECK(f->live.size() == 44 + interval);
    CHECK(parseWav(f->disk, /*exact=*/true) == static_cast<long>(interval));
    CHECK(pcmPrefixOk(f->disk, interval));
    endCapture(f);
  }

  // 6. The 16 MB cap closes the capture inside the packet write, under the FS
  //    lock the write already holds (writePackets fails on any wait for it),
  //    and the closed file is exact, including the last packet past the cap.
  //    Seed-independent and slow (16 MB of syncs), so only when asked ("cap"),
  //    and not after a failure above (a broken checkpoint makes it crawl).
  const bool runCap = argc > 2 && std::strcmp(argv[2], "cap") == 0 && failures == 0;
  if (runCap) {
    Store* f = startCapture(true);
    const uint32_t capPackets = (kMicWavMaxBytes + 1599) / 1600;
    writePackets(0, capPackets - 1);
    CHECK(gMicWavFile != nullptr);
    writePackets(capPackets - 1, 5);
    CHECK(!gMicWavFile);
    CHECK(lastCloseReason == "cap reached");
    CHECK(parseWav(f->disk, /*exact=*/true) == static_cast<long>(capPackets * 1600));
    CHECK(gMicWavBytes == capPackets * 1600);
    endCapture(f);
  }

  if (failures) {
    std::fprintf(stderr, "g2micwav checkpoint: %d check(s) failed (seed %u)\n", failures, seed);
    return 1;
  }
  std::fprintf(stderr, "g2micwav checkpoint: %d random cuts and %d scenarios passed (seed %u)\n",
               cuts, runCap ? 5 : 4, seed);
  return 0;
}
