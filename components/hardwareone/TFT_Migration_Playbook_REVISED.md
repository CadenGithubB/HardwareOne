# TFT Migration Playbook - REVISED (Simpler Approach)

## Why This is Better
Instead of creating separate wrapper files, we integrate the display abstraction directly into `Display_HAL.h` - just like Input HAL does. This keeps all display hardware abstraction in one place.

---

## OPTION 1: Dual-Mode Support (REVISED - No Wrapper Files!)

### Phase 1: Extend Display_HAL.h with Runtime Functions (30 minutes)

#### Step 1.1: Add Display Functions to Display_HAL.h
**File**: `/Users/morgan/esp/hardwareone-idf/components/hardwareone/Display_HAL.h`

Add at the end of the file (before `#endif`):

```cpp
// =============================================================================
// Display Runtime Functions
// =============================================================================
#if DISPLAY_ENABLED

// Global display instance (replaces oledDisplay)
extern DisplayDriver* gDisplay;

// Legacy compatibility alias
#define oledDisplay gDisplay

// Display control functions
bool displayInit();           // Initialize display hardware
void displayClear();          // Clear entire display
void displayUpdate();         // Update display (no-op for TFT, display() for OLED)
void displayDim(bool dim);    // Dim display (brightness control)

#endif // DISPLAY_ENABLED
```

#### Step 1.2: Create Display_HAL.cpp
**File**: `/Users/morgan/esp/hardwareone-idf/components/hardwareone/Display_HAL.cpp`

```cpp
#include "Display_HAL.h"
#include "System_BuildConfig.h"

#if DISPLAY_ENABLED

#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  #include "System_I2C.h"
  #include <Wire.h>
#endif

// Global display instance
DisplayDriver* gDisplay = nullptr;

bool displayInit() {
  if (gDisplay) {
    return true;  // Already initialized
  }
  
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  // I2C OLED initialization
  gDisplay = new Adafruit_SSD1306(DISPLAY_WIDTH, DISPLAY_HEIGHT, &Wire, DISPLAY_RESET_PIN);
  if (!gDisplay) return false;
  
  // Use I2C transaction for OLED
  bool success = i2cDeviceTransaction(DISPLAY_I2C_ADDR, 100000, 100, [&]() -> bool {
    return gDisplay->begin(SSD1306_SWITCHCAPVCC, DISPLAY_I2C_ADDR);
  });
  
  if (!success) {
    delete gDisplay;
    gDisplay = nullptr;
    return false;
  }
  
  gDisplay->clearDisplay();
  gDisplay->display();
  return true;
  
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  // SPI TFT initialization
  gDisplay = new Adafruit_ST7789(DISPLAY_SPI_CS, DISPLAY_SPI_DC, DISPLAY_SPI_RST);
  if (!gDisplay) return false;
  
  gDisplay->init(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  gDisplay->setRotation(0);  // 0=portrait, 1=landscape
  gDisplay->fillScreen(DISPLAY_COLOR_BLACK);
  return true;
  
#else
  return false;
#endif
}

void displayClear() {
  if (!gDisplay) return;
  
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  gDisplay->clearDisplay();
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  gDisplay->fillScreen(DISPLAY_COLOR_BLACK);
#endif
}

void displayUpdate() {
  if (!gDisplay) return;
  
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  gDisplay->display();  // Push framebuffer to OLED
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  // No-op for TFT (direct rendering)
#endif
}

void displayDim(bool dim) {
  if (!gDisplay) return;
  
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  gDisplay->dim(dim);
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  // TFT dimming via backlight PWM (if connected)
  #if DISPLAY_BL_PIN >= 0
    analogWrite(DISPLAY_BL_PIN, dim ? 64 : 255);
  #endif
#endif
}

#endif // DISPLAY_ENABLED
```

#### Step 1.3: Add to CMakeLists.txt
**File**: `/Users/morgan/esp/hardwareone-idf/components/hardwareone/CMakeLists.txt`

Add `Display_HAL.cpp` to the source list (around line 30):
```cmake
"Display_HAL.cpp"
```

### Phase 2: Update Color Constants (1 hour)

Same as before - Global search/replace across 20 files:
- `SSD1306_WHITE` → `DISPLAY_COLOR_WHITE`
- `SSD1306_BLACK` → `DISPLAY_COLOR_BLACK`
- `SSD1306_INVERSE` → `DISPLAY_COLOR_WHITE`

**Files**: All OLED_Mode_*.cpp, OLED_Utils.cpp, i2csensor-*-oled.h, etc.

### Phase 3: Update Core Display Code (1 hour)

#### Step 3.1: Update OLED_Display.h
**File**: `/Users/morgan/esp/hardwareone-idf/components/hardwareone/OLED_Display.h`

**Remove line 40**:
```cpp
#include <Adafruit_SSD1306.h>
```

**Change line 211** from:
```cpp
extern Adafruit_SSD1306* oledDisplay;
```

To:
```cpp
// Display object now provided by Display_HAL.h as gDisplay
// Legacy alias: #define oledDisplay gDisplay (in Display_HAL.h)
```

#### Step 3.2: Update OLED_Utils.cpp
**File**: `/Users/morgan/esp/hardwareone-idf/components/hardwareone/OLED_Utils.cpp`

**Remove old includes** (lines 13-14):
```cpp
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
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
  
  if (!displayInit()) {
    ERROR_SYSTEMF("Display initialization failed");
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
  
  if (!oledIsDirty()) {
    return;
  }
  
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  // I2C OLED - wrap in transaction
  i2cDeviceTransactionVoid(DISPLAY_I2C_ADDR, 100000, 50, [&]() {
    displayClear();
    
    // Render current mode
    const OLEDModeEntry* mode = findOLEDMode(currentOLEDMode);
    if (mode && mode->displayFunc) {
      mode->displayFunc();
    }
    
    renderOLEDFooter();
    displayUpdate();
  });
#else
  // SPI TFT - direct rendering
  displayClear();
  
  // Render current mode
  const OLEDModeEntry* mode = findOLEDMode(currentOLEDMode);
  if (mode && mode->displayFunc) {
    mode->displayFunc();
  }
  
  renderOLEDFooter();
  displayUpdate();  // No-op for TFT
#endif
  
  oledClearDirty();
  oledLastUpdate = millis();
}
```

**Update oledDisplayOff()** and **oledDisplayOn()** functions:
```cpp
void oledDisplayOff() {
#if ENABLE_OLED_DISPLAY
  if (!gDisplay) return;
  
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  i2cDeviceTransactionVoid(DISPLAY_I2C_ADDR, 100000, 50, [&]() {
    gDisplay->ssd1306_command(SSD1306_DISPLAYOFF);
  });
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  #if DISPLAY_BL_PIN >= 0
    digitalWrite(DISPLAY_BL_PIN, LOW);  // Turn off backlight
  #endif
#endif
#endif
}

void oledDisplayOn() {
#if ENABLE_OLED_DISPLAY
  if (!gDisplay) return;
  
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  i2cDeviceTransactionVoid(DISPLAY_I2C_ADDR, 100000, 50, [&]() {
    gDisplay->ssd1306_command(SSD1306_DISPLAYON);
  });
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  #if DISPLAY_BL_PIN >= 0
    digitalWrite(DISPLAY_BL_PIN, HIGH);  // Turn on backlight
  #endif
#endif
#endif
}
```

#### Step 3.3: Update System_FirstTimeSetup.cpp
**File**: `/Users/morgan/esp/hardwareone-idf/components/hardwareone/System_FirstTimeSetup.cpp`

**Change line 374-380**:
```cpp
#if ENABLE_OLED_DISPLAY
    if (gDisplay && oledConnected && oledEnabled) {
      displayClear();
      displayUpdate();
    }
#endif
```

### Phase 4: Test (1-2 hours)

Same as before - test with both OLED and TFT.

---

## OPTION 2: TFT-Only (Even Simpler!)

Same as Option 1, but skip the conditional compilation - just use ST7789 directly.

### Changes:
1. Set `DISPLAY_TYPE` to `DISPLAY_TYPE_ST7789` in System_BuildConfig.h
2. Create `Display_HAL.cpp` with only ST7789 code (no conditionals)
3. Update color constants (same 20 files)
4. Update OLED_Utils.cpp to use `displayInit()`, `displayClear()`, `displayUpdate()`

---

## Summary - REVISED Approach

### What Changed:
- ❌ **Removed**: Separate Display_Wrapper.h/cpp files
- ✅ **Added**: Display functions directly in Display_HAL.h/cpp
- ✅ **Benefit**: Follows Input HAL pattern, all HAL code in one place

### Files Modified:
**Option 1**: 23 files (same as before)
**Option 2**: 22 files (same as before)

### New Files Created:
**Option 1**: 1 file (`Display_HAL.cpp`)
**Option 2**: 1 file (`Display_HAL.cpp`)

### Effort:
**Option 1**: 3-4 hours (reduced from 4-6)
**Option 2**: 2-3 hours (same)

---

## Which Option?

The HAL-integrated approach is cleaner. Now choose:
- **Option 1**: Dual-mode (OLED + TFT) - 3-4 hours
- **Option 2**: TFT-only - 2-3 hours

Both are simpler now that we're using Display_HAL directly!
