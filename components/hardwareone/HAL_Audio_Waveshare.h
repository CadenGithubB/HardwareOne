#pragma once
#include <stddef.h>
#include <stdint.h>
// One lifetime claim for the board's shared I2S clock/data pins. Callers release
// only their successful claim, after their channel and codec are stopped.
bool waveshareAudioTryClaim();
void waveshareAudioRelease();
// Called only by HAL_Audio under its existing I2S hardware mutex.
bool waveshareMicStart(uint32_t rate);
void waveshareMicStop();
size_t waveshareMicRead(int16_t* out,size_t samples,uint32_t timeoutMs);
uint32_t waveshareMicOverruns();
