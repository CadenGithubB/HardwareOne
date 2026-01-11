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

// Initialize OLED early in boot sequence
void oledEarlyInit();

// Apply OLED settings (brightness, etc.) from gSettings
void oledApplySettings();

// Apply OLED brightness from gSettings (can be called independently)
void applyOLEDBrightness();

// Notify OLED UI that local display auth state changed (login/logout)
void oledNotifyLocalDisplayAuthChanged();

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
  OLED_LOGGING,        // Logging control and viewer (sensor + system logs)
  OLED_LOGIN,          // Login screen for OLED authentication
  OLED_LOGOUT,         // Logout confirmation screen
  OLED_QUICK_SETTINGS, // Quick settings panel (WiFi, Bluetooth, HTTP server toggles)
  OLED_GPS_MAP         // GPS map view with offline maps
};

// Menu item structure for OLED menu (legacy - kept for compatibility)
struct OLEDMenuItem {
  const char* name;      // Display name
  const char* iconName;  // Icon name from embedded icons
  OLEDMode targetMode;   // Mode to switch to when selected
};

// ============================================================================
// Modular OLED Mode Registration System
// ============================================================================

// Function pointer types for OLED mode callbacks
typedef void (*OLEDDisplayFunc)();
typedef bool (*OLEDAvailabilityFunc)(String* outReason);  // Returns true if available
typedef bool (*OLEDInputFunc)(int deltaX, int deltaY, uint32_t newlyPressed);  // Returns true if input handled

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
};

// Maximum number of OLED modes that can be registered
#define MAX_OLED_MODES 32

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
#define REGISTER_OLED_MODE_MODULE(modes, count, name) \
  static OLEDModeRegistrar _oled_mode_registrar(modes, count, name)

// ============================================================================
// Centralized Navigation Events (computed once per frame, use in inputFunc handlers)
// ============================================================================
// These are set by processGamepadMenuInput() before calling any inputFunc handler.
// Use these instead of raw deltaX/deltaY to get proper debounce and auto-repeat.

struct NavEvents {
  bool up;          // Navigation up triggered (first deflection or auto-repeat)
  bool down;        // Navigation down triggered
  bool left;        // Navigation left triggered
  bool right;       // Navigation right triggered
  int deltaX;       // Raw joystick X delta (for analog use cases)
  int deltaY;       // Raw joystick Y delta (for analog use cases)
};

extern NavEvents gNavEvents;  // Global navigation events, updated each frame

// Menu navigation state
extern int oledMenuSelectedIndex;
extern const OLEDMenuItem oledMenuItems[];
extern const int oledMenuItemCount;

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

// OLED display object (now provided by Display_HAL.h as gDisplay)
// Legacy alias: oledDisplay is defined as gDisplay in Display_HAL.h
// extern Adafruit_SSD1306* oledDisplay;  // Removed - use gDisplay from Display_HAL.h
extern bool oledConnected;
extern bool oledEnabled;

// OLED State Variables (defined in oled_display.cpp)
extern OLEDMode currentOLEDMode;
extern String customOLEDText;
extern unsigned long oledLastUpdate;
extern unsigned long animationFrame;

// Network menu state (for footer rendering)
extern bool networkShowingStatus;
extern bool networkShowingWiFiSubmenu;
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
// - gamepadSeq: increments on any gamepad input
// - gSensorStatusSeq: increments on sensor state changes
// Call oledMarkDirty() only for non-sensor changes (menu state, settings, etc.)

void oledMarkDirty();              // Force next render (for non-sensor changes)
void oledMarkDirtyMode(OLEDMode mode);  // Force next render (compatibility)
bool oledIsDirty();                // Check if anything changed since last render
void oledClearDirty();             // Record current sequences after render
void oledSetAlwaysDirty(bool always);  // For animations that need constant refresh

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
void displayPower();
void displayPowerCPU();
void displayPowerSleep();
void powerMenuUp();
void powerMenuDown();
void powerCpuUp();
void powerCpuDown();
void powerSleepUp();
void powerSleepDown();
void executePowerAction();
void executePowerCpuAction();
void executePowerSleepAction();
bool powerInputHandler(int deltaX, int deltaY, uint32_t newlyPressed);

// Network mode functions (OLED_Mode_Network.cpp)
void displayNetworkInfo();
void displayMeshStatus();
void networkMenuUp();
void networkMenuDown();
void executeNetworkAction();
void networkMenuBack();
bool networkInputHandler(int deltaX, int deltaY, uint32_t newlyPressed);

// System mode functions (OLED_Mode_System.cpp)
void displaySystemStatus();
void displayMemoryStats();
void displayWebStats();
void displayCustomText();
void displayUnavailable();

// Sensor mode functions (OLED_Mode_Sensors.cpp)
void displaySensorData();
void displayConnectedSensors();
// displayIMUActions() - moved to Sensor_IMU_BNO055.cpp (modular OLED mode)
// displayFmRadio() - moved to fm_radio.cpp (modular OLED mode)
void displayFileBrowser();
void displayAutomations();
void displayEspNow();
// displayToFData() - moved to Sensor_ToF_VL53L4CX.cpp (modular OLED mode)
void displayAPDSData();  // APDS still in oled_display.cpp (sensor disabled)

// Menu navigation functions
void oledMenuUp();
void oledMenuDown();
void oledMenuSelect();
void oledMenuBack();
void resetOLEDMenu();

// Mode stack navigation (for submenus and back navigation)
void pushOLEDMode(OLEDMode mode);
OLEDMode popOLEDMode();

// Input helper functions (for OLED UI components)
void updateInputState();
uint32_t getNewlyPressedButtons();
void getJoystickDelta(int& deltaX, int& deltaY);

// File browser state (defined in oled_display.cpp)
extern class FileManager* gOLEDFileManager;
extern bool oledFileBrowserNeedsInit;

// File browser navigation (defined in oled_file_browser.cpp)
void oledFileBrowserUp();
void oledFileBrowserDown();
void oledFileBrowserSelect();
void oledFileBrowserBack();
void resetOLEDFileBrowser();

// Helper Functions (static in .cpp, declared here for internal use)
void rotateCubePoint(float& x, float& y, float& z, float angleX, float angleY, float angleZ);
void projectCubePoint(float x, float y, float z, int& screenX, int& screenY, int centerX, int centerY);

#endif // ENABLE_OLED_DISPLAY

// Stub macro when OLED is disabled - expands to nothing
#if !ENABLE_OLED_DISPLAY
#define REGISTER_OLED_MODE_MODULE(modes, count, name)
#endif

#endif // OLED_DISPLAY_H
