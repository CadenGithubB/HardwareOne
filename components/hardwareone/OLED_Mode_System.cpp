// OLED_Mode_System.cpp - System status, memory, and web stats display modes
// Extracted from OLED_Display.cpp for modularity

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "System_Settings.h"
#include "System_Utils.h"
#include <esp_heap_caps.h>

#if ENABLE_WIFI
#include <WiFi.h>
#endif

#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>
#endif

// External references
extern Adafruit_SSD1306* oledDisplay;
extern bool oledConnected;
extern OLEDMode currentOLEDMode;
extern Settings gSettings;
extern String customOLEDText;
extern String unavailableOLEDTitle;
extern String unavailableOLEDReason;

// popOLEDMode is declared in OLED_Display.h

// Unavailable page state - defined in OLED_Display.cpp
extern unsigned long unavailableOLEDStartTime;

// ============================================================================
// System Status Display
// ============================================================================

void displaySystemStatus() {
  if (!oledDisplay || !oledConnected) return;
  
  oledDisplay->println("=== SYSTEM STATUS ===");
  oledDisplay->println();

#if ENABLE_WIFI
  // WiFi Status
  if (WiFi.isConnected()) {
    oledDisplay->print("WiFi: ");
    oledDisplay->println(WiFi.SSID());
    oledDisplay->print("IP: ");
    oledDisplay->println(WiFi.localIP());
  } else {
    oledDisplay->println("WiFi: Disconnected");
  }
#else
  oledDisplay->println("WiFi: Disabled");
#endif

  // Memory
  oledDisplay->print("Heap: ");
  oledDisplay->print(ESP.getFreeHeap() / 1024);
  oledDisplay->println(" KB");

  // Uptime
  unsigned long uptimeSec = millis() / 1000;
  unsigned long hours = uptimeSec / 3600;
  unsigned long minutes = (uptimeSec % 3600) / 60;
  oledDisplay->print("Up: ");
  oledDisplay->print(hours);
  oledDisplay->print("h ");
  oledDisplay->print(minutes);
  oledDisplay->println("m");
}

// ============================================================================
// Memory Stats Display
// ============================================================================

void displayMemoryStats() {
  if (!oledDisplay || !oledConnected) return;
  
  oledDisplay->setTextSize(1);
  oledDisplay->println("=== MEMORY ===");
  oledDisplay->println();
  
  // Heap memory
  size_t freeHeap = ESP.getFreeHeap();
  size_t totalHeap = ESP.getHeapSize();
  size_t usedHeap = totalHeap - freeHeap;
  int heapPercent = (usedHeap * 100) / totalHeap;
  
  oledDisplay->print("Heap: ");
  oledDisplay->print(freeHeap / 1024);
  oledDisplay->print("/");
  oledDisplay->print(totalHeap / 1024);
  oledDisplay->println("KB");
  
  // Draw heap usage bar
  const int barX = 0;
  const int barY = 26;
  const int barWidth = 100;
  const int barHeight = 8;
  
  oledDisplay->drawRect(barX, barY, barWidth, barHeight, DISPLAY_COLOR_WHITE);
  int fillWidth = (barWidth - 2) * heapPercent / 100;
  if (fillWidth > 0) {
    oledDisplay->fillRect(barX + 1, barY + 1, fillWidth, barHeight - 2, DISPLAY_COLOR_WHITE);
  }
  oledDisplay->setCursor(barX + barWidth + 4, barY);
  oledDisplay->print(heapPercent);
  oledDisplay->print("%");
  
  // PSRAM if available
  if (psramFound()) {
    size_t freePsram = ESP.getFreePsram();
    size_t totalPsram = ESP.getPsramSize();
    oledDisplay->setCursor(0, 38);
    oledDisplay->print("PSRAM: ");
    oledDisplay->print(freePsram / 1024);
    oledDisplay->print("/");
    oledDisplay->print(totalPsram / 1024);
    oledDisplay->println("KB");
  }
  
  // Min free heap (watermark)
  oledDisplay->setCursor(0, 48);
  oledDisplay->print("Min: ");
  oledDisplay->print(ESP.getMinFreeHeap() / 1024);
  oledDisplay->println("KB");
}

// ============================================================================
// Web Stats Display
// ============================================================================

void displayWebStats() {
  if (!oledDisplay || !oledConnected) return;
  
  oledDisplay->setTextSize(1);
  oledDisplay->println("=== WEB STATS ===");
  oledDisplay->println();
  
#if ENABLE_HTTP_SERVER
  extern httpd_handle_t server;
  extern unsigned long gServerStartTime;
  extern int gTotalSessions;
  extern int gFailedLoginAttempts;
  
  if (server) {
    oledDisplay->println("HTTP: Running");
    
    // Uptime
    if (gServerStartTime > 0) {
      unsigned long uptimeSec = (millis() - gServerStartTime) / 1000;
      unsigned long hours = uptimeSec / 3600;
      unsigned long minutes = (uptimeSec % 3600) / 60;
      oledDisplay->print("Up: ");
      oledDisplay->print(hours);
      oledDisplay->print("h ");
      oledDisplay->print(minutes);
      oledDisplay->println("m");
    }
    
    // Session stats
    oledDisplay->print("Sessions: ");
    oledDisplay->println(gTotalSessions);
    
    oledDisplay->print("Failed: ");
    oledDisplay->println(gFailedLoginAttempts);
  } else {
    oledDisplay->println("HTTP: Stopped");
    oledDisplay->println();
    oledDisplay->println("Run: openhttp");
  }
#else
  oledDisplay->println("HTTP: Disabled");
  oledDisplay->println();
  oledDisplay->println("Compile with");
  oledDisplay->println("ENABLE_HTTP_SERVER=1");
#endif
}

// ============================================================================
// Custom Text Display
// ============================================================================

void displayCustomText() {
  if (!oledDisplay || !oledConnected) return;
  
  if (customOLEDText.length() == 0) {
    oledDisplay->println("No custom text set");
    oledDisplay->println();
    oledDisplay->println("Use:");
    oledDisplay->println("oledtext \"message\"");
    return;
  }

  oledDisplay->println(customOLEDText);
}

// ============================================================================
// Unavailable Page Display
// ============================================================================

void displayUnavailable() {
  if (!oledDisplay || !oledConnected) return;
  
  oledDisplay->setTextSize(1);
  oledDisplay->println("=== " + unavailableOLEDTitle + " ===");
  oledDisplay->println();

  if (unavailableOLEDReason.length() == 0) {
    oledDisplay->println("Not available");
  } else {
    int start = 0;
    while (start < (int)unavailableOLEDReason.length()) {
      int nl = unavailableOLEDReason.indexOf('\n', start);
      if (nl < 0) {
        oledDisplay->println(unavailableOLEDReason.substring(start));
        break;
      }
      oledDisplay->println(unavailableOLEDReason.substring(start, nl));
      start = nl + 1;
    }
  }

  oledDisplay->println();

  // Only show/perform auto-return when a timeout is active
  if (unavailableOLEDStartTime != 0) {
    oledDisplay->println("Returning...");

    const unsigned long UNAVAILABLE_TIMEOUT_MS = 5000;
    if (millis() - unavailableOLEDStartTime >= UNAVAILABLE_TIMEOUT_MS) {
      currentOLEDMode = popOLEDMode();
    }
  }
}

// ============================================================================
// System Input Handler (minimal - just B for back)
// ============================================================================

bool systemStatusInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  // These modes only need B button to go back, handled by main input handler
  return false;
}

// ============================================================================
// Memory Stats Rendered (two-phase rendering)
// ============================================================================

// Pre-gathered memory data to avoid heap operations inside I2C transaction
struct MemoryRenderData {
  size_t freeHeap;
  size_t totalHeap;
  size_t usedHeap;
  int heapPercent;
  size_t freePSRAM;
  size_t totalPSRAM;
  size_t usedPSRAM;
  int psramPercent;
  size_t minFreeHeap;
  size_t largestBlock;
  bool hasPSRAM;
  bool valid;
};
static MemoryRenderData memoryRenderData = {0};

// Gather memory data (called OUTSIDE I2C transaction to avoid blocking gamepad)
void prepareMemoryData() {
  // Get heap data OUTSIDE I2C transaction
  memoryRenderData.freeHeap = ESP.getFreeHeap();
  memoryRenderData.totalHeap = ESP.getHeapSize();
  memoryRenderData.usedHeap = memoryRenderData.totalHeap - memoryRenderData.freeHeap;
  memoryRenderData.heapPercent = (memoryRenderData.usedHeap * 100) / memoryRenderData.totalHeap;
  
  // Get PSRAM data
  memoryRenderData.freePSRAM = ESP.getFreePsram();
  memoryRenderData.totalPSRAM = ESP.getPsramSize();
  memoryRenderData.hasPSRAM = (memoryRenderData.totalPSRAM > 0);
  
  if (memoryRenderData.hasPSRAM) {
    memoryRenderData.usedPSRAM = memoryRenderData.totalPSRAM - memoryRenderData.freePSRAM;
    memoryRenderData.psramPercent = (memoryRenderData.usedPSRAM * 100) / memoryRenderData.totalPSRAM;
  }
  
  // Get additional stats
  memoryRenderData.minFreeHeap = ESP.getMinFreeHeap();
  memoryRenderData.largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  
  memoryRenderData.valid = true;
}

// Render memory stats from pre-gathered data (called INSIDE I2C transaction)
void displayMemoryStatsRendered() {
  if (!oledDisplay || !oledConnected) return;
  
  if (!memoryRenderData.valid) {
    oledDisplay->clearDisplay();
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(0, 0);
    oledDisplay->println("Memory Error");
    return;
  }
  
  // Clear only content area to prevent flickering
  oledDisplay->fillRect(0, 0, SCREEN_WIDTH, OLED_CONTENT_HEIGHT, DISPLAY_COLOR_BLACK);
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Header
  oledDisplay->setCursor(0, 0);
  oledDisplay->println("MEMORY");
  
  // Internal RAM (DRAM) - compact layout
  oledDisplay->setCursor(0, 9);
  oledDisplay->print("Heap: ");
  oledDisplay->print(memoryRenderData.freeHeap / 1024);
  oledDisplay->print("/");
  oledDisplay->print(memoryRenderData.totalHeap / 1024);
  oledDisplay->print("KB");
  
  // Draw heap usage bar (smaller)
  int barWidth = 90;
  int barHeight = 5;
  int barX = 10;
  int barY = 18;
  oledDisplay->drawRect(barX, barY, barWidth, barHeight, DISPLAY_COLOR_WHITE);
  int fillWidth = (memoryRenderData.heapPercent * (barWidth - 2)) / 100;
  if (fillWidth > 0) {
    oledDisplay->fillRect(barX + 1, barY + 1, fillWidth, barHeight - 2, DISPLAY_COLOR_WHITE);
  }
  oledDisplay->setCursor(barX + barWidth + 2, barY - 1);
  oledDisplay->print(memoryRenderData.heapPercent);
  oledDisplay->print("%");
  
  // PSRAM info (compact)
  if (memoryRenderData.hasPSRAM) {
    oledDisplay->setCursor(0, 25);
    oledDisplay->print("PSRAM: ");
    oledDisplay->print(memoryRenderData.freePSRAM / 1024);
    oledDisplay->print("/");
    oledDisplay->print(memoryRenderData.totalPSRAM / 1024);
    oledDisplay->print("KB");
    
    // Draw PSRAM usage bar
    int psramBarY = 33;
    oledDisplay->drawRect(barX, psramBarY, barWidth, barHeight, DISPLAY_COLOR_WHITE);
    int psramFillWidth = (memoryRenderData.psramPercent * (barWidth - 2)) / 100;
    if (psramFillWidth > 0) {
      oledDisplay->fillRect(barX + 1, psramBarY + 1, psramFillWidth, barHeight - 2, DISPLAY_COLOR_WHITE);
    }
    oledDisplay->setCursor(barX + barWidth + 2, psramBarY - 1);
    oledDisplay->print(memoryRenderData.psramPercent);
    oledDisplay->print("%");
    
    // Additional stats (compact, single line)
    oledDisplay->setCursor(0, 41);
    oledDisplay->print("Min:");
    oledDisplay->print(memoryRenderData.minFreeHeap / 1024);
    oledDisplay->print(" Max:");
    oledDisplay->print(memoryRenderData.largestBlock / 1024);
    oledDisplay->print("KB");
  } else {
    oledDisplay->setCursor(0, 25);
    oledDisplay->println("PSRAM: None");
    
    // Additional stats when no PSRAM (more space available)
    oledDisplay->setCursor(0, 34);
    oledDisplay->print("Min Free: ");
    oledDisplay->print(memoryRenderData.minFreeHeap / 1024);
    oledDisplay->println("KB");
    
    oledDisplay->setCursor(0, 43);
    oledDisplay->print("Max Block: ");
    oledDisplay->print(memoryRenderData.largestBlock / 1024);
    oledDisplay->print("KB");
  }
}

// ============================================================================
// System Status Rendered (two-phase rendering)
// ============================================================================

// External battery functions
extern float getBatteryVoltage();
extern float getBatteryPercentage();
extern char getBatteryIcon();

// Pre-gathered system status data to avoid WiFi/heap operations inside I2C transaction
struct SystemStatusRenderData {
  bool wifiConnected;
  char ssid[16];  // Truncated SSID
  char ip[16];     // IP address string
  uint32_t freeHeap;
  unsigned long uptimeHours;
  unsigned long uptimeMinutes;
  float batteryVoltage;
  float batteryPercentage;
  char batteryIcon;
  bool valid;
};
static SystemStatusRenderData systemStatusRenderData = {0};

// Gather system status data (called OUTSIDE I2C transaction to avoid blocking gamepad)
void prepareSystemStatusData() {
  // Get WiFi data OUTSIDE I2C transaction
  systemStatusRenderData.wifiConnected = WiFi.isConnected();
  
  if (systemStatusRenderData.wifiConnected) {
    String ssid = WiFi.SSID();
    if (ssid.length() > 15) ssid = ssid.substring(0, 15);
    strncpy(systemStatusRenderData.ssid, ssid.c_str(), 15);
    systemStatusRenderData.ssid[15] = '\0';
    
    String ip = WiFi.localIP().toString();
    strncpy(systemStatusRenderData.ip, ip.c_str(), 15);
    systemStatusRenderData.ip[15] = '\0';
  }
  
  // Get heap data OUTSIDE I2C transaction
  systemStatusRenderData.freeHeap = ESP.getFreeHeap();
  
  // Calculate uptime OUTSIDE I2C transaction
  unsigned long uptimeSec = millis() / 1000;
  systemStatusRenderData.uptimeHours = uptimeSec / 3600;
  systemStatusRenderData.uptimeMinutes = (uptimeSec % 3600) / 60;
  
  // Get battery data OUTSIDE I2C transaction
  systemStatusRenderData.batteryVoltage = getBatteryVoltage();
  systemStatusRenderData.batteryPercentage = getBatteryPercentage();
  systemStatusRenderData.batteryIcon = getBatteryIcon();
  
  systemStatusRenderData.valid = true;
}

// Render system status from pre-gathered data (called INSIDE I2C transaction)
void displaySystemStatusRendered() {
  if (!oledDisplay || !oledConnected) return;
  
  if (!systemStatusRenderData.valid) {
    oledDisplay->clearDisplay();
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(0, 0);
    oledDisplay->println("System Error");
    return;
  }
  
  oledDisplay->println("=== SYSTEM STATUS ===");
  oledDisplay->println();

  // Battery Status (top priority)
#if ENABLE_BATTERY_MONITOR
  oledDisplay->print("Batt: ");
  oledDisplay->print(systemStatusRenderData.batteryVoltage, 2);
  oledDisplay->print("V ");
  oledDisplay->print((int)systemStatusRenderData.batteryPercentage);
  oledDisplay->print("% ");
  oledDisplay->print(systemStatusRenderData.batteryIcon);
#else
  oledDisplay->print("Power: USB");
#endif
  oledDisplay->println();

  // WiFi Status
  if (systemStatusRenderData.wifiConnected) {
    oledDisplay->print("WiFi: ");
    oledDisplay->println(systemStatusRenderData.ssid);
  } else {
    oledDisplay->println("WiFi: Disconnected");
  }

  // Memory
  oledDisplay->print("Heap: ");
  oledDisplay->print(systemStatusRenderData.freeHeap / 1024);
  oledDisplay->println(" KB");

  // Uptime
  oledDisplay->print("Up: ");
  oledDisplay->print(systemStatusRenderData.uptimeHours);
  oledDisplay->print("h ");
  oledDisplay->print(systemStatusRenderData.uptimeMinutes);
  oledDisplay->println("m");
}

#endif // ENABLE_OLED_DISPLAY
