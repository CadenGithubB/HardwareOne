#include "Arduino.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Real HardwareOne Arduino-style entry points are implemented in
// components/hardwareone/HardwareOne.cpp
extern void hardwareone_setup();
extern void hardwareone_loop();

// Arduino-style hooks that ESP-IDF's Arduino core will call.
void setup()
{
	// Delegate to the real HardwareOne setup
	hardwareone_setup();
}

void loop()
{
	// Delegate to the real HardwareOne loop
	hardwareone_loop();
}

// ESP-IDF entry point that boots the Arduino core
extern "C" void app_main(void)
{
    // Initialize Arduino core (Serial, peripherals, etc.)
    initArduino();

    // Run user setup once
    setup();

    // Run user loop forever with a small delay
    while (true) {
        loop();
        vTaskDelay(pdMS_TO_TICKS(10));  // 10 ms; adjust as needed
    }
}