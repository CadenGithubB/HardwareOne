#pragma once

// ESP32-P4X-EYE / P4-EYE MB v2.3 wiring, verified against Espressif's BSP:
// https://github.com/espressif/esp-bsp/blob/master/bsp/esp32_p4_eye/include/bsp/esp32_p4_eye.h
// A P4 CPU alone does not identify this board. Select HW_BOARD_P4X_EYE=1 in
// the board/deployment profile; optional device choices remain independent.
#if !defined(CONFIG_IDF_TARGET_ESP32P4)
#error "HW_BOARD_P4X_EYE requires the ESP32-P4 target"
#endif

#define EYE_LCD_SCLK 17
#define EYE_LCD_MOSI 16
#define EYE_LCD_DC 19
#define EYE_LCD_CS 18
#define EYE_LCD_RST 15
#define EYE_LCD_BL 20
#define EYE_LCD_EN 12
#define EYE_LCD_BL_ACTIVE_LOW 1

#define GPIO_ENCODER_PIN_A 48
#define GPIO_ENCODER_PIN_B 47
#define GPIO_ENCODER_PIN_BUTTON 2
#ifndef GPIO_ENCODER_STEPS_PER_DETENT
#define GPIO_ENCODER_STEPS_PER_DETENT 4
#endif
#ifndef GPIO_ENCODER_INVERT
#define GPIO_ENCODER_INVERT 0
#endif
#define GPIO_ENCODER_BUTTON_ACTIVE_LOW 1

// The three user keys (not BOOT/RESET), verified against Espressif's factory
// demo BSP and MB v2.3 / P4X MB v2.4 schematics. All are pulled low when pressed.
// https://github.com/espressif/esp-dev-kits/blob/master/examples/esp32-p4-eye/examples/common_components/esp32_p4_eye/include/bsp/esp32_p4_eye.h
#define EYE_BUTTON_1_PIN 3
#define EYE_BUTTON_2_PIN 4
#define EYE_BUTTON_3_PIN 5
// {pin, activeLow, HAL logical action bit}: X, Y, START complement the wheel's
// A (tap), B (hold) and SELECT (double tap). These are independent momentary keys.
#define GPIO_ENCODER_AUX_BUTTONS { \
  {EYE_BUTTON_1_PIN, true, 1u << 2}, \
  {EYE_BUTTON_2_PIN, true, 1u << 3}, \
  {EYE_BUTTON_3_PIN, true, 1u << 4} \
}

// The radio companion owns slot 1 (GPIO27..32). The card must use slot 0,
// with per-slot teardown; deinitializing the whole host would stop networking.
#define SD_MMC_SLOT 0
#define SD_MMC_CLK_PIN 43
#define SD_MMC_CMD_PIN 44
#define SD_MMC_D0_PIN 39
#define SD_MMC_D1_PIN 40
#define SD_MMC_D2_PIN 41
#define SD_MMC_D3_PIN 42
#define SD_MMC_DETECT_PIN 45
#define SD_MMC_POWER_PIN 46
#define SD_MMC_POWER_ACTIVE_LEVEL 0
#define SD_MMC_LDO_CHANNEL 4
#ifndef SD_MMC_MAX_FREQ_KHZ
#define SD_MMC_MAX_FREQ_KHZ 20000
#endif
