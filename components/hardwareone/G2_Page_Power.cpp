// =============================================================================
// G2 glasses — "Power" page implementation
// =============================================================================
// Two-level layout, both levels rendered as plain list-mode pages so the
// transport stays inside a single CREATE-list fragment (well under the
// 240 B single-fragment ceiling — three rows of <=20 chars each).
//
//   Level 1 (ACTIONS):
//     [0] <- Main Menu
//     [1] Restart
//     [2] Power Off
//
//   Level 2 (CONFIRM):
//     [0] <- Cancel
//     [1] Confirm Restart        (or "Confirm Power Off")
//
// On confirm we push a brief "Restarting..." / "Powering off..." text
// banner, sleep ~800 ms so the BLE notify lands and the lens has time
// to paint, then call esp_restart() / esp_deep_sleep_start().
// Neither of those returns, so this file never sees the next tap.
//
// Power Off uses esp_deep_sleep_start() with NO wake source configured —
// the chip drops to ~10 µA and stays there until the user hits the reset
// button. That's the closest analogue the ESP32 has to a real off
// switch; light/timer-based sleep would defeat the user's intent.

#include "G2_Page_Power.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "G2_Glasses.h"
#include "OLED_Display.h"  // oledPrepareForSleep — kills LDO2 on power-gated boards
#include "System_Debug.h"
#include "System_Power.h"   // powerSleepTransitionAllowed/Mark — anti-flap guard
#include "esp_system.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------

enum PowerLevel : uint8_t {
  POWER_LEVEL_ACTIONS = 0,
  POWER_LEVEL_CONFIRM = 1,
};

enum PowerAction : uint8_t {
  POWER_ACTION_NONE     = 0,
  POWER_ACTION_RESTART  = 1,
  POWER_ACTION_POWEROFF = 2,
};

static PowerLevel  gLevel   = POWER_LEVEL_ACTIONS;
static PowerAction gPending = POWER_ACTION_NONE;

#define POWER_ROW_LEN  24
#define POWER_MAX_ROWS  3

static char        gRows[POWER_MAX_ROWS][POWER_ROW_LEN];
static const char* gRowPtrs[POWER_MAX_ROWS];

// -----------------------------------------------------------------------------
// Row builders
// -----------------------------------------------------------------------------

static size_t buildActionRows() {
  snprintf(gRows[0], POWER_ROW_LEN, "<- Main Menu");
  snprintf(gRows[1], POWER_ROW_LEN, "Restart");
  snprintf(gRows[2], POWER_ROW_LEN, "Power Off");
  for (size_t i = 0; i < POWER_MAX_ROWS; i++) gRowPtrs[i] = gRows[i];
  return POWER_MAX_ROWS;
}

static size_t buildConfirmRows(PowerAction action) {
  snprintf(gRows[0], POWER_ROW_LEN, "<- Cancel");
  if (action == POWER_ACTION_POWEROFF) {
    snprintf(gRows[1], POWER_ROW_LEN, "Confirm Power Off");
  } else {
    snprintf(gRows[1], POWER_ROW_LEN, "Confirm Restart");
  }
  gRowPtrs[0] = gRows[0];
  gRowPtrs[1] = gRows[1];
  return 2;
}

// -----------------------------------------------------------------------------
// CLI text
// -----------------------------------------------------------------------------

void g2BuildPowerInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
  snprintf(out, cap,
           "Power\n"
           "Restart    reboot the device\n"
           "Power Off  enter deep sleep");
}

// -----------------------------------------------------------------------------
// Action executors — neither returns
// -----------------------------------------------------------------------------

static void doRestart() {
  g2ShowText("Restarting...");
  // Give the BLE notify task time to deliver the CREATE-text and let the
  // lens paint it before we yank power. 800 ms covers the worst-case
  // single-fragment swap latency observed on 2.2.0.242.
  vTaskDelay(pdMS_TO_TICKS(800));
  DEBUG_G2F("[G2] Power: esp_restart()");
  esp_restart();
}

static void doPowerOff() {
  // Anti-flap: a glitched menu-confirm path could fire this multiple times in
  // quick succession. Refuse if cooldown hasn't elapsed. (Deep sleep is even
  // more disruptive than light sleep — full chip reset on wake — so the
  // guard is at least as important here as in cmd_lightsleep.)
  unsigned long cooldownRemain = 0;
  if (!powerSleepTransitionAllowed(&cooldownRemain)) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Try again in %lus", (unsigned long)((cooldownRemain + 999) / 1000));
    g2ShowText(msg);
    DEBUG_G2F("[G2] Power: deep sleep refused — cooldown %lu ms remaining", cooldownRemain);
    vTaskDelay(pdMS_TO_TICKS(1200));
    return;
  }
  g2ShowText("Powering off...");
  vTaskDelay(pdMS_TO_TICKS(800));
  // Drop the LDO2 rail (if this board has one) so anything on I2C2 —
  // currently the OLED on FeatherS3[D] — truly loses power instead of just
  // sitting there with the panel "off" but Vcc still flowing. No resume
  // counterpart needed: deep sleep wakes via reset, so the normal boot
  // path re-asserts everything from scratch.
  oledPrepareForSleep();
  powerSleepTransitionMark();
  DEBUG_G2F("[G2] Power: esp_deep_sleep_start() — wake via reset button");
  // No wake source configured: chip stays in deep sleep until the user
  // hits the physical reset. Quiescent draw ~10 µA.
  esp_deep_sleep_start();
}

// -----------------------------------------------------------------------------
// Public — show menu
// -----------------------------------------------------------------------------

void g2ShowPowerMenu() {
  gLevel   = POWER_LEVEL_ACTIONS;
  gPending = POWER_ACTION_NONE;

  size_t n = buildActionRows();
  if (g2ShowListPage(gRowPtrs, n)) {
    g2SetHijackPage(G2_HIJACK_PAGE_POWER);
    DEBUG_G2F("[G2] Power menu shown (rows=%u)", (unsigned)n);
  } else {
    DEBUG_G2F("[G2] Power menu show FAILED");
  }
}

// -----------------------------------------------------------------------------
// Public — tap dispatch
// -----------------------------------------------------------------------------

void g2PowerHandleTap(uint32_t idx) {
  switch (gLevel) {

    case POWER_LEVEL_ACTIONS: {
      if (idx == 0) {
        // <- Back to root hijack menu.
        g2SetHijackPage(G2_HIJACK_PAGE_MAIN);
        extern void g2RedrawHijackMainMenu();
        g2RedrawHijackMainMenu();
        return;
      }
      if (idx == 1) {
        gPending = POWER_ACTION_RESTART;
      } else if (idx == 2) {
        gPending = POWER_ACTION_POWEROFF;
      } else {
        return;
      }
      gLevel = POWER_LEVEL_CONFIRM;
      size_t n = buildConfirmRows(gPending);
      g2ShowListPage(gRowPtrs, n);
      DEBUG_G2F("[G2] Power: confirm prompt for %s",
                gPending == POWER_ACTION_POWEROFF ? "Power Off" : "Restart");
      return;
    }

    case POWER_LEVEL_CONFIRM: {
      if (idx == 0) {
        // <- Cancel: back to action list.
        gLevel   = POWER_LEVEL_ACTIONS;
        PowerAction was = gPending;
        gPending = POWER_ACTION_NONE;
        size_t n = buildActionRows();
        g2ShowListPage(gRowPtrs, n);
        DEBUG_G2F("[G2] Power: cancelled (was %s)",
                  was == POWER_ACTION_POWEROFF ? "Power Off" : "Restart");
        return;
      }
      if (idx == 1) {
        if (gPending == POWER_ACTION_POWEROFF) {
          doPowerOff();   // does not return
        } else {
          doRestart();    // does not return
        }
      }
      return;
    }
  }
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
