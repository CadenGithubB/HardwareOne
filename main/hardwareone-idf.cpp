#include "Arduino.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_BT_ENABLED && CONFIG_IDF_TARGET_ESP32 && CONFIG_BTDM_CTRL_MODE_BLE_ONLY
#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#endif

#include "../components/hardwareone/System_AuthIdentity.h"
#include "../components/hardwareone/System_OTASafety.h"

// Real HardwareOne Arduino-style entry points are implemented in
// components/hardwareone/HardwareOne.cpp
extern void hardwareone_setup();
extern void hardwareone_loop();

// Arduino's rollback helper is weak and defaults to false, which makes
// initArduino() mark a PENDING_VERIFY image valid before HardwareOne setup has
// run. Keep validation under HardwareOne's native ESP-IDF probation instead.
extern "C" bool verifyRollbackLater(void)
{
    return true;
}

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
    // Inspect OTA state and arm the pending-image supervisor before Arduino can
    // auto-recover NVS or otherwise enter a setup path that might hang.
    otaSafetyInitEarly();

#if CONFIG_BT_ENABLED && CONFIG_IDF_TARGET_ESP32 && CONFIG_BTDM_CTRL_MODE_BLE_ONLY
    // Hand the BR/EDR exchange-memory block back to the heap. bt.c reserves
    // SOC_MEM_BT_EM_START..SOC_MEM_BT_EM_BREDR_REAL_END at link time
    // (SOC_RESERVE_MEMORY_REGION, bt.c) whether or not Classic BT is used, so on
    // this BLE-only build 0x3ffb2730..0x3ffb6388 = 15448 B of internal DRAM sits
    // reserved for a radio mode the controller cannot enter. Releasing
    // ESP_BT_MODE_CLASSIC_BT frees exactly that one row of
    // btdm_dram_available_region[] — the BLE exchange memory, controller .bss
    // and .data rows are BTDM-owned and are left untouched — and registers it as
    // a heap via heap_caps_add_region (INTERNAL|8BIT|DMA, so task stacks may use
    // it). Net gain is ~15032 B after TLSF overhead.
    //
    // Must run here: both entry points bail with ESP_ERR_INVALID_STATE unless
    // btdm_controller_status == ESP_BT_CONTROLLER_STATUS_IDLE, which holds only
    // before esp_bt_controller_init(). It is also what keeps the release safe --
    // btdm_controller_mem_init() memsets every region whose mode is not IDLE, and
    // the release clears this row's mode to IDLE, so a later BLE init will not
    // zero memory the heap has already handed out.
    //
    // Irreversible for the boot: BR/EDR cannot be used afterwards without a
    // reset. That costs nothing while CONFIG_BTDM_CTRL_MODE_BLE_ONLY holds (the
    // guard above), and nothing in this firmware uses SPP/A2DP/HFP. The call is
    // idempotent -- a second one returns ESP_OK after logging a warning.
    //
    // Deliberately not ESP_ERROR_CHECK: failing to reclaim is a lost
    // optimisation, not a boot-stopping fault.
    {
        const size_t beforeFree =
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const esp_err_t bredrRel =
            esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        if (bredrRel == ESP_OK) {
            const size_t afterFree =
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            ESP_LOGI("boot", "BR/EDR memory released: internal free %u -> %u (+%d B), largest block %u B",
                     (unsigned)beforeFree, (unsigned)afterFree,
                     (int)(afterFree - beforeFree),
                     (unsigned)heap_caps_get_largest_free_block(
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        } else {
            ESP_LOGW("boot", "BR/EDR memory release skipped: %s",
                     esp_err_to_name(bredrRel));
        }
    }
#endif

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
