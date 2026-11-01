#pragma once
#include <stdint.h>
struct TouchNavigation { bool up=false, down=false, left=false, right=false; uint32_t buttons=0; };
// UI owner/context changes discard queued input and any held finger.
void oledTouchContext(uint32_t context);
TouchNavigation oledTouchRead();
uint32_t oledWizardButtonPresses(uint32_t& previous, bool& initialized);
TouchNavigation oledWizardTouchNavigation();
