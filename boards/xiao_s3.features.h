// Default application choices for HW_BOARD=xiao_s3.
// Board/deployment/config headers are read in the same order by CMake and C++.
// Optional external peripherals and MQTT stay off in the ordinary image.
// A feature configuration or deployment may enable fitted hardware/services
// when its dependencies, controller ownership and image budget permit.
#pragma once

#undef ENABLE_MQTT
#define ENABLE_MQTT 0

// No display or input controller is fitted to the base/Sense assembly.
// External I2C devices use D4/D5 (GPIO5/6); these are choices, not board vetoes.
#undef I2C_FEATURE_LEVEL
#define I2C_FEATURE_LEVEL 0
#undef DISPLAY_TYPE
#define DISPLAY_TYPE 0
#undef INPUT_DEVICE_TYPE
#define INPUT_DEVICE_TYPE 0
