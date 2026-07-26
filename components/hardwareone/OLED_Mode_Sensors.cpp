// OLED_Mode_Sensors.cpp - Sensor overview and connected sensors display modes
// Extracted from OLED_Display.cpp for modularity

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "System_Settings.h"
#include "System_I2C.h"

#if ENABLE_RTC_SENSOR
#include "i2csensor_ds3231.h"
#endif

#if ENABLE_PRESENCE_SENSOR
#include "i2csensor_sths34pf80.h"
#endif

#if ENABLE_IMU_SENSOR
#include "i2csensor_bno055.h"     // gImuRunning / gImuConnected
#endif
#if ENABLE_TOF_SENSOR
#include "i2csensor_vl53l4cx.h"   // gTofRunning / gTofConnected
#endif
#if ENABLE_THERMAL_SENSOR
#include "i2csensor_mlx90640.h"   // gThermalRunning / gThermalConnected
#endif
#if ENABLE_GPS_SENSOR
#include "i2csensor_pa1010d.h"    // gGpsRunning / gGpsConnected
#endif
#if ENABLE_GAMEPAD_SENSOR
#include "i2csensor_seesaw.h"     // gInputRunning / gInputConnected
#endif
#if ENABLE_APDS_SENSOR
#include "i2csensor_apds9960.h"   // gApdsConnected
#endif

// External references
// (Sensor enabled/connected flags are provided by the per-sensor headers above.)

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
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
    oledDisplay->println("Sensors Error");
    return;
  }
  
  // Render content with scroll offset (starts after header)
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  int yPos = OLED_CONTENT_START_Y - connectedSensorsRenderData.scrollOffset;

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
  
  // Sensors Overview - shows status of all sensors (content starts after header)
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  
  int activeCount = 0;
  int totalCount = 0;
  
  // Thermal sensor status
#if ENABLE_THERMAL_SENSOR
  totalCount++;
  oledDisplay->print("Thermal: ");
  if (gThermalConnected && gThermalRunning) {
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
  if (gTofConnected && gTofRunning) {
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
  if (gImuConnected && gImuRunning) {
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
  if (gGpsConnected && gGpsRunning) {
    oledDisplay->println("ON");
    activeCount++;
  } else {
    oledDisplay->println("off");
  }
#endif

  // APDS sensor status
#if ENABLE_APDS_SENSOR
  {
    extern bool gApdsColorRunning;
    totalCount++;
    oledDisplay->print("APDS:    ");
    if (gApdsColorRunning) {
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
  if (gInputConnected && gInputRunning) {
    oledDisplay->println("ON");
    activeCount++;
  } else {
    oledDisplay->println("off");
  }
#endif

  // RTC sensor status
#if ENABLE_RTC_SENSOR
  totalCount++;
  oledDisplay->print("RTC:     ");
  if (gRtcConnected && gRtcRunning) {
    oledDisplay->println("ON");
    activeCount++;
  } else {
    oledDisplay->println("off");
  }
#endif

  // Presence sensor status
#if ENABLE_PRESENCE_SENSOR
  totalCount++;
  oledDisplay->print("Presence:");
  if (gPresenceConnected && gPresenceRunning) {
    oledDisplay->println(" ON");
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

// ============================================================================
// I2C Bus Scan — hardware diagnostics (Hardware menu, under Sensors)
// ============================================================================
// Scan-on-demand: A runs the scan, results list the found addresses + their
// identified names, per bus. Reuses the same probe primitives as the CLI
// `i2cscan` (i2cPingAddress + i2cConfirmPresent + identifySensor) — no logic
// duplicated. The scan is a blocking bus sweep (~a few hundred ms), so it runs
// in the pre-render prepare hook (outside the display I2C transaction) after a
// one-frame "Scanning..." paint, never in the render or the input task.

#if ENABLE_I2C_SYSTEM

#define I2C_DIAG_MAX 20
struct I2cDiagEntry { uint8_t bus; uint8_t addr; char name[18]; };
EXT_RAM_BSS_ATTR static I2cDiagEntry sI2cDiagResults[I2C_DIAG_MAX];
static int     sI2cDiagCount  = 0;
static int     sI2cDiagScroll = 0;
// 0=idle (prompt), 1=requested (paint "Scanning" next frame), 2=running
// (scan this prepare), 3=done (results). The two-step 1→2 gives the
// "Scanning..." frame time to paint before the blocking sweep.
static uint8_t sI2cDiagPhase  = 0;

static void i2cDiagRunScan() {
  sI2cDiagCount = 0;
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (!mgr) return;
  for (uint8_t bus = 0; bus < 2; bus++) {
    if (!mgr->isBusInitialized(bus)) continue;
    for (uint8_t addr = 1; addr < 127 && sI2cDiagCount < I2C_DIAG_MAX; addr++) {
      if (i2cPingAddress(addr, 100000, 50, bus) &&
          i2cConfirmPresent(addr, 100000, 50, bus)) {
        I2cDiagEntry& e = sI2cDiagResults[sI2cDiagCount++];
        e.bus  = bus;
        e.addr = addr;
        String id = identifySensor(addr);
        strncpy(e.name, id.c_str(), sizeof(e.name) - 1);
        e.name[sizeof(e.name) - 1] = '\0';
      }
    }
  }
}

// Pre-render hook (registered in the updateOLEDDisplay switch). Advances the
// scan phase; the actual sweep runs here, off the render transaction.
void prepareI2cDiagData() {
  if (sI2cDiagPhase == 1) { sI2cDiagPhase = 2; return; }  // let "Scanning" paint
  if (sI2cDiagPhase == 2) {
    i2cDiagRunScan();
    sI2cDiagScroll = 0;
    sI2cDiagPhase  = 3;
  }
}

static void displayI2cDiag() {
  if (!oledDisplay || !oledConnected) return;
  int y = OLED_CONTENT_START_Y;
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);

  if (sI2cDiagPhase == 0) {
    oledDisplay->setCursor(0, y);
    oledDisplay->println("I2C bus scan");
    oledDisplay->setCursor(0, y + 14);
    oledDisplay->print("A: Scan");
    return;
  }
  if (sI2cDiagPhase == 1 || sI2cDiagPhase == 2) {
    oledDisplay->setCursor(0, y);
    oledDisplay->print("Scanning...");
    oledMarkDirty();  // keep rendering until prepare advances to results
    return;
  }

  // Results.
  oledDisplay->setCursor(0, y);
  oledDisplay->printf("Found %d   A:rescan", sI2cDiagCount);
  y += 11;
  if (sI2cDiagCount == 0) {
    oledDisplay->setCursor(0, y);
    oledDisplay->print("No devices found");
    return;
  }
  const int rowH = 9;
  const int visible = 4;
  for (int i = 0; i < visible; i++) {
    int idx = sI2cDiagScroll + i;
    if (idx >= sI2cDiagCount) break;
    const I2cDiagEntry& e = sI2cDiagResults[idx];
    oledDisplay->setCursor(0, y);
    oledDisplay->printf("0x%02X b%u %.11s", e.addr, (unsigned)e.bus, e.name);
    y += rowH;
  }
  if (sI2cDiagScroll + visible < sI2cDiagCount) {
    oledDisplay->setCursor(122, OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - 8);
    oledDisplay->print("v");
  }
}

static bool i2cDiagInputHandler(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    sI2cDiagPhase = 1;   // request a scan (paints "Scanning" next frame)
    oledMarkDirty();
    return true;
  }
  if (sI2cDiagPhase == 3) {
    if (gNavEvents.down && sI2cDiagScroll + 4 < sI2cDiagCount) { sI2cDiagScroll++; return true; }
    if (gNavEvents.up   && sI2cDiagScroll > 0)                 { sI2cDiagScroll--; return true; }
  }
  return false;  // B falls through to the central back handler
}

// Reset to the prompt on forward entry so re-opening the screen doesn't show
// a stale result set from a previous visit.
static void i2cDiagOnEnter(bool isForward) {
  if (isForward) { sI2cDiagPhase = 0; sI2cDiagCount = 0; sI2cDiagScroll = 0; }
}

#endif  // ENABLE_I2C_SYSTEM

// ============================================================================
// Mode Registration
// ============================================================================

static const OLEDModeEntry sSensorModes[] = {
  { OLED_SENSOR_DATA,  "Sensor Data", "notify_sensor", displaySensorData,              nullptr, sensorDataInputHandler, false, -1, "B:Back" },
  { OLED_SENSOR_LIST,  "Sensor List", "notify_sensor", displayConnectedSensorsRendered, nullptr, nullptr,               false, -1, "B:Back" },
  { OLED_BOOT_SENSORS, "Boot",        "notify_sensor", displayConnectedSensorsRendered, nullptr, nullptr,               false, -1, "B:Back" },
#if ENABLE_I2C_SYSTEM
  { OLED_I2C_DIAG,     "I2C Scan",    "notify_sensor", displayI2cDiag,                  nullptr, i2cDiagInputHandler,   false, -1, "A:Scan B:Back", i2cDiagOnEnter },
#endif
};

REGISTER_OLED_MODE_MODULE(sSensorModes, sizeof(sSensorModes) / sizeof(sSensorModes[0]), "Sensors");

#endif // ENABLE_OLED_DISPLAY
