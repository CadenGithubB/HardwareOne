// Host harness for the boot-time crash counters and the OTA crash-loop escape
// decision. The runner inserts the shipping RTC layout, crashRecordBootConsume,
// the counter getters, crashRecordMarkBootHealthy, crashRecordConsumeCrashLoopEscape
// and crashRecordDetail from System_CrashRecord.cpp, and crashLoopEscapeWanted
// plus restartIntoCrashLoopRecovery from System_OTA.cpp, unchanged. RTC_NOINIT
// storage is plain process memory here, which models what RTC slow memory does
// across a reset: it keeps its bytes. A "reset" is another call to
// crashRecordBootConsume with the reason the ROM would report; esp_restart is a
// recorded boundary that unwinds instead of rebooting.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

// --- ESP-IDF surface the production code touches ----------------------------
typedef enum {
  ESP_RST_UNKNOWN,
  ESP_RST_POWERON,
  ESP_RST_EXT,
  ESP_RST_SW,
  ESP_RST_PANIC,
  ESP_RST_INT_WDT,
  ESP_RST_TASK_WDT,
  ESP_RST_WDT,
  ESP_RST_DEEPSLEEP,
  ESP_RST_BROWNOUT,
  ESP_RST_SDIO,
  ESP_RST_USB,
  ESP_RST_JTAG,
  ESP_RST_EFUSE,
  ESP_RST_PWR_GLITCH,
  ESP_RST_CPU_LOCKUP,
} esp_reset_reason_t;

#define RTC_NOINIT_ATTR
#define IRAM_ATTR

static char gElfSha[16] = "aaaaaaaaa";
static const char* esp_app_get_elf_sha256_str() { return gElfSha; }

struct RestartSignal {
  std::string what;
};
static std::string gLastEarlyLog;
[[noreturn]] static void esp_restart() { throw RestartSignal{gLastEarlyLog}; }
static constexpr char kTag[] = "ota";
#define ESP_EARLY_LOGE(tag, fmt, ...)                                      \
  do {                                                                     \
    char line_[160];                                                       \
    snprintf(line_, sizeof(line_), fmt, __VA_ARGS__);                      \
    gLastEarlyLog = std::string(tag) + ": " + line_;                       \
  } while (0)

// INSERT_PHASE_ENUM

// INSERT_CRASH_RECORD_STATE

// INSERT_CRASH_RECORD_FUNCTIONS

// INSERT_OTA_ESCAPE

// --- checks ------------------------------------------------------------------
static int gChecks = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #cond);   \
      exit(1);                                                             \
    }                                                                      \
    gChecks++;                                                             \
  } while (0)

// A fresh process image: the RTC block has never been written.
static void coldRtc() {
  rtcStateMagic = 0;
  rtcPanicMagic = 0;
  sHealthyMarked = false;
}

// Every boot is a new process on the device, so the one-shot healthy latch
// starts clear; the RTC statics keep whatever the previous "boot" left.
static void boot(esp_reset_reason_t reason) {
  sHealthyMarked = false;
  crashRecordBootConsume((uint32_t)reason);
}

// Runs the escape decision the way otaSystemCrashLoopEscapeEarly() does, with
// the partition and journal work replaced by the commit boundary. Returns true
// when the escape committed (the production path would now be in the updater).
static bool escapeIfWanted() {
  if (!crashLoopEscapeWanted()) return false;
  try {
    restartIntoCrashLoopRecovery("Main-image crash loop detected; entering factory recovery");
  } catch (const RestartSignal& restart) {
    CHECK(restart.what == "ota: Main-image crash loop detected; entering factory recovery");
    return true;
  }
  return false;
}

static void brownoutsNeverEscape() {
  coldRtc();
  boot(ESP_RST_POWERON);
  CHECK(crashRecordConsecutive() == 0);
  CHECK(crashRecordConsecutiveSoftwareFaults() == 0);
  CHECK(!escapeIfWanted());
  for (int i = 1; i <= 3; i++) {
    boot(ESP_RST_BROWNOUT);
    CHECK(crashRecordConsecutive() == (uint32_t)i);
    CHECK(crashRecordConsecutiveSoftwareFaults() == 0);
    CHECK(crashRecordPrevPhase() == CRASH_PHASE_PRE_SERIAL);
    CHECK(crashRecordPreviousBuildMatches());
    CHECK(!escapeIfWanted());
  }
  // The diagnostic text still reports every reset, with the software share.
  char detail[512];
  CHECK(crashRecordDetail(detail, sizeof(detail)) > 0);
  CHECK(strstr(detail, "consecutive  : 3  (fault resets in a row, 0 software;") != nullptr);
  // Two more and a sagging battery still never reaches the updater.
  boot(ESP_RST_BROWNOUT);
  boot(ESP_RST_BROWNOUT);
  CHECK(crashRecordConsecutive() == 5);
  CHECK(crashRecordConsecutiveSoftwareFaults() == 0);
  CHECK(!escapeIfWanted());
}

static void panicsEscapeAtThree() {
  coldRtc();
  boot(ESP_RST_POWERON);
  boot(ESP_RST_PANIC);
  CHECK(crashRecordConsecutiveSoftwareFaults() == 1);
  CHECK(!escapeIfWanted());
  boot(ESP_RST_TASK_WDT);
  CHECK(crashRecordConsecutiveSoftwareFaults() == 2);
  CHECK(!escapeIfWanted());
  boot(ESP_RST_PANIC);
  CHECK(crashRecordConsecutive() == 3);
  CHECK(crashRecordConsecutiveSoftwareFaults() == 3);
  CHECK(escapeIfWanted());
  // The RAM view of the boot that escaped is kept for the banner and crashlog.
  CHECK(crashRecordConsecutive() == 3);
  CHECK(crashRecordConsecutiveSoftwareFaults() == 3);
}

static void mixedResetsCountOnlySoftware() {
  coldRtc();
  boot(ESP_RST_POWERON);
  boot(ESP_RST_PANIC);
  boot(ESP_RST_BROWNOUT);
  boot(ESP_RST_INT_WDT);
  boot(ESP_RST_BROWNOUT);
  CHECK(crashRecordConsecutive() == 4);
  CHECK(crashRecordConsecutiveSoftwareFaults() == 2);
  CHECK(!escapeIfWanted());
  boot(ESP_RST_WDT);
  CHECK(crashRecordConsecutive() == 5);
  CHECK(crashRecordConsecutiveSoftwareFaults() == 3);
  CHECK(escapeIfWanted());
}

static void escapeThenSoftwareResetDoesNotBounce() {
  coldRtc();
  boot(ESP_RST_POWERON);
  boot(ESP_RST_PANIC);
  boot(ESP_RST_PANIC);
  boot(ESP_RST_PANIC);
  CHECK(escapeIfWanted());
  // The updater hands control back with a software reset, same build, RTC
  // block intact (phase PRE_SERIAL from the escape boot).
  boot(ESP_RST_SW);
  CHECK(crashRecordPrevPhase() == CRASH_PHASE_PRE_SERIAL);
  CHECK(crashRecordPreviousBuildMatches());
  CHECK(crashRecordConsecutive() == 0);
  CHECK(crashRecordConsecutiveSoftwareFaults() == 0);
  CHECK(!escapeIfWanted());
  // A genuinely broken image still gets caught again after a fresh three.
  boot(ESP_RST_PANIC);
  boot(ESP_RST_PANIC);
  CHECK(!escapeIfWanted());
  boot(ESP_RST_PANIC);
  CHECK(crashRecordConsecutiveSoftwareFaults() == 3);
  CHECK(escapeIfWanted());
}

static void escapeNeedsKnownPhaseAndSameBuild() {
  // Cold RTC: the first consume sees an unknown previous phase.
  coldRtc();
  boot(ESP_RST_PANIC);
  boot(ESP_RST_PANIC);
  boot(ESP_RST_PANIC);
  CHECK(crashRecordConsecutiveSoftwareFaults() == 3);
  CHECK(crashRecordPrevPhase() == CRASH_PHASE_PRE_SERIAL);
  CHECK(escapeIfWanted());

  coldRtc();
  boot(ESP_RST_PANIC);
  CHECK(crashRecordPrevPhase() == CRASH_PHASE_UNKNOWN);
  CHECK(!crashRecordPreviousBuildMatches());

  // The decision needs a known previous phase even at the threshold.
  coldRtc();
  boot(ESP_RST_POWERON);
  boot(ESP_RST_PANIC);
  boot(ESP_RST_PANIC);
  rtcBootPhase = CRASH_PHASE_UNKNOWN;  // the previous boot never advanced its marker
  boot(ESP_RST_PANIC);
  CHECK(crashRecordConsecutiveSoftwareFaults() == 3);
  CHECK(crashRecordPrevPhase() == CRASH_PHASE_UNKNOWN);
  CHECK(crashRecordPreviousBuildMatches());
  CHECK(!escapeIfWanted());
  boot(ESP_RST_PANIC);
  CHECK(crashRecordConsecutiveSoftwareFaults() == 4);
  CHECK(crashRecordPrevPhase() == CRASH_PHASE_PRE_SERIAL);
  CHECK(escapeIfWanted());

  // A different build was flashed between the crashes: no escape.
  coldRtc();
  boot(ESP_RST_POWERON);
  boot(ESP_RST_PANIC);
  boot(ESP_RST_PANIC);
  strcpy(gElfSha, "bbbbbbbbb");
  boot(ESP_RST_PANIC);
  CHECK(crashRecordConsecutiveSoftwareFaults() == 3);
  CHECK(!crashRecordPreviousBuildMatches());
  CHECK(!escapeIfWanted());
  boot(ESP_RST_PANIC);
  CHECK(crashRecordPreviousBuildMatches());
  CHECK(escapeIfWanted());
  strcpy(gElfSha, "aaaaaaaaa");
}

static void healthyRunAndPowerOnClearBothCounters() {
  coldRtc();
  boot(ESP_RST_POWERON);
  boot(ESP_RST_PANIC);
  boot(ESP_RST_BROWNOUT);
  CHECK(crashRecordConsecutive() == 2);
  CHECK(crashRecordConsecutiveSoftwareFaults() == 1);
  crashRecordMarkBootHealthy();
  crashRecordMarkBootHealthy();
  // The reported view of this boot does not change under the running image.
  CHECK(crashRecordConsecutive() == 2);
  CHECK(crashRecordConsecutiveSoftwareFaults() == 1);
  boot(ESP_RST_PANIC);
  CHECK(crashRecordConsecutive() == 1);
  CHECK(crashRecordConsecutiveSoftwareFaults() == 1);

  boot(ESP_RST_PANIC);
  boot(ESP_RST_BROWNOUT);
  CHECK(crashRecordConsecutive() == 3);
  CHECK(crashRecordConsecutiveSoftwareFaults() == 2);
  boot(ESP_RST_POWERON);
  CHECK(crashRecordConsecutive() == 0);
  CHECK(crashRecordConsecutiveSoftwareFaults() == 0);

  // A layout change or RTC-domain reset starts both counters clean.
  boot(ESP_RST_PANIC);
  boot(ESP_RST_PANIC);
  rtcStateMagic = 0;
  boot(ESP_RST_PANIC);
  CHECK(crashRecordConsecutive() == 1);
  CHECK(crashRecordConsecutiveSoftwareFaults() == 1);
}

int main() {
  brownoutsNeverEscape();
  panicsEscapeAtThree();
  mixedResetsCountOnlySoftware();
  escapeThenSoftwareResetDoesNotBounce();
  escapeNeedsKnownPhaseAndSameBuild();
  healthyRunAndPowerOnClearBothCounters();
  printf("crash record boot: ok\n");
  fprintf(stderr, "checks=%d\n", gChecks);
  return 0;
}
