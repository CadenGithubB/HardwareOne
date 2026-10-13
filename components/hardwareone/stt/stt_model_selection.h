#pragma once

// Firmware callers resolve System_BuildConfig.h first. Standalone model tools
// keep the original 5x5 identity when no profile selection is supplied.
#if defined(ENABLE_STT_MODEL_15X5_GEN2) && ENABLE_STT_MODEL_15X5_GEN2
#include "stt_model_identity_15x5_gen2.h"
#ifndef HW1_STT_MODEL_PATH
#define HW1_STT_MODEL_PATH "/sd/STT Models/quartznet15x5-gen2-25397.p4.stt"
#endif
#else
#include "stt_model_identity.h"
#ifndef HW1_STT_MODEL_PATH
#define HW1_STT_MODEL_PATH "/STT Models/quartznet5x5.p4.stt"
#endif
#endif
