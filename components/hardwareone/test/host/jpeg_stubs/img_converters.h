#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
typedef enum { PIXFORMAT_JPEG, PIXFORMAT_RGB888, PIXFORMAT_RGB565, PIXFORMAT_GRAYSCALE, PIXFORMAT_YUV422 } pixformat_t;
typedef struct { uint8_t* buf; size_t len; size_t width; size_t height; pixformat_t format; } camera_fb_t;
#ifdef __cplusplus
extern "C" {
#endif
bool fmt2rgb888(const uint8_t*, size_t, pixformat_t, uint8_t*);
#ifdef __cplusplus
}
#endif
