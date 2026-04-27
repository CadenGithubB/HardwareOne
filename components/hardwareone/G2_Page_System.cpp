// =============================================================================
// G2 glasses — "System" page implementation
// =============================================================================
// Pure info dump. Same data the OLED System mode shows, formatted for the
// G2 lens (~6 lines max for comfortable reading). Read-only — no taps,
// no state machine.

#include "G2_Page_System.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "Optional_EvenG2.h"   // g2ShowText
#include "System_Debug.h"

#include <esp_heap_caps.h>

#if ENABLE_WIFI
#include <WiFi.h>
#endif

// Battery accessors — same externs OLED_Mode_System uses. May not exist
// on slim builds without a battery monitor; guard with the same flag the
// OLED mode does. (Currently unconditional in this codebase but cheap to
// stay defensive.)
extern float   getBatteryVoltage();
extern uint8_t getBatteryPercentage();

void g2BuildSystemPage(char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';

  String s;
  s.reserve(384);

  // Header — distinguishes this from the Status snapshot.
  s += "System\n";

  // Uptime + free heap on one line for compactness.
  unsigned long secs = millis() / 1000UL;
  unsigned long h    = secs / 3600UL;
  unsigned long m    = (secs / 60UL) % 60UL;
  unsigned heapKb    = (unsigned)(ESP.getFreeHeap() / 1024);
  unsigned minHeapKb = (unsigned)(ESP.getMinFreeHeap() / 1024);
  unsigned heapBigKb = (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) / 1024);
  {
    char line[80];
    snprintf(line, sizeof(line), "Up %luh%lum  Heap %uK\n", h, m, heapKb);
    s += line;
    snprintf(line, sizeof(line), "MinHeap %uK  Block %uK\n",
             minHeapKb, heapBigKb);
    s += line;
  }

  // PSRAM if present.
  if (psramFound()) {
    char line[64];
    unsigned freeKb  = (unsigned)(ESP.getFreePsram() / 1024);
    unsigned totalKb = (unsigned)(ESP.getPsramSize() / 1024);
    snprintf(line, sizeof(line), "PSRAM %uK / %uK\n", freeKb, totalKb);
    s += line;
  }

  // Temperature — same builtin we use in the Status snapshot. Reasonable
  // proxy for SoC heat; not super accurate but a useful trend indicator.
  {
    char line[40];
    float tempC = temperatureRead();
    snprintf(line, sizeof(line), "SoC %.0fC\n", (double)tempC);
    s += line;
  }

  // Battery (if the monitor reports something usable).
  {
    float v = getBatteryVoltage();
    uint8_t pct = getBatteryPercentage();
    if (v > 0.0f) {
      char line[48];
      snprintf(line, sizeof(line), "Batt %.2fV  %u%%\n", (double)v, pct);
      s += line;
    }
  }

#if ENABLE_WIFI
  if (WiFi.isConnected()) {
    char line[80];
    String ssid = WiFi.SSID();
    long rssi   = WiFi.RSSI();
    snprintf(line, sizeof(line), "WiFi %s %lddBm",
             ssid.c_str(), rssi);
    s += line;
  } else {
    s += "WiFi off";
  }
#endif

  strncpy(out, s.c_str(), cap - 1);
  out[cap - 1] = '\0';
}

bool g2ShowSystemPage() {
  char buf[400];
  g2BuildSystemPage(buf, sizeof(buf));
  DEBUG_G2F("[G2] System page (%u B):\n%s", (unsigned)strlen(buf), buf);
  return g2ShowText(buf);
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
