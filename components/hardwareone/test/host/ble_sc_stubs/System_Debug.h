#pragma once
// The output calls System_BleSecureChannel.cpp makes; the harness defines them.
#include "Arduino.h"
void broadcastOutput(const String& s);
void broadcastOutputCore_Routed(const char* text, size_t len, uint8_t route);
