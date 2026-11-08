// Host harness for the P4 CPU clock switch (System_Power.cpp). The runner
// (test_power_p4_clock_switch.py) inserts the shipping p4SwitchCpuClock with
// its statics and the shipping powerSetCpuFrequencyMhz owner (I2C off) at
// the marker below. Only IDF's clock, cache, IPC and critical-section calls,
// the stack pointer and the RTOS mutex are fakes; they check the order and the
// state each call needs.
//
// Bench 2026-11-08: two clock switches in quick succession (`stt start` at
// 100 MHz) froze both cores (Core0 at _vector_table, Core1 in
// esp_ipc_isr_waiting_for_finish_cmd) and the interrupt watchdog reset the
// chip (rst:0x7). GCC had inlined p4SwitchCpuClock into the flash-resident
// owner, so the frozen-cache window fetched flash. That placement is checked
// on cross-compiled code (--placement); here: from the freeze to the unfreeze
// the switch reads only its own internal-RAM copy of the configuration (the
// same object whatever the caller's stack), and a task whose stack is not in
// internal RAM gets a refusal, with no stall, freeze or clock change.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "System_Power.h"

#define IRAM_ATTR
#define DRAM_ATTR
#define NOINLINE_ATTR __attribute__((noinline))

struct hal_utils_clk_div_t { uint32_t integer, denominator, numerator; };
struct rtc_cpu_freq_config_t {
  int source;
  uint32_t source_freq_mhz;
  hal_utils_clk_div_t div;
  uint32_t freq_mhz;
};

static std::vector<std::string> trace;
static bool interruptsOff = false, otherStalled = false, cacheFrozen = false;
static uint32_t current = 100;
static uint32_t getCpuFrequencyMhz() { return current; }
static bool displayRequiresFullSpeed() { return false; }

// The P4's internal DRAM (soc.h SOC_DRAM_LOW/HIGH) and the running task's SP.
static uintptr_t fakeSp = 0x4ff74800;
static void* esp_cpu_get_sp() { return reinterpret_cast<void*>(fakeSp); }
static bool esp_ptr_in_dram(const void* p) {
  const uintptr_t a = reinterpret_cast<uintptr_t>(p);
  return a >= 0x4ff00000 && a < 0x4ffc0000;
}

using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL_SAFE(mux) \
  (assert(!interruptsOff), (void)(mux), interruptsOff = true, trace.push_back("crit"))
#define portEXIT_CRITICAL_SAFE(mux) \
  (assert(interruptsOff && !cacheFrozen && !otherStalled), (void)(mux), interruptsOff = false, trace.push_back("uncrit"))

static bool rtc_clk_cpu_freq_mhz_to_config(uint32_t mhz, rtc_cpu_freq_config_t* out) {
  if (mhz != 100 && mhz != 200 && mhz != 400) return false;
  *out = {1, 400, {400 / mhz, 0, 0}, mhz};
  return true;
}
static void esp_clk_utils_mspi_speed_mode_sync_before_cpu_freq_switching(uint32_t src, uint32_t freq) {
  assert(interruptsOff && !otherStalled && !cacheFrozen);
  trace.push_back("before:" + std::to_string(src) + "/" + std::to_string(freq));
}
static void esp_clk_utils_mspi_speed_mode_sync_after_cpu_freq_switching(uint32_t src, uint32_t freq) {
  assert(interruptsOff && !otherStalled && !cacheFrozen);
  trace.push_back("after:" + std::to_string(src) + "/" + std::to_string(freq));
}
static void esp_ipc_isr_stall_other_cpu() {
  assert(interruptsOff && !otherStalled);
  otherStalled = true;
  trace.push_back("stall");
}
static void esp_ipc_isr_release_other_cpu() {
  assert(otherStalled && !cacheFrozen);
  otherStalled = false;
  trace.push_back("release");
}
static void esp_cache_freeze_ext_mem_cache() {
  assert(interruptsOff && otherStalled && !cacheFrozen);
  cacheFrozen = true;
  trace.push_back("freeze");
}
static void esp_cache_unfreeze_ext_mem_cache() {
  assert(cacheFrozen);
  cacheFrozen = false;
  trace.push_back("unfreeze");
}
static const rtc_cpu_freq_config_t* appliedFrom = nullptr;
static void rtc_clk_cpu_freq_set_config_fast(const rtc_cpu_freq_config_t* config) {
  assert(interruptsOff && otherStalled && cacheFrozen);
  appliedFrom = config;
  current = config->freq_mhz;
  trace.push_back("set:" + std::to_string(config->freq_mhz));
}

// FreeRTOS mutex (the owner's gate), always free here.
struct StaticSemaphore_t { int holder = 0; };
using SemaphoreHandle_t = StaticSemaphore_t*;
constexpr int pdTRUE = 1;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) { return storage; }
static int xSemaphoreTake(SemaphoreHandle_t m, int) {
  if (m->holder) return 0;
  m->holder = 1;
  return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t m) { m->holder = 0; }
static bool setCpuFrequencyMhz(uint32_t) {
  assert(!"the P4 owner must not use Arduino's unsynchronised switch");
  return false;
}

// INSERT_PRODUCTION

using Steps = std::vector<std::string>;
static void reset(uint32_t mhz) {
  trace.clear();
  current = mhz;
  appliedFrom = nullptr;
  fakeSp = 0x4ff74800;
  assert(!interruptsOff && !otherStalled && !cacheFrozen);
}

// The owner keeps its rtc_cpu_freq_config_t on its own stack. Called from two
// depths, a window that read the caller's object would see two addresses.
static NOINLINE_ATTR const rtc_cpu_freq_config_t* switchFromDeeperStack(uint32_t mhz) {
  volatile char pad[512];
  pad[0] = 1;
  assert(powerSetCpuFrequencyMhz(mhz));
  (void)pad[0];
  return appliedFrom;
}

int main() {
  // A raise and the drop right after it (the bench's failed `stt start`).
  reset(100);
  bool changed = false;
  assert(powerSetCpuFrequencyMhz(400, &changed) && changed && current == 400);
  assert(trace == Steps({"crit", "before:400/400", "stall", "freeze", "set:400", "unfreeze", "release",
                         "after:400/400", "uncrit"}));
  const rtc_cpu_freq_config_t* first = appliedFrom;
  trace.clear();
  assert(powerSetCpuFrequencyMhz(100, &changed) && changed && current == 100);
  assert(trace == Steps({"crit", "before:400/100", "stall", "freeze", "set:100", "unfreeze", "release",
                         "after:400/100", "uncrit"}));
  assert(appliedFrom == first);
  reset(100);
  assert(switchFromDeeperStack(400) == first);  // the switch's own copy, not the caller's
  assert(first->freq_mhz == 400);

  // A task whose stack is in PSRAM (xTaskCreateWithCaps) or anywhere outside
  // internal DRAM: refused before the critical section, the clock unchanged.
  for (uintptr_t sp : {uintptr_t(0x48100000), uintptr_t(0x4ffc0000), uintptr_t(0x4fefff00)}) {
    reset(100);
    fakeSp = sp;
    changed = true;
    assert(!powerSetCpuFrequencyMhz(400, &changed));
    assert(!changed && current == 100 && trace.empty() && sCpuFrequencyMutex->holder == 0);
  }
  // An unsupported clock never reaches the switch.
  reset(100);
  assert(!powerSetCpuFrequencyMhz(123) && trace.empty() && current == 100);
  std::puts("PASS: P4 clock switch reads only internal RAM in the frozen window; non-internal stacks refused");
  return 0;
}
