// System_RestartQuiesceCore.h - what an intentional restart does to the SDMMC
// host before esp_restart().
//
// On the ESP32-P4 a software restart is a CPU reset. esp_restart_noos() idles
// the GDMA engines before it resets them, but it pulses the SDMMC reset with no
// idle check, and the SDMMC host is an AXI master too. A restart taken while
// its DMA is moving data (the C6 link on slot 1 streaming, or an SD card write
// on slot 0) left the P4's SDMMC data path dead after the reboot: commands
// worked, every data transfer timed out on both slots, a fresh C6 power-on and
// a full Hosted/SDMMC re-init did not help, and only a chip reset (esptool,
// USB) recovered it (bench log 2026-11-08).
//
// So before restarting, the owner stops the SDMMC users at their source:
//   1. silenceRadio(): no SDIO RPC inside esp_restart() (the wifi_remote
//      esp_wifi_stop shutdown handler is unregistered, as Hosted's own restart
//      helper does) and the C6 is held in reset, so it stops sending.
//   2. lockStorage(timeout): the filesystem lock is taken and never given
//      back, so no SD operation starts and the one running has finished.
//   3. A drain wait, counted from the moment the radio went quiet: a transfer
//      the hold cut off mid-block ends with the SDMMC driver's 1 s timeout,
//      and the driver stops its DMA on that error path.
// Then the caller restarts. Every step is bounded; a refused or slow step does
// not stop the restart.
//
// Dependency-free on purpose (no ESP-IDF or Arduino), so the host test drives
// it with a fake clock (test_restart_quiesce_core.cpp).
#pragma once
#include <stdint.h>

namespace hw1 {
namespace restartq {

struct QuiescePolicy {
  uint32_t storageLockTimeoutMs = 2000;  // a filesystem operation still running gets this long
  uint32_t drainMs = 1100;               // IDF's SDMMC transfer timeout (1 s) plus margin
};

struct QuiesceResult {
  bool storageLocked = false;
  uint32_t drainedMs = 0;  // time from the radio going quiet to the return
};

// silenceRadio(): void(); lockStorage(uint32_t timeoutMs): bool;
// now(): uint32_t ms (wraps); sleep(uint32_t ms): void.
template <typename SilenceRadio, typename LockStorage, typename Now, typename Sleep>
QuiesceResult quiesceSdmmcForRestart(SilenceRadio silenceRadio, LockStorage lockStorage, Now now,
                                     Sleep sleep, const QuiescePolicy& policy = QuiescePolicy()) {
  QuiesceResult result;
  silenceRadio();
  const uint32_t quietAt = now();
  result.storageLocked = lockStorage(policy.storageLockTimeoutMs);
  const uint32_t elapsed = now() - quietAt;  // unsigned: survives a millis() wrap
  if (elapsed < policy.drainMs) sleep(policy.drainMs - elapsed);
  result.drainedMs = now() - quietAt;
  return result;
}

}  // namespace restartq
}  // namespace hw1
