/**
 * Camera Sensor Module - ESP32-S3 DVP Camera Implementation
 * 
 * Supports OV2640, OV3660, and OV5640 cameras on XIAO ESP32S3 Sense
 */

#include "System_Camera_DVP.h"
#include "System_TaskUtils.h"  // I2C_SENSOR_CORE (cam_pwr does a shared-Wire I2C scan in initCamera)
#include "System_Events.h"  // sensor_started/stopped parity for the camera
#include "System_Filesystem.h"  // requireQuotedPath (uniform quoted-path rule)
#include <esp_attr.h>
#include "System_Camera_Video.h"
#include "System_BuildConfig.h"

#if ENABLE_CAMERA_SENSOR

#include <Arduino.h>
#include <Wire.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <esp_heap_caps.h>
#include "esp_camera.h"
#include "sdkconfig.h"
#include "System_Debug.h"
#include "System_RamFlush.h"
#include "System_MemUtil.h"
#include "System_Command.h"
#include "System_Settings.h"
#include "System_I2C.h"
#include "System_Utils.h"   // argWantsJson
#include <ArduinoJson.h>
#include <atomic>

static SemaphoreHandle_t gCameraMutex = nullptr;

static SemaphoreHandle_t getCameraMutex() {
  if (!gCameraMutex) {
    gCameraMutex = xSemaphoreCreateRecursiveMutex();
  }
  return gCameraMutex;
}

static bool lockCameraMutex(uint32_t timeoutMs) {
  SemaphoreHandle_t m = getCameraMutex();
  if (!m) return true;
  TickType_t to = (timeoutMs == 0) ? 0 : pdMS_TO_TICKS(timeoutMs);
  return xSemaphoreTakeRecursive(m, to) == pdTRUE;
}

static void unlockCameraMutex() {
  SemaphoreHandle_t m = getCameraMutex();
  if (!m) return;
  xSemaphoreGiveRecursive(m);
}

// Helper to decode esp_camera_init error codes
// Note: Camera-specific errors use ESP_ERR_CAMERA_BASE (0x20000) in esp32-camera
static const char* cameraErrorToString(esp_err_t err) {
  switch (err) {
    case ESP_OK: return "OK";
    case ESP_ERR_NO_MEM: return "NO_MEM - Out of memory";
    case ESP_ERR_INVALID_ARG: return "INVALID_ARG - Invalid argument";
    case ESP_ERR_INVALID_STATE: return "INVALID_STATE - Invalid state (or camera not detected)";
    case ESP_ERR_NOT_FOUND: return "NOT_FOUND - Camera not detected on SCCB";
    case ESP_ERR_NOT_SUPPORTED: return "NOT_SUPPORTED - Operation not supported";
    case ESP_ERR_TIMEOUT: return "TIMEOUT - Operation timed out";
    case ESP_FAIL: return "FAIL - General failure";
    // Camera-specific errors (ESP_ERR_CAMERA_BASE = 0x20000)
    case 0x20001: return "ESP_ERR_CAMERA_NOT_DETECTED - Camera not found on SCCB";
    case 0x20002: return "ESP_ERR_CAMERA_FAILED_TO_SET_FRAME_SIZE - Frame size error";
    case 0x20003: return "ESP_ERR_CAMERA_FAILED_TO_SET_OUT_FORMAT - Output format error";
    default: return "Unknown error";
  }
}

// Camera state
bool gCameraRunning = false;
bool cameraConnected = false;
bool cameraStreaming = false;
// Sticky hardware-presence latch, distinct from cameraConnected (a runtime
// init flag that goes false on every stopCamera). Set once the SCCB probe in
// initCamera gets a sensor handle back, and never cleared — so a stopped
// camera still reports "silicon is present here", which is what callers need
// to tell "not started" apart from "no camera on this board".
bool cameraDetected = false;
const char* cameraModel = "Unknown";
int cameraWidth = 0;
int cameraHeight = 0;

static framesize_t cameraFramesizeFromSetting(int v) {
  // Indices 0..5 are the original "confirmed working" sizes — kept in
  // their existing slots so saved settings don't shift. Indices 6..10
  // append the sub-QVGA sizes added later (Apr 2026) for the lens
  // viewer and ESP-NOW thumbnail use cases.
  static const framesize_t kMap[] = {
    FRAMESIZE_QVGA,     // 0 (320x240)
    FRAMESIZE_VGA,      // 1 (640x480)
    FRAMESIZE_SVGA,     // 2 (800x600)
    FRAMESIZE_XGA,      // 3 (1024x768)
    FRAMESIZE_SXGA,     // 4 (1280x1024)
    FRAMESIZE_UXGA,     // 5 (1600x1200)
    FRAMESIZE_96X96,    // 6  (96x96)
    FRAMESIZE_QQVGA,    // 7  (160x120)
    FRAMESIZE_QCIF,     // 8  (176x144)
    FRAMESIZE_HQVGA,    // 9  (240x176)
    FRAMESIZE_240X240,  // 10 (240x240)
  };

  if (v >= 0 && v < (int)(sizeof(kMap) / sizeof(kMap[0]))) {
    return kMap[v];
  }

  // Default to VGA if invalid
  return FRAMESIZE_VGA;
}

static int cameraFramesizeSettingFromEnum(framesize_t fs) {
  // Match the order in cameraFramesizeFromSetting. New small sizes
  // (6..10) are appended; existing 0..5 unchanged.
  switch (fs) {
    case FRAMESIZE_QVGA:    return 0;
    case FRAMESIZE_VGA:     return 1;
    case FRAMESIZE_SVGA:    return 2;
    case FRAMESIZE_XGA:     return 3;
    case FRAMESIZE_SXGA:    return 4;
    case FRAMESIZE_UXGA:    return 5;
    case FRAMESIZE_96X96:   return 6;
    case FRAMESIZE_QQVGA:   return 7;
    case FRAMESIZE_QCIF:    return 8;
    case FRAMESIZE_HQVGA:   return 9;
    case FRAMESIZE_240X240: return 10;
    default: return 1;  // Default to VGA
  }
}

static void cameraDimsForFramesize(framesize_t fs, int& w, int& h) {
  switch (fs) {
    case FRAMESIZE_QVGA:    w = 320;  h = 240; break;
    case FRAMESIZE_VGA:     w = 640;  h = 480; break;
    case FRAMESIZE_SVGA:    w = 800;  h = 600; break;
    case FRAMESIZE_XGA:     w = 1024; h = 768; break;
    case FRAMESIZE_SXGA:    w = 1280; h = 1024; break;
    case FRAMESIZE_UXGA:    w = 1600; h = 1200; break;
    case FRAMESIZE_96X96:   w = 96;   h = 96;  break;
    case FRAMESIZE_QQVGA:   w = 160;  h = 120; break;
    case FRAMESIZE_QCIF:    w = 176;  h = 144; break;
    case FRAMESIZE_HQVGA:   w = 240;  h = 176; break;
    case FRAMESIZE_240X240: w = 240;  h = 240; break;
    default:                w = 640;  h = 480; break;
  }
}

// XIAO ESP32S3 Sense camera pins (directly on expansion board)
// These match the Seeed documentation for OV2640/OV3660/OV5640
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40  // Camera I2C SDA
#define SIOC_GPIO_NUM     39  // Camera I2C SCL

#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

// Static buffer for status JSON
static char* cameraStatusBuffer = nullptr;
static const size_t kStatusBufSize = 512;
// User-facing power requests own the desired state independently of the
// driver's transient gCameraRunning flag. Inline frame recovery checks this
// latch before and after re-init so a queued STOP cannot be undone even when
// the worker's bounded camera-mutex take expires behind a stuck frame fetch.
static std::atomic<bool> sCameraDesiredOn{false};

static bool stopCameraInternal(bool isRecovery);

bool initCamera(bool isRecovery) {
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] ========== initCamera() ENTRY ==========");
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] gCameraRunning=%d cameraConnected=%d", gCameraRunning, cameraConnected);
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Heap free: %u, PSRAM free: %u", esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  if (!lockCameraMutex(15000)) {
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] ERROR: camera mutex timeout (camera busy)");
    return false;
  }
  
  if (gCameraRunning) {
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Already initialized - returning true");
    INFO_CAMERAF("Already initialized");
    unlockCameraMutex();
    return true;
  }

  if (isRecovery && !sCameraDesiredOn) {
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Recovery cancelled by desired-off latch");
    unlockCameraMutex();
    return false;
  }

  // ---------------------------------------------------------------------
  // Boot-safety guard for camera framesize.
  // ---------------------------------------------------------------------
  // Setting index 6 = FRAMESIZE_96X96. On the OV3660 (and likely other
  // OV-family sensors) this produces JPEG output that consistently
  // exceeds the auto-sized frame buffer (~1843 B per esp32-camera's
  // internal estimate), causing a permanent FB-OVF storm. The flush
  // loop further down calls esp_camera_fb_get() which blocks on an
  // internal queue indefinitely when no complete frame ever arrives,
  // so a device with this setting persisted bricks at boot.
  //
  // Detected and fixed 2026-05-01 after a user got stuck in the boot
  // hang. Until we can size the frame buffer correctly for sub-QVGA
  // JPEG (or move to a different pixel format for the lens viewer),
  // revert this specific value to QVGA at boot and persist so the
  // recovery is sticky across reboots.
  //
  // Other sub-QVGA sizes (QQVGA, QCIF, HQVGA, 240x240) haven't shown
  // the same lockup in field testing — they may produce smaller JPEG
  // bitstreams or bigger auto buffers. Leave them alone for now.
  if (gSettings.cameraFramesize == 6) {
    BROADCAST_PRINTF("[CAM_INIT] WARN: framesize=6 (96x96) is unsupported "
                     "on this sensor (FB-OVF) — reverting to QVGA and persisting");
    setSetting(gSettings.cameraFramesize, 0);
  }

  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Starting initialization...");
  INFO_CAMERAF("Initializing camera...");
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] gSettings: framesize=%d quality=%d brightness=%d contrast=%d saturation=%d",
                gSettings.cameraFramesize, gSettings.cameraQuality,
                gSettings.cameraBrightness, gSettings.cameraContrast, gSettings.cameraSaturation);
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] gSettings: hmirror=%d vflip=%d aeLevel=%d",
                gSettings.cameraHMirror, gSettings.cameraVFlip, gSettings.cameraAELevel);
  INFO_CAMERAF("Settings from gSettings: framesize=%d quality=%d brightness=%d contrast=%d",
                gSettings.cameraFramesize, gSettings.cameraQuality,
                gSettings.cameraBrightness, gSettings.cameraContrast);

  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Creating camera_config_t struct...");
  camera_config_t config;
  memset(&config, 0, sizeof(config));  // Zero-initialize for safety
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] config struct zeroed, size=%u bytes", sizeof(config));
  
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] LEDC: channel=%d timer=%d", config.ledc_channel, config.ledc_timer);
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] GPIO pins configured:");
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT]   D0-D7: %d %d %d %d %d %d %d %d", 
                config.pin_d0, config.pin_d1, config.pin_d2, config.pin_d3,
                config.pin_d4, config.pin_d5, config.pin_d6, config.pin_d7);
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT]   XCLK=%d PCLK=%d VSYNC=%d HREF=%d",
                config.pin_xclk, config.pin_pclk, config.pin_vsync, config.pin_href);
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT]   SDA=%d SCL=%d PWDN=%d RESET=%d",
                config.pin_sccb_sda, config.pin_sccb_scl, config.pin_pwdn, config.pin_reset);
  
  // === DEBUG: Log GPIO states before init ===
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] === GPIO STATE CHECK (before init) ===");
  // Data pins
  for (int i = 0; i < 8; i++) {
    int pins[] = {config.pin_d0, config.pin_d1, config.pin_d2, config.pin_d3,
                  config.pin_d4, config.pin_d5, config.pin_d6, config.pin_d7};
    if (pins[i] >= 0) {
      DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] GPIO D%d (pin %d): level=%d", i, pins[i], gpio_get_level((gpio_num_t)pins[i]));
    }
  }
  // Control pins
  if (config.pin_xclk >= 0) DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] GPIO XCLK (pin %d): configured for LEDC output", config.pin_xclk);
  if (config.pin_pclk >= 0) DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] GPIO PCLK (pin %d): level=%d", config.pin_pclk, gpio_get_level((gpio_num_t)config.pin_pclk));
  if (config.pin_vsync >= 0) DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] GPIO VSYNC (pin %d): level=%d", config.pin_vsync, gpio_get_level((gpio_num_t)config.pin_vsync));
  if (config.pin_href >= 0) DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] GPIO HREF (pin %d): level=%d", config.pin_href, gpio_get_level((gpio_num_t)config.pin_href));
  if (config.pin_sccb_sda >= 0) DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] GPIO SDA (pin %d): level=%d", config.pin_sccb_sda, gpio_get_level((gpio_num_t)config.pin_sccb_sda));
  if (config.pin_sccb_scl >= 0) DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] GPIO SCL (pin %d): level=%d", config.pin_sccb_scl, gpio_get_level((gpio_num_t)config.pin_sccb_scl));
  if (config.pin_pwdn >= 0) DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] GPIO PWDN (pin %d): level=%d", config.pin_pwdn, gpio_get_level((gpio_num_t)config.pin_pwdn));
  if (config.pin_reset >= 0) DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] GPIO RESET (pin %d): level=%d", config.pin_reset, gpio_get_level((gpio_num_t)config.pin_reset));

  // === DEBUG: Manual power/reset sequence with timing ===
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] === POWER/RESET SEQUENCE ===");
  if (config.pin_pwdn >= 0) {
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Toggling PWDN pin %d: HIGH (power down)...", config.pin_pwdn);
    gpio_set_direction((gpio_num_t)config.pin_pwdn, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)config.pin_pwdn, 1);  // Power down
    vTaskDelay(pdMS_TO_TICKS(10));
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] PWDN pin %d: LOW (power up)...", config.pin_pwdn);
    gpio_set_level((gpio_num_t)config.pin_pwdn, 0);  // Power up
    vTaskDelay(pdMS_TO_TICKS(10));
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] PWDN sequence complete, level now=%d", gpio_get_level((gpio_num_t)config.pin_pwdn));
  }
  if (config.pin_reset >= 0) {
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Toggling RESET pin %d: LOW (reset active)...", config.pin_reset);
    gpio_set_direction((gpio_num_t)config.pin_reset, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)config.pin_reset, 0);  // Reset active
    vTaskDelay(pdMS_TO_TICKS(10));
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] RESET pin %d: HIGH (reset released)...", config.pin_reset);
    gpio_set_level((gpio_num_t)config.pin_reset, 1);  // Reset released
    vTaskDelay(pdMS_TO_TICKS(10));
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] RESET sequence complete, level now=%d", gpio_get_level((gpio_num_t)config.pin_reset));
  }
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Waiting 100ms for camera to stabilize after power/reset...");
  vTaskDelay(pdMS_TO_TICKS(100));

  // === DEBUG: SCCB/I2C Probe for camera ===
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] === SCCB/I2C PROBE ===");
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Probing for camera on SCCB bus (SDA=%d, SCL=%d)...", config.pin_sccb_sda, config.pin_sccb_scl);
  // Common OV camera I2C addresses: 0x30 (OV2640 write), 0x3C (OV3660/OV5640 write)
  // We'll try a simple I2C scan using Wire
  Wire.begin(config.pin_sccb_sda, config.pin_sccb_scl, 100000);  // 100kHz for SCCB
  uint8_t camAddrs[] = {0x30, 0x3C, 0x21, 0x1E};  // Common camera addresses
  bool foundCam = false;
  for (int i = 0; i < sizeof(camAddrs); i++) {
    Wire.beginTransmission(camAddrs[i]);
    uint8_t err = Wire.endTransmission();
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] SCCB probe 0x%02X: %s", camAddrs[i], err == 0 ? "FOUND!" : (err == 2 ? "NACK" : "Error"));
    if (err == 0) foundCam = true;
  }
  Wire.end();  // Release I2C for camera driver
  if (!foundCam) {
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] *** WARNING: No camera found on SCCB bus! Check connections! ***");
    INFO_CAMERAF("WARNING: No camera detected on I2C bus!");
  }

  // Start with conservative defaults - OV3660 is sensitive
  framesize_t fs = cameraFramesizeFromSetting(gSettings.cameraFramesize);
  // Hard cap at VGA (640×480) regardless of user setting. Larger frame
  // sizes (SVGA/XGA/UXGA) require ~60 KB+ contiguous DMA + frame buffers,
  // which is fragile under our load (BLE central + WiFi + HTTP all
  // contending for memory). VGA's JPEG output is ~25–40 KB, fits comfortably
  // in PSRAM, and reduces camera DMA bandwidth ~40% vs SVGA. If a user
  // really needs higher resolution, raise this manually after measuring
  // peak heap under the actual load they care about.
  if (fs > FRAMESIZE_VGA) {
    INFO_CAMERAF("Requested framesize=%d capped to VGA (memory-pressure guard)", (int)fs);
    fs = FRAMESIZE_VGA;
  }
  int jpegQ = gSettings.cameraQuality;
  // Clamp quality: 0 means "unset", use 10 as minimum for stability
  if (jpegQ < 10) jpegQ = 10;
  if (jpegQ > 63) jpegQ = 63;

  config.xclk_freq_hz = 20000000;  // 20MHz - standard for ESP32-CAM
  config.frame_size = fs;
  config.pixel_format = PIXFORMAT_JPEG;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = jpegQ;
  config.fb_count = 1;  // Start with 1, increase if PSRAM available
  // CAMERA_GRAB_LATEST: fb_get() returns the most recently captured frame;
  // older unread frames are silently overwritten by the sensor task.
  //
  // Was CAMERA_GRAB_WHEN_EMPTY until 2026-05-10. WHEN_EMPTY treats the fb
  // ring as a FIFO — the sensor stalls when full (cam_hal: FB-OVF) and
  // fb_get() returns the OLDEST queued frame. With fb_count=2 and a slow
  // consumer (e.g. G2 BLE stream draining one frame every ~1.5 s at
  // 288×144), every captured frame was already ~1.5 s old before we
  // started the BLE push, giving a ~3 s end-to-end preview latency.
  //
  // LATEST means streaming consumers (G2 lens, web MJPEG, snapshots) get
  // current frames at the cost of dropping old unread ones — the right
  // trade for live preview. Recording (System_Camera_Video.cpp recordingTask)
  // also benefits: when disk I/O hiccups, LATEST drops stale frames cleanly
  // instead of stalling the sensor and producing chunked playback. The
  // "frame-perfect contiguous capture" semantics of WHEN_EMPTY only matter
  // for use cases this firmware doesn't have (high-fps motion analysis).
  config.grab_mode = CAMERA_GRAB_LATEST;
  
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Initial config: xclk=%dHz fs=%d pix=%d fb_loc=%d qual=%d fb_cnt=%d grab=%d",
                config.xclk_freq_hz, config.frame_size, config.pixel_format,
                config.fb_location, config.jpeg_quality, config.fb_count, config.grab_mode);
  
  // menuconfig: Component config → Camera → "Enable PSRAM DMA mode by default".
  // When disabled, CAM DMA does not target PSRAM; framebuffers must live in DRAM.
#if CONFIG_CAMERA_PSRAM_DMA
  const bool camPsramDma = true;
#else
  const bool camPsramDma = false;
#endif

  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Checking PSRAM...");
  bool hasPsram = psramFound();
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] psramFound()=%d CONFIG_CAMERA_PSRAM_DMA=%d",
                hasPsram ? 1 : 0, camPsramDma ? 1 : 0);

  if (hasPsram && camPsramDma) {
    config.jpeg_quality = 10;  // Higher quality when PSRAM DMA available
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] PSRAM + PSRAM-DMA — quality=10, fb_count=2, FB in PSRAM");
  } else if (hasPsram && !camPsramDma) {
    // PSRAM_DMA disabled in menuconfig: the camera DMA peripheral writes
    // to internal DRAM line buffers (avoids PSRAM-bus contention during
    // DMA), and the driver memcpy's each completed frame into the FB.
    //
    // We KEEP the FB in PSRAM (don't force DRAM) for two reasons:
    //   1) Internal DRAM is fragmented under our load (BLE + WiFi + HTTP
    //      coexist). A 60 KB contiguous alloc routinely fails even when
    //      total free DRAM is 140+ KB. Forcing FB to DRAM caused
    //      `cam_dma_config: frame buffer malloc failed` on init.
    //   2) PSRAM has 8 MB free — trivially fits the FB.
    //
    // Net trade vs PSRAM_DMA=on:
    //   - Less peak PSRAM bandwidth during DMA → fewer NO-SOI/NO-EOI
    //     truncation errors when WiFi/BT are also touching PSRAM.
    //   - Slightly more CPU (one memcpy per frame, ~60 KB at PSRAM speed).
    //   - Frame rate loss is small at SVGA and below.
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = jpegQ;
    config.fb_count = 1;  // Single buffer — DMA→DRAM→FB pipeline is serial.
    config.grab_mode = CAMERA_GRAB_LATEST;
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] PSRAM_DMA disabled — DMA→DRAM line buf, FB in PSRAM, fb_count=1, jpeg_quality=%d", jpegQ);
  } else {
    // No PSRAM chip — use internal DRAM. Resolution is already capped
    // at VGA above, but if a future change raises that cap and PSRAM
    // is absent, drop to QVGA so the FB fits in DRAM.
    if (config.frame_size > FRAMESIZE_QVGA) {
      config.frame_size = FRAMESIZE_QVGA;
      INFO_CAMERAF("No PSRAM detected, limiting to QVGA resolution");
    }
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_LATEST;
  }

  INFO_CAMERAF("Config: xclk=%dMHz framesize=%d quality=%d fb_count=%d",
                config.xclk_freq_hz / 1000000, config.frame_size, 
                config.jpeg_quality, config.fb_count);

  // Initialize the camera
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Final config before esp_camera_init():");
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT]   xclk_freq_hz=%d", config.xclk_freq_hz);
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT]   frame_size=%d pixel_format=%d", config.frame_size, config.pixel_format);
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT]   fb_location=%d jpeg_quality=%d", config.fb_location, config.jpeg_quality);
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT]   fb_count=%d grab_mode=%d", config.fb_count, config.grab_mode);
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Heap before esp_camera_init: %u", esp_get_free_heap_size());
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Calling esp_camera_init()...");
  
  unsigned long initStart = millis();
  esp_err_t err = esp_camera_init(&config);
  unsigned long initTime = millis() - initStart;
  
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] esp_camera_init() returned 0x%x after %lu ms", err, initTime);
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Error decode: %s", cameraErrorToString(err));
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Heap after esp_camera_init: %u", esp_get_free_heap_size());
  
  if (err != ESP_OK) {
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] *** INIT FAILED! ***");
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Error code: 0x%x", err);
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Error meaning: %s", cameraErrorToString(err));
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Possible causes:");
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_STATE || err == 0x20001) {
      DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT]   - Camera not connected or bad ribbon cable");
      DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT]   - SCCB/I2C communication failed");
      DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT]   - Wrong I2C address for camera model");
      DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT]   - PWDN/RESET pins not configured correctly");
    } else if (err == ESP_ERR_NO_MEM) {
      DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT]   - Not enough memory for frame buffers");
      DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT]   - Try reducing resolution or fb_count");
    } else if (err == ESP_ERR_TIMEOUT) {
      DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT]   - Camera not responding (check XCLK)");
      DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT]   - DVP timing issue");
    }
    INFO_CAMERAF("Init failed: 0x%x (%s)", err, cameraErrorToString(err));
    cameraConnected = false;
    gCameraRunning = false;
    unlockCameraMutex();
    logSystemEvent("CAM", "camera init FAILED: 0x%x (%s)", err, cameraErrorToString(err));
    systemEventPost(SYSEVT_SENSOR_START_FAILED, "Camera", cameraErrorToString(err));
    return false;
  }
  
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] esp_camera_init() SUCCESS");
  INFO_CAMERAF("esp_camera_init() succeeded");

  // Get camera sensor info
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Getting camera sensor handle...");
  sensor_t* s = esp_camera_sensor_get();
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] esp_camera_sensor_get() returned %p", s);
  
  if (s) {
    // A sensor handle means the SCCB probe got an answer — real evidence of
    // silicon, independent of whether the model is one we recognize below.
    cameraDetected = true;
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Sensor info: PID=0x%x VER=0x%x MIDL=0x%x MIDH=0x%x",
                  s->id.PID, s->id.VER, s->id.MIDL, s->id.MIDH);
    INFO_CAMERAF("Sensor PID=0x%x", s->id.PID);
    switch (s->id.PID) {
      case OV2640_PID:
        cameraModel = "OV2640";
        break;
      case OV3660_PID:
        cameraModel = "OV3660";
        break;
      case OV5640_PID:
        cameraModel = "OV5640";
        break;
      default:
        cameraModel = "Unknown";
        break;
    }
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Detected camera model: %s", cameraModel);
    INFO_CAMERAF("Detected: %s", cameraModel);
    
    // OV3660 specific: needs time to stabilize before changing settings
    if (s->id.PID == OV3660_PID) {
      DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] OV3660 detected - waiting 500ms for stabilization");
      INFO_CAMERAF("OV3660 detected - waiting 500ms for sensor stabilization...");
      vTaskDelay(pdMS_TO_TICKS(500));
      DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] OV3660 stabilization wait complete");
    }
    
    // Flush any garbage frames BEFORE applying settings
    // OV3660 needs more flushes to clear overflow state
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Starting frame flush phase...");
    INFO_CAMERAF("Flushing initial frames...");
    int flushCount = (s->id.PID == OV3660_PID) ? 5 : 3;
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Will flush %d frames", flushCount);
    
    for (int i = 0; i < flushCount; i++) {
      DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Flush %d: calling esp_camera_fb_get()...", i);
      unsigned long flushStart = millis();
      camera_fb_t* fb = esp_camera_fb_get();
      unsigned long flushTime = millis() - flushStart;
      
      if (fb) {
        DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Flush %d: got frame in %lu ms - len=%u format=%d w=%u h=%u",
                      i, flushTime, fb->len, fb->format, fb->width, fb->height);
        if (fb->format == PIXFORMAT_JPEG && fb->len >= 2) {
          DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Flush %d: JPEG header bytes: 0x%02X 0x%02X",
                        i, fb->buf[0], fb->buf[1]);
        }
        INFO_CAMERAF("Flush frame %d: %u bytes, format=%d", i, fb->len, fb->format);
        esp_camera_fb_return(fb);
        DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Flush %d: frame returned to camera", i);
      } else {
        DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Flush %d: TIMEOUT after %lu ms - fb is NULL!", i, flushTime);
        INFO_CAMERAF("Flush frame %d: NULL (timeout)", i);
        // Don't break - keep trying to clear overflow
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Frame flush phase complete");
    
    // NOW apply user settings (after camera has stabilized)
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Applying user settings phase...");
    INFO_CAMERAF("Applying user settings...");
    
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] set_framesize(%d)...", fs);
    int r1 = s->set_framesize(s, fs);
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] set_framesize returned %d", r1);
    
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] set_quality(%d)...", jpegQ);
    int r2 = s->set_quality(s, jpegQ);
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] set_quality returned %d", r2);
    
    // Order: contrast → brightness → saturation. Mirrors the camerafx
    // command's order, which the user validated empirically as the
    // best-looking combination on OV3660. ESPHome bug #5499 reports each
    // call clears the others' enable bits on this sensor block; calling
    // them back-to-back here in this order matches what the user found
    // produces the "juiced" look they want as the default.
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] set_contrast(%d)...", gSettings.cameraContrast);
    s->set_contrast(s, gSettings.cameraContrast);

    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] set_brightness(%d)...", gSettings.cameraBrightness);
    s->set_brightness(s, gSettings.cameraBrightness);

    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] set_saturation(%d)...", gSettings.cameraSaturation);
    s->set_saturation(s, gSettings.cameraSaturation);
    
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] set_hmirror(%d)...", gSettings.cameraHMirror ? 1 : 0);
    s->set_hmirror(s, gSettings.cameraHMirror ? 1 : 0);
    
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] set_vflip(%d)...", gSettings.cameraVFlip ? 1 : 0);
    s->set_vflip(s, gSettings.cameraVFlip ? 1 : 0);
    
    // Standard settings
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Applying standard settings (AWB, AE, gain, etc.)...");
    s->set_special_effect(s, gSettings.cameraSpecialEffect);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, gSettings.cameraWBMode);
    if (s->set_sharpness) s->set_sharpness(s, gSettings.cameraSharpness);
    if (s->set_denoise) s->set_denoise(s, gSettings.cameraDenoise);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1);  // Alt AEC algorithm — user-validated as part of "juiced" defaults.
    s->set_ae_level(s, gSettings.cameraAELevel);  // Apply saved exposure compensation
    s->set_gain_ctrl(s, 1);
    s->set_agc_gain(s, 0);
    // GAINCEILING_128X (=6). The previous default of 2X starved AGC under
    // typical indoor light → image stayed dim → colors looked washed out
    // because chroma channels quantized to small numbers near the noise
    // floor. Higher ceiling lets AEC actually expose the scene, which is
    // a prerequisite for any of the saturation/contrast knobs to register.
    s->set_gainceiling(s, (gainceiling_t)6);
    s->set_bpc(s, 0);
    s->set_wpc(s, 1);
    s->set_raw_gma(s, 1);
    s->set_lenc(s, 1);
    s->set_dcw(s, 1);
    s->set_colorbar(s, 0);
    
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] All sensor settings applied");
    INFO_CAMERAF("Settings applied: brightness=%d contrast=%d saturation=%d hmirror=%d vflip=%d",
                  gSettings.cameraBrightness, gSettings.cameraContrast, 
                  gSettings.cameraSaturation, gSettings.cameraHMirror, gSettings.cameraVFlip);
    
    // OV3660: Flush frames AFTER changing settings to clear stale buffers
    // This prevents FB-OVF when resolution was changed
    if (s->id.PID == OV3660_PID) {
      DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] OV3660 post-settings flush starting...");
      vTaskDelay(pdMS_TO_TICKS(100));  // Let new settings take effect
      for (int i = 0; i < 3; i++) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb) {
          DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Post-flush %d: %u bytes %ux%u", i, fb->len, fb->width, fb->height);
          esp_camera_fb_return(fb);
        } else {
          DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Post-flush %d: NULL", i);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
      }
      DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] OV3660 post-settings flush complete");
    }
  } else {
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] WARNING: sensor handle is NULL!");
    INFO_CAMERAF("WARNING: esp_camera_sensor_get() returned NULL!");
  }

  // Set dimensions from the canonical helper so new sizes added to
  // the kMap (cameraFramesizeFromSetting) and dim table flow here
  // automatically.
  cameraDimsForFramesize(fs, cameraWidth, cameraHeight);

  cameraConnected = true;
  gCameraRunning = true;
  // A STOP may have timed out waiting for this recovery-held recursive mutex.
  // Do not publish a resurrected camera; tear it down while we still own the
  // lock. stopCameraInternal(true) is recursive and suppresses user events.
  if (isRecovery && !sCameraDesiredOn) {
    DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Recovery completed after desired-off; deinitializing");
    (void)stopCameraInternal(/*isRecovery=*/true);
    unlockCameraMutex();
    return false;
  }
  sensorStatusBumpWith("opencamera");

  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] ========== initCamera() COMPLETE ==========");
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] gCameraRunning=%d cameraConnected=%d", gCameraRunning, cameraConnected);
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Model=%s Resolution=%dx%d", cameraModel, cameraWidth, cameraHeight);
  DEBUG_CAMERA_LIFECYCLEF("[CAM_INIT] Final heap: %u, PSRAM: %u", esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  INFO_CAMERAF("Initialized: %s (%dx%d)", cameraModel, cameraWidth, cameraHeight);
  unlockCameraMutex();
  // Durable lifecycle event — skipped for per-frame recovery re-inits (isRecovery)
  // so a glitchy camera doesn't log "online" every few seconds during a stream.
  // A NULL sensor handle means esp_camera_init() succeeded but sensor config was
  // skipped (framesize/quality/format not applied) — report that honestly rather
  // than claiming a clean start.
  if (!isRecovery) {
    if (s) {
      logSystemEvent("CAM", "camera online: %s (%dx%d)", cameraModel, cameraWidth, cameraHeight);
    } else {
      logSystemEvent("CAM", "camera online but DEGRADED — sensor handle NULL, config not applied (%dx%d)",
                     cameraWidth, cameraHeight);
    }
    // Camera never routes through the I2C sensor queue, so the existing
    // sensor_started/stopped kinds miss it — post directly for parity.
    systemEventPost(SYSEVT_SENSOR_STARTED, "Camera", cameraModel);
  }
  return true;
}

static bool stopCameraInternal(bool isRecovery) {
  DEBUG_CAMERA_LIFECYCLEF("[CAM_STOP] stopCamera() called, gCameraRunning=%d", gCameraRunning);

  // Finalize AVI before taking the camera mutex. stopVideoRecording waits
  // for cam_record, which may itself be inside captureFrame (holding that
  // mutex) — calling it under the camera lock would deadlock. Skip on
  // recovery re-inits so a glitch mid-stream doesn't abort a recording.
  if (!isRecovery && videoRecording) {
    DEBUG_CAMERA_LIFECYCLEF("[CAM_STOP] Stopping active recording before deinit");
    stopVideoRecording();
  }

  if (!lockCameraMutex(15000)) {
    DEBUG_CAMERA_LIFECYCLEF("[CAM_STOP] ERROR: camera mutex timeout (camera busy)");
    return false;
  }

  // Check only after acquiring the camera mutex. In particular, inline frame
  // recovery temporarily sets gCameraRunning=false before re-initialising while
  // holding this recursive mutex. A concurrent worker STOP must wait for that
  // recovery to finish and then turn the camera back off, not mistake the
  // transient false value for an already-completed stop.
  if (!gCameraRunning) {
    DEBUG_CAMERA_LIFECYCLEF("[CAM_STOP] Already stopped, returning");
    unlockCameraMutex();
    return true;
  }

  DEBUG_CAMERA_LIFECYCLEF("[CAM_STOP] Heap before deinit: %u", esp_get_free_heap_size());
  INFO_CAMERAF("Stopping camera...");
  
  DEBUG_CAMERA_LIFECYCLEF("[CAM_STOP] Calling esp_camera_deinit()...");
  esp_camera_deinit();
  DEBUG_CAMERA_LIFECYCLEF("[CAM_STOP] esp_camera_deinit() complete");
  
  gCameraRunning = false;
  cameraStreaming = false;
  sensorStatusBumpWith("closecamera");
  if (!isRecovery) systemEventPost(SYSEVT_SENSOR_STOPPED, "Camera");
  
  DEBUG_CAMERA_LIFECYCLEF("[CAM_STOP] Heap after deinit: %u", esp_get_free_heap_size());
  INFO_CAMERAF("Stopped");

  unlockCameraMutex();
  return true;
}

void stopCamera(bool isRecovery) {
  (void)stopCameraInternal(isRecovery);
}

uint8_t* captureFrame(size_t* outLen) {
  DEBUG_CAMERA_CAPTUREF("[CAM_CAPTURE] ========== captureFrame() ENTRY ==========");
  DEBUG_CAMERA_CAPTUREF("[CAM_CAPTURE] gCameraRunning=%d cameraConnected=%d cameraStreaming=%d",
                gCameraRunning, cameraConnected, cameraStreaming);
  DEBUG_CAMERA_CAPTUREF("[CAM_CAPTURE] Heap: %u, PSRAM: %u",
                esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  if (!gCameraRunning) {
    DEBUG_CAMERA_CAPTUREF("[CAM_CAPTURE] Camera not enabled - returning NULL");
    if (outLen) *outLen = 0;
    return nullptr;
  }

  // Fast-fail: don't queue behind other captures, return busy immediately
  if (!lockCameraMutex(0)) {
    DEBUG_CAMERA_CAPTUREF("[CAM_CAPTURE] Camera busy (another capture in progress)");
    if (outLen) *outLen = 0;
    return nullptr;
  }

  // Single attempt - fail fast, recover immediately if needed
  camera_fb_t* fb = nullptr;
  DEBUG_CAMERA_CAPTUREF("[CAM_CAPTURE] Calling esp_camera_fb_get()...");

  unsigned long startMs = millis();
  fb = esp_camera_fb_get();
  unsigned long elapsed = millis() - startMs;

  DEBUG_CAMERA_CAPTUREF("[CAM_CAPTURE] esp_camera_fb_get() returned in %lu ms, fb=%p", elapsed, fb);
  
  if (!fb) {
    // Recovery logging - keep these for diagnosing camera issues
    DEBUG_CAMERA_CAPTUREF("[CAM_CAPTURE] Capture failed - attempting recovery...");

    (void)stopCameraInternal(/*isRecovery=*/true);
    vTaskDelay(pdMS_TO_TICKS(150));
    bool ok = sCameraDesiredOn && initCamera(/*isRecovery=*/true);
    if (ok) {
      fb = esp_camera_fb_get();
      // A STOP admitted while the second fetch was blocked owns the desired
      // state even if the worker could not take the mutex. Discard the recovered
      // frame; initCamera's post-init fence already prevents resurrection when
      // STOP arrived earlier in recovery.
      if (!sCameraDesiredOn && fb) {
        esp_camera_fb_return(fb);
        fb = nullptr;
      }
    }
    if (!ok || !fb) {
      DEBUG_CAMERA_CAPTUREF("[CAM_CAPTURE] Recovery failed");
    }

    if (!fb) {
      unlockCameraMutex();
      if (outLen) *outLen = 0;
      return nullptr;
    }
  }
  
  DEBUG_CAMERA_CAPTUREF("[CAM_CAPTURE] fb buf=%p len=%u %ux%u fmt=%d ts=%ld.%06ld",
                fb->buf, fb->len, fb->width, fb->height, fb->format,
                (long)fb->timestamp.tv_sec, (long)fb->timestamp.tv_usec);

  // Validate JPEG header (silent unless error)
  if (fb->format == PIXFORMAT_JPEG && fb->len >= 2) {
    if (fb->buf[0] != 0xFF || fb->buf[1] != 0xD8) {
      DEBUG_CAMERA_CAPTUREF("[CAM_CAPTURE] Invalid JPEG header: %02X %02X", fb->buf[0], fb->buf[1]);
      esp_camera_fb_return(fb);
      unlockCameraMutex();
      if (outLen) *outLen = 0;
      return nullptr;
    }
  }

  // Copy frame buffer (caller must free)
  uint8_t* buf = (uint8_t*)ps_alloc(fb->len, AllocPref::PreferPSRAM, "camera.frame");
  
  if (buf) {
    memcpy(buf, fb->buf, fb->len);
    if (outLen) *outLen = fb->len;
  } else {
    DEBUG_CAMERA_CAPTUREF("[CAM_CAPTURE] ALLOC FAILED: %u bytes, Heap: %u", 
                  fb->len, esp_get_free_heap_size());
    if (outLen) *outLen = 0;
  }

  esp_camera_fb_return(fb);
  
  // Note: With GRAB_LATEST mode, no flush needed - camera always gives latest frame
  DEBUG_CAMERA_CAPTUREF("[CAM_CAPTURE] EXIT buf=%p, len=%u", buf, outLen ? *outLen : 0);
  unlockCameraMutex();
  return buf;
}

// Set camera resolution - useful for ESP-NOW transmission (lower res = smaller files)
bool setCameraResolution(framesize_t size) {
  if (!gCameraRunning) {
    return false;
  }

  if (!lockCameraMutex(15000)) {
    DEBUG_CAMERA_SETTINGSF("[CAM_SET] ERROR: camera mutex timeout (camera busy)");
    return false;
  }
   
  sensor_t* s = esp_camera_sensor_get();
  if (!s) {
    unlockCameraMutex();
    return false;
  }
   
  int result = s->set_framesize(s, size);
  if (result == 0) {
    // Update tracked dimensions via the canonical helper (single
    // source of truth shared with the init path above).
    cameraDimsForFramesize(size, cameraWidth, cameraHeight);
    INFO_CAMERAF("Resolution set to %dx%d", cameraWidth, cameraHeight);
    unlockCameraMutex();
    return true;
  }
  unlockCameraMutex();
  return false;
}

// Set JPEG quality (0-63, lower = higher quality, larger file)
bool setCameraQuality(int quality) {
  if (!gCameraRunning) return false;

  if (!lockCameraMutex(15000)) {
    DEBUG_CAMERA_SETTINGSF("[CAM_SET] ERROR: camera mutex timeout (camera busy)");
    return false;
  }
  sensor_t* s = esp_camera_sensor_get();
  if (!s) {
    unlockCameraMutex();
    return false;
  }
  bool ok = (s->set_quality(s, quality) == 0);
  unlockCameraMutex();
  return ok;
}

// Capture frame at specific resolution (for ESP-NOW: use QQVGA 160x120)
uint8_t* captureFrameAtResolution(framesize_t size, int quality, size_t* outLen) {
  if (!gCameraRunning) {
    if (outLen) *outLen = 0;
    return nullptr;
  }

  if (!lockCameraMutex(15000)) {
    DEBUG_CAMERA_CAPTUREF("[CAM_CAPTURE] ERROR: camera mutex timeout (camera busy)");
    if (outLen) *outLen = 0;
    return nullptr;
  }
   
  // Save current settings
  sensor_t* s = esp_camera_sensor_get();
  if (!s) {
    unlockCameraMutex();
    if (outLen) *outLen = 0;
    return nullptr;
  }
  
  // Temporarily change resolution and quality
  framesize_t oldSize = (framesize_t)s->status.framesize;
  int oldQuality = s->status.quality;
  
  s->set_framesize(s, size);
  s->set_quality(s, quality);
  
  // Capture frame
  camera_fb_t* fb = esp_camera_fb_get();
  uint8_t* result = nullptr;
  
  if (fb) {
    result = (uint8_t*)ps_alloc(fb->len, AllocPref::PreferPSRAM, "camera.frame.resized");
    if (result) {
      memcpy(result, fb->buf, fb->len);
      if (outLen) *outLen = fb->len;
      DEBUG_CAMERA_CAPTUREF("Captured %dx%d frame: %u bytes (q=%d)",
                     fb->width, fb->height, (unsigned)fb->len, quality);
    } else {
      if (outLen) *outLen = 0;
    }
    esp_camera_fb_return(fb);
  } else {
    if (outLen) *outLen = 0;
  }
  
  // Restore original settings
  s->set_framesize(s, oldSize);
  s->set_quality(s, oldQuality);

  unlockCameraMutex();
   
  return result;
}

// Capture tiny frame suitable for ESP-NOW (160x120, high compression)
// ESP-NOW limit is 250 bytes per packet, so this captures very small images
// Returns grayscale thumbnail if JPEG is still too large
uint8_t* captureTinyFrame(size_t* outLen) {
  // Try QQVGA (160x120) with high compression (quality 40)
  return captureFrameAtResolution(FRAMESIZE_QQVGA, 40, outLen);
}

const char* buildCameraStatusJson() {
  if (!cameraStatusBuffer) {
    cameraStatusBuffer = (char*)ps_alloc(kStatusBufSize, AllocPref::PreferPSRAM, "camera.status.json");
    if (!cameraStatusBuffer) {
      static const char* kEmptyJson = "{}";
      return kEmptyJson;
    }
  }

  PSRAM_JSON_DOC(doc);
  // supported: camera silicon is wired on this board and compiled in. Always
  // true here; the !ENABLE_CAMERA_SENSOR stub reports false. This is the
  // camera's analog of the microphone's pdmAvailable — a build fact, not a probe.
  doc["supported"] = true;
  // detected: the SCCB probe has seen a sensor at least once since boot.
  // Sticky, so it stays true while the camera is stopped. Consumers wanting
  // "is there a camera?" want this; "connected" only answers "is it running?".
  doc["detected"] = cameraDetected;
  doc["enabled"] = gCameraRunning;
  doc["connected"] = cameraConnected;
  doc["streaming"] = cameraStreaming;
  doc["model"] = cameraModel;
  doc["width"] = cameraWidth;
  doc["height"] = cameraHeight;
  doc["psram"] = psramFound();

  serializeJson(doc, cameraStatusBuffer, kStatusBufSize);
  return cameraStatusBuffer;
}

// =============================================================================
// Camera power worker — init/stop/restart run here (large stack), not on the
// G2 tap dispatcher (4 KB) or other shallow stacks. captureFrame() recovery
// still calls init/stop inline while holding the camera mutex from the same
// task; do not route that path through this queue (deadlock risk).
// =============================================================================

enum : uint8_t {
  CAM_PWR_CMD_START   = 0,
  CAM_PWR_CMD_STOP    = 1,
  CAM_PWR_CMD_RESTART = 2,
};

struct CameraPwrMsg {
  uint8_t  cmd;
  uint8_t  completionSlot;
  uint16_t reserved;
  uint32_t completionGeneration;
};

struct CameraPwrCompletionSlot {
  StaticSemaphore_t storage;
  SemaphoreHandle_t done;
  uint32_t          generation;
  bool              inUse;
  bool              waiterAttached;
  bool              completed;
  bool              result;
};

static QueueHandle_t       sCamPwrQueue = nullptr;
static TaskHandle_t        sCamPwrTask  = nullptr;
static CameraPowerPostHook sCamPwrHook  = nullptr;
static StaticSemaphore_t   sCamPwrLifecycleMutexStorage;
static SemaphoreHandle_t   sCamPwrLifecycleMutex =
    xSemaphoreCreateMutexStatic(&sCamPwrLifecycleMutexStorage);
// Synchronous completions use bounded static storage rather than a caller task
// handle. A timed-out waiter detaches, but its generation remains reserved
// until the queued/in-flight worker completes it; only then can the slot be
// reused. This avoids stale task handles and never consumes or overwrites
// another subsystem's task notification.
constexpr uint8_t          kCamPwrNoCompletion = 0xff;
constexpr uint8_t          kCamPwrCompletionSlotCount = 4;
static CameraPwrCompletionSlot
    sCamPwrCompletions[kCamPwrCompletionSlotCount] = {};
static UBaseType_t         sCamPwrAdmissions = 0;
static TickType_t          sCamPwrLastDetachTick = 0;
static bool                sCamPwrHasDetached = false;

// A full quiet interval after the last completed command is the retirement
// boundary. The drained queue is deleted and the published pair is detached
// under sCamPwrLifecycleMutex, so admissions either land on this worker or
// create a wholly new pair; no sender can retain a queue being deleted.
constexpr uint32_t kCamPwrIdleRetireMs = 10000;

static bool cameraPwrLifecycleTake() {
  return sCamPwrLifecycleMutex &&
         xSemaphoreTake(sCamPwrLifecycleMutex, portMAX_DELAY) == pdTRUE;
}

static void cameraPwrLifecycleGive() {
  xSemaphoreGive(sCamPwrLifecycleMutex);
}

static bool cameraPwrReserveCompletionLocked(CameraPwrMsg& m) {
  for (uint8_t i = 0; i < kCamPwrCompletionSlotCount; ++i) {
    CameraPwrCompletionSlot& slot = sCamPwrCompletions[i];
    if (slot.inUse) continue;

    if (!slot.done) {
      slot.done = xSemaphoreCreateBinaryStatic(&slot.storage);
      if (!slot.done) {
        ERROR_CAMERAF("[CAM_PWR] static completion semaphore init failed slot=%u",
                      (unsigned)i);
        return false;
      }
    }
    // A give that raced just after a previous caller timed out can leave the
    // binary semaphore full. Generation matching makes it harmless; draining
    // here prevents it from completing this new request immediately.
    while (xSemaphoreTake(slot.done, 0) == pdTRUE) {}

    ++slot.generation;
    if (slot.generation == 0) ++slot.generation;
    slot.waiterAttached = true;
    slot.completed = false;
    slot.result = false;
    slot.inUse = true;
    m.completionSlot = i;
    m.completionGeneration = slot.generation;
    return true;
  }
  ERROR_CAMERAF("[CAM_PWR] all %u static completion slots are busy",
                (unsigned)kCamPwrCompletionSlotCount);
  return false;
}

static void cameraPwrReleaseCompletionLocked(const CameraPwrMsg& m) {
  if (m.completionSlot >= kCamPwrCompletionSlotCount) return;
  CameraPwrCompletionSlot& slot = sCamPwrCompletions[m.completionSlot];
  if (slot.inUse && slot.generation == m.completionGeneration) {
    slot.inUse = false;
    slot.waiterAttached = false;
    slot.completed = false;
  }
}

static void cameraPwrComplete(const CameraPwrMsg& m, bool result) {
  if (m.completionSlot >= kCamPwrCompletionSlotCount) return;
  if (!cameraPwrLifecycleTake()) {
    ERROR_CAMERAF("[CAM_PWR] lifecycle mutex unavailable for completion");
    return;
  }
  CameraPwrCompletionSlot& slot = sCamPwrCompletions[m.completionSlot];
  if (slot.inUse && slot.generation == m.completionGeneration) {
    slot.result = result;
    slot.completed = true;
    if (!slot.waiterAttached) {
      // The caller timed out while this command was queued or running. The
      // worker is the final owner of the generation and releases it now; no
      // stale signal is published and reuse was impossible before this point.
      cameraPwrReleaseCompletionLocked(m);
    } else if (xSemaphoreGive(slot.done) != pdTRUE) {
      ERROR_CAMERAF("[CAM_PWR] completion semaphore already full slot=%u gen=%lu",
                    (unsigned)m.completionSlot,
                    (unsigned long)m.completionGeneration);
    }
  }
  cameraPwrLifecycleGive();
}

void cameraPowerSetPostHook(CameraPowerPostHook hook) {
  if (!cameraPwrLifecycleTake()) {
    ERROR_CAMERAF("[CAM_PWR] lifecycle mutex unavailable; post hook unchanged");
    return;
  }
  sCamPwrHook = hook;
  cameraPwrLifecycleGive();
}

static void cameraPwrRunOne(const CameraPwrMsg& m) {
  bool result = false;
  switch (m.cmd) {
    case CAM_PWR_CMD_STOP:
      result = stopCameraInternal(/*isRecovery=*/false);
      break;
    case CAM_PWR_CMD_START:
      if (!gCameraRunning) {
        if (!initCamera()) {
          BROADCAST_PRINTF("[CAM_PWR] initCamera failed — reverting camera auto-start");
          setSetting(gSettings.cameraAutoStart, false);
          systemEventPost(SYSEVT_SENSOR_START_FAILED, "Camera", "init failed");
          // The camera is off because init failed, not because the user closed it —
          // don't let a ramflush capture read this as an intentional close.
          ramFlushMarkAutostartFailed(RF_CAMERA);
        }
      }
      result = gCameraRunning;
      break;
    case CAM_PWR_CMD_RESTART: {
      const bool was = gCameraRunning;
      if (was) {
        if (!stopCameraInternal(/*isRecovery=*/false)) break;
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!initCamera()) {
          BROADCAST_PRINTF("[CAM_PWR] restart: initCamera failed after stop");
        }
      }
      result = gCameraRunning;
      break;
    }
    default:
      break;
  }
  CameraPowerPostHook hook = nullptr;
  if (cameraPwrLifecycleTake()) {
    hook = sCamPwrHook;
    cameraPwrLifecycleGive();
  }
  if (hook) {
    hook();
  }
  cameraPwrComplete(m, result);
}

static void cameraPwrWorker(void* arg) {
  QueueHandle_t ownQueue = static_cast<QueueHandle_t>(arg);
  CameraPwrMsg m;
  for (;;) {
    if (xQueueReceive(ownQueue, &m,
                      pdMS_TO_TICKS(kCamPwrIdleRetireMs)) == pdTRUE) {
      cameraPwrRunOne(m);
      continue;
    }

    // Serialise the empty re-check with every ensure+send operation. If a
    // sender won the mutex first, its item is visible and this worker stays.
    // If retirement wins, both globals are detached before the mutex is
    // released, so the sender creates a new queue/task and never touches this
    // queue. No task is force-deleted; this worker owns its final vTaskDelete.
    if (!cameraPwrLifecycleTake()) continue;
    const bool ownsPublishedPair =
        sCamPwrTask == xTaskGetCurrentTaskHandle() &&
        sCamPwrQueue == ownQueue;
    const bool queueDrained = uxQueueMessagesWaiting(ownQueue) == 0;
    if (!ownsPublishedPair || !queueDrained || sCamPwrAdmissions != 0) {
      cameraPwrLifecycleGive();
      continue;
    }

    // No sender can be blocked on or retain ownQueue: every send holds an
    // admission reference from pointer capture through xQueueSend completion.
    // Delete the drained queue while new admissions are still excluded, then
    // detach the pair. The only unavoidable overlap left is the old dynamic
    // task's stack/TCB, which FreeRTOS reclaims later from an IDLE task after
    // this worker self-deletes; a replacement allocation may therefore need a
    // second 10 KB stack briefly and can fail explicitly under heap pressure.
    vQueueDelete(ownQueue);
    sCamPwrTask = nullptr;
    sCamPwrQueue = nullptr;
    sCamPwrLastDetachTick = xTaskGetTickCount();
    sCamPwrHasDetached = true;
    cameraPwrLifecycleGive();

    vTaskDelete(nullptr);
  }
}

// Caller must hold sCamPwrLifecycleMutex. Publishing and teardown of the pair
// happen under that same mutex, so a non-null pair always accepts the send that
// follows ensure. Dynamic xTaskCreate keeps the stack in internal DRAM.
static bool cameraPwrEnsureStartedLocked() {
  if (sCamPwrQueue && sCamPwrTask) {
    return true;
  }
  if (sCamPwrQueue || sCamPwrTask) {
    ERROR_CAMERAF("[CAM_PWR] inconsistent lifecycle state (queue=%p task=%p)",
                  (void*)sCamPwrQueue, (void*)sCamPwrTask);
    return false;
  }
  constexpr UBaseType_t kDepth = 6;
  // ESP-IDF xTaskCreate stack depth is in BYTES — 10240 here is 10 KB.
  // Observed HWM under light load was ~2.5–7 KB; leave headroom until a
  // worst-case capture/stream measurement justifies cutting.
  constexpr uint32_t  kStack = 10240;
  sCamPwrQueue = xQueueCreate(kDepth, sizeof(CameraPwrMsg));
  if (!sCamPwrQueue) {
    ERROR_CAMERAF("[CAM_PWR] queue create failed (DRAM free=%u largest=%u)",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (sCamPwrHasDetached) {
      ERROR_CAMERAF("[CAM_PWR] previous self-deleted worker detached %lu ms ago; "
                    "its internal stack/TCB are reclaimed asynchronously by IDLE",
                    (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount() -
                                                 sCamPwrLastDetachTick));
    }
    return false;
  }
  taskStackRecord("cam_pwr", kStack);
  const BaseType_t ok =
      // Pin to Core 1 (I2C_SENSOR_CORE): initCamera() does a shared-Wire I2C scan
      // before handing the bus to the camera driver, so this worker carries the
      // starve-mid-transaction → bus-storm → panic(4) hazard. Off the saturated Core 0.
      xTaskCreatePinnedToCore(cameraPwrWorker, "cam_pwr", kStack, sCamPwrQueue,
                  tskIDLE_PRIORITY + 2, &sCamPwrTask, I2C_SENSOR_CORE);
  if (ok != pdPASS) {
    ERROR_CAMERAF("[CAM_PWR] worker task create failed — need ~%u B internal stack "
                  "(DRAM free=%u largest=%u)",
                  (unsigned)kStack,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (sCamPwrHasDetached) {
      ERROR_CAMERAF("[CAM_PWR] previous self-deleted worker detached %lu ms ago; "
                    "its internal stack/TCB are reclaimed asynchronously by IDLE",
                    (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount() -
                                                 sCamPwrLastDetachTick));
    }
    vQueueDelete(sCamPwrQueue);
    sCamPwrQueue = nullptr;
    sCamPwrTask  = nullptr;
    return false;
  }
  return true;
}

bool cameraPowerWorkerEnsureStarted() {
  if (!cameraPwrLifecycleTake()) {
    ERROR_CAMERAF("[CAM_PWR] lifecycle mutex unavailable; cannot start worker");
    return false;
  }
  const bool ok = cameraPwrEnsureStartedLocked();
  cameraPwrLifecycleGive();
  return ok;
}

static bool cameraPwrSend(CameraPwrMsg& m, TickType_t queueTicks,
                          bool needsCompletion = false) {
  if (!cameraPwrLifecycleTake()) {
    ERROR_CAMERAF("[CAM_PWR] lifecycle mutex unavailable; command=%u rejected",
                  (unsigned)m.cmd);
    return false;
  }

  // Desired power is authoritative and published at admission, before any
  // allocation or queue operation can fail. In particular, a STOP that cannot
  // recreate cam_pwr still fences captureFrame's inline recovery from re-init.
  // Plain initCamera()/stopCamera() and recovery do not mutate this latch.
  sCameraDesiredOn = m.cmd != CAM_PWR_CMD_STOP;

  if (!cameraPwrEnsureStartedLocked()) {
    cameraPwrLifecycleGive();
    return false;
  }
  if (needsCompletion && !cameraPwrReserveCompletionLocked(m)) {
    cameraPwrLifecycleGive();
    return false;
  }
  QueueHandle_t targetQueue = sCamPwrQueue;
  ++sCamPwrAdmissions;
  cameraPwrLifecycleGive();

  // Do not block on a full queue while holding the lifecycle mutex: the worker
  // copies the post-hook under that mutex before it can receive the next item.
  // The admission reference prevents retirement/deletion while this send is in
  // flight, so targetQueue remains valid across the blocking call.
  const bool sent = xQueueSend(targetQueue, &m, queueTicks) == pdTRUE;

  if (!cameraPwrLifecycleTake()) {
    ERROR_CAMERAF("[CAM_PWR] lifecycle mutex unavailable after command=%u send",
                  (unsigned)m.cmd);
    return false;
  }
  configASSERT(sCamPwrAdmissions > 0);
  --sCamPwrAdmissions;
  const UBaseType_t queued = sent ? 0 : uxQueueMessagesWaiting(targetQueue);
  if (!sent) cameraPwrReleaseCompletionLocked(m);
  cameraPwrLifecycleGive();
  if (!sent) {
    ERROR_CAMERAF("[CAM_PWR] queue send failed command=%u queued=%u",
                  (unsigned)m.cmd, (unsigned)queued);
  }
  return sent;
}

bool cameraPowerRequestStartAsync() {
  CameraPwrMsg m{CAM_PWR_CMD_START, kCamPwrNoCompletion, 0, 0};
  return cameraPwrSend(m, 0);
}

bool cameraPowerRequestStopAsync() {
  CameraPwrMsg m{CAM_PWR_CMD_STOP, kCamPwrNoCompletion, 0, 0};
  return cameraPwrSend(m, 0);
}

static bool cameraPwrWaitDone(const CameraPwrMsg& m, uint32_t waitMs,
                              bool& result) {
  if (m.completionSlot >= kCamPwrCompletionSlotCount) return false;

  SemaphoreHandle_t done = nullptr;
  if (cameraPwrLifecycleTake()) {
    CameraPwrCompletionSlot& slot = sCamPwrCompletions[m.completionSlot];
    if (slot.inUse && slot.generation == m.completionGeneration) {
      done = slot.done;
    }
    cameraPwrLifecycleGive();
  }
  if (!done || xSemaphoreTake(done, pdMS_TO_TICKS(waitMs)) != pdTRUE) {
    if (cameraPwrLifecycleTake()) {
      CameraPwrCompletionSlot& slot = sCamPwrCompletions[m.completionSlot];
      if (slot.inUse && slot.generation == m.completionGeneration) {
        slot.waiterAttached = false;
        if (slot.completed) {
          // Completion raced the timeout and already published its give. The
          // worker no longer owns this generation, so drain and release it;
          // the API still reports the elapsed timeout honestly.
          while (xSemaphoreTake(slot.done, 0) == pdTRUE) {}
          cameraPwrReleaseCompletionLocked(m);
        }
        // Otherwise leave inUse set. The queued/in-flight worker owns the last
        // reference and will release this generation from cameraPwrComplete().
      }
      cameraPwrLifecycleGive();
    }
    return false;
  }

  bool matched = false;
  if (cameraPwrLifecycleTake()) {
    CameraPwrCompletionSlot& slot = sCamPwrCompletions[m.completionSlot];
    matched = slot.inUse && slot.generation == m.completionGeneration &&
              slot.completed;
    if (matched) result = slot.result;
    cameraPwrReleaseCompletionLocked(m);
    cameraPwrLifecycleGive();
  }
  return matched;
}

bool cameraPowerRequestStartSync(uint32_t waitMs) {
  CameraPwrMsg m{CAM_PWR_CMD_START, kCamPwrNoCompletion, 0, 0};
  if (!cameraPwrSend(m, pdMS_TO_TICKS(5000), true)) {
    return false;
  }
  bool result = false;
  if (!cameraPwrWaitDone(m, waitMs, result)) {
    ERROR_CAMERAF("[CAM_PWR] start timed out slot=%u gen=%lu",
                  (unsigned)m.completionSlot,
                  (unsigned long)m.completionGeneration);
    return false;
  }
  return result;
}

bool cameraPowerRequestStopSync(uint32_t waitMs) {
  CameraPwrMsg m{CAM_PWR_CMD_STOP, kCamPwrNoCompletion, 0, 0};
  if (!cameraPwrSend(m, pdMS_TO_TICKS(5000), true)) {
    return false;
  }
  bool result = false;
  if (!cameraPwrWaitDone(m, waitMs, result)) {
    ERROR_CAMERAF("[CAM_PWR] stop timed out slot=%u gen=%lu",
                  (unsigned)m.completionSlot,
                  (unsigned long)m.completionGeneration);
    return false;
  }
  return result;
}

bool cameraPowerRequestRestartSync(uint32_t waitMs) {
  CameraPwrMsg m{CAM_PWR_CMD_RESTART, kCamPwrNoCompletion, 0, 0};
  if (!cameraPwrSend(m, pdMS_TO_TICKS(5000), true)) {
    return false;
  }
  bool result = false;
  if (!cameraPwrWaitDone(m, waitMs, result)) {
    ERROR_CAMERAF("[CAM_PWR] restart timed out slot=%u gen=%lu",
                  (unsigned)m.completionSlot,
                  (unsigned long)m.completionGeneration);
    return false;
  }
  return result;
}

// Command handlers
const char* cmd_camera(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return buildCameraStatusJson();
}

const char* cmd_camerastart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gSettings.cameraEnabled) {
    return "ERROR: Camera is disabled - run 'cameraenabled 1' first";
  }
  if (cameraPowerRequestStartSync(60000)) {
    return "Camera started successfully";
  }
  return "Error: Camera initialization failed";
}

const char* cmd_camerastop(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return cameraPowerRequestStopSync(30000)
             ? "Camera stopped"
             : "Error: Camera stop failed or timed out";
}

const char* cmd_cameracapture(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) {
    return "Error: Camera not enabled - run opencamera first";
  }
  
  size_t len = 0;
  uint8_t* frame = captureFrame(&len);
  if (frame) {
    EXT_RAM_BSS_ATTR static char result[64];
    snprintf(result, sizeof(result), "Captured frame: %u bytes", (unsigned)len);
    free(frame);
    // The captured frame is measured then discarded — it is not written to
    // storage. To save a photo to disk, use 'camerasave'.
    cliHint("to save a photo to storage, run 'camerasave'");
    return result;
  }
  return "Error: Frame capture failed";
}

const char* cmd_camerares(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Parse resolution argument
  String sizeStr = argsInput;
  sizeStr.trim();
  sizeStr.toLowerCase();
  
  if (sizeStr.length() == 0) {
    EXT_RAM_BSS_ATTR static char result[256];
    snprintf(result, sizeof(result),
      "Current: %dx%d\nUsage: camerares <size>\n"
      "Sizes: 96x96 qqvga(160x120) qcif(176x144) hqvga(240x176) 240x240 "
      "qvga(320x240) vga(640x480) svga(800x600) xga(1024x768) "
      "sxga(1280x1024) uxga(1600x1200)\nNote: Requires camera restart",
      cameraWidth, cameraHeight);
    return result;
  }

  framesize_t newSize = FRAMESIZE_VGA;
  if      (sizeStr == "96x96")                          newSize = FRAMESIZE_96X96;
  else if (sizeStr == "qqvga" || sizeStr == "160x120")  newSize = FRAMESIZE_QQVGA;
  else if (sizeStr == "qcif"  || sizeStr == "176x144")  newSize = FRAMESIZE_QCIF;
  else if (sizeStr == "hqvga" || sizeStr == "240x176")  newSize = FRAMESIZE_HQVGA;
  else if (sizeStr == "240x240")                        newSize = FRAMESIZE_240X240;
  else if (sizeStr == "qvga"  || sizeStr == "320x240")  newSize = FRAMESIZE_QVGA;
  else if (sizeStr == "cif"   || sizeStr == "400x296")  newSize = FRAMESIZE_CIF;
  else if (sizeStr == "vga"   || sizeStr == "640x480")  newSize = FRAMESIZE_VGA;
  else if (sizeStr == "svga"  || sizeStr == "800x600")  newSize = FRAMESIZE_SVGA;
  else if (sizeStr == "xga"   || sizeStr == "1024x768") newSize = FRAMESIZE_XGA;
  else if (sizeStr == "sxga"  || sizeStr == "1280x1024") newSize = FRAMESIZE_SXGA;
  else if (sizeStr == "uxga"  || sizeStr == "1600x1200") newSize = FRAMESIZE_UXGA;
  else return "Error: Unknown resolution. Use: 96x96, qqvga, qcif, hqvga, 240x240, qvga, vga, svga, xga, sxga, uxga";
  
  // Save to settings for persistence
  setSetting(gSettings.cameraFramesize, (int)cameraFramesizeSettingFromEnum(newSize));
  
  // If camera is running, do a full restart for reliable resolution change
  bool wasEnabled = gCameraRunning;
  bool wasStreaming = cameraStreaming;
  
  const bool restartOk = !wasEnabled || cameraPowerRequestRestartSync(60000);
  
  EXT_RAM_BSS_ATTR static char result[96];
  if (!restartOk) {
    snprintf(result, sizeof(result),
             "Error: Resolution saved, but camera restart failed");
  } else if (wasStreaming) {
    snprintf(result, sizeof(result), "Resolution set to %dx%d (saved). Streaming stopped - please restart stream.", cameraWidth, cameraHeight);
  } else if (wasEnabled) {
    snprintf(result, sizeof(result), "Resolution set to %dx%d (saved). Camera restarted.", cameraWidth, cameraHeight);
  } else {
    snprintf(result, sizeof(result), "Resolution set to %dx%d (saved). Will apply on next camera start.", cameraWidth, cameraHeight);
  }
  return result;
}

// Numeric framesize command for the settings UI. The value is the setting
// INDEX (0-10) that maps to a framesize_t via cameraFramesizeFromSetting().
// Dropdown options on the web Sensors page also use this index space, so the
// query response (no args) must report the index — NOT the enum value, which
// would collide (e.g. setting index 5 = UXGA, but enum FRAMESIZE_240X240 = 5).
const char* cmd_cameraframesize(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String valStr = argsInput;
  valStr.trim();

  if (valStr.length() == 0) {
    EXT_RAM_BSS_ATTR static char result[64];
    snprintf(result, sizeof(result), "cameraFramesize=%d", gSettings.cameraFramesize);
    return result;
  }
  
  int newSize = valStr.toInt();
  if (newSize < 0 || newSize > 10) {
    return "Error: Framesize must be 0-10 (0-5: QVGA..UXGA, 6-10: 96x96/QQVGA/QCIF/HQVGA/240x240)";
  }
  
  setSetting(gSettings.cameraFramesize, newSize);
  
  // If camera is running, restart to apply
  bool wasEnabled = gCameraRunning;
  const bool restartOk = !wasEnabled || cameraPowerRequestRestartSync(60000);
  
  EXT_RAM_BSS_ATTR static char result[80];
  if (!restartOk) {
    snprintf(result, sizeof(result),
             "Error: Resolution saved, but camera restart failed");
  } else {
    snprintf(result, sizeof(result), "Resolution set to %dx%d. %s",
             cameraWidth, cameraHeight,
             wasEnabled ? "Camera restarted." : "Will apply on next start.");
  }
  return result;
}

const char* cmd_cameraquality(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String valStr = argsInput;
  valStr.trim();
  
  if (valStr.length() == 0) {
    EXT_RAM_BSS_ATTR static char result[80];
    snprintf(result, sizeof(result), "Current: %d\nUsage: cameraquality <0-63> (lower = better quality, larger file)", 
             gSettings.cameraQuality);
    return result;
  }
  
  int quality = valStr.toInt();
  if (quality < 0 || quality > 63) {
    return "Error: Quality must be 0-63";
  }
  
  // Save to settings for persistence
  setSetting(gSettings.cameraQuality, quality);
  
  // Apply live if camera is running (quality can be changed without restart)
  if (gCameraRunning) {
    setCameraQuality(quality);
    EXT_RAM_BSS_ATTR static char result[64];
    snprintf(result, sizeof(result), "JPEG quality set to %d (saved, applied live)", quality);
    return result;
  }
  
  EXT_RAM_BSS_ATTR static char result[64];
  snprintf(result, sizeof(result), "JPEG quality set to %d (saved, will apply on camera start)", quality);
  return result;
}

const char* cmd_cameratiny(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) {
    return "Error: Camera not enabled - run opencamera first";
  }
  
  size_t len = 0;
  uint8_t* frame = captureTinyFrame(&len);
  if (frame) {
    EXT_RAM_BSS_ATTR static char result[96];
    snprintf(result, sizeof(result), "Tiny frame (160x120): %u bytes %s", 
             (unsigned)len, len <= 250 ? "(ESP-NOW compatible)" : "(too large for single ESP-NOW packet)");
    free(frame);
    return result;
  }
  return "Error: Tiny frame capture failed";
}

// Helper to apply a camera setting and optionally save
static bool applyCameraSetting(const char* name, int value, int minVal, int maxVal, 
                                int (*setter)(sensor_t*, int), int* settingPtr) {
  if (!gCameraRunning) return false;
  if (value < minVal || value > maxVal) return false;
  
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return false;
  
  if (setter(s, value) == 0) {
    if (settingPtr) {
      *settingPtr = value;
      writeSettingsJson();  // Persist
    }
    return true;
  }
  return false;
}

const char* cmd_camerabrightness(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) return "Error: Camera not enabled";
  
  String valStr = argsInput;
  valStr.trim();
  
  if (valStr.length() == 0) {
    EXT_RAM_BSS_ATTR static char result[64];
    snprintf(result, sizeof(result), "Brightness: %d (range -2 to 2)", gSettings.cameraBrightness);
    return result;
  }
  
  int val = valStr.toInt();
  sensor_t* s = esp_camera_sensor_get();
  if (s && s->set_brightness(s, val) == 0) {
    setSetting(gSettings.cameraBrightness, val);
    EXT_RAM_BSS_ATTR static char result[48];
    snprintf(result, sizeof(result), "Brightness set to %d (saved)", val);
    return result;
  }
  return "Error: Failed (use -2 to 2)";
}

const char* cmd_cameracontrast(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) return "Error: Camera not enabled";
  
  String valStr = argsInput;
  valStr.trim();
  
  if (valStr.length() == 0) {
    EXT_RAM_BSS_ATTR static char result[64];
    snprintf(result, sizeof(result), "Contrast: %d (range -2 to 2)", gSettings.cameraContrast);
    return result;
  }
  
  int val = valStr.toInt();
  sensor_t* s = esp_camera_sensor_get();
  if (s && s->set_contrast(s, val) == 0) {
    setSetting(gSettings.cameraContrast, val);
    EXT_RAM_BSS_ATTR static char result[48];
    snprintf(result, sizeof(result), "Contrast set to %d (saved)", val);
    return result;
  }
  return "Error: Failed (use -2 to 2)";
}

const char* cmd_camerasaturation(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) return "Error: Camera not enabled";
  
  String valStr = argsInput;
  valStr.trim();
  
  if (valStr.length() == 0) {
    EXT_RAM_BSS_ATTR static char result[64];
    snprintf(result, sizeof(result), "Saturation: %d (range -2 to 2)", gSettings.cameraSaturation);
    return result;
  }
  
  int val = valStr.toInt();
  sensor_t* s = esp_camera_sensor_get();
  if (s && s->set_saturation(s, val) == 0) {
    setSetting(gSettings.cameraSaturation, val);
    EXT_RAM_BSS_ATTR static char result[48];
    snprintf(result, sizeof(result), "Saturation set to %d (saved)", val);
    return result;
  }
  return "Error: Failed (use -2 to 2)";
}

const char* cmd_camerawb(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) return "Error: Camera not enabled";
  
  String valStr = argsInput;
  valStr.trim();
  
  if (valStr.length() == 0) {
    EXT_RAM_BSS_ATTR static char result[96];
    snprintf(result, sizeof(result), "WB mode: %d (0=Auto,1=Sunny,2=Cloudy,3=Office,4=Home)", gSettings.cameraWBMode);
    return result;
  }
  
  int val = valStr.toInt();
  if (val < 0 || val > 4) return "Error: WB mode must be 0-4";
  
  sensor_t* s = esp_camera_sensor_get();
  if (s && s->set_wb_mode(s, val) == 0) {
    setSetting(gSettings.cameraWBMode, val);
    EXT_RAM_BSS_ATTR static char result[48];
    snprintf(result, sizeof(result), "WB mode set to %d (saved)", val);
    return result;
  }
  return "Error: Failed to set WB mode";
}

const char* cmd_camerasharpness(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) return "Error: Camera not enabled";
  
  String valStr = argsInput;
  valStr.trim();
  
  if (valStr.length() == 0) {
    EXT_RAM_BSS_ATTR static char result[64];
    snprintf(result, sizeof(result), "Sharpness: %d (range -2 to 2, OV3660 only)", gSettings.cameraSharpness);
    return result;
  }
  
  int val = valStr.toInt();
  if (val < -2 || val > 2) return "Error: Sharpness must be -2 to 2";
  
  sensor_t* s = esp_camera_sensor_get();
  if (s && s->set_sharpness && s->set_sharpness(s, val) == 0) {
    setSetting(gSettings.cameraSharpness, val);
    EXT_RAM_BSS_ATTR static char result[48];
    snprintf(result, sizeof(result), "Sharpness set to %d (saved)", val);
    return result;
  }
  return "Error: Failed (OV3660 only, use -2 to 2)";
}

const char* cmd_cameradenoise(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) return "Error: Camera not enabled";
  
  String valStr = argsInput;
  valStr.trim();
  
  if (valStr.length() == 0) {
    EXT_RAM_BSS_ATTR static char result[64];
    snprintf(result, sizeof(result), "Denoise: %d (range 0-8)", gSettings.cameraDenoise);
    return result;
  }
  
  int val = valStr.toInt();
  if (val < 0 || val > 8) return "Error: Denoise must be 0-8";
  
  sensor_t* s = esp_camera_sensor_get();
  if (s && s->set_denoise && s->set_denoise(s, val) == 0) {
    setSetting(gSettings.cameraDenoise, val);
    EXT_RAM_BSS_ATTR static char result[48];
    snprintf(result, sizeof(result), "Denoise set to %d (saved)", val);
    return result;
  }
  return "Error: Failed to set denoise";
}

const char* cmd_cameraeffect(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) return "Error: Camera not enabled";
  
  String valStr = argsInput;
  valStr.trim();
  
  if (valStr.length() == 0) {
    EXT_RAM_BSS_ATTR static char result[96];
    snprintf(result, sizeof(result), "Effect: %d (0=None,1=Neg,2=Gray,3=Red,4=Green,5=Blue,6=Sepia)", gSettings.cameraSpecialEffect);
    return result;
  }
  
  int val = valStr.toInt();
  if (val < 0 || val > 6) return "Error: Effect must be 0-6";
  
  sensor_t* s = esp_camera_sensor_get();
  if (s && s->set_special_effect(s, val) == 0) {
    setSetting(gSettings.cameraSpecialEffect, val);
    EXT_RAM_BSS_ATTR static char result[48];
    snprintf(result, sizeof(result), "Effect set to %d (saved)", val);
    return result;
  }
  return "Error: Failed to set effect";
}

const char* cmd_cameraexposure(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) return "Error: Camera not enabled";
  
  String valStr = argsInput;
  valStr.trim();
  
  if (valStr.length() == 0) {
    EXT_RAM_BSS_ATTR static char result[80];
    snprintf(result, sizeof(result), "AE Level: %d (range -2 to 2, negative=darker)", gSettings.cameraAELevel);
    return result;
  }
  
  int val = valStr.toInt();
  if (val < -2 || val > 2) return "Error: AE Level must be -2 to 2";
  
  sensor_t* s = esp_camera_sensor_get();
  if (s && s->set_ae_level(s, val) == 0) {
    setSetting(gSettings.cameraAELevel, val);
    EXT_RAM_BSS_ATTR static char result[64];
    snprintf(result, sizeof(result), "AE Level set to %d (saved)", val);
    return result;
  }
  return "Error: Failed to set AE level";
}

const char* cmd_cameraaec(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) return "Error: Camera not enabled";

  sensor_t* s = esp_camera_sensor_get();
  if (!s) return "Error: Camera sensor not available";

  String arg = argsInput;
  arg.trim();
  
  if (arg.length() == 0) {
    bool enabled = (s->status.aec != 0);
    return enabled ? "Auto exposure: ON" : "Auto exposure: OFF (manual)";
  }

  arg.toLowerCase();

  bool enable = (arg == "on" || arg == "1" || arg == "true" || arg == "auto");
  if (s->set_exposure_ctrl(s, enable ? 1 : 0) == 0) {
    return enable ? "Auto exposure enabled" : "Auto exposure disabled (manual)";
  }
  return "Error: Failed";
}

const char* cmd_camerafps(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String valStr = argsInput;
  valStr.trim();

  if (valStr.length() == 0) {
    EXT_RAM_BSS_ATTR static char buf[96];
    snprintf(buf, sizeof(buf), "Camera FPS: %d fps\nUsage: camerafps <1-20>",
             gSettings.cameraStreamFps);
    return buf;
  }

  int val = valStr.toInt();
  if (val < 1 || val > 20) return "Error: cameraStreamFps must be 1-20";
  setSetting(gSettings.cameraStreamFps, val);
  EXT_RAM_BSS_ATTR static char buf[64];
  snprintf(buf, sizeof(buf), "cameraStreamFps set to %d fps", val);
  return buf;
}

const char* cmd_cameraaecvalue(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) return "Error: Camera not enabled";

  String valStr = argsInput;
  valStr.trim();
  
  if (valStr.length() == 0) {
    return "Error: invalid arguments — Usage: cameraaecvalue <0-1200>";
  }

  int val = valStr.toInt();
  if (val < 0 || val > 1200) return "Error: AEC value must be 0-1200";

  sensor_t* s = esp_camera_sensor_get();
  if (!s) return "Error: Camera sensor not available";

  (void)s->set_exposure_ctrl(s, 0);
  if (s->set_aec_value(s, val) == 0) {
    EXT_RAM_BSS_ATTR static char result[64];
    snprintf(result, sizeof(result), "Manual exposure set to %d", val);
    return result;
  }
  return "Error: Failed";
}

const char* cmd_cameraagc(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) return "Error: Camera not enabled";

  sensor_t* s = esp_camera_sensor_get();
  if (!s) return "Error: Camera sensor not available";

  String arg = argsInput;
  arg.trim();
  
  if (arg.length() == 0) {
    bool enabled = (s->status.agc != 0);
    return enabled ? "Auto gain: ON" : "Auto gain: OFF (manual)";
  }

  arg.toLowerCase();

  bool enable = (arg == "on" || arg == "1" || arg == "true" || arg == "auto");
  if (s->set_gain_ctrl(s, enable ? 1 : 0) == 0) {
    return enable ? "Auto gain enabled" : "Auto gain disabled (manual)";
  }
  return "Error: Failed";
}

const char* cmd_cameraagcgain(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) return "Error: Camera not enabled";

  String valStr = argsInput;
  valStr.trim();
  
  if (valStr.length() == 0) {
    return "Error: invalid arguments — Usage: cameraagcgain <0-30>";
  }

  int val = valStr.toInt();
  if (val < 0 || val > 30) return "Error: AGC gain must be 0-30";

  sensor_t* s = esp_camera_sensor_get();
  if (!s) return "Error: Camera sensor not available";

  (void)s->set_gain_ctrl(s, 0);
  if (s->set_agc_gain(s, val) == 0) {
    EXT_RAM_BSS_ATTR static char result[64];
    snprintf(result, sizeof(result), "Manual gain set to %d", val);
    return result;
  }
  return "Error: Failed";
}

// =============================================================================
// Runtime-only sensor controls — no persistence, no UI integration.
// Use these to test which OV3660 settings actually improve image quality
// before promoting any of them to gSettings + persisted JSON.
// =============================================================================

// Gainceiling: 0..6 → 2X, 4X, 8X, 16X, 32X, 64X, 128X.
// Most likely fix for "washed out" indoor symptom: default value 0 caps the
// sensor at 2× analog gain so AGC can't expose dim scenes; raising to 6 (128X)
// gives AEC headroom to actually brighten the image.
const char* cmd_cameragainceiling(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) return "Error: Camera not enabled";

  String s = argsInput; s.trim();
  sensor_t* sens = esp_camera_sensor_get();
  if (!sens) return "Error: Camera sensor not available";

  if (s.length() == 0) {
    EXT_RAM_BSS_ATTR static char r[64];
    static const char* kNames[] = {"2X","4X","8X","16X","32X","64X","128X"};
    int v = sens->status.gainceiling;
    snprintf(r, sizeof(r), "Gainceiling: %d (%s). Set with: cameragainceiling <0-6>",
             v, (v >= 0 && v <= 6) ? kNames[v] : "?");
    return r;
  }
  int v = s.toInt();
  if (v < 0 || v > 6) return "Error: gainceiling must be 0..6 (2X..128X)";
  if (sens->set_gainceiling(sens, (gainceiling_t)v) == 0) {
    EXT_RAM_BSS_ATTR static char r[64];
    static const char* kNames[] = {"2X","4X","8X","16X","32X","64X","128X"};
    snprintf(r, sizeof(r), "Gainceiling set to %d (%s)", v, kNames[v]);
    return r;
  }
  return "Error: Failed";
}

// parseBoolArg(const String&) is defined in System_Utils.h. It returns
// -1 on empty, 0/1 on parsed values, -2 on unparseable input — same
// semantics this file's command handlers use.

// Shared shape for the simple on/off sensor toggles below. argsInput
// empty → report current value; valid bool → call setter; bad input →
// usage hint. `currentVal` is read from sensor_t::status; setter is the
// sensor_t function pointer.
static const char* cameraBoolToggle(const String& argsInput,
                                    const char* tag,
                                    uint8_t currentVal,
                                    int (*setter)(sensor_t*, int)) {
  if (!gCameraRunning) return "Error: Camera not enabled";
  sensor_t* s = esp_camera_sensor_get();
  if (!s || !setter) return "Error: Camera sensor not available";

  EXT_RAM_BSS_ATTR static char r[80];
  String a = argsInput; a.trim();
  if (a.length() == 0) {
    snprintf(r, sizeof(r), "%s: %s", tag, currentVal ? "ON" : "OFF");
    return r;
  }
  int p = parseBoolArg(a);
  if (p < 0) {
    snprintf(r, sizeof(r), "Usage: <on|off>  (current %s: %s)",
             tag, currentVal ? "ON" : "OFF");
    return r;
  }
  if (setter(s, p) == 0) {
    snprintf(r, sizeof(r), "%s: %s", tag, p ? "ON" : "OFF");
    return r;
  }
  return "Error: Failed";
}

const char* cmd_camerawhitebal(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  sensor_t* s = esp_camera_sensor_get();
  return cameraBoolToggle(argsInput, "whitebal", s ? s->status.awb : 0,
                          s ? s->set_whitebal : nullptr);
}

const char* cmd_cameraawbgain(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  sensor_t* s = esp_camera_sensor_get();
  return cameraBoolToggle(argsInput, "awb_gain", s ? s->status.awb_gain : 0,
                          s ? s->set_awb_gain : nullptr);
}

const char* cmd_cameraaec2(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  sensor_t* s = esp_camera_sensor_get();
  return cameraBoolToggle(argsInput, "aec2", s ? s->status.aec2 : 0,
                          s ? s->set_aec2 : nullptr);
}

const char* cmd_cameradcw(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  sensor_t* s = esp_camera_sensor_get();
  return cameraBoolToggle(argsInput, "dcw", s ? s->status.dcw : 0,
                          s ? s->set_dcw : nullptr);
}

const char* cmd_camerabpc(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  sensor_t* s = esp_camera_sensor_get();
  return cameraBoolToggle(argsInput, "bpc", s ? s->status.bpc : 0,
                          s ? s->set_bpc : nullptr);
}

const char* cmd_camerawpc(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  sensor_t* s = esp_camera_sensor_get();
  return cameraBoolToggle(argsInput, "wpc", s ? s->status.wpc : 0,
                          s ? s->set_wpc : nullptr);
}

const char* cmd_cameragamma(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  sensor_t* s = esp_camera_sensor_get();
  return cameraBoolToggle(argsInput, "raw_gma", s ? s->status.raw_gma : 0,
                          s ? s->set_raw_gma : nullptr);
}

const char* cmd_cameralenc(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  sensor_t* s = esp_camera_sensor_get();
  return cameraBoolToggle(argsInput, "lenc", s ? s->status.lenc : 0,
                          s ? s->set_lenc : nullptr);
}

// Color bar test pattern. Use to confirm the decode + display pipeline is
// honest before chasing tuning: if the colorbar renders vivid+saturated,
// the issue is exposure/gain, not the BMP build / palette / lens.
const char* cmd_cameracolorbar(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  sensor_t* s = esp_camera_sensor_get();
  return cameraBoolToggle(argsInput, "colorbar", s ? s->status.colorbar : 0,
                          s ? s->set_colorbar : nullptr);
}

// Direct sensor register poke. Escape hatch for OV3660 register tweaks
// the high-level API doesn't cover (e.g. issue #220 register fix:
// camerareg 0x3824 0x1f 0x04). Format: <addr_hex> <mask_hex> <value_hex>.
const char* cmd_camerareg(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) return "Error: Camera not enabled";
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return "Error: Camera sensor not available";

  String a = argsInput; a.trim();
  if (a.length() == 0) {
    return "Error: invalid arguments — Usage: camerareg <addr_hex> <mask_hex> <value_hex>  "
           "(example: camerareg 0x3824 0x1f 0x04)";
  }
  unsigned addr = 0, mask = 0, val = 0;
  // Accept "0x" prefix and bare hex. sscanf with %x handles both.
  if (sscanf(a.c_str(), "%x %x %x", &addr, &mask, &val) != 3) {
    return "Error: Bad format. Usage: camerareg <addr_hex> <mask_hex> <value_hex>";
  }
  if (s->set_reg(s, (int)addr, (int)mask, (int)val) == 0) {
    EXT_RAM_BSS_ATTR static char r[80];
    snprintf(r, sizeof(r), "set_reg(0x%04X, 0x%02X, 0x%02X) ok", addr, mask, val);
    return r;
  }
  return "Error: set_reg failed";
}

// Print all current sensor status values. Emits each section as its own
// broadcast line so the per-broadcast 256-byte cap in BROADCAST_PRINTF
// doesn't truncate the dump. Returns a short summary so the cmd
// dispatcher's response line shows something useful too.
//
// NOTE: these values come from sensor_t::status (software-side cache).
// On OV3660, brightness/contrast/saturation share a DSP control block
// where each setter clears the enable bits of the others — so
// status.brightness=2 doesn't guarantee the brightness bit is enabled
// in the hardware register. Use camerafx to set them together.
const char* cmd_cameradump(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) return "Error: Camera not enabled";
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return "Error: Camera sensor not available";

  static const char* kGCNames[] = {"2X","4X","8X","16X","32X","64X","128X"};
  const camera_status_t& st = s->status;
  int gc = (st.gainceiling <= 6) ? st.gainceiling : 0;

  BROADCAST_PRINTF("[CAM_DUMP] PID=0x%04X  framesize=%u  quality=%u  scale=%d",
                   (unsigned)s->id.PID, (unsigned)st.framesize,
                   (unsigned)st.quality, (int)st.scale);
  BROADCAST_PRINTF("[CAM_DUMP] brightness=%d  contrast=%d  saturation=%d  "
                   "sharpness=%d  denoise=%u  special_effect=%u",
                   (int)st.brightness, (int)st.contrast, (int)st.saturation,
                   (int)st.sharpness, (unsigned)st.denoise,
                   (unsigned)st.special_effect);
  BROADCAST_PRINTF("[CAM_DUMP] wb_mode=%u  awb=%u  awb_gain=%u",
                   (unsigned)st.wb_mode, (unsigned)st.awb, (unsigned)st.awb_gain);
  BROADCAST_PRINTF("[CAM_DUMP] aec=%u  aec2=%u  aec_value=%u  ae_level=%d",
                   (unsigned)st.aec, (unsigned)st.aec2,
                   (unsigned)st.aec_value, (int)st.ae_level);
  BROADCAST_PRINTF("[CAM_DUMP] agc=%u  agc_gain=%u  gainceiling=%u (%s)",
                   (unsigned)st.agc, (unsigned)st.agc_gain,
                   (unsigned)st.gainceiling, kGCNames[gc]);
  BROADCAST_PRINTF("[CAM_DUMP] bpc=%u  wpc=%u  raw_gma=%u  lenc=%u  dcw=%u",
                   (unsigned)st.bpc, (unsigned)st.wpc,
                   (unsigned)st.raw_gma, (unsigned)st.lenc, (unsigned)st.dcw);
  BROADCAST_PRINTF("[CAM_DUMP] hmirror=%u  vflip=%u  colorbar=%u",
                   (unsigned)st.hmirror, (unsigned)st.vflip,
                   (unsigned)st.colorbar);
  return "Sensor status dumped (see [CAM_DUMP] lines above)";
}

// Set brightness, contrast, and saturation together in one sequence.
// Workaround for the OV3660 DSP-block chained-set behaviour: each of
// set_brightness/contrast/saturation clears the other two's enable
// bits, so cycling them individually leaves only the most-recent one
// active. By calling all three back-to-back here, only the LAST call
// clears the others — but since we set all three to known values, the
// final state has all three set deterministically. (Documented in
// esphome/issues#5499.)
//
// Usage: camerafx <bri> <con> <sat>   (each -2..+2)
const char* cmd_camerafx(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) return "Error: Camera not enabled";
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return "Error: Camera sensor not available";

  String a = argsInput; a.trim();
  if (a.length() == 0) {
    EXT_RAM_BSS_ATTR static char r[96];
    snprintf(r, sizeof(r),
             "camerafx: bri=%d con=%d sat=%d. Usage: camerafx <bri> <con> <sat>  (-2..+2 each)",
             (int)s->status.brightness, (int)s->status.contrast,
             (int)s->status.saturation);
    return r;
  }
  int bri = -99, con = -99, sat = -99;
  if (sscanf(a.c_str(), "%d %d %d", &bri, &con, &sat) != 3) {
    return "Error: invalid arguments — Usage: camerafx <bri> <con> <sat>  (-2..+2 each)";
  }
  if (bri < -2 || bri > 2 || con < -2 || con > 2 || sat < -2 || sat > 2) {
    return "Error: Each value must be -2..+2";
  }

  // Order matters: write contrast first, brightness second, saturation
  // last. The last call's enable bit always wins for the *other* two,
  // but we've explicitly set all three values just before — so the
  // hardware register ends with the latest brightness/contrast values
  // (preserved since they were written into the DSP block) and the
  // saturation value (the last call).
  int rc1 = s->set_contrast(s, con);
  int rc2 = s->set_brightness(s, bri);
  int rc3 = s->set_saturation(s, sat);

  // Persist so reboot picks them up.
  setSetting(gSettings.cameraBrightness, bri);
  setSetting(gSettings.cameraContrast,   con);
  setSetting(gSettings.cameraSaturation, sat);

  EXT_RAM_BSS_ATTR static char r[120];
  snprintf(r, sizeof(r),
           "camerafx applied: bri=%d (rc=%d), con=%d (rc=%d), sat=%d (rc=%d) — saved",
           bri, rc2, con, rc1, sat, rc3);
  return r;
}

const char* cmd_camerahmirror(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) return "Error: Camera not enabled";
  
  String arg = argsInput;
  arg.trim();
  
  if (arg.length() == 0) {
    return gSettings.cameraHMirror ? "H-Mirror: ON" : "H-Mirror: OFF";
  }
  
  arg.toLowerCase();
  bool enable = (arg == "on" || arg == "1" || arg == "true");
  
  sensor_t* s = esp_camera_sensor_get();
  if (s && s->set_hmirror(s, enable ? 1 : 0) == 0) {
    setSetting(gSettings.cameraHMirror, enable);
    return enable ? "H-Mirror enabled (saved)" : "H-Mirror disabled (saved)";
  }
  return "Error: Failed";
}

const char* cmd_cameravflip(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) return "Error: Camera not enabled";
  
  String arg = argsInput;
  arg.trim();
  
  if (arg.length() == 0) {
    return gSettings.cameraVFlip ? "V-Flip: ON" : "V-Flip: OFF";
  }
  
  arg.toLowerCase();
  bool enable = (arg == "on" || arg == "1" || arg == "true");
  
  sensor_t* s = esp_camera_sensor_get();
  if (s && s->set_vflip(s, enable ? 1 : 0) == 0) {
    setSetting(gSettings.cameraVFlip, enable);
    return enable ? "V-Flip enabled (saved)" : "V-Flip disabled (saved)";
  }
  return "Error: Failed";
}

const char* cmd_camerarotate(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) return "Error: Camera not started";
  
  String arg = argsInput;
  arg.trim();
  
  if (arg.length() == 0) {
    bool rotated = gSettings.cameraHMirror && gSettings.cameraVFlip;
    return rotated ? "Rotate 180: ON (hmirror+vflip)" : "Rotate 180: OFF";
  }
  
  arg.toLowerCase();
  bool enable = (arg == "on" || arg == "1" || arg == "true" || arg == "180");
  
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_hmirror(s, enable ? 1 : 0);
    s->set_vflip(s, enable ? 1 : 0);
    setSetting(gSettings.cameraHMirror, enable);
    setSetting(gSettings.cameraVFlip, enable);
    return enable ? "Rotated 180° (hmirror+vflip enabled, saved)" : "Rotation disabled (saved)";
  }
  return "Error: Failed";
}

// ============================================================================
// Camera Settings Commands
// ============================================================================

const char* cmd_cameraautostart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) {
    return gSettings.cameraAutoStart ? "[Camera] Auto-start: enabled" : "[Camera] Auto-start: disabled";
  }
  arg.toLowerCase();
  if (arg == "on" || arg == "true" || arg == "1") {
    setSetting(gSettings.cameraAutoStart, true);
    return "[Camera] Auto-start enabled";
  } else if (arg == "off" || arg == "false" || arg == "0") {
    setSetting(gSettings.cameraAutoStart, false);
    return "[Camera] Auto-start disabled";
  }
  return "Error: invalid arguments — Usage: cameraautostart [on|off]";
}

const char* cmd_camerastoragelocation(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
  valStr.trim();
  
  if (valStr.length() == 0) {
    EXT_RAM_BSS_ATTR static char buf[64];
    snprintf(buf, sizeof(buf), "cameraStorageLocation = %d (0=LittleFS, 1=SD, 2=Both)", gSettings.cameraStorageLocation);
    return buf;
  }
  int val = valStr.toInt();
  if (val < 0 || val > 2) return "Error: cameraStorageLocation must be 0-2";
  setSetting(gSettings.cameraStorageLocation, val);
  EXT_RAM_BSS_ATTR static char buf[48];
  snprintf(buf, sizeof(buf), "cameraStorageLocation set to %d", val);
  return buf;
}

const char* cmd_cameracapturefolder(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String val = argsInput;
  val.trim();
  
  if (val.length() == 0) {
    EXT_RAM_BSS_ATTR static char buf[256];
    snprintf(buf, sizeof(buf), "cameraCaptureFolder = %s", gSettings.cameraCaptureFolder.c_str());
    return buf;
  }
  setSetting(gSettings.cameraCaptureFolder, val);
  EXT_RAM_BSS_ATTR static char buf[256];
  snprintf(buf, sizeof(buf), "cameraCaptureFolder set to %s", val.c_str());
  return buf;
}

const char* cmd_cameramaxstoredimages(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
  valStr.trim();
  
  if (valStr.length() == 0) {
    EXT_RAM_BSS_ATTR static char buf[64];
    snprintf(buf, sizeof(buf), "cameraMaxStoredImages = %d", gSettings.cameraMaxStoredImages);
    return buf;
  }
  int val = valStr.toInt();
  if (val < 0 || val > 1000) return "Error: cameraMaxStoredImages must be 0-1000";
  setSetting(gSettings.cameraMaxStoredImages, val);
  EXT_RAM_BSS_ATTR static char buf[48];
  snprintf(buf, sizeof(buf), "cameraMaxStoredImages set to %d", val);
  return buf;
}

const char* cmd_cameraautocapture(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput;
  arg.trim();
  
  if (arg.length() == 0) {
    return gSettings.cameraAutoCapture ? "cameraAutoCapture = true" : "cameraAutoCapture = false";
  }
  bool enable = (arg == "1" || arg.equalsIgnoreCase("true") || arg.equalsIgnoreCase("on"));
  setSetting(gSettings.cameraAutoCapture, enable);
  // Set default capture folder if enabling and folder is empty
  if (enable && gSettings.cameraCaptureFolder.length() == 0) {
    setSetting(gSettings.cameraCaptureFolder, String("/photos"));
  }
  return enable ? "cameraAutoCapture set to true" : "cameraAutoCapture set to false";
}

const char* cmd_cameraautocaptureinterval(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
  valStr.trim();
  
  if (valStr.length() == 0) {
    EXT_RAM_BSS_ATTR static char buf[64];
    snprintf(buf, sizeof(buf), "cameraAutoCaptureInterval = %d sec", gSettings.cameraAutoCaptureIntervalSec);
    return buf;
  }
  int val = valStr.toInt();
  if (val < 10 || val > 3600) return "Error: cameraAutoCaptureInterval must be 10-3600";
  setSetting(gSettings.cameraAutoCaptureIntervalSec, val);
  EXT_RAM_BSS_ATTR static char buf[48];
  snprintf(buf, sizeof(buf), "cameraAutoCaptureInterval set to %d sec", val);
  return buf;
}

const char* cmd_camerasendaftercapture(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput;
  arg.trim();
  
  if (arg.length() == 0) {
    return gSettings.cameraSendAfterCapture ? "cameraSendAfterCapture = true" : "cameraSendAfterCapture = false";
  }
  bool enable = (arg == "1" || arg.equalsIgnoreCase("true") || arg.equalsIgnoreCase("on"));
  setSetting(gSettings.cameraSendAfterCapture, enable);
  return enable ? "cameraSendAfterCapture set to true" : "cameraSendAfterCapture set to false";
}

const char* cmd_cameratargetdevice(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String val = argsInput;
  val.trim();
  
  if (val.length() == 0) {
    EXT_RAM_BSS_ATTR static char buf[256];
    snprintf(buf, sizeof(buf), "cameraTargetDevice = %s", gSettings.cameraTargetDevice.c_str());
    return buf;
  }
  setSetting(gSettings.cameraTargetDevice, val);
  EXT_RAM_BSS_ATTR static char buf[256];
  snprintf(buf, sizeof(buf), "cameraTargetDevice set to %s", val.c_str());
  return buf;
}

// Forward declare ImageManager for camerasave
#include "System_ImageManager.h"
extern ImageManager gImageManager;

const char* cmd_camerasave(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) {
    return "Error: Camera not enabled - run opencamera first";
  }
  
  // Determine storage location from settings
  ImageStorageLocation loc = IMAGE_STORAGE_LITTLEFS;
  if (gSettings.cameraStorageLocation == 1) loc = IMAGE_STORAGE_SD;
  else if (gSettings.cameraStorageLocation == 2) loc = IMAGE_STORAGE_BOTH;
  
  // Ensure capture folder exists and set default if needed
  if (gSettings.cameraCaptureFolder.length() == 0) {
    setSetting(gSettings.cameraCaptureFolder, String("/photos"));
  }
  
  // Capture and save
  String savedPath = gImageManager.captureAndSave(loc);
  if (savedPath.length() > 0) {
    EXT_RAM_BSS_ATTR static char result[128];
    snprintf(result, sizeof(result), "Saved: %s", savedPath.c_str());
    return result;
  }
  return "Error: Failed to save image";
}

// ── Video recording commands ────────────────────────────────────────────────
// These forward to System_Camera_Video. Kept here so they register alongside
// the rest of the camera command table.
EXT_RAM_BSS_ATTR static char gCameraCmdBuffer[192];

const char* cmd_camerarecord(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gCameraRunning) return "Error: Camera not enabled - run opencamera first";

  String arg = argsInput;
  arg.trim();

  if (arg.length() == 0) {
    return videoRecording ? "Recording: active" : "Recording: stopped";
  }
  if (arg == "1" || arg.equalsIgnoreCase("start")) {
    return startVideoRecording() ? "Recording started"
                                 : "Error: Failed to start recording (SD card available?)";
  }
  if (arg == "0" || arg.equalsIgnoreCase("stop")) {
    bool wasRecording = videoRecording;
    stopVideoRecording();
    if (!wasRecording) return "Recording stopped";
    EXT_RAM_BSS_ATTR static char out[160];
    snprintf(out, sizeof(out), "Recording stopped — %s (%lu frames)",
             videoLastRecordingPath(), (unsigned long)videoLastRecordingFrames());
    return out;
  }
  return "Error: invalid arguments — Usage: camerarecord <start|stop|1|0>";
}

const char* cmd_cameravideolist(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    doc["count"] = getVideoRecordingCount();
    JsonArray arr = doc["recordings"].to<JsonArray>();
    String list = getVideoRecordingsList();  // "name:size\nname:size"
    int start = 0;
    while (start < (int)list.length()) {
      int nl = list.indexOf('\n', start);
      String entry = (nl < 0) ? list.substring(start) : list.substring(start, nl);
      entry.trim();
      if (entry.length()) {
        int colon = entry.lastIndexOf(':');
        JsonObject o = arr.add<JsonObject>();
        if (colon > 0) { o["filename"] = entry.substring(0, colon); o["size"] = entry.substring(colon + 1).toInt(); }
        else           { o["filename"] = entry; }
      }
      if (nl < 0) break;
      start = nl + 1;
    }
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }

  int count = getVideoRecordingCount();
  if (count == 0) return "No video recordings found";
  String list = getVideoRecordingsList();
  snprintf(gCameraCmdBuffer, sizeof(gCameraCmdBuffer),
           "Recordings (%d):\n%s", count, list.c_str());
  return gCameraCmdBuffer;
}

const char* cmd_cameravideodelete(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);
  String name;
  const char* qerr = requireQuotedToken(a, 0, name);
  if (qerr) return qerr;
  if (a.has(1)) return "Error: unexpected argument — usage: cameravideodelete \"<filename>\"";
  return deleteVideoRecording(name) ? "Deleted" : "Error: Delete failed (file not found or SD unavailable)";
}

// Command registry
// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
const CommandEntry cameraCommands[] = {
  {"cameraread",       "Read camera status",              false, cmd_camera},
  {"opencamera",       "Start camera sensor.",            false, cmd_camerastart},
  {"closecamera",      "Stop camera sensor.",             false, cmd_camerastop},
  {"cameracapture",    "Capture a single frame",          false, cmd_cameracapture},
  {"camerasave",       "Save current frame to storage",   false, cmd_camerasave},
  {"camerares",        "Set camera resolution: <res>",    false, cmd_camerares, "Usage: camerares <96x96|qqvga|qcif|hqvga|240x240|qvga|cif|vga|svga|xga|sxga|uxga>"},
  {"cameraframesize",  "Set resolution by index: <0-10>", true,  cmd_cameraframesize, "Usage: cameraframesize <0..10> (0-5: QVGA..UXGA, 6-10: 96x96/QQVGA/QCIF/HQVGA/240x240)"},
  {"cameraquality",    "Set JPEG quality: <0-63>",        false, cmd_cameraquality, "Usage: cameraquality <0..63> (lower = better quality, larger file)"},
  {"camerafps",            "Camera FPS: <1-20>",          true, cmd_camerafps, "Usage: camerafps <1..20>"},
  {"cameratiny",       "Capture tiny frame for ESP-NOW",  false, cmd_cameratiny},
  {"camerabrightness", "Set brightness: <-2..2>",         false, cmd_camerabrightness, "Usage: camerabrightness <-2..2>"},
  {"cameracontrast",   "Set contrast: <-2..2>",           false, cmd_cameracontrast, "Usage: cameracontrast <-2..2>"},
  {"camerasaturation", "Set saturation: <-2..2>",         false, cmd_camerasaturation, "Usage: camerasaturation <-2..2>"},
  {"camerawb",         "White balance mode: <0-4>",       true,  cmd_camerawb, "Usage: camerawb <0..4> (0=Auto,1=Sunny,2=Cloudy,3=Office,4=Home)"},
  {"camerasharpness",  "Set sharpness: <-2..2>",          true,  cmd_camerasharpness, "Usage: camerasharpness <-2..2> (OV3660 only)"},
  {"cameradenoise",    "Set denoise level: <0-8>",        true,  cmd_cameradenoise, "Usage: cameradenoise <0..8>"},
  {"cameraeffect",     "Special effect: <0-6>",           true,  cmd_cameraeffect, "Usage: cameraeffect <0..6> (0=None,1=Negative,2=Grayscale,3=Red,4=Green,5=Blue,6=Sepia)"},
  {"cameraexposure",   "Set AE level: <-2..2>",           true,  cmd_cameraexposure, "Usage: cameraexposure <-2..2> (negative=darker)"},
  {"cameraaec",        "Auto exposure: <on|off>",         true,  cmd_cameraaec, "Usage: cameraaec <on|off|1|0|true|auto>"},
  {"cameraaecvalue",   "Exposure value: <0-1200>",        true,  cmd_cameraaecvalue, "Usage: cameraaecvalue <0..1200>"},
  {"cameraagc",        "Auto gain: <on|off>",             true,  cmd_cameraagc, "Usage: cameraagc <on|off|1|0|true|auto>"},
  {"cameraagcgain",    "Gain value: <0-30>",              true,  cmd_cameraagcgain, "Usage: cameraagcgain <0..30>"},
  // ── Runtime sensor tuning (no persistence — for testing OV3660 image quality) ──
  {"cameragainceiling","Gainceiling: <0-6> (2X..128X)",   true,  cmd_cameragainceiling, "Usage: cameragainceiling <0..6> (2X..128X)"},
  {"camerawhitebal",   "AWB master: <on|off>",            true,  cmd_camerawhitebal, "Usage: camerawhitebal <on|off>"},
  {"cameraawbgain",    "AWB gain: <on|off>",              true,  cmd_cameraawbgain, "Usage: cameraawbgain <on|off>"},
  {"cameraaec2",       "Alt AEC algorithm: <on|off>",     true,  cmd_cameraaec2, "Usage: cameraaec2 <on|off>"},
  {"cameradcw",        "Downsize crop window: <on|off>",  true,  cmd_cameradcw, "Usage: cameradcw <on|off>"},
  {"camerabpc",        "Black pixel correction: <on|off>",true,  cmd_camerabpc, "Usage: camerabpc <on|off>"},
  {"camerawpc",        "White pixel correction: <on|off>",true,  cmd_camerawpc, "Usage: camerawpc <on|off>"},
  {"cameragamma",      "Raw gamma: <on|off>",             true,  cmd_cameragamma, "Usage: cameragamma <on|off>"},
  {"cameralenc",       "Lens shading correction: <on|off>",true, cmd_cameralenc, "Usage: cameralenc <on|off>"},
  {"cameracolorbar",   "Color bar test pattern: <on|off>",true,  cmd_cameracolorbar, "Usage: cameracolorbar <on|off>"},
  {"camerareg",        "Direct register write: <addr_hex> <mask_hex> <value_hex>", true, cmd_camerareg, "Usage: camerareg <addr_hex> <mask_hex> <value_hex> (example: camerareg 0x3824 0x1f 0x04)"},
  {"cameradump",       "Print all current sensor values", false, cmd_cameradump},
  {"camerafx",         "Set bri/con/sat together: <bri> <con> <sat> (-2..+2 each)", false, cmd_camerafx, "Usage: camerafx <bri> <con> <sat> (-2..+2 each)"},
  {"camerahmirror",    "Horizontal mirror: <on|off>",     false, cmd_camerahmirror, "Usage: camerahmirror <on|off|1|0|true>"},
  {"cameravflip",      "Vertical flip: <on|off>",         false, cmd_cameravflip, "Usage: cameravflip <on|off|1|0|true>"},
  {"camerarotate",     "Rotate 180°: <on|off>",           false, cmd_camerarotate, "Usage: camerarotate <on|off|1|0|true|180>"},
  {"cameraautostart",  "Auto-start: <on|off>",            true,  cmd_cameraautostart, "Usage: cameraautostart <on|off|1|0|true|false>"},
  {"camerastoragelocation", "Storage location: <0-2>",    true,  cmd_camerastoragelocation, "Usage: camerastoragelocation <0..2> (0=LittleFS,1=SD,2=Both)"},
  {"cameracapturefolder",   "Photo folder: <path>",       true,  cmd_cameracapturefolder, "Usage: cameracapturefolder <path>"},
  {"cameramaxstoredimages", "Max stored: <0-1000>",       true,  cmd_cameramaxstoredimages, "Usage: cameramaxstoredimages <0..1000> (0=unlimited)"},
  {"cameraautocapture",     "Auto-capture: <on|off>",     true,  cmd_cameraautocapture, "Usage: cameraautocapture <on|off|1|0|true>"},
  {"cameraautocaptureinterval", "Auto-capture: <sec>",    true, cmd_cameraautocaptureinterval, "Usage: cameraautocaptureinterval <10..3600>"},
  {"camerasendaftercapture", "Send after capture: <on|off>", true, cmd_camerasendaftercapture, "Usage: camerasendaftercapture <on|off|1|0|true>"},
  {"cameratargetdevice",    "Target device: <name>",      true,  cmd_cameratargetdevice, "Usage: cameratargetdevice <name>"},
  {"camerarecord",          "Start/stop MJPEG-AVI recording (SD only): <start|stop>", false, cmd_camerarecord, "Usage: camerarecord <start|stop|1|0>"},
  {"cameravideolist",       "List AVI recordings on SD (add 'json' for JSON output)",  false, cmd_cameravideolist},
  {"cameravideodelete",     "Delete recording: \"<filename>\"", true, cmd_cameravideodelete, "Usage: cameravideodelete \"<filename>\""},
};

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry cameraSettingEntries[] = {
  { "cameraEnabled", SETTING_BOOL, &gSettings.cameraEnabled, 1, 0, nullptr, 0, 1, "Enabled", nullptr, false, nullptr, "cameraenabled" },
  { "cameraAutoStart", SETTING_BOOL, &gSettings.cameraAutoStart, 0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr, false, nullptr, "cameraautostart" },
  { "cameraFramesize", SETTING_INT, &gSettings.cameraFramesize, 10, 0, nullptr, 0, 10, "Resolution", "0:320x240 (QVGA),1:640x480 (VGA),2:800x600 (SVGA),3:1024x768 (XGA),4:1280x1024 (SXGA),5:1600x1200 (UXGA),"
    "6:96x96,7:160x120 (QQVGA),8:176x144 (QCIF),9:240x176 (HQVGA),10:240x240", false, "image", nullptr },
  { "cameraBrightness", SETTING_INT, &gSettings.cameraBrightness, 2, 0, nullptr, -2, 2, "Brightness (-2 to 2)", nullptr, false, "tuning", "camerabrightness" },
  { "cameraContrast", SETTING_INT, &gSettings.cameraContrast, 2, 0, nullptr, -2, 2, "Contrast (-2 to 2)", nullptr, false, "tuning", "cameracontrast" },
  { "cameraSaturation", SETTING_INT, &gSettings.cameraSaturation, 2, 0, nullptr, -2, 2, "Saturation (-2 to 2)", nullptr, false, "tuning", "camerasaturation" },
  { "cameraAELevel", SETTING_INT, &gSettings.cameraAELevel, 0, 0, nullptr, -2, 2, "Exposure Compensation (-2 to 2)", nullptr, false, "tuning", "cameraexposure" },
  { "cameraWBMode", SETTING_INT, &gSettings.cameraWBMode, 0, 0, nullptr, 0, 4, "White Balance", "0:Auto,1:Sunny,2:Cloudy,3:Office,4:Home", false, "tuning", "camerawb" },
  { "cameraSharpness", SETTING_INT, &gSettings.cameraSharpness, 0, 0, nullptr, -2, 2, "Sharpness (-2 to 2, OV3660)", nullptr, false, "tuning", "camerasharpness" },
  { "cameraDenoise", SETTING_INT, &gSettings.cameraDenoise, 0, 0, nullptr, 0, 8, "Denoise (0-8)", nullptr, false, "tuning", "cameradenoise" },
  { "cameraSpecialEffect", SETTING_INT, &gSettings.cameraSpecialEffect, 0, 0, nullptr, 0, 6, "Special Effect", "0:None,1:Negative,2:Grayscale,3:Red Tint,4:Green Tint,5:Blue Tint,6:Sepia", false, "tuning", "cameraeffect" },
  { "cameraHMirror", SETTING_BOOL, &gSettings.cameraHMirror, 0, 0, nullptr, 0, 1, "Horizontal mirror", nullptr, false, "image", "camerahmirror" },
  { "cameraVFlip", SETTING_BOOL, &gSettings.cameraVFlip, 0, 0, nullptr, 0, 1, "Vertical flip", nullptr, false, "image", "cameravflip" },
  { "cameraQuality", SETTING_INT, &gSettings.cameraQuality, 12, 0, nullptr, 0, 63, "JPEG quality (0-63, lower=better)", nullptr, false, "image", "cameraquality" },
  { "cameraStreamFps", SETTING_INT, &gSettings.cameraStreamFps, 5, 0, nullptr, 1, 20, "Camera FPS (higher=smoother)", nullptr, false, "image", "camerafps" },
  { "g2StreamToneMap", SETTING_INT, &gSettings.g2StreamToneMap, 1, 0, nullptr, 0, 3,
    "G2 lens 4-bpp tone", "0:Linear,1:Balanced,2:Shadows,3:Legacy", false, "image", "g2streamtonemap" },
  { "g2PackRateMs", SETTING_INT, &gSettings.g2PackRateMs, 80, 0, nullptr, 20, 2000, "G2 SD-pack animation cadence (ms per frame)", nullptr, false, "image", "g2packrate" },
  { "cameraStorageLocation", SETTING_INT, &gSettings.cameraStorageLocation, 1, 0, nullptr, 0, 2, "Storage Location", "0:LittleFS (Internal),1:SD Card,2:Both", false, "storage", "camerastoragelocation" },
  { "cameraCaptureFolder", SETTING_STRING, &gSettings.cameraCaptureFolder, 0, 0, "/photos", 0, 0, "Photo folder path", nullptr, false, "storage", "cameracapturefolder" },
  { "cameraMaxStoredImages", SETTING_INT, &gSettings.cameraMaxStoredImages, 100, 0, nullptr, 0, 1000, "Max images (0=unlimited)", nullptr, false, "storage", "cameramaxstoredimages" },
  { "cameraAutoCapture", SETTING_BOOL, &gSettings.cameraAutoCapture, 0, 0, nullptr, 0, 1, "Enable auto-capture", nullptr, false, "autoCapture", "cameraautocapture" },
  { "cameraAutoCaptureInterval", SETTING_INT, &gSettings.cameraAutoCaptureIntervalSec, 60, 0, nullptr, 10, 3600, "Auto-capture interval (sec)", nullptr, false, "autoCapture", "cameraautocaptureinterval" },
  { "cameraSendAfterCapture", SETTING_BOOL, &gSettings.cameraSendAfterCapture, 0, 0, nullptr, 0, 1, "Send to target after capture", nullptr, false, "autoCapture", "camerasendaftercapture" },
  { "cameraTargetDevice", SETTING_STRING, &gSettings.cameraTargetDevice, 0, 0, nullptr, 0, 0, "ESP-NOW target device name", nullptr, false, "autoCapture", "cameratargetdevice" },
};

static bool isCameraConnected() {
  if (!gCameraRunning) return true;
  return cameraConnected;
}

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule cameraSettingsModule = {
  "camera",
  "hardware.sensors.camera",
  cameraSettingEntries,
  sizeof(cameraSettingEntries) / sizeof(cameraSettingEntries[0]),
  isCameraConnected,
  "ESP32-S3 camera sensor"
};
const size_t cameraCommandsCount = sizeof(cameraCommands) / sizeof(cameraCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp

#endif // ENABLE_CAMERA_SENSOR
