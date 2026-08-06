#include "System_OTASafety.h"
#include "System_OTA.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

/* Probation-abort breadcrumb.
 *
 * All five abort causes used to reach the recovery updater as one generic
 * `rollback_detected`, so an operator could see THAT a trial image was rejected
 * but never WHICH check rejected it - and the reason existed only as a string
 * literal passed to a log line on a UART nobody was attached to.
 *
 * RTC_NOINIT survives esp_restart and the bootloader, and the recovery updater
 * declares no RTC variables of its own, so this survives the whole
 * main -> factory -> main round trip. It is deliberately not NVS: one caller
 * (the supervisor-allocation failure) runs before storage is up. */
RTC_NOINIT_ATTR static uint32_t rtcProbationMagic;
RTC_NOINIT_ATTR static uint8_t rtcProbationCause;
RTC_NOINIT_ATTR static uint32_t rtcProbationUptimeMs;
RTC_NOINIT_ATTR static char rtcProbationDetail[64];

namespace {

constexpr char kTag[] = "OTA-SAFETY";
constexpr uint32_t kProbationMagic = 0x50524231u; /* "PRB1" */

// A pending image gets five minutes to finish setup, then five minutes after
// RUNNING to accumulate one uninterrupted healthy minute. A completed-loop gap
// above five seconds restarts that healthy minute; no completed loop for thirty
// seconds is a hang and reboots immediately. All limits apply only to an
// unverified OTA image; normal and factory boots create no supervisor task.
constexpr TickType_t kSetupTimeoutTicks = pdMS_TO_TICKS(5 * 60 * 1000);
constexpr TickType_t kHealthyIntervalTicks = pdMS_TO_TICKS(60 * 1000);
constexpr TickType_t kHealthyHeartbeatGapTicks = pdMS_TO_TICKS(5 * 1000);
constexpr TickType_t kLoopHangTimeoutTicks = pdMS_TO_TICKS(30 * 1000);
constexpr TickType_t kProbationHardLimitTicks = pdMS_TO_TICKS(5 * 60 * 1000);
constexpr TickType_t kMarkRetryTicks = pdMS_TO_TICKS(1000);
constexpr uint32_t kMinimumHealthyLoops = 10;

enum class ProbationPhase : uint32_t {
  Inactive = 0,
  Setup,
  Running,
  MarkingValid,
  Validated,
};

std::atomic<ProbationPhase> sPhase{ProbationPhase::Inactive};
std::atomic<TickType_t> sBootStartedTick{0};
std::atomic<TickType_t> sRunningStartedTick{0};
std::atomic<TickType_t> sHealthyStartedTick{0};
std::atomic<TickType_t> sLastLoopHeartbeatTick{0};
std::atomic<TickType_t> sLastMarkAttemptTick{0};
std::atomic<uint32_t> sHealthyLoopCount{0};

inline TickType_t elapsedTicks(TickType_t now, TickType_t then) {
  // Unsigned subtraction intentionally remains correct across the FreeRTOS tick
  // counter wrap for every interval used here (all are far below half-range).
  return static_cast<TickType_t>(now - then);
}

bool isUnverifiedState(esp_ota_img_states_t state) {
  return state == ESP_OTA_IMG_NEW || state == ESP_OTA_IMG_PENDING_VERIFY;
}

bool runningImageIsUnverified() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running) return false;

  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  return esp_ota_get_state_partition(running, &state) == ESP_OK &&
         isUnverifiedState(state);
}

[[noreturn]] void rebootPendingImage(OtaProbationCause cause, const char* reason) {
  // Record WHICH check rejected the image before the reboot destroys the
  // evidence. The image itself is about to be rolled back and cannot be
  // inspected afterwards, so this is the only surviving account.
  rtcProbationCause = static_cast<uint8_t>(cause);
  rtcProbationUptimeMs =
      static_cast<uint32_t>(esp_timer_get_time() / 1000);
  snprintf(rtcProbationDetail, sizeof(rtcProbationDetail), "%s",
           reason ? reason : "unknown");
  rtcProbationMagic = kProbationMagic;
  ESP_EARLY_LOGE(kTag, "Pending image probation failed [%s]: %s; rebooting for rollback",
                 otaSafetyProbationCauseName(cause), reason ? reason : "unknown");
  // Give UART/logging a scheduling opportunity. The bootloader, not this task,
  // changes the still-PENDING image to ABORTED and selects the fallback.
  vTaskDelay(pdMS_TO_TICKS(100));
  esp_restart();
  for (;;) vTaskDelay(portMAX_DELAY);
}

void probationSupervisor(void*) {
  for (;;) {
    const ProbationPhase phase = sPhase.load(std::memory_order_acquire);
    if (phase == ProbationPhase::Inactive || phase == ProbationPhase::Validated) {
      vTaskDelete(nullptr);
    }

    const TickType_t now = xTaskGetTickCount();
    if (phase == ProbationPhase::Setup) {
      const TickType_t bootStarted = sBootStartedTick.load(std::memory_order_relaxed);
      if (elapsedTicks(now, bootStarted) >= kSetupTimeoutTicks) {
        rebootPendingImage(OTA_PROBATION_SETUP_TIMEOUT,
                       "setup did not reach RUNNING within 5 minutes");
      }
    } else if (phase == ProbationPhase::Running ||
               phase == ProbationPhase::MarkingValid) {
      const TickType_t lastHeartbeat =
          sLastLoopHeartbeatTick.load(std::memory_order_relaxed);
      if (elapsedTicks(now, lastHeartbeat) >= kLoopHangTimeoutTicks) {
        rebootPendingImage(OTA_PROBATION_LOOP_STALL,
                       "no completed main-loop heartbeat for 30 seconds");
      }

      const TickType_t runningStarted =
          sRunningStartedTick.load(std::memory_order_relaxed);
      if (elapsedTicks(now, runningStarted) >= kProbationHardLimitTicks) {
        rebootPendingImage(OTA_PROBATION_HEALTH_TIMEOUT,
                       "healthy probation did not complete within 5 minutes");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

bool isNvsPartition(const esp_partition_t* partition) {
  if (!partition || partition->type != ESP_PARTITION_TYPE_DATA) return false;
  return partition->subtype == ESP_PARTITION_SUBTYPE_DATA_NVS ||
         partition->subtype == ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS;
}

}  // namespace

const char* otaSafetyProbationCauseName(OtaProbationCause cause) {
  switch (cause) {
    case OTA_PROBATION_SETUP_TIMEOUT:    return "setup_timeout";
    case OTA_PROBATION_LOOP_STALL:       return "loop_stall";
    case OTA_PROBATION_HEALTH_TIMEOUT:   return "health_timeout";
    case OTA_PROBATION_SUPERVISOR_ALLOC: return "supervisor_alloc_failed";
    case OTA_PROBATION_JOURNAL_REFUSED:  return "journal_refused";
    case OTA_PROBATION_NONE:             break;
  }
  return "none";
}

bool otaSafetyTakeProbationAbort(OtaProbationCause* cause, uint32_t* uptimeMs,
                                 char* detail, size_t detailSize, bool consume) {
  if (rtcProbationMagic != kProbationMagic ||
      rtcProbationCause == OTA_PROBATION_NONE) {
    return false;
  }
  if (cause) *cause = static_cast<OtaProbationCause>(rtcProbationCause);
  if (uptimeMs) *uptimeMs = rtcProbationUptimeMs;
  if (detail && detailSize) {
    // RTC contents are not trusted to be terminated after an unclean boot.
    rtcProbationDetail[sizeof(rtcProbationDetail) - 1] = '\0';
    snprintf(detail, detailSize, "%s", rtcProbationDetail);
  }
  if (consume) {
    rtcProbationMagic = 0;
    rtcProbationCause = OTA_PROBATION_NONE;
  }
  return true;
}

bool otaSafetyAcceptProvisioningBoot() {
  // Reaching interactive first-time setup is stronger evidence than the loop
  // probation it replaces: the bootloader verified the image, storage mounted,
  // settings loaded, and the firmware is talking to a human. The loop probation
  // cannot help here anyway - firstTimeSetupIfNeeded() blocks inside
  // hardwareone_setup(), so otaSafetySetupReachedRunning() and the loop
  // heartbeat are both unreachable until a person finishes typing, and the
  // 5-minute setup deadline was firing on operators who simply read slowly.
  //
  // Safe because the two situations are disjoint: an OTA-delivered image lands
  // on a filesystem that still holds users.json, since OTA never touches
  // LittleFS. First-time setup therefore implies cable provisioning with an
  // operator physically present, while rollback protection exists for
  // unattended field updates. The caller must confirm the filesystem actually
  // mounted - a broken image that LOST its storage also reports "setup
  // required", and that case must keep its probation.
  const ProbationPhase phase = sPhase.load(std::memory_order_acquire);
  if (phase == ProbationPhase::Inactive || phase == ProbationPhase::Validated) {
    return true;  // nothing to accept: not an unverified image
  }
  ProbationPhase expected = phase;
  if (!sPhase.compare_exchange_strong(expected, ProbationPhase::MarkingValid,
                                      std::memory_order_acq_rel)) {
    return false;
  }
  if (!otaSystemCanMarkImageValid()) {
    rebootPendingImage(OTA_PROBATION_JOURNAL_REFUSED,
                       "OTA trial journal is missing or inconsistent");
  }
  const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "Could not accept provisioning boot: %s (0x%x)",
             esp_err_to_name(err), err);
    sPhase.store(phase, std::memory_order_release);
    return false;
  }
  sPhase.store(ProbationPhase::Validated, std::memory_order_release);
  otaSystemOnImageMarkedValid();
  ESP_LOGW(kTag,
           "OTA image accepted on reaching first-time setup; probation ended "
           "early because an operator is provisioning over a cable");
  return true;
}

void otaSafetyInitEarly() {
  if (!runningImageIsUnverified()) return;

  const TickType_t now = xTaskGetTickCount();
  sBootStartedTick.store(now, std::memory_order_relaxed);
  sPhase.store(ProbationPhase::Setup, std::memory_order_release);

#if CONFIG_FREERTOS_UNICORE
  constexpr BaseType_t kSupervisorCore = tskNO_AFFINITY;
#else
  // HardwareOne's ESP-IDF main task is pinned to core 0. The independent
  // supervisor lives on core 1 so a tight loop on the main core cannot starve it.
  constexpr BaseType_t kSupervisorCore = 1;
#endif

  TaskHandle_t supervisor = nullptr;
  const BaseType_t created = xTaskCreatePinnedToCore(
      probationSupervisor, "ota_probation", 3072, nullptr, 6, &supervisor,
      kSupervisorCore);
  if (created != pdPASS) {
    rebootPendingImage(OTA_PROBATION_SUPERVISOR_ALLOC,
                     "could not create probation supervisor");
  }

  ESP_EARLY_LOGW(kTag,
                 "Running image is unverified; destructive storage recovery is blocked");
}

bool otaSafetyIsPendingVerification() {
  const ProbationPhase phase = sPhase.load(std::memory_order_acquire);
  return phase == ProbationPhase::Setup || phase == ProbationPhase::Running ||
         phase == ProbationPhase::MarkingValid;
}

bool otaSafetyAllowsDestructiveStorageRecovery() {
  return false;
}

void otaSafetySetupReachedRunning() {
  // Publish the timestamps before the phase transition. The supervisor runs
  // on the other core and treats Running as permission to consume all three;
  // publishing Running first leaves a real window where it observes zero and
  // immediately diagnoses a false 30-second hang on a sufficiently long boot.
  if (sPhase.load(std::memory_order_acquire) != ProbationPhase::Setup) return;

  const TickType_t now = xTaskGetTickCount();
  sRunningStartedTick.store(now, std::memory_order_relaxed);
  sHealthyStartedTick.store(now, std::memory_order_relaxed);
  sLastLoopHeartbeatTick.store(now, std::memory_order_relaxed);
  sLastMarkAttemptTick.store(0, std::memory_order_relaxed);
  sHealthyLoopCount.store(0, std::memory_order_relaxed);

  ProbationPhase expected = ProbationPhase::Setup;
  if (!sPhase.compare_exchange_strong(expected, ProbationPhase::Running,
                                      std::memory_order_acq_rel)) {
    return;
  }
  ESP_LOGW(kTag, "Setup reached RUNNING; starting 60-second OTA probation");
}

void otaSafetyLoopHeartbeat(bool coreHealthy) {
  if (sPhase.load(std::memory_order_acquire) != ProbationPhase::Running) return;

  const TickType_t now = xTaskGetTickCount();
  const TickType_t previous =
      sLastLoopHeartbeatTick.exchange(now, std::memory_order_relaxed);

  if (!coreHealthy ||
      elapsedTicks(now, previous) > kHealthyHeartbeatGapTicks) {
    sHealthyStartedTick.store(now, std::memory_order_relaxed);
    sHealthyLoopCount.store(coreHealthy ? 1U : 0U, std::memory_order_relaxed);
    return;
  }

  const uint32_t healthyLoops =
      sHealthyLoopCount.fetch_add(1, std::memory_order_relaxed) + 1;
  const TickType_t healthyStarted =
      sHealthyStartedTick.load(std::memory_order_relaxed);
  if (healthyLoops < kMinimumHealthyLoops ||
      elapsedTicks(now, healthyStarted) < kHealthyIntervalTicks) {
    return;
  }

  const TickType_t lastAttempt =
      sLastMarkAttemptTick.load(std::memory_order_relaxed);
  if (lastAttempt != 0 && elapsedTicks(now, lastAttempt) < kMarkRetryTicks) return;
  sLastMarkAttemptTick.store(now, std::memory_order_relaxed);

  ProbationPhase expected = ProbationPhase::Running;
  if (!sPhase.compare_exchange_strong(expected, ProbationPhase::MarkingValid,
                                      std::memory_order_acq_rel)) {
    return;
  }

  if (!otaSystemCanMarkImageValid()) {
    // Keep the image pending. The supervisor's hard limit will reboot it and
    // let the bootloader roll back; an incoherent/missing OTA journal must
    // never be papered over by accepting the image anyway.
    rebootPendingImage(OTA_PROBATION_JOURNAL_REFUSED,
                     "OTA trial journal is missing or inconsistent");
  }

  const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) {
    sPhase.store(ProbationPhase::Validated, std::memory_order_release);
    otaSystemOnImageMarkedValid();
    ESP_LOGI(kTag,
             "OTA image marked valid after a full healthy 60-second probation");
    return;
  }

  ESP_LOGE(kTag, "Could not mark OTA image valid: %s (0x%x); will retry",
           esp_err_to_name(err), err);
  sPhase.store(ProbationPhase::Running, std::memory_order_release);
}

// Some vendored stacks automatically bulk-erase NVS after an init error. Block
// only whole-partition erase attempts on every boot; ordinary page erases used
// by NVS garbage collection remain legal.
extern "C" esp_err_t __real_nvs_flash_erase(void);
extern "C" esp_err_t __wrap_nvs_flash_erase(void) {
  ESP_EARLY_LOGE(kTag, "Blocked automatic full NVS erase; retained data preserved");
  return ESP_ERR_INVALID_STATE;
}

extern "C" esp_err_t __real_esp_partition_erase_range(
    const esp_partition_t* partition, size_t offset, size_t size);
extern "C" esp_err_t __wrap_esp_partition_erase_range(
    const esp_partition_t* partition, size_t offset, size_t size) {
  if (isNvsPartition(partition) && offset == 0 && size == partition->size) {
    ESP_EARLY_LOGE(kTag,
                   "Blocked automatic full erase of NVS partition '%s'",
                   partition && partition->label[0] ? partition->label : "?");
    return ESP_ERR_INVALID_STATE;
  }
  return __real_esp_partition_erase_range(partition, offset, size);
}
