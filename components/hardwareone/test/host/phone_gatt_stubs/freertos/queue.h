#pragma once
#include "FreeRTOS.h"
struct StaticQueue_t {unsigned capacity=0,size=0;};
using QueueHandle_t=StaticQueue_t*;
QueueHandle_t xQueueCreateStatic(unsigned,unsigned,uint8_t*,StaticQueue_t*);
int xQueueReset(QueueHandle_t);
int xQueueSend(QueueHandle_t,const void*,TickType_t);
int xQueueReceive(QueueHandle_t,void*,TickType_t);
unsigned uxQueueMessagesWaiting(QueueHandle_t);
