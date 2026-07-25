// OLED_Mode_System.cpp - System status, memory, and web stats display modes
// Extracted from OLED_Display.cpp for modularity

#include "WebServer_Handle.h"
#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "System_Settings.h"
#include "System_Utils.h"
#include <esp_heap_caps.h>

#if ENABLE_WIFI
#include <WiFi.h>
#include "System_WiFi.h"  // wifiRadioOn() — RADIO power axis
#endif

#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>
#endif

// External references
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
  
  // Header is rendered by the system - content starts at OLED_CONTENT_START_Y
  int y = OLED_CONTENT_START_Y;
  oledDisplay->setTextSize(1);
  
  // Show the feature name as a label
  oledDisplay->setCursor(0, y);
  oledDisplay->println(unavailableOLEDTitle);
  y += 10;

  if (unavailableOLEDReason.length() == 0) {
    oledDisplay->setCursor(0, y);
    oledDisplay->println("Not available");
  } else {
    int start = 0;
    while (start < (int)unavailableOLEDReason.length()) {
      oledDisplay->setCursor(0, y);
      int nl = unavailableOLEDReason.indexOf('\n', start);
      if (nl < 0) {
        oledDisplay->println(unavailableOLEDReason.substring(start));
        y += 10;
        break;
      }
      oledDisplay->println(unavailableOLEDReason.substring(start, nl));
      y += 10;
      start = nl + 1;
    }
  }

  // Only show/perform auto-return when a timeout is active
  if (unavailableOLEDStartTime != 0) {
    oledDisplay->setCursor(0, y + 2);
    oledDisplay->println("Returning...");

    const unsigned long UNAVAILABLE_TIMEOUT_MS = 5000;
    if (millis() - unavailableOLEDStartTime >= UNAVAILABLE_TIMEOUT_MS) {
      requestOLEDMode(popOLEDMode(), "unavail.timeout.autoreturn", false, /*isBackNav=*/true);
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
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
    oledDisplay->println("Memory Error");
    return;
  }
  
  // Header is rendered by the system - content starts at OLED_CONTENT_START_Y
  int y = OLED_CONTENT_START_Y;
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  const int barX = 0;
  const int barWidth = SCREEN_WIDTH - 22;  // Leave room for percentage text
  const int barHeight = 6;
  
  // --- Heap (DRAM) ---
  oledDisplay->setCursor(0, y);
  oledDisplay->print("Heap ");
  oledDisplay->print(memoryRenderData.freeHeap / 1024);
  oledDisplay->print("/");
  oledDisplay->print(memoryRenderData.totalHeap / 1024);
  oledDisplay->print("KB");
  y += 9;
  
  // Heap bar
  oledDisplay->drawRect(barX, y, barWidth, barHeight, DISPLAY_COLOR_WHITE);
  int fillWidth = (memoryRenderData.heapPercent * (barWidth - 2)) / 100;
  if (fillWidth > 0) {
    oledDisplay->fillRect(barX + 1, y + 1, fillWidth, barHeight - 2, DISPLAY_COLOR_WHITE);
  }
  oledDisplay->setCursor(barWidth + 3, y - 1);
  oledDisplay->print(memoryRenderData.heapPercent);
  oledDisplay->print("%");
  y += barHeight + 3;
  
  // --- PSRAM ---
  if (memoryRenderData.hasPSRAM) {
    oledDisplay->setCursor(0, y);
    oledDisplay->print("PSRAM ");
    oledDisplay->print(memoryRenderData.freePSRAM / 1024);
    oledDisplay->print("/");
    oledDisplay->print(memoryRenderData.totalPSRAM / 1024);
    oledDisplay->print("KB");
    y += 9;
    
    // PSRAM bar
    oledDisplay->drawRect(barX, y, barWidth, barHeight, DISPLAY_COLOR_WHITE);
    int psramFillWidth = (memoryRenderData.psramPercent * (barWidth - 2)) / 100;
    if (psramFillWidth > 0) {
      oledDisplay->fillRect(barX + 1, y + 1, psramFillWidth, barHeight - 2, DISPLAY_COLOR_WHITE);
    }
    oledDisplay->setCursor(barWidth + 3, y - 1);
    oledDisplay->print(memoryRenderData.psramPercent);
    oledDisplay->print("%");
  } else {
    oledDisplay->setCursor(0, y);
    oledDisplay->print("PSRAM: None");
  }
}

// ============================================================================
// Live Perf Stats (two-phase rendering, self-refreshing)
// ============================================================================
// Top-N task CPU% + stack low-watermarks on 128x64, backed directly by
// uxTaskGetSystemState (the same raw API taskstats/perftop use — there is no
// text parsing). CPU% is a NON-BLOCKING interval delta against the previous
// 500 ms sample (reportAllTaskStacks' RunSample pattern) — never cmd_perftop's
// vTaskDelay(750) two-snapshot form, which would park the main loop. The
// baseline is deliberately mode-LOCAL: sharing reportAllTaskStacks' 60 s
// baseline would corrupt its dCPU column (baselines are per-consumer).

#include "System_TaskUtils.h"
#include "System_MemUtil.h"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR (perf render data lives in PSRAM)

// Full task lists (not top-4): both pages scroll through every task with
// up/down (the I2C-diag viewport-scroll pattern). ~20 tasks run on this
// firmware, so 24 slots cover it; anything beyond is dropped (never seen).
#define PERF_MAX_TASKS 24
#define PERF_VISIBLE_ROWS 4
// Live-refresh cadence: re-sample task stats twice a second (2 Hz). Both the
// CPU% interval delta and the on-screen numbers advance at this rate.
#define PERF_SAMPLE_MS 500

struct PerfRenderData {
  // loop-health strip (from perfGetLoopSnapshot)
  bool     loopValid;
  uint32_t lapsPerSec, avgMs, maxMs, stalls5s, totalStalls;
  // all tasks by interval CPU%, sorted descending (IDLE excluded)
  int      cpuCount;
  char     cpuName[PERF_MAX_TASKS][11];
  uint8_t  cpuPct[PERF_MAX_TASKS];
  // all tasks by stack min-free, sorted ascending (riskiest first; BYTES)
  int      stkCount;
  char     stkName[PERF_MAX_TASKS][11];
  uint32_t stkFree[PERF_MAX_TASKS];
  bool     valid;
};
// ~700 B — PSRAM, written and read on the main loop only (prepare → render).
EXT_RAM_BSS_ATTR static PerfRenderData perfRenderData;
static uint8_t sPerfPage  = 0;   // 0 = CPU, 1 = STACK
static int     sCpuScroll = 0;   // viewport offsets, one per page
static int     sStkScroll = 0;

// Grow-only sample buffers (cmd_taskstats' ps_alloc cache pattern).
static TaskStatus_t* sPerfTaskBuf = nullptr;
static UBaseType_t   sPerfTaskCap = 0;
// Previous-sample runtime baseline, matched by handle.
static TaskHandle_t* sPerfPrevHandle = nullptr;
static uint32_t*     sPerfPrevRun    = nullptr;
static UBaseType_t   sPerfPrevCount  = 0;
static uint32_t      sPerfPrevTotal  = 0;

void preparePerfData() {
  // Expensive sampling at 2 Hz; between samples the cached data re-renders.
  static uint32_t sSampleStamp = 0;
  if (!everyMs(&sSampleStamp, PERF_SAMPLE_MS)) return;

  perfRenderData.loopValid = perfGetLoopSnapshot(
      perfRenderData.lapsPerSec, perfRenderData.avgMs, perfRenderData.maxMs,
      perfRenderData.stalls5s, perfRenderData.totalStalls);

  UBaseType_t n = uxTaskGetNumberOfTasks();
  if (n + 4 > sPerfTaskCap) {
    UBaseType_t cap = n + 4;
    TaskStatus_t* buf = (TaskStatus_t*)ps_alloc(cap * sizeof(TaskStatus_t),
                                                AllocPref::PreferPSRAM, "oled.perf.tasks");
    TaskHandle_t* ph  = (TaskHandle_t*)ps_alloc(cap * sizeof(TaskHandle_t),
                                                AllocPref::PreferPSRAM, "oled.perf.prevh");
    uint32_t*     pr  = (uint32_t*)ps_alloc(cap * sizeof(uint32_t),
                                            AllocPref::PreferPSRAM, "oled.perf.prevr");
    if (!buf || !ph || !pr) return;  // keep old buffers/data; retry next tick
    // Grow-only: old buffers stay allocated on the rare regrow (task count is
    // near-constant after boot); tracking-tagged so the leak-hunt sees them.
    sPerfTaskBuf = buf; sPerfPrevHandle = ph; sPerfPrevRun = pr;
    sPerfTaskCap = cap;
    sPerfPrevCount = 0;  // baseline invalid at the new size — skip one interval
  }

  uint32_t totalRun = 0;
  UBaseType_t got = uxTaskGetSystemState(sPerfTaskBuf, sPerfTaskCap, &totalRun);
  if (got == 0) return;  // capacity race — sample again next second

  // --- CPU page: interval delta vs previous sample, FULL sorted list ------
  // Prepare and render both run sequentially on the main loop, so inserting
  // straight into the render arrays is race-free.
  uint32_t dTotal = totalRun - sPerfPrevTotal;  // unsigned wrap-safe (u32 us counters)
  perfRenderData.cpuCount = 0;
  if (sPerfPrevCount > 0 && dTotal > 0) {
    for (UBaseType_t i = 0; i < got; i++) {
      const char* nm = sPerfTaskBuf[i].pcTaskName;
      if (strncmp(nm, "IDLE", 4) == 0) continue;  // idle time isn't load
      // Find this handle in the previous sample (order can shift).
      uint32_t prev = 0; bool found = false;
      for (UBaseType_t j = 0; j < sPerfPrevCount; j++) {
        if (sPerfPrevHandle[j] == sPerfTaskBuf[i].xHandle) { prev = sPerfPrevRun[j]; found = true; break; }
      }
      if (!found) continue;  // new task this interval — no baseline yet
      uint32_t dRun = sPerfTaskBuf[i].ulRunTimeCounter - prev;
      uint32_t p = (uint32_t)(((uint64_t)dRun * 100U) / dTotal);
      if (p > 125) continue;  // handle reuse artifact (taskstats' rejection rule)
      if (p > 100) p = 100;
      // Sorted insert (descending pct), capped at PERF_MAX_TASKS.
      int cnt = perfRenderData.cpuCount;
      int at = cnt < PERF_MAX_TASKS ? cnt : PERF_MAX_TASKS;
      while (at > 0 && perfRenderData.cpuPct[at - 1] < p) at--;
      if (at >= PERF_MAX_TASKS) continue;
      int last = cnt < PERF_MAX_TASKS ? cnt : PERF_MAX_TASKS - 1;
      for (int m = last; m > at; m--) {
        perfRenderData.cpuPct[m] = perfRenderData.cpuPct[m - 1];
        memcpy(perfRenderData.cpuName[m], perfRenderData.cpuName[m - 1],
               sizeof(perfRenderData.cpuName[m]));
      }
      strncpy(perfRenderData.cpuName[at], nm, 10);
      perfRenderData.cpuName[at][10] = '\0';
      perfRenderData.cpuPct[at] = (uint8_t)p;
      if (cnt < PERF_MAX_TASKS) perfRenderData.cpuCount++;
    }
  }

  // --- STACK page: FULL list, lowest free-bytes first (HWM is BYTES) ------
  perfRenderData.stkCount = 0;
  for (UBaseType_t i = 0; i < got; i++) {
    uint32_t f = (uint32_t)sPerfTaskBuf[i].usStackHighWaterMark;
    int cnt = perfRenderData.stkCount;
    int at = cnt < PERF_MAX_TASKS ? cnt : PERF_MAX_TASKS;
    while (at > 0 && perfRenderData.stkFree[at - 1] > f) at--;
    if (at >= PERF_MAX_TASKS) continue;
    int last = cnt < PERF_MAX_TASKS ? cnt : PERF_MAX_TASKS - 1;
    for (int m = last; m > at; m--) {
      perfRenderData.stkFree[m] = perfRenderData.stkFree[m - 1];
      memcpy(perfRenderData.stkName[m], perfRenderData.stkName[m - 1],
             sizeof(perfRenderData.stkName[m]));
    }
    strncpy(perfRenderData.stkName[at], sPerfTaskBuf[i].pcTaskName, 10);
    perfRenderData.stkName[at][10] = '\0';
    perfRenderData.stkFree[at] = f;
    if (cnt < PERF_MAX_TASKS) perfRenderData.stkCount++;
  }

  // Roll the baseline.
  for (UBaseType_t i = 0; i < got; i++) {
    sPerfPrevHandle[i] = sPerfTaskBuf[i].xHandle;
    sPerfPrevRun[i]    = sPerfTaskBuf[i].ulRunTimeCounter;
  }
  sPerfPrevCount = got;
  sPerfPrevTotal = totalRun;
  perfRenderData.valid = true;
}

static void displayPerfStatsRendered() {
  if (!oledDisplay || !oledConnected) return;
  int y = OLED_CONTENT_START_Y;
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);

  if (!perfRenderData.valid) {
    oledDisplay->setCursor(0, y);
    oledDisplay->println("Sampling...");
    oledMarkDirtyUntil(millis() + PERF_SAMPLE_MS);  // keep ticking until data lands
    return;
  }

  if (sPerfPage == 0) {
    // Row 1: loop-health strip — laps/sec through the main loop, average
    // lap time, and stalls in the last completed 5 s window (a floating
    // value that rises and falls, not the ever-climbing since-boot total —
    // matches the other two live metrics on this line).
    oledDisplay->setCursor(0, y);
    if (perfRenderData.loopValid) {
      oledDisplay->printf("Loop %lu/s %lums st%lu",
                          (unsigned long)perfRenderData.lapsPerSec,
                          (unsigned long)perfRenderData.avgMs,
                          (unsigned long)perfRenderData.stalls5s);
    } else {
      oledDisplay->print("Loop: warming up");
    }
    y += 9;

    if (perfRenderData.cpuCount == 0) {
      // CPU% is a delta between two 500 ms samples — the first visit needs
      // ~1 s before rows appear. The final dirty-hold keeps us ticking.
      oledDisplay->setCursor(0, y);
      oledDisplay->print("Measuring CPU...");
    } else {
      // Scrollable task viewport: name + bar + pct, sorted by CPU%.
      if (sCpuScroll > perfRenderData.cpuCount - PERF_VISIBLE_ROWS) {
        sCpuScroll = perfRenderData.cpuCount - PERF_VISIBLE_ROWS;
      }
      if (sCpuScroll < 0) sCpuScroll = 0;
      const int barX = 50, barW = 46, barH = 6;
      for (int row = 0; row < PERF_VISIBLE_ROWS; row++) {
        int r = sCpuScroll + row;
        if (r >= perfRenderData.cpuCount) break;
        char nm[9];
        strncpy(nm, perfRenderData.cpuName[r], 8); nm[8] = '\0';
        oledDisplay->setCursor(0, y);
        oledDisplay->print(nm);
        oledDisplay->drawRect(barX, y, barW, barH, DISPLAY_COLOR_WHITE);
        int fill = (perfRenderData.cpuPct[r] * (barW - 2)) / 100;
        if (fill > 0) oledDisplay->fillRect(barX + 1, y + 1, fill, barH - 2, DISPLAY_COLOR_WHITE);
        oledDisplay->setCursor(barX + barW + 2, y);
        oledDisplay->printf("%u%%", (unsigned)perfRenderData.cpuPct[r]);
        y += 9;
      }
      if (sCpuScroll > 0) {
        oledDisplay->setCursor(120, OLED_CONTENT_START_Y + 9);
        oledDisplay->print("\x18");
      }
      if (sCpuScroll + PERF_VISIBLE_ROWS < perfRenderData.cpuCount) {
        oledDisplay->setCursor(120, OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - 8);
        oledDisplay->print("\x19");
      }
    }
  } else {
    // STACK page: every task, riskiest first — min-free-ever in bytes
    // (post-4x-fix units — display raw, never *4). '!' = under 512 B free
    // (note the tiny kernel tasks ipc0/ipc1 always live near their mark).
    oledDisplay->setCursor(0, y);
    oledDisplay->printf("Min free stack  %d", perfRenderData.stkCount);
    y += 9;
    if (sStkScroll > perfRenderData.stkCount - PERF_VISIBLE_ROWS) {
      sStkScroll = perfRenderData.stkCount - PERF_VISIBLE_ROWS;
    }
    if (sStkScroll < 0) sStkScroll = 0;
    for (int row = 0; row < PERF_VISIBLE_ROWS; row++) {
      int r = sStkScroll + row;
      if (r >= perfRenderData.stkCount) break;
      oledDisplay->setCursor(0, y);
      oledDisplay->printf("%-10.10s %5lu%s", perfRenderData.stkName[r],
                          (unsigned long)perfRenderData.stkFree[r],
                          perfRenderData.stkFree[r] < 512 ? "!" : "");
      y += 9;
    }
    if (sStkScroll > 0) {
      oledDisplay->setCursor(120, OLED_CONTENT_START_Y + 9);
      oledDisplay->print("\x18");
    }
    if (sStkScroll + PERF_VISIBLE_ROWS < perfRenderData.stkCount) {
      oledDisplay->setCursor(120, OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - 8);
      oledDisplay->print("\x19");
    }
  }

  // Keep re-rendering while this page is active so the 2 Hz sample gate in
  // preparePerfData refreshes the numbers live. A bare oledMarkDirty() would be
  // undone by the oledClearDirty() at the tail of the shared render frame, so
  // use the timed dirty — a separate deadline (oledDirtyUntilMs) that
  // clearDirty never touches. The hold (one sample interval) comfortably
  // exceeds the render throttle, so the page never falls clean while shown.
  oledMarkDirtyUntil(millis() + PERF_SAMPLE_MS);
}

static bool perfStatsInputHandler(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    sPerfPage ^= 1;   // page keeps its own scroll offset
    oledMarkDirty();
    return true;
  }
  // Up/Down scroll the current page's task viewport.
  int count = (sPerfPage == 0) ? perfRenderData.cpuCount : perfRenderData.stkCount;
  int* scroll = (sPerfPage == 0) ? &sCpuScroll : &sStkScroll;
  if (gNavEvents.down && *scroll + PERF_VISIBLE_ROWS < count) { (*scroll)++; return true; }
  if (gNavEvents.up   && *scroll > 0)                         { (*scroll)--; return true; }
  return false;  // B falls through to the central back handler
}

// Fresh view on forward entry: CPU page first, both viewports at the top.
// (Back-nav preserves the previous position, matching the menu convention.)
static void perfOnEnter(bool isForward) {
  if (!isForward) return;
  sPerfPage  = 0;
  sCpuScroll = 0;
  sStkScroll = 0;
}

// ============================================================================
// System Status Rendered (two-phase rendering)
// ============================================================================

// External battery functions
extern float getBatteryVoltage();
extern float getBatteryPercentage();
extern char getBatteryIcon();
extern bool isBatteryCharging();
extern bool isUsbPresent();

// Pre-gathered system status data to avoid WiFi/heap operations inside I2C transaction
struct SystemStatusRenderData {
  bool wifiConnected;  // CONNECTION axis: associated to an AP
  bool radioOn;        // RADIO axis: powered up at all (WiFi or ESP-NOW)
  char ssid[16];  // Truncated SSID
  char ip[16];     // IP address string
  uint32_t freeHeap;
  unsigned long uptimeHours;
  unsigned long uptimeMinutes;
  float batteryVoltage;
  float batteryPercentage;
  char batteryIcon;
  bool batteryCharging;       // CRATE > +threshold (cell taking charge)
  bool batteryUsbPresent;     // USB connected (charging OR float-plateau)
  bool valid;
};
static SystemStatusRenderData systemStatusRenderData = {0};

// Gather system status data (called OUTSIDE I2C transaction to avoid blocking gamepad)
void prepareSystemStatusData() {
  // Get WiFi data OUTSIDE I2C transaction
  systemStatusRenderData.wifiConnected = WiFi.isConnected();
#if ENABLE_WIFI
  systemStatusRenderData.radioOn = wifiRadioOn();
#else
  systemStatusRenderData.radioOn = false;
#endif

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
  systemStatusRenderData.batteryCharging = isBatteryCharging();
  systemStatusRenderData.batteryUsbPresent = isUsbPresent();
  
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
  
  // Header shows "System Status", no need for title here
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);

  // Battery Status (top priority). Four-state rendering — mirrors the G2
  // corner widget's logic so the OLED and lens columns stay consistent:
  //   icon=='?'              "Power: USB"           no cell installed
  //   isCharging             "Batt: V.VVV NN% USB+" USB in, taking charge
  //   usbPresent (no charge) "Batt: V.VVV NN% USB"  USB in, cell at float
  //   else                   "Batt: V.VVV NN% I"    on battery (I = M/H/F/L/E)
#if ENABLE_BATTERY_MONITOR
  if (systemStatusRenderData.batteryIcon == '?') {
    oledDisplay->print("Power: USB");
  } else {
    oledDisplay->print("Batt: ");
    oledDisplay->print(systemStatusRenderData.batteryVoltage, 2);
    oledDisplay->print("V ");
    oledDisplay->print((int)systemStatusRenderData.batteryPercentage);
    oledDisplay->print("% ");
    if (systemStatusRenderData.batteryCharging) {
      oledDisplay->print("USB+");
    } else if (systemStatusRenderData.batteryUsbPresent) {
      oledDisplay->print("USB");
    } else {
      oledDisplay->print(systemStatusRenderData.batteryIcon);
    }
  }
#else
  oledDisplay->print("Power: USB");
#endif
  oledDisplay->println();

  // Two separate axes: RADIO power, then WiFi CONNECTION.
  oledDisplay->print("Radio: ");
  oledDisplay->println(systemStatusRenderData.radioOn ? "ON" : "OFF");

  // WiFi Status (connection)
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

// ============================================================================
// Mode Registration
// ============================================================================

static const OLEDModeEntry sSystemModes[] = {
  { OLED_SYSTEM_STATUS, "System",    "settings", displaySystemStatusRendered, nullptr, nullptr, false, -1, "B:Back" },
  { OLED_CUSTOM_TEXT,   "Text",      "text",     displayCustomText,           nullptr, nullptr, false, -1, "B:Back" },
  { OLED_MEMORY_STATS,  "Memory",    "memory",   displayMemoryStatsRendered,  nullptr, nullptr, false, -1, "B:Back" },
  { OLED_PERF_STATS,    "Perf",      "memory",   displayPerfStatsRendered,    nullptr, perfStatsInputHandler, false, -1, "\x18\x19:Scroll A:Page B:Back", perfOnEnter },
  { OLED_UNAVAILABLE,   "Unavail",   nullptr,    displayUnavailable,          nullptr, nullptr, false, -1, nullptr  },  // dynamic hints
};

REGISTER_OLED_MODE_MODULE(sSystemModes, sizeof(sSystemModes) / sizeof(sSystemModes[0]), "System");

#endif // ENABLE_OLED_DISPLAY
