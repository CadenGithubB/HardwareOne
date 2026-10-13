set(hw_cfg_file "${CMAKE_CURRENT_LIST_DIR}/System_BuildConfig.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${hw_cfg_file}")

# Default values used when the matching regex doesn't fire (file missing,
# unexpected formatting, etc.). The per-file `#if` guards inside each .cpp
# are the source of truth for runtime behaviour — these CMake gates only
# decide which files reach the compiler. A "0" default means "skip it"
# rather than "include it" so a parse failure is fail-safe (no spurious
# compile errors) and easy to spot in build output.
set(HW_CFG_I2C_FEATURE_LEVEL 0)
set(HW_CFG_DISPLAY_TYPE 0)
set(HW_CFG_INPUT_DEVICE_TYPE 0)  # 0 = NONE; header values 1/2 select a driver
set(HW_CFG_ENABLE_MQTT 0)
set(HW_CFG_ENABLE_LLM_BACKEND 0)         # the LLM feature (registry/chat/commands/surfaces)
set(HW_CFG_ENABLE_LLM_SOURCE_ONBOARD 0)  # on-device PSRAM engine as a source
set(HW_CFG_ENABLE_LLM_SOURCE_HTTP 0)     # optional Wi-Fi OpenClaw source
set(HW_CFG_ENABLE_LLM_SOURCE_CM5 0)      # CM5 co-processor as a source
set(HW_CFG_ENABLE_MAPS 0)
set(HW_CFG_ENABLE_AUTOMATION 0)
set(HW_CFG_ENABLE_CAMERA_SENSOR 0)
set(HW_CFG_ENABLE_ESP_SR 0)
set(HW_CFG_ENABLE_LOCAL_STT 0)
set(HW_CFG_ENABLE_STT_SPEAKER_LABELS 0)
set(HW_CFG_ENABLE_EDGE_IMPULSE 0)
set(HW_CFG_ENABLE_GAMES 0)
set(HW_CFG_ENABLE_WEB_GAME_DARKROOM 0)
set(HW_CFG_ENABLE_BLUETOOTH 0)
set(HW_CFG_ENABLE_G2_GLASSES 0)
set(HW_CFG_ENABLE_R1_HEALTH 0)
set(HW_CFG_ENABLE_G2_TESTSUITE 0)
set(HW_CFG_ENABLE_RASPBERRY_PI_HOST_POWER 0)
set(HW_CFG_ENABLE_RASPBERRY_PI_HOST_FAN 0)

if(EXISTS "${hw_cfg_file}")
    file(READ "${hw_cfg_file}" HW_CFG_CONTENT)

    # A board feature profile (boards/<board>.features.h) overrides the shared
    # editable defaults; a deployment profile, prepended below, overrides both.
    # Build property, not a variable: requirement expansion runs in script mode.
    idf_build_get_property(_hw_board_feature_file HW1_BOARD_FEATURE_FILE)
    if(_hw_board_feature_file)
        if(NOT EXISTS "${_hw_board_feature_file}")
            message(FATAL_ERROR
                "HW1 board feature header is missing: ${_hw_board_feature_file}")
        endif()
        file(READ "${_hw_board_feature_file}" _hw_board_features)
        set(HW_CFG_CONTENT "${_hw_board_features}\n${HW_CFG_CONTENT}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${_hw_board_feature_file}")
    endif()

    # A checked-in deployment profile overrides the editable defaults in
    # System_BuildConfig.h. Put its text first so the existing first-match regex
    # parser sees the deployment values, exactly as the C preprocessor will after
    # System_BuildConfig.h includes the file before its derived rules.
    if(DEFINED HW1_DEPLOYMENT_FEATURE_FILE AND
       NOT "${HW1_DEPLOYMENT_FEATURE_FILE}" STREQUAL "")
        if(NOT EXISTS "${HW1_DEPLOYMENT_FEATURE_FILE}")
            message(FATAL_ERROR
                "HW1 deployment feature header is missing: ${HW1_DEPLOYMENT_FEATURE_FILE}")
        endif()
        file(READ "${HW1_DEPLOYMENT_FEATURE_FILE}" _hw_deployment_features)
        set(HW_CFG_CONTENT "${_hw_deployment_features}\n${HW_CFG_CONTENT}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${HW1_DEPLOYMENT_FEATURE_FILE}")
        message(STATUS
            "HardwareOne feature profile: ${HW1_DEPLOYMENT_FEATURE_FILE}")
    endif()

    # Ordinary named feature configurations are generated in the build tree.
    # Like board profiles, use a build property so the requirement-expansion
    # script sees the exact same file as the compiler and root build.
    idf_build_get_property(_hw_feature_config_file HW1_FEATURE_CONFIG_FILE)
    if(_hw_feature_config_file)
        if(NOT EXISTS "${_hw_feature_config_file}")
            message(FATAL_ERROR
                "HW1 feature configuration header is missing: ${_hw_feature_config_file}")
        endif()
        file(READ "${_hw_feature_config_file}" _hw_feature_config)
        set(HW_CFG_CONTENT "${_hw_feature_config}\n${HW_CFG_CONTENT}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${_hw_feature_config_file}")
        message(STATUS "HardwareOne feature configuration: ${_hw_feature_config_file}")
    endif()

    # Source selection reads these flags as text: the first definition wins
    # (feature configuration, deployment and board profiles are prepended to
    # the shared header). The regexes need whitespace after each name, so
    # ENABLE_LLM_BACKEND cannot match ENABLE_LLM_BACKEND_FOO and the LLM
    # source flags never alias. Each first definition must be a bare integer
    # literal, optionally followed by a comment: `(4)` or `0 + 4` would read
    # differently here than in the C++ preprocessor, which keeps the feature
    # while CMake drops its sources, so stop the configure instead. A flag
    # with no definition keeps its 0 default above.
    set(HW_CFG_TEXT_FLAGS
        I2C_FEATURE_LEVEL DISPLAY_TYPE INPUT_DEVICE_TYPE ENABLE_MQTT
        ENABLE_LLM_BACKEND ENABLE_LLM_SOURCE_ONBOARD ENABLE_LLM_SOURCE_CM5
        ENABLE_LLM_SOURCE_HTTP
        ENABLE_MAPS ENABLE_CAMERA_SENSOR ENABLE_ESP_SR ENABLE_LOCAL_STT
        ENABLE_STT_SPEAKER_LABELS
        ENABLE_AUTOMATION ENABLE_EDGE_IMPULSE ENABLE_GAMES ENABLE_WEB_GAME_DARKROOM
        ENABLE_BLUETOOTH ENABLE_G2_GLASSES ENABLE_R1_HEALTH ENABLE_G2_TESTSUITE
        ENABLE_RASPBERRY_PI_HOST_POWER ENABLE_RASPBERRY_PI_HOST_FAN)
    foreach(_hw_flag IN LISTS HW_CFG_TEXT_FLAGS)
        string(REGEX MATCH "#define[ \t]+${_hw_flag}[ \t][^\n]*" _hw_definition "${HW_CFG_CONTENT}")
        if(_hw_definition AND NOT _hw_definition MATCHES
           "^#define[ \t]+${_hw_flag}[ \t]+[0-9]+[ \t\r]*(//.*|/\\*.*)?$")
            message(FATAL_ERROR
                "${_hw_flag} must be defined as a bare integer literal, found: ${_hw_definition}\n"
                "CMake selects sources from this flag's text; see System_BuildConfig.h.")
        endif()
        string(REGEX MATCH "#define[ \t]+${_hw_flag}[ \t]+([0-9]+)" _ "${HW_CFG_CONTENT}")
        if(CMAKE_MATCH_1)
            set(HW_CFG_${_hw_flag} "${CMAKE_MATCH_1}")
        endif()
    endforeach()
endif()

# The resolved sdkconfig determines whether Bluedroid exists. Mirror the
# System_BuildConfig.h dependency rule so source selection matches C++ guards.
# Optional SDK policy comes from presets and a named/deployment selection;
# presets/qtpy_esp32.sdkconfig.defaults keeps the lean QT Py default BT-free.
# CONFIG_* is unset during early requirement expansion, which only affects
# that early no-op pass.
if(NOT CONFIG_BT_ENABLED OR NOT CONFIG_BT_BLUEDROID_ENABLED)
    set(HW_CFG_ENABLE_BLUETOOTH 0)
endif()

# R1 Health / Pet / Tests need BT + G2 (matches System_BuildConfig.h derived rules).
if(NOT (HW_CFG_ENABLE_BLUETOOTH GREATER 0 AND HW_CFG_ENABLE_G2_GLASSES GREATER 0))
    set(HW_CFG_ENABLE_R1_HEALTH 0)
    set(HW_CFG_ENABLE_G2_TESTSUITE 0)
endif()

# Coarse OLED gate: skip every OLED_*.cpp (except OLED_Utils.cpp, whose
# always-called entry points compile to empty bodies) when the user
# has no display hardware, or selects an I2C display with I2C disabled.
# The P4X-EYE SPI backend remains available without an I2C bus. The
# per-feature .cpp guards (`#if ENABLE_OLED_DISPLAY && ...`) still apply
# inside each file when the bus + display are present but a sub-feature
# (ESPNOW, ESP_SR, BONDED_MODE) is off.
set(HW_CFG_BUILD_OLED_MODES 0)
if(HW_CFG_DISPLAY_TYPE EQUAL 4 OR
   (HW_CFG_DISPLAY_TYPE GREATER 0 AND HW_CFG_I2C_FEATURE_LEVEL GREATER 0))
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
        HAL_Bluetooth.cpp
        HAL_Radio.cpp
        System_RadioCompanion.cpp
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
        System_CommandRegistry.cpp
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
        System_Icons_Lookup.cpp
        HAL_Input.cpp
        HAL_Display.cpp
        HAL_JPEG.cpp
        HAL_JPEG_Software.cpp
        HAL_JPEG_P4.cpp
        HAL_SDCard.cpp
        System_Mutex.cpp
        System_Power.cpp
        System_NeoPixel.cpp
        # OLED_Utils.cpp stays unconditional: every build calls its
        # display entry points (oledUpdate, oledEarlyInit,
        # oledNotifyLocalDisplayAuthChanged and the rest at the end of the
        # file), whose bodies compile away when DISPLAY_TYPE=0. The
        # per-mode OLED_Mode_*.cpp files (handled below) only appear when a
        # display is actually selected.
        OLED_Utils.cpp
        System_MemUtil.cpp
        System_MemTracker.cpp
        System_MemoryMonitor.cpp
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
        HAL_Camera.cpp
        HAL_Camera_DVP.cpp
        HAL_Camera_P4.cpp
        System_Camera_DVP.cpp
        System_Camera_Video.cpp
        System_ImageManager.cpp
        System_ESPSR.cpp
        System_VFS.cpp
        System_Microphone.cpp
        System_Dictation.cpp
        System_Transcript.cpp
        QuickNote_Store.cpp
        System_QuickNote.cpp
        System_TranscriptionUI.cpp
        OLED_Mode_Transcription.cpp
        G2_Page_Transcription.cpp
        G2_Page_Notes.cpp
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
        System_NTP.cpp
        network/wifi/System_WiFiNetworks.cpp
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
        i2csensor_ht16k33.cpp
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

# GPIO input and SPI display are independent of the optional I2C bus.
if(HW_CFG_INPUT_DEVICE_TYPE EQUAL 3)
    list(APPEND hardwareone_srcs Input_GPIOEncoder.cpp)
endif()
if(HW_CFG_DISPLAY_TYPE EQUAL 4)
    list(APPEND hardwareone_srcs HAL_Display_P4Eye.cpp)
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
    message(STATUS "Local display modes EXCLUDED by display/bus selection")
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
        esp_adc
        esp_driver_gpio
        esp_driver_i2s
        json
        mbedtls
        esp_wifi
        esp_http_server
        esp_https_server
        esp32-camera
        espressif__esp_jpeg
        fatfs
        sdmmc
        esp_driver_sdmmc
        esp_driver_sdspi
        espressif__libsodium
)

if(HW_CFG_DISPLAY_TYPE EQUAL 4)
    list(APPEND hardwareone_requires esp_lcd esp_driver_spi esp_driver_ledc)
endif()

if(HW_CFG_ENABLE_MQTT GREATER 0)
    list(APPEND hardwareone_srcs
            System_MQTT.cpp
    )
    list(APPEND hardwareone_requires mqtt)
    message(STATUS "MQTT support ENABLED")
else()
    message(STATUS "MQTT support DISABLED")
endif()

# Network/web policy belongs to System_BuildConfig.h. These sources already
# guard their implementations, including migration restore without the web UI.
# Keep their SDK requirements above available during IDF's early discovery pass;
# unused implementations are removed by the compiler/linker in lean builds.
list(APPEND hardwareone_srcs
    WebServer_Events.cpp
    WebServer_Server.cpp
    WebServer_Utils.cpp
    WebAssets.cpp
    WebPage_Sensors.cpp
    WebPage_ESPNow.cpp
    WebPage_ESPNow_Metadata.cpp
    WebPage_Bond.cpp
    WebPage_Bluetooth.cpp
    WebPage_Speech.cpp
    WebPage_MQTT.cpp
    WebPage_Battery.cpp
    WebPage_Power.cpp
    WebServer_MigrationTool.cpp
)
# Application source filters remain independent of the network/web selection.
if(HW_CFG_ENABLE_MAPS GREATER 0)
    list(APPEND hardwareone_srcs WebPage_Maps.cpp)
endif()
if(HW_CFG_ENABLE_R1_HEALTH GREATER 0)
    list(APPEND hardwareone_srcs WebPage_R1_Health.cpp)
endif()
if(HW_CFG_ENABLE_GAMES GREATER 0 AND HW_CFG_ENABLE_WEB_GAME_DARKROOM GREATER 0)
    list(APPEND hardwareone_srcs WebPage_DarkRoom.cpp)
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
    if(HW_CFG_ENABLE_LLM_SOURCE_HTTP GREATER 0)
        list(APPEND hardwareone_srcs System_LLMHttp.cpp)
        list(APPEND hardwareone_requires esp_http_client)
        message(STATUS "LLM source: HTTP OpenClaw ENABLED")
    endif()
    list(APPEND hardwareone_srcs WebPage_LLM.cpp)
    # OLED modes are always compiled and self-gate on ENABLE_OLED_DISPLAY —
    # see the OLED block above; no CMake-side display gate belongs here.
    list(APPEND hardwareone_srcs OLED_Mode_LLM.cpp)
    message(STATUS "LLM feature ENABLED (sources: onboard=${HW_CFG_ENABLE_LLM_SOURCE_ONBOARD} cm5=${HW_CFG_ENABLE_LLM_SOURCE_CM5} http=${HW_CFG_ENABLE_LLM_SOURCE_HTTP})")
else()
    message(STATUS "LLM feature DISABLED")
endif()

# IDF expands component requirements before CONFIG_SOC_* is available. P4 is
# the supported hardware target in the pinned SDK; the backend itself uses the
# SOC capability plus the qualified-driver marker, never application chip checks.
if(IDF_TARGET STREQUAL "esp32p4" OR CONFIG_SOC_JPEG_DECODE_SUPPORTED)
    list(APPEND hardwareone_requires esp_driver_jpeg)
endif()

# Both backends inspect SCCB controller ownership before driver initialization.
if(HW_CFG_ENABLE_CAMERA_SENSOR GREATER 0)
    list(APPEND hardwareone_requires esp_driver_i2c)
endif()

# The shared speech consumer selects target-specific ESP-SR libraries through
# the component; no chip-specific audio path is required here.
if(HW_CFG_ENABLE_ESP_SR GREATER 0)
    list(APPEND hardwareone_requires espressif__esp-sr)
endif()

# The local backend has no display or UART dependency. Its model/runtime is
# enabled explicitly by a deployment profile; all other builds keep Pi STT.
if(HW_CFG_ENABLE_LOCAL_STT GREATER 0)
    list(APPEND hardwareone_srcs System_STT.cpp System_STTLocal.cpp)
    if(IDF_TARGET STREQUAL "esp32p4")
        # stt_lm.cpp is inert unless System_STTLocal.cpp uses it (ENABLE_STT_LM).
        list(APPEND hardwareone_srcs stt/quartznet_runtime.cpp stt/quartznet_frontend.cpp stt/stt_lm.cpp)
        # The resident model (stt/quartznet_runtime.h) is on unless the build is
        # configured with -DHW1_STT_RESIDENT_MODEL=0. stt_dl_metadata.cpp is its
        # PSRAM metadata hook for the tree-local ESP-DL (tools/p4/prepare_esp_dl.py).
        if(NOT DEFINED HW1_STT_RESIDENT_MODEL)
            set(HW1_STT_RESIDENT_MODEL 1)
        endif()
        if(HW1_STT_RESIDENT_MODEL)
            set(_hw1_stt_resident_model 1)
            list(APPEND hardwareone_srcs stt/stt_dl_metadata.cpp)
        else()
            set(_hw1_stt_resident_model 0)
        endif()
        # Session speaker labels (stt/speaker_model.h, ENABLE_STT_SPEAKER_LABELS): their sources, plus the
        # explicit-PSRAM helpers in stt_dl_metadata.cpp they allocate with.
        if(HW_CFG_ENABLE_STT_SPEAKER_LABELS GREATER 0)
            # The background speaker task and the STT worker both run PIE vector code on core 1. ESP-IDF
            # before 6.0 does not save PIE register q3 on a context switch, so the voiceprints come out wrong:
            # refuse to build until the IDF carries the fix (tools/p4/idf-patches/README.md).
            set(_hw1_portasm "${IDF_PATH}/components/freertos/FreeRTOS-Kernel/portable/riscv/portasm.S")
            if(EXISTS "${_hw1_portasm}")
                file(READ "${_hw1_portasm}" _hw1_portasm_text)
                if(NOT _hw1_portasm_text MATCHES "esp\\.vst\\.128\\.ip[ \t]+q3")
                    message(FATAL_ERROR "ENABLE_STT_SPEAKER_LABELS needs the ESP-IDF PIE context-switch fix "
                        "(upstream b25cb29, ESP-IDF 6.0+): apply tools/p4/idf-patches/"
                        "0001-freertos-fix-xesppie-registers-save-restore.patch to ${IDF_PATH} (see its README).")
                endif()
            endif()
            list(APPEND hardwareone_srcs stt/speaker_frontend.cpp stt/speaker_model.cpp)
            if(NOT HW1_STT_RESIDENT_MODEL)
                list(APPEND hardwareone_srcs stt/stt_dl_metadata.cpp)
            endif()
        endif()
        list(APPEND hardwareone_requires espressif__esp-dl esp_rom mbedtls)
    endif()
endif()

# CSI/ISP stays behind the shared camera API; native S3 uses esp32-camera.
if(IDF_TARGET STREQUAL "esp32p4" AND HW_CFG_ENABLE_CAMERA_SENSOR GREATER 0)
    list(APPEND hardwareone_requires esp_video esp_cam_sensor esp_driver_gpio)
endif()

# ESP32-C6 radio companion over ESP-Hosted. HAL_Radio.cpp and
# System_RadioCompanion.cpp compile to nothing without CONFIG_ESP_WIFI_REMOTE_ENABLED.
if(IDF_TARGET STREQUAL "esp32p4")
    list(APPEND hardwareone_requires esp_hosted esp_now_hosted esp_event esp_timer esp_driver_uart)
endif()

idf_component_register(
    SRCS
        ${hardwareone_srcs}
    INCLUDE_DIRS
        .
    REQUIRES
        ${hardwareone_requires}
    LDFRAGMENTS
        esp32_camera_psram.lf
)

# IDF evaluates component requirements in script mode before registration.
# Source properties are only legal after idf_component_register returns.
# QuickNote runs on cold paths only (`note` commands, a worker that wakes at
# most every 200 ms while a note records, and the Notes lens page's taps and
# 500 ms polls), so it is built for size: the XIAO-S3 app partition has little
# headroom left.
set_source_files_properties(System_QuickNote.cpp QuickNote_Store.cpp G2_Page_Notes.cpp
                            PROPERTIES COMPILE_OPTIONS "-Os")
if(HW_CFG_ENABLE_LOCAL_STT GREATER 0 AND IDF_TARGET STREQUAL "esp32p4")
    # The STT engine is built with -O2 instead of the app-wide -Os (source
    # options follow the component's, so -O2 wins): measured ~41 ms per 15x5
    # phrase on the P4. The frontend keeps -ffp-contract=off (no FMA
    # contraction: features stay bit-identical to the reference). One call per
    # file: set_source_files_properties replaces COMPILE_OPTIONS.
    set_source_files_properties(stt/quartznet_frontend.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-O2")
    set_source_files_properties(stt/quartznet_runtime.cpp PROPERTIES COMPILE_OPTIONS "-O2")
    set_source_files_properties(stt/stt_lm.cpp PROPERTIES COMPILE_OPTIONS "-O2")
    # Speaker features and pooling are tight float loops (at -Os they were ~3x slower).
    if(HW_CFG_ENABLE_STT_SPEAKER_LABELS GREATER 0)
        set_source_files_properties(stt/speaker_frontend.cpp stt/speaker_model.cpp PROPERTIES COMPILE_OPTIONS "-O2")
    endif()
    # Speaker diagnostics (System_STTLocal.h): off unless configured with -DHW1_STT_SPEAKER_DIAGNOSTICS=1.
    if(DEFINED HW1_STT_SPEAKER_DIAGNOSTICS)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE HW1_STT_SPEAKER_DIAGNOSTICS=${HW1_STT_SPEAKER_DIAGNOSTICS})
    endif()
    # Component-wide: ModelCache's layout depends on it, so every translation
    # unit (System_STTLocal.cpp included) must see the same value.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HW1_STT_RESIDENT_MODEL=${_hw1_stt_resident_model})
    # ESP-SR also registers a model whitelist. Union QuartzNet's exact ops and
    # kernel variants into it; otherwise SR's WN10 list strips our Relu/int8 Conv.
    idf_build_get_property(_stt_build_components BUILD_COMPONENTS)
    if("espressif__esp-dl" IN_LIST _stt_build_components)
        idf_component_get_property(_stt_dl_dir espressif__esp-dl COMPONENT_DIR)
    elseif("esp-dl" IN_LIST _stt_build_components)
        idf_component_get_property(_stt_dl_dir esp-dl COMPONENT_DIR)
    else()
        message(FATAL_ERROR "Local STT requires the pinned ESP-DL component")
    endif()
    include("${_stt_dl_dir}/cmake/compile_register.cmake")
    esp_dl_compile_add_yml("${CMAKE_CURRENT_LIST_DIR}/stt/quartznet.compile_req.yml")
    # The speaker-embedding trunk (8-bit weights, 16-bit activations), only with speaker labels.
    if(HW_CFG_ENABLE_STT_SPEAKER_LABELS GREATER 0)
        esp_dl_compile_add_yml("${CMAKE_CURRENT_LIST_DIR}/stt/speaker.compile_req.yml")
    endif()
endif()

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

# Exact, generated gzip assets are committed for Arduino and other build systems.
# Always check source/header consistency, including incremental builds; never
# modify the checkout while CMake/Ninja are running.
if(PYTHON)
    set(_hw_web_python "${PYTHON}")
else()
    set(_hw_web_python "${Python3_EXECUTABLE}")
endif()
add_custom_target(hardwareone_check_web_assets
    COMMAND "${_hw_web_python}"
        "${CMAKE_CURRENT_LIST_DIR}/web_assets/generate.py" --check
    COMMENT "Checking embedded web assets"
    VERBATIM
)
add_dependencies(${COMPONENT_LIB} hardwareone_check_web_assets)
