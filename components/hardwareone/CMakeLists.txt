set(hw_cfg_file "${CMAKE_CURRENT_LIST_DIR}/System_BuildConfig.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${hw_cfg_file}")

set(HW_CFG_NETWORK_FEATURE_LEVEL 0)
set(HW_CFG_WEB_FEATURE_LEVEL 0)
set(HW_CFG_ENABLE_MQTT 0)

if(EXISTS "${hw_cfg_file}")
    file(READ "${hw_cfg_file}" HW_CFG_CONTENT)
    string(REGEX MATCH "#define[ \t]+NETWORK_FEATURE_LEVEL[ \t]+([0-9]+)" _net_match "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_NETWORK_FEATURE_LEVEL "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+WEB_FEATURE_LEVEL[ \t]+([0-9]+)" _web_match "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_WEB_FEATURE_LEVEL "${CMAKE_MATCH_1}")
    endif()

    string(REGEX MATCH "#define[ \t]+ENABLE_MQTT[ \t]+([0-9]+)" _mqtt_match "${HW_CFG_CONTENT}")
    if(CMAKE_MATCH_1)
        set(HW_CFG_ENABLE_MQTT "${CMAKE_MATCH_1}")
    endif()
endif()

set(HW_CFG_ENABLE_HTTP_SERVER 0)
if(HW_CFG_NETWORK_FEATURE_LEVEL GREATER_EQUAL 2 AND HW_CFG_WEB_FEATURE_LEVEL GREATER 0)
    set(HW_CFG_ENABLE_HTTP_SERVER 1)
endif()

set(hardwareone_srcs
        HardwareOne.cpp
        i2csensor-apds9960.cpp
        i2csensor-pa1010d.cpp
        System_Maps.cpp
        i2csensor-seesaw.cpp
        i2csensor-bno055.cpp
        i2csensor-mlx90640.cpp
        i2csensor-vl53l4cx.cpp
        i2csensor-rda5807.cpp
        i2csensor-ds3231.cpp
        i2csensor-sths34pf80.cpp
        System_Automation.cpp
        Optional_Bluetooth.cpp
        BLE_IDF.cpp
        Optional_EvenG2.cpp
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
        OLED_Utils.cpp
        OLED_Settings.cpp
        OLED_ESPNow.cpp
        OLED_SettingsEditor.cpp
        OLED_FirstTimeSetup.cpp
        OLED_Mode_Settings.cpp
        OLED_Mode_Auth.cpp
        OLED_Mode_CLI.cpp
        OLED_Mode_Logging.cpp
        OLED_Mode_Map.cpp
        OLED_Mode_Power.cpp
        OLED_Mode_Network.cpp
        OLED_Mode_System.cpp
        OLED_Mode_Sensors.cpp
        OLED_Mode_Animations.cpp
        OLED_Mode_Menu.cpp
        OLED_Mode_FileBrowser.cpp
        OLED_Mode_Automations.cpp
        OLED_Mode_Speech.cpp
        OLED_Mode_Remote.cpp
        OLED_Mode_UnifiedMenu.cpp
        OLED_Mode_RemoteSettings.cpp
        OLED_Mode_SetPattern.cpp
        OLED_RemoteSettings.cpp
        OLED_UI.cpp
        System_MemoryMonitor.cpp
        i2csensor-pca9685.cpp
        System_Hardware.cpp
        System_SensorLogging.cpp
        System_SensorStubs.cpp
        System_Settings.cpp
        System_SetupWizard.cpp
        System_FeatureRegistry.cpp
        OLED_SetupWizard.cpp
        System_Utils.cpp
        System_TaskUtils.cpp
        System_User.cpp
        System_Auth.cpp
        System_Users.cpp
        System_WiFi.cpp
        System_Camera_DVP.cpp
        System_ImageManager.cpp
        System_EdgeImpulse.cpp
        System_ESPSR.cpp
        System_VFS.cpp
        System_Microphone.cpp
        System_SensorRegistry.cpp
        System_Notifications.cpp
)

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
            WebPage_Maps.cpp
            WebPage_Games.cpp
            WebPage_ESPNow.cpp
            WebPage_ESPNow_Metadata.cpp
            WebPage_Pair.cpp
            WebPage_Bluetooth.cpp
            WebPage_Speech.cpp
            WebPage_MQTT.cpp
    )
    list(APPEND hardwareone_requires esp_http_server)
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
    target_compile_definitions(${COMPONENT_LIB} PUBLIC ENABLE_CAMERA_SENSOR=1)
    message(STATUS "XIAO ESP32S3 Sense features ENABLED (camera, mic, SD card)")
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=format-truncation
    -Wno-error=maybe-uninitialized
)
