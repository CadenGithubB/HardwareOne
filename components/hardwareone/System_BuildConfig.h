#ifndef SYSTEM_BUILD_CONFIG_H
#define SYSTEM_BUILD_CONFIG_H

// =============================================================================
// I2C FEATURE LEVEL CONFIGURATION
// =============================================================================
// Set ONE of these levels to control I2C functionality:
//
//   I2C_LEVEL_FULL (3)       - All I2C features: OLED + all sensors
//   I2C_LEVEL_STANDALONE (2) - OLED + Gamepad only (standalone device control)
//   I2C_LEVEL_OLED_ONLY (1)  - OLED display only, no sensors
//   I2C_LEVEL_DISABLED (0)   - No I2C at all (maximum memory savings)
//
// =============================================================================

#define I2C_LEVEL_DISABLED   0
#define I2C_LEVEL_OLED_ONLY  1
#define I2C_LEVEL_STANDALONE 2  // OLED + Gamepad for standalone control
#define I2C_LEVEL_FULL       3

// ┌─────────────────────────────────────────────────────────────────────────┐
// │  SET YOUR DESIRED LEVEL HERE:                                           │
// └─────────────────────────────────────────────────────────────────────────┘
// Minimal build: disable all I2C features (no OLED, no sensors, no gamepad).
#define I2C_FEATURE_LEVEL    I2C_LEVEL_FULL

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

#else  // I2C_LEVEL_FULL (default)
  // Level 2: Everything enabled
  #define ENABLE_I2C_SYSTEM       1
  #define ENABLE_OLED_DISPLAY     1
  #define ENABLE_THERMAL_SENSOR   1
  #define ENABLE_TOF_SENSOR       1
  #define ENABLE_IMU_SENSOR       1
  #define ENABLE_GAMEPAD_SENSOR   1
  #define ENABLE_APDS_SENSOR      1
  #define ENABLE_GPS_SENSOR       1
  #define ENABLE_FM_RADIO         1

#endif

// =============================================================================
// BLUETOOTH CONFIGURATION (independent of I2C)
// =============================================================================
// BLE support using NimBLE stack for smart glasses and external device communication
//
//   0 = Disabled (saves ~60-80KB flash, ~30-40KB RAM)
//   1 = Enabled  (BLE server with GATT services)
//
#define ENABLE_BLUETOOTH        1 

// =============================================================================
// DISPLAY CONFIGURATION
// =============================================================================
// Display type selection for compile-time hardware abstraction.
// This allows swapping between different display hardware without code changes.
//
// Supported display types:
//   DISPLAY_TYPE_NONE     (0) - No display
//   DISPLAY_TYPE_SSD1306  (1) - 128x64 monochrome OLED (I2C, current default)
//   DISPLAY_TYPE_ST7789   (2) - 240x240 or 240x320 color TFT (SPI) [future]
//   DISPLAY_TYPE_ILI9341  (3) - 320x240 color TFT (SPI) [future]
//
// Note: Only SSD1306 is fully implemented. Other types are placeholders for future.
// =============================================================================

#define DISPLAY_TYPE_NONE      0
#define DISPLAY_TYPE_SSD1306   1
#define DISPLAY_TYPE_ST7789    2
#define DISPLAY_TYPE_ILI9341   3

// ┌─────────────────────────────────────────────────────────────────────────┐
// │  SET YOUR DISPLAY TYPE HERE:                                            │
// └─────────────────────────────────────────────────────────────────────────┘
#define DISPLAY_TYPE  DISPLAY_TYPE_SSD1306

// =============================================================================
// NETWORK CONFIGURATION
// =============================================================================

#define NET_LEVEL_DISABLED   0
#define NET_LEVEL_WIFI_ONLY  1
#define NET_LEVEL_WIFI_HTTP  2
#define NET_LEVEL_WIFI_ESPNOW 3

// Compile-time network feature level.
// NET_LEVEL_DISABLED disables WiFi, HTTP server, and ESP-NOW.
#define NETWORK_FEATURE_LEVEL NET_LEVEL_WIFI_ESPNOW

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
