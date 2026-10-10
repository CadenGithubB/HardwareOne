#pragma once

#include "HAL_Input.h"

// Input producer operations. UI/transport readers use HAL_Input.h instead.
bool inputCacheInit();
bool inputCacheInvalidate();
bool inputCacheReset(uint32_t buttons, int joyX, int joyY, bool clearPresses);
bool inputPublishState(uint32_t buttons, int joyX, int joyY,
                       uint32_t pressedEdges, bool activity, unsigned long stamp);
bool inputPublishGamepadState(uint32_t buttons, int joyX, int joyY,
                              uint32_t interruptPresses, unsigned long stamp);
