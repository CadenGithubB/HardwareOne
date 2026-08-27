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
set(HW_CFG_INPUT_DEVICE_TYPE 0)  # 0 = NONE; header values 1/2 select a driver
set(HW_CFG_ENABLE_MQTT 0)
set(HW_CFG_ENABLE_HTTPS 0)
set(HW_CFG_ENABLE_LLM_BACKEND 0)         # the LLM feature (registry/chat/commands/surfaces)
set(HW_CFG_ENABLE_LLM_SOURCE_ONBOARD 0)  # on-device PSRAM engine as a source
set(HW_CFG_ENABLE_LLM_SOURCE_CM5 0)      # CM5 co-processor as a source
set(HW_CFG_ENABLE_MAPS 0)
set(HW_CFG_ENABLE_AUTOMATION 0)
set(HW_CFG_ENABLE_EDGE_IMPULSE 0)
set(HW_CFG_ENABLE_GAMES 0)
set(HW_CFG_ENABLE_WEB_GAME_MAZE 1)
set(HW_CFG_ENABLE_WEB_GAME_DARKROOM 0)
set(HW_CFG_ENABLE_BLUETOOTH 0)
set(HW_CFG_ENABLE_G2_GLASSES 0)
set(HW_CFG_ENABLE_R1_HEALTH 0)
set(HW_CFG_ENABLE_G2_TESTSUITE 0)
set(HW_CFG_ENABLE_RASPBERRY_PI_HOST_POWER 0)
set(HW_CFG_ENABLE_RASPBERRY_PI_HOST_FAN 0)

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

    string(REGEX MATCH "#define[ \t]+INPUT_DEVICE_TYPE[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_INPUT_DEVICE_TYPE "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+ENABLE_MQTT[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_ENABLE_MQTT "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+ENABLE_HTTPS[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_ENABLE_HTTPS "${CMAKE_MATCH_1}")
    endif()

    # LLM: one feature flag plus one flag per answer SOURCE. Each must stay a
    # plain literal in the header — these regexes require whitespace then digits
    # straight after the name, so ENABLE_LLM_BACKEND cannot swallow a longer
    # name like ENABLE_LLM_BACKEND_FOO, and the three never alias each other.
    string(REGEX MATCH "#define[ \t]+ENABLE_LLM_BACKEND[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_ENABLE_LLM_BACKEND "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+ENABLE_LLM_SOURCE_ONBOARD[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_ENABLE_LLM_SOURCE_ONBOARD "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+ENABLE_LLM_SOURCE_CM5[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_ENABLE_LLM_SOURCE_CM5 "${CMAKE_MATCH_1}")
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

    string(REGEX MATCH "#define[ \t]+ENABLE_WEB_GAME_MAZE[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_ENABLE_WEB_GAME_MAZE "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+ENABLE_WEB_GAME_DARKROOM[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_ENABLE_WEB_GAME_DARKROOM "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+ENABLE_BLUETOOTH[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_ENABLE_BLUETOOTH "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+ENABLE_G2_GLASSES[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_ENABLE_G2_GLASSES "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+ENABLE_R1_HEALTH[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_ENABLE_R1_HEALTH "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+ENABLE_G2_TESTSUITE[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_ENABLE_G2_TESTSUITE "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+ENABLE_RASPBERRY_PI_HOST_POWER[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_ENABLE_RASPBERRY_PI_HOST_POWER "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+ENABLE_RASPBERRY_PI_HOST_FAN[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_ENABLE_RASPBERRY_PI_HOST_FAN "${CMAKE_MATCH_1}")
    endif()
endif()

# The active board's sdkconfig is ground truth for whether the Bluedroid stack
# exists — mirror of the System_BuildConfig.h derived rule. A board that drops
# the stack (boards/qtpy_esp32.defaults: CONFIG_BT_ENABLED=n) force-downgrades
# the app-side BT flag so this file's source-list filter matches what the
# per-file #if gates resolve to. CONFIG_* comes from the per-board sdkconfig;
# during early expansion it is unset, which only affects the early no-op pass.
if(NOT CONFIG_BT_ENABLED OR NOT CONFIG_BT_BLUEDROID_ENABLED)
    set(HW_CFG_ENABLE_BLUETOOTH 0)
endif()

# R1 Health / Pet / Tests need BT + G2 (matches System_BuildConfig.h derived rules).
if(NOT (HW_CFG_ENABLE_BLUETOOTH GREATER 0 AND HW_CFG_ENABLE_G2_GLASSES GREATER 0))
    set(HW_CFG_ENABLE_R1_HEALTH 0)
    set(HW_CFG_ENABLE_G2_TESTSUITE 0)
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
        System_BleSecureChannel.cpp
        BLE_IDF.cpp
        G2_Glasses.cpp
        G2_Ring.cpp
        BLE_CentralTx.cpp
        BLE_Events.cpp
        BLE_Peers.cpp
        G2_Page_Sensors.cpp
        G2_Page_Network.cpp
        G2_Page_Settings.cpp
        G2_Page_Files.cpp
        G2_Page_Power.cpp
        G2_Page_CameraSettings.cpp
        G2_Page_TextEntry.cpp
        G2_Page_ESPNow.cpp
        G2_Page_Users.cpp
        G2_Page_LED.cpp
        G2_Health.cpp
        R1_HealthHistoryStore.cpp
        G2_HijackFsm.cpp
        G2_HijackCmd.cpp
        System_Battery.cpp
        System_CLI.cpp
        System_CLIMode.cpp
        System_CLIConfirm.cpp
        System_SetupWizardMode.cpp
        System_Command.cpp
        System_CrashRecord.cpp
        System_OTA.cpp
        System_OTASafety.cpp
        System_Debug.cpp
        System_ESPNow.cpp
        System_ESPNow_Crypto.cpp
        System_ESPNow_Files.cpp
        System_ESPNow_FsList.cpp
        System_ESPNow_Handlers_Crypto.cpp
        System_ESPNow_Identity.cpp
        System_ESPNow_MeshKeys.cpp
        System_ESPNow_Router.cpp
        System_ESPNow_Saturation.cpp
        System_ESPNow_Sensors.cpp
        System_ESPNow_Sessions.cpp
        System_ESPNow_Tx.cpp
        System_FileManager.cpp
        System_Filesystem.cpp
        System_FirstTimeSetup.cpp
        System_I2C.cpp
        System_I2C_Manager.cpp
        System_PollPause.cpp
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
        System_MemUtil.cpp
        System_MemTracker.cpp
        System_MemoryMonitor.cpp
        System_Hardware.cpp
        System_SensorLogging.cpp
        System_CaptureCrypto.cpp
        System_SensorStubs.cpp
        System_RamFlush.cpp
        System_Settings.cpp
        System_SettingsEditorCore.cpp
        System_SetupWizard.cpp
        System_FeatureRegistry.cpp
        System_BootState.cpp
        System_TimeAnchors.cpp
        System_Utils.cpp
        System_TaskUtils.cpp
        System_User.cpp
        System_UartLink.cpp
        System_Cm5Presence.cpp
        System_LiveAudio.cpp
        System_WiFi.cpp
        System_Camera_DVP.cpp
        System_Camera_Video.cpp
        System_ImageManager.cpp
        System_ESPSR.cpp
        System_VFS.cpp
        System_Microphone.cpp
        System_Dictation.cpp
        HAL_Audio.cpp
        System_SensorRegistry.cpp
        System_Notifications.cpp
        System_EventCatalog.cpp
        System_EventCatalogJson.cpp
        System_Events.cpp
        System_AuthIdentity.cpp
        System_BondedPeer.cpp
        System_MeshPeers.cpp
        System_SelfDevice.cpp
        System_Clock.cpp
)

# cm5 power and cm5 fan share one module (System_Cm5HostControl.cpp) but keep
# independent flags, so the source is compiled when either one is on and the
# in-file guards drop whichever protocol is off.
if(HW_CFG_ENABLE_RASPBERRY_PI_HOST_POWER GREATER 0 OR HW_CFG_ENABLE_RASPBERRY_PI_HOST_FAN GREATER 0)
    list(APPEND hardwareone_srcs System_Cm5HostControl.cpp)
    message(STATUS "Raspberry Pi/CM5 host control ENABLED (power=${HW_CFG_ENABLE_RASPBERRY_PI_HOST_POWER} fan=${HW_CFG_ENABLE_RASPBERRY_PI_HOST_FAN})")
else()
    message(STATUS "Raspberry Pi/CM5 host control DISABLED (skipping System_Cm5HostControl.cpp)")
endif()

# ── Per-feature source lists (added below based on HW_CFG_* flags) ──
# Each block follows the existing MQTT/LLM pattern further down. These
# files all have internal `#if ENABLE_*` guards, so excluding them at
# CMake level is equivalent to compiling-and-getting-an-empty-.obj —
# only faster and cleaner in the build graph.

if(HW_CFG_BUILD_G2 GREATER 0)
    list(APPEND hardwareone_srcs
        System_G2_Protocol.cpp
        System_R1_Protocol.cpp
        # Compress-only LZ4 for the CompressMode=2 image path. G2-gated
        # because the image push is its only caller.
        System_Lz4.cpp
    )
    message(STATUS "G2 protocol (R1 ring + glasses wire format) ENABLED")
else()
    message(STATUS "G2 protocol DISABLED — saves ~2.7 KLOC of compile per build")
endif()

# Optional G2 lens page: its own TU with an internal
# `#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_G2_TESTSUITE` guard.
# It is reached only from a launcher row gated on the same flag, so dropping
# the file drops the whole feature — nothing else calls in.
if(HW_CFG_BUILD_G2 GREATER 0 AND HW_CFG_ENABLE_G2_TESTSUITE GREATER 0)
    list(APPEND hardwareone_srcs G2_Page_TestSuite.cpp)
    message(STATUS "G2 transport test bench ENABLED")
else()
    message(STATUS "G2 transport test bench EXCLUDED")
endif()

# Optional G2 lens page: Automations app. Needs the automation engine
# (System_Automation.cpp supplies AUTOMATIONS_JSON_FILE and the
# `automation ...` commands the taps submit), so it layers ENABLE_AUTOMATION
# on top of the G2 gate — G2-without-automation used to link-fail on the
# AUTOMATIONS_JSON_FILE extern. Internal guard + header stubs mirror
# G2_Page_TestSuite; its Apps row is gated on the same flag.
if(HW_CFG_BUILD_G2 GREATER 0 AND HW_CFG_ENABLE_AUTOMATION GREATER 0)
    list(APPEND hardwareone_srcs G2_Page_Automations.cpp)
    message(STATUS "G2 Automations app page ENABLED")
else()
    message(STATUS "G2 Automations app page EXCLUDED")
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
        i2csensor_apds9960.cpp
        i2csensor_pa1010d.cpp
        i2csensor_bno055.cpp
        i2csensor_mlx90640.cpp
        i2csensor_vl53l4cx.cpp
        i2csensor_rda5807.cpp
        i2csensor_ds3231.cpp
        i2csensor_sths34pf80.cpp
        i2csensor_pca9685.cpp
        i2csensor_max17048.cpp
    )
    # Input device: exactly one of the two drivers compiles in. Both own the
    # gamepadTask() function symbol so they would collide at link time if
    # both were included. INPUT_DEVICE_TYPE in System_BuildConfig.h selects.
    if(HW_CFG_INPUT_DEVICE_TYPE EQUAL 2)
        list(APPEND hardwareone_srcs i2csensor_ano_encoder.cpp)
        message(STATUS "Input device: ANO rotary encoder")
    elseif(HW_CFG_INPUT_DEVICE_TYPE EQUAL 1)
        list(APPEND hardwareone_srcs i2csensor_seesaw.cpp)
        message(STATUS "Input device: Seesaw mini gamepad")
    else()
        message(STATUS "Input device: none")
    endif()
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
        OLED_Mode_LED.cpp
        OLED_Mode_Network.cpp
        OLED_Mode_System.cpp
        OLED_Mode_Sensors.cpp
        OLED_Mode_Animations.cpp
        OLED_Mode_Menu.cpp
        OLED_Mode_FileBrowser.cpp
        OLED_Mode_Speech.cpp
        OLED_Mode_Bluetooth.cpp
        OLED_Mode_Remote.cpp
        OLED_Mode_UnifiedMenu.cpp
        OLED_Mode_RemoteSettings.cpp
        OLED_Mode_SetPattern.cpp
        OLED_Mode_ChangePassword.cpp
        OLED_Mode_UserManager.cpp
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
    if(HW_CFG_ENABLE_R1_HEALTH GREATER 0)
        list(APPEND hardwareone_srcs OLED_Mode_R1_Health.cpp)
    endif()
    message(STATUS "OLED display modes INCLUDED (DISPLAY_TYPE=${HW_CFG_DISPLAY_TYPE})")
else()
    message(STATUS "OLED display modes EXCLUDED (DISPLAY_TYPE=0 or I2C_FEATURE_LEVEL=0)")
endif()

set(hardwareone_requires
        arduino
        hardwareone_libs
        hw1_ota_protocol
        bt
        nvs_flash
        app_update
        bootloader_support
        esp_app_format
        json
        mbedtls
        esp_wifi
        esp32-camera
        fatfs
        sdmmc
        esp_driver_sdspi
        espressif__libsodium
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
            WebPage_Battery.cpp
            WebServer_MigrationTool.cpp
    )
    # Maps / Games web pages: tied to their feature flags. Their .cpp
    # files have `#if ENABLE_WEB_*` guards but skipping them at CMake
    # level avoids parsing 1000s of lines of guarded-out content on
    # every build.
    if(HW_CFG_ENABLE_MAPS GREATER 0)
        list(APPEND hardwareone_srcs WebPage_Maps.cpp)
    endif()
    if(HW_CFG_ENABLE_R1_HEALTH GREATER 0)
        list(APPEND hardwareone_srcs WebPage_R1_Health.cpp)
    endif()
    if(HW_CFG_ENABLE_GAMES GREATER 0 AND HW_CFG_ENABLE_WEB_GAME_MAZE GREATER 0)
        list(APPEND hardwareone_srcs WebPage_Games.cpp)
    endif()
    if(HW_CFG_ENABLE_GAMES GREATER 0 AND HW_CFG_ENABLE_WEB_GAME_DARKROOM GREATER 0)
        list(APPEND hardwareone_srcs WebPage_DarkRoom.cpp)
    endif()
    list(APPEND hardwareone_requires esp_http_server)
    if(HW_CFG_ENABLE_HTTPS GREATER 0)
        list(APPEND hardwareone_requires esp_https_server)
        message(STATUS "HTTPS support ENABLED")
    endif()
endif()

# ---------------------------------------------------------------------------
# LLM: one feature, N sources.
#
#   ENABLE_LLM_BACKEND        the feature — model registry, conversation layer,
#                             CLI commands, and every UI surface.
#   ENABLE_LLM_SOURCE_ONBOARD the on-device PSRAM inference engine, as ONE source
#                             of answers. Its weight is the engine TUs below plus
#                             the esp-dsp dependency.
#   ENABLE_LLM_SOURCE_CM5     the CM5 co-processor as another source (no TU of its
#                             own yet — lands with the backend registry).
#
# Illegal combinations are rejected by #error in System_BuildConfig.h, not here;
# CMake only decides which files reach the compiler. Surfaces follow the feature
# flag plus whatever capability flag they already depended on, which is the §5
# "USER-FACING APPS" convention in the header.
# ---------------------------------------------------------------------------
if(HW_CFG_ENABLE_LLM_SOURCE_ONBOARD GREATER 0)
    list(APPEND hardwareone_srcs System_LLM.cpp)
    list(APPEND hardwareone_srcs System_LLM_Kernels.cpp)
    list(APPEND hardwareone_srcs System_LLM_Sampler.cpp)
    list(APPEND hardwareone_srcs System_LLM_Tokenizer.cpp)
    list(APPEND hardwareone_srcs System_LLM_Model.cpp)
    list(APPEND hardwareone_srcs System_LLM_Menu.cpp)
    list(APPEND hardwareone_requires espressif__esp-dsp)
    message(STATUS "LLM source: ONBOARD engine ENABLED")
endif()

if(HW_CFG_ENABLE_LLM_BACKEND GREATER 0)
    list(APPEND hardwareone_srcs System_LLMBackend.cpp)
    list(APPEND hardwareone_srcs System_LLMCommands.cpp)
    list(APPEND hardwareone_srcs System_LLMChat.cpp)
    if(HW_CFG_ENABLE_LLM_SOURCE_CM5 GREATER 0)
        list(APPEND hardwareone_srcs System_LLMCm5.cpp)
        message(STATUS "LLM source: CM5 co-processor ENABLED")
    endif()
    if(HW_CFG_ENABLE_HTTP_SERVER)
        list(APPEND hardwareone_srcs WebPage_LLM.cpp)
    endif()
    # OLED modes are always compiled and self-gate on ENABLE_OLED_DISPLAY —
    # see the OLED block above; no CMake-side display gate belongs here.
    list(APPEND hardwareone_srcs OLED_Mode_LLM.cpp)
    message(STATUS "LLM feature ENABLED (sources: onboard=${HW_CFG_ENABLE_LLM_SOURCE_ONBOARD} cm5=${HW_CFG_ENABLE_LLM_SOURCE_CM5})")
else()
    message(STATUS "LLM feature DISABLED")
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
# Crash-record panic interposition
# ============================================================================
# panic_handler.c calls esp_panic_handler() as a plain cross-TU call, so the
# linker can interpose on it. System_CrashRecord.cpp defines
# __wrap_esp_panic_handler(), stashes the crash detail in RTC memory, and then
# tail-calls __real_esp_panic_handler() so the normal Guru Meditation output and
# reboot are completely unchanged.
#
# This is what makes a field crash diagnosable at all: the panic banner goes to
# UART and the device reboots in 0 s, so without this the most useful sentence
# the firmware ever prints is lost unless someone happened to be on serial.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")

# Retained-data preservation. Some vendored init paths erase NVS through
# esp_partition_erase_range(), while others call nvs_flash_erase().
# System_OTASafety.cpp interposes on both on every main-firmware boot, blocks
# only whole-NVS-partition erases, and leaves ordinary NVS page GC untouched.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=nvs_flash_erase"
    "-Wl,--wrap=esp_partition_erase_range"
)

# ============================================================================
# Build-time guard: no raw LittleFS/SD file I/O outside System_VFS.cpp
# ============================================================================
# The permission refactor (Phases 1-6) routes every file read/write through
# VFS::*Guarded with an AuthContext, so the [PERM] DENY audit trail and the
# role-based rule table actually apply. This check fails the build if anyone
# reintroduces raw LittleFS/SD .open / .exists / .remove / .rename / .mkdir /
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
