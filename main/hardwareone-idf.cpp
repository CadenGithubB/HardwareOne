#include "Arduino.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "../components/hardwareone/System_AuthIdentity.h"

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

    // Allocate the TLS auth-identity slot for this (main) task. The
    // ExecIdentityGuard ctor would lazy-init anyway, but doing it explicitly
    // here makes the intent clear at the entry point.
    initAuthIdentityForCurrentTask();

    // Install SYSTEM as the main task's baseline identity, sticky for the
    // lifetime of the process. Web, Serial, and G2 paths still RAII-install
    // their per-action identity on top of this via executeCommand() /
    // ExecIdentityGuard; the guard's destructor restores SYSTEM as the
    // "ambient" identity. Direct OLED-side filesystem/settings access (file
    // browser, log viewer, etc. — which bypass executeCommand()) inherits
    // SYSTEM and sees the full filesystem the way the operator standing in
    // front of the device expects. app_main() never returns, so this guard
    // never destructs.
    ExecIdentityGuard mainTaskIdentity(systemIdentity("main"));

    // Run user setup once
    setup();

    // Run user loop forever with a small delay
    while (true) {
        loop();
        vTaskDelay(pdMS_TO_TICKS(10));  // 10 ms; adjust as needed
    }
}