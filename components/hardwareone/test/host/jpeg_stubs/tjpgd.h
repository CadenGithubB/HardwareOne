#pragma once
#include <stddef.h>
#include <stdint.h>
#define JD_USE_SCALE CONFIG_JD_USE_SCALE
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {JDR_OK, JDR_INTR, JDR_INP, JDR_MEM1, JDR_MEM2, JDR_PAR, JDR_FMT1} JRESULT;
typedef struct {uint16_t left, right, top, bottom;} JRECT;
typedef struct {void* device; unsigned width, height;} JDEC;
JRESULT jd_prepare(JDEC*, size_t (*)(JDEC*, uint8_t*, size_t), void*, size_t, void*);
JRESULT jd_decomp(JDEC*, int (*)(JDEC*, void*, JRECT*), uint8_t);
#ifdef __cplusplus
}
#endif
