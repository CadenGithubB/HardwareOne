idf_component_register(
    SRC_DIRS
        Adafruit_BNO055
        Adafruit_Unified_Sensor
        Adafruit_BusIO
        Adafruit_GFX_Library
        Adafruit_SSD1306
        Adafruit_NeoPixel
        Adafruit_APDS9960_Library
        STM32duino_VL53L4CX/src
        Adafruit_MLX90640
        Adafruit_MLX90640/utility
        Adafruit_PWM_Servo_Driver_Library
        Adafruit_GPS_Library/src
        Adafruit_seesaw_Library
        Radio/src
        liblc3/src

    INCLUDE_DIRS
        ArduinoJson
        ArduinoJson/src
        Adafruit_BNO055
        Adafruit_Unified_Sensor
        Adafruit_BusIO
        Adafruit_GFX_Library
        Adafruit_SSD1306
        Adafruit_NeoPixel
        Adafruit_APDS9960_Library
        STM32duino_VL53L4CX/src
        Adafruit_MLX90640
        Adafruit_PWM_Servo_Driver_Library
        Adafruit_GPS_Library/src
        Adafruit_seesaw_Library
        Radio/src
        liblc3/include

    REQUIRES
        arduino
)

# liblc3 vendored from https://github.com/google/liblc3 (Apache-2.0).
# Its .c files use both <lc3.h> (public) and "common.h" / "bits.h" /
# etc. (private, sibling files in src/). Add src/ to those translation
# units' include path so the private headers resolve. Public consumers
# only get liblc3/include via the INCLUDE_DIRS list above.
file(GLOB LIBLC3_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/liblc3/src/*.c")
set_source_files_properties(
    ${LIBLC3_SOURCES}
    PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/liblc3/src;${CMAKE_CURRENT_SOURCE_DIR}/liblc3/include"
    COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-sign-compare;-Wno-unused-variable"
    # Baseline LC3 only. LC3plus (2.5 ms / 5 ms frames) and LC3plus HR
    # (48/96 kHz high-res) are upstream extensions that default ON in
    # liblc3/src/common.h; Bluetooth LE Audio's baseline LC3 is 7.5 ms and
    # 10 ms only, and the G2 mic is hardwired to 10 ms / 16 kHz mono
    # (kMicLc3FrameUs / kMicLc3SampleHz in G2_Glasses.cpp).
    #
    # This is not a trim of dead code the linker could have found: the MDCT
    # windows are reached through lc3_mdct_win[dt][sr] (tables.c), a runtime-
    # indexed pointer table, so --gc-sections has to keep every entry. The
    # flags make the compiler emit NULL for the unused (dt,sr) slots instead.
    #
    # Safe by construction: resolve_dt() / resolve_srate() (lc3.c) are guarded
    # by these same flags, so a 2.5 ms or HR request now resolves to
    # LC3_NUM_DT / LC3_NUM_SRATE and lc3_setup_decoder() returns NULL rather
    # than dereferencing a NULL window.
    COMPILE_DEFINITIONS "LC3_PLUS=0;LC3_PLUS_HR=0"
)