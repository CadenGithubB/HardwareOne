#pragma once
#include "HAL_Input.h"
#define INPUT_TASK_NAME "input"
#define INPUT_STACK_WORDS 2048
#define TASK_PRIORITY_LOW 1
#define INPUT_TASK_TAG "input"
#define tskNO_AFFINITY -1
inline int xTaskCreateLogged(void (*task)(void*), const char* name, int words,
                             void* arg, int priority, TaskHandle_t* handle,
                             const char* tag, int affinity) {
  (void)task; (void)name; (void)words; (void)arg; (void)priority; (void)tag; (void)affinity;
  ++fixture::taskCreates;
  *handle = &fixture::taskCreates;
  return pdPASS;
}
