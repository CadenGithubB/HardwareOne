set(hw_cfg_file "${CMAKE_CURRENT_LIST_DIR}/System_BuildConfig.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${hw_cfg_file}")

# Default values used when the matching regex doesn't fire (file missing,
# unexpected formatting, etc.). The per-file `#if` guards inside each .cpp
# are the source of truth for runtime behaviour — these CMake gates only
# decide which files reach the compiler. A "0" default means "skip it"
# rather than "include it" so a parse failure is fail-safe (no spurious
# compile errors) and easy to spot in build output.
set(HW_CFG_NETWORK_FEATURE_LEVEL 0)
set(HW_CFG_WEB_FEATURE_LEVEL 0)
set(HW_CFG_I2C_FEATURE_LEVEL 0)
set(HW_CFG_DISPLAY_TYPE 0)
set(HW_CFG_ENABLE_MQTT 0)
set(HW_CFG_ENABLE_HTTPS 0)
set(HW_CFG_ENABLE_LLM 0)
set(HW_CFG_ENABLE_MAPS 0)
set(HW_CFG_ENABLE_AUTOMATION 0)
set(HW_CFG_ENABLE_EDGE_IMPULSE 0)
set(HW_CFG_ENABLE_GAMES 0)
set(HW_CFG_ENABLE_BLUETOOTH 0)
set(HW_CFG_ENABLE_G2_GLASSES 0)

if(EXISTS "${hw_cfg_file}")
    file(READ "${hw_cfg_file}" HW_CFG_CONTENT)

    # Helper-equivalent: extract `#define NAME <integer>` from the header
    # and bind it to the matching HW_CFG_* variable. Inline-repeated
    # because CMake macros over file scope add more rope than this is
    # worth for ~10 flags.
    string(REGEX MATCH "#define[ \t]+NETWORK_FEATURE_LEVEL[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_NETWORK_FEATURE_LEVEL "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+WEB_FEATURE_LEVEL[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_WEB_FEATURE_LEVEL "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+I2C_FEATURE_LEVEL[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_I2C_FEATURE_LEVEL "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+DISPLAY_TYPE[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_DISPLAY_TYPE "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+ENABLE_MQTT[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_ENABLE_MQTT "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+ENABLE_HTTPS[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_ENABLE_HTTPS "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+ENABLE_ONDEVICE_LLM[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_ENABLE_LLM "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+ENABLE_MAPS[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_ENABLE_MAPS "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+ENABLE_AUTOMATION[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_ENABLE_AUTOMATION "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+ENABLE_EDGE_IMPULSE[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_ENABLE_EDGE_IMPULSE "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+ENABLE_GAMES[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_ENABLE_GAMES "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+ENABLE_BLUETOOTH[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_ENABLE_BLUETOOTH "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+ENABLE_G2_GLASSES[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_ENABLE_G2_GLASSES "${CMAKE_MATCH_1}")
    endif()
endif()

set(HW_CFG_ENABLE_HTTP_SERVER 0)
if(HW_CFG_NETWORK_FEATURE_LEVEL GREATER_EQUAL 2 AND HW_CFG_WEB_FEATURE_LEVEL GREATER 0)
    set(HW_CFG_ENABLE_HTTP_SERVER 1)
endif()

# Coarse OLED gate: skip every OLED_*.cpp (except OLED_Utils.cpp, which
# carries the !ENABLE_OLED_DISPLAY stub implementations) when the user
# has no display hardware OR has disabled the I2C bus entirely. The
# per-feature .cpp guards (`#if ENABLE_OLED_DISPLAY && ...`) still apply
# inside each file when the bus + display are present but a sub-feature
# (ESPNOW, ESP_SR, BONDED_MODE) is off.
set(HW_CFG_BUILD_OLED_MODES 0)
if(HW_CFG_DISPLAY_TYPE GREATER 0 AND HW_CFG_I2C_FEATURE_LEVEL GREATER 0)
    set(HW_CFG_BUILD_OLED_MODES 1)
endif()

# G2 build gate. Matches the per-file `#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES`
# guards in each G2/R1 .cpp. When off we drop the protocol .cpp's from the
# source list entirely (System_G2_Protocol.cpp + System_R1_Protocol.cpp ≈
# 2.7 KLOC of source) so the build doesn't pay the parse + compile cost.
# The other G2 files (G2_Glasses, G2_Ring, G2_Page_*, G2_HijackFsm) stay
# in the unconditional list — their bodies are #if-wrapped so they compile
# to empty translation units quickly when disabled, and the linker dead-
# strips any symbols they leak.
set(HW_CFG_BUILD_G2 0)
if(HW_CFG_ENABLE_BLUETOOTH GREATER 0 AND HW_CFG_ENABLE_G2_GLASSES GREATER 0)
    set(HW_CFG_BUILD_G2 1)
endif()

set(hardwareone_srcs
        HardwareOne.cpp
        Bluetooth.cpp
        BLE_IDF.cpp
        G2_Glasses.cpp
        G2_Ring.cpp
        BLE_Events.cpp
        BLE_Peers.cpp
        G2_Page_Sensors.cpp
        G2_Page_Network.cpp
        G2_Page_Settings.cpp
        G2_Page_Files.cpp
        G2_Page_Power.cpp
        G2_Page_CameraSettings.cpp
        G2_Page_TestSuite.cpp
        G2_Page_TextEntry.cpp
        G2_Page_ESPNow.cpp
        G2_HijackFsm.cpp
        G2_HijackCmd.cpp
        System_Battery.cpp
        System_CLI.cpp
        System_Command.cpp
        System_Debug.cpp
        System_ESPNow.cpp
        System_ESPNow_Sensors.cpp
        System_FileManager.cpp
        System_Filesystem.cpp
        System_FirstTimeSetup.cpp
        System_I2C.cpp
        System_I2C_Manager.cpp
        System_Icons.cpp
        HAL_Input.cpp
        HAL_Display.cpp
        System_Mutex.cpp
        System_Power.cpp
        System_NeoPixel.cpp
        # OLED_Utils.cpp stays unconditional: it carries the
        # !ENABLE_OLED_DISPLAY stub implementations needed by every
        # consumer when DISPLAY_TYPE=0. The per-mode OLED_Mode_*.cpp
        # files (handled below) only appear when a display is actually
        # selected.
        OLED_Utils.cpp
        System_MemoryMonitor.cpp
        System_Hardware.cpp
        System_SensorLogging.cpp
        System_SensorStubs.cpp
        System_Settings.cpp
        System_SetupWizard.cpp
        System_FeatureRegistry.cpp
        System_Utils.cpp
        System_TaskUtils.cpp
        System_User.cpp
        System_WiFi.cpp
        System_Camera_DVP.cpp
        System_Camera_Video.cpp
        System_ImageManager.cpp
        System_ESPSR.cpp
        System_VFS.cpp
        System_Microphone.cpp
        System_SensorRegistry.cpp
        System_Notifications.cpp
        System_AuthIdentity.cpp
)

# ── Per-feature source lists (added below based on HW_CFG_* flags) ──
# Each block follows the existing MQTT/LLM pattern further down. These
# files all have internal `#if ENABLE_*` guards, so excluding them at
# CMake level is equivalent to compiling-and-getting-an-empty-.obj —
# only faster and cleaner in the build graph.

if(HW_CFG_BUILD_G2 GREATER 0)
    list(APPEND hardwareone_srcs
        System_G2_Protocol.cpp
        System_R1_Protocol.cpp
    )
    message(STATUS "G2 protocol (R1 ring + glasses wire format) ENABLED")
else()
    message(STATUS "G2 protocol DISABLED — saves ~2.7 KLOC of compile per build")
endif()

if(HW_CFG_ENABLE_MAPS GREATER 0)
    list(APPEND hardwareone_srcs System_Maps.cpp)
    message(STATUS "Maps support ENABLED")
else()
    message(STATUS "Maps support DISABLED (skipping System_Maps.cpp)")
endif()

if(HW_CFG_ENABLE_AUTOMATION GREATER 0)
    list(APPEND hardwareone_srcs System_Automation.cpp)
    message(STATUS "Automation support ENABLED")
else()
    message(STATUS "Automation support DISABLED (skipping System_Automation.cpp)")
endif()

if(HW_CFG_ENABLE_EDGE_IMPULSE GREATER 0)
    list(APPEND hardwareone_srcs System_EdgeImpulse.cpp)
    message(STATUS "Edge Impulse support ENABLED")
else()
    message(STATUS "Edge Impulse support DISABLED (skipping System_EdgeImpulse.cpp)")
endif()

if(HW_CFG_I2C_FEATURE_LEVEL GREATER 0)
    list(APPEND hardwareone_srcs
        i2csensor-apds9960.cpp
        i2csensor-pa1010d.cpp
        i2csensor-seesaw.cpp
        i2csensor-bno055.cpp
        i2csensor-mlx90640.cpp
        i2csensor-vl53l4cx.cpp
        i2csensor-rda5807.cpp
        i2csensor-ds3231.cpp
        i2csensor-sths34pf80.cpp
        i2csensor-pca9685.cpp
    )
    message(STATUS "I2C sensors INCLUDED (per-sensor flags inside each .cpp)")
else()
    message(STATUS "I2C sensors EXCLUDED (I2C_FEATURE_LEVEL=0)")
endif()

if(HW_CFG_BUILD_OLED_MODES GREATER 0)
    list(APPEND hardwareone_srcs
        OLED_Settings.cpp
        OLED_ESPNow.cpp
        OLED_SettingsEditor.cpp
        OLED_FirstTimeSetup.cpp
        OLED_Mode_Auth.cpp
        OLED_Mode_CLI.cpp
        OLED_Mode_CLIInput.cpp
        OLED_Mode_Logging.cpp
        OLED_Mode_Power.cpp
        OLED_Mode_Network.cpp
        OLED_Mode_System.cpp
        OLED_Mode_Sensors.cpp
        OLED_Mode_Animations.cpp
        OLED_Mode_Menu.cpp
        OLED_Mode_FileBrowser.cpp
        OLED_Mode_Speech.cpp
        OLED_Mode_Remote.cpp
        OLED_Mode_UnifiedMenu.cpp
        OLED_Mode_RemoteSettings.cpp
        OLED_Mode_SetPattern.cpp
        OLED_Mode_ChangePassword.cpp
        OLED_RemoteSettings.cpp
        OLED_UI.cpp
        OLED_SetupWizard.cpp
    )
    # Map / Automation OLED modes are gated on both the display AND
    # their feature flag. Layering the conditions here keeps the .cpp
    # internal guards as a redundant safety net rather than the only
    # source of truth.
    if(HW_CFG_ENABLE_MAPS GREATER 0)
        list(APPEND hardwareone_srcs OLED_Mode_Map.cpp)
    endif()
    if(HW_CFG_ENABLE_AUTOMATION GREATER 0)
        list(APPEND hardwareone_srcs OLED_Mode_Automations.cpp)
    endif()
    message(STATUS "OLED display modes INCLUDED (DISPLAY_TYPE=${HW_CFG_DISPLAY_TYPE})")
else()
    message(STATUS "OLED display modes EXCLUDED (DISPLAY_TYPE=0 or I2C_FEATURE_LEVEL=0)")
endif()

set(hardwareone_requires
        arduino
        hardwareone_libs
        bt
        esp_wifi
        esp32-camera
        fatfs
        sdmmc
        esp_driver_sdspi
)

if(HW_CFG_ENABLE_MQTT GREATER 0)
    list(APPEND hardwareone_srcs
            System_MQTT.cpp
    )
    list(APPEND hardwareone_requires mqtt)
    message(STATUS "MQTT support ENABLED")
else()
    message(STATUS "MQTT support DISABLED")
endif()

if(HW_CFG_ENABLE_HTTP_SERVER)
    list(APPEND hardwareone_srcs
            WebServer_Events.cpp
            WebServer_Server.cpp
            WebServer_Utils.cpp
            WebPage_Sensors.cpp
            WebPage_ESPNow.cpp
            WebPage_ESPNow_Metadata.cpp
            WebPage_Bond.cpp
            WebPage_Bluetooth.cpp
            WebPage_Speech.cpp
            WebPage_MQTT.cpp
            WebServer_MigrationTool.cpp
    )
    # Maps / Games web pages: tied to their feature flags. Their .cpp
    # files have `#if ENABLE_WEB_*` guards but skipping them at CMake
    # level avoids parsing 1000s of lines of guarded-out content on
    # every build.
    if(HW_CFG_ENABLE_MAPS GREATER 0)
        list(APPEND hardwareone_srcs WebPage_Maps.cpp)
    endif()
    if(HW_CFG_ENABLE_GAMES GREATER 0)
        list(APPEND hardwareone_srcs WebPage_Games.cpp)
    endif()
    list(APPEND hardwareone_requires esp_http_server)
    if(HW_CFG_ENABLE_HTTPS GREATER 0)
        list(APPEND hardwareone_requires esp_https_server)
        message(STATUS "HTTPS support ENABLED")
    endif()
endif()

if(HW_CFG_ENABLE_LLM GREATER 0)
    list(APPEND hardwareone_srcs System_LLM.cpp)
    list(APPEND hardwareone_requires espressif__esp-dsp)
    if(HW_CFG_ENABLE_HTTP_SERVER)
        list(APPEND hardwareone_srcs WebPage_LLM.cpp)
    endif()
    list(APPEND hardwareone_srcs OLED_Mode_LLM.cpp)
    message(STATUS "On-device LLM ENABLED")
else()
    message(STATUS "On-device LLM DISABLED")
endif()

idf_component_register(
    SRCS
        ${hardwareone_srcs}
    INCLUDE_DIRS
        .
    REQUIRES
        ${hardwareone_requires}
)

# Enable Sense features for ESP32-S3 boards (camera, mic, SD card on expansion board)
# This enables SD_CS_PIN and other Sense-specific defines in System_BuildConfig.h
if(CONFIG_IDF_TARGET_ESP32S3)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC XIAO_ESP32S3_SENSE_ENABLED=1)
    message(STATUS "XIAO ESP32S3 Sense features ENABLED (mic, SD card)")
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=format-truncation
    -Wno-error=maybe-uninitialized
)

# ============================================================================
# Build-time guard: no raw LittleFS file I/O outside System_VFS.cpp
# ============================================================================
# The permission refactor (Phases 1-6) routes every file read/write through
# VFS::*Guarded with an AuthContext, so the [PERM] DENY audit trail and the
# role-based rule table actually apply. This check fails the build if anyone
# reintroduces raw LittleFS.open / .exists / .remove / .rename / .mkdir /
# .rmdir outside System_VFS.cpp — stops regressions before flash instead of
# discovering them in an audit months later.
#
# Stat queries (totalBytes/usedBytes) and mount/format (begin/format) are
# NOT file I/O and stay allowed. See tools/check_no_raw_littlefs.py for the
# exact pattern + comment-line exclusion logic.
#
# Runs at CMake configure time — fires on every clean build / reconfigure.
# Incremental builds skip it (CMake doesn't reconfigure), so the guard
# catches new code as soon as someone adds it but doesn't slow rebuilds.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_LIST_DIR}/tools/check_no_raw_littlefs.py"
        "${CMAKE_CURRENT_LIST_DIR}"
    RESULT_VARIABLE _hw_littlefs_check_rc
    ERROR_VARIABLE  _hw_littlefs_check_err
)
if(NOT _hw_littlefs_check_rc EQUAL 0)
    # Surface the script's stderr verbatim — it already includes the file:line
    # offenders, the suggested fix, and the AuthContext picker. Wrapping in
    # FATAL_ERROR halts the build with a clear "configure failed" exit.
    message(FATAL_ERROR "${_hw_littlefs_check_err}")
endif()
