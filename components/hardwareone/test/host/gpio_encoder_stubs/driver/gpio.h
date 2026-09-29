#pragma once
#include "HAL_Input.h"
using gpio_num_t = int;
#define GPIO_IS_VALID_GPIO(pin) ((pin) >= 0 && (pin) < 55)
inline int gpio_reset_pin(gpio_num_t pin) { ++fixture::resets[pin]; return 0; }
