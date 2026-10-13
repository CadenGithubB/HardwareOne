// Drives the production AVI recorder file path from System_Camera_Video.cpp
// (header skeleton, frame writer, first-frame geometry patch, finalizer and
// failed-start cleanup) against the shared fake VFS and the real FsLockGuard.
// The recorder task loop is not compiled; recordFrame() below follows its
// per-frame order (geometry patch on the first frame, then the chunk write).
#define FAKE_VFS_PROVIDE_VFS_NAMESPACE
#define FAKE_VFS_LOCK_DOUBLE
#include <Arduino.h>
#include "fake_vfs.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

void webBatchTestStringAppend() {}

static void hostLog(const char*, ...) {}
#define ERROR_CAMERAF(...) hostLog(__VA_ARGS__)
#define INFO_CAMERA_VIDEOF(...) hostLog(__VA_ARGS__)
#define ERROR_STORAGEF(...) hostLog(__VA_ARGS__)

using TaskHandle_t = void*;

enum { SYSEVT_VIDEO_SAVED = 1, SYSEVT_VIDEO_RECORD_FAILED = 2 };
static std::vector<std::string> savedEvents;
static std::vector<std::string> failedEvents;
static void systemEventPost(int kind, const char* subject, const char* detail = nullptr) {
  const std::string text = std::string(subject ? subject : "") + "|" + (detail ? detail : "");
  if (kind == SYSEVT_VIDEO_SAVED) savedEvents.push_back(text);
  if (kind == SYSEVT_VIDEO_RECORD_FAILED) failedEvents.push_back(text);
}

namespace VFS {
namespace Scopes { constexpr const char* VIDEOS = "/sd/VIDEOS"; }
static unsigned sdInUseEnds = 0;
inline void sdInUseEnd(const char*) { ++sdInUseEnds; }
}  // namespace VFS

// INSERT_FS_GUARD

// INSERT_RECORDER

#if HAS_FINALIZE_OUTCOME
#define FINALIZE(ok, elapsedMs) finalizeAndClose(ok, elapsedMs)
#else
#define FINALIZE(ok, elapsedMs) ((void)(ok), (void)(elapsedMs), finalizeAndClose(), true)
#endif

static const char* const kPath = "/sd/VIDEOS/VID_1.AVI";
static IdxEntry gIndex[16];
static size_t gAccepted = 0;  // bytes the recorder has written so far

#define CHECK(condition, what) \
  do { if (!(condition)) { std::fprintf(stderr, "FAIL: %s\n", what); return 1; } } while (0)

static std::vector<uint8_t> jpeg(uint16_t width, uint16_t height, size_t len) {
  std::vector<uint8_t> bytes(len, 0);
  const uint8_t head[] = {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x11, 0x08,
                          uint8_t(height >> 8), uint8_t(height), uint8_t(width >> 8), uint8_t(width)};
  std::memcpy(bytes.data(), head, sizeof head);
  bytes[len - 2] = 0xFF;
  bytes[len - 1] = 0xD9;
  return bytes;
}

static uint32_t at32(const std::string& bytes, size_t offset) {
  return uint32_t(uint8_t(bytes[offset])) | uint32_t(uint8_t(bytes[offset + 1])) << 8 |
         uint32_t(uint8_t(bytes[offset + 2])) << 16 | uint32_t(uint8_t(bytes[offset + 3])) << 24;
}

// The start sequence of startVideoRecording: open, header skeleton, counters.
static bool startClip() {
  auto& fs = fake_vfs::fs();
  fs.reset();
  fake_vfs::wireLockDouble();
  fs.mkdirAll("/sd/VIDEOS");
  savedEvents.clear();
  failedEvents.clear();
  VFS::sdInUseEnds = 0;
  std::snprintf(s_path, sizeof(s_path), "%s", kPath);
  s_file = VFS::openGuarded(String(s_path), FILE_WRITE, VFS::systemAuth(VFS::Scopes::VIDEOS, "video.record"), true);
  if (!s_file) return false;
  s_idx = gIndex;
  s_idxCap = 16;
  uint8_t header[AVI_HEADER_SIZE];
  buildHeaderSkeleton(header, 640, 480, 10);
  if (s_file.write(header, AVI_HEADER_SIZE) != AVI_HEADER_SIZE) return false;
  s_frameCount = s_width = s_height = s_maxFrameBytes = s_moviPayloadBytes = 0;
  s_startMs = 0;
  gAccepted = AVI_HEADER_SIZE;
  return true;
}

static bool recordFrame(size_t len) {
  const auto frame = jpeg(320, 240, len);
#if HAS_DIMS_HELPER
  if (s_frameCount == 0) {
    if (!patchFirstFrameDims(frame.data(), frame.size())) return false;
    gAccepted += 16;
  }
#endif
  if (!writeFrameChunk(frame.data(), frame.size())) return false;
  gAccepted += 8 + len + (len & 1);
  return true;
}

static bool recordThree() { return recordFrame(100) && recordFrame(101) && recordFrame(150); }

static int expectNotSaved(bool finalized, const char* what) {
  CHECK(!finalized, what);
  CHECK(savedEvents.empty(), "no VIDEO_SAVED for an incomplete clip");
  CHECK(failedEvents.size() == 1 && failedEvents[0].rfind("VID_1.AVI|", 0) == 0,
        "one VIDEO_RECORD_FAILED carrying the clip name");
  CHECK(fake_vfs::fs().sdWriteFailureNotes > 0, "the failure is reported to VFS");
  CHECK(!s_file, "the clip handle is closed");
  std::puts("PASS");
  return 0;
}

static int run(const std::string& name) {
  auto& fs = fake_vfs::fs();
  CHECK(startClip(), "clip starts");
  if (name == "saved") {
    CHECK(recordThree(), "three frames recorded");
    CHECK(FINALIZE(true, 600), "complete clip finalizes");
    CHECK(savedEvents.size() == 1 && savedEvents[0] == "VID_1.AVI|3 frames", "one VIDEO_SAVED with the clip name");
    CHECK(failedEvents.empty(), "no VIDEO_RECORD_FAILED for a complete clip");
    CHECK(fs.sdWriteFailureNotes == 0 && !s_file, "no failure reported and the handle closed");
    const std::string bytes = fs.text(kPath);
    const uint32_t movi = (8 + 100) + (8 + 101 + 1) + (8 + 150);
    CHECK(bytes.size() == AVI_HEADER_SIZE + movi + 8 + 3 * 16, "file holds header, frames and index");
    CHECK(at32(bytes, 4) == bytes.size() - 8 && at32(bytes, 216) == 4 + movi, "RIFF and movi sizes");
    CHECK(at32(bytes, 48) == 3 && at32(bytes, 140) == 3, "frame counts");
    CHECK(bytes.compare(AVI_HEADER_SIZE + movi, 4, "idx1") == 0, "index follows the last frame");
    CHECK(at32(bytes, 32) == 200000, "dwMicroSecPerFrame from the measured duration (600 ms / 3 frames)");
    CHECK(at32(bytes, 132) / double(at32(bytes, 128)) == 5.0, "dwRate/dwScale gives the measured 5 fps");
    CHECK(at32(bytes, 60) == 150 && at32(bytes, 144) == 150, "dwSuggestedBufferSize is the largest frame");
    CHECK(at32(bytes, 160) == (320u | 240u << 16), "rcFrame is the recorded geometry");
    CHECK(at32(bytes, 64) == 320 && at32(bytes, 68) == 240 && at32(bytes, 176) == 320 && at32(bytes, 180) == 240,
          "first-frame geometry in avih and strf");
  } else if (name == "short_idx") {
    CHECK(recordThree(), "three frames recorded");
    fs.faults.shortWriteAfter[kPath] = gAccepted + 4;
    return expectNotSaved(FINALIZE(true, 600), "a short idx1 write fails the clip");
  } else if (name == "failed_seek") {
    CHECK(recordThree(), "three frames recorded");
    fs.faults.seek.add(kPath);
    return expectNotSaved(FINALIZE(true, 600), "a failed finalize seek fails the clip");
  } else if (name == "failed_patch") {
    CHECK(recordThree(), "three frames recorded");
    fs.faults.shortWriteAfter[kPath] = gAccepted + 8 + 3 * 16;  // index complete, first patch short
    return expectNotSaved(FINALIZE(true, 600), "a failed header patch fails the clip");
  } else if (name == "flush_error") {
    CHECK(recordThree(), "three frames recorded");
    fs.faults.flush.add(kPath);
    return expectNotSaved(FINALIZE(true, 600), "a flush error fails the clip");
  } else if (name == "frame_failed") {
    CHECK(recordFrame(100), "first frame recorded");
    fs.faults.shortWriteAfter[kPath] = gAccepted;  // the next chunk write is refused outright
    CHECK(!recordFrame(101), "second frame write fails");
    fs.faults.shortWriteAfter.clear();             // the finalize writes themselves succeed
    return expectNotSaved(FINALIZE(false, 600), "a clip whose frame write failed is not saved");
  } else if (name == "dims_seek" || name == "dims_write") {
#if HAS_DIMS_HELPER
    if (name == "dims_seek") fs.faults.seek.add(kPath);
    else fs.faults.shortWriteAfter[kPath] = AVI_HEADER_SIZE + 4;
    const auto frame = jpeg(320, 240, 100);
    CHECK(!patchFirstFrameDims(frame.data(), frame.size()), "a failed geometry seek or write is reported");
    CHECK(fs.text(kPath).size() == AVI_HEADER_SIZE, "nothing is appended at an unknown offset");
    fs.faults.clear();
    return expectNotSaved(FINALIZE(false, 600), "a clip whose geometry patch failed is not saved");
#else
    CHECK(false, "first-frame geometry patch has no checked helper (seeks and writes unchecked inline)");
#endif
  } else if (name == "failed_start") {
#if HAS_DISCARD_HELPER
    discardFailedStart();
    CHECK(!s_file && !fs.exists(kPath), "a failed start removes the half-made file");
    CHECK(VFS::sdInUseEnds == 1, "a failed start releases the SD registration");
#else
    CHECK(false, "failed starts close the file but leave it on the card (no cleanup helper)");
#endif
  } else {
    CHECK(false, "known scenario");
  }
  (void)s_task;
  std::puts("PASS");
  return 0;
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <scenario>\n", argv[0]);
    return 2;
  }
  return run(argv[1]);
}
