#pragma once
// =============================================================================
// System_CrashRecord — post-mortem capture for crashes that reboot the device
// =============================================================================
//
// WHY THIS EXISTS
// A panic prints a Guru Meditation banner (or "***ERROR*** A stack overflow in
// task <name> has been detected.") to UART and then reboots in 0 s
// (CONFIG_ESP_SYSTEM_PANIC_REBOOT_DELAY_SECONDS=0). In the field nobody is
// attached to serial, so the single most diagnostic sentence the firmware ever
// produces is destroyed milliseconds after it is written. This module catches it
// on the way past and parks it in RTC memory, where it survives the reset.
//
// WHAT IT IS NOT
// This is INSTRUMENTATION, not automation. Nothing here changes the boot path or
// disables anything. Reading these records must never gate a subsystem — see
// docs/PRE_1_0_HARDENING_AUDIT.md §5.2 for why an auto-safe-mode boot-loop
// breaker was explicitly rejected (it would misfire on brownouts, on
// attacker-induced panics, and on a counter that isn't consecutive).
//
// WHERE THE DATA LIVES
// RTC_NOINIT_ATTR lands in .rtc_noinit -> rtc_data_location -> rtc_slow_seg
// (0x50000000, 8192 B) because CONFIG_ESP32S3_RTCDATA_IN_FAST_MEM is NOT set.
// Only ~84 B of that was in use before this file. NOTE: the "7 KiB RTCRAM" line
// in the boot log is RTC *FAST* heap — a DIFFERENT pool that RTC_NOINIT never
// touches. Do not size against it.
//
// WHAT SURVIVES WHAT
//   panic / abort / WDT reset / brownout ......... yes (software reset)
//   deep sleep ................................... yes (RTC slow can't power down on S3)
//   true power-off ............................... NO
//   RTC-domain reset ............................. NO — esp_restart_noos() arms the
//     RTC WDT with STAGE1 = RESET_RTC, so a STALLED restart wipes this *including*
//     rtcMagic. That means "RTC came up cold" is NOT proof of a power cycle; cross
//     check against the NVS boot counter before claiming one.
//
#include <stdint.h>
#include <stddef.h>

// Boot checkpoints. Stored as a byte in RTC and advanced as setup() progresses,
// so a crash during boot says WHERE. Latched to plain RAM at the top of the
// pre-Serial block before this boot overwrites it — otherwise the consume point
// (which runs at ~phase 3) reads its own value, not the previous boot's.
enum CrashBootPhase : uint8_t {
  CRASH_PHASE_UNKNOWN      = 0,
  CRASH_PHASE_PRE_SERIAL   = 1,   // 1. RTC crash tracking, no Serial yet
  CRASH_PHASE_FS_SETTINGS  = 2,   // 2. Serial + filesystem + settings load
  CRASH_PHASE_DEBUG_BUF    = 3,   // 3. Mutexes + debug system + buffers
  CRASH_PHASE_HARDWARE     = 4,   // 4. Battery, LED, I2C buses
  CRASH_PHASE_BANNER_OLED  = 5,   // 5. Build banner + OLED early init
  CRASH_PHASE_FIRST_TIME   = 6,   // 6. First-time setup + credentials
  CRASH_PHASE_NETWORK      = 7,   // 7. WiFi + NTP
  CRASH_PHASE_AUTOSTART    = 8,   // 8. Device discovery + service auto-start
  CRASH_PHASE_BOOT_DIAG    = 9,   // 9. Boot-complete diagnostics
  CRASH_PHASE_RUNNING      = 10,  // setup() returned; loop() is ticking
  CRASH_PHASE__COUNT
};

const char* crashPhaseName(uint8_t phase);

// Register the panic-time capture hook. Call ONCE, early in setup().
//
// This rides Arduino's set_arduino_panic_handler() rather than defining
// __wrap_esp_panic_handler ourselves: the bundled Arduino core already defines
// that symbol (esp32-hal-misc.c), so a second definition is a duplicate-symbol
// link error. Arduino's wrapper is dead code in an ESP-IDF build until something
// supplies -Wl,--wrap=esp_panic_handler (only its Arduino-IDE platform.txt does),
// which our CMakeLists now adds — so the flag activates their wrapper and this
// hook is what it calls. Bonus: their wrapper already walks a full backtrace with
// esp_backtrace_get_next_frame(), which we get for free.
void crashRecordInstallPanicHook();

// Advance the boot-phase marker. Cheap (one RTC byte store); safe anywhere.
void crashRecordSetPhase(uint8_t phase);

// Called ONCE from the pre-Serial block, before anything else touches RTC.
// Latches the previous boot's phase, validates/decodes any panic record left by
// __wrap_esp_panic_handler, maintains the CONSECUTIVE crash counter (distinct
// from rtcCrashCount, which is cumulative-since-poweron), and computes the crash
// signature. Does no I/O.
void crashRecordBootConsume(uint32_t resetReason);

// Emit the decoded previous-crash record over esp_rom_printf. Safe to call in
// the pre-Serial phase: no filesystem, no Serial, no debug task, no heap. This
// is the ONLY render path that survives a setup()-phase boot loop, where the
// device never reaches loop() and therefore never reaches any normal log sink.
void crashRecordEmitEarly();

bool        crashRecordHavePrevious();     // was a decodable panic record left behind?
bool        crashRecordPreviousBuildMatches(); // did the immediately previous boot run this exact build?
uint32_t    crashRecordConsecutive();      // consecutive fault resets (0 after a healthy run)
uint32_t    crashRecordSignature();        // FNV-1a over reason+addr+exception+elfsha
uint32_t    crashRecordRepeatCount();      // how many boots in a row produced this signature
uint8_t     crashRecordPrevPhase();        // CrashBootPhase the PREVIOUS boot died in
const char* crashRecordReasonText();       // abort/assert text, or the exception string ("" if none)

// Captured backtrace (PC values, innermost first). Decodable with addr2line only
// when the build id matches — crashRecordDetail() says so explicitly.
uint8_t         crashRecordBacktraceLen();
const uint32_t* crashRecordBacktrace();

// One-line summary for the BOOT event. Keep the caller's buffer <=160 chars: the
// consume point runs BEFORE initDebugSystem(), so logSystemEvent() takes its
// pre-init branch (a bare snprintf into gPreInitEvents[12][192]) which truncates
// with NO [CUT] marker. Anything longer loses its tail silently.
size_t crashRecordSummary(char* buf, size_t n);

// Full multi-line detail for the `crashlog` command (normal task context).
size_t crashRecordDetail(char* buf, size_t n);

// Append the decoded record to /system/sys_logs/crash-history.log (64 KB cap,
// oldest-trimmed). Call at BOOT once the filesystem is up — never from panic
// context, where flash writes are forbidden (see the capture notes in the .cpp).
// The RTC copy is invalidated at consume, so without this the record survives
// only in the current boot's RAM. Caller passes the reset-reason label so there
// is one label vocabulary (see crashRecordDetail) and the current boot number
// for the header line. Returns bytes rendered, 0 if nothing to record / failed.
size_t crashRecordPersistToFile(const char* resetReasonLabel,
                                uint32_t resetReasonCode,
                                unsigned long bootNumber);

// Mark this boot healthy so the CONSECUTIVE counter resets. Called from loop()
// once uptime passes a threshold. Without this the counter would only ever clear
// on a power cycle and would therefore measure "crashes since poweron", the exact
// conflation this counter exists to avoid. Idempotent.
void crashRecordMarkBootHealthy();
