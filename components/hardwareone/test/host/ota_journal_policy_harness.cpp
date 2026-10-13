// Host harness for System_OTA's emergency recovery arm.
//
// phaseName, commitRecord, transitionAndCommit, armRecoveryBoot,
// bootRecoveryForRecord, resumeRecoveryBootIntent and armEmergencyRecovery are
// inserted unchanged from System_OTA.cpp by test_ota_journal_policy.py, and the
// journal state machine is the real hw1_ota_protocol.c linked beside this file.
// Only the NVS slot store, the factory-image check, the boot-partition switch and
// esp_random are fakes.
#include "hw1_ota_protocol.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

using esp_err_t = int;
#define ESP_OK 0
#define ESP_FAIL -1

static unsigned assertions = 0;
static void check(bool condition, const char* description) {
  ++assertions;
  if (!condition) throw std::runtime_error(description);
}

static const char* esp_err_to_name(esp_err_t err) { return err == ESP_OK ? "ESP_OK" : "ESP_FAIL"; }

struct esp_partition_t {
  const char* label;
};
static const esp_partition_t gFactory{"factory"};

typedef struct {
  uint8_t valid_slots;
  uint8_t corrupt_slots;
  uint8_t selected_slot;
  uint32_t sequence;
} hw1_ota_nvs_info_t;

// The durable journal: one committed record, with the store's sequence check.
static hw1_ota_record_t gStored;
static unsigned gCommits = 0;
static esp_err_t hw1_ota_nvs_commit(const hw1_ota_record_t* desired, uint32_t expected_sequence,
                                    hw1_ota_record_t* stored, hw1_ota_nvs_info_t* info) {
  if (expected_sequence != gStored.sequence) return ESP_FAIL;
  if (hw1_ota_record_validate(desired) != HW1_OTA_OK) return ESP_FAIL;
  gStored = *desired;
  gStored.sequence = gStored.sequence + 1u;
  *stored = gStored;
  info->sequence = gStored.sequence;
  ++gCommits;
  return ESP_OK;
}

static bool gFactoryValid = true;
static unsigned gVerifyCalls = 0;
static bool verifyFactoryUpdater(const esp_partition_t** partitionOut, char*, size_t,
                                 char* reason, size_t reasonSize) {
  ++gVerifyCalls;
  if (!gFactoryValid) {
    std::snprintf(reason, reasonSize, "Error: factory recovery image failed verification");
    return false;
  }
  if (partitionOut) *partitionOut = &gFactory;
  return true;
}

static esp_err_t gSetBootResult = ESP_OK;
static unsigned gSetBootCalls = 0;
static const esp_partition_t* gBootTarget = nullptr;
static esp_err_t esp_ota_set_boot_partition(const esp_partition_t* partition) {
  ++gSetBootCalls;
  if (gSetBootResult == ESP_OK) gBootTarget = partition;
  return gSetBootResult;
}

static uint32_t esp_random() {
  static uint32_t counter = 0x5000u;
  return ++counter;
}

// INSERT_FUNCTIONS

static constexpr uint16_t kPolicy =
    HW1_OTA_REQUEST_ALLOW_DOWNGRADE | HW1_OTA_REQUEST_FORCED_POWER_OVERRIDE;

static hw1_ota_verified_manifest_t makeVerified() {
  hw1_ota_verified_manifest_t verified{};
  std::snprintf(verified.manifest.board_id, sizeof(verified.manifest.board_id), "feathers3");
  std::snprintf(verified.manifest.layout_id, sizeof(verified.manifest.layout_id), "hw1-f3-ota-v1");
  std::snprintf(verified.manifest.project_name, sizeof(verified.manifest.project_name), "HardwareOne");
  std::snprintf(verified.manifest.version, sizeof(verified.manifest.version), "1.2.3+f3o1");
  std::snprintf(verified.manifest.min_updater_version, sizeof(verified.manifest.min_updater_version),
                "1.0.0");
  verified.manifest.format_version = HW1_OTA_MANIFEST_FORMAT_VERSION;
  verified.manifest.image_size = 4096;
  verified.manifest.data_schema = 1;
  for (size_t i = 0; i < HW1_OTA_SHA256_SIZE; ++i) {
    verified.manifest.image_sha256[i] = static_cast<uint8_t>(i + 1);
  }
  verified.signature_algorithm = HW1_OTA_SIGNATURE_RSA_PSS_SHA256;
  return verified;
}

static hw1_ota_record_t freshJournal() {
  std::memset(&gStored, 0, sizeof(gStored));
  hw1_ota_record_init(&gStored);
  gCommits = 0;
  hw1_ota_record_t record = gStored;
  return record;
}

static void step(hw1_ota_record_t& record, hw1_ota_event_t event,
                 const hw1_ota_transition_args_t* args = nullptr) {
  char reason[96]{};
  check(transitionAndCommit(record, event, args, reason, sizeof(reason)), "setup transition commits");
}

static void begin(hw1_ota_record_t& record, uint64_t operation, hw1_ota_source_t source,
                  uint16_t flags) {
  char reason[96]{};
  const hw1_ota_verified_manifest_t verified = makeVerified();
  check(hw1_ota_begin(&record, operation, source, flags,
                      source == HW1_OTA_SOURCE_STAGED_FILE ? &verified : nullptr) == HW1_OTA_OK,
        "setup operation begins");
  check(commitRecord(record, reason, sizeof(reason)), "setup operation commits");
}

static void resetPlatform() {
  gFactoryValid = true;
  gVerifyCalls = 0;
  gSetBootResult = ESP_OK;
  gSetBootCalls = 0;
  gBootTarget = nullptr;
  gCommits = 0;
}

static bool arm(hw1_ota_record_t& record, std::string* reasonOut = nullptr) {
  char reason[160]{};
  const bool armed = armEmergencyRecovery(record, "replaced by emergency recovery", reason,
                                          sizeof(reason));
  if (reasonOut) *reasonOut = reason;
  return armed;
}

static void scenarioArmedReplaySwitchFailureIsJournaled() {
  hw1_ota_record_t record = freshJournal();
  begin(record, 7, HW1_OTA_SOURCE_RECOVERY_UPLOAD, 0);
  step(record, HW1_OTA_EVENT_ARM_RECOVERY_BOOT);
  resetPlatform();
  gSetBootResult = ESP_FAIL;
  check(!arm(record), "a failed factory switch is reported");
  check(gStored.phase == HW1_OTA_PHASE_FAILED, "the armed intent does not stay RECOVERY_BOOT_ARMED");
  check(gStored.last_result.code == HW1_OTA_RESULT_BOOT_SWITCH_ERROR,
        "the failed switch is journaled as BOOT_SWITCH_ERROR");
  check(gStored.last_result.native_error == ESP_FAIL, "the native error is kept");
  check(gStored.last_result.operation_id == 7, "the result belongs to the replayed operation");
}

static void scenarioArmedReplaySelectsFactory() {
  hw1_ota_record_t record = freshJournal();
  begin(record, 8, HW1_OTA_SOURCE_RECOVERY_UPLOAD, HW1_OTA_REQUEST_ALLOW_DOWNGRADE);
  step(record, HW1_OTA_EVENT_ARM_RECOVERY_BOOT);
  resetPlatform();
  check(arm(record), "an armed intent replays");
  check(gBootTarget == &gFactory && gSetBootCalls == 1, "the factory updater is selected once");
  check(gStored.phase == HW1_OTA_PHASE_RECOVERY_BOOT_ARMED && gStored.operation_id == 8,
        "the replay keeps the operation it found");
  check(gStored.request_flags == HW1_OTA_REQUEST_ALLOW_DOWNGRADE,
        "a live operation keeps its own request flags");
}

static void scenarioRequestedRecoveryReplays() {
  hw1_ota_record_t record = freshJournal();
  begin(record, 9, HW1_OTA_SOURCE_RECOVERY_UPLOAD, 0);
  resetPlatform();
  check(arm(record), "an interrupted direct-recovery request replays");
  check(gStored.phase == HW1_OTA_PHASE_RECOVERY_BOOT_ARMED && gStored.operation_id == 9,
        "the request is armed, not replaced");
  check(gVerifyCalls == 1, "the factory image is verified once");
}

static void scenarioFailedRecordIsReplacedWithoutPolicy() {
  hw1_ota_record_t record = freshJournal();
  begin(record, 10, HW1_OTA_SOURCE_STAGED_FILE, kPolicy);
  step(record, HW1_OTA_EVENT_ARM_RECOVERY_BOOT);
  hw1_ota_transition_args_t failure{};
  failure.result_code = HW1_OTA_RESULT_POWER_UNSAFE;
  failure.detail = "battery too low";
  step(record, HW1_OTA_EVENT_FAIL, &failure);
  resetPlatform();
  check(arm(record), "a failed record is replaced by an emergency operation");
  check(gStored.operation_id != 10 && gStored.operation_id != 0, "a new operation is opened");
  check(gStored.source == HW1_OTA_SOURCE_RECOVERY_UPLOAD, "the new operation is a direct recovery");
  check(gStored.phase == HW1_OTA_PHASE_RECOVERY_BOOT_ARMED, "the new operation is armed");
  check(gStored.request_flags == HW1_OTA_REQUEST_SUPERSEDE_PENDING_RESULT,
        "only the pending result is superseded; no policy flag is inherited");
  check(gStored.last_result.code == HW1_OTA_RESULT_POWER_UNSAFE,
        "the earlier failure stays readable");
  check(gBootTarget == &gFactory, "the factory updater is selected");
}

static void scenarioNewOperationSwitchFailureIsJournaled() {
  hw1_ota_record_t record = freshJournal();
  resetPlatform();
  gSetBootResult = ESP_FAIL;
  check(!arm(record), "a failed switch on a fresh journal is reported");
  check(gStored.phase == HW1_OTA_PHASE_FAILED, "the new operation does not stay armed");
  check(gStored.last_result.code == HW1_OTA_RESULT_BOOT_SWITCH_ERROR,
        "the failed switch is journaled as BOOT_SWITCH_ERROR");
  check(gStored.request_flags == 0, "an idle journal with no pending result needs no flag");
}

static void scenarioStagedRequestIsCanceledFirst() {
  hw1_ota_record_t record = freshJournal();
  begin(record, 11, HW1_OTA_SOURCE_STAGED_FILE, HW1_OTA_REQUEST_FORCED_POWER_OVERRIDE);
  resetPlatform();
  check(arm(record), "an unlaunched staged request is replaced");
  check(gStored.last_result.code == HW1_OTA_RESULT_CANCELED &&
            gStored.last_result.operation_id == 11,
        "the staged request is canceled with its own result");
  check(std::strcmp(gStored.last_result.detail, "replaced by emergency recovery") == 0,
        "the caller's cancel detail is journaled");
  check(gStored.phase == HW1_OTA_PHASE_RECOVERY_BOOT_ARMED && gStored.operation_id != 11,
        "a new direct-recovery operation is armed");
  check(gStored.request_flags == HW1_OTA_REQUEST_SUPERSEDE_PENDING_RESULT,
        "the forced-power override of the canceled request is not inherited");
}

static void scenarioUnusableFactoryLeavesJournal() {
  hw1_ota_record_t record = freshJournal();
  begin(record, 12, HW1_OTA_SOURCE_STAGED_FILE, 0);
  const hw1_ota_record_t before = gStored;
  resetPlatform();
  gFactoryValid = false;
  std::string reason;
  check(!arm(record, &reason), "an unusable factory image refuses the arm");
  check(gCommits == 0 && gSetBootCalls == 0, "nothing is committed or switched");
  check(std::memcmp(&gStored, &before, sizeof(before)) == 0, "the journal is unchanged");
  check(reason.find("factory") != std::string::npos, "the reason names the factory image");
}

static void scenarioActivePhaseIsRefused(bool trial) {
  hw1_ota_record_t record = freshJournal();
  begin(record, 13, trial ? HW1_OTA_SOURCE_STAGED_FILE : HW1_OTA_SOURCE_RECOVERY_UPLOAD, 0);
  step(record, HW1_OTA_EVENT_RECOVERY_STARTED);
  step(record, HW1_OTA_EVENT_APPLY_STARTED);
  if (trial) {
    hw1_ota_transition_args_t verified{};
    const hw1_ota_verified_manifest_t manifest = makeVerified();
    verified.verified_manifest = &manifest;
    step(record, HW1_OTA_EVENT_IMAGE_VERIFIED, &verified);
    step(record, HW1_OTA_EVENT_TRIAL_STARTED);
  }
  const hw1_ota_record_t before = gStored;
  resetPlatform();
  std::string reason;
  check(!arm(record, &reason), "a live operation is not replaced");
  check(gVerifyCalls == 0 && gCommits == 0 && gSetBootCalls == 0,
        "the refusal touches neither the factory image nor the journal");
  check(std::memcmp(&gStored, &before, sizeof(before)) == 0, "the journal is unchanged");
  check(reason.find(trial ? "trial" : "applying") != std::string::npos, "the reason names the phase");
}

int main() {
  try {
    scenarioArmedReplaySwitchFailureIsJournaled();
    scenarioArmedReplaySelectsFactory();
    scenarioRequestedRecoveryReplays();
    scenarioFailedRecordIsReplacedWithoutPolicy();
    scenarioNewOperationSwitchFailureIsJournaled();
    scenarioStagedRequestIsCanceledFirst();
    scenarioUnusableFactoryLeavesJournal();
    scenarioActivePhaseIsRefused(false);
    scenarioActivePhaseIsRefused(true);
  } catch (const std::exception& failure) {
    std::fprintf(stderr, "FAIL: %s\n", failure.what());
    return 1;
  }
  std::printf("OTA emergency recovery arm: %u checks passed\n", assertions);
  return 0;
}
