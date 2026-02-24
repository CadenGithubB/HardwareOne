/**
 * Camera Sensor Module - ESP32-S3 DVP Camera Implementation
 * 
 * Supports OV2640, OV3660, and OV5640 cameras on XIAO ESP32S3 Sense
 */

#include "System_Camera_DVP.h"
#include "System_BuildConfig.h"

#if ENABLE_CAMERA_SENSOR

#include <Arduino.h>
#include <Wire.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_camera.h"
#include "System_Debug.h"
#include "System_MemUtil.h"
#include "System_Command.h"
#include "System_Settings.h"
#include <ArduinoJson.h>

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
bool cameraEnabled = false;
bool cameraConnected = false;
bool cameraStreaming = false;
const char* cameraModel = "Unknown";
int cameraWidth = 0;
int cameraHeight = 0;

static framesize_t cameraFramesizeFromSetting(int v) {
  // Simplified to only confirmed working resolutions
  static const framesize_t kMap[] = {
    FRAMESIZE_QVGA,     // 0 (320x240)
    FRAMESIZE_VGA,      // 1 (640x480)
    FRAMESIZE_SVGA,     // 2 (800x600)
    FRAMESIZE_XGA,      // 3 (1024x768)
    FRAMESIZE_SXGA,     // 4 (1280x1024)
    FRAMESIZE_UXGA      // 5 (1600x1200)
  };

  if (v >= 0 && v < (int)(sizeof(kMap) / sizeof(kMap[0]))) {
    return kMap[v];
  }

  // Default to VGA if invalid
  return FRAMESIZE_VGA;
}

static int cameraFramesizeSettingFromEnum(framesize_t fs) {
  // Match the simplified resolution list
  switch (fs) {
    case FRAMESIZE_QVGA: return 0;
    case FRAMESIZE_VGA:  return 1;
    case FRAMESIZE_SVGA: return 2;
    case FRAMESIZE_XGA:  return 3;
    case FRAMESIZE_SXGA: return 4;
    case FRAMESIZE_UXGA: return 5;
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

bool initCamera() {
  DEBUG_CAMERAF("[CAM_INIT] ========== initCamera() ENTRY ==========");
  DEBUG_CAMERAF("[CAM_INIT] cameraEnabled=%d cameraConnected=%d", cameraEnabled, cameraConnected);
  DEBUG_CAMERAF("[CAM_INIT] Heap free: %u, PSRAM free: %u", esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  if (!lockCameraMutex(15000)) {
    DEBUG_CAMERAF("[CAM_INIT] ERROR: camera mutex timeout (camera busy)");
    return false;
  }
  
  if (cameraEnabled) {
    DEBUG_CAMERAF("[CAM_INIT] Already initialized - returning true");
    INFO_SENSORSF("[Camera] Already initialized");
    unlockCameraMutex();
    return true;
  }

  DEBUG_CAMERAF("[CAM_INIT] Starting initialization...");
  INFO_SENSORSF("[Camera] Initializing camera...");
  DEBUG_CAMERAF("[CAM_INIT] gSettings: framesize=%d quality=%d brightness=%d contrast=%d saturation=%d",
                gSettings.cameraFramesize, gSettings.cameraQuality,
                gSettings.cameraBrightness, gSettings.cameraContrast, gSettings.cameraSaturation);
  DEBUG_CAMERAF("[CAM_INIT] gSettings: hmirror=%d vflip=%d aeLevel=%d",
                gSettings.cameraHMirror, gSettings.cameraVFlip, gSettings.cameraAELevel);
  INFO_SENSORSF("[Camera] Settings from gSettings: framesize=%d quality=%d brightness=%d contrast=%d",
                gSettings.cameraFramesize, gSettings.cameraQuality,
                gSettings.cameraBrightness, gSettings.cameraContrast);

  DEBUG_CAMERAF("[CAM_INIT] Creating camera_config_t struct...");
  camera_config_t config;
  memset(&config, 0, sizeof(config));  // Zero-initialize for safety
  DEBUG_CAMERAF("[CAM_INIT] config struct zeroed, size=%u bytes", sizeof(config));
  
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  DEBUG_CAMERAF("[CAM_INIT] LEDC: channel=%d timer=%d", config.ledc_channel, config.ledc_timer);
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
  
  DEBUG_CAMERAF("[CAM_INIT] GPIO pins configured:");
  DEBUG_CAMERAF("[CAM_INIT]   D0-D7: %d %d %d %d %d %d %d %d", 
                config.pin_d0, config.pin_d1, config.pin_d2, config.pin_d3,
                config.pin_d4, config.pin_d5, config.pin_d6, config.pin_d7);
  DEBUG_CAMERAF("[CAM_INIT]   XCLK=%d PCLK=%d VSYNC=%d HREF=%d",
                config.pin_xclk, config.pin_pclk, config.pin_vsync, config.pin_href);
  DEBUG_CAMERAF("[CAM_INIT]   SDA=%d SCL=%d PWDN=%d RESET=%d",
                config.pin_sccb_sda, config.pin_sccb_scl, config.pin_pwdn, config.pin_reset);
  
  // === DEBUG: Log GPIO states before init ===
  DEBUG_CAMERAF("[CAM_INIT] === GPIO STATE CHECK (before init) ===");
  // Data pins
  for (int i = 0; i < 8; i++) {
    int pins[] = {config.pin_d0, config.pin_d1, config.pin_d2, config.pin_d3,
                  config.pin_d4, config.pin_d5, config.pin_d6, config.pin_d7};
    if (pins[i] >= 0) {
      DEBUG_CAMERAF("[CAM_INIT] GPIO D%d (pin %d): level=%d", i, pins[i], gpio_get_level((gpio_num_t)pins[i]));
    }
  }
  // Control pins
  if (config.pin_xclk >= 0) DEBUG_CAMERAF("[CAM_INIT] GPIO XCLK (pin %d): configured for LEDC output", config.pin_xclk);
  if (config.pin_pclk >= 0) DEBUG_CAMERAF("[CAM_INIT] GPIO PCLK (pin %d): level=%d", config.pin_pclk, gpio_get_level((gpio_num_t)config.pin_pclk));
  if (config.pin_vsync >= 0) DEBUG_CAMERAF("[CAM_INIT] GPIO VSYNC (pin %d): level=%d", config.pin_vsync, gpio_get_level((gpio_num_t)config.pin_vsync));
  if (config.pin_href >= 0) DEBUG_CAMERAF("[CAM_INIT] GPIO HREF (pin %d): level=%d", config.pin_href, gpio_get_level((gpio_num_t)config.pin_href));
  if (config.pin_sccb_sda >= 0) DEBUG_CAMERAF("[CAM_INIT] GPIO SDA (pin %d): level=%d", config.pin_sccb_sda, gpio_get_level((gpio_num_t)config.pin_sccb_sda));
  if (config.pin_sccb_scl >= 0) DEBUG_CAMERAF("[CAM_INIT] GPIO SCL (pin %d): level=%d", config.pin_sccb_scl, gpio_get_level((gpio_num_t)config.pin_sccb_scl));
  if (config.pin_pwdn >= 0) DEBUG_CAMERAF("[CAM_INIT] GPIO PWDN (pin %d): level=%d", config.pin_pwdn, gpio_get_level((gpio_num_t)config.pin_pwdn));
  if (config.pin_reset >= 0) DEBUG_CAMERAF("[CAM_INIT] GPIO RESET (pin %d): level=%d", config.pin_reset, gpio_get_level((gpio_num_t)config.pin_reset));

  // === DEBUG: Manual power/reset sequence with timing ===
  DEBUG_CAMERAF("[CAM_INIT] === POWER/RESET SEQUENCE ===");
  if (config.pin_pwdn >= 0) {
    DEBUG_CAMERAF("[CAM_INIT] Toggling PWDN pin %d: HIGH (power down)...", config.pin_pwdn);
    gpio_set_direction((gpio_num_t)config.pin_pwdn, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)config.pin_pwdn, 1);  // Power down
    vTaskDelay(pdMS_TO_TICKS(10));
    DEBUG_CAMERAF("[CAM_INIT] PWDN pin %d: LOW (power up)...", config.pin_pwdn);
    gpio_set_level((gpio_num_t)config.pin_pwdn, 0);  // Power up
    vTaskDelay(pdMS_TO_TICKS(10));
    DEBUG_CAMERAF("[CAM_INIT] PWDN sequence complete, level now=%d", gpio_get_level((gpio_num_t)config.pin_pwdn));
  }
  if (config.pin_reset >= 0) {
    DEBUG_CAMERAF("[CAM_INIT] Toggling RESET pin %d: LOW (reset active)...", config.pin_reset);
    gpio_set_direction((gpio_num_t)config.pin_reset, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)config.pin_reset, 0);  // Reset active
    vTaskDelay(pdMS_TO_TICKS(10));
    DEBUG_CAMERAF("[CAM_INIT] RESET pin %d: HIGH (reset released)...", config.pin_reset);
    gpio_set_level((gpio_num_t)config.pin_reset, 1);  // Reset released
    vTaskDelay(pdMS_TO_TICKS(10));
    DEBUG_CAMERAF("[CAM_INIT] RESET sequence complete, level now=%d", gpio_get_level((gpio_num_t)config.pin_reset));
  }
  DEBUG_CAMERAF("[CAM_INIT] Waiting 100ms for camera to stabilize after power/reset...");
  vTaskDelay(pdMS_TO_TICKS(100));

  // === DEBUG: SCCB/I2C Probe for camera ===
  DEBUG_CAMERAF("[CAM_INIT] === SCCB/I2C PROBE ===");
  DEBUG_CAMERAF("[CAM_INIT] Probing for camera on SCCB bus (SDA=%d, SCL=%d)...", config.pin_sccb_sda, config.pin_sccb_scl);
  // Common OV camera I2C addresses: 0x30 (OV2640 write), 0x3C (OV3660/OV5640 write)
  // We'll try a simple I2C scan using Wire
  Wire.begin(config.pin_sccb_sda, config.pin_sccb_scl, 100000);  // 100kHz for SCCB
  uint8_t camAddrs[] = {0x30, 0x3C, 0x21, 0x1E};  // Common camera addresses
  bool foundCam = false;
  for (int i = 0; i < sizeof(camAddrs); i++) {
    Wire.beginTransmission(camAddrs[i]);
    uint8_t err = Wire.endTransmission();
    DEBUG_CAMERAF("[CAM_INIT] SCCB probe 0x%02X: %s", camAddrs[i], err == 0 ? "FOUND!" : (err == 2 ? "NACK" : "Error"));
    if (err == 0) foundCam = true;
  }
  Wire.end();  // Release I2C for camera driver
  if (!foundCam) {
    DEBUG_CAMERAF("[CAM_INIT] *** WARNING: No camera found on SCCB bus! Check connections! ***");
    INFO_SENSORSF("[Camera] WARNING: No camera detected on I2C bus!");
  }

  // Start with conservative defaults - OV3660 is sensitive
  framesize_t fs = cameraFramesizeFromSetting(gSettings.cameraFramesize);
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
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;  // Default, changed to LATEST if PSRAM
  
  DEBUG_CAMERAF("[CAM_INIT] Initial config: xclk=%dHz fs=%d pix=%d fb_loc=%d qual=%d fb_cnt=%d grab=%d",
                config.xclk_freq_hz, config.frame_size, config.pixel_format,
                config.fb_location, config.jpeg_quality, config.fb_count, config.grab_mode);
  
  // OV3660 fix: Use conservative grab mode even with PSRAM to avoid FB-OVF/timeout
  DEBUG_CAMERAF("[CAM_INIT] Checking PSRAM...");
  bool hasPsram = psramFound();
  DEBUG_CAMERAF("[CAM_INIT] psramFound() = %d", hasPsram);
  if (hasPsram) {
    config.jpeg_quality = 10;  // Higher quality when PSRAM available
    config.fb_count = 2;       // Need 2 buffers for GRAB_LATEST - DMA fills one while other is processed
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    DEBUG_CAMERAF("[CAM_INIT] PSRAM found - using quality=10, fb_count=2, GRAB_WHEN_EMPTY");
  } else {
    // Fallback for no PSRAM - reduce resolution and use internal DRAM
    if (config.frame_size > FRAMESIZE_SVGA) {
      config.frame_size = FRAMESIZE_SVGA;
      INFO_SENSORSF("[Camera] No PSRAM detected, limiting to SVGA resolution");
    }
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }

  INFO_SENSORSF("[Camera] Config: xclk=%dMHz framesize=%d quality=%d fb_count=%d",
                config.xclk_freq_hz / 1000000, config.frame_size, 
                config.jpeg_quality, config.fb_count);

  // Initialize the camera
  DEBUG_CAMERAF("[CAM_INIT] Final config before esp_camera_init():");
  DEBUG_CAMERAF("[CAM_INIT]   xclk_freq_hz=%d", config.xclk_freq_hz);
  DEBUG_CAMERAF("[CAM_INIT]   frame_size=%d pixel_format=%d", config.frame_size, config.pixel_format);
  DEBUG_CAMERAF("[CAM_INIT]   fb_location=%d jpeg_quality=%d", config.fb_location, config.jpeg_quality);
  DEBUG_CAMERAF("[CAM_INIT]   fb_count=%d grab_mode=%d", config.fb_count, config.grab_mode);
  DEBUG_CAMERAF("[CAM_INIT] Heap before esp_camera_init: %u", esp_get_free_heap_size());
  DEBUG_CAMERAF("[CAM_INIT] Calling esp_camera_init()...");
  
  unsigned long initStart = millis();
  esp_err_t err = esp_camera_init(&config);
  unsigned long initTime = millis() - initStart;
  
  DEBUG_CAMERAF("[CAM_INIT] esp_camera_init() returned 0x%x after %lu ms", err, initTime);
  DEBUG_CAMERAF("[CAM_INIT] Error decode: %s", cameraErrorToString(err));
  DEBUG_CAMERAF("[CAM_INIT] Heap after esp_camera_init: %u", esp_get_free_heap_size());
  
  if (err != ESP_OK) {
    DEBUG_CAMERAF("[CAM_INIT] *** INIT FAILED! ***");
    DEBUG_CAMERAF("[CAM_INIT] Error code: 0x%x", err);
    DEBUG_CAMERAF("[CAM_INIT] Error meaning: %s", cameraErrorToString(err));
    DEBUG_CAMERAF("[CAM_INIT] Possible causes:");
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_STATE || err == 0x20001) {
      DEBUG_CAMERAF("[CAM_INIT]   - Camera not connected or bad ribbon cable");
      DEBUG_CAMERAF("[CAM_INIT]   - SCCB/I2C communication failed");
      DEBUG_CAMERAF("[CAM_INIT]   - Wrong I2C address for camera model");
      DEBUG_CAMERAF("[CAM_INIT]   - PWDN/RESET pins not configured correctly");
    } else if (err == ESP_ERR_NO_MEM) {
      DEBUG_CAMERAF("[CAM_INIT]   - Not enough memory for frame buffers");
      DEBUG_CAMERAF("[CAM_INIT]   - Try reducing resolution or fb_count");
    } else if (err == ESP_ERR_TIMEOUT) {
      DEBUG_CAMERAF("[CAM_INIT]   - Camera not responding (check XCLK)");
      DEBUG_CAMERAF("[CAM_INIT]   - DVP timing issue");
    }
    INFO_SENSORSF("[Camera] Init failed: 0x%x (%s)", err, cameraErrorToString(err));
    cameraConnected = false;
    cameraEnabled = false;
    unlockCameraMutex();
    return false;
  }
  
  DEBUG_CAMERAF("[CAM_INIT] esp_camera_init() SUCCESS");
  INFO_SENSORSF("[Camera] esp_camera_init() succeeded");

  // Get camera sensor info
  DEBUG_CAMERAF("[CAM_INIT] Getting camera sensor handle...");
  sensor_t* s = esp_camera_sensor_get();
  DEBUG_CAMERAF("[CAM_INIT] esp_camera_sensor_get() returned %p", s);
  
  if (s) {
    DEBUG_CAMERAF("[CAM_INIT] Sensor info: PID=0x%x VER=0x%x MIDL=0x%x MIDH=0x%x",
                  s->id.PID, s->id.VER, s->id.MIDL, s->id.MIDH);
    INFO_SENSORSF("[Camera] Sensor PID=0x%x", s->id.PID);
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
    DEBUG_CAMERAF("[CAM_INIT] Detected camera model: %s", cameraModel);
    INFO_SENSORSF("[Camera] Detected: %s", cameraModel);
    
    // OV3660 specific: needs time to stabilize before changing settings
    if (s->id.PID == OV3660_PID) {
      DEBUG_CAMERAF("[CAM_INIT] OV3660 detected - waiting 500ms for stabilization");
      INFO_SENSORSF("[Camera] OV3660 detected - waiting 500ms for sensor stabilization...");
      vTaskDelay(pdMS_TO_TICKS(500));
      DEBUG_CAMERAF("[CAM_INIT] OV3660 stabilization wait complete");
    }
    
    // Flush any garbage frames BEFORE applying settings
    // OV3660 needs more flushes to clear overflow state
    DEBUG_CAMERAF("[CAM_INIT] Starting frame flush phase...");
    INFO_SENSORSF("[Camera] Flushing initial frames...");
    int flushCount = (s->id.PID == OV3660_PID) ? 5 : 3;
    DEBUG_CAMERAF("[CAM_INIT] Will flush %d frames", flushCount);
    
    for (int i = 0; i < flushCount; i++) {
      DEBUG_CAMERAF("[CAM_INIT] Flush %d: calling esp_camera_fb_get()...", i);
      unsigned long flushStart = millis();
      camera_fb_t* fb = esp_camera_fb_get();
      unsigned long flushTime = millis() - flushStart;
      
      if (fb) {
        DEBUG_CAMERAF("[CAM_INIT] Flush %d: got frame in %lu ms - len=%u format=%d w=%u h=%u",
                      i, flushTime, fb->len, fb->format, fb->width, fb->height);
        if (fb->format == PIXFORMAT_JPEG && fb->len >= 2) {
          DEBUG_CAMERAF("[CAM_INIT] Flush %d: JPEG header bytes: 0x%02X 0x%02X",
                        i, fb->buf[0], fb->buf[1]);
        }
        INFO_SENSORSF("[Camera] Flush frame %d: %u bytes, format=%d", i, fb->len, fb->format);
        esp_camera_fb_return(fb);
        DEBUG_CAMERAF("[CAM_INIT] Flush %d: frame returned to camera", i);
      } else {
        DEBUG_CAMERAF("[CAM_INIT] Flush %d: TIMEOUT after %lu ms - fb is NULL!", i, flushTime);
        INFO_SENSORSF("[Camera] Flush frame %d: NULL (timeout)", i);
        // Don't break - keep trying to clear overflow
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    DEBUG_CAMERAF("[CAM_INIT] Frame flush phase complete");
    
    // NOW apply user settings (after camera has stabilized)
    DEBUG_CAMERAF("[CAM_INIT] Applying user settings phase...");
    INFO_SENSORSF("[Camera] Applying user settings...");
    
    DEBUG_CAMERAF("[CAM_INIT] set_framesize(%d)...", fs);
    int r1 = s->set_framesize(s, fs);
    DEBUG_CAMERAF("[CAM_INIT] set_framesize returned %d", r1);
    
    DEBUG_CAMERAF("[CAM_INIT] set_quality(%d)...", jpegQ);
    int r2 = s->set_quality(s, jpegQ);
    DEBUG_CAMERAF("[CAM_INIT] set_quality returned %d", r2);
    
    DEBUG_CAMERAF("[CAM_INIT] set_brightness(%d)...", gSettings.cameraBrightness);
    s->set_brightness(s, gSettings.cameraBrightness);
    
    DEBUG_CAMERAF("[CAM_INIT] set_contrast(%d)...", gSettings.cameraContrast);
    s->set_contrast(s, gSettings.cameraContrast);
    
    DEBUG_CAMERAF("[CAM_INIT] set_saturation(%d)...", gSettings.cameraSaturation);
    s->set_saturation(s, gSettings.cameraSaturation);
    
    DEBUG_CAMERAF("[CAM_INIT] set_hmirror(%d)...", gSettings.cameraHMirror ? 1 : 0);
    s->set_hmirror(s, gSettings.cameraHMirror ? 1 : 0);
    
    DEBUG_CAMERAF("[CAM_INIT] set_vflip(%d)...", gSettings.cameraVFlip ? 1 : 0);
    s->set_vflip(s, gSettings.cameraVFlip ? 1 : 0);
    
    // Standard settings
    DEBUG_CAMERAF("[CAM_INIT] Applying standard settings (AWB, AE, gain, etc.)...");
    s->set_special_effect(s, gSettings.cameraSpecialEffect);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, gSettings.cameraWBMode);
    if (s->set_sharpness) s->set_sharpness(s, gSettings.cameraSharpness);
    if (s->set_denoise) s->set_denoise(s, gSettings.cameraDenoise);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 0);
    s->set_ae_level(s, gSettings.cameraAELevel);  // Apply saved exposure compensation
    s->set_gain_ctrl(s, 1);
    s->set_agc_gain(s, 0);
    s->set_gainceiling(s, (gainceiling_t)0);
    s->set_bpc(s, 0);
    s->set_wpc(s, 1);
    s->set_raw_gma(s, 1);
    s->set_lenc(s, 1);
    s->set_dcw(s, 1);
    s->set_colorbar(s, 0);
    
    DEBUG_CAMERAF("[CAM_INIT] All sensor settings applied");
    INFO_SENSORSF("[Camera] Settings applied: brightness=%d contrast=%d saturation=%d hmirror=%d vflip=%d",
                  gSettings.cameraBrightness, gSettings.cameraContrast, 
                  gSettings.cameraSaturation, gSettings.cameraHMirror, gSettings.cameraVFlip);
    
    // OV3660: Flush frames AFTER changing settings to clear stale buffers
    // This prevents FB-OVF when resolution was changed
    if (s->id.PID == OV3660_PID) {
      DEBUG_CAMERAF("[CAM_INIT] OV3660 post-settings flush starting...");
      vTaskDelay(pdMS_TO_TICKS(100));  // Let new settings take effect
      for (int i = 0; i < 3; i++) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb) {
          DEBUG_CAMERAF("[CAM_INIT] Post-flush %d: %u bytes %ux%u", i, fb->len, fb->width, fb->height);
          esp_camera_fb_return(fb);
        } else {
          DEBUG_CAMERAF("[CAM_INIT] Post-flush %d: NULL", i);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
      }
      DEBUG_CAMERAF("[CAM_INIT] OV3660 post-settings flush complete");
    }
  } else {
    DEBUG_CAMERAF("[CAM_INIT] WARNING: sensor handle is NULL!");
    INFO_SENSORSF("[Camera] WARNING: esp_camera_sensor_get() returned NULL!");
  }

  // Set dimensions based on confirmed working resolutions
  switch (fs) {
    case FRAMESIZE_QVGA:   cameraWidth = 320;  cameraHeight = 240; break;
    case FRAMESIZE_VGA:    cameraWidth = 640;  cameraHeight = 480; break;
    case FRAMESIZE_SVGA:   cameraWidth = 800;  cameraHeight = 600; break;
    case FRAMESIZE_XGA:    cameraWidth = 1024; cameraHeight = 768; break;
    case FRAMESIZE_SXGA:   cameraWidth = 1280; cameraHeight = 1024; break;
    case FRAMESIZE_UXGA:   cameraWidth = 1600; cameraHeight = 1200; break;
    default:               cameraWidth = 640;  cameraHeight = 480; break;
  }

  cameraConnected = true;
  cameraEnabled = true;
  sensorStatusBumpWith("opencamera");

  DEBUG_CAMERAF("[CAM_INIT] ========== initCamera() COMPLETE ==========");
  DEBUG_CAMERAF("[CAM_INIT] cameraEnabled=%d cameraConnected=%d", cameraEnabled, cameraConnected);
  DEBUG_CAMERAF("[CAM_INIT] Model=%s Resolution=%dx%d", cameraModel, cameraWidth, cameraHeight);
  DEBUG_CAMERAF("[CAM_INIT] Final heap: %u, PSRAM: %u", esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  INFO_SENSORSF("[Camera] Initialized: %s (%dx%d)", cameraModel, cameraWidth, cameraHeight);
  unlockCameraMutex();
  return true;
}

void stopCamera() {
  DEBUG_CAMERAF("[CAM_STOP] stopCamera() called, cameraEnabled=%d", cameraEnabled);
  if (!cameraEnabled) {
    DEBUG_CAMERAF("[CAM_STOP] Already stopped, returning");
    return;
  }

  if (!lockCameraMutex(15000)) {
    DEBUG_CAMERAF("[CAM_STOP] ERROR: camera mutex timeout (camera busy)");
    return;
  }

  DEBUG_CAMERAF("[CAM_STOP] Heap before deinit: %u", esp_get_free_heap_size());
  INFO_SENSORSF("[Camera] Stopping camera...");
  
  DEBUG_CAMERAF("[CAM_STOP] Calling esp_camera_deinit()...");
  esp_camera_deinit();
  DEBUG_CAMERAF("[CAM_STOP] esp_camera_deinit() complete");
  
  cameraEnabled = false;
  cameraStreaming = false;
  sensorStatusBumpWith("closecamera");
  
  DEBUG_CAMERAF("[CAM_STOP] Heap after deinit: %u", esp_get_free_heap_size());
  INFO_SENSORSF("[Camera] Stopped");

  unlockCameraMutex();
}

uint8_t* captureFrame(size_t* outLen) {
  // Verbose debug logging - uncomment for troubleshooting camera issues
  // DEBUG_CAMERAF("[CAM_CAPTURE] ========== captureFrame() ENTRY ==========");
  // DEBUG_CAMERAF("[CAM_CAPTURE] cameraEnabled=%d cameraConnected=%d cameraStreaming=%d",
  //               cameraEnabled, cameraConnected, cameraStreaming);
  // DEBUG_CAMERAF("[CAM_CAPTURE] Heap: %u, PSRAM: %u", 
  //               esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  
  if (!cameraEnabled) {
    // DEBUG_CAMERAF("[CAM_CAPTURE] Camera not enabled - returning NULL");
    // INFO_SENSORSF("[Camera] captureFrame() - camera not enabled, returning NULL");
    if (outLen) *outLen = 0;
    return nullptr;
  }

  // Fast-fail: don't queue behind other captures, return busy immediately
  if (!lockCameraMutex(0)) {
    // DEBUG_CAMERAF("[CAM_CAPTURE] Camera busy (another capture in progress)");
    // INFO_SENSORSF("[Camera] captureFrame() - camera busy, try again");
    if (outLen) *outLen = 0;
    return nullptr;
  }

  // Single attempt - fail fast, recover immediately if needed
  camera_fb_t* fb = nullptr;
  // DEBUG_CAMERAF("[CAM_CAPTURE] Calling esp_camera_fb_get()...");
  
  // unsigned long startMs = millis();
  fb = esp_camera_fb_get();
  // unsigned long elapsed = millis() - startMs;
  
  // DEBUG_CAMERAF("[CAM_CAPTURE] esp_camera_fb_get() returned in %lu ms, fb=%p", elapsed, fb);
  
  if (!fb) {
    // Recovery logging - keep these for diagnosing camera issues
    DEBUG_CAMERAF("[CAM_CAPTURE] Capture failed - attempting recovery...");

    stopCamera();
    vTaskDelay(pdMS_TO_TICKS(150));
    bool ok = initCamera();
    if (ok) {
      fb = esp_camera_fb_get();
    }
    if (!ok || !fb) {
      DEBUG_CAMERAF("[CAM_CAPTURE] Recovery failed");
    }

    if (!fb) {
      unlockCameraMutex();
      if (outLen) *outLen = 0;
      return nullptr;
    }
  }
  
  // DEBUG_CAMERAF("[CAM_CAPTURE] Frame buffer details:");
  // DEBUG_CAMERAF("[CAM_CAPTURE]   fb->buf = %p", fb->buf);
  // DEBUG_CAMERAF("[CAM_CAPTURE]   fb->len = %u", fb->len);
  // DEBUG_CAMERAF("[CAM_CAPTURE]   fb->width = %u", fb->width);
  // DEBUG_CAMERAF("[CAM_CAPTURE]   fb->height = %u", fb->height);
  // DEBUG_CAMERAF("[CAM_CAPTURE]   fb->format = %d", fb->format);
  // DEBUG_CAMERAF("[CAM_CAPTURE]   fb->timestamp = {%ld, %ld}", 
  //               (long)fb->timestamp.tv_sec, (long)fb->timestamp.tv_usec);
  // INFO_SENSORSF("[Camera] Got frame: len=%u, format=%d, width=%u, height=%u", 
  //               fb->len, fb->format, fb->width, fb->height);

  // Validate JPEG header (silent unless error)
  if (fb->format == PIXFORMAT_JPEG && fb->len >= 2) {
    if (fb->buf[0] != 0xFF || fb->buf[1] != 0xD8) {
      DEBUG_CAMERAF("[CAM_CAPTURE] Invalid JPEG header: %02X %02X", fb->buf[0], fb->buf[1]);
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
    DEBUG_CAMERAF("[CAM_CAPTURE] ALLOC FAILED: %u bytes, Heap: %u", 
                  fb->len, esp_get_free_heap_size());
    if (outLen) *outLen = 0;
  }

  esp_camera_fb_return(fb);
  
  // Note: With GRAB_LATEST mode, no flush needed - camera always gives latest frame
  // DEBUG_CAMERAF("[CAM_CAPTURE] EXIT buf=%p, len=%u", buf, outLen ? *outLen : 0);
  unlockCameraMutex();
  return buf;
}

// Set camera resolution - useful for ESP-NOW transmission (lower res = smaller files)
bool setCameraResolution(framesize_t size) {
  if (!cameraEnabled) {
    return false;
  }

  if (!lockCameraMutex(15000)) {
    DEBUG_CAMERAF("[CAM_SET] ERROR: camera mutex timeout (camera busy)");
    return false;
  }
   
  sensor_t* s = esp_camera_sensor_get();
  if (!s) {
    unlockCameraMutex();
    return false;
  }
   
  int result = s->set_framesize(s, size);
  if (result == 0) {
    // Update tracked dimensions (confirmed working resolutions only)
    switch (size) {
      case FRAMESIZE_QVGA:  cameraWidth = 320;  cameraHeight = 240; break;
      case FRAMESIZE_VGA:   cameraWidth = 640;  cameraHeight = 480; break;
      case FRAMESIZE_SVGA:  cameraWidth = 800;  cameraHeight = 600; break;
      case FRAMESIZE_XGA:   cameraWidth = 1024; cameraHeight = 768; break;
      case FRAMESIZE_SXGA:  cameraWidth = 1280; cameraHeight = 1024; break;
      case FRAMESIZE_UXGA:  cameraWidth = 1600; cameraHeight = 1200; break;
      default: break;
    }
    INFO_SENSORSF("[Camera] Resolution set to %dx%d", cameraWidth, cameraHeight);
    unlockCameraMutex();
    return true;
  }
  unlockCameraMutex();
  return false;
}

// Set JPEG quality (0-63, lower = higher quality, larger file)
bool setCameraQuality(int quality) {
  if (!cameraEnabled) return false;

  if (!lockCameraMutex(15000)) {
    DEBUG_CAMERAF("[CAM_SET] ERROR: camera mutex timeout (camera busy)");
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
  if (!cameraEnabled) {
    if (outLen) *outLen = 0;
    return nullptr;
  }

  if (!lockCameraMutex(15000)) {
    DEBUG_CAMERAF("[CAM_CAPTURE] ERROR: camera mutex timeout (camera busy)");
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
      DEBUG_SENSORSF("[Camera] Captured %dx%d frame: %u bytes (q=%d)", 
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
  doc["enabled"] = cameraEnabled;
  doc["connected"] = cameraConnected;
  doc["streaming"] = cameraStreaming;
  doc["model"] = cameraModel;
  doc["width"] = cameraWidth;
  doc["height"] = cameraHeight;
  doc["psram"] = psramFound();

  serializeJson(doc, cameraStatusBuffer, kStatusBufSize);
  return cameraStatusBuffer;
}

// Command handlers
const char* cmd_camera(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return buildCameraStatusJson();
}

const char* cmd_camerastart(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (initCamera()) {
    return "Camera started successfully";
  }
  return "Camera initialization failed";
}

const char* cmd_camerastop(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  stopCamera();
  return "Camera stopped";
}

const char* cmd_cameracapture(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!cameraEnabled) {
    return "Camera not enabled - run opencamera first";
  }
  
  size_t len = 0;
  uint8_t* frame = captureFrame(&len);
  if (frame) {
    static char result[64];
    snprintf(result, sizeof(result), "Captured frame: %u bytes", (unsigned)len);
    free(frame);
    return result;
  }
  return "Frame capture failed";
}

const char* cmd_camerares(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Parse resolution argument
  String sizeStr = args;
  sizeStr.trim();
  sizeStr.toLowerCase();
  
  if (sizeStr.length() == 0) {
    static char result[160];
    snprintf(result, sizeof(result), 
      "Current: %dx%d\nUsage: camerares <size>\nSizes: qqvga(160x120) qvga(320x240) vga(640x480) svga(800x600) xga(1024x768)\nNote: Requires camera restart",
      cameraWidth, cameraHeight);
    return result;
  }
  
  framesize_t newSize = FRAMESIZE_VGA;
  if (sizeStr == "qqvga" || sizeStr == "160x120") newSize = FRAMESIZE_QQVGA;
  else if (sizeStr == "qvga" || sizeStr == "320x240") newSize = FRAMESIZE_QVGA;
  else if (sizeStr == "cif" || sizeStr == "400x296") newSize = FRAMESIZE_CIF;
  else if (sizeStr == "vga" || sizeStr == "640x480") newSize = FRAMESIZE_VGA;
  else if (sizeStr == "svga" || sizeStr == "800x600") newSize = FRAMESIZE_SVGA;
  else if (sizeStr == "xga" || sizeStr == "1024x768") newSize = FRAMESIZE_XGA;
  else if (sizeStr == "sxga" || sizeStr == "1280x1024") newSize = FRAMESIZE_SXGA;
  else if (sizeStr == "uxga" || sizeStr == "1600x1200") newSize = FRAMESIZE_UXGA;
  else return "Unknown resolution. Use: qqvga, qvga, vga, svga, xga, sxga, uxga";
  
  // Save to settings for persistence
  setSetting(gSettings.cameraFramesize, (int)cameraFramesizeSettingFromEnum(newSize));
  
  // If camera is running, do a full restart for reliable resolution change
  bool wasEnabled = cameraEnabled;
  bool wasStreaming = cameraStreaming;
  
  if (wasEnabled) {
    stopCamera();
    vTaskDelay(pdMS_TO_TICKS(100));  // Brief delay for hardware to settle
    initCamera();
  }
  
  static char result[96];
  if (wasStreaming) {
    snprintf(result, sizeof(result), "Resolution set to %dx%d (saved). Streaming stopped - please restart stream.", cameraWidth, cameraHeight);
  } else if (wasEnabled) {
    snprintf(result, sizeof(result), "Resolution set to %dx%d (saved). Camera restarted.", cameraWidth, cameraHeight);
  } else {
    snprintf(result, sizeof(result), "Resolution set to %dx%d (saved). Will apply on next camera start.", cameraWidth, cameraHeight);
  }
  return result;
}

// Numeric framesize command for settings UI (accepts framesize enum value directly)
const char* cmd_cameraframesize(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String valStr = args;
  valStr.trim();
  
  if (valStr.length() == 0) {
    static char result[64];
    framesize_t fs = cameraFramesizeFromSetting(gSettings.cameraFramesize);
    snprintf(result, sizeof(result), "cameraFramesize=%d", (int)fs);
    return result;
  }
  
  int newSize = valStr.toInt();
  if (newSize < 0 || newSize > 5) {
    return "Framesize must be 0-5 (QVGA/VGA/SVGA/XGA/SXGA/UXGA)";
  }
  
  setSetting(gSettings.cameraFramesize, newSize);
  
  // If camera is running, restart to apply
  bool wasEnabled = cameraEnabled;
  if (wasEnabled) {
    stopCamera();
    vTaskDelay(pdMS_TO_TICKS(100));
    initCamera();
  }
  
  static char result[80];
  snprintf(result, sizeof(result), "Resolution set to %dx%d. %s", cameraWidth, cameraHeight,
           wasEnabled ? "Camera restarted." : "Will apply on next start.");
  return result;
}

const char* cmd_cameraquality(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String valStr = args;
  valStr.trim();
  
  if (valStr.length() == 0) {
    static char result[80];
    snprintf(result, sizeof(result), "Current: %d\nUsage: cameraquality <0-63> (lower = better quality, larger file)", 
             gSettings.cameraQuality);
    return result;
  }
  
  int quality = valStr.toInt();
  if (quality < 0 || quality > 63) {
    return "Quality must be 0-63";
  }
  
  // Save to settings for persistence
  setSetting(gSettings.cameraQuality, quality);
  
  // Apply live if camera is running (quality can be changed without restart)
  if (cameraEnabled) {
    setCameraQuality(quality);
    static char result[64];
    snprintf(result, sizeof(result), "JPEG quality set to %d (saved, applied live)", quality);
    return result;
  }
  
  static char result[64];
  snprintf(result, sizeof(result), "JPEG quality set to %d (saved, will apply on camera start)", quality);
  return result;
}

const char* cmd_cameratiny(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!cameraEnabled) {
    return "Camera not enabled - run opencamera first";
  }
  
  size_t len = 0;
  uint8_t* frame = captureTinyFrame(&len);
  if (frame) {
    static char result[96];
    snprintf(result, sizeof(result), "Tiny frame (160x120): %u bytes %s", 
             (unsigned)len, len <= 250 ? "(ESP-NOW compatible)" : "(too large for single ESP-NOW packet)");
    free(frame);
    return result;
  }
  return "Tiny frame capture failed";
}

// Helper to apply a camera setting and optionally save
static bool applyCameraSetting(const char* name, int value, int minVal, int maxVal, 
                                int (*setter)(sensor_t*, int), int* settingPtr) {
  if (!cameraEnabled) return false;
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

const char* cmd_camerabrightness(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!cameraEnabled) return "Camera not enabled";
  
  String valStr = args;
  valStr.trim();
  
  if (valStr.length() == 0) {
    static char result[64];
    snprintf(result, sizeof(result), "Brightness: %d (range -2 to 2)", gSettings.cameraBrightness);
    return result;
  }
  
  int val = valStr.toInt();
  sensor_t* s = esp_camera_sensor_get();
  if (s && s->set_brightness(s, val) == 0) {
    setSetting(gSettings.cameraBrightness, val);
    static char result[48];
    snprintf(result, sizeof(result), "Brightness set to %d (saved)", val);
    return result;
  }
  return "Failed (use -2 to 2)";
}

const char* cmd_cameracontrast(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!cameraEnabled) return "Camera not enabled";
  
  String valStr = args;
  valStr.trim();
  
  if (valStr.length() == 0) {
    static char result[64];
    snprintf(result, sizeof(result), "Contrast: %d (range -2 to 2)", gSettings.cameraContrast);
    return result;
  }
  
  int val = valStr.toInt();
  sensor_t* s = esp_camera_sensor_get();
  if (s && s->set_contrast(s, val) == 0) {
    setSetting(gSettings.cameraContrast, val);
    static char result[48];
    snprintf(result, sizeof(result), "Contrast set to %d (saved)", val);
    return result;
  }
  return "Failed (use -2 to 2)";
}

const char* cmd_camerasaturation(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!cameraEnabled) return "Camera not enabled";
  
  String valStr = args;
  valStr.trim();
  
  if (valStr.length() == 0) {
    static char result[64];
    snprintf(result, sizeof(result), "Saturation: %d (range -2 to 2)", gSettings.cameraSaturation);
    return result;
  }
  
  int val = valStr.toInt();
  sensor_t* s = esp_camera_sensor_get();
  if (s && s->set_saturation(s, val) == 0) {
    setSetting(gSettings.cameraSaturation, val);
    static char result[48];
    snprintf(result, sizeof(result), "Saturation set to %d (saved)", val);
    return result;
  }
  return "Failed (use -2 to 2)";
}

const char* cmd_camerawb(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!cameraEnabled) return "Camera not enabled";
  
  String valStr = args;
  valStr.trim();
  
  if (valStr.length() == 0) {
    static char result[96];
    snprintf(result, sizeof(result), "WB mode: %d (0=Auto,1=Sunny,2=Cloudy,3=Office,4=Home)", gSettings.cameraWBMode);
    return result;
  }
  
  int val = valStr.toInt();
  if (val < 0 || val > 4) return "WB mode must be 0-4";
  
  sensor_t* s = esp_camera_sensor_get();
  if (s && s->set_wb_mode(s, val) == 0) {
    setSetting(gSettings.cameraWBMode, val);
    static char result[48];
    snprintf(result, sizeof(result), "WB mode set to %d (saved)", val);
    return result;
  }
  return "Failed to set WB mode";
}

const char* cmd_camerasharpness(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!cameraEnabled) return "Camera not enabled";
  
  String valStr = args;
  valStr.trim();
  
  if (valStr.length() == 0) {
    static char result[64];
    snprintf(result, sizeof(result), "Sharpness: %d (range -2 to 2, OV3660 only)", gSettings.cameraSharpness);
    return result;
  }
  
  int val = valStr.toInt();
  if (val < -2 || val > 2) return "Sharpness must be -2 to 2";
  
  sensor_t* s = esp_camera_sensor_get();
  if (s && s->set_sharpness && s->set_sharpness(s, val) == 0) {
    setSetting(gSettings.cameraSharpness, val);
    static char result[48];
    snprintf(result, sizeof(result), "Sharpness set to %d (saved)", val);
    return result;
  }
  return "Failed (OV3660 only, use -2 to 2)";
}

const char* cmd_cameradenoise(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!cameraEnabled) return "Camera not enabled";
  
  String valStr = args;
  valStr.trim();
  
  if (valStr.length() == 0) {
    static char result[64];
    snprintf(result, sizeof(result), "Denoise: %d (range 0-8)", gSettings.cameraDenoise);
    return result;
  }
  
  int val = valStr.toInt();
  if (val < 0 || val > 8) return "Denoise must be 0-8";
  
  sensor_t* s = esp_camera_sensor_get();
  if (s && s->set_denoise && s->set_denoise(s, val) == 0) {
    setSetting(gSettings.cameraDenoise, val);
    static char result[48];
    snprintf(result, sizeof(result), "Denoise set to %d (saved)", val);
    return result;
  }
  return "Failed to set denoise";
}

const char* cmd_cameraeffect(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!cameraEnabled) return "Camera not enabled";
  
  String valStr = args;
  valStr.trim();
  
  if (valStr.length() == 0) {
    static char result[96];
    snprintf(result, sizeof(result), "Effect: %d (0=None,1=Neg,2=Gray,3=Red,4=Green,5=Blue,6=Sepia)", gSettings.cameraSpecialEffect);
    return result;
  }
  
  int val = valStr.toInt();
  if (val < 0 || val > 6) return "Effect must be 0-6";
  
  sensor_t* s = esp_camera_sensor_get();
  if (s && s->set_special_effect(s, val) == 0) {
    setSetting(gSettings.cameraSpecialEffect, val);
    static char result[48];
    snprintf(result, sizeof(result), "Effect set to %d (saved)", val);
    return result;
  }
  return "Failed to set effect";
}

const char* cmd_cameraexposure(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!cameraEnabled) return "Camera not enabled";
  
  String valStr = args;
  valStr.trim();
  
  if (valStr.length() == 0) {
    static char result[80];
    snprintf(result, sizeof(result), "AE Level: %d (range -2 to 2, negative=darker)", gSettings.cameraAELevel);
    return result;
  }
  
  int val = valStr.toInt();
  if (val < -2 || val > 2) return "AE Level must be -2 to 2";
  
  sensor_t* s = esp_camera_sensor_get();
  if (s && s->set_ae_level(s, val) == 0) {
    setSetting(gSettings.cameraAELevel, val);
    static char result[64];
    snprintf(result, sizeof(result), "AE Level set to %d (saved)", val);
    return result;
  }
  return "Failed to set AE level";
}

const char* cmd_cameraaec(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!cameraEnabled) return "Camera not enabled";

  sensor_t* s = esp_camera_sensor_get();
  if (!s) return "Camera sensor not available";

  String arg = args;
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
  return "Failed";
}

const char* cmd_camerastreaminterval(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String valStr = args;
  valStr.trim();
  
  if (valStr.length() == 0) {
    static char buf[96];
    snprintf(buf, sizeof(buf), "Stream interval: %d ms (lower=faster)\nUsage: camerastreaminterval <50-2000>",
             gSettings.cameraStreamIntervalMs);
    return buf;
  }

  int val = valStr.toInt();
  if (val < 50 || val > 2000) return "cameraStreamIntervalMs must be 50-2000";
  setSetting(gSettings.cameraStreamIntervalMs, val);
  static char buf[64];
  snprintf(buf, sizeof(buf), "cameraStreamIntervalMs set to %d ms", val);
  return buf;
}

const char* cmd_cameraaecvalue(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!cameraEnabled) return "Camera not enabled";

  String valStr = args;
  valStr.trim();
  
  if (valStr.length() == 0) {
    return "Usage: cameraaecvalue <0-1200>";
  }

  int val = valStr.toInt();
  if (val < 0 || val > 1200) return "AEC value must be 0-1200";

  sensor_t* s = esp_camera_sensor_get();
  if (!s) return "Camera sensor not available";

  (void)s->set_exposure_ctrl(s, 0);
  if (s->set_aec_value(s, val) == 0) {
    static char result[64];
    snprintf(result, sizeof(result), "Manual exposure set to %d", val);
    return result;
  }
  return "Failed";
}

const char* cmd_cameraagc(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!cameraEnabled) return "Camera not enabled";

  sensor_t* s = esp_camera_sensor_get();
  if (!s) return "Camera sensor not available";

  String arg = args;
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
  return "Failed";
}

const char* cmd_cameraagcgain(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!cameraEnabled) return "Camera not enabled";

  String valStr = args;
  valStr.trim();
  
  if (valStr.length() == 0) {
    return "Usage: cameraagcgain <0-30>";
  }

  int val = valStr.toInt();
  if (val < 0 || val > 30) return "AGC gain must be 0-30";

  sensor_t* s = esp_camera_sensor_get();
  if (!s) return "Camera sensor not available";

  (void)s->set_gain_ctrl(s, 0);
  if (s->set_agc_gain(s, val) == 0) {
    static char result[64];
    snprintf(result, sizeof(result), "Manual gain set to %d", val);
    return result;
  }
  return "Failed";
}

const char* cmd_camerahmirror(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!cameraEnabled) return "Camera not enabled";
  
  String arg = args;
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
  return "Failed";
}

const char* cmd_cameravflip(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!cameraEnabled) return "Camera not enabled";
  
  String arg = args;
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
  return "Failed";
}

const char* cmd_camerarotate(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!cameraEnabled) return "Camera not started";
  
  String arg = args;
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
  return "Failed";
}

// ============================================================================
// Camera Settings Commands
// ============================================================================

const char* cmd_cameraautostart(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = args; arg.trim();
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
  return "Usage: cameraautostart [on|off]";
}

const char* cmd_camerastoragelocation(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  
  if (valStr.length() == 0) {
    static char buf[64];
    snprintf(buf, sizeof(buf), "cameraStorageLocation = %d (0=LittleFS, 1=SD, 2=Both)", gSettings.cameraStorageLocation);
    return buf;
  }
  int val = valStr.toInt();
  if (val < 0 || val > 2) return "Error: cameraStorageLocation must be 0-2";
  setSetting(gSettings.cameraStorageLocation, val);
  static char buf[48];
  snprintf(buf, sizeof(buf), "cameraStorageLocation set to %d", val);
  return buf;
}

const char* cmd_cameracapturefolder(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String val = args;
  val.trim();
  
  if (val.length() == 0) {
    static char buf[128];
    snprintf(buf, sizeof(buf), "cameraCaptureFolder = %s", gSettings.cameraCaptureFolder.c_str());
    return buf;
  }
  setSetting(gSettings.cameraCaptureFolder, val);
  static char buf[128];
  snprintf(buf, sizeof(buf), "cameraCaptureFolder set to %s", val.c_str());
  return buf;
}

const char* cmd_cameramaxstoredimages(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  
  if (valStr.length() == 0) {
    static char buf[64];
    snprintf(buf, sizeof(buf), "cameraMaxStoredImages = %d", gSettings.cameraMaxStoredImages);
    return buf;
  }
  int val = valStr.toInt();
  if (val < 0 || val > 1000) return "Error: cameraMaxStoredImages must be 0-1000";
  setSetting(gSettings.cameraMaxStoredImages, val);
  static char buf[48];
  snprintf(buf, sizeof(buf), "cameraMaxStoredImages set to %d", val);
  return buf;
}

const char* cmd_cameraautocapture(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = args;
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

const char* cmd_cameraautocaptureinterval(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  
  if (valStr.length() == 0) {
    static char buf[64];
    snprintf(buf, sizeof(buf), "cameraAutoCaptureInterval = %d sec", gSettings.cameraAutoCaptureIntervalSec);
    return buf;
  }
  int val = valStr.toInt();
  if (val < 10 || val > 3600) return "Error: cameraAutoCaptureInterval must be 10-3600";
  setSetting(gSettings.cameraAutoCaptureIntervalSec, val);
  static char buf[48];
  snprintf(buf, sizeof(buf), "cameraAutoCaptureInterval set to %d sec", val);
  return buf;
}

const char* cmd_camerasendaftercapture(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = args;
  arg.trim();
  
  if (arg.length() == 0) {
    return gSettings.cameraSendAfterCapture ? "cameraSendAfterCapture = true" : "cameraSendAfterCapture = false";
  }
  bool enable = (arg == "1" || arg.equalsIgnoreCase("true") || arg.equalsIgnoreCase("on"));
  setSetting(gSettings.cameraSendAfterCapture, enable);
  return enable ? "cameraSendAfterCapture set to true" : "cameraSendAfterCapture set to false";
}

const char* cmd_cameratargetdevice(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String val = args;
  val.trim();
  
  if (val.length() == 0) {
    static char buf[128];
    snprintf(buf, sizeof(buf), "cameraTargetDevice = %s", gSettings.cameraTargetDevice.c_str());
    return buf;
  }
  setSetting(gSettings.cameraTargetDevice, val);
  static char buf[128];
  snprintf(buf, sizeof(buf), "cameraTargetDevice set to %s", val.c_str());
  return buf;
}

// Forward declare ImageManager for camerasave
#include "System_ImageManager.h"
extern ImageManager gImageManager;

const char* cmd_camerasave(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!cameraEnabled) {
    return "Camera not enabled - run opencamera first";
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
    static char result[128];
    snprintf(result, sizeof(result), "Saved: %s", savedPath.c_str());
    return result;
  }
  return "Failed to save image";
}

// Command registry
const CommandEntry cameraCommands[] = {
  {"cameraread",       "Read camera status",              false, cmd_camera},
  {"opencamera",       "Start camera sensor.",            false, cmd_camerastart, nullptr, "sensor", "camera", "open"},
  {"closecamera",      "Stop camera sensor.",             false, cmd_camerastop, nullptr, "sensor", "camera", "close"},
  {"cameracapture",    "Capture a single frame",          false, cmd_cameracapture, nullptr, "sensor", "camera", "take picture"},
  {"camerasave",       "Save current frame to storage",   false, cmd_camerasave},
  {"camerares",        "Set camera resolution: <res>",    false, cmd_camerares},
  {"cameraframesize",  "Set resolution: <0-5>",           true,  cmd_cameraframesize},
  {"cameraquality",    "Set JPEG quality: <0-63>",        false, cmd_cameraquality},
  {"camerastreaminterval", "Stream interval: <ms>",       true, cmd_camerastreaminterval},
  {"cameratiny",       "Capture tiny frame for ESP-NOW",  false, cmd_cameratiny},
  {"camerabrightness", "Set brightness: <-2..2>",         false, cmd_camerabrightness},
  {"cameracontrast",   "Set contrast: <-2..2>",           false, cmd_cameracontrast},
  {"camerasaturation", "Set saturation: <-2..2>",         false, cmd_camerasaturation},
  {"camerawb",         "White balance mode: <0-4>",       true,  cmd_camerawb},
  {"camerasharpness",  "Set sharpness: <-2..2>",          true,  cmd_camerasharpness},
  {"cameradenoise",    "Set denoise level: <0-8>",        true,  cmd_cameradenoise},
  {"cameraeffect",     "Special effect: <0-6>",           true,  cmd_cameraeffect},
  {"cameraexposure",   "Set AE level: <-2..2>",           true,  cmd_cameraexposure},
  {"cameraaec",        "Auto exposure: <on|off>",         true,  cmd_cameraaec},
  {"cameraaecvalue",   "Exposure value: <0-1200>",        true,  cmd_cameraaecvalue},
  {"cameraagc",        "Auto gain: <on|off>",             true,  cmd_cameraagc},
  {"cameraagcgain",    "Gain value: <0-30>",              true,  cmd_cameraagcgain},
  {"camerahmirror",    "Horizontal mirror: <on|off>",     false, cmd_camerahmirror},
  {"cameravflip",      "Vertical flip: <on|off>",         false, cmd_cameravflip},
  {"camerarotate",     "Rotate 180°: <on|off>",           false, cmd_camerarotate},
  {"cameraautostart",  "Auto-start: <on|off>",            true,  cmd_cameraautostart},
  {"camerastoragelocation", "Storage location: <0-2>",    true,  cmd_camerastoragelocation},
  {"cameracapturefolder",   "Photo folder: <path>",       true,  cmd_cameracapturefolder},
  {"cameramaxstoredimages", "Max stored: <0-1000>",       true,  cmd_cameramaxstoredimages},
  {"cameraautocapture",     "Auto-capture: <on|off>",     true,  cmd_cameraautocapture},
  {"cameraautocaptureinterval", "Auto-capture: <sec>",    true, cmd_cameraautocaptureinterval},
  {"camerasendaftercapture", "Send after capture: <on|off>", true, cmd_camerasendaftercapture},
  {"cameratargetdevice",    "Target device: <name>",      true,  cmd_cameratargetdevice},
};

static const SettingEntry cameraSettingEntries[] = {
  { "cameraAutoStart",          SETTING_BOOL,   &gSettings.cameraAutoStart,              0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr },
  { "cameraFramesize",          SETTING_INT,    &gSettings.cameraFramesize,              1, 0, nullptr, 0, 5, "Resolution",
    "0:320x240 (QVGA),1:640x480 (VGA),2:800x600 (SVGA),3:1024x768 (XGA),4:1280x1024 (SXGA),5:1600x1200 (UXGA)" },
  { "cameraBrightness",         SETTING_INT,    &gSettings.cameraBrightness,             0, 0, nullptr, -2, 2, "Brightness (-2 to 2)", nullptr },
  { "cameraContrast",           SETTING_INT,    &gSettings.cameraContrast,               0, 0, nullptr, -2, 2, "Contrast (-2 to 2)", nullptr },
  { "cameraSaturation",         SETTING_INT,    &gSettings.cameraSaturation,             0, 0, nullptr, -2, 2, "Saturation (-2 to 2)", nullptr },
  { "cameraAELevel",            SETTING_INT,    &gSettings.cameraAELevel,                0, 0, nullptr, -2, 2, "Exposure Compensation (-2 to 2)", nullptr },
  { "cameraWBMode",             SETTING_INT,    &gSettings.cameraWBMode,                 0, 0, nullptr, 0, 4, "White Balance",
    "0:Auto,1:Sunny,2:Cloudy,3:Office,4:Home" },
  { "cameraSharpness",          SETTING_INT,    &gSettings.cameraSharpness,              0, 0, nullptr, -2, 2, "Sharpness (-2 to 2, OV3660)", nullptr },
  { "cameraDenoise",            SETTING_INT,    &gSettings.cameraDenoise,                0, 0, nullptr, 0, 8, "Denoise (0-8)", nullptr },
  { "cameraSpecialEffect",      SETTING_INT,    &gSettings.cameraSpecialEffect,          0, 0, nullptr, 0, 6, "Special Effect",
    "0:None,1:Negative,2:Grayscale,3:Red Tint,4:Green Tint,5:Blue Tint,6:Sepia" },
  { "cameraHMirror",            SETTING_BOOL,   &gSettings.cameraHMirror,                0, 0, nullptr, 0, 1, "Horizontal mirror", nullptr },
  { "cameraVFlip",              SETTING_BOOL,   &gSettings.cameraVFlip,                  0, 0, nullptr, 0, 1, "Vertical flip", nullptr },
  { "cameraQuality",            SETTING_INT,    &gSettings.cameraQuality,                0, 0, nullptr, 0, 63, "JPEG quality (0-63, lower=better)", nullptr },
  { "cameraStreamIntervalMs",   SETTING_INT,    &gSettings.cameraStreamIntervalMs,     200, 0, nullptr, 50, 2000, "Stream interval ms (lower=faster)", nullptr },
  { "cameraStorageLocation",    SETTING_INT,    &gSettings.cameraStorageLocation,        0, 0, nullptr, 0, 2, "Storage Location", "0:LittleFS (Internal),1:SD Card,2:Both" },
  { "cameraCaptureFolder",      SETTING_STRING, &gSettings.cameraCaptureFolder,          0, 0, nullptr, 0, 0, "Photo folder path", nullptr },
  { "cameraMaxStoredImages",    SETTING_INT,    &gSettings.cameraMaxStoredImages,        0, 0, nullptr, 0, 1000, "Max images (0=unlimited)", nullptr },
  { "cameraAutoCapture",        SETTING_BOOL,   &gSettings.cameraAutoCapture,            0, 0, nullptr, 0, 1, "Enable auto-capture", nullptr },
  { "cameraAutoCaptureInterval",SETTING_INT,    &gSettings.cameraAutoCaptureIntervalSec, 0, 0, nullptr, 10, 3600, "Auto-capture interval (sec)", nullptr },
  { "cameraSendAfterCapture",   SETTING_BOOL,   &gSettings.cameraSendAfterCapture,       0, 0, nullptr, 0, 1, "Send to target after capture", nullptr },
  { "cameraTargetDevice",       SETTING_STRING, &gSettings.cameraTargetDevice,           0, 0, nullptr, 0, 0, "ESP-NOW target device name", nullptr },
};

static bool isCameraConnected() {
  if (!cameraEnabled) return true;
  return cameraConnected;
}

extern const SettingsModule cameraSettingsModule = {
  "camera",
  "camera",
  cameraSettingEntries,
  sizeof(cameraSettingEntries) / sizeof(cameraSettingEntries[0]),
  isCameraConnected,
  "ESP32-S3 camera sensor"
};
const size_t cameraCommandsCount = sizeof(cameraCommands) / sizeof(cameraCommands[0]);

// Auto-register with command system
static CommandModuleRegistrar _camera_cmd_registrar(cameraCommands, cameraCommandsCount, "camera");

#endif // ENABLE_CAMERA_SENSOR
