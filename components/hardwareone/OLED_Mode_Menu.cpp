// OLED_Mode_Menu.cpp - Menu display functions
// Extracted from OLED_Display.cpp for modularity

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "HAL_Input.h"
#include "System_Battery.h"
#include "System_Icons.h"
#include "System_Settings.h"
#include "System_BuildConfig.h"
#include <esp_app_desc.h>

#if ENABLE_OLED_DISPLAY

extern DisplayDriver* oledDisplay;

// Dynamic menu system
extern OLEDMenuItemEx gDynamicMenuItems[];
extern int gDynamicMenuItemCount;
extern void buildDynamicMenu();

// Remote submenu system
extern bool isInRemoteSubmenu();
extern OLEDMenuItemEx* getRemoteSubmenuItems();
extern int getRemoteSubmenuItemCount();
extern int getRemoteSubmenuSelection();
extern const char* getRemoteSubmenuId();

// Sensor menu state variables from OLED_Display.cpp
extern const OLEDMenuItem oledSensorMenuItems[];
extern const int oledSensorMenuItemCount;

// BatteryIconState declared in OLED_Utils.h (included above)

// External functions
extern float getBatteryPercentage();
extern char getBatteryIcon();
extern void drawIcon(DisplayDriver* display, const char* iconName, int x, int y, uint16_t color);
extern void drawIconScaled(DisplayDriver* display, const char* iconName, int x, int y, uint16_t color, float scale);
extern void enterUnavailablePage(const String& title, const String& reason);
extern void resetOLEDMenu();  // defined in OLED_Utils.cpp; called by the launcher onEnter hook

// ============================================================================
// Shared menu primitives (used by BOTH the sensor menu and the main launcher)
// ============================================================================
// These collapse the formerly hand-rolled list/menu systems onto OLEDScrollState:
//   * menuItemRightPaneDraw - availability badge + status text in the icon pane
//   * populateMenuScroll     - fill an OLEDScrollState from an OLEDMenuItem[]
//   * oledMenuExecuteItem    - availability-aware "select" dispatch
// Per-mode entry init for launched modes now runs via OLEDModeEntry::onEnterFunc,
// so the dispatch here stays tiny.

// Right-pane decorator for the SELECTED item: availability badge (D/X) in the
// top-left of the icon pane, and a status word (Ready/Off/No HW/N/A) below the
// icon. Reads the backing descriptor via item->userData. Ported verbatim from
// the old displaySensorMenu / displayMenuListStyle right-pane logic.
static void menuItemRightPaneDraw(Adafruit_SSD1306* d, OLEDScrollItem* item,
                                  int areaX, int iconY, int iconSize) {
  if (!d || !item || !item->userData) return;
  const OLEDMenuItem* mi = (const OLEDMenuItem*)item->userData;
  MenuAvailability avail = getMenuAvailability(mi->targetMode, nullptr);

  d->setTextSize(1);
  d->setTextColor(DISPLAY_COLOR_WHITE);

  if (avail != MenuAvailability::AVAILABLE) {
    d->setCursor(areaX + 2, OLED_CONTENT_START_Y);
    d->print(avail == MenuAvailability::FEATURE_DISABLED ? "D" : "X");
  }

  int textY = iconY + iconSize + 2;
  if (textY + 8 <= OLED_CONTENT_HEIGHT) {
    d->setCursor(areaX + 2, textY);
    switch (avail) {
      case MenuAvailability::AVAILABLE:        d->print("Ready"); break;
      case MenuAvailability::FEATURE_DISABLED: d->print("Off");   break;
      case MenuAvailability::NOT_DETECTED:     d->print("No HW"); break;
      case MenuAvailability::NOT_BUILT:        d->print("N/A");   break;
      default: break;
    }
  }
}

// Availability sort rank (lower = higher priority): Ready, then Off, then No-HW.
static int menuAvailRank(MenuAvailability a) {
  switch (a) {
    case MenuAvailability::AVAILABLE:        return 0;
    case MenuAvailability::FEATURE_DISABLED: return 1;
    case MenuAvailability::NOT_DETECTED:     return 2;
    default:                                 return 3;
  }
}

// Fill `s` from an OLEDMenuItem[] array: line1 = name, iconName = icon, and
// userData = the descriptor (so the right-pane callback + oledMenuExecuteItem
// can read targetMode). dropNotBuilt skips compiled-out entries; sortByAvailability
// orders Ready->Off->No-HW (the sensor-menu behavior). Preserves the cursor.
static void populateMenuScroll(OLEDScrollState* s, const OLEDMenuItem* items, int count,
                               bool sortByAvailability, bool dropNotBuilt) {
  oledScrollClearKeepSelection(s);

  int order[OLED_SCROLL_MAX_ITEMS];
  int n = 0;
  for (int i = 0; i < count && n < OLED_SCROLL_MAX_ITEMS; i++) {
    if (dropNotBuilt &&
        getMenuAvailability(items[i].targetMode, nullptr) == MenuAvailability::NOT_BUILT) {
      continue;
    }
    order[n++] = i;
  }

  if (sortByAvailability) {
    for (int a = 0; a < n - 1; a++) {
      for (int b = 0; b < n - a - 1; b++) {
        if (menuAvailRank(getMenuAvailability(items[order[b]].targetMode, nullptr)) >
            menuAvailRank(getMenuAvailability(items[order[b + 1]].targetMode, nullptr))) {
          int t = order[b]; order[b] = order[b + 1]; order[b + 1] = t;
        }
      }
    }
  }

  for (int k = 0; k < n; k++) {
    const OLEDMenuItem* mi = &items[order[k]];
    oledScrollAddItem(s, mi->name, nullptr, true, (void*)mi);
    s->items[s->itemCount - 1].iconName = mi->iconName;  // oledScrollAddItem doesn't set iconName
  }
  oledScrollClampSelection(s);
}

// Availability-aware "select": switch to an available item's target mode (that
// mode's own onEnterFunc handles its entry init), or push the current menu and
// show the unavailable page (so B returns here). `slug` is the trace reason.
static void oledMenuExecuteItem(const OLEDMenuItem* item, const char* slug) {
  if (!item) return;
  OLEDMode target = item->targetMode;
  String reason;
  MenuAvailability avail = getMenuAvailability(target, &reason);
  if (avail != MenuAvailability::AVAILABLE) {
    if (reason.length() == 0) {
      switch (avail) {
        case MenuAvailability::FEATURE_DISABLED: reason = "Disabled"; break;
        case MenuAvailability::NOT_DETECTED:     reason = "Not detected"; break;
        case MenuAvailability::NOT_BUILT:        reason = "Not built"; break;
        default:                                 reason = "Unavailable"; break;
      }
    }
    pushOLEDMode(currentOLEDMode);  // B from the unavailable page returns to this menu
    enterUnavailablePage(item->name, reason);
    return;
  }
  requestOLEDMode(target, slug);
}

// ============================================================================
// Main Menu Display (List Style - Only Option)
// ============================================================================

// Forward declare category arrays from OLED_Utils.cpp
extern const OLEDMenuItem oledMenuCategory0[], oledMenuCategory1[], oledMenuCategory2[];
extern const OLEDMenuItem oledMenuCategory3[], oledMenuCategory4[], oledMenuCategory5[];
extern const int oledMenuCategory0Count, oledMenuCategory1Count, oledMenuCategory2Count;
extern const int oledMenuCategory3Count, oledMenuCategory4Count, oledMenuCategory5Count;

// Helper to get category items and count
void getCategoryItems(int categoryId, const OLEDMenuItem** outItems, int* outCount) {
  switch (categoryId) {
    case 0: *outItems = oledMenuCategory0; *outCount = oledMenuCategory0Count; break;
    case 1: *outItems = oledMenuCategory1; *outCount = oledMenuCategory1Count; break;
    case 2: *outItems = oledMenuCategory2; *outCount = oledMenuCategory2Count; break;
    case 3: *outItems = oledMenuCategory3; *outCount = oledMenuCategory3Count; break;
    case 4: *outItems = oledMenuCategory4; *outCount = oledMenuCategory4Count; break;
    case 5: *outItems = oledMenuCategory5; *outCount = oledMenuCategory5Count; break;
    default: *outItems = nullptr; *outCount = 0; break;
  }
}

// ============================================================================
// Main Launcher - OLEDScrollState (categories -> category items)
// ============================================================================
// Two live levels: the top-level category list and one category's item list.
// The authoritative cursor/level still lives in the globals the header
// breadcrumb and resetOLEDMenu read (oledMenuSelectedIndex /
// oledMenuCategorySelected / oledMenuCategoryItemIndex); sMainScroll is a
// per-frame VIEW rebuilt from them, so those consumers keep working unchanged.
//
// (The old launcher also had a dynamic "remote submenu" path behind
// isInRemoteSubmenu(); that subsystem is currently unreachable - nothing calls
// buildRemoteSubmenu() - so the live launcher is just these two levels.)

static OLEDScrollState sMainScroll;
static bool sMainScrollInit = false;
static char sCatLabels[8][20];  // "<Category> >" decorated labels (persist for line1 ptrs)

static void mainMenuEnsureInit() {
  if (sMainScrollInit) return;
  oledScrollInit(&sMainScroll, nullptr, 4);
  oledScrollSetSplitPane(&sMainScroll, 68, 74, 32);
  sMainScrollInit = true;
}

// Rebuild sMainScroll for the current level and sync its cursor from the
// authoritative global.
static void mainMenuPopulate() {
  mainMenuEnsureInit();

  if (oledMenuCategorySelected < 0) {
    // Level 0 - categories. Decorate names with " >"; no availability badge.
    oledScrollClearKeepSelection(&sMainScroll);
    int maxLabels = (int)(sizeof(sCatLabels) / sizeof(sCatLabels[0]));
    int n = oledMenuCategoryCount < maxLabels ? oledMenuCategoryCount : maxLabels;
    for (int i = 0; i < n; i++) {
      snprintf(sCatLabels[i], sizeof(sCatLabels[i]), "%s >", oledMenuCategories[i].name);
      oledScrollAddItem(&sMainScroll, sCatLabels[i], nullptr, true, (void*)&oledMenuCategories[i]);
      sMainScroll.items[sMainScroll.itemCount - 1].iconName = oledMenuCategories[i].iconName;
    }
    sMainScroll.rightPaneDraw = nullptr;          // categories: icon only
    sMainScroll.selectedIndex = oledMenuSelectedIndex;
  } else {
    // Level 1 - the selected category's items, with availability badges.
    const OLEDMenuItem* items = nullptr;
    int count = 0;
    getCategoryItems(oledMenuCategorySelected, &items, &count);
    populateMenuScroll(&sMainScroll, items, count, /*sort=*/false, /*dropNotBuilt=*/false);
    sMainScroll.rightPaneDraw = menuItemRightPaneDraw;
    sMainScroll.selectedIndex = oledMenuCategoryItemIndex;
  }
  oledScrollClampSelection(&sMainScroll);
}

void displayMenuListStyle() {
  if (!oledDisplay || !oledConnected) return;
  mainMenuPopulate();
  oledScrollRender(oledDisplay, &sMainScroll, true, true, nullptr);
}

static bool mainMenuInputHandler(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  mainMenuPopulate();

  // Up/Down within the current level; mirror the cursor back to the global.
  if (oledScrollHandleNav(&sMainScroll)) {
    if (oledMenuCategorySelected >= 0) oledMenuCategoryItemIndex = sMainScroll.selectedIndex;
    else                               oledMenuSelectedIndex     = sMainScroll.selectedIndex;
    return true;
  }

  // A - descend into a category, or launch the selected item.
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    if (oledMenuCategorySelected < 0) {
      oledMenuSelectedIndex     = sMainScroll.selectedIndex;
      oledMenuCategorySelected  = sMainScroll.selectedIndex;  // category id == list position
      oledMenuCategoryItemIndex = 0;
    } else {
      OLEDScrollItem* sel = oledScrollGetSelected(&sMainScroll);
      if (sel && sel->userData) oledMenuExecuteItem((const OLEDMenuItem*)sel->userData, "menu.select");
    }
    return true;
  }

  // START - cycle data source when bonded (LOCAL/REMOTE/BOTH). SELECT->Quick
  // Settings is handled by the global handler now that the launcher uses the
  // normal registered-mode dispatch path.
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_START)) {
    if (oledRemoteSourceAvailable()) {
      oledCycleDataSource();
      return true;
    }
  }

  // B - leave a category submenu; at the top level let the global handler run
  // (there is nowhere above the category list to go).
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    if (oledMenuCategorySelected >= 0) {
      oledMenuCategorySelected  = -1;
      oledMenuCategoryItemIndex = 0;
      return true;   // cursor restores to oledMenuSelectedIndex on the next populate
    }
    return false;
  }

  return false;
}

// Entry hook: a fresh visit returns to the top category list (resetOLEDMenu);
// back-navigation preserves your place (isForward == false).
static void menuOnEnter(bool isForward) {
  if (isForward) resetOLEDMenu();
}

// ============================================================================
// Sensor Submenu Display
// ============================================================================

// ============================================================================
// Sensor Menu - OLEDScrollState (split-pane, availability badge + status)
// ============================================================================

static OLEDScrollState sSensorScroll;
static bool sSensorScrollInit = false;

static void sensorMenuPopulate() {
  if (!sSensorScrollInit) {
    oledScrollInit(&sSensorScroll, nullptr, 4);
    oledScrollSetSplitPane(&sSensorScroll, 78, 84, 32);
    sSensorScroll.rightPaneDraw = menuItemRightPaneDraw;
    sSensorScrollInit = true;
  }
  // Drop compiled-out sensors and sort Ready->Off->No-HW (the prior behavior).
  populateMenuScroll(&sSensorScroll, oledSensorMenuItems, oledSensorMenuItemCount,
                     /*sort=*/true, /*dropNotBuilt=*/true);
}

void displaySensorMenu() {
  if (!oledDisplay || !oledConnected) return;
  sensorMenuPopulate();
  oledScrollRender(oledDisplay, &sSensorScroll, true, true, nullptr);
}

static bool sensorMenuInputHandler(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  sensorMenuPopulate();
  if (oledScrollHandleNav(&sSensorScroll)) return true;

  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    OLEDScrollItem* sel = oledScrollGetSelected(&sSensorScroll);
    if (sel && sel->userData) {
      oledMenuExecuteItem((const OLEDMenuItem*)sel->userData, "sensormenu.select");
    }
    return true;
  }

  // B: return false so the global handler pops the mode stack (oledMenuBack()).
  return false;
}

// Columns: mode, name, iconName, displayFunc, availFunc, inputFunc, showInMenu, menuOrder, hints
static const OLEDModeEntry sSensorMenuModes[] = {
  { OLED_SENSOR_MENU, "Sensors", "sensor", displaySensorMenu, nullptr, sensorMenuInputHandler, false, -1, "A:Select B:Back" },
};

REGISTER_OLED_MODE_MODULE(sSensorMenuModes, sizeof(sSensorMenuModes) / sizeof(sSensorMenuModes[0]), "SensorMenu");

// ============================================================================
// Automations Display - Full implementation in OLED_Mode_Automations.cpp
// ============================================================================

// ============================================================================
// Logo Display (with 3D animated device)
// ============================================================================

extern void rotateCubePoint(float& x, float& y, float& z, float angleX, float angleY, float angleZ);
extern void projectCubePoint(float x, float y, float z, int& screenX, int& screenY, int centerX, int centerY);

void displayLogo() {
  // Text on the left
  oledDisplay->setTextSize(2);
  oledDisplay->setCursor(0, 10);
  oledDisplay->println("Hardware");
  oledDisplay->println("  One");
  oledDisplay->setTextSize(1);
  oledDisplay->setCursor(0, 44);
  oledDisplay->print("v");
  oledDisplay->println(esp_app_get_description()->version);

  // Animated 3D device model on the right
  static unsigned long animStartTime = 0;
  if (animStartTime == 0) animStartTime = millis();

  unsigned long elapsed = millis() - animStartTime;
  float animProgress = (elapsed % 4000) / 4000.0;

  float angleY = sin(animProgress * 2 * PI) * 0.25;
  float angleX = 0.15;
  float angleZ = 0;

  const int deviceX = 112;
  const int deviceY = 32;
  const float width = 12.5;
  const float height = 25.0;
  const float depth = 5.0;

  float vertices[8][3] = {
    { -width, -height, -depth },
    { width, -height, -depth },
    { width, height, -depth },
    { -width, height, -depth },
    { -width, -height, depth },
    { width, -height, depth },
    { width, height, depth },
    { -width, height, depth }
  };

  int projected[8][2];
  float rotated[8][3];
  for (int i = 0; i < 8; i++) {
    float x = vertices[i][0];
    float y = vertices[i][1];
    float z = vertices[i][2];

    rotateCubePoint(x, y, z, angleX, angleY, angleZ);
    rotated[i][0] = x;
    rotated[i][1] = y;
    rotated[i][2] = z;
    projectCubePoint(x, y, z, projected[i][0], projected[i][1], deviceX, deviceY);
  }

  // Face visibility helper
  auto isFaceVisible = [&](int v0, int v1, int v2) -> bool {
    float edge1[3] = {
      rotated[v1][0] - rotated[v0][0],
      rotated[v1][1] - rotated[v0][1],
      rotated[v1][2] - rotated[v0][2]
    };
    float edge2[3] = {
      rotated[v2][0] - rotated[v0][0],
      rotated[v2][1] - rotated[v0][1],
      rotated[v2][2] - rotated[v0][2]
    };

    float normal[3] = {
      edge1[1] * edge2[2] - edge1[2] * edge2[1],
      edge1[2] * edge2[0] - edge1[0] * edge2[2],
      edge1[0] * edge2[1] - edge1[1] * edge2[0]
    };

    return normal[2] > 0;
  };

  // Draw visible faces
  if (isFaceVisible(0, 1, 5)) {
    oledDisplay->drawLine(projected[0][0], projected[0][1], projected[1][0], projected[1][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[1][0], projected[1][1], projected[5][0], projected[5][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[5][0], projected[5][1], projected[4][0], projected[4][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[4][0], projected[4][1], projected[0][0], projected[0][1], DISPLAY_COLOR_WHITE);
  }

  if (isFaceVisible(3, 7, 6)) {
    oledDisplay->drawLine(projected[3][0], projected[3][1], projected[2][0], projected[2][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[2][0], projected[2][1], projected[6][0], projected[6][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[6][0], projected[6][1], projected[7][0], projected[7][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[7][0], projected[7][1], projected[3][0], projected[3][1], DISPLAY_COLOR_WHITE);
  }

  if (isFaceVisible(4, 5, 6)) {
    oledDisplay->drawLine(projected[4][0], projected[4][1], projected[5][0], projected[5][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[5][0], projected[5][1], projected[6][0], projected[6][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[6][0], projected[6][1], projected[7][0], projected[7][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[7][0], projected[7][1], projected[4][0], projected[4][1], DISPLAY_COLOR_WHITE);
  }

  if (isFaceVisible(0, 3, 2)) {
    oledDisplay->drawLine(projected[0][0], projected[0][1], projected[1][0], projected[1][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[1][0], projected[1][1], projected[2][0], projected[2][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[2][0], projected[2][1], projected[3][0], projected[3][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[3][0], projected[3][1], projected[0][0], projected[0][1], DISPLAY_COLOR_WHITE);
  }

  if (isFaceVisible(0, 4, 7)) {
    oledDisplay->drawLine(projected[0][0], projected[0][1], projected[4][0], projected[4][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[4][0], projected[4][1], projected[7][0], projected[7][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[7][0], projected[7][1], projected[3][0], projected[3][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[3][0], projected[3][1], projected[0][0], projected[0][1], DISPLAY_COLOR_WHITE);
  }

  if (isFaceVisible(1, 2, 6)) {
    oledDisplay->drawLine(projected[1][0], projected[1][1], projected[5][0], projected[5][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[5][0], projected[5][1], projected[6][0], projected[6][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[6][0], projected[6][1], projected[2][0], projected[2][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(projected[2][0], projected[2][1], projected[1][0], projected[1][1], DISPLAY_COLOR_WHITE);
  }

  // Add front panel details when visible
  float frontZ = depth * cos(angleY) * cos(angleX);
  float frontVisibility = cos(angleY);

  if (frontZ > 0 && frontVisibility > 0.7) {
    float screenVerts[4][3] = {
      { -width * 0.7f, -height * 0.9f, depth },
      { width * 0.7f, -height * 0.9f, depth },
      { width * 0.7f, -height * 0.5f, depth },
      { -width * 0.7f, -height * 0.5f, depth }
    };

    int screenProj[4][2];
    for (int i = 0; i < 4; i++) {
      float x = screenVerts[i][0];
      float y = screenVerts[i][1];
      float z = screenVerts[i][2];
      rotateCubePoint(x, y, z, angleX, angleY, angleZ);
      projectCubePoint(x, y, z, screenProj[i][0], screenProj[i][1], deviceX, deviceY);
    }

    oledDisplay->drawLine(screenProj[0][0], screenProj[0][1], screenProj[1][0], screenProj[1][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(screenProj[1][0], screenProj[1][1], screenProj[2][0], screenProj[2][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(screenProj[2][0], screenProj[2][1], screenProj[3][0], screenProj[3][1], DISPLAY_COLOR_WHITE);
    oledDisplay->drawLine(screenProj[3][0], screenProj[3][1], screenProj[0][0], screenProj[0][1], DISPLAY_COLOR_WHITE);

    // ToF sensor
    float tofX = -width * 0.4f;
    float tofY = height * 0.125f;
    float tofZ = depth;
    rotateCubePoint(tofX, tofY, tofZ, angleX, angleY, angleZ);
    int tofScreenX, tofScreenY;
    projectCubePoint(tofX, tofY, tofZ, tofScreenX, tofScreenY, deviceX, deviceY);
    oledDisplay->fillRect(tofScreenX - 2, tofScreenY - 1, 5, 3, DISPLAY_COLOR_BLACK);
    oledDisplay->drawRect(tofScreenX - 2, tofScreenY - 1, 5, 3, DISPLAY_COLOR_WHITE);

    // Thermal IR sensor
    float irX = width * 0.3f;
    float irY = height * 0.125f;
    float irZ = depth;
    rotateCubePoint(irX, irY, irZ, angleX, angleY, angleZ);
    int irScreenX, irScreenY;
    projectCubePoint(irX, irY, irZ, irScreenX, irScreenY, deviceX, deviceY);
    oledDisplay->fillCircle(irScreenX, irScreenY, 3, DISPLAY_COLOR_BLACK);
    oledDisplay->drawCircle(irScreenX, irScreenY, 3, DISPLAY_COLOR_WHITE);
  }
}

// ============================================================================
// Logo Mode Registration
// ============================================================================

static const OLEDModeEntry sLogoModes[] = {
  { OLED_MENU, "Menu", "menu", displayMenuListStyle, nullptr, mainMenuInputHandler, false, -1, "A:Select B:Back", menuOnEnter },
  { OLED_LOGO, "Logo", "logo", displayLogo,          nullptr, nullptr, false, -1, "B:Back" },
};

REGISTER_OLED_MODE_MODULE(sLogoModes, sizeof(sLogoModes) / sizeof(sLogoModes[0]), "MenuAndLogo");

// Linker anchor — called once from printRegisteredOLEDModes(). This file's only
// external reference used to be getCategoryItems() (called by the now-deleted
// oledMenuUp/Down/Select in OLED_Utils.cpp). With that gone, --gc-sections would
// drop this entire object file, so the SensorMenu + MenuAndLogo static registrars
// above would never run and OLED_MENU/OLED_LOGO/OLED_SENSOR_MENU would render
// "Mode N no render". An external call to this no-op keeps the file linked.
void oledMenuModeInit() {}

#endif // ENABLE_OLED_DISPLAY
