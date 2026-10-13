/**
 * System_Camera_Video — MJPEG-in-AVI recording
 * See System_Camera_Video.h for overview.
 */

#include "System_Camera_Video.h"
#include "System_Events.h"  // systemEventPost — event register producer
#include "System_VFS.h"
#include "System_TaskUtils.h"  // taskStackRecord — taskstats stack-size registry
#include "System_MemUtil.h"  // ps_alloc — PSRAM-first HTTP download chunk
#include <strings.h>   // strcasecmp (used by the always-compiled viewer below)

// Exposed to System_I2C.cpp status builder so UI can gate SD-dependent
// features (e.g. the Record button). Defined unconditionally — the sensor
// status JSON always carries `sdAvailable` and `sdWritable`, even when
// camera is not compiled.
bool sdCardIsMountedForStatus() {
  return VFS::isSDAvailable();
}

bool sdCardIsWritableForStatus() {
  return VFS::isSDWritable();
}

// ── Video folder ─────────────────────────────────────────────────────────────
// Shared by the recorder (camera builds) and the always-available viewer
// endpoints further down. Viewing/listing recordings is part of the base
// experience: any board with an SD card can browse and download existing
// videos, even without a camera to record new ones.
static const char* VIDEO_FOLDER_SD = "/sd/VIDEOS";

// HTTP + auth headers for the video viewer endpoints (handleVideoRecording*).
// Compiled whenever the web server is present, independent of the camera, so
// /api/videos and /api/videos/file are always registered.
#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>
#include "WebServer_Utils.h"   // WEB_AUTH_OR_RETURN
#include "WebServer_Server.h"  // makeWebAuthCtx
#include "System_User.h"       // AuthContext, tgRequireAuth
#endif

#if ENABLE_CAMERA_SENSOR

#include "System_Camera_DVP.h"
#include "System_Settings.h"
#include "System_Debug.h"
#include "System_Mutex.h"  // FsLockGuard — serialize SD with mic/sensorlog
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// ── Public state ────────────────────────────────────────────────────────────
bool videoRecording = false;

// Serializes start/stop so G2 stream + web CLI can't double-open an AVI
// or spawn two cam_record tasks (TOCTOU on videoRecording alone).
static SemaphoreHandle_t s_recCtrl = nullptr;
static portMUX_TYPE s_recCtrlInitMux = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t getRecCtrl() {
  if (s_recCtrl) return s_recCtrl;

  // Allocate outside the critical section, then publish exactly one handle.
  // Two first-use callers may both allocate, but they always return the same
  // mutex; the losing candidate is deleted after leaving the spinlock.
  SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
  if (!candidate) return nullptr;

  portENTER_CRITICAL(&s_recCtrlInitMux);
  if (!s_recCtrl) {
    s_recCtrl = candidate;
    candidate = nullptr;
  }
  SemaphoreHandle_t result = s_recCtrl;
  portEXIT_CRITICAL(&s_recCtrlInitMux);

  if (candidate) vSemaphoreDelete(candidate);
  return result;
}

// Offsets into the 224-byte header skeleton that get patched on finalize.
static const uint32_t AVI_HEADER_SIZE        = 224;
static const uint32_t OFF_RIFF_SIZE          = 4;
static const uint32_t OFF_AVIH_US_PER_FRAME  = 32;
static const uint32_t OFF_AVIH_TOTAL_FRAMES  = 48;
static const uint32_t OFF_AVIH_BUFFER_SIZE   = 60;
static const uint32_t OFF_AVIH_WIDTH         = 64;   // dwWidth, dwHeight
static const uint32_t OFF_STRH_SCALE         = 128;  // dwScale, then dwRate
static const uint32_t OFF_STRH_LENGTH        = 140;
static const uint32_t OFF_STRH_BUFFER_SIZE   = 144;
static const uint32_t OFF_STRH_RC_RIGHT      = 160;  // rcFrame.right, rcFrame.bottom (16-bit each)
static const uint32_t OFF_STRF_WIDTH         = 176;  // biWidth, biHeight
static const uint32_t OFF_MOVI_SIZE          = 216;

// SD-in-use registration name: the card stays mounted while a clip is open.
static const char* const VIDEO_SD_OWNER = "video recording";

// Cap on frames per recording (~160 KB of index + whatever the frames cost).
// At 10 fps that's 16+ minutes; at 30 fps about 5.5 minutes. Prevents runaway.
static const uint32_t MAX_FRAMES = 10000;

// Recorder task stack — large JPEGs occasionally get memcpy'd here.
static const uint32_t VIDEO_REC_STACK_WORDS = 6144;

// ── Private state (recorder-owned) ──────────────────────────────────────────
static File s_file;
static char s_path[96] = {0};
static TaskHandle_t s_task = nullptr;

static uint32_t s_frameCount = 0;
static uint32_t s_width = 0;
static uint32_t s_height = 0;
static uint32_t s_maxFrameBytes = 0;
static uint32_t s_moviPayloadBytes = 0;  // bytes of frames+headers under 'movi'
static uint32_t s_startMs = 0;

// Index entries buffered in PSRAM and flushed at finalize.
struct IdxEntry {
  uint32_t ckid;
  uint32_t flags;
  uint32_t offset;  // relative to start of movi payload (i.e. after 'movi' fourcc)
  uint32_t size;
};
static IdxEntry* s_idx = nullptr;
static size_t s_idxCap = 0;

// ── Little-endian write helpers ─────────────────────────────────────────────
static inline void put32(uint8_t* p, uint32_t v) {
  p[0] = v & 0xff;
  p[1] = (v >> 8) & 0xff;
  p[2] = (v >> 16) & 0xff;
  p[3] = (v >> 24) & 0xff;
}
static inline void put16(uint8_t* p, uint16_t v) {
  p[0] = v & 0xff;
  p[1] = (v >> 8) & 0xff;
}
static inline uint32_t fourcc(const char* s) {
  return (uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);
}

// ── AVI header skeleton ─────────────────────────────────────────────────────
// Written once when recording starts, patched in-place on finalize.
// Width/height are patched from the first frame; frame count, sizes, the
// measured frame timing, buffer size and rcFrame are patched at the end.
static void buildHeaderSkeleton(uint8_t* h, uint32_t w, uint32_t hgt, uint32_t fps) {
  memset(h, 0, AVI_HEADER_SIZE);

  const uint32_t usPerFrame = fps ? (1000000UL / fps) : 100000UL;

  // RIFF
  put32(h + 0,  fourcc("RIFF"));
  put32(h + 4,  0);                  // RIFF size — patched
  put32(h + 8,  fourcc("AVI "));

  // LIST hdrl (size = 4 fourcc + avih block 64 + strl LIST 124 = 192)
  put32(h + 12, fourcc("LIST"));
  put32(h + 16, 192);
  put32(h + 20, fourcc("hdrl"));

  // avih
  put32(h + 24, fourcc("avih"));
  put32(h + 28, 56);
  put32(h + 32, usPerFrame);          // dwMicroSecPerFrame
  put32(h + 36, 0);                   // dwMaxBytesPerSec
  put32(h + 40, 0);                   // dwPaddingGranularity
  put32(h + 44, 0x10);                // dwFlags = AVIF_HASINDEX
  put32(h + 48, 0);                   // dwTotalFrames — patched
  put32(h + 52, 0);                   // dwInitialFrames
  put32(h + 56, 1);                   // dwStreams
  put32(h + 60, w * hgt);             // dwSuggestedBufferSize (approx)
  put32(h + 64, w);                   // dwWidth
  put32(h + 68, hgt);                 // dwHeight
  // 16 reserved bytes already zero

  // LIST strl (size = 4 fourcc + strh block 64 + strf block 48 = 116)
  put32(h + 88,  fourcc("LIST"));
  put32(h + 92,  116);
  put32(h + 96,  fourcc("strl"));

  // strh
  put32(h + 100, fourcc("strh"));
  put32(h + 104, 56);
  put32(h + 108, fourcc("vids"));
  put32(h + 112, fourcc("MJPG"));
  put32(h + 116, 0);                  // dwFlags
  put16(h + 120, 0);                  // wPriority
  put16(h + 122, 0);                  // wLanguage
  put32(h + 124, 0);                  // dwInitialFrames
  put32(h + 128, 1);                  // dwScale
  put32(h + 132, fps ? fps : 10);     // dwRate → fps = dwRate/dwScale
  put32(h + 136, 0);                  // dwStart
  put32(h + 140, 0);                  // dwLength — patched
  put32(h + 144, w * hgt);            // dwSuggestedBufferSize
  put32(h + 148, 0xFFFFFFFF);         // dwQuality = -1
  put32(h + 152, 0);                  // dwSampleSize
  put16(h + 156, 0);                  // rcFrame.left
  put16(h + 158, 0);                  // rcFrame.top
  put16(h + 160, (uint16_t)w);        // rcFrame.right
  put16(h + 162, (uint16_t)hgt);      // rcFrame.bottom

  // strf (BITMAPINFOHEADER)
  put32(h + 164, fourcc("strf"));
  put32(h + 168, 40);
  put32(h + 172, 40);                 // biSize
  put32(h + 176, w);                  // biWidth
  put32(h + 180, hgt);                // biHeight
  put16(h + 184, 1);                  // biPlanes
  put16(h + 186, 24);                 // biBitCount
  put32(h + 188, fourcc("MJPG"));     // biCompression
  put32(h + 192, w * hgt * 3);        // biSizeImage

  // LIST movi
  put32(h + 212, fourcc("LIST"));
  put32(h + 216, 0);                  // movi size — patched
  put32(h + 220, fourcc("movi"));
}

// ── JPEG size/dimension probe ───────────────────────────────────────────────
// MJPEG AVI needs baseline W/H in the header. Extract from the first frame's
// SOF0 marker (0xFFC0). Returns false if markers can't be parsed.
static bool probeJpegDims(const uint8_t* jpeg, size_t len, uint32_t* outW, uint32_t* outH) {
  if (len < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) return false;
  size_t i = 2;
  while (i + 9 < len) {
    if (jpeg[i] != 0xFF) return false;
    uint8_t marker = jpeg[i + 1];
    if (marker == 0xD8 || marker == 0xD9) { i += 2; continue; }
    uint16_t segLen = ((uint16_t)jpeg[i + 2] << 8) | jpeg[i + 3];
    // SOF0..SOF3 carry dims. SOF0 (baseline) is the normal case for esp_camera.
    if (marker >= 0xC0 && marker <= 0xC3) {
      *outH = ((uint16_t)jpeg[i + 5] << 8) | jpeg[i + 6];
      *outW = ((uint16_t)jpeg[i + 7] << 8) | jpeg[i + 8];
      return true;
    }
    i += 2 + segLen;
  }
  return false;
}

// ── File helpers ────────────────────────────────────────────────────────────
static bool ensureVideosFolder() {
  if (!VFS::isSDAvailable()) return false;
  // Route through VFS (shared FsLockGuard + permission guard) using the
  // "/sd/..." path form; VFS dispatches /sd to the SD backend internally.
  AuthContext sys = VFS::systemAuth(VFS::Scopes::VIDEOS, "video.ensure_folder");
  if (VFS::existsGuarded(VIDEO_FOLDER_SD, sys)) return true;
  if (VFS::mkdirGuarded(VIDEO_FOLDER_SD, sys)) {
    INFO_STORAGEF("[Video] Created folder on SD: %s", VIDEO_FOLDER_SD);
    return true;
  }
  ERROR_STORAGEF("[Video] Failed to create folder on SD: %s", VIDEO_FOLDER_SD);
  return false;
}

// ── Recorder task ───────────────────────────────────────────────────────────
// captureFrame is declared in System_Camera_DVP.h (included above).

// Returns false on SD write failure. Caller must stop recording — continuing
// would leave frameCount ahead of bytes on disk (unplayable AVI, zeroed
// movi size after a failed finalize seek/write under contention).
static bool writeFrameChunk(const uint8_t* jpeg, size_t len) {
  // Chunk: '00dc' + size(LE) + data + optional pad byte to keep even alignment.
  uint8_t hdr[8];
  put32(hdr + 0, fourcc("00dc"));
  put32(hdr + 4, (uint32_t)len);

  uint32_t offsetInMovi = s_moviPayloadBytes;  // before the chunk header itself

  // Hold the FS mutex for the whole chunk — mic WAV + sensorlog also take
  // FsLockGuard around SD writes. Unlocked concurrent SD access was truncating
  // this AVI mid-frame and preventing header finalize patches from sticking.
  FsLockGuard fsGuard("video.frame.write");

  if (s_file.write(hdr, 8) != 8) return false;
  if (s_file.write(jpeg, len) != len) return false;
  s_moviPayloadBytes += 8 + len;

  if (len & 1) {
    uint8_t pad = 0;
    if (s_file.write(&pad, 1) != 1) return false;
    s_moviPayloadBytes += 1;
  }

  if (s_idx && s_frameCount < s_idxCap) {
    IdxEntry& e = s_idx[s_frameCount];
    e.ckid   = fourcc("00dc");
    e.flags  = 0x10;                 // AVIIF_KEYFRAME
    e.offset = offsetInMovi;         // relative to start of 'movi' payload
    e.size   = (uint32_t)len;
  }

  if (len > s_maxFrameBytes) s_maxFrameBytes = (uint32_t)len;
  s_frameCount++;
  return true;
}

// First frame: write its geometry into the header, then return the handle to
// the start of the movi payload. False when any seek or write fails; frames
// must not be appended then, because the handle position is unknown. A frame
// whose geometry cannot be parsed keeps the skeleton values.
static bool patchFirstFrameDims(const uint8_t* jpeg, size_t len) {
  uint32_t w = 0, h = 0;
  if (!probeJpegDims(jpeg, len, &w, &h) || w == 0 || h == 0) return true;
  FsLockGuard fsGuard("video.dims.patch");
  uint8_t dims[8];
  put32(dims + 0, w);
  put32(dims + 4, h);
  if (!s_file.seek(OFF_AVIH_WIDTH) || s_file.write(dims, 8) != 8 ||
      !s_file.seek(OFF_STRF_WIDTH) || s_file.write(dims, 8) != 8 ||
      !s_file.seek(AVI_HEADER_SIZE)) {
    return false;
  }
  s_width = w;
  s_height = h;
  return true;
}

// Completes the clip and reports the outcome. VIDEO_SAVED is posted only when
// every frame was written (recordedOk) and the index, every header patch, the
// flush and the final size all check out; otherwise the failure is reported to
// VFS as an SD write failure and VIDEO_RECORD_FAILED is posted with the clip
// name instead. The file is kept either way: frames already on the card stay
// recoverable.
// Timing comes from what was recorded: elapsedMs of wall clock over the frames
// written, so a clip that dropped frames still plays in real time.
static bool finalizeAndClose(bool recordedOk, uint32_t elapsedMs) {
  const char* slash = strrchr(s_path, '/');
  const char* name = slash ? slash + 1 : s_path;
  char det[24];
  snprintf(det, sizeof(det), "%u frames", (unsigned)s_frameCount);
  if (!s_file) {
    VFS::noteSDWriteFailure("video recording finalize");
    systemEventPost(SYSEVT_VIDEO_RECORD_FAILED, name, det);
    return false;
  }

  bool ok = recordedOk;
  const bool hasIdx = s_idx && s_frameCount > 0;
  const uint32_t idxBytes = hasIdx ? s_frameCount * 16 : 0;
  const uint32_t fileSize = AVI_HEADER_SIZE + s_moviPayloadBytes + (hasIdx ? 8 + idxBytes : 0);
  {
    FsLockGuard fsGuard("video.finalize");

    // The index goes right after the last complete frame, wherever an earlier
    // failure left the handle.
    bool written = s_file.seek(AVI_HEADER_SIZE + s_moviPayloadBytes);
    if (written && hasIdx) {
      uint8_t idxHdr[8];
      put32(idxHdr + 0, fourcc("idx1"));
      put32(idxHdr + 4, idxBytes);
      // Each IdxEntry is already 16 bytes in little-endian layout on xtensa.
      written = s_file.write(idxHdr, 8) == 8 &&
                s_file.write((const uint8_t*)s_idx, idxBytes) == idxBytes;
    }

    uint8_t buf[4];
    auto patch32 = [&](uint32_t off, uint32_t val) {
      if (!written) return;
      put32(buf, val);
      written = s_file.seek(off) && s_file.write(buf, 4) == 4;
    };

    patch32(OFF_RIFF_SIZE, fileSize - 8);
    patch32(OFF_AVIH_TOTAL_FRAMES, s_frameCount);
    patch32(OFF_STRH_LENGTH, s_frameCount);
    // movi LIST size includes the 'movi' fourcc (4) + payload
    patch32(OFF_MOVI_SIZE, 4 + s_moviPayloadBytes);
    if (s_frameCount > 0 && elapsedMs > 0) {
      uint64_t usPerFrame = (uint64_t)elapsedMs * 1000u / s_frameCount;
      if (usPerFrame == 0) usPerFrame = 1;
      if (usPerFrame > UINT32_MAX) usPerFrame = UINT32_MAX;
      patch32(OFF_AVIH_US_PER_FRAME, (uint32_t)usPerFrame);
      // rate/scale = frames per second
      patch32(OFF_STRH_SCALE, (uint32_t)usPerFrame);
      patch32(OFF_STRH_SCALE + 4, 1000000u);
    }
    if (s_maxFrameBytes > 0) {
      patch32(OFF_AVIH_BUFFER_SIZE, s_maxFrameBytes);
      patch32(OFF_STRH_BUFFER_SIZE, s_maxFrameBytes);
    }
    if (s_width > 0 && s_height > 0) {
      patch32(OFF_STRH_RC_RIGHT, (s_width & 0xffffu) | ((s_height & 0xffffu) << 16));
    }

    s_file.flush();
    if (!written || s_file.getWriteError() || s_file.size() != fileSize) ok = false;
    s_file.close();
  }

  if (!ok) {
    ERROR_CAMERAF("[Video] Recording not saved: %s (%u frames, %u bytes expected)",
                  s_path, (unsigned)s_frameCount, (unsigned)fileSize);
    VFS::noteSDWriteFailure("video recording finalize");
    systemEventPost(SYSEVT_VIDEO_RECORD_FAILED, name, det);
    return false;
  }
  INFO_CAMERA_VIDEOF("Recording finalized: %s (%u frames, %u bytes)",
                     s_path, (unsigned)s_frameCount, (unsigned)fileSize);
  systemEventPost(SYSEVT_VIDEO_SAVED, name, det);
  return true;
}

// A start that fails after the file was created removes the half-made file
// and releases the card.
static void discardFailedStart() {
  s_file.close();
  if (!VFS::removeGuarded(String(s_path),
                          VFS::systemAuth(VFS::Scopes::VIDEOS, "video.discard"))) {
    ERROR_STORAGEF("[Video] Could not remove incomplete recording: %s", s_path);
  }
  VFS::sdInUseEnd(VIDEO_SD_OWNER);
}

static void recordingTask(void* /*arg*/) {
  // gSettings.cameraStreamFps is 1..20; convert to ms per frame for the
  // pacing delay. Fall back to ~10 fps if the setting is somehow zero.
  const uint32_t fps = (gSettings.cameraStreamFps > 0)
                         ? (uint32_t)gSettings.cameraStreamFps : 10;
  const uint32_t intervalMs = 1000 / fps;
  bool recordedOk = true;

  while (videoRecording && s_frameCount < MAX_FRAMES) {
    TickType_t loopStart = xTaskGetTickCount();

    size_t len = 0;
    uint8_t* jpeg = captureFrame(&len);
    if (jpeg && len > 0) {
      // On the first frame, patch width/height into the header if we can.
      if (s_frameCount == 0 && !patchFirstFrameDims(jpeg, len)) {
        ERROR_CAMERAF("[Video] Header geometry patch failed — stopping");
        recordedOk = false;
      } else if (!writeFrameChunk(jpeg, len)) {
        ERROR_CAMERAF("[Video] SD write failed mid-recording at frame %u — stopping",
                      (unsigned)s_frameCount);
        recordedOk = false;
      }
      free(jpeg);
      if (!recordedOk) {
        VFS::noteSDWriteFailure("video recording frame write");
        videoRecording = false;
        break;
      }
    }

    // Pace to target interval; accept drift if writes took longer than budget.
    vTaskDelayUntil(&loopStart, pdMS_TO_TICKS(intervalMs));
  }

  (void)finalizeAndClose(recordedOk, millis() - s_startMs);
  videoRecording = false;  // in case we exited via MAX_FRAMES
  cameraSessionEnd(CameraSessionKind::Recording);
  VFS::sdInUseEnd(VIDEO_SD_OWNER);
  s_task = nullptr;
  vTaskDelete(nullptr);
}

// ── Public API ──────────────────────────────────────────────────────────────
bool startVideoRecording() {
  STACK_TRACEF("startVideoRecording.enter");
  extern bool gCameraRunning;
  SemaphoreHandle_t ctrl = getRecCtrl();
  if (!ctrl) {
    ERROR_CAMERAF("[Video] Cannot create recorder control mutex");
    return false;
  }
  if (xSemaphoreTake(ctrl, pdMS_TO_TICKS(5000)) != pdTRUE) {
    INFO_CAMERA_VIDEOF("Cannot record — recorder control busy");
    return false;
  }

  auto unlock = [&]() {
    xSemaphoreGive(ctrl);
  };

  if (!gCameraRunning) {
    INFO_CAMERA_VIDEOF("Cannot record — camera not enabled");
    STACK_TRACEF("startVideoRecording.camera_disabled_exit");
    unlock();
    return false;
  }
  // videoRecording OR a still-finalizing task — reject a second start.
  if (videoRecording || s_task) {
    INFO_CAMERA_VIDEOF("Already recording");
    STACK_TRACEF("startVideoRecording.already_recording_exit");
    unlock();
    return false;
  }
  // Gate on WRITABLE not just mounted — a card can be mounted but have
  // the underlying SD write fail on the actual file create (flaky card,
  // bad sector, write-protect). isSDWritable() has a lazy re-probe so if the card
  // was briefly glitchy and has since recovered, we'll pick it up.
  const bool sdWritable = VFS::isSDWritable();
  STACK_TRACEF("startVideoRecording.sd_writable=%d (mounted=%d)",
               sdWritable ? 1 : 0, VFS::isSDAvailable() ? 1 : 0);
  if (!sdWritable) {
    INFO_CAMERA_VIDEOF("Cannot record — SD card not writable");
    unlock();
    return false;
  }
  if (!ensureVideosFolder()) {
    STACK_TRACEF("startVideoRecording.ensure_folder_failed");
    unlock();
    return false;
  }
  STACK_TRACEF("startVideoRecording.folder_ok");

  snprintf(s_path, sizeof(s_path), "%s/VID_%lu.AVI", VIDEO_FOLDER_SD, (unsigned long)millis());

  // Hold the card before opening the clip: sdunmount, sdmount and sdformat
  // refuse until the recorder has closed it.
  if (!VFS::sdInUseBegin(VIDEO_SD_OWNER)) {
    ERROR_STORAGEF("[Video] Cannot register SD use");
    unlock();
    return false;
  }

  // Route the recording handle through VFS (shared FsLockGuard + permission
  // guard); VFS dispatches the "/sd/..." path to the SD backend. Reuse s_path
  // (already the /sd form) — this also fixes a latent double-millis() mismatch
  // where the old `bare` buffer recomputed millis() and could disagree with the
  // s_path used in the log/error lines. Per-frame writes below stay on the
  // returned handle (same as LittleFS streaming). create=true: without it the
  // open returns null for a non-existent path even on a writable card.
  STACK_TRACEF("startVideoRecording.before_open path=%s", s_path);
  s_file = VFS::openGuarded(String(s_path), FILE_WRITE,
                           VFS::systemAuth(VFS::Scopes::VIDEOS, "video.record"), /*create=*/true);
  STACK_TRACEF("startVideoRecording.after_open ok=%d", s_file ? 1 : 0);
  if (!s_file) {
    ERROR_STORAGEF("[Video] Failed to create file: %s", s_path);
    STACK_TRACEF("startVideoRecording.sd_open_failed_returning_false");
    // Flag the card as not-writable — next isSDWritable() call will
    // re-probe, giving the UI accurate state without needing a remount.
    VFS::noteSDWriteFailure("video recording file create");
    VFS::sdInUseEnd(VIDEO_SD_OWNER);
    unlock();
    return false;
  }

  // Allocate index buffer in PSRAM.
  if (!s_idx) {
    s_idx = (IdxEntry*)ps_alloc(MAX_FRAMES * sizeof(IdxEntry), AllocPref::PreferPSRAM, "video.idx");
    s_idxCap = s_idx ? MAX_FRAMES : 0;
  }

  // Initial header — dims will be patched on the first frame. FPS taken
  // directly from the persisted camera FPS setting (1-20 range).
  uint32_t fps = (gSettings.cameraStreamFps > 0)
                   ? (uint32_t)gSettings.cameraStreamFps : 10;
  if (fps > 60) fps = 60;

  uint8_t header[AVI_HEADER_SIZE];
  buildHeaderSkeleton(header, 640, 480, fps);  // placeholder dims
  {
    FsLockGuard fsGuard("video.header.write");
    const size_t wrote = s_file.write(header, AVI_HEADER_SIZE);
    s_file.flush();
    if (wrote != AVI_HEADER_SIZE) {
      ERROR_STORAGEF("[Video] Short AVI header write: %u/%u", (unsigned)wrote,
                     (unsigned)AVI_HEADER_SIZE);
      discardFailedStart();
      VFS::noteSDWriteFailure("video header write");
      unlock();
      return false;
    }
  }

  s_frameCount = 0;
  s_width = 0;
  s_height = 0;
  s_maxFrameBytes = 0;
  s_moviPayloadBytes = 0;
  s_startMs = millis();
  videoRecording = true;
  // The recorder task ends this session when it exits.
  cameraSessionBegin(CameraSessionKind::Recording);

  taskStackRecord("cam_record", VIDEO_REC_STACK_WORDS);
  BaseType_t ok = xTaskCreatePinnedToCore(
    recordingTask, "cam_record", VIDEO_REC_STACK_WORDS, nullptr,
    /*priority*/ 5, &s_task, /*core*/ 1);
  if (ok != pdPASS) {
    ERROR_CAMERAF("[Video] Failed to start recorder task");
    videoRecording = false;
    cameraSessionEnd(CameraSessionKind::Recording);
    discardFailedStart();
    s_task = nullptr;
    unlock();
    return false;
  }

  INFO_CAMERA_VIDEOF("Recording started: %s", s_path);
  {
    const char* slash = strrchr(s_path, '/');
    systemEventPost(SYSEVT_VIDEO_RECORD_STARTED, slash ? slash + 1 : s_path);
  }
  unlock();
  return true;
}

void stopVideoRecording() {
  SemaphoreHandle_t ctrl = getRecCtrl();
  if (!ctrl) {
    // startVideoRecording also refuses to proceed without this mutex, so
    // clearing the task's poll flag is the only safe fallback required here.
    videoRecording = false;
    return;
  }
  if (xSemaphoreTake(ctrl, pdMS_TO_TICKS(5000)) != pdTRUE) {
    // Still clear the flag so a stuck ctrl can't leave recording orphaned;
    // the task loop polls videoRecording.
    videoRecording = false;
    return;
  }

  if (!videoRecording && !s_task) {
    xSemaphoreGive(ctrl);
    return;
  }
  videoRecording = false;
  // Release ctrl before waiting so finalize isn't blocked on a nested
  // start attempt that only needs to observe s_task != nullptr.
  xSemaphoreGive(ctrl);

  // Wait for task to finalize; finalizeAndClose runs inside it.
  int waits = 150;  // up to ~3 s for SD flush on large clips
  while (s_task && waits-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// Path + frame count of the most recently finalized recording. Valid until the
// next startVideoRecording() (which resets s_path/s_frameCount). Used for the
// camerarecord-stop confirmation message.
const char* videoLastRecordingPath()   { return s_path; }
uint32_t    videoLastRecordingFrames() { return s_frameCount; }

#endif  // ENABLE_CAMERA_SENSOR — end of recorder (needs the camera)

// ── Listing / delete (base experience — no camera required) ──────────────────
// Reading, enumerating, and serving recordings needs only an SD card, so these
// live outside the camera gate. They return empty/false when SD is unavailable
// or no recordings exist, matching the graceful-degradation pattern used by the
// camera status/frame endpoints.
String getVideoRecordingsList() {
  String out;
  if (!VFS::isSDAvailable()) return out;
  File root = VFS::openGuarded(VIDEO_FOLDER_SD, FILE_READ, VFS::systemAuth(VFS::Scopes::VIDEOS, "video.list"));
  if (!root || !root.isDirectory()) return out;
  File f = root.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      const char* name = f.name();
      size_t n = strlen(name);
      if (n >= 4 && strcasecmp(name + n - 4, ".avi") == 0) {
        if (out.length() > 0) out += "\n";
        out += name;
        out += ":";
        out += String((uint32_t)f.size());
      }
    }
    f = root.openNextFile();
  }
  root.close();
  return out;
}

bool deleteVideoRecording(const String& filename) {
  if (!VFS::isSDAvailable()) return false;
  // Reject paths that try to escape the folder.
  if (filename.indexOf('/') >= 0 || filename.indexOf("..") >= 0) return false;
  String path = String(VIDEO_FOLDER_SD) + "/" + filename;
  return VFS::removeGuarded(path, VFS::systemAuth(VFS::Scopes::VIDEOS, "video.delete"));
}

// ── HTTP viewer endpoints (base experience) ──────────────────────────────────
// Registered unconditionally in WebPage_Sensors.cpp::registerSensorHandlers so
// /api/videos and /api/videos/file exist on every web build — camera or not.
#if ENABLE_HTTP_SERVER
esp_err_t handleVideoRecordingsList(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);

  // Count is a by-product of the single walk, deliberately not a second helper.
  // Enumerating a directory fopen()s and fclose()s every file it visits (~11 ms
  // per entry on this hardware), so a separate counting pass doubles the cost of
  // the whole request. Measured 1218 -> 714 ms over 46 recordings. ESP-IDF's
  // httpd is single-task, so that time blocks every other request, including the
  // camera preview. Do not reintroduce a count-only walk.
  String raw = getVideoRecordingsList();

  // Build the files array first so the count falls out of the same parse.
  // List items are newline-separated "name:size".
  String files = "";
  int count = 0;
  if (raw.length() > 0) {
    int start = 0;
    while (start < (int)raw.length()) {
      int nl = raw.indexOf('\n', start);
      if (nl < 0) nl = raw.length();
      String item = raw.substring(start, nl);
      int colon = item.indexOf(':');
      if (colon > 0) {
        if (count) files += ",";
        files += "{\"name\":\"";
        files += item.substring(0, colon);
        files += "\",\"size\":";
        files += item.substring(colon + 1);
        files += "}";
        count++;
      }
      start = nl + 1;
    }
  }

  String json = "{\"count\":";
  json += count;
  json += ",\"sdAvailable\":";
  json += VFS::isSDAvailable() ? "true" : "false";
  json += ",\"files\":[";
  json += files;
  json += "]}";

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json.c_str(), json.length());
  return ESP_OK;
}

esp_err_t handleVideoRecordingFile(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);

  if (!VFS::isSDAvailable()) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_send(req, "SD card unavailable", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  char query[128];
  char filename[64] = {0};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    httpd_query_key_value(query, "name", filename, sizeof(filename));
  }
  if (strlen(filename) == 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "Missing filename parameter", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  // Prevent directory traversal.
  if (strchr(filename, '/') || strstr(filename, "..")) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "Invalid filename", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  char path[96];
  snprintf(path, sizeof(path), "%s/%s", VIDEO_FOLDER_SD, filename);
  File f = VFS::openGuarded(String(path), FILE_READ, VFS::systemAuth(VFS::Scopes::VIDEOS, "video.serve"));
  if (!f) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_send(req, "Recording not found", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Content-Length is intentionally omitted — httpd_resp_send_chunk uses
  // HTTP/1.1 chunked transfer encoding, which is incompatible with a
  // declared total length. Browsers handle chunked downloads fine.
  httpd_resp_set_type(req, "video/x-msvideo");
  char contentDisp[128];
  snprintf(contentDisp, sizeof(contentDisp), "attachment; filename=\"%s\"", filename);
  httpd_resp_set_hdr(req, "Content-Disposition", contentDisp);
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

  // Stream in 8KB chunks — AVI files can easily exceed PSRAM if we tried to
  // slurp them whole (unlike mic WAVs which are capped at ~2MB). The HTTP
  // server consumes each chunk synchronously, and File::read supports external
  // destinations (using a sector-sized DMA bounce buffer where required), so
  // this staging allocation can prefer PSRAM without changing its lifetime.
  const size_t CHUNK = 8192;
  uint8_t* buf = static_cast<uint8_t*>(
      ps_alloc(CHUNK, AllocPref::PreferPSRAM, "video.http.chunk"));
  if (!buf) {
    f.close();
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_send(req, "Allocation failed", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  while (true) {
    size_t got = f.read(buf, CHUNK);
    if (got == 0) break;
    if (httpd_resp_send_chunk(req, (const char*)buf, got) != ESP_OK) break;
  }
  httpd_resp_send_chunk(req, nullptr, 0);  // end
  free(buf);
  f.close();
  return ESP_OK;
}

#endif  // ENABLE_HTTP_SERVER
