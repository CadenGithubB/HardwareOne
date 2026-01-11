// OLED_Mode_Sensors.cpp - Sensor overview and connected sensors display modes
// Extracted from OLED_Display.cpp for modularity

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "System_Settings.h"
#include "System_I2C.h"

// External references
extern Adafruit_SSD1306* oledDisplay;
extern bool oledConnected;
extern OLEDMode currentOLEDMode;
extern Settings gSettings;

// Sensor state (managed by I2C system)
extern bool imuConnected;
extern bool imuEnabled;
extern bool tofConnected;
extern bool tofEnabled;
extern bool thermalConnected;
extern bool thermalEnabled;
extern bool gpsConnected;
extern bool gpsEnabled;
extern bool gamepadConnected;
extern bool gamepadEnabled;
extern bool apdsConnected;

// Device registry
extern ConnectedDevice connectedDevices[];
extern int connectedDeviceCount;

// OLED display constants are macros defined in OLED_Display.h:
// SCREEN_WIDTH = 128, OLED_CONTENT_HEIGHT = SCREEN_HEIGHT - OLED_FOOTER_HEIGHT

// ============================================================================
// Connected Sensors Rendered (two-phase rendering)
// ============================================================================

// Pre-gathered connected sensors data to avoid array operations inside I2C transaction
struct ConnectedSensorsRenderData {
  int connectedCount;
  int totalHeight;
  int scrollOffset;
  bool isPaused;
  bool valid;
};
static ConnectedSensorsRenderData connectedSensorsRenderData = {0};

// Gather connected sensors data (called OUTSIDE I2C transaction to avoid blocking gamepad)
void prepareConnectedSensorsData() {
  // Count connected sensors OUTSIDE I2C transaction
  connectedSensorsRenderData.connectedCount = 0;
  for (int i = 0; i < connectedDeviceCount; i++) {
    if (connectedDevices[i].isConnected) connectedSensorsRenderData.connectedCount++;
  }
  
  // Calculate layout parameters OUTSIDE I2C transaction
  const int lineHeight = 8;
  const int sensorSpacing = 3;
  const int headerLines = 2;
  const int linesPerSensor = 2;
  int totalLines = headerLines + (connectedSensorsRenderData.connectedCount * linesPerSensor);
  connectedSensorsRenderData.totalHeight = (totalLines * lineHeight) + (connectedSensorsRenderData.connectedCount * sensorSpacing);
  
  // Smooth scrolling - scroll once through all sensors, then hold at end (no loop restart)
  static unsigned long lastScrollTime = 0;
  static bool scrollComplete = false;
  const int scrollSpeed = 40;  // Faster scroll for crisp feel
  const int maxScroll = max(0, connectedSensorsRenderData.totalHeight - OLED_CONTENT_HEIGHT);
  
  unsigned long now = millis();
  if (maxScroll > 0 && !scrollComplete) {
    if (now - lastScrollTime >= scrollSpeed) {
      if (connectedSensorsRenderData.scrollOffset < maxScroll) {
        connectedSensorsRenderData.scrollOffset++;
      } else {
        scrollComplete = true;  // Hold at end, don't restart
      }
      lastScrollTime = now;
    }
  }
  
  // Reset scroll state when switching away from this mode
  static OLEDMode lastMode = OLED_OFF;
  if (currentOLEDMode != lastMode) {
    connectedSensorsRenderData.scrollOffset = 0;
    scrollComplete = false;
    lastScrollTime = now;
    lastMode = currentOLEDMode;
  }
  
  connectedSensorsRenderData.isPaused = false;
  connectedSensorsRenderData.valid = true;
}

// Render connected sensors from pre-gathered data (called INSIDE I2C transaction)
void displayConnectedSensorsRendered() {
  if (!oledDisplay || !oledConnected) return;
  
  if (!connectedSensorsRenderData.valid) {
    oledDisplay->clearDisplay();
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(0, 0);
    oledDisplay->println("Sensors Error");
    return;
  }
  
  // Clear only content area to prevent flickering
  oledDisplay->fillRect(0, 0, SCREEN_WIDTH, OLED_CONTENT_HEIGHT, DISPLAY_COLOR_BLACK);
  
  // Render content with scroll offset
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  int yPos = -connectedSensorsRenderData.scrollOffset;

  // Header with sensor count
  if (yPos >= 0 && yPos < OLED_CONTENT_HEIGHT) {
    oledDisplay->setCursor(4, yPos);  // Indent for scrollbar
    oledDisplay->print("Sensors (");
    oledDisplay->print(connectedSensorsRenderData.connectedCount);
    oledDisplay->print("):");
  }
  yPos += 8;
  yPos += 8;  // Extra spacing
  
  // Draw scrollbar if needed (constrained to content area)
  if (connectedSensorsRenderData.totalHeight > OLED_CONTENT_HEIGHT) {
    const int barX = 1;
    const int barWidth = 2;
    const int barTop = 0;
    const int barBottom = OLED_CONTENT_HEIGHT - 1;
    
    // Draw track
    for (int y = barTop; y <= barBottom; y += 4) {
      oledDisplay->drawPixel(barX, y, DISPLAY_COLOR_WHITE);
    }
    
    // Draw thumb
    int maxScroll = max(0, connectedSensorsRenderData.totalHeight - OLED_CONTENT_HEIGHT);
    if (maxScroll > 0) {
      int thumbSize = max(6, (OLED_CONTENT_HEIGHT * (OLED_CONTENT_HEIGHT)) / connectedSensorsRenderData.totalHeight);
      int maxThumbY = barBottom - thumbSize + 1;
      int thumbY = barTop + (connectedSensorsRenderData.scrollOffset * maxThumbY) / maxScroll;
      thumbY = max(barTop, min(thumbY, maxThumbY));
      
      for (int y = thumbY; y < thumbY + thumbSize && y <= barBottom; y++) {
        oledDisplay->drawPixel(barX, y, DISPLAY_COLOR_WHITE);
      }
    }
  }
  
  // Draw sensors (only if visible in content area)
  int sensorIndex = 0;
  for (int i = 0; i < connectedDeviceCount && sensorIndex < connectedSensorsRenderData.connectedCount; i++) {
    if (connectedDevices[i].isConnected) {
      // Only draw if within content area
      if (yPos >= -8 && yPos < OLED_CONTENT_HEIGHT) {
        oledDisplay->setCursor(4, yPos);
        oledDisplay->print(connectedDevices[i].name);
      }
      yPos += 8;
      
      if (yPos >= -8 && yPos < OLED_CONTENT_HEIGHT) {
        oledDisplay->setCursor(8, yPos);
        oledDisplay->print("0x");
        oledDisplay->print(connectedDevices[i].address, HEX);
      }
      yPos += 8 + 3;  // Add spacing
      sensorIndex++;
    }
  }
}

// ============================================================================
// Sensor Data Overview Display
// ============================================================================

void displaySensorData() {
  if (!oledDisplay || !oledConnected) return;
  
  // Sensors Overview - shows status of all sensors (compact to fit in content area)
  oledDisplay->println("SENSORS");
  
  int activeCount = 0;
  int totalCount = 0;
  
  // Thermal sensor status
#if ENABLE_THERMAL_SENSOR
  totalCount++;
  oledDisplay->print("Thermal: ");
  if (thermalConnected && thermalEnabled) {
    oledDisplay->println("ON");
    activeCount++;
  } else {
    oledDisplay->println("off");
  }
#endif

  // ToF sensor status
#if ENABLE_TOF_SENSOR
  totalCount++;
  oledDisplay->print("ToF:     ");
  if (tofConnected && tofEnabled) {
    oledDisplay->println("ON");
    activeCount++;
  } else {
    oledDisplay->println("off");
  }
#endif

  // IMU sensor status
#if ENABLE_IMU_SENSOR
  totalCount++;
  oledDisplay->print("IMU:     ");
  if (imuConnected && imuEnabled) {
    oledDisplay->println("ON");
    activeCount++;
  } else {
    oledDisplay->println("off");
  }
#endif

  // GPS sensor status
#if ENABLE_GPS_SENSOR
  totalCount++;
  oledDisplay->print("GPS:     ");
  if (gpsConnected && gpsEnabled) {
    oledDisplay->println("ON");
    activeCount++;
  } else {
    oledDisplay->println("off");
  }
#endif

  // APDS sensor status
#if ENABLE_APDS_SENSOR
  {
    extern bool apdsColorEnabled;
    totalCount++;
    oledDisplay->print("APDS:    ");
    if (apdsColorEnabled) {
      oledDisplay->println("ON");
      activeCount++;
    } else {
      oledDisplay->println("off");
    }
  }
#endif

  // Gamepad status
#if ENABLE_GAMEPAD_SENSOR
  totalCount++;
  oledDisplay->print("Gamepad: ");
  if (gamepadConnected && gamepadEnabled) {
    oledDisplay->println("ON");
    activeCount++;
  } else {
    oledDisplay->println("off");
  }
#endif

  // Summary line at bottom (compact - no blank line to save space)
  oledDisplay->print(activeCount);
  oledDisplay->print("/");
  oledDisplay->print(totalCount);
  oledDisplay->println(" active");
}

// ============================================================================
// Connected Sensors Display (scrollable list)
// ============================================================================

void displayConnectedSensors() {
  if (!oledDisplay || !oledConnected) return;
  
  static int scrollOffset = 0;  // Vertical scroll position in pixels
  static unsigned long lastScrollTime = 0;
  const int scrollSpeed = 50;  // Milliseconds between scroll steps
  
  oledDisplay->setTextSize(1);
  oledDisplay->println("CONNECTED DEVICES");
  oledDisplay->drawFastHLine(0, 10, SCREEN_WIDTH, DISPLAY_COLOR_WHITE);
  
  // Count connected devices
  int connectedCount = 0;
  for (int i = 0; i < connectedDeviceCount; i++) {
    if (connectedDevices[i].isConnected) {
      connectedCount++;
    }
  }
  
  if (connectedCount == 0) {
    oledDisplay->setCursor(0, 20);
    oledDisplay->println("No devices detected");
    return;
  }
  
  // Calculate content height (each device = 10 pixels)
  const int itemHeight = 10;
  const int contentHeight = connectedCount * itemHeight;
  const int viewportHeight = OLED_CONTENT_HEIGHT - 12;  // After header
  
  // Auto-scroll if content exceeds viewport
  if (contentHeight > viewportHeight) {
    unsigned long now = millis();
    if (now - lastScrollTime >= scrollSpeed) {
      scrollOffset++;
      if (scrollOffset >= contentHeight - viewportHeight + itemHeight) {
        scrollOffset = 0;  // Wrap around
      }
      lastScrollTime = now;
    }
  } else {
    scrollOffset = 0;
  }
  
  // Draw devices
  int yPos = 12 - scrollOffset;
  for (int i = 0; i < connectedDeviceCount; i++) {
    if (!connectedDevices[i].isConnected) continue;
    
    // Only draw if visible
    if (yPos >= 10 && yPos < OLED_CONTENT_HEIGHT) {
      oledDisplay->setCursor(0, yPos);
      oledDisplay->print(connectedDevices[i].name);
      oledDisplay->print(" 0x");
      if (connectedDevices[i].address < 0x10) oledDisplay->print("0");
      oledDisplay->print(connectedDevices[i].address, HEX);
    }
    yPos += itemHeight;
  }
  
  // Draw scroll indicator if scrollable
  if (contentHeight > viewportHeight) {
    int scrollbarHeight = viewportHeight;
    int thumbHeight = max(4, (viewportHeight * viewportHeight) / contentHeight);
    int thumbY = 12 + (scrollOffset * (scrollbarHeight - thumbHeight)) / (contentHeight - viewportHeight);
    
    oledDisplay->drawFastVLine(SCREEN_WIDTH - 2, 12, scrollbarHeight, DISPLAY_COLOR_WHITE);
    oledDisplay->fillRect(SCREEN_WIDTH - 3, thumbY, 3, thumbHeight, DISPLAY_COLOR_WHITE);
  }
}

// ============================================================================
// Sensor Input Handler (minimal - B for back)
// ============================================================================

bool sensorDataInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  // These modes only need B button to go back, handled by main input handler
  return false;
}

#endif // ENABLE_OLED_DISPLAY
