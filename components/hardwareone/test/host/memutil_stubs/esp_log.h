#pragma once

inline void esp_logw_host_stub(const char*, const char*, ...) {}

#define ESP_LOGW(...) esp_logw_host_stub(__VA_ARGS__)
