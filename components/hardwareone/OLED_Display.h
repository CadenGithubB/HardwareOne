#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

#include <Arduino.h>
#include "System_BuildConfig.h"
#include "System_Settings.h"   // gSettings.oledBus — used by the OLED_TRANSACTION macro

// =============================================================================
// OLED WRAPPER FUNCTIONS - Safe to call regardless of ENABLE_OLED_DISPLAY
// =============================================================================
// These functions can be called without guards - they compile to no-ops when disabled

// Update boot progress (percent 0-100 and label string)
void oledSetBootProgress(int percent, const char* label);

// Update OLED display if connected and enabled
void oledUpdate();

// Per-loop tick that idle-logs-out the local-display (OLED) session after the
// configured window (gSettings.sessionIdleDisplay). No-op stub in non-OLED
// builds. See OLED_Utils.cpp for the policy.
void localDisplaySessionTick();

// Initialize OLED early in boot sequence
void oledEarlyInit();

// Apply OLED settings (brightness, etc.) from gSettings
void oledApplySettings();

// Apply OLED brightness from gSettings (can be called independently)
void applyOLEDBrightness();

// Apply OLED rotation (oledFlipped) from gSettings — calls setRotation + clears
// the buffer so the next render tick repaints in the new orientation.
void applyOLEDRotation();

// Sleep lifecycle. On boards where the OLED rides an I2C bus with a
// software-controllable power rail (I2C2_POWER_PIN), oledPrepareForSleep()
// sends the SSD1306 software-off command and then drops the rail so the
// chip loses Vcc — true power-off, not just panel-dark. oledResumeFromSleep()
// raises the rail, waits for the LDO to settle, re-initialises the SSD1306
// (state was lost on the power cycle), and reapplies rotation + brightness.
// On boards where the OLED is on the always-on bus, these are equivalent to
// the existing oledDisplayOff/On so callers don't have to branch.
void oledPrepareForSleep();
void oledResumeFromSleep();

// Notify OLED UI that local display auth state changed (login/logout)
void oledNotifyLocalDisplayAuthChanged();

// Returns true if the display should be blocked pending authentication.
bool shouldBlockForDisplayAuth();

// Display power control (abstracted from hardware-specific commands)
void oledDisplayOff();
void oledDisplayOn();
void oledShowSleepScreen(int seconds);

// =============================================================================
// FULL OLED IMPLEMENTATION - Only included when ENABLE_OLED_DISPLAY=1
// =============================================================================
#if ENABLE_OLED_DISPLAY

#include "HAL_Display.h"

// Animation system types and globals
enum OLEDAnimationType {
  ANIM_BOUNCE,
  ANIM_WAVE,
  ANIM_SPINNER,
  ANIM_MATRIX,
  ANIM_STARFIELD,
  ANIM_PLASMA,
  ANIM_FIRE,
  ANIM_GAME_OF_LIFE,
  ANIM_RADAR,
  ANIM_WAVEFORM,
  ANIM_SCROLLTEST,
  ANIM_BOOT_PROGRESS
};
struct OLEDAnimation {
  const char* name;
  OLEDAnimationType type;
  void (*renderFunc)();
  const char* description;
};
extern OLEDAnimationType currentAnimation;
extern const OLEDAnimation gAnimationRegistry[];
extern const int gAnimationCount;

// OLED Display Modes
enum OLEDMode {
  OLED_OFF,
  OLED_MENU,           // Main menu with app icons
  OLED_SENSOR_MENU,    // Sensor submenu
  OLED_SYSTEM_STATUS,
  OLED_SENSOR_DATA,
  OLED_SENSOR_LIST,
  OLED_THERMAL_VISUAL,
  OLED_NETWORK_INFO,
  OLED_MESH_STATUS,
  OLED_CUSTOM_TEXT,
  OLED_UNAVAILABLE,
  OLED_LOGO,
  OLED_ANIMATION,
  OLED_BOOT_SENSORS,
  OLED_IMU_ACTIONS,
  OLED_GPS_DATA,
  OLED_FM_RADIO,
  OLED_FILE_BROWSER,
  OLED_AUTOMATIONS,    // Automation status view
  OLED_ESPNOW,         // ESP-NOW peer status view
  OLED_TOF_DATA,       // ToF distance sensor view
  OLED_APDS_DATA,      // APDS color/proximity/gesture view
  OLED_POWER,          // Power options main menu
  OLED_POWER_CPU,      // CPU frequency submenu
  OLED_POWER_SLEEP,    // Sleep/restart submenu
  OLED_GAMEPAD_VISUAL, // Gamepad button/joystick visualization
  OLED_BLUETOOTH,      // Bluetooth connection and message view
  OLED_MEMORY_STATS,   // Memory/heap/PSRAM usage statistics
  OLED_REMOTE_SENSORS, // Remote sensor data from ESP-NOW mesh workers
  OLED_WEB_STATS,      // Web server statistics (connections, failed logins, etc)
  OLED_SETTINGS,       // Settings editor with visual slider/dial controls
  OLED_CLI_VIEWER,     // CLI output viewer (read-only console)
  OLED_CLI_INPUT,      // CLI command input (keyboard + last output lines)
  OLED_LOGGING,        // Logging control and viewer (sensor + system logs)
  OLED_LOGIN,          // Login screen for OLED authentication
  OLED_LOGOUT,         // Logout confirmation screen
  OLED_QUICK_SETTINGS, // Quick settings panel (WiFi, Bluetooth, HTTP server toggles)
  OLED_GPS_MAP,        // GPS map view with offline maps
  OLED_MICROPHONE,     // PDM microphone VU meter and recording
  OLED_RTC_DATA,       // RTC date/time/temperature display
  OLED_PRESENCE_DATA,  // STHS34PF80 IR presence/motion sensor view
  OLED_SPEECH,         // ESP-SR speech recognition status and control
  OLED_REMOTE,         // Remote device UI (paired mode only)
  OLED_UNIFIED_MENU,   // Unified local+remote actions menu
  OLED_REMOTE_SETTINGS,// Remote device settings editor (paired mode only)
  OLED_SET_PATTERN,    // Set gamepad pattern password
  OLED_CHANGE_PASSWORD,// Change password for authenticated user
  OLED_NOTIFICATIONS,  // Notification history viewer
  OLED_LLM,            // On-device LLM chat
  OLED_NETWORK_STATUS,    // WiFi status detail (pushed from OLED_NETWORK_INFO)
  OLED_NETWORK_WIFI_MENU, // WiFi management submenu (pushed from OLED_NETWORK_INFO)
  OLED_NETWORK_WIFI_LIST, // Saved-network list → connect (pushed from WIFI_MENU)
  OLED_NETWORK_WIFI_REMOVE, // Saved-network list → confirm + delete (pushed from WIFI_MENU)
  OLED_NETWORK_WIFI_SCAN, // Scanned-network list → password + add (pushed from WIFI_MENU)
  OLED_SPEECH_STATUS,     // ESP-SR live status detail (pushed from OLED_SPEECH)
  OLED_BLUETOOTH_STATUS,  // BT status detail (pushed from OLED_BLUETOOTH)
  OLED_BLUETOOTH_G2,      // G2 glasses submenu (pushed from OLED_BLUETOOTH)
  OLED_BLUETOOTH_G2_STATUS // G2 glasses status detail (pushed from OLED_BLUETOOTH_G2)
};

// Menu item structure for OLED menu (legacy - kept for compatibility)
struct OLEDMenuItem {
  const char* name;      // Display name
  const char* iconName;  // Icon name from embedded icons
  OLEDMode targetMode;   // Mode to switch to when selected
};

// Extended menu item for dynamic menus (supports remote items and submenus)
struct OLEDMenuItemEx {
  char name[24];         // Display name (copied, not pointer)
  char iconName[24];     // Icon name (copied)
  char command[48];      // CLI command to execute (for remote items)
  OLEDMode targetMode;   // Mode to switch to (for local items, OLED_OFF for command items)
  bool isRemote;         // True if this is a remote item
  bool isSubmenu;        // True if this opens a submenu
  bool needsInput;       // True if command requires user input (parsed from help at load time)
  char submenuId[16];    // Submenu identifier (e.g., "sensors", "network", "system")
};

// Maximum dynamic menu items (local + remote)
#define MAX_DYNAMIC_MENU_ITEMS 32

// Dynamic menu state
extern OLEDMenuItemEx gDynamicMenuItems[];
extern int gDynamicMenuItemCount;

// Build dynamic menu based on current DataSource (LOCAL/REMOTE/BOTH)
void buildDynamicMenu();

// Get filtered menu item count based on DataSource
int getFilteredMenuItemCount();

// ============================================================================
// Modular OLED Mode Registration System
// ============================================================================

// Function pointer types for OLED mode callbacks
typedef void (*OLEDDisplayFunc)();
typedef bool (*OLEDAvailabilityFunc)(String* outReason);  // Returns true if available
typedef bool (*OLEDInputFunc)(int deltaX, int deltaY, uint32_t newlyPressed);  // Returns true if input handled
// Optional per-mode entry hook. Fires once from requestOLEDMode() when this mode
// actually becomes current. `isForward` is true for forward navigation (menu
// select, CLI command, boot, login) and false for back-navigation (B button /
// popOLEDMode), so a mode can reset its view on a fresh visit but preserve state
// when the user returns to it. nullptr = no entry side-effects. This mirrors the
// onEnter/onExit lifecycle the CLI mode framework already uses (System_CLIMode.h)
// — the single place that owns "what happens when this mode is entered", instead
// of scattering resets across cmd_oledmode / the menu-select path.
typedef void (*OLEDModeEnterFunc)(bool isForward);

// OLED Mode Entry - defines a display mode that can be registered from any module
struct OLEDModeEntry {
  OLEDMode mode;              // The enum value for this mode
  const char* name;           // Display name for menu
  const char* iconName;       // Icon name for menu (from embedded icons)
  OLEDDisplayFunc displayFunc;      // Function to render this mode
  OLEDAvailabilityFunc availFunc;   // Function to check if mode is available (nullptr = always available)
  OLEDInputFunc inputFunc;          // Function to handle gamepad input (nullptr = default B=back, X=action)
  bool showInMenu;            // Whether to show in main menu
  int menuOrder;              // Order in menu (lower = earlier, -1 = end)
  const char* hints;          // Footer hints string (nullptr = use central switch fallback)
  OLEDModeEnterFunc onEnterFunc;  // Optional entry hook (nullptr = none). Trailing field:
                                  // existing 9-field initializers value-initialize it to nullptr.
};

// Maximum number of OLED modes that can be registered
// 47 enum values + sensor modules registering multiple entries each
#define MAX_OLED_MODES 64

// OLED Mode Registration Functions
void registerOLEDMode(const OLEDModeEntry* mode);
void registerOLEDModes(const OLEDModeEntry* modes, size_t count);
const OLEDModeEntry* findOLEDMode(OLEDMode mode);
const OLEDModeEntry* getOLEDModeByIndex(size_t index);
size_t getRegisteredOLEDModeCount();
void printRegisteredOLEDModes();  // Print summary of registered modes (call from setup)

// Auto-registration class for use in module files
class OLEDModeRegistrar {
public:
  OLEDModeRegistrar(const OLEDModeEntry* modes, size_t count, const char* moduleName);
};

// Macro for automatic registration in module files
// Uses token pasting with __LINE__ to generate unique variable names,
// allowing multiple registrations in the same translation unit.
#define _OLED_REG_CONCAT2(a, b) a##b
#define _OLED_REG_CONCAT(a, b) _OLED_REG_CONCAT2(a, b)
#define REGISTER_OLED_MODE_MODULE(modes, count, name) \
  static OLEDModeRegistrar _OLED_REG_CONCAT(_oled_mode_registrar_, __LINE__)(modes, count, name)

// ============================================================================
// Centralized Navigation Events (computed once per frame, use in inputFunc handlers)
// ============================================================================
// These are set by processOLEDInput() before calling any inputFunc handler.
// Use these instead of raw deltaX/deltaY to get proper debounce and auto-repeat.

struct NavEvents {
  bool up;          // Navigation up triggered (first deflection or auto-repeat)
  bool down;        // Navigation down triggered
  bool left;        // Navigation left triggered
  bool right;       // Navigation right triggered
  int  deltaX;      // Raw joystick X deflection (analog; 0 from non-joystick inputs)
  int  deltaY;      // Raw joystick Y deflection (analog; 0 from non-joystick inputs)
  int  wheelDelta;  // Signed rotary-encoder detent count this frame (0 from non-wheel inputs).
                    //   Wheel and joystick are SEPARATE signals — modes that want wheel
                    //   responsiveness read wheelDelta; modes that want joystick deflection
                    //   read deltaX/Y. Neither input device fakes the other's signal, so
                    //   modes don't have to know which physical hardware produced the input.
};

extern NavEvents gNavEvents;  // Global navigation events, updated each frame

// =============================================================================
// Data Source Selection (for paired mode)
// =============================================================================
// When paired with another device, controls whether menu items use local,
// remote, or both data sources. Toggled via Start button when paired+online.
enum class DataSource {
  LOCAL,   // Use only local sensors/data
  REMOTE,  // Use only remote sensors/data (via paired peer)
  BOTH     // Show both local and remote data (split view or merged)
};

extern DataSource gDataSource;           // Current global data source
extern bool gDataSourceIndicatorVisible; // Show source indicator in UI

// Cycle through available data sources (called on Start button when paired)
void oledCycleDataSource();

// Get display string for current source
const char* oledGetDataSourceLabel();

// Check if remote data source is available (paired + online)
bool oledRemoteSourceAvailable();

// Menu navigation state
extern int oledMenuSelectedIndex;
// Category menu system
extern int oledMenuCategorySelected;
extern int oledMenuCategoryItemIndex;
extern const OLEDMenuItem oledMenuCategories[];
extern const int oledMenuCategoryCount;
extern const OLEDMenuItem oledMenuCategory0[];
extern const int oledMenuCategory0Count;
extern const OLEDMenuItem oledMenuCategory1[];
extern const int oledMenuCategory1Count;
extern const OLEDMenuItem oledMenuCategory2[];
extern const int oledMenuCategory2Count;
extern const OLEDMenuItem oledMenuCategory3[];
extern const int oledMenuCategory3Count;
extern const OLEDMenuItem oledMenuCategory4[];
extern const int oledMenuCategory4Count;
extern const OLEDMenuItem oledMenuCategory5[];
extern const int oledMenuCategory5Count;

// Menu availability enum for checking if menu items are accessible
enum class MenuAvailability {
  AVAILABLE,
  FEATURE_DISABLED,      // Feature exists but turned off in settings
  UNINITIALIZED,         // Hardware exists but not initialized/detected
  NOT_BUILT,             // Feature not compiled in
  NOT_DETECTED           // Hardware not found
};

MenuAvailability getMenuAvailability(OLEDMode mode, String* outReason);

// =============================================================================
// Display Hardware Abstraction Layer
// =============================================================================
// Display dimensions and configuration are now defined in Display_HAL.h
// based on DISPLAY_TYPE in System_BuildConfig.h. This allows swapping
// between different display hardware at compile time.
//
// The following macros are provided by Display_HAL.h:
//   SCREEN_WIDTH, SCREEN_HEIGHT       - Display dimensions
//   OLED_FOOTER_HEIGHT                - Reserved footer area
//   OLED_CONTENT_HEIGHT               - Usable content area
//   DISPLAY_FG, DISPLAY_BG            - Default colors
//   DisplayDriver                     - Type alias for display class
// =============================================================================
#include "HAL_Display.h"

// OLED-specific configuration (I2C address defined in System_I2C.h as I2C_ADDR_OLED)
#define OLED_RESET -1
#define OLED_I2C_ADDRESS 0x3D

// Helper macro to wrap OLED operations in an I2C transaction.
// Requires System_I2C.h (i2cDeviceTransactionVoid) and System_Settings.h
// (gSettings.oledBus) in scope before use.
//
// Bus-aware: routes the per-bus mutex + clock to the OLED's configured bus
// (gSettings.oledBus), matching HAL_Display's render path. The Adafruit SSD1306
// object writes to its own TwoWire (the oledBus wire) internally regardless, so
// the previous 4-arg legacy form (implicit bus 0) took the WRONG bus's mutex
// when the OLED is on bus 1 (e.g. FeatherS3[D], I2C2) — fixed here.
#ifndef OLED_TRANSACTION
#define OLED_TRANSACTION(code) \
  i2cDeviceTransactionVoid((uint8_t)gSettings.oledBus, OLED_I2C_ADDRESS, 400000, 500, [&]() { code; })
#endif

// OLED display object (now provided by Display_HAL.h as gDisplay)
// Legacy alias: oledDisplay is defined as gDisplay in Display_HAL.h
// extern Adafruit_SSD1306* oledDisplay;  // Removed - use gDisplay from Display_HAL.h
extern bool oledConnected;
extern bool gOledEnabled;

// OLED State Variables (defined in oled_display.cpp)
extern OLEDMode currentOLEDMode;
void setOLEDMode(OLEDMode newMode);  // internal – prefer requestOLEDMode for external transitions

// Single authoritative mode transition entry point.
// Handles auth gating, back-nav stack push, and standardised debug logging.
// Use this for all user/external mode changes; keep setOLEDMode for pop/internal transitions.
// pushStack=false skips the back-nav push (for boot, system, or replace-in-place transitions).
// isBackNav: pass true ONLY for back-navigation (popOLEDMode() destinations) so the
// target mode's onEnterFunc receives isForward=false and can preserve its state.
void requestOLEDMode(OLEDMode newMode, const char* reason, bool pushStack = true, bool isBackNav = false);

// Slug <-> enum helpers (canonical CLI slugs; used by CLI, boot defaults, and Web selects).
// modeFromSlug returns (OLEDMode)-1 for unknown slugs.
OLEDMode     modeFromSlug(const String& slug);
const char*  slugFromMode(OLEDMode mode);

extern String customOLEDText;
extern unsigned long oledLastUpdate;
extern unsigned long animationFrame;

// (Network menu state flags removed — OLED_NETWORK_STATUS / OLED_NETWORK_WIFI_MENU
//  are now real pushed sub-modes; footer hints come from OLEDModeEntry::hints.)
extern unsigned long animationLastUpdate;
extern int animationFPS;

// Boot sequence state (defined in oled_display.cpp)
extern bool oledBootModeActive;
extern int bootProgressPercent;
extern String bootProgressLabel;

// OLED Initialization and Control
bool initOLEDDisplay();
void stopOLEDDisplay();
void updateOLEDDisplay();
void displayAnimation();
void displayConnectedSensors();

// ============================================================================
// OLED Change Detection - Skip rendering when nothing has changed
// ============================================================================
// Automatically detects changes via existing sequence counters:
// - seq: increments on any gamepad input
// - gSensorStatusSeq: increments on sensor state changes
// Call oledMarkDirty() only for non-sensor changes (menu state, settings, etc.)

void oledMarkDirty();              // Force next render (for non-sensor changes)
void oledMarkDirtyMode(OLEDMode mode);  // Force next render (compatibility)
bool oledIsDirty();                // Check if anything changed since last render
void oledClearDirty();             // Record current sequences after render
void oledSetAlwaysDirty(bool always);  // For animations that need constant refresh
void oledMarkDirtyUntil(unsigned long untilMs);  // Keep rendering until timestamp (for popup auto-dismiss)

// Boot sequence helpers (called from setup() and loop())
bool earlyOLEDInit();  // Probe and init OLED for boot animation
void processOLEDBootSequence();  // Handle boot phase transitions

// OLED command registry (for system_utils.cpp)
struct CommandEntry;
extern const CommandEntry oledCommands[];
extern const size_t oledCommandsCount;

// Display Mode Functions
void displayMenu();
void displayLogo();

// Power mode functions (OLED_Mode_Power.cpp)
// Navigation, actions, and input handling are now registered via REGISTER_OLED_MODE_MODULE.
// Display functions still declared for the render switch in updateOLEDDisplay().
void displayPower();
void displayPowerCPU();
void displayPowerSleep();

// Network mode functions (OLED_Mode_Network.cpp)
// All display/input/navigation registered via REGISTER_OLED_MODE_MODULE for:
//   OLED_NETWORK_INFO       — main menu (OLEDScrollState, Power-style)
//   OLED_NETWORK_STATUS     — WiFi status detail (pushed sub-mode)
//   OLED_NETWORK_WIFI_MENU  — WiFi management (pushed sub-mode, owns Add-WiFi keyboard flow)
//   OLED_MESH_STATUS, OLED_WEB_STATS, OLED_ESPNOW, OLED_REMOTE_SENSORS

// System mode functions (OLED_Mode_System.cpp)
void displaySystemStatus();
void displayMemoryStats();
void displayWebStats();
void displayCustomText();
void displayUnavailable();

// Sensor mode functions (OLED_Mode_Sensors.cpp)
void displaySensorData();
void displayConnectedSensors();
// displayIMUActions() - moved to i2csensor_bno055.cpp (modular OLED mode)
// displayFmRadio() - moved to fm_radio.cpp (modular OLED mode)
void displayFileBrowser();
void displayAutomations();
void displayEspNow();
// displayToFData() - moved to i2csensor_vl53l4cx.cpp (modular OLED mode)
#if ENABLE_APDS_SENSOR
void displayAPDSData();
#endif

// Menu navigation. The launcher's up/down/select now live in its registered
// inputFunc (mainMenuInputHandler) on an OLEDScrollState; oledMenuBack() remains
// the shared "B = pop the mode stack" helper used by the global input handler.
bool oledMenuBack();  // Returns true if handled (was in submenu)
void resetOLEDMenu();


// (The sensor menu's filter/sort is now folded into populateMenuScroll() in
// OLED_Mode_Menu.cpp — it no longer exposes sort/index helpers.)

// Mode stack navigation (for submenus and back navigation)
void pushOLEDMode(OLEDMode mode);
OLEDMode popOLEDMode();

// Input helper functions (for OLED UI components)
void updateInputState();
uint32_t getNewlyPressedButtons();
void getJoystickDelta(int& deltaX, int& deltaY);

// File browser state (defined in oled_display.cpp)
extern class FileManager* gOledFileManager;
extern bool oledFileBrowserNeedsInit;

// File browser navigation (defined in oled_file_browser.cpp)
void oledFileBrowserUp();
void oledFileBrowserDown();
void oledFileBrowserSelect();
void oledFileBrowserBack();
void resetOLEDFileBrowser();

// ============================================================================
// File picker — modal callback layer over OLED_Mode_FileBrowser
// ============================================================================
// Lets any OLED mode use the file browser as a *picker* instead of just a
// viewer. Caller fills out a FilePickerRequest, calls oledFilePickerPush(),
// then transitions to OLED_FILE_BROWSER via requestOLEDMode(). When the user
// picks a file (or cancels at root), the browser pops back to the requester
// mode and fires onPicked exactly once.
//
// While a picker is active the browser:
//   - shows req.title in place of the breadcrumb
//   - hides files where req.filter() returns false (folders always shown so
//     the user can navigate)
//   - on Enter on a file: fires onPicked(fullPath, cancelled=false), pops mode
//   - on Back at root: fires onPicked(nullptr, cancelled=true), pops mode
//
// While no picker is active the browser behaves as before — generic viewer.
// The .hwmap-load special case stays as a fallback for users who reach
// the file browser via Tools→Files instead of through a Map-mode picker.

#include "System_FileManager.h"  // pulls FileEntry in for the filter signature

struct FilePickerRequest {
  // Header text shown in place of the normal breadcrumb. Truncated to fit.
  char title[32];

  // Directory to navigate to on picker entry. Empty = "/".
  char startPath[FILE_MANAGER_MAX_PATH];

  // Optional filter — return true to show the entry, false to hide it.
  // Folders are ALWAYS shown regardless of this filter so the user can
  // navigate. nullptr = show all files.
  bool (*filter)(const FileEntry& entry);

  // Required. Called exactly once when the picker resolves:
  //   cancelled=false → user selected a file; fullPath is valid
  //   cancelled=true  → user backed out at root; fullPath is nullptr
  // Runs AFTER the mode pop, so currentOLEDMode == requesterMode when
  // the callback fires. The callback may freely call requestOLEDMode /
  // mutate state in the requester's locals.
  void (*onPicked)(const char* fullPath, bool cancelled);

  // The OLED mode that requested the picker. Stored so we can sanity-check
  // we're popping back to a valid mode (e.g. user didn't navigate away
  // mid-picker via global Home).
  OLEDMode requesterMode;
};

// Push a picker request. Caller is responsible for transitioning to the
// browser via requestOLEDMode(OLED_FILE_BROWSER, ...) after pushing. Only
// one picker can be active at a time; pushing while another is pending is
// rejected (returns false).
bool oledFilePickerPush(const FilePickerRequest& req);

// True between push and callback fire — useful for the browser's own
// state and for callers that want to suppress duplicate pushes.
bool oledFilePickerIsActive();

// Helper Functions (static in .cpp, declared here for internal use)
void rotateCubePoint(float& x, float& y, float& z, float angleX, float angleY, float angleZ);
void projectCubePoint(float x, float y, float z, int& screenX, int& screenY, int centerX, int centerY);

#endif // ENABLE_OLED_DISPLAY

// Stub macro when OLED is disabled - expands to nothing
#if !ENABLE_OLED_DISPLAY
#define REGISTER_OLED_MODE_MODULE(modes, count, name)
#endif

#endif // OLED_DISPLAY_H
