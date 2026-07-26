/**
 * System_Camera_Video — MJPEG-in-AVI recording
 * See System_Camera_Video.h for overview.
 */

#include "System_Camera_Video.h"
#include "System_Events.h"  // systemEventPost — event register producer
#include "System_VFS.h"
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
#include "System_MemUtil.h"
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ── Public state ────────────────────────────────────────────────────────────
bool videoRecording = false;

// Offsets into the 224-byte header skeleton that get patched on finalize.
static const uint32_t AVI_HEADER_SIZE        = 224;
static const uint32_t OFF_RIFF_SIZE          = 4;
static const uint32_t OFF_AVIH_TOTAL_FRAMES  = 48;
static const uint32_t OFF_STRH_LENGTH        = 140;
static const uint32_t OFF_MOVI_SIZE          = 216;

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
// Width/height/fps known up front (from first frame); frame count and size
// fields patched at end.
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

static void writeFrameChunk(const uint8_t* jpeg, size_t len) {
  // Chunk: '00dc' + size(LE) + data + optional pad byte to keep even alignment.
  uint8_t hdr[8];
  put32(hdr + 0, fourcc("00dc"));
  put32(hdr + 4, (uint32_t)len);

  uint32_t offsetInMovi = s_moviPayloadBytes;  // before the chunk header itself

  s_file.write(hdr, 8);
  s_file.write(jpeg, len);
  s_moviPayloadBytes += 8 + len;

  if (len & 1) {
    uint8_t pad = 0;
    s_file.write(&pad, 1);
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
}

static void finalizeAndClose() {
  if (!s_file) return;

  // Write idx1 chunk at end of file.
  if (s_idx && s_frameCount > 0) {
    uint8_t idxHdr[8];
    put32(idxHdr + 0, fourcc("idx1"));
    put32(idxHdr + 4, s_frameCount * 16);
    s_file.write(idxHdr, 8);
    // Each IdxEntry is already 16 bytes in little-endian layout on xtensa.
    s_file.write((const uint8_t*)s_idx, s_frameCount * 16);
  }

  uint32_t fileSize = s_file.size();

  // Patch header fields.
  uint8_t buf[4];

  s_file.seek(OFF_RIFF_SIZE);
  put32(buf, fileSize - 8);
  s_file.write(buf, 4);

  s_file.seek(OFF_AVIH_TOTAL_FRAMES);
  put32(buf, s_frameCount);
  s_file.write(buf, 4);

  s_file.seek(OFF_STRH_LENGTH);
  put32(buf, s_frameCount);
  s_file.write(buf, 4);

  s_file.seek(OFF_MOVI_SIZE);
  // movi LIST size includes the 'movi' fourcc (4) + payload
  put32(buf, 4 + s_moviPayloadBytes);
  s_file.write(buf, 4);

  s_file.flush();
  s_file.close();

  INFO_CAMERA_VIDEOF("Recording finalized: %s (%u frames, %u bytes)",
                s_path, (unsigned)s_frameCount, (unsigned)fileSize);
  {
    const char* slash = strrchr(s_path, '/');
    char det[24];
    snprintf(det, sizeof(det), "%u frames", (unsigned)s_frameCount);
    systemEventPost(SYSEVT_VIDEO_SAVED, slash ? slash + 1 : s_path, det);
  }
}

static void recordingTask(void* /*arg*/) {
  // gSettings.cameraStreamFps is 1..20; convert to ms per frame for the
  // pacing delay. Fall back to ~10 fps if the setting is somehow zero.
  const uint32_t fps = (gSettings.cameraStreamFps > 0)
                         ? (uint32_t)gSettings.cameraStreamFps : 10;
  const uint32_t intervalMs = 1000 / fps;

  while (videoRecording && s_frameCount < MAX_FRAMES) {
    TickType_t loopStart = xTaskGetTickCount();

    size_t len = 0;
    uint8_t* jpeg = captureFrame(&len);
    if (jpeg && len > 0) {
      // On the first frame, patch width/height into the header if we can.
      if (s_frameCount == 0) {
        uint32_t w = 0, h = 0;
        if (probeJpegDims(jpeg, len, &w, &h) && w > 0 && h > 0) {
          s_width = w;
          s_height = h;
          // Rewrite dims in-place (we still haven't written any frames).
          s_file.seek(64);  // avih.dwWidth
          uint8_t dims[8];
          put32(dims + 0, w);
          put32(dims + 4, h);
          s_file.write(dims, 8);
          s_file.seek(176);  // biWidth/biHeight
          s_file.write(dims, 8);
          s_file.seek(AVI_HEADER_SIZE);
        }
      }
      writeFrameChunk(jpeg, len);
      free(jpeg);
    }

    // Pace to target interval; accept drift if writes took longer than budget.
    vTaskDelayUntil(&loopStart, pdMS_TO_TICKS(intervalMs));
  }

  finalizeAndClose();
  videoRecording = false;  // in case we exited via MAX_FRAMES
  s_task = nullptr;
  vTaskDelete(nullptr);
}

// ── Public API ──────────────────────────────────────────────────────────────
bool startVideoRecording() {
  STACK_TRACEF("startVideoRecording.enter");
  extern bool gCameraRunning;
  if (!gCameraRunning) {
    INFO_CAMERA_VIDEOF("Cannot record — camera not enabled");
    STACK_TRACEF("startVideoRecording.camera_disabled_exit");
    return false;
  }
  if (videoRecording) {
    INFO_CAMERA_VIDEOF("Already recording");
    STACK_TRACEF("startVideoRecording.already_recording_exit");
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
    return false;
  }
  if (!ensureVideosFolder()) {
    STACK_TRACEF("startVideoRecording.ensure_folder_failed");
    return false;
  }
  STACK_TRACEF("startVideoRecording.folder_ok");

  snprintf(s_path, sizeof(s_path), "%s/VID_%lu.AVI", VIDEO_FOLDER_SD, (unsigned long)millis());

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
  s_file.write(header, AVI_HEADER_SIZE);
  s_file.flush();

  s_frameCount = 0;
  s_width = 0;
  s_height = 0;
  s_maxFrameBytes = 0;
  s_moviPayloadBytes = 0;
  s_startMs = millis();
  videoRecording = true;

  BaseType_t ok = xTaskCreatePinnedToCore(
    recordingTask, "cam_record", VIDEO_REC_STACK_WORDS, nullptr,
    /*priority*/ 5, &s_task, /*core*/ 1);
  if (ok != pdPASS) {
    ERROR_CAMERAF("[Video] Failed to start recorder task");
    videoRecording = false;
    s_file.close();
    return false;
  }

  INFO_CAMERA_VIDEOF("Recording started: %s", s_path);
  {
    const char* slash = strrchr(s_path, '/');
    systemEventPost(SYSEVT_VIDEO_RECORD_STARTED, slash ? slash + 1 : s_path);
  }
  return true;
}

void stopVideoRecording() {
  if (!videoRecording) return;
  videoRecording = false;
  // Wait for task to finalize; finalizeAndClose runs inside it.
  int waits = 100;
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
int getVideoRecordingCount() {
  if (!VFS::isSDAvailable()) return 0;
  int count = 0;
  File root = VFS::openGuarded(VIDEO_FOLDER_SD, FILE_READ, VFS::systemAuth(VFS::Scopes::VIDEOS, "video.list"));
  if (!root || !root.isDirectory()) return 0;
  File f = root.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      const char* name = f.name();
      size_t n = strlen(name);
      if (n >= 4 && strcasecmp(name + n - 4, ".avi") == 0) count++;
    }
    f = root.openNextFile();
  }
  root.close();
  return count;
}

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

  String raw = getVideoRecordingsList();
  int count = getVideoRecordingCount();

  // Build JSON — list items are newline-separated "name:size".
  String json = "{\"count\":";
  json += count;
  json += ",\"sdAvailable\":";
  json += VFS::isSDAvailable() ? "true" : "false";
  json += ",\"files\":[";

  if (count > 0 && raw.length() > 0) {
    int start = 0;
    bool first = true;
    while (start < (int)raw.length()) {
      int nl = raw.indexOf('\n', start);
      if (nl < 0) nl = raw.length();
      String item = raw.substring(start, nl);
      int colon = item.indexOf(':');
      if (colon > 0) {
        if (!first) json += ",";
        json += "{\"name\":\"";
        json += item.substring(0, colon);
        json += "\",\"size\":";
        json += item.substring(colon + 1);
        json += "}";
        first = false;
      }
      start = nl + 1;
    }
  }
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
  // slurp them whole (unlike mic WAVs which are capped at ~2MB).
  const size_t CHUNK = 8192;
  uint8_t* buf = (uint8_t*)malloc(CHUNK);
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
