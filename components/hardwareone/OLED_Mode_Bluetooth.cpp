// OLED_Mode_Bluetooth.cpp - Bluetooth / G2-glasses OLED modes
//
// Umbrella-compliant (matches OLED_Mode_Power / OLED_Mode_Network): each screen
// is its own OLED mode — no bluetoothInG2Menu / gBluetoothShowingStatus flag-soup.
// Menus are OLEDScrollState models; status screens and the G2 submenu are pushed
// sub-modes entered via requestOLEDMode and popped by the global B handler.
//   OLED_BLUETOOTH            main menu
//   OLED_BLUETOOTH_STATUS     BT status detail (pushed)
//   OLED_BLUETOOTH_G2         G2 glasses submenu (pushed, ENABLE_G2_GLASSES)
//   OLED_BLUETOOTH_G2_STATUS  G2 status detail (pushed, ENABLE_G2_GLASSES)
//   OLED_BLUETOOTH_R1         R1 ring submenu (pushed, ENABLE_G2_GLASSES)
//   OLED_BLUETOOTH_PHONE      phone submenu: link + pipe settings (pushed, ENABLE_BLE_PHONE_PIPE)
//   OLED_BLUETOOTH_PHONE_CODE `phone pair new`'s code (pushed by the command, ENABLE_BLE_PHONE_PIPE)
//
// (Moved out of Bluetooth.cpp so every OLED mode lives in its own OLED_Mode_*.cpp.
//  BLE internals are reached through Bluetooth.h; the recent-message buffer stays
//  file-local in Bluetooth.cpp behind bleGetRecentMessages().)

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY && ENABLE_BLUETOOTH

#include <Adafruit_SSD1306.h>
#include <cstdarg>    // va_list (status detail rows)
#include "OLED_Utils.h"
#include "HAL_Input.h"
#include "System_Settings.h"
#include "Bluetooth.h"
#include "OLED_SettingsEditor.h"  // openSettingsEditorForModule
#if ENABLE_G2_GLASSES
#include "G2_Glasses.h"
#include "G2_Ring.h"      // R1 ring telemetry + poll (submenu below, mirrors G2)
#include "BLE_Peers.h"    // gBlePeerData[BLE_PEER_R1_RING].autoReconnect
#include "BLE_CentralCoordinator.h"  // bleCentralRingShouldYield (ring paused for the phone pipe)
#include "OLED_UI.h"      // oledToastShow
#endif
#include "BLE_PhonePipe.h"    // phonePipeReady (stub without the pipe)
#include "BLE_LinkStatus.h"   // role, relay pipe and app console words (status, Phone, watcher)
#if ENABLE_BLE_PHONE_PIPE
#include "OLED_UI.h"          // oledToastShow
#include "BLE_PhonePairCode.h"  // hw1code::wipe for the code screen's copy
#endif

// ---- Main menu model -------------------------------------------------------
enum BtAction {
  BT_ACT_STATUS,
  BT_ACT_SETTINGS,
  BT_ACT_CONSOLE,
  BT_ACT_GLASSES,
  BT_ACT_PIPE,
  BT_ACT_G2,
  BT_ACT_R1,
  BT_ACT_PHONE,
  BT_ACT_ADVERTISING,
  BT_ACT_DISCONNECT,
};
EXT_RAM_BSS_ATTR static OLEDScrollState sBtMenuScroll;
static bool sBtMenuInit = false;
EXT_RAM_BSS_ATTR static BtAction sBtActions[OLED_SCROLL_MAX_ITEMS];
// The link the rows were built from: a tap does what its row said.
// bleLinkStatus() never blocks and reads gBLEState only under its lifecycle
// lock: deinit frees it.
static BleLinkStatus sBtShown;

// One row per role in the build, marked " *" while it runs. A row that does not
// run switches to it; one that runs stops it. App console is server mode, a
// direct app connection (`openconsole` closes the glasses, ring and phone
// first; the device rows open and close one device each): beside the glasses
// the console already rides the phone link, so its confirm says server. The
// confirm lines fit the 19-column confirm box.
struct BtRoleRow {
  BtAction act;
  const char* label;
  hw1link::Role role;       // the role it marks; the glasses with or without the pipe
  const char* start;        // the command when it does not run
  const char* stop;         // the command while it runs
  const char* startAsk;
  const char* stopAsk;
  const char* startNote;    // second confirm line while another role runs, or nullptr
};
// What a switch to server mode stops, named for the roles this build has.
#if ENABLE_G2_GLASSES && ENABLE_BLE_PHONE_PIPE
#define BT_SERVER_STOPS "Stops glasses/pipe"
#elif ENABLE_G2_GLASSES
#define BT_SERVER_STOPS "Stops the glasses"
#else
#define BT_SERVER_STOPS "Stops the pipe"
#endif
static const BtRoleRow kBtRoles[] = {
  {BT_ACT_CONSOLE, "App console", hw1link::Role::Console, "openconsole", "closeconsole",
   "Switch to server?", "Stop app console?", BT_SERVER_STOPS},
#if ENABLE_G2_GLASSES
  {BT_ACT_GLASSES, "Glasses", hw1link::Role::Glasses, "openg2", "closeg2",
   "Open glasses?", "Close glasses?", nullptr},
#endif
#if ENABLE_BLE_PHONE_PIPE
  {BT_ACT_PIPE, "Phone pipe", hw1link::Role::PhonePipe, "openphone", "closephone",
   "Open phone pipe?", "Close phone pipe?", nullptr},
#endif
};

static const BtRoleRow* btRoleRow(BtAction act) {
  for (const BtRoleRow& r : kBtRoles) if (r.act == act) return &r;
  return nullptr;
}

static bool btRoleRuns(const BtRoleRow& r, hw1link::Role running) {
  if (running == hw1link::Role::GlassesPipe) running = hw1link::Role::Glasses;
  return running == r.role;
}

// The command the open role confirm runs, admin-gated via cmd_exec. Never
// init/deinit Bluetooth on the OLED input task.
static const char* sBtRoleCommand = nullptr;
static void btRoleConfirmed(void* /*userData*/) {
  if (sBtRoleCommand) (void)oledRunCommand(sBtRoleCommand);
}

// Stopping, or switching away from a running role, defaults to No.
static void btRoleTapped(const BtRoleRow& r, hw1link::Role running) {
  const bool runs = btRoleRuns(r, running);
  const bool other = !runs && running != hw1link::Role::Off;
  const char* cmd = runs ? r.stop : r.start;
  if (oledConfirmRequest(runs ? r.stopAsk : r.startAsk, other ? r.startNote : nullptr,
                         btRoleConfirmed, nullptr, !runs && !other)) {
    sBtRoleCommand = cmd;
  }
}

static void addBtItem(const char* label, BtAction act) {
  if (oledScrollAddItem(&sBtMenuScroll, label)) {
    int idx = sBtMenuScroll.itemCount - 1;
    if (idx >= 0 && idx < (int)(sizeof(sBtActions) / sizeof(sBtActions[0]))) {
      sBtActions[idx] = act;
    }
  }
}

static void populateBtMenu() {
  if (!sBtMenuInit) {
    oledScrollInit(&sBtMenuScroll, nullptr, OLED_CONTENT_HEIGHT / 8);
    sBtMenuInit = true;
  }
  oledScrollClearKeepSelection(&sBtMenuScroll);
  sBtShown = bleLinkStatus();

  addBtItem("Status", BT_ACT_STATUS);
  addBtItem("Settings", BT_ACT_SETTINGS);
  for (const BtRoleRow& r : kBtRoles) {
    // Beside the glasses the phone is the Phone >> submenu's row (Open/Close
    // phone on the shared stack); there is no phone-only role to switch to.
    if (r.act == BT_ACT_PIPE && (sBtShown.role == hw1link::Role::Glasses ||
                                 sBtShown.role == hw1link::Role::GlassesPipe)) continue;
    addBtItem(r.label, r.act);
  }
#if ENABLE_G2_GLASSES
  addBtItem("G2 Glasses >>", BT_ACT_G2);
  addBtItem("R1 Ring >>",    BT_ACT_R1);   // Even Realities ring — same BLE-client subsystem
#endif
#if ENABLE_BLE_PHONE_PIPE
  addBtItem("Phone >>",      BT_ACT_PHONE);  // Android phone (relay pipe) — beside the glasses or phone-only
#endif
  // The app console's rows: only while its server runs / while an app is
  // connected. On the phone pipe's link `bledisconnect` ends the console
  // session and the pipe keeps the link, so the row says so.
  if (sBtShown.consoleView.up) addBtItem("Advertising", BT_ACT_ADVERTISING);
  if (sBtShown.consoleView.clients) {
    addBtItem(sBtShown.consoleView.up ? "Disconnect" : "End console", BT_ACT_DISCONNECT);
  }

  oledScrollClampSelection(&sBtMenuScroll);
}

// ---- OLED_BLUETOOTH_STATUS: BT status detail (read-only, pushed) -----------
// The three link axes in BLE_LinkStatusCore.h's words, as the lens, web page and
// CLI show them: the role running now (and the saved mode when it differs), the
// relay pipe (this device is central to the app's GATT server) and the app
// console (the app is central to this device: the server's own link, or beside
// the glasses the pipe's link). The content area holds 5 rows on a 64 px OLED
// and 13 on the P4 canvas, so the axes come first and the console server's
// detail fills what is left.
static constexpr int kBtRowHeight = 8;

static int btRowsLeft() {
  return (OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - oledDisplay->getCursorY()) / kBtRowHeight;
}

// One row cut at 21 columns: Adafruit wraps a longer one onto the next row.
static void btRow(const char* label, const char* value) {
  char line[22];
  snprintf(line, sizeof(line), "%s%s", label, value);
  oledDisplay->println(line);
}

// A detail row, drawn only while it fits: the footer clears its area over
// anything below and would leave the edges of a cut row.
static void btDetail(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void btDetail(const char* fmt, ...) {
  if (btRowsLeft() < 1) return;
  char line[22];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  oledDisplay->println(line);
}

// "Role: Phone pipe", then "Saved: App console" when the saved mode differs.
static void btRoleRows(const BleLinkStatus& s) {
  btRow("Role: ", hw1link::roleText(s.role));
  if (hw1link::savedDiffers(s.role, s.saved)) btRow("Saved: ", hw1link::roleText(s.saved));
}

static void displayBluetoothStatusDetail() {
  if (!oledDisplay) return;
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(SSD1306_WHITE);
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);

  const BleLinkStatus s = bleLinkStatus();
  char pipe[16];  // "Pipe: " + 15 = 21 columns ("fail: not found")
  if (s.role == hw1link::Role::Off) {
    btRoleRows(s);  // Off always names the saved role (the one boot starts)
    oledDisplay->println();
    oledDisplay->println("Select a role in");
    oledDisplay->println("the Bluetooth menu");
    return;
  }
  if (s.role != hw1link::Role::Console) {
    // A central role: the glasses/ring client, the relay pipe, or both. The
    // Console row is the console on the pipe's link ("waiting", "signed in").
    btRoleRows(s);
#if ENABLE_G2_GLASSES
    if (isG2ClientInitialized()) {
      const bool glasses = isG2Connected(), ring = g2RingIsConnected();
      // Role, Glasses, Ring, Pipe, Console fill a 64 px OLED; with a Saved row
      // the glasses and ring share one.
      if (btRowsLeft() >= 2 + (s.pipeBuilt ? 1 : 0) + 1) {
        btRow("Glasses: ", glasses ? "connected" : "--");
        btRow("Ring: ", ring ? "connected" : "--");
      } else {
        char line[22];
        snprintf(line, sizeof(line), "Glasses: %s Ring: %s", glasses ? "ok" : "--", ring ? "ok" : "--");
        oledDisplay->println(line);
      }
    }
#endif
    if (s.pipeBuilt) btRow("Pipe: ", hw1link::pipeLine(s.pipe, pipe, sizeof(pipe)));
    btRow("Console: ", hw1link::consoleText(s.console));
    return;
  }

  // The app console server runs; the pipe never runs beside it, so its row
  // reads its own words: off, no secret, or closing just after a role switch.
  btRoleRows(s);
  btRow("Console: ", hw1link::consoleText(s.console));
  char line[24];  // "Clients: 4/4 Adv: off" is 21 columns; the count never passes the maximum
  snprintf(line, sizeof(line), "Clients: %u/%d Adv: %s", (unsigned)s.consoleView.clients,
           BLE_MAX_CONNECTIONS, s.consoleView.advertising ? "on" : "off");
  oledDisplay->println(line);  // app clients only: peer links are not the console's
  if (s.pipeBuilt) btRow("Pipe: ", hw1link::pipeLine(s.pipe, pipe, sizeof(pipe)));

  if (!gBLEState || !gBLEState->initialized) return;  // stopped since the snapshot
  // Device name (truncate if needed)
  const char* name = gSettings.bleDeviceName.length() > 0 ? gSettings.bleDeviceName.c_str() : "HardwareOne";
  char truncated[13];
  if (strlen(name) > 12) {
    memcpy(truncated, name, 11);
    truncated[11] = '~';
    truncated[12] = '\0';
    name = truncated;
  }
  btDetail("Name: %s", name);

  if (gBLEState->connectionState == BLE_STATE_CONNECTED) {
    btDetail("Rx:%lu Tx:%lu", (unsigned long)gBLEState->commandsReceived,
             (unsigned long)gBLEState->responsesSent);
    btDetail("Peers:");
    int shown = 0;
    for (int i = 0; i < BLE_MAX_CONNECTIONS && shown < 3; i++) {
      if (!gBLEState->connections[i].active) continue;
      uint32_t duration = (millis() - gBLEState->connections[i].connectedSince) / 1000;
      btDetail("#%d %s %lus", i, gBLEState->connections[i].deviceName.c_str(), (unsigned long)duration);
      shown++;
    }
  } else {
    btDetail("TX Power: %d", gSettings.bleTxPower);
    btDetail("Total: %lu", (unsigned long)gBLEState->totalConnections);
  }

  const uint8_t streams = gBLEState->streamFlags;
  btDetail("Streams: %s%s%s", (streams & BLE_STREAM_SENSORS) ? "S" : "s",
           (streams & BLE_STREAM_SYSTEM) ? " Sys" : " sys", (streams & BLE_STREAM_EVENTS) ? " Ev" : " ev");
  btDetail("Int:%lu/%lu", (unsigned long)gBLEState->sensorStreamInterval,
           (unsigned long)gBLEState->systemStreamInterval);

  // Recent messages (newest first) via the Bluetooth.cpp accessor.
  const char* recent[2];
  int nRecent = bleGetRecentMessages(recent, 2);
  if (nRecent > 0) {
    btDetail("%s", "");
    btDetail("Last:");
    for (int i = 0; i < nRecent; i++) btDetail("%s", recent[i]);
  }
}

// ---- OLED_BLUETOOTH: main menu (display + input) ---------------------------
static void displayBluetoothStatus() {
  if (!oledDisplay) return;
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(SSD1306_WHITE);

  populateBtMenu();

  // Decorated single-line render from the scroll state (honors the scroll
  // window), with the original inline state indicators.
  const int lineHeight = 8;
  int visEnd = sBtMenuScroll.scrollOffset + sBtMenuScroll.visibleLines;
  if (visEnd > sBtMenuScroll.itemCount) visEnd = sBtMenuScroll.itemCount;
  int row = 0;
  for (int i = sBtMenuScroll.scrollOffset; i < visEnd; i++, row++) {
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y + row * lineHeight);
    oledDisplay->print(i == sBtMenuScroll.selectedIndex ? "> " : "  ");
    if (sBtMenuScroll.items[i].line1) oledDisplay->print(sBtMenuScroll.items[i].line1);
    switch (sBtActions[i]) {
      case BT_ACT_CONSOLE:
      case BT_ACT_GLASSES:
      case BT_ACT_PIPE:
        if (const BtRoleRow* r = btRoleRow(sBtActions[i])) {
          if (btRoleRuns(*r, sBtShown.role)) oledDisplay->print(" *");
        }
        break;
#if ENABLE_G2_GLASSES
      case BT_ACT_G2:
        if (isG2Connected()) oledDisplay->print(" *");
        break;
      case BT_ACT_R1:
        if (g2RingIsConnected()) oledDisplay->print(" *");
        break;
#endif
#if ENABLE_BLE_PHONE_PIPE
      case BT_ACT_PHONE:
        if (sBtShown.pipe.phase == hw1link::Pipe::Ready) oledDisplay->print(" *");
        break;
#endif
      case BT_ACT_ADVERTISING:
        if (sBtShown.consoleView.advertising) oledDisplay->print(" *");
        break;
      default:
        break;
    }
  }

  // Scroll indicators
  if (sBtMenuScroll.scrollOffset > 0) {
    oledDisplay->setCursor(120, OLED_CONTENT_START_Y);
    oledDisplay->print("\x18");
  }
  if (visEnd < sBtMenuScroll.itemCount) {
    oledDisplay->setCursor(120, OLED_CONTENT_START_Y + (sBtMenuScroll.visibleLines - 1) * lineHeight);
    oledDisplay->print("\x19");
  }
}

static bool bluetoothInputHandler(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  if (oledScrollHandleNav(&sBtMenuScroll)) return true;

  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    int idx = sBtMenuScroll.selectedIndex;
    if (idx < 0 || idx >= sBtMenuScroll.itemCount) return true;
    switch (sBtActions[idx]) {
      case BT_ACT_STATUS:
        requestOLEDMode(OLED_BLUETOOTH_STATUS, "bluetooth.status");
        break;
      case BT_ACT_SETTINGS:
        if (openSettingsEditorForModule("bluetooth")) {
          requestOLEDMode(OLED_SETTINGS, "bluetooth.settings");
        }
        break;
      case BT_ACT_CONSOLE:  // the role the rows showed (sBtShown)
      case BT_ACT_GLASSES:
      case BT_ACT_PIPE:
        if (const BtRoleRow* r = btRoleRow(sBtActions[idx])) btRoleTapped(*r, sBtShown.role);
        break;
#if ENABLE_G2_GLASSES
      case BT_ACT_G2:
        requestOLEDMode(OLED_BLUETOOTH_G2, "bluetooth.g2");
        break;
      case BT_ACT_R1:
        requestOLEDMode(OLED_BLUETOOTH_R1, "bluetooth.r1");
        break;
#endif
#if ENABLE_BLE_PHONE_PIPE
      case BT_ACT_PHONE:
        requestOLEDMode(OLED_BLUETOOTH_PHONE, "bluetooth.phone");
        break;
#endif
      case BT_ACT_ADVERTISING:  // the rows' console view (sBtShown)
        if (sBtShown.consoleView.up) executeOLEDCommand("bleadv toggle");
        break;
      case BT_ACT_DISCONNECT:
        if (sBtShown.consoleView.clients) executeOLEDCommand("bledisconnect");
        break;
      default:
        break;
    }
    return true;
  }

  // B: return false so the global handler pops the mode stack.
  return false;
}

#if ENABLE_G2_GLASSES
// ---- OLED_BLUETOOTH_G2: G2 glasses submenu --------------------------------
static const char g2TextInputBuffer[] = "Hello from ESP32!";  // fixed "Show Text" payload (const: .rodata, was 64 B of .data)

EXT_RAM_BSS_ATTR static OLEDScrollState sG2MenuScroll;  // PSRAM, matching sBtMenuScroll
static bool sG2MenuInit = false;
// Index → action: 0 Connect, 1 Disconnect, 2 Status, 3 Show Text, 4 Nav Mode
static const char* kG2MenuItems[] = { "Connect", "Disconnect", "Status", "Show Text", "Nav Mode" };

static void populateG2Menu() {
  if (!sG2MenuInit) {
    oledScrollInit(&sG2MenuScroll, nullptr, OLED_CONTENT_HEIGHT / 8);
    sG2MenuInit = true;
  }
  oledScrollClearKeepSelection(&sG2MenuScroll);
  for (unsigned i = 0; i < sizeof(kG2MenuItems) / sizeof(kG2MenuItems[0]); i++) {
    oledScrollAddItem(&sG2MenuScroll, kG2MenuItems[i]);
  }
  oledScrollClampSelection(&sG2MenuScroll);
}

static void displayG2Menu() {
  if (!oledDisplay) return;
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(SSD1306_WHITE);
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);

  // Header with connection status
  oledDisplay->print("G2 GLASSES ");
  if (isG2Connected()) {
    oledDisplay->println("[OK]");
  } else if (isG2ClientInitialized()) {
    G2State state = getG2State();
    if (state == G2_STATE_SCANNING) {
      oledDisplay->println("[SCAN]");
    } else if (state == G2_STATE_CONNECTING || state == G2_STATE_AUTHENTICATING) {
      oledDisplay->println("[...]");
    } else {
      oledDisplay->println("[--]");
    }
  } else {
    oledDisplay->println("[OFF]");
  }

  populateG2Menu();

  extern bool gG2MenuNavEnabled;
  for (int i = 0; i < sG2MenuScroll.itemCount; i++) {
    oledDisplay->print(i == sG2MenuScroll.selectedIndex ? "> " : "  ");
    if (sG2MenuScroll.items[i].line1) oledDisplay->print(sG2MenuScroll.items[i].line1);
    if (i == 0 && isG2Connected()) oledDisplay->print(" *");        // Connect
    else if (i == 4 && gG2MenuNavEnabled) oledDisplay->print(" *"); // Nav Mode
    oledDisplay->println();
  }
}

static bool g2InputHandler(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  if (oledScrollHandleNav(&sG2MenuScroll)) return true;

  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    switch (sG2MenuScroll.selectedIndex) {
      case 0:  // Connect — admin-gated via cmd_exec (cmd_g2connect stamps)
        if (!isG2Connected()) {
          executeOLEDCommand("openg2");
        }
        break;
      case 1:  // Disconnect — admin-gated via cmd_exec
        if (isG2Connected()) {
          executeOLEDCommand("closeg2");
        }
        break;
      case 2:  // Status
        requestOLEDMode(OLED_BLUETOOTH_G2_STATUS, "bluetooth.g2.status");
        break;
      case 3:  // Show Text (fixed payload)
        if (isG2Connected()) g2ShowText(g2TextInputBuffer);
        break;
      case 4:  // Nav Mode toggle
        { extern bool gG2MenuNavEnabled; gG2MenuNavEnabled = !gG2MenuNavEnabled; }
        break;
    }
    return true;
  }
  return false;  // B: global pop
}

// ---- OLED_BLUETOOTH_G2_STATUS: G2 status detail (read-only, pushed) --------
static void displayG2StatusDetail() {
  if (!oledDisplay) return;
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(SSD1306_WHITE);
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);

  oledDisplay->println("== G2 GLASSES ==");
  oledDisplay->println();
  oledDisplay->print("State: ");
  oledDisplay->println(getG2StateString());

  if (isG2Connected()) {
    char statusBuf[64];
    getG2Status(statusBuf, sizeof(statusBuf));
    oledDisplay->println(statusBuf);
    extern bool gG2MenuNavEnabled;
    oledDisplay->print("Nav Mode: ");
    oledDisplay->println(gG2MenuNavEnabled ? "ON" : "OFF");
  } else {
    oledDisplay->println();
    oledDisplay->println("Not connected.");
    oledDisplay->println("Use Connect to");
    oledDisplay->println("pair glasses.");
  }
}

// ---- OLED_BLUETOOTH_R1: R1 ring connect submenu -----------------------------
// Connection controls only. Live vitals / Poll / Track live in OLED_R1_HEALTH
// (ENABLE_R1_HEALTH) so Bluetooth stays the link surface.
static int sR1Sel = 0;   // 0 Connect, 1 Disconnect, 2 AutoReconnect

static void r1OnEnter(bool isForward) {
  if (!isForward) return;
  sR1Sel = 0;
}

static void displayR1Ring() {
  if (!oledDisplay) return;
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(SSD1306_WHITE);

  const bool conn = g2RingIsConnected();

  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  oledDisplay->print("R1 RING ");
  if (!bleSubsystemActive()) oledDisplay->println("[BLE off]");
  else if (conn)             oledDisplay->println("[OK]");
  else if (bleCentralRingShouldYield()) oledDisplay->println("[paused]");  // phone pipe has its link
  else                       oledDisplay->println("[--]");

  oledDisplay->setCursor(0, OLED_CONTENT_START_Y + 10);
#if ENABLE_R1_HEALTH
  oledDisplay->println("Vitals: R1 Health");
#else
  oledDisplay->println("Connect / AutoRecon");
#endif

  const int a0 = OLED_CONTENT_START_Y + 28;
  for (int i = 0; i < 3; i++) {
    oledDisplay->setCursor(0, a0 + i * 8);
    oledDisplay->print(i == sR1Sel ? "> " : "  ");
    if (i == 0)      { oledDisplay->print("Connect"); if (conn) oledDisplay->print(" *"); }
    else if (i == 1) oledDisplay->print("Disconnect");
    else             { oledDisplay->print("AutoRecon "); oledDisplay->print(blePeerAutoReconnectEnabled(BLE_PEER_R1_RING) ? "ON" : "OFF"); }
  }
}

static bool r1InputHandler(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  if (gNavEvents.up   && sR1Sel > 0) { sR1Sel--; return true; }
  if (gNavEvents.down && sR1Sel < 2) { sR1Sel++; return true; }
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    switch (sR1Sel) {
      case 0:  // Connect — async, self-scans; admin-gated on cmd_exec
        if (!g2RingIsConnected()) {
          char out[96];
          if (!executeOLEDCommandWithResult("openring", out, sizeof(out)) ||
              strncmp(out, "Error", 5) == 0) {
            oledToastShow(bleCentralRingShouldYield() ? "Ring paused: pipe on" : "Ring connect refused", 2000);
          }
        }
        break;
      case 1:  // Disconnect — async, admin-gated
        if (g2RingIsConnected()) executeOLEDCommand("closering");
        break;
      case 2:  // AutoReconnect toggle — dispatch the opposite of the current state
        executeOLEDCommand(blePeerAutoReconnectEnabled(BLE_PEER_R1_RING)
                           ? "bleautoreconnect r1-ring off"
                           : "bleautoreconnect r1-ring on");
        break;
    }
    return true;
  }
  return false;  // B: global pop back to the Bluetooth menu
}
#endif // ENABLE_G2_GLASSES

#if ENABLE_BLE_PHONE_PIPE
// ---- OLED_BLUETOOTH_PHONE: phone submenu ------------------------------------
// Same shape as R1/G2: the link control first (Open/Close phone: the pipe on
// the shared client stack, connecting the paired phone), then the pipe's own
// settings. Pairing is `phone pair`, which needs a local admin — this screen —
// while the pipe runs; Android must use the same pipe passphrase. New code and
// pair runs `phone pair new` (super admin): a device code, shown on its own
// screen below, replaces the secret and opens a 3 min window.
enum PhoneAction { PH_ACT_PIPE, PH_ACT_AUTO, PH_ACT_LABEL,
                   PH_ACT_PAIR, PH_ACT_PAIR_NEW, PH_ACT_FORGET };
static constexpr int kPhoneHeaderLines = 3;  // pipe + pairing, the pipe's words, the console
EXT_RAM_BSS_ATTR static OLEDScrollState sPhoneScroll;
static bool sPhoneScrollInit = false;
EXT_RAM_BSS_ATTR static PhoneAction sPhoneActions[OLED_SCROLL_MAX_ITEMS];
// The state the visible rows were built from: a tap does what its row said.
static BleLinkStatus sPhoneShown;

static bool phoneReady(const BleLinkStatus& s) { return s.pipe.phase == hw1link::Pipe::Ready; }

// The pipe owner runs: BLE_LinkStatusCore.h reports a stopped pipe as off or no secret.
static bool btPipeRunning(const BleLinkStatus& s) {
  return s.pipeBuilt && s.pipe.phase != hw1link::Pipe::Off && s.pipe.phase != hw1link::Pipe::NoSecret;
}

// The pipe at a glance: [OK] up, [..] on its way, [!!] needs the user, [off].
static const char* phonePipeBadge(hw1link::Pipe p) {
  using hw1link::Pipe;
  switch (p) {
    case Pipe::Ready: return "[OK]";
    case Pipe::Waiting: case Pipe::Searching: case Pipe::Linking:
    case Pipe::Pairing: case Pipe::Closing: return "[..]";
    case Pipe::Failed: case Pipe::Fault: case Pipe::NoSecret: case Pipe::NotPaired: return "[!!]";
    case Pipe::Idle: return "[--]";  // running, not seeking (e.g. after Disconnect)
    case Pipe::Off: case Pipe::NotBuilt: break;
  }
  return "[off]";
}

static void addPhoneItem(const char* label, PhoneAction act) {
  if (!oledScrollAddItem(&sPhoneScroll, label, nullptr, act != PH_ACT_LABEL)) return;
  const int idx = sPhoneScroll.itemCount - 1;
  if (idx >= 0 && idx < OLED_SCROLL_MAX_ITEMS) sPhoneActions[idx] = act;
}

// Fixed rows, like R1: a row that does not apply now says why when pressed.
static void populatePhoneMenu(const BleLinkStatus& s) {
  if (!sPhoneScrollInit) {
    oledScrollInit(&sPhoneScroll, nullptr, oledVisibleRows(8, kPhoneHeaderLines * 8, 2));
    sPhoneScrollInit = true;
  }
  oledScrollClearKeepSelection(&sPhoneScroll);
  addPhoneItem(btPipeRunning(s) ? "Close Phone" : "Open Phone", PH_ACT_PIPE);
  addPhoneItem(s.pipeAutoReconnect ? "AutoRecon ON" : "AutoRecon OFF", PH_ACT_AUTO);
  addPhoneItem("- Pipe settings -", PH_ACT_LABEL);
  addPhoneItem("Pair Phone", PH_ACT_PAIR);
  addPhoneItem("New code and pair", PH_ACT_PAIR_NEW);
  addPhoneItem("Forget Phone", PH_ACT_FORGET);
  oledScrollClampSelection(&sPhoneScroll);
}

static void displayPhone() {
  if (!oledDisplay) return;
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(SSD1306_WHITE);

  const BleLinkStatus s = bleLinkStatus();
  sPhoneShown = s;
  populatePhoneMenu(s);

  // The relay pipe (this device is central to the app's server), then the app
  // console, which beside the glasses rides the pipe's link: "PIPE [OK] paired"
  // / "ready" / "Console: signed in". 21 columns each.
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  char line[22];
  snprintf(line, sizeof(line), "PIPE %s %s", phonePipeBadge(s.pipe.phase),
           s.pipePaired ? "paired" : "not paired");
  oledDisplay->println(line);
  oledDisplay->println(hw1link::pipeLine(s.pipe, line, sizeof(line)));
  snprintf(line, sizeof(line), "Console: %s", hw1link::consoleText(s.console));
  oledDisplay->println(line);

  const int lineHeight = 8;
  const int top = OLED_CONTENT_START_Y + kPhoneHeaderLines * lineHeight;
  int visEnd = sPhoneScroll.scrollOffset + sPhoneScroll.visibleLines;
  if (visEnd > sPhoneScroll.itemCount) visEnd = sPhoneScroll.itemCount;
  int row = 0;
  for (int i = sPhoneScroll.scrollOffset; i < visEnd; i++, row++) {
    oledDisplay->setCursor(0, top + row * lineHeight);
    const bool label = sPhoneActions[i] == PH_ACT_LABEL;
    if (!label) oledDisplay->print(i == sPhoneScroll.selectedIndex ? "> " : "  ");
    if (sPhoneScroll.items[i].line1) oledDisplay->print(sPhoneScroll.items[i].line1);
    if (sPhoneActions[i] == PH_ACT_PIPE && phoneReady(s)) oledDisplay->print(" *");
  }
  if (sPhoneScroll.scrollOffset > 0) {
    oledDisplay->setCursor(120, top);
    oledDisplay->print("\x18");
  }
  if (visEnd < sPhoneScroll.itemCount) {
    oledDisplay->setCursor(120, top + (sPhoneScroll.visibleLines - 1) * lineHeight);
    oledDisplay->print("\x19");
  }
}

static void phoneForgetConfirmed(void* /*userData*/) {
  if (oledRunCommand("phone forget")) oledToastShow("Forgetting phone", 2000);
}

// `phone pair new` stops the pipe and derives a key: on cmd_exec, never the
// display task. The command opens the code screen itself; a refusal comes
// back as a toast.
static void phonePairNewConfirmed(void* /*userData*/) {
  (void)oledRunCommandAsync("phone pair new", nullptr, nullptr);
}

static bool phoneInputHandler(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  const bool up = gNavEvents.up;
  if (oledScrollHandleNav(&sPhoneScroll)) {
    // The section label is not a stop: carry on past it the same way.
    const int idx = sPhoneScroll.selectedIndex;
    if (idx >= 0 && idx < sPhoneScroll.itemCount && sPhoneActions[idx] == PH_ACT_LABEL) {
      if (up) oledScrollUp(&sPhoneScroll); else oledScrollDown(&sPhoneScroll);
    }
    return true;
  }
  if (!INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) && !INPUT_CHECK(newlyPressed, INPUT_BUTTON_X))
    return false;  // B: global pop back to the Bluetooth menu
  const int idx = sPhoneScroll.selectedIndex;
  if (idx < 0 || idx >= sPhoneScroll.itemCount) return true;
  const BleLinkStatus& s = sPhoneShown;
  const bool ready = phoneReady(s);
  switch (sPhoneActions[idx]) {
    case PH_ACT_PIPE:  // the pipe on the shared stack; a paired phone is connected on open
      if (btPipeRunning(s)) { (void)oledRunCommand("closephone"); break; }
      if (oledRunCommand("openphone"))
        oledToastShow(s.pipePaired ? "Connecting phone" : "Phone open; pair it", 2000);
      break;
    case PH_ACT_AUTO:  // the direction the row showed
      (void)oledRunCommand(s.pipeAutoReconnect ? "bleautoreconnect phone off"
                                           : "bleautoreconnect phone on");
      break;
    case PH_ACT_LABEL:
      break;
    case PH_ACT_PAIR:  // opens a 60 s enrollment window (starts a stopped pipe)
      if (ready) oledToastShow("Disconnect first", 2000);
      else if (oledRunCommand("phone pair")) oledToastShow("Pairing open 60s", 2500);
      break;
    case PH_ACT_PAIR_NEW:  // replaces the secret, so it asks first (defaults to No)
      (void)oledConfirmRequest("New pairing code?", "Replaces the secret", phonePairNewConfirmed, nullptr, false);
      break;
    case PH_ACT_FORGET:
      if (!s.pipePaired) oledToastShow("No phone paired", 2000);
      else oledConfirmRequest("Forget phone?", "Pair again after", phoneForgetConfirmed,
                              nullptr, false);
      break;
  }
  return true;
}

// ---- OLED_BLUETOOTH_PHONE_CODE: `phone pair new`'s code -----------------------
// The time left, then the code as XXXX-XXXX on two large rows; nothing else
// draws it. Each frame copies it from the pipe owner into this stack and wipes
// that copy after drawing: never a toast, the OLED console or any history. B
// (or leaving the screen: the link watcher) closes the window.
static uint32_t sPairShownId = 0;  // the window this screen showed (display task)

static void displayPhonePairCode() {
  if (!oledDisplay) return;
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(SSD1306_WHITE);
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  char code[20];
  const PhonePairView v = phonePipePairView(code);
  const bool shown = v.state == PhonePairWindow::Open && v.onDisplay && code[0];
  // Only for the display session that issued it, checked here (outside the
  // owner's leaf lock): after a logout or another login the copy is wiped and
  // the window closed, never drawn for the next session.
  if (shown && !(v.source == SOURCE_LOCAL_DISPLAY &&
                 transportSessionEpochIsLive(SOURCE_LOCAL_DISPLAY, v.epoch))) {
    hw1code::wipe(code, sizeof(code));
    phonePipePairClose(v.id);
  } else if (shown) {
    sPairShownId = v.id;
    // At most 59:59 (the window is 3 min), so the line's length is bounded.
    const uint32_t ms = v.msLeft < 3599000u ? v.msLeft : 3599000u;
    const unsigned left = unsigned((ms + 999) / 1000);
    char line[22];
    snprintf(line, sizeof(line), "Pairing code  %u:%02u", left / 60, left % 60);
    oledDisplay->println(line);
    char half[10];  // "XXXX-XXXX": 108 px at size 2
    oledDisplay->setTextSize(2);
    for (int row = 0; row < 2; ++row) {
      memcpy(half, code + row * 10, 9);
      half[9] = '\0';
      oledDisplay->setCursor(0, OLED_CONTENT_START_Y + 8 + row * 16);
      oledDisplay->print(half);
    }
    oledDisplay->setTextSize(1);
    hw1code::wipe(half, sizeof(half));
    hw1code::wipe(code, sizeof(code));
    return;
  }
  const char* why = v.state == PhonePairWindow::Paired ? "Phone paired"
                  : v.state == PhonePairWindow::Expired ? "Code expired"
                  : v.state == PhonePairWindow::Closed ? "Pairing closed"
                  : "No code to show";
  oledDisplay->println(why);
  if (v.state == PhonePairWindow::Expired || v.state == PhonePairWindow::Closed) {
    oledDisplay->println();
    oledDisplay->println("New code and pair");
    oledDisplay->println("makes another one");
  }
}

// B closes the window and the global handler pops the screen; nothing else
// does anything here.
static bool phonePairCodeInput(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  if (!INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) return true;
  if (sPairShownId) phonePipePairClose(sPairShownId);
  return false;
}
#endif // ENABLE_BLE_PHONE_PIPE

// ---- Link watcher ------------------------------------------------------------
// The display redraws only when something marks it dirty, and a link coming up
// or dropping is not input. Compare a small signature each tick (bleLinkStatus()
// never blocks) and mark the display dirty when it changes: the menu markers,
// the G2 / R1 / Phone screens and the BT status detail all read these.
void oledBluetoothLinkWatch() {
  static bool seeded = false;
  static uint32_t lastSig = 0;
  static const char* lastPipeLabel = nullptr;
  const BleLinkStatus link = bleLinkStatus();
  // Role, saved role, pipe phase and console state, 4 bits each; then the
  // console's client count and the flags the rows show.
  uint32_t sig = uint32_t(link.role) | uint32_t(link.saved) << 4 |
                 uint32_t(link.pipe.phase) << 8 | uint32_t(link.console) << 12 |
                 uint32_t(link.consoleView.clients & 0xF) << 16;
  if (link.consoleView.up)          sig |= 1u << 20;
  if (link.consoleView.advertising) sig |= 1u << 21;
  if (link.pipePaired)              sig |= 1u << 22;
  if (link.pipeAutoReconnect)       sig |= 1u << 23;
#if ENABLE_G2_GLASSES
  if (isG2Connected())     sig |= 1u << 24;
  if (g2RingIsConnected()) sig |= 1u << 25;
#endif
  // The pipe's own static text, compared by pointer: a new failure reason.
  const char* pipeLabel = link.pipeLabel;
  uint32_t pairSig = 0;
#if ENABLE_BLE_PHONE_PIPE
  // `phone pair new`'s window lives while its code screen does: leaving the
  // screen (B, a jump elsewhere, a logout) closes it. The screen request from
  // cmd_exec gets 2 s to land. Its countdown redraws once a second.
  const PhonePairView pair = phonePipePairView(nullptr);
  if (pair.state == PhonePairWindow::Open && pair.onDisplay) {
    if (currentOLEDMode == OLED_BLUETOOTH_PHONE_CODE) sPairShownId = pair.id;
    else if (sPairShownId == pair.id || pair.ageMs >= 2000) phonePipePairClose(pair.id);
    pairSig = (pair.msLeft + 999) / 1000 + 1;  // the screen's seconds, never 0
  }
  pairSig = pairSig << 4 | uint32_t(pair.state);
#endif
  static uint32_t lastPairSig = 0;
  if (seeded && sig == lastSig && pipeLabel == lastPipeLabel && pairSig == lastPairSig) return;
  const bool changed = seeded;
  seeded = true;
  lastSig = sig;
  lastPipeLabel = pipeLabel;
  lastPairSig = pairSig;
  if (changed) oledMarkDirty();
}

// Availability check for Bluetooth OLED mode
static bool bluetoothOLEDModeAvailable(String* /*outReason*/) {
  return true;  // Always show in menu
}

// ---- Registration ----------------------------------------------------------
// Columns: mode, name, iconName, displayFunc, availFunc, inputFunc, showInMenu, menuOrder, hints
static const OLEDModeEntry bluetoothOLEDModes[] = {
  { OLED_BLUETOOTH,        "Bluetooth", "bt_idle", displayBluetoothStatus,
    bluetoothOLEDModeAvailable, bluetoothInputHandler, true,  45, "A:Select B:Back" },
  { OLED_BLUETOOTH_STATUS, "BT Status", "bt_idle", displayBluetoothStatusDetail,
    nullptr, nullptr,                                  false, -1, "B:Back" },
#if ENABLE_G2_GLASSES
  { OLED_BLUETOOTH_G2,        "G2",        "bt_idle", displayG2Menu,
    nullptr, g2InputHandler,                           false, -1, "A:Select B:Back" },
  { OLED_BLUETOOTH_G2_STATUS, "G2 Status", "bt_idle", displayG2StatusDetail,
    nullptr, nullptr,                                  false, -1, "B:Back" },
  { OLED_BLUETOOTH_R1,        "R1 Ring",   "bt_idle", displayR1Ring,
    nullptr, r1InputHandler,                           false, -1, "A:Select B:Back", r1OnEnter },
#endif
#if ENABLE_BLE_PHONE_PIPE
  { OLED_BLUETOOTH_PHONE,     "Phone",     "bt_idle", displayPhone,
    nullptr, phoneInputHandler,                        false, -1, "A:Select B:Back" },
  { OLED_BLUETOOTH_PHONE_CODE, "Pair Code", "bt_idle", displayPhonePairCode,
    nullptr, phonePairCodeInput,                       false, -1, "B:Close" },
#endif
};

REGISTER_OLED_MODE_MODULE(bluetoothOLEDModes, sizeof(bluetoothOLEDModes) / sizeof(bluetoothOLEDModes[0]), "Bluetooth");

// Linker anchor — with no external reference, --gc-sections drops this whole object
// file and the registrar above never runs, so OLED_BLUETOOTH renders "no render".
// printRegisteredOLEDModes() calls this no-op to force the file to be linked.
// (Mirrors oledMenuModeInit() in OLED_Mode_Menu.cpp.)
void oledBluetoothModeInit() {}

#endif // ENABLE_OLED_DISPLAY && ENABLE_BLUETOOTH
