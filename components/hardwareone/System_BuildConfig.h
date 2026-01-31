#ifndef SYSTEM_BUILD_CONFIG_H
#define SYSTEM_BUILD_CONFIG_H

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║                     USER CONFIGURATION - EDIT HERE                        ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║  All user-configurable options are in this section.                       ║
// ║  Everything below this section is auto-derived or board-specific.         ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

// I2C Feature Level: Controls OLED and I2C sensors
//   0 = DISABLED   - No I2C (max memory savings)
//   1 = OLED_ONLY  - OLED display only
//   2 = STANDALONE - OLED + Gamepad
//   3 = FULL       - OLED + all sensors
//   4 = CUSTOM     - Use individual sensor flags below
#define I2C_FEATURE_LEVEL       0

// ─────────────────────────────────────────────────────────────────────────────
// CUSTOM SENSOR SELECTION (only used when I2C_FEATURE_LEVEL = 4)
// ─────────────────────────────────────────────────────────────────────────────
// Enable/disable individual sensors for fine-grained control.
// This allows combinations like OLED + Gamepad + GPS without heavy sensors.
#if I2C_FEATURE_LEVEL == 4
  #define CUSTOM_ENABLE_OLED        1   // SSD1306 OLED display
  #define CUSTOM_ENABLE_GAMEPAD     1   // Adafruit Seesaw gamepad
  #define CUSTOM_ENABLE_GPS         1   // PA1010D GPS module
  #define CUSTOM_ENABLE_IMU         0   // BNO055 IMU (uses ~1KB RAM)
  #define CUSTOM_ENABLE_TOF         0   // VL53L4CX ToF sensor
  #define CUSTOM_ENABLE_THERMAL     0   // MLX90640 thermal camera (uses ~3KB RAM)
  #define CUSTOM_ENABLE_APDS        0   // APDS9960 gesture/proximity
  #define CUSTOM_ENABLE_FM_RADIO    0   // RDA5807 FM radio
  #define CUSTOM_ENABLE_RTC         1   // DS3231 precision RTC
  #define CUSTOM_ENABLE_PRESENCE    1   // STHS34PF80 IR presence/motion
#endif

// Network Feature Level: Controls WiFi/HTTP/ESP-NOW
//   0 = DISABLED   - No networking
//   1 = WIFI_ONLY  - WiFi without HTTP server
//   2 = WIFI_HTTP  - WiFi + HTTP server
//   3 = WIFI_ESPNOW - WiFi + HTTP + ESP-NOW mesh
#define NETWORK_FEATURE_LEVEL   3

// Camera: ESP32-S3 DVP camera (OV2640/OV3660/OV5640)
//   0 = Disabled, 1 = Enabled
#define ENABLE_CAMERA_SENSOR    1

// Microphone: PDM microphone via I2S
//   0 = Disabled, 1 = Enabled
#define ENABLE_MICROPHONE_SENSOR 1

// Battery Monitoring: ADC-based LiPo battery voltage monitoring
//   0 = Disabled (shows "USB" on OLED), 1 = Enabled (Feather ESP32 GPIO35)
//   Disable this if your board doesn't have battery monitoring hardware
#define ENABLE_BATTERY_MONITOR 0

// Bluetooth: BLE server with GATT services
//   0 = Disabled, 1 = Enabled
#define ENABLE_BLUETOOTH        1

// Even G2 Smart Glasses: BLE client to connect to Even Realities G2 glasses
//   0 = Disabled, 1 = Enabled (requires ENABLE_BLUETOOTH=1)
//   When enabled, ESP32 can act as BLE central to connect to glasses
//   This is mutually exclusive with phone BLE connections at runtime
#define ENABLE_G2_GLASSES       1

// MQTT: Home Assistant integration via MQTT broker
//   0 = Disabled, 1 = Enabled (requires ENABLE_WIFI=1)
#define ENABLE_MQTT             1

// Edge Impulse: ML inference
//   0 = Disabled, 1 = Enabled
#define ENABLE_EDGE_IMPULSE     0

#define ENABLE_ESP_SR           1

// Games: Browser-based games web page
//   0 = Disabled, 1 = Enabled
#define ENABLE_GAMES            0

// Maps: Offline maps and waypoints web page
//   0 = Disabled, 1 = Enabled
#define ENABLE_MAPS             1

// Automation: Scheduled tasks and conditional command system
//   0 = Disabled, 1 = Enabled
#define ENABLE_AUTOMATION       0

// Display Type: Hardware display selection
//   0 = NONE, 1 = SSD1306 (OLED), 2 = ST7789 (TFT), 3 = ILI9341 (TFT)
#define DISPLAY_TYPE            1

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║                    END OF USER CONFIGURATION                              ║
// ╚═══════════════════════════════════════════════════════════════════════════╝


// =============================================================================
// FEATURE LEVEL CONSTANTS (do not modify)
// =============================================================================

#define I2C_LEVEL_DISABLED   0
#define I2C_LEVEL_OLED_ONLY  1
#define I2C_LEVEL_STANDALONE 2
#define I2C_LEVEL_FULL       3
#define I2C_LEVEL_CUSTOM     4

#define NET_LEVEL_DISABLED   0
#define NET_LEVEL_WIFI_ONLY  1
#define NET_LEVEL_WIFI_HTTP  2
#define NET_LEVEL_WIFI_ESPNOW 3

#define DISPLAY_TYPE_NONE      0
#define DISPLAY_TYPE_SSD1306   1
#define DISPLAY_TYPE_ST7789    2
#define DISPLAY_TYPE_ILI9341   3

// =============================================================================
// DERIVED FLAGS (automatically set based on I2C_FEATURE_LEVEL)
// =============================================================================

#if I2C_FEATURE_LEVEL == I2C_LEVEL_DISABLED
  // Level 0: Everything disabled
  #define ENABLE_I2C_SYSTEM       0
  #define ENABLE_OLED_DISPLAY     0
  #define ENABLE_THERMAL_SENSOR   0
  #define ENABLE_TOF_SENSOR       0
  #define ENABLE_IMU_SENSOR       0
  #define ENABLE_GAMEPAD_SENSOR   0
  #define ENABLE_APDS_SENSOR      0
  #define ENABLE_GPS_SENSOR       0
  #define ENABLE_FM_RADIO         0
  #define ENABLE_RTC_SENSOR       0
  #define ENABLE_PRESENCE_SENSOR  0

#elif I2C_FEATURE_LEVEL == I2C_LEVEL_OLED_ONLY
  // Level 1: OLED only, no sensors
  #define ENABLE_I2C_SYSTEM       1
  #define ENABLE_OLED_DISPLAY     1
  #define ENABLE_THERMAL_SENSOR   0
  #define ENABLE_TOF_SENSOR       0
  #define ENABLE_IMU_SENSOR       0
  #define ENABLE_GAMEPAD_SENSOR   0
  #define ENABLE_APDS_SENSOR      0
  #define ENABLE_GPS_SENSOR       0
  #define ENABLE_FM_RADIO         0
  #define ENABLE_RTC_SENSOR       0
  #define ENABLE_PRESENCE_SENSOR  0

#elif I2C_FEATURE_LEVEL == I2C_LEVEL_STANDALONE
  // Level 2: OLED + Gamepad for standalone device control
  #define ENABLE_I2C_SYSTEM       1
  #define ENABLE_OLED_DISPLAY     1
  #define ENABLE_THERMAL_SENSOR   0
  #define ENABLE_TOF_SENSOR       0
  #define ENABLE_IMU_SENSOR       0
  #define ENABLE_GAMEPAD_SENSOR   1
  #define ENABLE_APDS_SENSOR      0
  #define ENABLE_GPS_SENSOR       0
  #define ENABLE_FM_RADIO         0
  #define ENABLE_RTC_SENSOR       0
  #define ENABLE_PRESENCE_SENSOR  0

#elif I2C_FEATURE_LEVEL == I2C_LEVEL_FULL
  // Level 3: Everything enabled
  #define ENABLE_I2C_SYSTEM       1
  #define ENABLE_OLED_DISPLAY     1
  #define ENABLE_THERMAL_SENSOR   1
  #define ENABLE_TOF_SENSOR       1
  #define ENABLE_IMU_SENSOR       1
  #define ENABLE_GAMEPAD_SENSOR   1
  #define ENABLE_APDS_SENSOR      1
  #define ENABLE_GPS_SENSOR       1
  #define ENABLE_FM_RADIO         1
  #define ENABLE_RTC_SENSOR       1
  #define ENABLE_PRESENCE_SENSOR  1

#else  // I2C_LEVEL_CUSTOM
  // Level 4: Custom sensor selection from user config section
  #define ENABLE_I2C_SYSTEM       1
  #define ENABLE_OLED_DISPLAY     CUSTOM_ENABLE_OLED
  #define ENABLE_THERMAL_SENSOR   CUSTOM_ENABLE_THERMAL
  #define ENABLE_TOF_SENSOR       CUSTOM_ENABLE_TOF
  #define ENABLE_IMU_SENSOR       CUSTOM_ENABLE_IMU
  #define ENABLE_GAMEPAD_SENSOR   CUSTOM_ENABLE_GAMEPAD
  #define ENABLE_APDS_SENSOR      CUSTOM_ENABLE_APDS
  #define ENABLE_GPS_SENSOR       CUSTOM_ENABLE_GPS
  #define ENABLE_FM_RADIO         CUSTOM_ENABLE_FM_RADIO
  #define ENABLE_RTC_SENSOR       CUSTOM_ENABLE_RTC
  #define ENABLE_PRESENCE_SENSOR  CUSTOM_ENABLE_PRESENCE

#endif

// =============================================================================
// DERIVED NETWORK FLAGS (based on NETWORK_FEATURE_LEVEL)
// =============================================================================

#if NETWORK_FEATURE_LEVEL == NET_LEVEL_DISABLED
  #define ENABLE_WIFI         0
  #define ENABLE_HTTP_SERVER  0
  #define ENABLE_ESPNOW       0
#elif NETWORK_FEATURE_LEVEL == NET_LEVEL_WIFI_ONLY
  #define ENABLE_WIFI         1
  #define ENABLE_HTTP_SERVER  0
  #define ENABLE_ESPNOW       0
#elif NETWORK_FEATURE_LEVEL == NET_LEVEL_WIFI_HTTP
  #define ENABLE_WIFI         1
  #define ENABLE_HTTP_SERVER  1
  #define ENABLE_ESPNOW       0
#else // NET_LEVEL_WIFI_ESPNOW
  #define ENABLE_WIFI         1
  #define ENABLE_HTTP_SERVER  1
  #define ENABLE_ESPNOW       1
#endif

// Force ENABLE_MQTT off if WiFi is disabled (MQTT requires WiFi)
#if !ENABLE_WIFI
  #undef ENABLE_MQTT
  #define ENABLE_MQTT 0
#endif

// =============================================================================
// MEMORY SAVINGS REFERENCE
// =============================================================================
// Level DISABLED (0): Maximum savings
//   - All I2C code excluded (~100KB+ flash, ~50KB+ RAM saved)
//
// Level OLED_ONLY (1): Moderate savings  
//   - OLED display works for boot progress, menus, status
//   - Sensors disabled (~80KB flash, ~45KB RAM saved vs FULL)
//
// Level STANDALONE (2): OLED + Gamepad + Bluetooth
//   - Standalone device control via gamepad
//   - BLE communication with smart glasses
//   - GAMEPAD: +8-12KB flash, +6KB RAM (Seesaw)
//
// Level FULL (3): All features enabled
//   - THERMAL: +20-25KB flash, +15KB RAM (MLX90640)
//   - TOF:     +25-30KB flash, +10KB RAM (VL53L4CX)
//   - IMU:     +12-18KB flash, +8KB RAM  (BNO055)
//   - GAMEPAD: +8-12KB flash,  +6KB RAM  (Seesaw)
//   - APDS:    +6-10KB flash,  +4KB RAM  (APDS9960)
//   - GPS:     +5-8KB flash,   +4KB RAM  (PA1010D)
//   - FM:      +5-8KB flash,   +3KB RAM  (SI4713)
//   - PRESENCE:+4-6KB flash,   +2KB RAM  (STHS34PF80)

// =============================================================================
// BOARD HARDWARE CONFIGURATION
// =============================================================================
// Supported boards are auto-detected via Arduino board defines from menuconfig.
// Each board has specific I2C pins, NeoPixel pins, and battery monitoring.
//
// To add a new board:
//   1. Set CONFIG_ARDUINO_VARIANT in menuconfig (Component config -> Arduino)
//   2. The define is: ARDUINO_<UPPERCASE_VARIANT>_DEV (e.g., adafruit_qtpy_esp32 -> ARDUINO_ADAFRUIT_QTPY_ESP32_DEV)
//   3. Add a new #elif block below with the appropriate pin definitions
//   4. Update the BOARD_NAME string for identification
// =============================================================================

// --- Adafruit QT Py ESP32 (ESP32-PICO) ---
#if defined(ARDUINO_ADAFRUIT_QTPY_ESP32_DEV)
  #define BOARD_SUPPORTED       1
  #define BOARD_NAME            "Adafruit QT Py ESP32"
  
  // I2C Bus Configuration (using Wire1 for sensor breakouts)
  #define I2C_SDA_PIN_DEFAULT   22
  #define I2C_SCL_PIN_DEFAULT   19
  
  // NeoPixel Configuration
  #define NEOPIXEL_PIN_DEFAULT  5
  #define NEOPIXEL_POWER_PIN    8
  #define NEOPIXEL_COUNT_DEFAULT 1
  
  // Battery Monitoring (no built-in battery monitor on QT Py)
  #define BATTERY_ADC_PIN       -1
  #define BATTERY_MONITOR_AVAILABLE 0

// --- Adafruit Feather ESP32 V2 ---
#elif defined(ARDUINO_ADAFRUIT_FEATHER_ESP32_V2_DEV)
  #define BOARD_SUPPORTED       1
  #define BOARD_NAME            "Adafruit Feather ESP32 V2"
  
  // I2C Bus Configuration (primary Wire bus)
  #define I2C_SDA_PIN_DEFAULT   22
  #define I2C_SCL_PIN_DEFAULT   20
  
  // NeoPixel Configuration
  #define NEOPIXEL_PIN_DEFAULT  0
  #define NEOPIXEL_POWER_PIN    2
  #define NEOPIXEL_COUNT_DEFAULT 1
  
  // Battery Monitoring
  #define BATTERY_ADC_PIN       35
  #define BATTERY_MONITOR_AVAILABLE 1

// --- Adafruit Feather ESP32 (Original) ---
#elif defined(ARDUINO_FEATHER_ESP32_DEV)
  #define BOARD_SUPPORTED       1
  #define BOARD_NAME            "Adafruit Feather ESP32"
  
  // I2C Bus Configuration
  #define I2C_SDA_PIN_DEFAULT   23
  #define I2C_SCL_PIN_DEFAULT   22
  
  // NeoPixel Configuration (no built-in NeoPixel)
  #define NEOPIXEL_PIN_DEFAULT  -1
  #define NEOPIXEL_POWER_PIN    -1
  #define NEOPIXEL_COUNT_DEFAULT 0
  
  // Battery Monitoring
  #define BATTERY_ADC_PIN       35
  #define BATTERY_MONITOR_AVAILABLE 1

// --- Seeed Studio XIAO ESP32S3 Sense (with camera/mic expansion) ---
// Note: Sense uses same variant as base XIAO ESP32S3, expansion board is add-on hardware
// To enable Sense-specific features, define XIAO_ESP32S3_SENSE_ENABLED in your build
// IMPORTANT: This block MUST come before the base XIAO block so it matches first
#elif defined(ARDUINO_XIAO_ESP32S3_SENSE_DEV) || (defined(ARDUINO_XIAO_ESP32S3_DEV) && defined(XIAO_ESP32S3_SENSE_ENABLED))
  #define BOARD_SUPPORTED       1
  #define BOARD_NAME            "Seeed XIAO ESP32S3 Sense"
  
  // I2C Bus Configuration (same as base XIAO ESP32S3)
  #define I2C_SDA_PIN_DEFAULT   5   // GPIO5 (D4)
  #define I2C_SCL_PIN_DEFAULT   6   // GPIO6 (D5)
  
  // NeoPixel Configuration (no built-in NeoPixel)
  #define NEOPIXEL_PIN_DEFAULT  -1
  #define NEOPIXEL_POWER_PIN    -1
  #define NEOPIXEL_COUNT_DEFAULT 0
  
  // User LED - DISABLED on Sense board (GPIO21 is used for SD_CS)
  // The expansion board SD card takes priority over the base board LED
  #define USER_LED_PIN          -1
  #define USER_LED_ACTIVE_LOW   1
  
  // Battery Monitoring
  #define BATTERY_ADC_PIN       -1
  #define BATTERY_MONITOR_AVAILABLE 0
  
  // Sense-specific: SD Card (directly on expansion board)
  // Verified working via sddiag: CS=21, SCK=7, MISO=8, MOSI=9
  #define SD_CS_PIN             21  // GPIO21 (directly on expansion board)
  #define SD_SCK_PIN            7   // GPIO7
  #define SD_MISO_PIN           8   // GPIO8
  #define SD_MOSI_PIN           9   // GPIO9
  
  // Sense-specific: Camera (directly on expansion board, I2C on GPIO39/40)
  #define CAMERA_AVAILABLE      1
  
  // Sense-specific: Digital Microphone PDM
  #define MIC_CLK_PIN           42  // GPIO42 (PDM clock)
  #define MIC_DATA_PIN          41  // GPIO41 (PDM data)

// --- Seeed Studio XIAO ESP32S3 (base board without expansion) ---
// Set CONFIG_ARDUINO_VARIANT="XIAO_ESP32S3" in menuconfig
// Note: To use with Sense expansion board, define XIAO_ESP32S3_SENSE_ENABLED
#elif defined(ARDUINO_XIAO_ESP32S3_DEV)
  #define BOARD_SUPPORTED       1
  #define BOARD_NAME            "Seeed XIAO ESP32S3"
  
  // I2C Bus Configuration (D4=SDA, D5=SCL per Seeed pinout)
  #define I2C_SDA_PIN_DEFAULT   5   // GPIO5 (D4)
  #define I2C_SCL_PIN_DEFAULT   6   // GPIO6 (D5)
  
  // NeoPixel Configuration (no built-in NeoPixel)
  #define NEOPIXEL_PIN_DEFAULT  -1
  #define NEOPIXEL_POWER_PIN    -1
  #define NEOPIXEL_COUNT_DEFAULT 0
  
  // User LED (active low on GPIO21)
  #define USER_LED_PIN          21
  #define USER_LED_ACTIVE_LOW   1
  
  // Battery Monitoring (no dedicated ADC pin - requires external wiring)
  #define BATTERY_ADC_PIN       -1
  #define BATTERY_MONITOR_AVAILABLE 0

// --- Seeed Studio XIAO ESP32S3 Plus (16MB flash, more GPIOs) ---
// Set CONFIG_ARDUINO_VARIANT="XIAO_ESP32S3_Plus" in menuconfig
#elif defined(ARDUINO_XIAO_ESP32S3_PLUS_DEV)
  #define BOARD_SUPPORTED       1
  #define BOARD_NAME            "Seeed XIAO ESP32S3 Plus"
  
  // I2C Bus Configuration (same as base XIAO ESP32S3)
  #define I2C_SDA_PIN_DEFAULT   5   // GPIO5 (D4)
  #define I2C_SCL_PIN_DEFAULT   6   // GPIO6 (D5)
  
  // NeoPixel Configuration (no built-in NeoPixel)
  #define NEOPIXEL_PIN_DEFAULT  -1
  #define NEOPIXEL_POWER_PIN    -1
  #define NEOPIXEL_COUNT_DEFAULT 0
  
  // User LED (active low on GPIO21)
  #define USER_LED_PIN          21
  #define USER_LED_ACTIVE_LOW   1
  
  // Battery Monitoring (Plus has ADC_BAT on GPIO10)
  #define BATTERY_ADC_PIN       10  // GPIO10 (ADC_BAT)
  #define BATTERY_MONITOR_AVAILABLE 1
  
  // Plus-specific: Additional UART
  #define TX1_PIN               42  // GPIO42
  #define RX1_PIN               41  // GPIO41
  
  // Plus-specific: Additional SPI
  #define MOSI1_PIN             11  // GPIO11
  #define MISO1_PIN             12  // GPIO12
  #define SCK1_PIN              13  // GPIO13

// --- Generic ESP32 (fallback with warning) ---
#elif defined(ARDUINO_ESP32_DEV)
  #define BOARD_SUPPORTED       1
  #define BOARD_NAME            "Generic ESP32"
  #warning "Using generic ESP32 pin configuration. Verify I2C pins match your hardware."
  
  // I2C Bus Configuration (common defaults)
  #define I2C_SDA_PIN_DEFAULT   21
  #define I2C_SCL_PIN_DEFAULT   22
  
  // NeoPixel Configuration
  #define NEOPIXEL_PIN_DEFAULT  -1
  #define NEOPIXEL_POWER_PIN    -1
  #define NEOPIXEL_COUNT_DEFAULT 0
  
  // Battery Monitoring
  #define BATTERY_ADC_PIN       -1
  #define BATTERY_MONITOR_AVAILABLE 0

// --- Unsupported Board ---
#else
  #define BOARD_SUPPORTED       0
  #define BOARD_NAME            "Unknown/Unsupported"
  #warning "Board not explicitly supported. Using safe defaults. Check pin configuration!"
  
  // Safe fallback defaults (may not work - user should verify)
  #define I2C_SDA_PIN_DEFAULT   21
  #define I2C_SCL_PIN_DEFAULT   22
  #define NEOPIXEL_PIN_DEFAULT  -1
  #define NEOPIXEL_POWER_PIN    -1
  #define NEOPIXEL_COUNT_DEFAULT 0
  #define BATTERY_ADC_PIN       -1
  #define BATTERY_MONITOR_AVAILABLE 0

#endif

// =============================================================================
// BOARD VALIDATION (compile-time check)
// =============================================================================
// Uncomment to enforce strict board checking (build will fail on unsupported boards)
// #define REQUIRE_SUPPORTED_BOARD 1

#if defined(REQUIRE_SUPPORTED_BOARD) && !BOARD_SUPPORTED
  #error "Unsupported board detected! Please add your board configuration to System_BuildConfig.h or disable REQUIRE_SUPPORTED_BOARD."
#endif

#endif // SYSTEM_BUILD_CONFIG_H
