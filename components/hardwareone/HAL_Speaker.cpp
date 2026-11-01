#include "HAL_Speaker.h"
#include "System_BuildConfig.h"
#if !ENABLE_SPEAKER
esp_err_t speakerPlayTone(uint32_t, uint32_t, uint8_t) {
  return ESP_ERR_NOT_SUPPORTED;
}
#endif
