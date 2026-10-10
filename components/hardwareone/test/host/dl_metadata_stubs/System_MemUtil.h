#pragma once
// Host fake of HardwareOne's System_MemUtil.h API that stt_dl_metadata.cpp uses.
#include <atomic>
#include <cstddef>
#include <cstdint>
enum class AllocPref : uint8_t { PreferPSRAM = 0, PreferInternal = 1, RequirePSRAM = 2, RequireInternal = 3, DefaultHeap = 4 };
using AllocPolicy = AllocPref;
bool psramAvailableRuntime();
bool psramBypassEnabled();
extern std::atomic<uint32_t> gPsAllocFallbacks;
extern "C" void memAllocDebug(const char *op, void *ptr, size_t size, bool requestedPS, bool usedPS, bool fellBack, const char *tag);
void *ps_alloc(size_t size, AllocPolicy policy, const char *tag);
void ps_free(void *ptr);
