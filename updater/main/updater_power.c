#include "updater_power.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/*
 * Power interlock for the recovery updater.
 *
 * The question this module answers is narrow: is it safe to START writing an
 * application partition right now? The strength of the answer is a property of
 * the board, not of this code, so the backend is chosen by
 * CONFIG_HW1_UPDATER_POWER_* which each updater/boards/<board>.defaults
 * declares.
 *
 * Note what an interrupted write actually costs, because it bounds how
 * paranoid this needs to be: the write targets ota_0 while otadata still
 * selects the previous image, so losing power mid-write leaves an invalid
 * ota_0 and the device comes back on the factory recovery image. The interlock
 * prevents a wasted round trip, not a brick.
 */

#if CONFIG_HW1_UPDATER_POWER_GAUGE
/* ---------------------------------------------------------------------------
 * FeatherS3 class: a real USB-presence pin plus a MAX17048 fuel gauge.
 * ------------------------------------------------------------------------ */
#include "driver/gpio.h"
#include "driver/i2c.h"

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
    status->usb_sense_available = true;

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
    status->battery_present = status->battery_valid;
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

#elif CONFIG_HW1_UPDATER_POWER_ADC
/* ---------------------------------------------------------------------------
 * Adafruit Feather ESP32 V2 class: VBAT through a 2:1 divider on ADC1, and
 * nothing at all that reports USB presence.
 *
 * The board simply does not expose a VBUS sense line -- the main firmware says
 * as much in System_Battery.cpp ("the divider can't tell us whether VBUS is
 * connected, only what the cell terminal sits at"), and the Feather V2 entry
 * in System_BuildConfig.h sets BATTERY_VBUS_SENSE_PIN to -1. So the strict
 * "USB present, therefore safe" shortcut is unavailable and the decision has
 * to be made from cell voltage alone.
 *
 * The trap that shapes the policy below: a unit deployed on mains power with
 * NO battery fitted reads a near-zero divider voltage. A naive "refuse below
 * 3.6 V" gate would refuse every recovery upload on such a unit, forever, with
 * no cable to argue with -- turning a safety interlock into the exact brick it
 * was meant to prevent. So a reading below the absent threshold is treated as
 * proof that the power is coming from USB: the board is executing this code,
 * therefore it is powered, therefore a cell reading 0.4 V is not the thing
 * powering it.
 * ------------------------------------------------------------------------ */
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

#define HW1_ADC_SAMPLES 16
#define HW1_ADC_DEFAULT_VREF_MV 1100

static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_adc_cali_handle;
static bool s_adc_ready;

static esp_err_t ensure_adc(void)
{
    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
        .default_vref = HW1_ADC_DEFAULT_VREF_MV,
    };
    esp_err_t err;

    if (s_adc_ready) {
        return ESP_OK;
    }
    err = adc_oneshot_new_unit(&unit_config, &s_adc_handle);
    if (err != ESP_OK) {
        return err;
    }
    err = adc_oneshot_config_channel(
        s_adc_handle, (adc_channel_t)CONFIG_HW1_UPDATER_BATTERY_ADC_CHANNEL,
        &channel_config);
    if (err != ESP_OK) {
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return err;
    }
    /* Calibration is best-effort. Without an eFuse calibration value the line
     * fitting scheme falls back to the default Vref, which is accurate enough
     * for a go/no-go threshold; a failure here must not block recovery. */
    if (adc_cali_create_scheme_line_fitting(&cali_config,
                                            &s_adc_cali_handle) != ESP_OK) {
        s_adc_cali_handle = NULL;
    }
    s_adc_ready = true;
    return ESP_OK;
}

esp_err_t updater_power_sample(updater_power_status_t *status,
                               char *reason, size_t reason_size)
{
    int raw_sum = 0;
    int millivolts = 0;
    esp_err_t err;
    int i;

    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(status, 0, sizeof(*status));
    /* Stated explicitly rather than left as a zeroed field: callers must not
     * read usb_present on this board, and /status must not imply otherwise. */
    status->usb_sense_available = false;

    err = ensure_adc();
    if (err != ESP_OK) {
        if (reason != NULL && reason_size != 0) {
            snprintf(reason, reason_size, "battery ADC unavailable: %s",
                     esp_err_to_name(err));
        }
        return err;
    }

    for (i = 0; i < HW1_ADC_SAMPLES; i++) {
        int raw = 0;
        err = adc_oneshot_read(
            s_adc_handle, (adc_channel_t)CONFIG_HW1_UPDATER_BATTERY_ADC_CHANNEL,
            &raw);
        if (err != ESP_OK) {
            if (reason != NULL && reason_size != 0) {
                snprintf(reason, reason_size, "battery ADC read failed: %s",
                         esp_err_to_name(err));
            }
            return err;
        }
        raw_sum += raw;
    }
    raw_sum /= HW1_ADC_SAMPLES;

    if (s_adc_cali_handle != NULL) {
        err = adc_cali_raw_to_voltage(s_adc_cali_handle, raw_sum, &millivolts);
        if (err != ESP_OK) {
            if (reason != NULL && reason_size != 0) {
                snprintf(reason, reason_size,
                         "battery ADC calibration failed: %s",
                         esp_err_to_name(err));
            }
            return err;
        }
    } else {
        /* Uncalibrated linear fallback across the 12 dB range. */
        millivolts = (raw_sum * HW1_ADC_DEFAULT_VREF_MV * 4) / 4095;
    }

    status->battery_millivolts =
        (uint32_t)((millivolts * CONFIG_HW1_UPDATER_BATTERY_DIVIDER_X100) / 100);
    status->battery_present =
        status->battery_millivolts >= CONFIG_HW1_UPDATER_BATTERY_ABSENT_MILLIVOLTS;
    status->battery_valid = true;
    if (reason != NULL && reason_size != 0) {
        if (status->battery_present) {
            snprintf(reason, reason_size, "fresh sample: battery %" PRIu32 " mV",
                     status->battery_millivolts);
        } else {
            snprintf(reason, reason_size,
                     "fresh sample: %" PRIu32 " mV on VBAT, no cell attached",
                     status->battery_millivolts);
        }
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
    if (!status.battery_present) {
        /* See the block comment above: running code with no usable cell means
         * the power is external, which is the safest state this board can be
         * in. Allow, and say why, so the operator is not left guessing. */
        if (reason != NULL && reason_size != 0) {
            snprintf(reason, reason_size,
                     "no cell attached (%" PRIu32 " mV on VBAT); running on "
                     "external power",
                     status.battery_millivolts);
        }
        return ESP_OK;
    }
    if (status.battery_millivolts < CONFIG_HW1_UPDATER_MIN_BATTERY_MILLIVOLTS) {
        if (reason != NULL && reason_size != 0) {
            snprintf(reason, reason_size,
                     "battery %" PRIu32 " mV is below required %d mV and this "
                     "board cannot sense USB",
                     status.battery_millivolts,
                     CONFIG_HW1_UPDATER_MIN_BATTERY_MILLIVOLTS);
        }
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

#elif CONFIG_HW1_UPDATER_POWER_NONE
/* ---------------------------------------------------------------------------
 * Adafruit QT Py ESP32 class: no battery connector, no gauge, no divider.
 *
 * There is nothing to sample. The board cannot be running on anything but
 * external power, so the interlock reports that and allows the write. The
 * alternative -- refusing because no reading is available -- would make
 * recovery permanently impossible on this hardware, which inverts the point
 * of the check.
 * ------------------------------------------------------------------------ */
esp_err_t updater_power_sample(updater_power_status_t *status,
                               char *reason, size_t reason_size)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(status, 0, sizeof(*status));
    status->usb_sense_available = false;
    status->battery_valid = false;
    status->battery_present = false;
    if (reason != NULL && reason_size != 0) {
        snprintf(reason, reason_size,
                 "board has no battery or USB sense; external power assumed");
    }
    return ESP_OK;
}

esp_err_t updater_power_require_safe(char *reason, size_t reason_size)
{
    updater_power_status_t status;
    return updater_power_sample(&status, reason, reason_size);
}

#else
#error "No recovery power-sense backend selected. Set CONFIG_HW1_UPDATER_POWER_* \
        in updater/boards/<board>.defaults."
#endif
