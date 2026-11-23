#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

#include <Arduino.h>
#include "System_BuildConfig.h"

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
void applyOledBrightness();

// Apply OLED rotation (oledFlipped) from gSettings — calls setRotation + clears
// the buffer so the next render tick repaints in the new orientation.
void applyOledRotation();

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
// Declared centrally in System_User.h (included by OLED_Utils.h); retained
// here for display-only includers.
void oledNotifyLocalDisplayAuthChanged();

// Returns true if the display should be blocked pending authentication.
bool shouldBlockForDisplayAuth();

// `phone pair new` (phone link v2) shows its code only on the code screen.
// Ready: the command on this task came from the display UI and its session is
// live, and this build has the screen. Show: posts the screen to the display
// owner (any task). Both false without the display or the phone link.
bool oledPhonePairCodeSinkReady();
bool oledShowPhonePairCode();

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
enum OledAnimationType {
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
struct OledAnimation {
  const char* name;
  OledAnimationType type;
  void (*renderFunc)();
  const char* description;
};
extern OledAnimationType currentAnimation;
extern const OledAnimation gAnimationRegistry[];
extern const int gAnimationCount;

// OLED Display Modes
enum OledMode {
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
  OLED_WEB_STATS,      // Web menu: server start/stop, auto start, HTTPS, status
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
  OLED_REMOTE_SETTINGS,// Remote device settings editor (paired mode only)
  OLED_SET_PATTERN,    // Set gamepad pattern password
  OLED_CHANGE_PASSWORD,// Change password for authenticated user
  OLED_NOTIFICATIONS,  // Notification history viewer
  OLED_LLM,            // On-device LLM chat
  OLED_NETWORK_STATUS,    // WiFi status detail (pushed from OLED_NETWORK_INFO)
  OLED_NETWORK_WIFI_LIST, // Saved-network list → connect (pushed from OLED_NETWORK_INFO)
  OLED_NETWORK_WIFI_REMOVE, // Saved-network list → confirm + delete (pushed from OLED_NETWORK_INFO)
  OLED_NETWORK_WIFI_SCAN, // Scanned-network list → password + add (pushed from OLED_NETWORK_INFO)
  OLED_SPEECH_STATUS,     // ESP-SR live status detail (pushed from OLED_SPEECH)
  OLED_BLUETOOTH_STATUS,  // BT status detail (pushed from OLED_BLUETOOTH)
  OLED_BLUETOOTH_G2,      // G2 glasses submenu (pushed from OLED_BLUETOOTH)
  OLED_BLUETOOTH_G2_STATUS,// G2 glasses status detail (pushed from OLED_BLUETOOTH_G2)
  OLED_LED,                // NeoPixel LED control (color / effect / brightness)
  OLED_PERF_STATS,         // Live task CPU% / stack profiler (System & Diag)
  OLED_I2C_DIAG,           // I2C bus scan — hardware diagnostics (Hardware menu)
  OLED_BLUETOOTH_R1,       // R1 ring submenu — connect only (pushed from OLED_BLUETOOTH)
  OLED_R1_HEALTH,          // R1 Health — vitals / Poll / Track (ENABLE_R1_HEALTH)
  OLED_USER_MANAGER,       // Admin user manager — list / add / delete / role (Config menu)
  OLED_TRANSCRIPTION, OLED_TRANSCRIPTION_LIVE, OLED_TRANSCRIPTS, OLED_TRANSCRIPT_VIEW,
  OLED_BLUETOOTH_PHONE,    // Phone relay pipe submenu (pushed from OLED_BLUETOOTH)
  OLED_BLUETOOTH_PHONE_CODE, // `phone pair new` code screen (pushed by the command; never for a guest)
  OLED_WEB_STATUS,         // Web server status detail (pushed from OLED_WEB_STATS)
  OLED_CAMERA,             // One-bit live camera preview (append to preserve existing IDs)
};

// Guest OLED session helpers (view-only UX). Allowlist drives menu filter +
// requestOledMode gate; oledGuestBlocksMutate covers direct-API mutates.
// The guest and admin answers come from a per-session role cache (dropped at
// the session boundary, refilled on a gIdentityGeneration change); a failed
// lookup is guest and non-admin. Commands still check the role themselves.
bool oledIsGuestSession();
bool oledSessionIsAdmin();
bool oledModeAllowedForGuest(OledMode mode);
// True if guest — toast shown; callers should no-op the mutate.
bool oledGuestBlocksMutate();

// Menu item structure for OLED menu (legacy - kept for compatibility)
struct OledMenuItem {
  const char* name;      // Display name
  const char* iconName;  // Icon name from embedded icons
  OledMode targetMode;   // Mode to switch to when selected
};

// ============================================================================
// Modular OLED Mode Registration System
// ============================================================================

// Function pointer types for OLED mode callbacks
typedef void (*OledDisplayFunc)();
typedef bool (*OledAvailabilityFunc)(String* outReason);  // Returns true if available
typedef bool (*OledInputFunc)(int deltaX, int deltaY, uint32_t newlyPressed);  // Returns true if input handled
// Optional per-mode entry hook. Fires once from requestOledMode() when this mode
// actually becomes current. `isForward` is true for forward navigation (menu
// select, CLI command, boot, login) and false for back-navigation (B button /
// popOledMode), so a mode can reset its view on a fresh visit but preserve state
// when the user returns to it. nullptr = no entry side-effects. This mirrors the
// onEnter/onExit lifecycle the CLI mode framework already uses (System_CLIMode.h)
// — the single place that owns "what happens when this mode is entered", instead
// of scattering resets across cmd_oledmode / the menu-select path.
typedef void (*OledModeEnterFunc)(bool isForward);
// Optional per-mode header hook. oledRenderHeader fills an OledHeaderSpec
// (OLED_HeaderCore.h) with the default title (the mode name, or the menu,
// settings and file breadcrumbs) and hands it to the mode, which may set its
// own title (prefix/text/cut, or format into spec.buf), a short context such
// as a page counter ("3/12", dropped first when the bar is full) or hide the
// Power, Notify or Context item. nullptr = the defaults.
struct OledHeaderSpec;
typedef void (*OledHeaderFunc)(OledHeaderSpec& spec);

// OLED Mode Entry - defines a display mode that can be registered from any module
struct OledModeEntry {
  OledMode mode;              // The enum value for this mode
  const char* name;           // Display name for menu
  const char* iconName;       // Icon name for menu (from embedded icons)
  OledDisplayFunc displayFunc;      // Function to render this mode
  OledAvailabilityFunc availFunc;   // The mode's own entry rule (nullptr = none): false refuses entry
                                    // with *outReason. getMenuAvailability() and oledmode consult it.
  OledInputFunc inputFunc;          // Function to handle gamepad input (nullptr = default B=back, X=action)
  bool showInMenu;            // Not read: launcher rows come from the oledMenuCategory tables
  int menuOrder;              // Not read (see showInMenu)
  const char* hints;          // Footer hints string (nullptr = use central switch fallback)
  OledModeEnterFunc onEnterFunc;  // Optional entry hook (nullptr = none). Trailing field:
                                  // existing 9-field initializers value-initialize it to nullptr.
  OledHeaderFunc headerFunc;      // Optional header hook (nullptr = defaults). Trailing field,
                                  // value-initialized to nullptr by shorter initializers.
};

// Maximum number of OLED modes that can be registered
// 47 enum values + sensor modules registering multiple entries each
#define MAX_OLED_MODES 64

// OLED Mode Registration Functions
void registerOledMode(const OledModeEntry* mode);
void registerOledModes(const OledModeEntry* modes, size_t count);
const OledModeEntry* findOledMode(OledMode mode);
const OledModeEntry* getOledModeByIndex(size_t index);
size_t getRegisteredOledModeCount();
void printRegisteredOledModes();  // Print summary of registered modes (call from setup)

// Auto-registration class for use in module files
class OledModeRegistrar {
public:
  OledModeRegistrar(const OledModeEntry* modes, size_t count, const char* moduleName);
};

// Macro for automatic registration in module files
// Uses token pasting with __LINE__ to generate unique variable names,
// allowing multiple registrations in the same translation unit.
#define _OLED_REG_CONCAT2(a, b) a##b
#define _OLED_REG_CONCAT(a, b) _OLED_REG_CONCAT2(a, b)
#define REGISTER_OLED_MODE_MODULE(modes, count, name) \
  static OledModeRegistrar _OLED_REG_CONCAT(_oled_mode_registrar_, __LINE__)(modes, count, name)

// ============================================================================
// Centralized Navigation Events (computed once per frame, use in inputFunc handlers)
// ============================================================================
// These are set by processOledInput() before calling any inputFunc handler.
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

// Menu navigation state
extern int oledMenuSelectedIndex;
// Category menu system
extern int oledMenuCategorySelected;
extern int oledMenuCategoryItemIndex;
extern const OledMenuItem oledMenuCategories[];
extern const int oledMenuCategoryCount;
extern const OledMenuItem oledMenuCategory0[];
extern const int oledMenuCategory0Count;
extern const OledMenuItem oledMenuCategory1[];
extern const int oledMenuCategory1Count;
extern const OledMenuItem oledMenuCategory2[];
extern const int oledMenuCategory2Count;
extern const OledMenuItem oledMenuCategory3[];
extern const int oledMenuCategory3Count;
extern const OledMenuItem oledMenuCategory4[];
extern const int oledMenuCategory4Count;
extern const OledMenuItem oledMenuCategory5[];
extern const int oledMenuCategory5Count;
extern const OledMenuItem oledSensorMenuItems[];
extern const int oledSensorMenuItemCount;

// Menu availability enum for checking if menu items are accessible
enum class MenuAvailability {
  AVAILABLE,
  FEATURE_DISABLED,      // Feature exists but turned off in settings
  UNINITIALIZED,         // Hardware or link is there but not started; X on the unavailable page starts it
  NOT_BUILT,             // Feature not compiled in
  NOT_DETECTED           // Hardware not found
};

// The mode module's own entry rule (its registered OledModeEntry::availFunc).
// True when the mode registers no rule or its rule allows entry; on a refusal
// *outReason gets the module's reason.
bool oledModeRuleAllows(OledMode mode, String* outReason);

// Launcher availability: the build, hardware and feature state of the mode
// (not built, not detected, turned off), then the mode's own entry rule, whose
// refusal makes an otherwise available mode FEATURE_DISABLED with its reason.
// Whether a mode can be entered now, and why not: the build, the hardware and
// settings state (oledModeFeatureState) and the mode's own entry rule. The one
// question for every path that enters a mode: the launcher, oledmode and the
// saved default mode. Owner task only: the rule may read the display session.
// When the mode cannot be entered and outReason is given, *outReason is never
// left empty.
MenuAvailability getMenuAvailability(OledMode mode, String* outReason);

// The mode has something to draw in this build: a registered module with a
// display function, or OLED_OFF. The registry is filled before boot and never
// changes, so any task may ask. A mode that is not drawable is not in the build;
// entering it would leave the screen blank.
bool oledModeInBuild(OledMode mode);

// The unavailable page (OLED_UNAVAILABLE): what the launcher shows for a mode
// it cannot enter. The target mode and its availability class decide the
// hints and the X action; the title and the reason are only drawn.
// NOT_BUILT offers no action, UNINITIALIZED keeps the page up until the user
// starts the feature with X, the other classes return to the previous screen
// after a timeout.
void enterUnavailablePage(OledMode target, MenuAvailability availability,
                          const String& title, const String& reason);
extern OledMode unavailableOledTarget;
extern MenuAvailability unavailableOledAvailability;
extern String unavailableOledTitle;
extern String unavailableOledReason;
extern unsigned long unavailableOledStartTime;

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

// Address selected by the display HAL; retained for status output.
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
#define OLED_I2C_ADDRESS gDisplayI2cAddress
#endif

// Compatibility for setup screens that draw and flush in one transaction.
// Hardware lifecycle and controls use HAL_Display directly. Route legacy
// flushes through the same latched bus as the retained driver, even after a
// reboot-required bus setting is saved. Requires System_I2C.h in the caller.
#ifndef OLED_TRANSACTION
#if DISPLAY_TYPE == DISPLAY_TYPE_SSD1306
#define OLED_TRANSACTION(code) \
  do { if (displayIsReady()) { \
    i2cDeviceTransactionVoid(displayI2cBus(), OLED_I2C_ADDRESS, 400000, 500, [&]() { code; }); \
  } } while (0)
#else
#define OLED_TRANSACTION(code) do { if (displayIsReady()) { code; } } while (0)
#endif
#endif

// OLED display object (now provided by Display_HAL.h as gDisplay)
// Legacy alias: oledDisplay is defined as gDisplay in Display_HAL.h
// extern Adafruit_SSD1306* oledDisplay;  // Removed - use gDisplay from Display_HAL.h
extern bool oledConnected;
extern bool gOledRunning;

// OLED State Variables (defined in oled_display.cpp)
extern OledMode currentOledMode;
void setOledMode(OledMode newMode);  // internal – prefer requestOledMode for external transitions

// Single authoritative mode transition entry point.
// Handles auth gating, back-nav stack push, and standardised debug logging.
// Use this for all user/external mode changes; keep setOledMode for pop/internal transitions.
// pushStack=false skips the back-nav push (for boot, system, or replace-in-place transitions).
// isBackNav: pass true ONLY for back-navigation (popOledMode() destinations) so the
// target mode's onEnterFunc receives isForward=false and can preserve its state.
// Called off the OLED owner task (cmd_exec for CLI, web, MQTT, BLE and automation
// commands), it posts the request and returns; the owner applies it, gates and
// onEnter hook included, at the start of its next frame.
void requestOledMode(OledMode newMode, const char* reason, bool pushStack = true, bool isBackNav = false);

// requestOledMode() with a preparation step (a mode's own state reset) that runs
// on the owner task right before the transition. Same deferral off the owner.
void requestOledModePrepared(OledMode newMode, const char* reason, bool pushStack,
                             void (*prepare)());

// Slug <-> enum helpers (canonical CLI slugs; used by CLI, boot defaults, and Web selects).
// modeFromSlug returns (OledMode)-1 for unknown slugs.
OledMode     modeFromSlug(const String& slug);
// gSettings.oledDefaultMode as a mode: OLED_SYSTEM_STATUS for an unknown slug
// or a mode this build cannot draw (oledModeInBuild).
OledMode     savedDefaultOledMode();
const char*  slugFromMode(OledMode mode);

extern String customOledText;
extern unsigned long oledLastUpdate;
extern unsigned long animationFrame;

// (Network menu state flags removed — OLED_NETWORK_STATUS and the saved/scan
//  pickers are real pushed sub-modes; footer hints come from OledModeEntry::hints.)
extern unsigned long animationLastUpdate;
extern int animationFps;

// Boot sequence state (defined in oled_display.cpp)
extern bool oledBootModeActive;
extern int bootProgressPercent;
extern String bootProgressLabel;

// OLED Initialization and Control
bool initOledDisplay();
void stopOledDisplay();
void updateOledDisplay();
void displayAnimation();

// ============================================================================
// OLED Change Detection - Skip rendering when nothing has changed
// ============================================================================
// Automatically detects changes via existing sequence counters:
// - seq: increments on any gamepad input
// - gSensorStatusSeq: increments on sensor state changes
// Call oledMarkDirty() only for non-sensor changes (menu state, settings, etc.)

void oledMarkDirty();              // Force next render (for non-sensor changes)
void oledMarkDirtyMode(OledMode mode);  // Force next render (compatibility)
bool oledIsDirty();                // Check if anything changed since last render
void oledClearDirty();             // Record current sequences after render
void oledSetAlwaysDirty(bool always);  // For animations that need constant refresh
void oledMarkDirtyUntil(unsigned long untilMs);  // Keep rendering until timestamp (for popup auto-dismiss)

// CLI Output page (OLED_Mode_CLI.cpp): consulted by the dirty poll, the shared
// footer and the local-display session boundary.
bool oledCliViewerNeedsFrame();
const char* oledCliViewerFooterHint();
void oledCliViewerResetSessionState();

// Boot sequence helpers (called from setup() and loop())
bool earlyOledInit();  // Probe and init OLED for boot animation
void processOledBootSequence();  // Handle boot phase transitions

// Display Mode Functions
void displayMenu();
void displayLogo();

// Power mode functions (OLED_Mode_Power.cpp)
// Navigation, actions, and input handling are now registered via REGISTER_OLED_MODE_MODULE.
// Display functions still declared for the render switch in updateOledDisplay().
void displayPower();
void displayPowerCpu();
void displayPowerSleep();

// Network mode functions (OLED_Mode_Network.cpp)
// All display/input/navigation registered via REGISTER_OLED_MODE_MODULE for:
//   OLED_NETWORK_INFO       — WiFi menu (OledScrollState; owns the Add-WiFi keyboard flow)
//   OLED_NETWORK_STATUS     — WiFi status detail (pushed sub-mode)
//   OLED_WEB_STATS          — Web menu (server, auto start, HTTPS); OLED_WEB_STATUS detail
//   OLED_MESH_STATUS, OLED_ESPNOW, OLED_REMOTE_SENSORS

// System mode functions (OLED_Mode_System.cpp)
void displayCustomText();
void displayUnavailable();

// Sensor mode functions (OLED_Mode_Sensors.cpp)
void displaySensorData();
// displayImuActions() - moved to HAL_Motion.cpp (modular OLED mode)
// displayFmRadio() - moved to fm_radio.cpp (modular OLED mode)
void displayFileBrowser();
void displayAutomations();
void displayEspNow();
// displayTofData() - moved to i2csensor_vl53l4cx.cpp (modular OLED mode)
#if ENABLE_APDS_SENSOR
void displayApdsData();
#endif

// Menu navigation. The launcher's up/down/select now live in its registered
// inputFunc (mainMenuInputHandler) on an OledScrollState; oledMenuBack() remains
// the shared "B = pop the mode stack" helper used by the global input handler.
bool oledMenuBack();  // Returns true if handled (was in submenu)
void resetOledMenu();


// (The sensor menu's filter/sort is now folded into populateMenuScroll() in
// OLED_Mode_Menu.cpp — it no longer exposes sort/index helpers.)

// Mode stack navigation (for submenus and back navigation)
void pushOledMode(OledMode mode);
OledMode popOledMode();
void clearOledModeStack();

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

// ============================================================================
// File picker — modal callback layer over OLED_Mode_FileBrowser
// ============================================================================
// Lets any OLED mode use the file browser as a *picker* instead of just a
// viewer. Caller fills out a FilePickerRequest, calls oledFilePickerPush(),
// then transitions to OLED_FILE_BROWSER via requestOledMode(). When the user
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
  // Runs AFTER the mode pop, so currentOledMode == requesterMode when
  // the callback fires. The callback may freely call requestOledMode /
  // mutate state in the requester's locals.
  void (*onPicked)(const char* fullPath, bool cancelled);

  // The OLED mode that requested the picker. Stored so we can sanity-check
  // we're popping back to a valid mode (e.g. user didn't navigate away
  // mid-picker via global Home).
  OledMode requesterMode;
};

// Push a picker request. Caller is responsible for transitioning to the
// browser via requestOledMode(OLED_FILE_BROWSER, ...) after pushing. Only
// one picker can be active at a time; pushing while another is pending is
// rejected (returns false).
bool oledFilePickerPush(const FilePickerRequest& req);

// True between push and callback fire — useful for the browser's own
// state and for callers that want to suppress duplicate pushes.
bool oledFilePickerIsActive();

// Helper Functions (static in .cpp, declared here for internal use)
void rotateCubePoint(float& x, float& y, float& z, float angleX, float angleY, float angleZ);
void projectCubePoint(float x, float y, float z, int& screenX, int& screenY, int centerX, int centerY);

#else  // !ENABLE_OLED_DISPLAY

// The OLED is not in this build. Its state reads as absent, the boot sequence
// has no step, and a mode registration expands to nothing.
inline bool gOledRunning = false;
inline bool oledConnected = false;
inline void processOledBootSequence() {}
#define REGISTER_OLED_MODE_MODULE(modes, count, name)

#endif // ENABLE_OLED_DISPLAY

#endif // OLED_DISPLAY_H
