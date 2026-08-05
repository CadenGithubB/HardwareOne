#include "updater_power.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define HW1_VBUS_SENSE_GPIO GPIO_NUM_34
#define HW1_GAUGE_I2C_PORT I2C_NUM_0
#define HW1_GAUGE_SDA_GPIO GPIO_NUM_8
#define HW1_GAUGE_SCL_GPIO GPIO_NUM_9
#define HW1_GAUGE_ADDRESS 0x36u
#define HW1_GAUGE_SOC_REGISTER 0x04u
#define HW1_GAUGE_TIMEOUT_MS 100u

static bool s_i2c_installed;

static esp_err_t ensure_i2c(void)
{
    i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = HW1_GAUGE_SDA_GPIO,
        .scl_io_num = HW1_GAUGE_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
        .clk_flags = 0,
    };
    esp_err_t err;
    if (s_i2c_installed) {
        return ESP_OK;
    }
    err = i2c_param_config(HW1_GAUGE_I2C_PORT, &config);
    if (err == ESP_OK) {
        err = i2c_driver_install(HW1_GAUGE_I2C_PORT, config.mode, 0, 0, 0);
    }
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_i2c_installed = true;
        return ESP_OK;
    }
    return err;
}

esp_err_t updater_power_sample(updater_power_status_t *status,
                               char *reason, size_t reason_size)
{
    uint8_t reg = HW1_GAUGE_SOC_REGISTER;
    uint8_t raw[2] = {0};
    esp_err_t err;
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(status, 0, sizeof(*status));

    gpio_config_t gpio = {
        .pin_bit_mask = 1ULL << HW1_VBUS_SENSE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&gpio);
    if (err != ESP_OK) {
        if (reason != NULL && reason_size != 0) {
            snprintf(reason, reason_size, "VBUS GPIO sample failed: %s",
                     esp_err_to_name(err));
        }
        return err;
    }
    status->usb_present = gpio_get_level(HW1_VBUS_SENSE_GPIO) == 1;
    if (status->usb_present) {
        if (reason != NULL && reason_size != 0) {
            snprintf(reason, reason_size, "fresh sample: USB present");
        }
        return ESP_OK;
    }

    err = ensure_i2c();
    if (err == ESP_OK) {
        err = i2c_master_write_read_device(
            HW1_GAUGE_I2C_PORT, HW1_GAUGE_ADDRESS, &reg, sizeof(reg),
            raw, sizeof(raw), pdMS_TO_TICKS(HW1_GAUGE_TIMEOUT_MS));
    }
    if (err != ESP_OK) {
        if (reason != NULL && reason_size != 0) {
            snprintf(reason, reason_size,
                     "USB absent and fresh MAX17048 SOC unavailable: %s",
                     esp_err_to_name(err));
        }
        return err;
    }
    status->battery_percent = (float)raw[0] + ((float)raw[1] / 256.0f);
    status->battery_valid = status->battery_percent >= 0.0f &&
                            status->battery_percent <= 100.0f;
    if (!status->battery_valid) {
        if (reason != NULL && reason_size != 0) {
            snprintf(reason, reason_size,
                     "MAX17048 returned invalid SOC %.2f%%",
                     (double)status->battery_percent);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (reason != NULL && reason_size != 0) {
        snprintf(reason, reason_size, "fresh sample: battery %.1f%%",
                 (double)status->battery_percent);
    }
    return ESP_OK;
}

esp_err_t updater_power_require_safe(char *reason, size_t reason_size)
{
    updater_power_status_t status;
    esp_err_t err = updater_power_sample(&status, reason, reason_size);
    if (err != ESP_OK) {
        return err;
    }
    if (status.usb_present) {
        return ESP_OK;
    }
    if (!status.battery_valid ||
        status.battery_percent < CONFIG_HW1_UPDATER_MIN_BATTERY_PERCENT) {
        if (reason != NULL && reason_size != 0) {
            snprintf(reason, reason_size,
                     "USB absent and battery %.1f%% is below required %d%%",
                     (double)status.battery_percent,
                     CONFIG_HW1_UPDATER_MIN_BATTERY_PERCENT);
        }
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
