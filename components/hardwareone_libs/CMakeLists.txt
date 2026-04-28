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
)