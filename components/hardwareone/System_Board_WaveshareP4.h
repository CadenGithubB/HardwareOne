#pragma once
// Waveshare LCD-5 BSP/schematic: docs/WAVESHARE_P4_SUPPORT.md.
#define WS_LCD_RESET_PIN 27
#define WS_LCD_BACKLIGHT_PIN 26
#define WS_SPEAKER_AMP_PIN 53
#define WS_SPEAKER_MCLK_PIN 13
#define WS_SPEAKER_BCLK_PIN 12
#define WS_SPEAKER_WS_PIN 10
#define WS_SPEAKER_DATA_PIN 9
#define WS_SPEAKER_I2C_PORT 1
#define WS_SPEAKER_I2C_BUS 0

// ES7210 ADC: standard I2S capture, sharing MCLK/BCLK/WS with playback.
#define MIC_BACKEND_ES7210 1
#define WS_MIC_DATA_PIN 11
#define WS_MIC_I2C_ADDRESS 0x40
#define WS_MIC_I2C_BUS 0

// GT911 shares the codec bus; reset and interrupt are not wired to the MCU.
#define WS_TOUCH_I2C_BUS 0
#define WS_TOUCH_WIDTH 720
#define WS_TOUCH_HEIGHT 1280

// MicroSD: slot 0 IO-MUX pins, separate from the C6 on slot 1.
// Schematic: GPIO45 drives the active-low AO3401 power switch; no CD GPIO.
#define SD_MMC_SLOT 0
#define SD_MMC_CLK_PIN 43
#define SD_MMC_CMD_PIN 44
#define SD_MMC_D0_PIN 39
#define SD_MMC_D1_PIN 40
#define SD_MMC_D2_PIN 41
#define SD_MMC_D3_PIN 42
#define SD_MMC_DETECT_PIN -1
#define SD_MMC_POWER_PIN 45
#define SD_MMC_POWER_ACTIVE_LEVEL 0
#define SD_MMC_LDO_CHANNEL 4
#ifndef SD_MMC_MAX_FREQ_KHZ
#define SD_MMC_MAX_FREQ_KHZ 40000
#endif

// BAT_ADC: R12=200k from BAT, R15=100k to GND (schematic LCD-5).
// Charger STAT goes to TP1, not a P4 GPIO; no cell/USB/charge detection.
#define BATTERY_ADC_PIN 20
#define BATTERY_MONITOR_AVAILABLE 1
#define BATTERY_BACKEND_ADC 1
#define BATTERY_BACKEND_FUEL_GAUGE 0
#ifndef BATTERY_ADC_DIVIDER
#define BATTERY_ADC_DIVIDER 3.0f
#endif
#ifndef BATTERY_ADC_ATTEN
#define BATTERY_ADC_ATTEN ADC_ATTEN_DB_6
#endif

// OV5647 CSI module: SCCB shares the managed touch/audio bus; module supplies XCLK.
#define CAMERA_SCCB_SHARED 1
#define CAMERA_SCCB_MANAGER_BUS 0
#define CAMERA_POWER_PIN -1
#define CAMERA_XCLK_PIN -1
#define CAMERA_RESET_PIN -1
#define CAMERA_SDA_PIN 7
#define CAMERA_SCL_PIN 8
#define CAMERA_NATIVE_WIDTH 800
#define CAMERA_NATIVE_HEIGHT 640
#define CAMERA_BUFFER_COUNT 2
