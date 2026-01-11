# TFT Display Migration Guide

## Summary: What Needs to Change for ST7789 Support

Good news: **Most of your display code is already hardware-agnostic!** The OLED modularization you did has paid off. However, there are some hardware-specific assumptions baked into the rendering code that need to be addressed.

## Code Distribution Analysis

### ✅ Already Hardware-Agnostic (No Changes Needed)
These files work with the Display HAL and will automatically adapt:
- **All OLED_Mode_*.cpp files** - Use `SCREEN_WIDTH`, `SCREEN_HEIGHT` macros
- **All i2csensor-*-oled.h files** - Sensor display modules
- **OLED_Display.h** - Already includes `Display_HAL.h`
- **Display dimensions** - All use `SCREEN_WIDTH`, `SCREEN_HEIGHT`, `OLED_CONTENT_HEIGHT`, `OLED_FOOTER_HEIGHT`
- **System_FirstTimeSetup.cpp** - Only one `oledDisplay->` call, properly guarded

### ⚠️ Hardware-Specific Code (Needs Abstraction)

#### 1. **Direct Hardware Access** (`oledDisplay` global)
**Location**: Throughout OLED_*.cpp files
**Issue**: Code directly calls `oledDisplay->` methods (Adafruit_SSD1306 specific)
**Examples**:
```cpp
oledDisplay->setTextSize(1);
oledDisplay->setTextColor(SSD1306_WHITE);
oledDisplay->setCursor(0, 0);
oledDisplay->print("Hello");
oledDisplay->fillRect(x, y, w, h, SSD1306_BLACK);
oledDisplay->drawLine(x1, y1, x2, y2, SSD1306_WHITE);
```

**Solution**: Create a unified display wrapper or use Adafruit_GFX base class

#### 2. **Monochrome Color Assumptions**
**Location**: All OLED rendering code
**Issue**: Uses `SSD1306_WHITE`, `SSD1306_BLACK`, `SSD1306_INVERSE` (1-bit colors)
**Examples**:
```cpp
display->setTextColor(SSD1306_WHITE);
display->fillRect(0, 0, 128, 64, SSD1306_BLACK);
display->drawRect(x, y, w, h, SSD1306_WHITE);
```

**Solution**: Use `DISPLAY_COLOR_*` macros from Display_HAL.h instead

#### 3. **Display Update Pattern**
**Location**: OLED_Utils.cpp, all mode files
**Issue**: SSD1306 requires `display()` call to push framebuffer to screen
**Pattern**:
```cpp
oledDisplay->clearDisplay();  // Clear framebuffer
// ... draw stuff ...
oledDisplay->display();       // Push to screen
```

**ST7789 Pattern** (no framebuffer):
```cpp
tft.fillScreen(BLACK);  // Direct write to screen
// ... draw stuff ... (writes immediately)
// No display() call needed
```

#### 4. **I2C Transaction Wrapping**
**Location**: OLED_Utils.cpp initialization
**Issue**: OLED uses I2C transactions with mutex locking
**Example**:
```cpp
i2cDeviceTransactionVoid(0x3D, 100000, 50, [&]() {
  oledDisplay->clearDisplay();
  oledDisplay->display();
});
```

**ST7789**: Uses SPI, no I2C transactions needed

## Recommended Migration Strategy

### Option 1: Dual-Mode Support (Recommended)
Keep both OLED and TFT support, switch at compile time:

1. **Create Display Wrapper Class** (`Display_Wrapper.h/cpp`):
```cpp
class DisplayWrapper {
public:
  void init();
  void clear();
  void update();  // No-op for TFT, display() for OLED
  void setTextSize(uint8_t size);
  void setTextColor(uint16_t color);
  void setCursor(int16_t x, int16_t y);
  void print(const char* text);
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  // ... etc for all drawing primitives
  
private:
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
  Adafruit_SSD1306* display;
#elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
  Adafruit_ST7789* display;
#endif
};

extern DisplayWrapper* gDisplay;  // Replace oledDisplay
```

2. **Update Color References**:
   - Replace `SSD1306_WHITE` → `DISPLAY_COLOR_WHITE`
   - Replace `SSD1306_BLACK` → `DISPLAY_COLOR_BLACK`
   - Already defined in Display_HAL.h!

3. **Rename Files** (Optional but recommended):
   - `OLED_Utils.cpp` → `Display_Utils.cpp`
   - `OLED_Mode_*.cpp` → `Display_Mode_*.cpp`
   - `OLED_Display.h` → `Display_Core.h`
   - Keep backward compatibility with `#define` aliases

### Option 2: TFT-Only (Simpler)
If you're only using TFT going forward:

1. **Change Display Type**:
   ```c
   #define DISPLAY_TYPE  DISPLAY_TYPE_ST7789  // In System_BuildConfig.h
   ```

2. **Replace oledDisplay Global**:
   ```cpp
   // In OLED_Utils.cpp
   #if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
     Adafruit_SSD1306* oledDisplay = nullptr;
   #elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
     Adafruit_ST7789* oledDisplay = nullptr;  // Reuse same variable name
   #endif
   ```

3. **Update Initialization**:
   ```cpp
   bool initOLEDDisplay() {
   #if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
     oledDisplay = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
     if (!oledDisplay->begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS)) {
       return false;
     }
   #elif DISPLAY_TYPE == DISPLAY_TYPE_ST7789
     oledDisplay = new Adafruit_ST7789(DISPLAY_SPI_CS, DISPLAY_SPI_DC, DISPLAY_SPI_RST);
     oledDisplay->init(240, 320);
     oledDisplay->setRotation(0);  // 0=portrait, 1=landscape
   #endif
     return true;
   }
   ```

4. **Remove `display()` Calls for TFT**:
   ```cpp
   void updateOLEDDisplay() {
     if (!oledDisplay) return;
     
     oledDisplay->clearDisplay();  // Works for both (fillScreen for TFT)
     
     // ... render current mode ...
     
   #if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
     oledDisplay->display();  // Only needed for OLED framebuffer
   #endif
   }
   ```

## Specific Files That Need Updates

### Core Files (Must Update)
1. **OLED_Utils.cpp** (~4200 lines)
   - `initOLEDDisplay()` - Add ST7789 initialization
   - `updateOLEDDisplay()` - Conditional `display()` call
   - `oledDisplay` global declaration - Make type conditional
   - Remove I2C transaction wrapping for TFT

2. **OLED_Display.h**
   - Update `oledDisplay` extern declaration to be type-conditional
   - Already includes Display_HAL.h ✓

### Mode Files (Color Updates Only)
All 13 OLED_Mode_*.cpp files need:
- Replace `SSD1306_WHITE` → `DISPLAY_COLOR_WHITE`
- Replace `SSD1306_BLACK` → `DISPLAY_COLOR_BLACK`
- Replace `SSD1306_INVERSE` → `DISPLAY_COLOR_WHITE` (no inverse on TFT)

**Files**:
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

### Sensor OLED Files (Color Updates Only)
All 7 i2csensor-*-oled.h files need color constant updates:
- i2csensor-apds9960-oled.h
- i2csensor-bno055-oled.h
- i2csensor-mlx90640-oled.h
- i2csensor-pa1010d-oled.h
- i2csensor-rda5807-oled.h
- i2csensor-seesaw-oled.h
- i2csensor-vl53l4cx-oled.h

### Utility Files
1. **OLED_Utils.cpp** - Scrolling system uses hardcoded colors
2. **OLED_Footer.h** - Footer rendering
3. **OLED_SettingsEditor.cpp** - Settings UI
4. **OLED_FirstTimeSetup.cpp** - Setup wizard
5. **System_FirstTimeSetup.cpp** - One `oledDisplay->` call (line 377)

## Key Differences: OLED vs TFT

| Feature | SSD1306 (OLED) | ST7789 (TFT) |
|---------|----------------|--------------|
| **Interface** | I2C | SPI |
| **Resolution** | 128x64 | 240x320 |
| **Color** | 1-bit (B&W) | 16-bit RGB565 |
| **Framebuffer** | Yes (in RAM) | No (direct write) |
| **Update** | `display()` call | Immediate |
| **Speed** | Slower (I2C) | Faster (SPI) |
| **Memory** | 1KB framebuffer | No framebuffer |
| **Colors** | 2 (black/white) | 65,536 colors |

## Benefits of TFT Migration

1. **Larger Display**: 240x320 vs 128x64 (15x more pixels!)
2. **Color Support**: 65K colors vs 2 colors
3. **Faster Updates**: SPI is much faster than I2C
4. **No Framebuffer**: Saves 1KB RAM (OLED) or 150KB RAM (TFT would need)
5. **Better Visibility**: IPS panel with wide viewing angles

## Challenges

1. **Memory**: Full color framebuffer would need 153,600 bytes (240×320×2)
   - **Solution**: Use direct rendering (no framebuffer), redraw only changed areas
2. **Font Scaling**: Larger display needs bigger fonts
   - **Solution**: Use Adafruit_GFX built-in fonts or custom fonts
3. **UI Redesign**: More screen space allows richer layouts
   - **Solution**: Keep existing layouts, optionally enhance later
4. **Color Scheme**: Need to design color palette
   - **Solution**: Start with white-on-black (same as OLED), add colors gradually

## Next Steps

1. **Choose Strategy**: Option 1 (dual-mode) or Option 2 (TFT-only)
2. **Update Display_HAL.h**: Already done ✓
3. **Create Display Wrapper**: If using Option 1
4. **Update OLED_Utils.cpp**: Initialization and update logic
5. **Global Search/Replace**: Color constants
6. **Test Build**: Verify compilation with ST7789
7. **Test Hardware**: Connect display and verify rendering
8. **Optimize**: Add color, improve layouts, leverage larger screen

## Estimated Effort

- **Option 1 (Dual-mode)**: ~4-6 hours
  - Create wrapper class: 1-2 hours
  - Update all color references: 1-2 hours
  - Test both modes: 1-2 hours

- **Option 2 (TFT-only)**: ~2-3 hours
  - Update initialization: 30 minutes
  - Global color constant replacement: 1 hour
  - Test and debug: 1-1.5 hours

## Conclusion

Your OLED modularization has made this migration **much easier** than it could have been. The display code is already well-organized into mode files, uses dimension macros, and has a clear separation of concerns. The main work is:

1. Abstracting the display object type
2. Replacing color constants
3. Handling the framebuffer vs direct-write difference

The rest of your codebase (sensors, networking, commands, etc.) is completely display-agnostic and requires **zero changes**.
