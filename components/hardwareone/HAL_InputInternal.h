#pragma once

#include "HAL_Input.h"

void inputTouchPublish(uint8_t count, uint8_t id, uint16_t x, uint16_t y, uint32_t stamp);
void inputTouchCancel();

// Input producer operations. UI/transport readers use HAL_Input.h instead.
bool inputCacheInit();
bool inputCacheInvalidate();
bool inputCacheReset(uint32_t buttons, int joyX, int joyY, bool clearPresses);
// navHidden: see InputCache::navHidden (the ANO's; 0 for every other backend).
bool inputPublishState(uint32_t buttons, int joyX, int joyY,
                       uint32_t pressedEdges, bool activity, unsigned long stamp,
                       uint32_t navHidden = 0);
bool inputPublishGamepadState(uint32_t buttons, int joyX, int joyY,
                              uint32_t interruptPresses, unsigned long stamp);
