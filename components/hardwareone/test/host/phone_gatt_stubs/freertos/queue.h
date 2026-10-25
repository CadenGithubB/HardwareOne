#pragma once
#include "FreeRTOS.h"
// Padded to the P4's 92 B so the host check of sizeof(Gatt) <= 9728 (internal
// RAM) is not looser than the target's.
struct StaticQueue_t {unsigned capacity=0,size=0;uint8_t target[84]{};};
static_assert(sizeof(StaticQueue_t)>=92,"stub smaller than the P4's StaticQueue_t");
using QueueHandle_t=StaticQueue_t*;
QueueHandle_t xQueueCreateStatic(unsigned,unsigned,uint8_t*,StaticQueue_t*);
int xQueueReset(QueueHandle_t);
int xQueueSend(QueueHandle_t,const void*,TickType_t);
int xQueueReceive(QueueHandle_t,void*,TickType_t);
unsigned uxQueueMessagesWaiting(QueueHandle_t);
