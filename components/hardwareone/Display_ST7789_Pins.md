# ST7789 2.0" 320x240 IPS TFT Display - Pin Configuration

## Display Information
- **Product**: Adafruit 2.0" 320x240 Color IPS TFT Display with microSD Card Breakout
- **Product ID**: 4311
- **Guide**: https://learn.adafruit.com/adafruit-2-0-320x240-color-ips-tft-display
- **Controller**: ST7789
- **Resolution**: 240x320 pixels (portrait orientation)
- **Interface**: SPI
- **Features**: 
  - IPS (In-Plane Switching) for wide viewing angles
  - RGB565 color (16-bit, 65K colors)
  - microSD card slot
  - PWM-safe backlight control
  - EYESPI 18-pin 0.5mm FPC connector

## Pin Mapping

### Display Pins (SPI Interface)
| Pin Name | Function | ESP32 Pin | Notes |
|----------|----------|-----------|-------|
| **Vin** | Power (3-5V) | 3.3V or 5V | Reverse polarity protected |
| **3Vo** | 3.3V Output | - | From onboard regulator |
| **GND** | Ground | GND | Power and signal ground |
| **SCK** | SPI Clock | GPIO 18 | Hardware SPI (VSPI_CLK) |
| **MISO** | SPI Data Out | GPIO 19 | For SD card (VSPI_MISO) |
| **MOSI** | SPI Data In | GPIO 23 | Hardware SPI (VSPI_MOSI) |
| **CS** | TFT Chip Select | GPIO 5 | Active low |
| **RST** | Display Reset | GPIO 17 | Can be -1 for auto-reset |
| **D/C** | Data/Command | GPIO 16 | High=Data, Low=Command |
| **SDCS** | SD Card CS | GPIO 15 | For microSD card (optional) |
| **BL** | Backlight PWM | -1 (always on) | Optional PWM control |

### EYESPI Connector Pinout
The display has an 18-pin 0.5mm pitch FPC connector (EYESPI standard):
1. GND
2. 3V3
3. SCK
4. MOSI
5. MISO
6. CS (TFT)
7. D/C
8. RST
9. BL (Backlight)
10. SDCS (SD Card)
11-18. Additional signals/GND

## Configuration in Display_HAL.h

```c
#define DISPLAY_TYPE  DISPLAY_TYPE_ST7789  // In System_BuildConfig.h

// Pin definitions (in Display_HAL.h)
#define DISPLAY_SPI_CS        5     // TFT Chip Select
#define DISPLAY_SPI_DC        16    // Data/Command select
#define DISPLAY_SPI_RST       17    // Reset (can be -1 if using auto-reset)
#define DISPLAY_SPI_MOSI      23    // SPI MOSI (hardware SPI)
#define DISPLAY_SPI_SCLK      18    // SPI Clock (hardware SPI)
#define DISPLAY_SPI_MISO      19    // SPI MISO (for SD card if present)
#define DISPLAY_BL_PIN        -1    // PWM backlight pin (-1 = always on)
#define DISPLAY_SD_CS         15    // SD card chip select
```

## Initialization Code Example

```cpp
#include "Display_HAL.h"

// Create display object using HAL-defined pins
DisplayDriver tft = DisplayDriver(DISPLAY_SPI_CS, DISPLAY_SPI_DC, DISPLAY_SPI_RST);

void setup() {
  // Initialize ST7789 for 240x320 display
  tft.init(240, 320);
  
  // Optional: Set rotation (0-3)
  tft.setRotation(0);  // 0=portrait, 1=landscape, 2=inverted portrait, 3=inverted landscape
  
  // Optional: Configure backlight PWM
  #if DISPLAY_BL_PIN >= 0
    pinMode(DISPLAY_BL_PIN, OUTPUT);
    analogWrite(DISPLAY_BL_PIN, 255);  // Full brightness
  #endif
  
  // Clear screen
  tft.fillScreen(DISPLAY_COLOR_BLACK);
  
  // Test drawing
  tft.setCursor(0, 0);
  tft.setTextColor(DISPLAY_COLOR_WHITE);
  tft.setTextSize(2);
  tft.println("Hello ST7789!");
}
```

## Hardware SPI vs Software SPI

### Hardware SPI (Recommended - Faster)
- Uses dedicated SPI pins (VSPI on ESP32)
- Much faster performance
- Required for smooth graphics and animations
- Pin assignments:
  - MOSI: GPIO 23
  - MISO: GPIO 19
  - SCK: GPIO 18
  - CS: Any GPIO (we use GPIO 5)

### Software SPI (Flexible but Slower)
- Can use any GPIO pins
- Slower performance
- Useful if hardware SPI pins are already in use
- Specify all pins in constructor:
  ```cpp
  Adafruit_ST7789 tft = Adafruit_ST7789(CS, DC, MOSI, SCLK, RST);
  ```

## Backlight Control

The display has a PWM-safe backlight:
- **Default**: Pulled high (backlight always on)
- **PWM Control**: Connect to ESP32 PWM-capable GPIO
- **On/Off**: Pull high (on) or low (off)
- **Brightness**: Use PWM duty cycle (0-255)

Example PWM control:
```cpp
#define DISPLAY_BL_PIN 4  // Change in Display_HAL.h

void setBacklight(uint8_t brightness) {
  analogWrite(DISPLAY_BL_PIN, brightness);  // 0-255
}
```

## SD Card Usage

The breakout includes a microSD card slot:
- Uses same SPI bus as display
- Separate chip select (SDCS pin)
- Requires SdFat library
- Can store images, fonts, data files

## Notes

1. **Auto-Reset**: The display has an auto-reset circuit, so RST pin can be set to -1
2. **Level Shifting**: Breakout includes CD74HC4050 level shifter (~10ns propagation delay)
3. **Power**: Can run on 3.3V or 5V (onboard regulator provides 3.3V to display)
4. **Orientation**: Default is portrait (240x320), use `setRotation()` for landscape
5. **Performance**: Use hardware SPI for best performance with color graphics
6. **Memory**: RGB565 requires 2 bytes per pixel (240×320 = 153,600 bytes for full framebuffer)

## Troubleshooting

- **Blank screen**: Check power, CS, DC, RST connections
- **Garbled display**: Verify SPI clock speed isn't too high
- **Wrong colors**: Check RGB565 color format
- **Slow performance**: Ensure using hardware SPI, not software SPI
- **SD card not working**: Check SDCS pin and SdFat library installation
