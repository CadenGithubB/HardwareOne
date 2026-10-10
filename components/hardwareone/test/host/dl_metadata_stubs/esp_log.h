#pragma once
#include <stdio.h>
#define ESP_LOGE(tag, fmt, ...) ((void)fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__))
#define ESP_LOGW(tag, fmt, ...) ((void)fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__))
#define ESP_LOGI(tag, fmt, ...) ((void)fprintf(stderr, "I %s: " fmt "\n", tag, ##__VA_ARGS__))
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGV(tag, fmt, ...) ((void)0)
#define ESP_ERROR_CHECK(x) ((void)(x))
