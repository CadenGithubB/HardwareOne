#pragma once
#include "esp_err.h"
typedef void* jpeg_decoder_handle_t;
typedef enum { JPEG_DEC_ALLOC_INPUT_BUFFER, JPEG_DEC_ALLOC_OUTPUT_BUFFER } jpeg_dec_buffer_alloc_direction_t;
typedef struct { jpeg_dec_buffer_alloc_direction_t buffer_direction; } jpeg_decode_memory_alloc_cfg_t;
typedef struct { int intr_priority; int timeout_ms; } jpeg_decode_engine_cfg_t;
enum { JPEG_DECODE_OUT_FORMAT_RGB888 = 1, JPEG_DECODE_OUT_FORMAT_YUV444 = 4, JPEG_DEC_RGB_ELEMENT_ORDER_RGB = 2, JPEG_YUV_RGB_CONV_STD_BT601 = 3 };
typedef struct { int output_format; int rgb_order; int conv_std; } jpeg_decode_cfg_t;
void* jpeg_alloc_decoder_mem(size_t, const jpeg_decode_memory_alloc_cfg_t*, size_t*);
esp_err_t jpeg_new_decoder_engine(const jpeg_decode_engine_cfg_t*, jpeg_decoder_handle_t*);
esp_err_t jpeg_del_decoder_engine(jpeg_decoder_handle_t);
esp_err_t jpeg_decoder_process(jpeg_decoder_handle_t, const jpeg_decode_cfg_t*, const uint8_t*, uint32_t, uint8_t*, uint32_t, uint32_t*);
