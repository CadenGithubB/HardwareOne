#include "radio_backend.h"

#include "sdkconfig.h"
#include "esp_log.h"

#if CONFIG_ESP_WIFI_REMOTE_ENABLED
#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_hosted.h"
#include "esp_now_hosted_host.h"
#include "esp32-hal-hosted.h"
#endif

namespace {
constexpr const char *kTag = "HW1_RADIO";

#if CONFIG_ESP_WIFI_REMOTE_ENABLED
// ESP32-P4X-EYE / EYE-MB v2.3 board profile. Keep wiring here, rather than
// putting CPU or board checks into the shared mesh protocol.
namespace eye {
constexpr gpio_num_t kC6Boot = GPIO_NUM_33;
constexpr gpio_num_t kC6UartRx = GPIO_NUM_35; // Also the P4 BOOT strap.
constexpr gpio_num_t kC6UartTx = GPIO_NUM_36;
constexpr int8_t kSdioClk = 28;
constexpr int8_t kSdioCmd = 27;
constexpr int8_t kSdioD0 = 29;
constexpr int8_t kSdioD1 = 30;
constexpr int8_t kSdioD2 = 31;
constexpr int8_t kSdioD3 = 32;
constexpr int8_t kC6Enable = 9;
} // namespace eye

bool sPrepared = false;
esp_hosted_coprocessor_fwver_t sVersion = {};

esp_err_t prepareEyeBoard()
{
    // Set the output latch before enabling it: C6 BOOT must be high when
    // Hosted pulses EN. Leave the ROM UART idle; never pull P4 BOOT low.
    ESP_RETURN_ON_ERROR(gpio_set_level(eye::kC6Boot, 1), kTag, "C6 BOOT level");
    ESP_RETURN_ON_ERROR(gpio_set_direction(eye::kC6Boot, GPIO_MODE_OUTPUT),
                        kTag, "C6 BOOT direction");
    ESP_RETURN_ON_ERROR(gpio_set_direction(eye::kC6UartRx, GPIO_MODE_INPUT),
                        kTag, "C6 UART RX direction");
    ESP_RETURN_ON_ERROR(gpio_set_pull_mode(eye::kC6UartRx, GPIO_PULLUP_ONLY),
                        kTag, "C6 UART RX pull-up");
    ESP_RETURN_ON_ERROR(gpio_set_direction(eye::kC6UartTx, GPIO_MODE_INPUT),
                        kTag, "C6 UART TX direction");

    // Arduino's generic esp32p4 variant overrides the Kconfig SDIO pins with
    // a different board's wiring. Set its pin map before hostedInitWiFi().
    if (!hostedSetPins(eye::kSdioClk, eye::kSdioCmd, eye::kSdioD0,
                       eye::kSdioD1, eye::kSdioD2, eye::kSdioD3, eye::kC6Enable)) {
        ESP_LOGE(kTag, "Could not set EYE SDIO pins before Hosted startup");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
#endif
} // namespace

esp_err_t hw1MeshRadioPrepare(void)
{
#if CONFIG_ESP_WIFI_REMOTE_ENABLED
    // initArduino() resets the global log level after Hosted's constructor
    // muted per-RPC INFO traffic. Restore only those tags; retain diagnostics
    // from the transport and all RPC warnings/errors.
    esp_log_level_set("rpc_core", ESP_LOG_WARN);
    esp_log_level_set("rpc_rsp", ESP_LOG_WARN);
    esp_log_level_set("rpc_evt", ESP_LOG_WARN);
    if (sPrepared) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(prepareEyeBoard(), kTag, "EYE board preparation failed");

    // This Arduino helper calls esp_hosted_init/connect_to_slave and records
    // its lifecycle state. It does not call esp_wifi_init/start. Calling the
    // raw Hosted APIs alone would leave Arduino unaware of this preparation.
    if (!hostedInitWiFi()) {
        ESP_LOGE(kTag, "Arduino Hosted transport initialization failed");
        return ESP_FAIL;
    }
    // The helper's version diagnostic is advisory; require a successful RPC
    // before allowing the application to continue into Wi-Fi initialization.
    ESP_RETURN_ON_ERROR(esp_hosted_get_coprocessor_fwversion(&sVersion),
                        kTag, "C6 firmware version query failed");
    sPrepared = true;
    ESP_LOGI(kTag, "PREPARED backend=hosted board=esp32-p4x-eye coprocessor=%" PRIu32
                  ".%" PRIu32 ".%" PRIu32,
             sVersion.major1, sVersion.minor1, sVersion.patch1);
#endif
    return ESP_OK;
}

void hw1MeshRadioPrintStats(void)
{
#if CONFIG_ESP_WIFI_REMOTE_ENABLED
    esp_now_hosted_host_stats_t bridge = {};
    esp_now_hosted_host_get_stats(&bridge);
    ESP_LOGI(kTag, "RADIO_STATS backend=hosted prepared=%u arduino_hosted=%u",
             static_cast<unsigned>(sPrepared),
             static_cast<unsigned>(hostedIsInitialized()));
    ESP_LOGI(kTag, "BRIDGE_STATS malformed=%" PRIu32 " stale_responses=%" PRIu32
                  " event_drops=%" PRIu32 " request_timeouts=%" PRIu32
                  " transport_errors=%" PRIu32,
             bridge.malformed_messages, bridge.stale_responses,
             bridge.events_dropped, bridge.request_timeouts, bridge.transport_errors);
#else
    ESP_LOGI(kTag, "RADIO_STATS backend=native");
#endif
}
