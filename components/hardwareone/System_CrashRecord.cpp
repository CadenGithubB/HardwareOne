#include "System_CrashRecord.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_rom_sys.h"
#include "esp_app_desc.h"
#include "esp_private/panic_internal.h"   // g_panic_abort, g_panic_abort_details
#include "esp32-hal.h"                    // set_arduino_panic_handler / arduino_panic_info_t
#include "System_Filesystem.h"            // appendLineWithCap — boot-time persist ONLY, never panic context
#include "System_Clock.h"                 // Clock::isValidEpoch — one epoch-validity vocabulary

// =============================================================================
// RTC layout
// =============================================================================
// Two independent records, each with its own guard word:
//
//   PANIC record  — written from panic context by __wrap_esp_panic_handler.
//   STATE record  — written from normal boot/loop context (phase, counters).
//
// They are separate because their write contexts and lifetimes differ: the panic
// record is consumed and invalidated on the next boot, while the state record
// persists across boots to carry the consecutive counter and repeat detection.
//
// GUARD WORDS ARE LAYOUT-DERIVED. RTC slow memory is NOT cleared by erasing
// flash — only by a true cold power-on. So reflashing a build that reorders
// these fields, while the device stays powered, would otherwise consume the old
// bytes through a still-valid magic and emit a plausible, WRONG record. Folding
// the field sizes into the magic makes any layout change self-invalidating.
#define CRASH_LAYOUT_VERSION 2u
#define CRASH_REASON_MAX     128   // assert text runs long: "assert failed: xQueueGenericSend queue.c:832 (pxQueue)"
#define CRASH_ELFSHA_MAX     16    // CONFIG_APP_RETRIEVE_LEN_ELF_SHA=9 -> 9 chars + NUL
#define CRASH_BT_MAX         12    // Arduino walks up to 60 frames; 12 is plenty to place a fault and costs 48 B

#define CRASH_PANIC_MAGIC \
  (0x43505200u ^ (CRASH_LAYOUT_VERSION * 0x9E37u) ^ (CRASH_REASON_MAX << 8) ^ \
   (CRASH_BT_MAX << 4) ^ CRASH_ELFSHA_MAX)
#define CRASH_STATE_MAGIC \
  (0x43535400u ^ (CRASH_LAYOUT_VERSION * 0x9E37u))

// --- panic-time record -------------------------------------------------------
// `volatile` is load-bearing, not decoration. The magic MUST be observably
// written last, and these are plain statics the compiler is otherwise free to
// reorder. A torn record with a valid magic is worse than no record: it reads as
// authoritative. Paired with an explicit barrier at the write site.
static RTC_NOINIT_ATTR volatile uint32_t rtcPanicMagic;
static RTC_NOINIT_ATTR char              rtcPanicReason[CRASH_REASON_MAX];
static RTC_NOINIT_ATTR char              rtcPanicElfSha[CRASH_ELFSHA_MAX];
static RTC_NOINIT_ATTR uint32_t          rtcPanicAddr;
static RTC_NOINIT_ATTR uint8_t           rtcPanicCore;
static RTC_NOINIT_ATTR uint8_t           rtcPanicWasAbort;
static RTC_NOINIT_ATTR uint8_t           rtcPanicBtLen;
static RTC_NOINIT_ATTR uint8_t           rtcPanicBtCorrupt;
static RTC_NOINIT_ATTR uint32_t          rtcPanicBt[CRASH_BT_MAX];

// --- boot-managed state record ----------------------------------------------
static RTC_NOINIT_ATTR volatile uint32_t rtcStateMagic;
static RTC_NOINIT_ATTR uint8_t           rtcBootPhase;         // advanced during THIS boot
static RTC_NOINIT_ATTR uint32_t          rtcConsecutive;       // consecutive fault resets
static RTC_NOINIT_ATTR uint32_t          rtcLastSignature;     // signature of the previous crash
static RTC_NOINIT_ATTR uint32_t          rtcRepeatCount;       // boots in a row with that signature
static RTC_NOINIT_ATTR char              rtcStateElfSha[CRASH_ELFSHA_MAX]; // build that last reached consume

// --- decoded view of the PREVIOUS boot (plain RAM, valid after consume) ------
static bool     sHavePrev       = false;
static uint8_t  sPrevPhase      = CRASH_PHASE_UNKNOWN;
static uint32_t sSignature      = 0;
static uint32_t sRepeat         = 0;
static uint32_t sConsecutive    = 0;
static uint32_t sResetReason    = 0;   // retained for isFaultReset gating, not rendered (see crashRecordDetail)
static char     sReason[CRASH_REASON_MAX] = {0};
static char     sElfSha[CRASH_ELFSHA_MAX] = {0};
static uint32_t sAddr           = 0;
static uint8_t  sCore           = 0;
static bool     sElfShaMatches  = false;
static bool     sPreviousBuildMatches = false;
static bool     sHealthyMarked  = false;
static bool     sWasAbort       = false;
static uint8_t  sBtLen          = 0;
static bool     sBtCorrupt      = false;
static uint32_t sBt[CRASH_BT_MAX] = {0};

static const char* const kPhaseNames[] = {
  "unknown", "pre-serial", "fs+settings", "debug-buffers", "hardware",
  "banner+oled", "first-time-setup", "network", "autostart", "boot-diag", "running"
};

const char* crashPhaseName(uint8_t phase) {
  if (phase >= CRASH_PHASE__COUNT) return "invalid";
  return kPhaseNames[phase];
}

// Hand-rolled bounded copy. Used from panic context, where the discipline is to
// call as little as possible — this avoids depending on where libc's strncpy
// happens to live or what it does. Always NUL-terminates.
static void IRAM_ATTR crashCopyStr(char* dst, const char* src, size_t cap) {
  if (!dst || cap == 0) return;
  size_t i = 0;
  if (src) {
    for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
  }
  dst[i] = '\0';
}

static uint32_t fnv1a(const void* data, size_t len, uint32_t seed) {
  const uint8_t* p = (const uint8_t*)data;
  uint32_t h = seed;
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

// =============================================================================
// Panic-context capture
// =============================================================================
// Invoked from Arduino's __wrap_esp_panic_handler (esp32-hal-misc.c), which our
// -Wl,--wrap=esp_panic_handler flag activates. Arduino has already walked the
// backtrace for us by this point.
//
// WHAT IS SAFE HERE — the other core is stalled, the TIMG watchdogs are off, the
// RTC WDT is armed at ~10 s (the hard budget for everything below), and the
// flash cache has been re-enabled by panic_enable_cache(). So reading rodata and
// doing plain stores is fine. FORBIDDEN: malloc, any locking FreeRTOS call,
// printf, LittleFS, and esp_flash_*/esp_partition_* (the flash guard IPCs the
// stalled core and deadlocks). Nothing below does any of those.
static void crashPanicHook(arduino_panic_info_t* info, void* /*arg*/) {
  // Invalidate first. If we fault partway through the capture, the next boot
  // must see an INVALID record rather than a half-written one that reads as
  // authoritative. PANIC_ENTRY_COUNT_MAX is 2, so a faulting hook gets exactly
  // one retry before a hard restart — magic-cleared-first is what makes that
  // survivable.
  rtcPanicMagic = 0;
  __asm__ __volatile__("" ::: "memory");

  if (info) {
    rtcPanicAddr      = (uint32_t)(uintptr_t)info->pc;
    rtcPanicCore      = (uint8_t)info->core;
    rtcPanicBtCorrupt = info->backtrace_corrupt ? 1 : 0;
    uint8_t n = 0;
    for (; n < CRASH_BT_MAX && n < info->backtrace_len; n++) rtcPanicBt[n] = info->backtrace[n];
    rtcPanicBtLen = n;
  } else {
    rtcPanicAddr = 0;
    rtcPanicCore = 0xFF;
    rtcPanicBtLen = 0;
    rtcPanicBtCorrupt = 0;
  }

  // THE IMPORTANT PART. For the whole abort class — every configASSERT, every
  // ESP_ERROR_CHECK failure, heap-corruption aborts, __stack_chk_fail, and the
  // FreeRTOS stack-overflow canary message ("***ERROR*** A stack overflow in
  // task <name> has been detected.") — esp_panic_handler() sets info->reason to
  // NULL and the real text lives in g_panic_abort_details instead. Arduino's
  // shim copies info->reason verbatim, so relying on it alone would capture an
  // EMPTY string on exactly the crashes that are easiest to act on. Read the
  // abort globals directly.
  rtcPanicWasAbort = g_panic_abort ? 1 : 0;
  const char* text = nullptr;
  if (g_panic_abort && g_panic_abort_details) {
    text = g_panic_abort_details;
  } else if (info && info->reason) {
    text = info->reason;
  }
  crashCopyStr(rtcPanicReason, text, CRASH_REASON_MAX);

  // Without the build id the captured PCs are undecodable after any rebuild —
  // and this repo rebuilds constantly. Documented as safe in panic context.
  crashCopyStr(rtcPanicElfSha, esp_app_get_elf_sha256_str(), CRASH_ELFSHA_MAX);

  // Barrier, then magic LAST.
  __asm__ __volatile__("" ::: "memory");
  rtcPanicMagic = CRASH_PANIC_MAGIC;
  __asm__ __volatile__("" ::: "memory");
}

void crashRecordInstallPanicHook() {
  set_arduino_panic_handler(crashPanicHook, nullptr);
}

// =============================================================================
// Boot-time consume
// =============================================================================
void crashRecordSetPhase(uint8_t phase) {
  rtcBootPhase = phase;
}

// Mirrors HardwareOne.cpp's fault set. ESP_RST_PWR_GLITCH and ESP_RST_CPU_LOCKUP
// are deliberately absent: on ESP32-S3 the ROM->esp_reset_reason mapping cannot
// produce them (a real power glitch arrives as ESP_RST_UNKNOWN), so testing for
// them would be dead weight here.
static bool isFaultReset(uint32_t r) {
  switch ((esp_reset_reason_t)r) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
      return true;
    default:
      return false;
  }
}

void crashRecordBootConsume(uint32_t resetReason) {
  sResetReason = resetReason;
  const char* nowSha = esp_app_get_elf_sha256_str();

  // --- state record ---------------------------------------------------------
  if (rtcStateMagic != CRASH_STATE_MAGIC) {
    // Cold power-on, RTC-domain reset, or a layout change. Start clean.
    rtcBootPhase     = CRASH_PHASE_UNKNOWN;
    rtcConsecutive   = 0;
    rtcLastSignature = 0;
    rtcRepeatCount   = 0;
    rtcStateElfSha[0] = '\0';
    rtcStateMagic    = CRASH_STATE_MAGIC;
  }

  // Latch the previous boot's phase BEFORE this boot advances it. The consume
  // point runs at roughly phase 3, so reading the RTC byte later in setup()
  // would return this boot's own progress, not the dead boot's.
  sPrevPhase = rtcBootPhase;
  sPreviousBuildMatches = nowSha && rtcStateElfSha[0] &&
                          strcmp(nowSha, rtcStateElfSha) == 0;

  const bool fault = isFaultReset(resetReason);
  if (fault) {
    rtcConsecutive++;
  } else if ((esp_reset_reason_t)resetReason == ESP_RST_POWERON) {
    rtcConsecutive = 0;
  }
  sConsecutive = rtcConsecutive;

  // --- panic record ---------------------------------------------------------
  // Gate on the reset reason too. RTC_NOINIT also survives commanded reboots and
  // deep-sleep wakes, so a stale-but-valid record would otherwise be replayed as
  // a fresh crash on every ordinary restart.
  sHavePrev = (rtcPanicMagic == CRASH_PANIC_MAGIC) && fault;
  if (sHavePrev) {
    crashCopyStr(sReason, rtcPanicReason, sizeof(sReason));
    crashCopyStr(sElfSha, rtcPanicElfSha, sizeof(sElfSha));
    sAddr      = rtcPanicAddr;
    sCore      = rtcPanicCore;
    sWasAbort  = (rtcPanicWasAbort != 0);
    sBtCorrupt = (rtcPanicBtCorrupt != 0);
    sBtLen     = (rtcPanicBtLen > CRASH_BT_MAX) ? CRASH_BT_MAX : rtcPanicBtLen;
    for (uint8_t i = 0; i < sBtLen; i++) sBt[i] = rtcPanicBt[i];

    sElfShaMatches = (nowSha && sElfSha[0] && strcmp(nowSha, sElfSha) == 0);
  } else {
    sReason[0] = '\0';
    sElfSha[0] = '\0';
    sAddr = 0; sCore = 0; sWasAbort = false;
    sBtLen = 0; sBtCorrupt = false;
    sElfShaMatches = false;
  }

  // --- signature + repeat ---------------------------------------------------
  // Folds the build id in, so the same fault before and after a rebuild does NOT
  // collide — without it the counter would read as "same crash" across a fix.
  if (sHavePrev) {
    uint32_t h = 2166136261u;
    h = fnv1a(sReason, strlen(sReason), h);
    h = fnv1a(&sAddr, sizeof(sAddr), h);
    h = fnv1a(&sWasAbort, sizeof(sWasAbort), h);
    h = fnv1a(sElfSha, strlen(sElfSha), h);
    sSignature = h ? h : 1u;
  } else if (fault) {
    // No panic record (brownout, or a stage-1 watchdog reset that never reached
    // the panic handler). Reset reason + phase is all the entropy there is, so
    // this signature is LOW CONFIDENCE — on the brownout path the phase is
    // nearly always identical and repeats will over-count.
    uint32_t h = 2166136261u;
    h = fnv1a(&resetReason, sizeof(resetReason), h);
    h = fnv1a(&sPrevPhase, sizeof(sPrevPhase), h);
    sSignature = h ? h : 1u;
  } else {
    sSignature = 0;
  }

  if (sSignature) {
    if (sSignature == rtcLastSignature) {
      rtcRepeatCount++;
    } else {
      rtcLastSignature = sSignature;
      rtcRepeatCount   = 1;
    }
    sRepeat = rtcRepeatCount;
  } else {
    sRepeat = 0;
  }

  // Invalidate the panic record now that it is decoded into RAM, so it can never
  // be replayed on a later boot. Same invalidate-before-use discipline as
  // rtcRebootReasonMagic and ramFlushConsumeOverlay().
  rtcPanicMagic = 0;

  // This boot starts here.
  // Record the build independently of the optional panic record. Stage-1 WDT
  // and brownout resets may never reach the panic hook, but crash-loop recovery
  // still needs to distinguish the same broken build from a newly flashed fix.
  crashCopyStr(rtcStateElfSha, nowSha, sizeof(rtcStateElfSha));
  rtcBootPhase = CRASH_PHASE_PRE_SERIAL;
}

void crashRecordEmitEarly() {
  if (!sHavePrev && sConsecutive == 0) return;

  esp_rom_printf("\r\n=== PREVIOUS CRASH ===\r\n");
  if (sHavePrev) {
    esp_rom_printf("  %s: %s\r\n",
                   sPrevPhase == CRASH_PHASE_RUNNING ? "runtime" : "during boot",
                   sReason[0] ? sReason : "(no text)");
    esp_rom_printf("  core=%u %s pc=0x%08x phase=%s\r\n",
                   (unsigned)sCore, sWasAbort ? "abort" : "fault",
                   (unsigned)sAddr, crashPhaseName(sPrevPhase));
    esp_rom_printf("  build=%s%s\r\n", sElfSha[0] ? sElfSha : "?",
                   sElfShaMatches ? "" : " (DIFFERENT BUILD - pc not decodable here)");
    if (sBtLen) {
      esp_rom_printf("  backtrace:");
      for (uint8_t i = 0; i < sBtLen; i++) esp_rom_printf(" 0x%08x", (unsigned)sBt[i]);
      esp_rom_printf("%s\r\n", sBtCorrupt ? " (CORRUPT)" : "");
    }
  } else {
    esp_rom_printf("  no panic record (brownout or watchdog reset before the panic handler)\r\n");
    esp_rom_printf("  phase=%s\r\n", crashPhaseName(sPrevPhase));
  }
  esp_rom_printf("  consecutive=%u sig=0x%08x x%u\r\n",
                 (unsigned)sConsecutive, (unsigned)sSignature, (unsigned)sRepeat);
  esp_rom_printf("======================\r\n\r\n");
}

bool        crashRecordHavePrevious() { return sHavePrev; }
bool        crashRecordPreviousBuildMatches() { return sPreviousBuildMatches; }
uint32_t    crashRecordConsecutive()  { return sConsecutive; }
uint32_t    crashRecordSignature()    { return sSignature; }
uint32_t    crashRecordRepeatCount()  { return sRepeat; }
uint8_t     crashRecordPrevPhase()    { return sPrevPhase; }
const char* crashRecordReasonText()   { return sReason; }

size_t crashRecordSummary(char* buf, size_t n) {
  if (!buf || n == 0) return 0;
  if (!sHavePrev) {
    if (sConsecutive == 0) { buf[0] = '\0'; return 0; }
    return (size_t)snprintf(buf, n, "phase=%s consec=%lu sig=0x%08lx x%lu (no panic record)",
                            crashPhaseName(sPrevPhase), (unsigned long)sConsecutive,
                            (unsigned long)sSignature, (unsigned long)sRepeat);
  }
  // Kept short on purpose — the caller logs this BEFORE initDebugSystem(), where
  // the pre-init path truncates at ~165 chars with no marker.
  return (size_t)snprintf(buf, n, "%.70s | core%u %s pc=0x%08lx | phase=%s consec=%lu x%lu",
                          sReason[0] ? sReason : "(no text)",
                          (unsigned)sCore, sWasAbort ? "abort" : "fault",
                          (unsigned long)sAddr, crashPhaseName(sPrevPhase),
                          (unsigned long)sConsecutive, (unsigned long)sRepeat);
}

size_t crashRecordDetail(char* buf, size_t n) {
  if (!buf || n == 0) return 0;
  if (!sHavePrev && sConsecutive == 0) {
    return (size_t)snprintf(buf, n, "No crash recorded since the last power-on.");
  }
  size_t w = 0;
  // Reset reason is deliberately NOT rendered here — the caller owns the label
  // table (resetReasonLabel in System_Utils.cpp) and prints it, so there is one
  // reset-reason vocabulary rather than two that can drift apart. That drift is
  // exactly what left kResetReasonLabels short of the enum.
  w += (size_t)snprintf(buf + w, (w < n) ? n - w : 0,
                        "  boot phase   : %s\n"
                        "  consecutive  : %lu  (fault resets in a row; clears after a healthy run)\n"
                        "  signature    : 0x%08lx  seen x%lu in a row\n",
                        crashPhaseName(sPrevPhase),
                        (unsigned long)sConsecutive,
                        (unsigned long)sSignature, (unsigned long)sRepeat);
  if (sHavePrev) {
    w += (size_t)snprintf(buf + w, (w < n) ? n - w : 0,
                          "  detail       : %s\n"
                          "  class        : %s\n"
                          "  core / pc    : %u / 0x%08lx\n"
                          "  build        : %s%s\n",
                          sReason[0] ? sReason : "(no text)",
                          sWasAbort ? "abort (assert / ESP_ERROR_CHECK / stack overflow / heap corruption)"
                                    : "fault (load/store prohibited, illegal instruction, ...)",
                          (unsigned)sCore, (unsigned long)sAddr,
                          sElfSha[0] ? sElfSha : "?",
                          sElfShaMatches ? " (this build)" : " (DIFFERENT build - pc not decodable here)");
    if (sBtLen && w < n) {
      w += (size_t)snprintf(buf + w, n - w, "  backtrace    :");
      for (uint8_t i = 0; i < sBtLen && w < n; i++) {
        w += (size_t)snprintf(buf + w, n - w, " 0x%08lx", (unsigned long)sBt[i]);
      }
      if (w < n) w += (size_t)snprintf(buf + w, n - w, "%s\n", sBtCorrupt ? "  (CORRUPT)" : "");
    }
  } else {
    w += (size_t)snprintf(buf + w, (w < n) ? n - w : 0,
                          "  detail       : none - the reset happened without reaching the panic\n"
                          "                 handler (brownout, or a stage-1 watchdog reset).\n"
                          "                 Signature is low-confidence on this path.\n");
  }
  return w;
}

size_t crashRecordPersistToFile(const char* resetReasonLabel,
                                uint32_t resetReasonCode,
                                unsigned long bootNumber) {
  if (!sHavePrev && sConsecutive == 0) return 0;

  // Transient, not static: one write per crashed boot doesn't earn a permanent
  // buffer. Sized for header + crashRecordDetail's worst case (~620 B with a
  // full 12-frame backtrace — same budget as crashlog's 640 B render buffer).
  const size_t kCap = 1024;
  char* buf = (char*)malloc(kCap);
  if (!buf) return 0;

  size_t w = (size_t)snprintf(buf, kCap,
                              "=== boot #%lu died: %s (%lu) | logged at boot #%lu +%lums",
                              bootNumber > 0 ? bootNumber - 1 : 0,
                              resetReasonLabel ? resetReasonLabel : "?",
                              (unsigned long)resetReasonCode,
                              bootNumber, (unsigned long)millis());
  // Wall clock is usually not synced yet at this point in boot; stamp it only
  // when a warm RTC carried real time across the reset.
  time_t now = time(nullptr);
  if (Clock::isValidEpoch(now) && w < kCap) {
    struct tm lt;
    localtime_r(&now, &lt);
    w += strftime(buf + w, kCap - w, " %Y-%m-%d %H:%M:%S", &lt);
  }
  if (w < kCap) w += (size_t)snprintf(buf + w, kCap - w, " ===\n");
  if (w < kCap) w += crashRecordDetail(buf + w, kCap - w);

  const bool ok = appendLineWithCap("/system/sys_logs/crash-history.log",
                                    String(buf), 64 * 1024);
  free(buf);
  return ok ? w : 0;
}

void crashRecordMarkBootHealthy() {
  if (sHealthyMarked) return;
  sHealthyMarked = true;
  rtcConsecutive = 0;
  // sConsecutive is intentionally NOT cleared: it describes the boot we already
  // reported, and clearing it would make `crashlog` contradict the boot banner.
}

uint8_t         crashRecordBacktraceLen() { return sBtLen; }
const uint32_t* crashRecordBacktrace()    { return sBt; }
