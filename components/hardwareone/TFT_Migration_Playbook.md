# TFT Migration Playbook - Implementation Plan

## Choose Your Approach

### Option 1: Dual-Mode Support (Recommended)
**Pros**: Keep both OLED and TFT working, switch at compile time  
**Cons**: Slightly more complex, maintains both code paths  
**Effort**: ~4-6 hours  
**Best for**: If you want flexibility or might use both displays

### Option 2: TFT-Only Migration  
**Pros**: Simpler, less code to maintain  
**Cons**: Removes OLED support permanently  
**Effort**: ~2-3 hours  
**Best for**: If you're committed to TFT only

---

## OPTION 1: DUAL-MODE SUPPORT PLAYBOOK

### Phase 1: Create Display Abstraction Layer (1-2 hours)

#### Step 1.1: Create Display_Wrapper.h
**File**: `/Users/morgan/esp/hardwareone-idf/components/hardwareone/Display_Wrapper.h`

```cpp
#ifndef DISPLAY_WRAPPER_H
#define DISPLAY_WRAPPER_H

#include "Display_HAL.h"
#include <Adafruit_GFX.h>

#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  #include <Adafruit_SSD1306.h>
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  #include <Adafruit_ST7789.h>
#endif

// Unified display wrapper that works with both OLED and TFT
class DisplayWrapper {
public:
  DisplayWrapper();
  ~DisplayWrapper();
  
  // Initialization
  bool begin();
  
  // Display control
  void clearDisplay();      // Clear entire display
  void display();           // Update display (no-op for TFT)
  void invertDisplay(bool i);
  void dim(bool dim);
  
  // Drawing primitives (pass-through to Adafruit_GFX)
  void drawPixel(int16_t x, int16_t y, uint16_t color);
  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color);
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color);
  void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void drawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color);
  void fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color);
  void drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color);
  void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color);
  void drawRoundRect(int16_t x0, int16_t y0, int16_t w, int16_t h, int16_t radius, uint16_t color);
  void fillRoundRect(int16_t x0, int16_t y0, int16_t w, int16_t h, int16_t radius, uint16_t color);
  
  // Text functions
  void setCursor(int16_t x, int16_t y);
  void setTextColor(uint16_t c);
  void setTextColor(uint16_t c, uint16_t bg);
  void setTextSize(uint8_t s);
  void setTextWrap(bool w);
  void setRotation(uint8_t r);
  
  // Print functions (inherited from Print class)
  size_t print(const char* str);
  size_t print(const String& str);
  size_t println(const char* str);
  size_t println(const String& str);
  size_t printf(const char* format, ...);
  
  // Get underlying GFX object for advanced operations
  Adafruit_GFX* getGFX();
  
  // Display info
  int16_t width() const;
  int16_t height() const;
  
private:
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  Adafruit_SSD1306* display;
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  Adafruit_ST7789* display;
#endif
};

// Global display instance (replaces oledDisplay)
extern DisplayWrapper* gDisplay;

// Legacy compatibility
#define oledDisplay gDisplay

#endif // DISPLAY_WRAPPER_H
```

#### Step 1.2: Create Display_Wrapper.cpp
**File**: `/Users/morgan/esp/hardwareone-idf/components/hardwareone/Display_Wrapper.cpp`

```cpp
#include "Display_Wrapper.h"
#include "System_I2C.h"
#include <Wire.h>

DisplayWrapper* gDisplay = nullptr;

DisplayWrapper::DisplayWrapper() : display(nullptr) {
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  display = new Adafruit_SSD1306(DISPLAY_WIDTH, DISPLAY_HEIGHT, &Wire, DISPLAY_RESET_PIN);
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  display = new Adafruit_ST7789(DISPLAY_SPI_CS, DISPLAY_SPI_DC, DISPLAY_SPI_RST);
#endif
}

DisplayWrapper::~DisplayWrapper() {
  if (display) {
    delete display;
    display = nullptr;
  }
}

bool DisplayWrapper::begin() {
  if (!display) return false;
  
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  // I2C OLED initialization
  if (!display->begin(SSD1306_SWITCHCAPVCC, DISPLAY_I2C_ADDR)) {
    return false;
  }
  display->clearDisplay();
  display->display();
  return true;
  
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  // SPI TFT initialization
  display->init(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  display->setRotation(0);  // 0=portrait, 1=landscape
  display->fillScreen(DISPLAY_COLOR_BLACK);
  return true;
#endif
}

void DisplayWrapper::clearDisplay() {
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  display->clearDisplay();
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  display->fillScreen(DISPLAY_COLOR_BLACK);
#endif
}

void DisplayWrapper::display() {
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  display->display();  // Push framebuffer to OLED
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  // No-op for TFT (direct rendering)
#endif
}

void DisplayWrapper::invertDisplay(bool i) {
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  display->invertDisplay(i);
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  display->invertDisplay(i);
#endif
}

void DisplayWrapper::dim(bool dim) {
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  display->dim(dim);
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  // TFT dimming via backlight PWM (if connected)
  #if DISPLAY_BL_PIN >= 0
    analogWrite(DISPLAY_BL_PIN, dim ? 64 : 255);
  #endif
#endif
}

// Drawing primitives - pass through to underlying display
void DisplayWrapper::drawPixel(int16_t x, int16_t y, uint16_t color) {
  display->drawPixel(x, y, color);
}

void DisplayWrapper::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
  display->drawLine(x0, y0, x1, y1, color);
}

void DisplayWrapper::drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
  display->drawFastVLine(x, y, h, color);
}

void DisplayWrapper::drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
  display->drawFastHLine(x, y, w, color);
}

void DisplayWrapper::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  display->drawRect(x, y, w, h, color);
}

void DisplayWrapper::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  display->fillRect(x, y, w, h, color);
}

void DisplayWrapper::drawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color) {
  display->drawCircle(x0, y0, r, color);
}

void DisplayWrapper::fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color) {
  display->fillCircle(x0, y0, r, color);
}

void DisplayWrapper::drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color) {
  display->drawTriangle(x0, y0, x1, y1, x2, y2, color);
}

void DisplayWrapper::fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color) {
  display->fillTriangle(x0, y0, x1, y1, x2, y2, color);
}

void DisplayWrapper::drawRoundRect(int16_t x0, int16_t y0, int16_t w, int16_t h, int16_t radius, uint16_t color) {
  display->drawRoundRect(x0, y0, w, h, radius, color);
}

void DisplayWrapper::fillRoundRect(int16_t x0, int16_t y0, int16_t w, int16_t h, int16_t radius, uint16_t color) {
  display->fillRoundRect(x0, y0, w, h, radius, color);
}

// Text functions
void DisplayWrapper::setCursor(int16_t x, int16_t y) {
  display->setCursor(x, y);
}

void DisplayWrapper::setTextColor(uint16_t c) {
  display->setTextColor(c);
}

void DisplayWrapper::setTextColor(uint16_t c, uint16_t bg) {
  display->setTextColor(c, bg);
}

void DisplayWrapper::setTextSize(uint8_t s) {
  display->setTextSize(s);
}

void DisplayWrapper::setTextWrap(bool w) {
  display->setTextWrap(w);
}

void DisplayWrapper::setRotation(uint8_t r) {
  display->setRotation(r);
}

// Print functions
size_t DisplayWrapper::print(const char* str) {
  return display->print(str);
}

size_t DisplayWrapper::print(const String& str) {
  return display->print(str);
}

size_t DisplayWrapper::println(const char* str) {
  return display->println(str);
}

size_t DisplayWrapper::println(const String& str) {
  return display->println(str);
}

size_t DisplayWrapper::printf(const char* format, ...) {
  va_list args;
  va_start(args, format);
  char buffer[256];
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  return display->print(buffer);
}

Adafruit_GFX* DisplayWrapper::getGFX() {
  return display;
}

int16_t DisplayWrapper::width() const {
  return DISPLAY_WIDTH;
}

int16_t DisplayWrapper::height() const {
  return DISPLAY_HEIGHT;
}
```

#### Step 1.3: Add to CMakeLists.txt
**File**: `/Users/morgan/esp/hardwareone-idf/components/hardwareone/CMakeLists.txt`

Add `Display_Wrapper.cpp` to the source list (around line 30):
```cmake
"Display_Wrapper.cpp"
```

### Phase 2: Update Color Constants (1 hour)

#### Step 2.1: Global Search and Replace
Run these replacements across all OLED files:

**Search**: `SSD1306_WHITE`  
**Replace**: `DISPLAY_COLOR_WHITE`

**Search**: `SSD1306_BLACK`  
**Replace**: `DISPLAY_COLOR_BLACK`

**Search**: `SSD1306_INVERSE`  
**Replace**: `DISPLAY_COLOR_WHITE`  (no inverse on TFT)

**Files to update** (20 files):
- OLED_Mode_Animations.cpp
- OLED_Mode_CLI.cpp
- OLED_Mode_FileBrowser.cpp
- OLED_Mode_Logging.cpp
- OLED_Mode_Login.cpp
- OLED_Mode_Logout.cpp
- OLED_Mode_Map.cpp
- OLED_Mode_Menu.cpp
- OLED_Mode_Network.cpp
- OLED_Mode_Power.cpp
- OLED_Mode_Sensors.cpp
- OLED_Mode_Settings.cpp
- OLED_Mode_System.cpp
- OLED_Utils.cpp
- OLED_Footer.h
- OLED_SettingsEditor.cpp
- OLED_FirstTimeSetup.cpp
- i2csensor-*-oled.h (all 7 files)

### Phase 3: Update Core Display Code (1-2 hours)

#### Step 3.1: Update OLED_Display.h
**File**: `/Users/morgan/esp/hardwareone-idf/components/hardwareone/OLED_Display.h`

**Change line 40-41** from:
```cpp
#include <Adafruit_SSD1306.h>
```

To:
```cpp
#include "Display_Wrapper.h"
```

**Change line 211** from:
```cpp
extern Adafruit_SSD1306* oledDisplay;
```

To:
```cpp
// Display object now provided by Display_Wrapper.h as gDisplay
// Legacy alias: #define oledDisplay gDisplay
```

#### Step 3.2: Update OLED_Utils.cpp
**File**: `/Users/morgan/esp/hardwareone-idf/components/hardwareone/OLED_Utils.cpp`

**Add at top** (after existing includes):
```cpp
#include "Display_Wrapper.h"
```

**Remove old declaration** (around line 211):
```cpp
Adafruit_SSD1306* oledDisplay = nullptr;
```

**Update initOLEDDisplay()** function (around line 500):
```cpp
bool initOLEDDisplay() {
  if (gDisplay) {
    return true;  // Already initialized
  }
  
  gDisplay = new DisplayWrapper();
  if (!gDisplay) {
    ERROR_SYSTEMF("Failed to allocate display wrapper");
    return false;
  }
  
  bool success = false;
  
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  // I2C OLED - use transaction wrapper
  success = i2cDeviceTransaction(DISPLAY_I2C_ADDR, 100000, 100, [&]() -> bool {
    return gDisplay->begin();
  });
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  // SPI TFT - direct initialization
  success = gDisplay->begin();
#endif
  
  if (!success) {
    ERROR_SYSTEMF("Display initialization failed");
    delete gDisplay;
    gDisplay = nullptr;
    oledConnected = false;
    return false;
  }
  
  oledConnected = true;
  oledEnabled = true;
  INFO_SYSTEMF("Display initialized: %s (%dx%d)", 
               DISPLAY_NAME, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  return true;
}
```

**Update updateOLEDDisplay()** function:
```cpp
void updateOLEDDisplay() {
  if (!gDisplay || !oledConnected || !oledEnabled) {
    return;
  }
  
  // Check if anything changed
  if (!oledIsDirty()) {
    return;
  }
  
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  // I2C OLED - wrap in transaction
  i2cDeviceTransactionVoid(DISPLAY_I2C_ADDR, 100000, 50, [&]() {
    gDisplay->clearDisplay();
    
    // Render current mode
    const OLEDModeEntry* mode = findOLEDMode(currentOLEDMode);
    if (mode && mode->displayFunc) {
      mode->displayFunc();
    }
    
    // Render footer
    renderOLEDFooter();
    
    gDisplay->display();  // Push framebuffer
  });
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  // SPI TFT - direct rendering
  gDisplay->clearDisplay();
  
  // Render current mode
  const OLEDModeEntry* mode = findOLEDMode(currentOLEDMode);
  if (mode && mode->displayFunc) {
    mode->displayFunc();
  }
  
  // Render footer
  renderOLEDFooter();
  
  // No display() call needed for TFT
#endif
  
  oledClearDirty();
  oledLastUpdate = millis();
}
```

#### Step 3.3: Update System_FirstTimeSetup.cpp
**File**: `/Users/morgan/esp/hardwareone-idf/components/hardwareone/System_FirstTimeSetup.cpp`

**Change line 374-380** from:
```cpp
#if ENABLE_OLED_DISPLAY
    if (oledDisplay && oledConnected && oledEnabled) {
      i2cDeviceTransactionVoid(0x3D, 100000, 50, [&]() {
        oledDisplay->clearDisplay();
        oledDisplay->display();
      });
    }
#endif
```

To:
```cpp
#if ENABLE_OLED_DISPLAY
    if (gDisplay && oledConnected && oledEnabled) {
      gDisplay->clearDisplay();
      gDisplay->display();
    }
#endif
```

### Phase 4: Test and Verify (1-2 hours)

#### Step 4.1: Test with OLED (Current Hardware)
1. Ensure `DISPLAY_TYPE` is set to `DISPLAY_TYPE_SSD1306` in System_BuildConfig.h
2. Build: `idf.py build`
3. Flash and test all display modes
4. Verify no regressions

#### Step 4.2: Test with TFT (New Hardware)
1. Change `DISPLAY_TYPE` to `DISPLAY_TYPE_ST7789` in System_BuildConfig.h
2. Build: `idf.py build`
3. Connect ST7789 display with proper wiring
4. Flash and test all display modes
5. Verify rendering, colors, and responsiveness

---

## OPTION 2: TFT-ONLY MIGRATION PLAYBOOK

### Phase 1: Update Display Type (5 minutes)

#### Step 1.1: Change System_BuildConfig.h
**File**: `/Users/morgan/esp/hardwareone-idf/components/hardwareone/System_BuildConfig.h`

Change:
```cpp
#define DISPLAY_TYPE  DISPLAY_TYPE_SSD1306
```

To:
```cpp
#define DISPLAY_TYPE  DISPLAY_TYPE_ST7789
```

### Phase 2: Update Color Constants (1 hour)

Same as Option 1, Phase 2 - Global search/replace across 20 files.

### Phase 3: Update Display Object (30 minutes)

#### Step 3.1: Update OLED_Display.h
**File**: `/Users/morgan/esp/hardwareone-idf/components/hardwareone/OLED_Display.h`

**Change line 40** from:
```cpp
#include <Adafruit_SSD1306.h>
```

To:
```cpp
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  #include <Adafruit_SSD1306.h>
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  #include <Adafruit_ST7789.h>
#endif
```

**Change line 211** from:
```cpp
extern Adafruit_SSD1306* oledDisplay;
```

To:
```cpp
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  extern Adafruit_SSD1306* oledDisplay;
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  extern Adafruit_ST7789* oledDisplay;
#endif
```

#### Step 3.2: Update OLED_Utils.cpp
**File**: `/Users/morgan/esp/hardwareone-idf/components/hardwareone/OLED_Utils.cpp`

**Change display declaration** (around line 211):
```cpp
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  Adafruit_SSD1306* oledDisplay = nullptr;
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  Adafruit_ST7789* oledDisplay = nullptr;
#endif
```

**Update initOLEDDisplay()** function:
```cpp
bool initOLEDDisplay() {
  if (oledDisplay) {
    return true;
  }
  
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  oledDisplay = new Adafruit_SSD1306(DISPLAY_WIDTH, DISPLAY_HEIGHT, &Wire, OLED_RESET);
  if (!oledDisplay) return false;
  
  bool success = i2cDeviceTransaction(DISPLAY_I2C_ADDR, 100000, 100, [&]() -> bool {
    return oledDisplay->begin(SSD1306_SWITCHCAPVCC, DISPLAY_I2C_ADDR);
  });
  
  if (!success) {
    delete oledDisplay;
    oledDisplay = nullptr;
    return false;
  }
  
  oledDisplay->clearDisplay();
  oledDisplay->display();
  
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  oledDisplay = new Adafruit_ST7789(DISPLAY_SPI_CS, DISPLAY_SPI_DC, DISPLAY_SPI_RST);
  if (!oledDisplay) return false;
  
  oledDisplay->init(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  oledDisplay->setRotation(0);  // 0=portrait
  oledDisplay->fillScreen(DISPLAY_COLOR_BLACK);
#endif
  
  oledConnected = true;
  oledEnabled = true;
  return true;
}
```

**Update updateOLEDDisplay()** function:
```cpp
void updateOLEDDisplay() {
  if (!oledDisplay || !oledConnected || !oledEnabled) return;
  if (!oledIsDirty()) return;
  
  oledDisplay->clearDisplay();  // Works for both (fillScreen for TFT)
  
  // Render current mode
  const OLEDModeEntry* mode = findOLEDMode(currentOLEDMode);
  if (mode && mode->displayFunc) {
    mode->displayFunc();
  }
  
  renderOLEDFooter();
  
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  oledDisplay->display();  // Only for OLED framebuffer
#endif
  
  oledClearDirty();
  oledLastUpdate = millis();
}
```

### Phase 4: Test (1 hour)

1. Build: `idf.py build`
2. Connect ST7789 display
3. Flash and test all modes
4. Verify rendering and colors

---

## Summary Comparison

| Aspect | Option 1: Dual-Mode | Option 2: TFT-Only |
|--------|---------------------|-------------------|
| **Effort** | 4-6 hours | 2-3 hours |
| **Files Created** | 2 new files | 0 new files |
| **Files Modified** | 23 files | 22 files |
| **OLED Support** | ✅ Maintained | ❌ Removed |
| **TFT Support** | ✅ Added | ✅ Added |
| **Flexibility** | High | Low |
| **Code Complexity** | Medium | Low |
| **Future-Proof** | Yes | No |

## Recommendation

**Option 1 (Dual-Mode)** is recommended because:
1. Maintains backward compatibility
2. Allows testing without hardware commitment
3. Future-proof for different display types
4. Only 2-3 hours more work
5. Cleaner abstraction pattern

Choose **Option 2 (TFT-Only)** if:
- You're certain you'll never use OLED again
- You want the simplest possible migration
- You want to minimize code maintenance

## Next Steps

1. **Choose Option 1 or Option 2**
2. **Approve this playbook**
3. **I will implement the chosen option step-by-step**
4. **Test with your hardware**
5. **Iterate on any issues**
