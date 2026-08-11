#include "System_OTA.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "System_AuthIdentity.h"
#include "System_Battery.h"
#include "System_Command.h"
#include "System_OTASafety.h"
#include "System_CommandTypes.h"
#include "System_CrashRecord.h"
#include "System_Events.h"
#include "System_SelfDevice.h"
#include "System_MemUtil.h"  // ps_alloc — image hashing must not use cmd_exec_task's stack
#include "System_Mutex.h"
#include "System_VFS.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "hw1_ota_idf.h"
#include "hw1_ota_nvs.h"
#include "hw1_ota_protocol.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#if ENABLE_BLUETOOTH
#include "Bluetooth.h"
#include "System_BleSecureChannel.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

#ifndef HW1_OTA_LAYOUT
#define HW1_OTA_LAYOUT 0
#endif

namespace {

constexpr char kTag[] = "HW1-OTA";
constexpr char kCandidatePart[] = "/system/ota/candidate.part";
constexpr char kManifestPart[] = "/system/ota/manifest.part";
constexpr char kCandidatePath[] = "/system/ota/candidate.bin";
constexpr char kManifestPath[] = "/system/ota/manifest.json";
constexpr size_t kManifestEnvelopeMax = 2048;
constexpr uint32_t kOtaSlotSize = 0x5A0000U;
constexpr uint32_t kDataSchemaV1 = 1;
constexpr uint32_t kFreshPowerMs = 30000;
constexpr float kMinimumBatteryPercent = 30.0f;
constexpr size_t kMinimumRecoveryPassphrase = 12;
constexpr size_t kMaximumRecoveryPassphrase = 63;
constexpr uint32_t kCrashLoopEscapeCount = 3;
constexpr size_t kBleUploadHeaderSize = 12;  // 00 + HW1OTA + version + offset_be32
constexpr size_t kBleUploadChunkMax = 477;   // 489-byte secure plaintext at MTU 517
// 60 s, not 30 s: Android's own GATT operation timeout is ~30 s, so a single
// stalled write on the phone freezes the uploader for almost exactly that long.
// At 30 s this timer expired in the same breath and discarded a transfer the
// phone was about to resume, turning a recoverable hiccup into a full re-upload
// (observed killing a 2.2 MB staging run after a 32.6 s stall). The window must
// sit clearly above the peer's stall ceiling, not level with it.
constexpr uint32_t kBleUploadIdleTimeoutMs = 60000;

#if HW1_OTA_LAYOUT

#if defined(HW1_OTA_BOARD_ID) && defined(HW1_OTA_LAYOUT_ID)
#if defined(CONFIG_SECURE_FLASH_ENC_ENABLED) && CONFIG_SECURE_FLASH_ENC_ENABLED
constexpr char kRequiredVersionSuffix[] = HW1_OTA_VERSION_SUFFIX_FEATHERS3_FLASH_ENCRYPTED;
#else
constexpr char kRequiredVersionSuffix[] = HW1_OTA_VERSION_SUFFIX_FEATHERS3_PLAIN;
#endif
#else
#error "The OTA layout requires HW1_OTA_BOARD_ID and HW1_OTA_LAYOUT_ID"
#endif

extern "C" const uint8_t _binary_hw1_ota_public_key_pem_start[];
extern "C" const uint8_t _binary_hw1_ota_public_key_pem_end[];

struct CandidateInfo {
  hw1_ota_verified_manifest_t verified{};
  esp_app_desc_t appDesc{};
  uint8_t digest[HW1_OTA_SHA256_SIZE]{};
  uint32_t size = 0;
};

#if ENABLE_BLUETOOTH
struct BleUploadSession {
  bool active = false;
  bool failed = false;
  uint16_t connId = 0;
  char member[16]{};
  const char* path = nullptr;
  uint32_t expected = 0;
  uint32_t received = 0;
  uint32_t lastActivityMs = 0;
  uint8_t expectedDigest[32]{};
  mbedtls_sha256_context sha{};
  bool shaInitialized = false;
  File file;
  char warning[96]{};
  // Authorized operator, captured once at `begin`. Per-frame authorization
  // compares against this instead of re-deriving the role, because
  // isSuperAdminUser() reads and parses the whole of users.json off LittleFS
  // under the global filesystem lock (System_User.cpp:358) — at ~11k frames
  // per image that starved the BLE controller badly enough to lose the link.
  // The privilege gate itself is unchanged: `begin` and `finish` both still go
  // through currentSecureBleConnection(), which does the full superadmin check.
  char authUser[40]{};
  uint32_t resumedFrom = 0;
};

BleUploadSession gBleUpload;
SemaphoreHandle_t gBleUploadMutex = nullptr;

// Diagnostics that must outlive a teardown, so `otawrite status` can explain a
// transfer that already collapsed. Without these the whole staging path emitted
// no evidence at all and failures could only be inferred from the peer's side.
uint32_t gBleUploadFramesAccepted = 0;
uint32_t gBleUploadFramesRejected = 0;
uint32_t gBleUploadResumedFrom = 0;
// Wall-clock cost of the last resume re-hash. Reported so the margin against
// the 5 s task-watchdog threshold is a measurement rather than an assumption.
uint32_t gBleUploadResumeMs = 0;
char gBleUploadLastTeardown[40] = "none";
// Incremented by the shared command queue whenever a submission is dropped
// because the queue was full (System_Utils.cpp).
extern "C" volatile uint32_t gCmdExecDropCount;

bool ensureBleUploadMutex() {
  if (gBleUploadMutex) return true;
  gBleUploadMutex = xSemaphoreCreateMutex();
  return gBleUploadMutex != nullptr;
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parseSha256Hex(const String& text, uint8_t output[32]) {
  if (text.length() != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    const int hi = hexNibble(text[i * 2]);
    const int lo = hexNibble(text[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    output[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

void resetBleUploadLocked(bool removePartial, const char* reason = nullptr) {
  if (reason && gBleUpload.active) {
    snprintf(gBleUploadLastTeardown, sizeof(gBleUploadLastTeardown), "%s@%" PRIu32,
             reason, gBleUpload.received);
  }
  const char* path = gBleUpload.path;
  if (gBleUpload.file) {
    FsLockGuard guard("ota.ble.close");
    gBleUpload.file.flush();
    gBleUpload.file.close();
  }
  if (gBleUpload.shaInitialized) {
    mbedtls_sha256_free(&gBleUpload.sha);
    gBleUpload.shaInitialized = false;
  }
  if (removePartial && path) {
    const AuthContext auth = VFS::systemAuth(VFS::Scopes::OTA, "hwone.ota_ble_abort");
    FsLockGuard guard("ota.ble.remove");
    if (VFS::existsGuarded(path, auth)) (void)VFS::removeGuarded(path, auth);
  }
  gBleUpload.active = false;
  gBleUpload.failed = false;
  gBleUpload.connId = 0;
  gBleUpload.member[0] = '\0';
  gBleUpload.path = nullptr;
  gBleUpload.expected = 0;
  gBleUpload.received = 0;
  gBleUpload.lastActivityMs = 0;
  memset(gBleUpload.expectedDigest, 0, sizeof(gBleUpload.expectedDigest));
  gBleUpload.warning[0] = '\0';
  memset(gBleUpload.authUser, 0, sizeof(gBleUpload.authUser));
  gBleUpload.resumedFrom = 0;
}

// Sidecar naming the contract a partial was written under, so a resume can
// prove the bytes on flash belong to the image now being requested rather than
// to an abandoned earlier one.
String uploadMetaPath(const char* path) { return String(path) + ".meta"; }

void writeUploadMeta(const char* path, uint32_t size, const uint8_t digest[32],
                     const AuthContext& auth) {
  char line[80];
  int n = snprintf(line, sizeof(line), "%" PRIu32 ":", size);
  for (size_t i = 0; i < 32 && n > 0 && n < (int)sizeof(line) - 2; ++i) {
    n += snprintf(line + n, sizeof(line) - n, "%02x", digest[i]);
  }
  File meta = VFS::openGuarded(uploadMetaPath(path), "w", auth, true);
  if (!meta) return;
  meta.write(reinterpret_cast<const uint8_t*>(line), strlen(line));
  meta.flush();
  meta.close();
}

bool uploadMetaMatches(const char* path, uint32_t size, const uint8_t digest[32],
                       const AuthContext& auth) {
  File meta = VFS::openGuarded(uploadMetaPath(path), "r", auth);
  if (!meta) return false;
  char stored[80]{};
  const int read = meta.read(reinterpret_cast<uint8_t*>(stored), sizeof(stored) - 1);
  meta.close();
  if (read <= 0) return false;
  stored[read] = '\0';
  char want[80];
  int n = snprintf(want, sizeof(want), "%" PRIu32 ":", size);
  for (size_t i = 0; i < 32 && n > 0 && n < (int)sizeof(want) - 2; ++i) {
    n += snprintf(want + n, sizeof(want) - n, "%02x", digest[i]);
  }
  return strncmp(stored, want, sizeof(want)) == 0;
}

bool currentSecureBleConnection(uint16_t& connId, char* reason, size_t reasonSize) {
  const auto* context = static_cast<CommandContext*>(currentCommandContext());
  if (!context || context->origin != ORIGIN_BLUETOOTH ||
      context->auth.transport != SOURCE_BLUETOOTH) {
    snprintf(reason, reasonSize, "Error: otawrite is available only through Bluetooth");
    return false;
  }
  const char* sidText = context->auth.sid.c_str();
  if (!sidText || sidText[0] == '\0') {
    snprintf(reason, reasonSize, "Error: otawrite requires the encrypted Bluetooth channel");
    return false;
  }
  char* sidEnd = nullptr;
  const unsigned long parsed = strtoul(sidText, &sidEnd, 10);
  if (!sidEnd || *sidEnd != '\0' || parsed > UINT16_MAX ||
      !bleScEstablished(static_cast<uint16_t>(parsed))) {
    snprintf(reason, reasonSize, "Error: otawrite requires the encrypted Bluetooth channel");
    return false;
  }
  String user;
  connId = static_cast<uint16_t>(parsed);
  if (!bleGetAuthenticatedUser(connId, user) || !isSuperAdminUser(user)) {
    snprintf(reason, reasonSize, "Error: otawrite requires a live superadmin Bluetooth session");
    return false;
  }
  return true;
}
#endif

const char* phaseName(hw1_ota_phase_t phase) {
  switch (phase) {
    case HW1_OTA_PHASE_IDLE: return "idle";
    case HW1_OTA_PHASE_REQUESTED: return "requested";
    case HW1_OTA_PHASE_RECOVERY_BOOT_ARMED: return "recovery_boot_armed";
    case HW1_OTA_PHASE_RECOVERY_RUNNING: return "recovery_running";
    case HW1_OTA_PHASE_APPLYING: return "applying";
    case HW1_OTA_PHASE_IMAGE_VERIFIED: return "image_verified";
    case HW1_OTA_PHASE_TRIAL_BOOT_ARMED: return "trial_boot_armed";
    case HW1_OTA_PHASE_TRIAL_RUNNING: return "trial_running";
    case HW1_OTA_PHASE_SUCCEEDED: return "succeeded";
    case HW1_OTA_PHASE_FAILED: return "failed";
    case HW1_OTA_PHASE_CANCELED: return "canceled";
  }
  return "unknown";
}

const char* resultName(hw1_ota_result_code_t code) {
  switch (code) {
    case HW1_OTA_RESULT_NONE: return "none";
    case HW1_OTA_RESULT_SUCCESS: return "success";
    case HW1_OTA_RESULT_CANCELED: return "canceled";
    case HW1_OTA_RESULT_MANIFEST_INVALID: return "manifest_invalid";
    case HW1_OTA_RESULT_SIGNATURE_INVALID: return "signature_invalid";
    case HW1_OTA_RESULT_INCOMPATIBLE_IMAGE: return "incompatible_image";
    case HW1_OTA_RESULT_DIGEST_MISMATCH: return "digest_mismatch";
    case HW1_OTA_RESULT_POWER_UNSAFE: return "power_unsafe";
    case HW1_OTA_RESULT_STORAGE_ERROR: return "storage_error";
    case HW1_OTA_RESULT_FLASH_ERROR: return "flash_error";
    case HW1_OTA_RESULT_BOOT_SWITCH_ERROR: return "boot_switch_error";
    case HW1_OTA_RESULT_HEALTH_TIMEOUT: return "health_timeout";
    case HW1_OTA_RESULT_ROLLBACK_DETECTED: return "rollback_detected";
    case HW1_OTA_RESULT_INTERNAL_ERROR: return "internal_error";
  }
  return "unknown";
}

bool commandIsAutomation() {
  auto* ctx = static_cast<CommandContext*>(currentCommandContext());
  return ctx && (ctx->origin == ORIGIN_AUTOMATION || ctx->automationName[0] != '\0');
}

bool loadRecord(hw1_ota_record_t& record, hw1_ota_nvs_info_t* info,
                bool allowEmpty, char* reason, size_t reasonSize) {
  memset(&record, 0, sizeof(record));
  if (info) memset(info, 0, sizeof(*info));
  esp_err_t err = hw1_ota_nvs_load(&record, info);
  if (err == ESP_OK) return true;
  if (err == ESP_ERR_NVS_NOT_FOUND && allowEmpty) {
    hw1_ota_record_init(&record);
    return true;
  }
  if (reason && reasonSize) {
    snprintf(reason, reasonSize, "Error: OTA journal unavailable: %s", esp_err_to_name(err));
  }
  return false;
}

bool commitRecord(hw1_ota_record_t& record, char* reason, size_t reasonSize) {
  hw1_ota_record_t stored{};
  hw1_ota_nvs_info_t info{};
  esp_err_t err = hw1_ota_nvs_commit(&record, record.sequence, &stored, &info);
  if (err != ESP_OK) {
    if (reason && reasonSize) {
      snprintf(reason, reasonSize, "Error: OTA journal commit failed: %s", esp_err_to_name(err));
    }
    return false;
  }
  record = stored;
  return true;
}

bool transitionAndCommit(hw1_ota_record_t& record, hw1_ota_event_t event,
                         const hw1_ota_transition_args_t* args,
                         char* reason, size_t reasonSize) {
  const hw1_ota_status_t status = hw1_ota_transition(&record, event, args);
  if (status != HW1_OTA_OK) {
    if (reason && reasonSize) {
      snprintf(reason, reasonSize, "Error: OTA state transition rejected (%d)", (int)status);
    }
    return false;
  }
  return commitRecord(record, reason, reasonSize);
}

bool fieldToString(const char* field, size_t fieldSize, char* output,
                   size_t outputSize) {
  const void* end = memchr(field, '\0', fieldSize);
  if (!end || !output || outputSize == 0) return false;
  const size_t length = static_cast<const char*>(end) - field;
  if (length == 0 || length >= outputSize) return false;
  memcpy(output, field, length);
  output[length] = '\0';
  return true;
}

bool verifyFactoryUpdater(const esp_partition_t** partitionOut,
                          char* updaterVersion, size_t updaterVersionSize,
                          char* reason, size_t reasonSize) {
  const esp_partition_t* factory = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, "factory");
  if (!factory) {
    snprintf(reason, reasonSize, "Error: factory recovery partition not found");
    return false;
  }

  esp_partition_pos_t position{factory->address, factory->size};
  esp_image_metadata_t metadata{};
  metadata.start_addr = factory->address;
  esp_err_t err = esp_image_verify(ESP_IMAGE_VERIFY, &position, &metadata);
  if (err != ESP_OK) {
    snprintf(reason, reasonSize, "Error: factory recovery image failed verification: %s",
             esp_err_to_name(err));
    return false;
  }

  esp_app_desc_t desc{};
  err = esp_ota_get_partition_description(factory, &desc);
  char project[sizeof(desc.project_name) + 1]{};
  char version[sizeof(desc.version) + 1]{};
  if (err != ESP_OK ||
      !fieldToString(desc.project_name, sizeof(desc.project_name), project, sizeof(project)) ||
      !fieldToString(desc.version, sizeof(desc.version), version, sizeof(version)) ||
      strcmp(project, "hw1-updater") != 0 ||
      !String(version).endsWith(kRequiredVersionSuffix)) {
    snprintf(reason, reasonSize, "Error: factory partition is not the compatible HardwareOne updater");
    return false;
  }

  bool semverValid = false;
  (void)hw1_ota_semver_compare(version, version, &semverValid);
  if (!semverValid) {
    snprintf(reason, reasonSize, "Error: factory updater version is not valid semver");
    return false;
  }
  if (updaterVersion && updaterVersionSize) {
    snprintf(updaterVersion, updaterVersionSize, "%s", version);
  }
  if (partitionOut) *partitionOut = factory;
  return true;
}

bool decodeBase64Exact(const char* encoded, size_t encodedLength,
                       uint8_t* output, size_t outputSize) {
  if (!encoded || !output) return false;
  size_t decoded = 0;
  const int err = mbedtls_base64_decode(output, outputSize, &decoded,
      reinterpret_cast<const unsigned char*>(encoded), encodedLength);
  return err == 0 && decoded == outputSize;
}

bool loadVerifiedManifest(const char* path,
                          hw1_ota_verified_manifest_t& verified,
                          char* reason, size_t reasonSize) {
  const AuthContext auth = VFS::systemAuth(VFS::Scopes::OTA, "hwone.ota_manifest");
  File file = VFS::openGuarded(path, "r", auth);
  if (!file) {
    snprintf(reason, reasonSize, "Error: manifest file is missing");
    return false;
  }
  const size_t size = file.size();
  if (size == 0 || size > kManifestEnvelopeMax) {
    file.close();
    snprintf(reason, reasonSize, "Error: manifest envelope size is invalid");
    return false;
  }

  JsonDocument doc;
  const DeserializationError jsonError = deserializeJson(doc, file);
  file.close();
  JsonObjectConst root = doc.as<JsonObjectConst>();
  JsonObjectConst signatureObject = root["signature"].as<JsonObjectConst>();
  if (jsonError || root.isNull() || root.size() != 4 ||
      signatureObject.isNull() || signatureObject.size() != 2 ||
      !root["format"].is<const char*>() ||
      !root["formatVersion"].is<unsigned int>() ||
      !root["payload"].is<const char*>() ||
      !signatureObject["algorithm"].is<const char*>() ||
      !signatureObject["value"].is<const char*>()) {
    snprintf(reason, reasonSize, "Error: manifest envelope is malformed");
    return false;
  }
  if (strcmp(root["format"].as<const char*>(), HW1_OTA_MANIFEST_ENVELOPE_FORMAT) != 0 ||
      root["formatVersion"].as<unsigned int>() != HW1_OTA_MANIFEST_FORMAT_VERSION ||
      strcmp(signatureObject["algorithm"].as<const char*>(),
             HW1_OTA_MANIFEST_SIGNATURE_ALGORITHM) != 0) {
    snprintf(reason, reasonSize, "Error: manifest envelope format is unsupported");
    return false;
  }

  const char* payloadText = root["payload"].as<const char*>();
  const char* signatureText = signatureObject["value"].as<const char*>();
  uint8_t payload[HW1_OTA_MANIFEST_PAYLOAD_WIRE_SIZE]{};
  uint8_t signature[HW1_OTA_RSA3072_SIGNATURE_SIZE]{};
  if (strlen(payloadText) != 300 || strlen(signatureText) != 512 ||
      !decodeBase64Exact(payloadText, strlen(payloadText), payload, sizeof(payload)) ||
      !decodeBase64Exact(signatureText, strlen(signatureText), signature, sizeof(signature))) {
    snprintf(reason, reasonSize, "Error: manifest payload or signature base64 is invalid");
    return false;
  }

  const size_t pemSize = static_cast<size_t>(
      _binary_hw1_ota_public_key_pem_end - _binary_hw1_ota_public_key_pem_start);
  hw1_ota_idf_rsa_public_key_t key{
      .public_key_pem = _binary_hw1_ota_public_key_pem_start,
      .public_key_pem_size = pemSize,
  };
  const hw1_ota_status_t status = hw1_ota_manifest_verify(
      payload, sizeof(payload), signature, sizeof(signature),
      hw1_ota_idf_rsa3072_pss_sha256_verify, &key, &verified);
  memset(payload, 0, sizeof(payload));
  memset(signature, 0, sizeof(signature));
  if (status != HW1_OTA_OK) {
    snprintf(reason, reasonSize, "Error: manifest signature verification failed (%d)", (int)status);
    return false;
  }
  return true;
}

bool hashAndDescribeImage(const char* path, CandidateInfo& candidate,
                          char* reason, size_t reasonSize) {
  const AuthContext auth = VFS::systemAuth(VFS::Scopes::OTA, "hwone.ota_candidate");
  File file = VFS::openGuarded(path, "r", auth);
  if (!file) {
    snprintf(reason, reasonSize, "Error: candidate image is missing");
    return false;
  }
  const size_t fileSize = file.size();
  if (fileSize == 0 || fileSize > kOtaSlotSize || fileSize > UINT32_MAX) {
    file.close();
    snprintf(reason, reasonSize, "Error: candidate image size does not fit ota_0");
    return false;
  }

  esp_image_header_t header{};
  esp_image_segment_header_t firstSegment{};
  if (file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header) ||
      file.read(reinterpret_cast<uint8_t*>(&firstSegment), sizeof(firstSegment)) !=
          sizeof(firstSegment) ||
      file.read(reinterpret_cast<uint8_t*>(&candidate.appDesc), sizeof(candidate.appDesc)) !=
          sizeof(candidate.appDesc)) {
    file.close();
    snprintf(reason, reasonSize, "Error: candidate image header is truncated");
    return false;
  }
  if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.segment_count == 0 ||
      header.chip_id != ESP_CHIP_ID_ESP32S3 ||
      candidate.appDesc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
    file.close();
    snprintf(reason, reasonSize, "Error: candidate is not an ESP32-S3 application image");
    return false;
  }

  if (!file.seek(0, SeekSet)) {
    file.close();
    snprintf(reason, reasonSize, "Error: candidate image seek failed");
    return false;
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  int cryptoError = mbedtls_sha256_starts(&sha, 0);
  // Off the stack deliberately. This runs on cmd_exec_task, whose stack is 8192
  // BYTES, so a 4 KB automatic buffer here overflowed it and panicked the device
  // mid-validation — after the signature had verified but before anything was
  // journaled. Observed as "stack overflow in task cmd_exec_task" on the first
  // staging attempt that ever survived long enough to reach this code.
  constexpr size_t kHashChunk = 4096;
  uint8_t* buffer = static_cast<uint8_t*>(
      ps_alloc(kHashChunk, AllocPref::PreferPSRAM, "ota.hash.buf"));
  if (!buffer) {
    mbedtls_sha256_free(&sha);
    file.close();
    snprintf(reason, reasonSize, "Error: no memory to hash the candidate image");
    return false;
  }
  size_t total = 0;
  while (cryptoError == 0 && total < fileSize) {
    const size_t wanted = (fileSize - total) < kHashChunk ? (fileSize - total) : kHashChunk;
    const size_t count = file.read(buffer, wanted);
    if (count != wanted) {
      cryptoError = -1;
      break;
    }
    cryptoError = mbedtls_sha256_update(&sha, buffer, count);
    total += count;
  }
  if (cryptoError == 0) {
    cryptoError = mbedtls_sha256_finish(&sha, candidate.digest);
  }
  mbedtls_sha256_free(&sha);
  memset(buffer, 0, kHashChunk);
  free(buffer);
  file.close();
  if (cryptoError != 0 || total != fileSize) {
    memset(candidate.digest, 0, sizeof(candidate.digest));
    snprintf(reason, reasonSize, "Error: candidate image could not be hashed completely");
    return false;
  }
  candidate.size = static_cast<uint32_t>(fileSize);
  return true;
}

bool validateCandidate(const char* imagePath, const char* manifestPath,
                       bool allowDowngrade, CandidateInfo& candidate,
                       char* reason, size_t reasonSize) {
  memset(&candidate, 0, sizeof(candidate));
  char updaterVersion[33]{};
  if (!verifyFactoryUpdater(nullptr, updaterVersion, sizeof(updaterVersion),
                            reason, reasonSize) ||
      !loadVerifiedManifest(manifestPath, candidate.verified, reason, reasonSize) ||
      !hashAndDescribeImage(imagePath, candidate, reason, reasonSize)) {
    return false;
  }

  const hw1_ota_target_policy_t policy{
      .board_id = HW1_OTA_BOARD_ID,
      .layout_id = HW1_OTA_LAYOUT_ID,
      .project_name = "hardwareone-idf",
      .required_version_suffix = kRequiredVersionSuffix,
      .current_updater_version = updaterVersion,
      .maximum_image_size = kOtaSlotSize,
      .minimum_data_schema = kDataSchemaV1,
      .maximum_data_schema = kDataSchemaV1,
  };
  uint32_t mismatches = 0;
  const hw1_ota_status_t policyStatus = hw1_ota_manifest_validate(
      &candidate.verified, &policy, candidate.size, candidate.digest, &mismatches);
  if (policyStatus != HW1_OTA_OK) {
    snprintf(reason, reasonSize, "Error: candidate/manifest policy mismatch mask=0x%08" PRIx32,
             mismatches);
    return false;
  }

  char imageProject[sizeof(candidate.appDesc.project_name) + 1]{};
  char imageVersion[sizeof(candidate.appDesc.version) + 1]{};
  if (!fieldToString(candidate.appDesc.project_name,
                     sizeof(candidate.appDesc.project_name), imageProject,
                     sizeof(imageProject)) ||
      !fieldToString(candidate.appDesc.version, sizeof(candidate.appDesc.version),
                     imageVersion, sizeof(imageVersion)) ||
      strcmp(imageProject, candidate.verified.manifest.project_name) != 0 ||
      strcmp(imageVersion, candidate.verified.manifest.version) != 0) {
    snprintf(reason, reasonSize, "Error: candidate app descriptor does not match signed manifest");
    return false;
  }

  if (!allowDowngrade) {
    const esp_app_desc_t* running = esp_app_get_description();
    char runningVersion[sizeof(running->version) + 1]{};
    bool semverValid = false;
    if (!fieldToString(running->version, sizeof(running->version), runningVersion,
                       sizeof(runningVersion)) ||
        hw1_ota_semver_compare(imageVersion, runningVersion, &semverValid) < 0 ||
        !semverValid) {
      snprintf(reason, reasonSize,
               "Error: candidate is older than running firmware; use allow-downgrade explicitly");
      return false;
    }
  }
  return true;
}

bool credentialConfigured() {
  nvs_handle_t handle = 0;
  if (nvs_open(HW1_OTA_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
  size_t apLength = 0;
  size_t tokenLength = 0;
  const esp_err_t apErr = nvs_get_str(handle, "ap_pass", nullptr, &apLength);
  const esp_err_t tokenErr = nvs_get_str(handle, "auth_token", nullptr, &tokenLength);
  bool valid = apErr == ESP_OK && tokenErr == ESP_OK &&
               apLength > kMinimumRecoveryPassphrase &&
               apLength <= kMaximumRecoveryPassphrase + 1 &&
               tokenLength == apLength;
  char apPass[kMaximumRecoveryPassphrase + 1]{};
  char authToken[kMaximumRecoveryPassphrase + 1]{};
  if (valid) {
    size_t readAp = sizeof(apPass);
    size_t readToken = sizeof(authToken);
    valid = nvs_get_str(handle, "ap_pass", apPass, &readAp) == ESP_OK &&
            nvs_get_str(handle, "auth_token", authToken, &readToken) == ESP_OK &&
            readAp == readToken && memcmp(apPass, authToken, readAp) == 0;
  }
  nvs_close(handle);
  memset(apPass, 0, sizeof(apPass));
  memset(authToken, 0, sizeof(authToken));
  return valid;
}

bool storeCredential(const String& passphrase, char* reason, size_t reasonSize) {
  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(HW1_OTA_NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err == ESP_OK) err = nvs_set_str(handle, "ap_pass", passphrase.c_str());
  if (err == ESP_OK) err = nvs_set_str(handle, "auth_token", passphrase.c_str());
  if (err == ESP_OK) err = nvs_commit(handle);
  if (handle) nvs_close(handle);
  if (err != ESP_OK) {
    snprintf(reason, reasonSize, "Error: could not store recovery credential: %s",
             esp_err_to_name(err));
    return false;
  }
  return true;
}

bool clearCredential(char* reason = nullptr, size_t reasonSize = 0) {
  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(HW1_OTA_NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err == ESP_OK) {
    esp_err_t one = nvs_erase_key(handle, "ap_pass");
    if (one != ESP_OK && one != ESP_ERR_NVS_NOT_FOUND) err = one;
  }
  if (err == ESP_OK) {
    esp_err_t one = nvs_erase_key(handle, "auth_token");
    if (one != ESP_OK && one != ESP_ERR_NVS_NOT_FOUND) err = one;
  }
  if (err == ESP_OK) err = nvs_commit(handle);
  if (handle) nvs_close(handle);
  if (err != ESP_OK && reason && reasonSize) {
    snprintf(reason, reasonSize, "Error: could not clear recovery credential: %s",
             esp_err_to_name(err));
  }
  return err == ESP_OK;
}

bool powerIsSafe(bool force, char* reason, size_t reasonSize) {
  const uint32_t age = millis() - gBatteryState.lastReadMs;
  if (age <= kFreshPowerMs && gBatteryState.usbPresent) return true;
  if (age <= kFreshPowerMs && gBatteryState.status != BATTERY_NOT_PRESENT &&
      gBatteryState.status != BATTERY_UNKNOWN &&
      gBatteryState.percentage >= kMinimumBatteryPercent) {
    return true;
  }
  if (force) return true;
  snprintf(reason, reasonSize,
           "Error: unsafe or stale power state; connect USB or use a fresh battery >=30%% "
           "(force-power is an explicit recorded override)");
  return false;
}

void removeStagedFiles() {
  const AuthContext auth = VFS::systemAuth(VFS::Scopes::OTA, "hwone.ota_cleanup");
  FsLockGuard guard("ota.cleanup");
  if (VFS::existsGuarded(kCandidatePath, auth)) {
    (void)VFS::removeGuarded(kCandidatePath, auth);
  }
  if (VFS::existsGuarded(kManifestPath, auth)) {
    (void)VFS::removeGuarded(kManifestPath, auth);
  }
}

void removeAllOtaFiles() {
  removeStagedFiles();
  const AuthContext auth = VFS::systemAuth(VFS::Scopes::OTA, "hwone.ota_cleanup_parts");
  FsLockGuard guard("ota.cleanup.parts");
  if (VFS::existsGuarded(kCandidatePart, auth)) {
    (void)VFS::removeGuarded(kCandidatePart, auth);
  }
  if (VFS::existsGuarded(kManifestPart, auth)) {
    (void)VFS::removeGuarded(kManifestPart, auth);
  }
}

#if ENABLE_BLUETOOTH
const char* bleUploadJson(bool success, bool final, const char* error = nullptr) {
  static char response[320];
  JsonDocument doc;
  doc["success"] = success;
  doc["active"] = gBleUpload.active;
  doc["member"] = gBleUpload.member;
  doc["size"] = gBleUpload.received;
  doc["expected"] = gBleUpload.expected;
  doc["final"] = final;
  doc["chunkMax"] = kBleUploadChunkMax;
  // Diagnostics. Before these, a collapsed transfer left no device-side trace at
  // all and the cause could only be guessed at from the peer.
  doc["framesOk"] = gBleUploadFramesAccepted;
  doc["framesBad"] = gBleUploadFramesRejected;
  doc["queueDrops"] = gCmdExecDropCount;
  doc["resumedFrom"] = gBleUploadResumedFrom;
  doc["resumeMs"] = gBleUploadResumeMs;
  doc["lastTeardown"] = gBleUploadLastTeardown;
  if (error && error[0]) doc["error"] = error;
  else if (gBleUpload.warning[0]) doc["warning"] = gBleUpload.warning;
  serializeJson(doc, response, sizeof(response));
  return response;
}

bool otaUploadJournalStartable(char* reason, size_t reasonSize) {
  hw1_ota_record_t record{};
  if (!loadRecord(record, nullptr, true, reason, reasonSize)) return false;
  if (hw1_ota_result_pending(&record)) {
    snprintf(reason, reasonSize,
             "Error: acknowledge the pending OTA result before uploading another image");
    return false;
  }
  if (record.phase != HW1_OTA_PHASE_IDLE &&
      record.phase != HW1_OTA_PHASE_SUCCEEDED &&
      record.phase != HW1_OTA_PHASE_FAILED &&
      record.phase != HW1_OTA_PHASE_CANCELED) {
    snprintf(reason, reasonSize,
             "Error: OTA phase %s does not allow a staging upload", phaseName(record.phase));
    return false;
  }
  return true;
}

const char* cmdOtaWrite(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (commandIsAutomation()) return "Error: otawrite is forbidden from automations";

  static char error[192];
  uint16_t connId = 0;
  if (!currentSecureBleConnection(connId, error, sizeof(error))) return error;
  if (!ensureBleUploadMutex()) return "Error: OTA Bluetooth upload mutex unavailable";

  CommandArgs args(argsInput);
  if (!args.has(0)) {
    return "Error: Usage: otawrite begin <candidate|manifest> <size> <sha256> | status | finish | abort";
  }
  const String action = args.arg(0);

  if (action == "begin") {
    if (args.count() != 4) {
      return "Error: Usage: otawrite begin <candidate|manifest> <size> <sha256>";
    }
    const String member = args.arg(1);
    const char* path = nullptr;
    uint32_t sizeLimit = 0;
    if (member == "candidate") {
      path = kCandidatePart;
      sizeLimit = kOtaSlotSize;
    } else if (member == "manifest") {
      path = kManifestPart;
      sizeLimit = kManifestEnvelopeMax;
    } else {
      return "Error: OTA upload member must be candidate or manifest";
    }

    char* parseEnd = nullptr;
    const unsigned long long parsed = strtoull(args.arg(2).c_str(), &parseEnd, 10);
    if (!parseEnd || *parseEnd != '\0' || parsed == 0 || parsed > sizeLimit ||
        parsed > UINT32_MAX) {
      return "Error: OTA upload size is invalid for this member";
    }
    uint8_t expectedDigest[32]{};
    if (!parseSha256Hex(args.arg(3), expectedDigest)) {
      return "Error: OTA upload requires an exact 64-character SHA-256 digest";
    }
    if (!otaUploadJournalStartable(error, sizeof(error))) return error;

    String owner;
    if (!bleGetAuthenticatedUser(connId, owner) || owner.length() == 0) {
      return "Error: otawrite requires a live superadmin Bluetooth session";
    }

    xSemaphoreTake(gBleUploadMutex, portMAX_DELAY);
    // Close any live session WITHOUT deleting its bytes. A dropped link used to
    // discard the partial and force a full re-upload; because the protocol is
    // offset-addressed, those bytes are resumable whenever the contract below
    // still matches. An abandoned partial under a different contract is
    // truncated a few lines down instead.
    resetBleUploadLocked(false, "superseded");
    const AuthContext auth = VFS::systemAuth(VFS::Scopes::OTA, "hwone.ota_ble_begin");
    uint32_t resumeFrom = 0;
    {
      FsLockGuard guard("ota.ble.begin");
      // A candidate starts a new pair, so never let a prior manifest.part be
      // accidentally paired with it. A manifest upload deliberately preserves
      // the candidate that just finished.
      if (member == "candidate") {
        if (VFS::existsGuarded(kManifestPart, auth)) (void)VFS::removeGuarded(kManifestPart, auth);
      } else if (!VFS::existsGuarded(kCandidatePart, auth)) {
        xSemaphoreGive(gBleUploadMutex);
        return "Error: upload and finish candidate before beginning manifest";
      }
      if (uploadMetaMatches(path, static_cast<uint32_t>(parsed), expectedDigest, auth) &&
          VFS::existsGuarded(path, auth)) {
        File probe = VFS::openGuarded(path, "r", auth);
        if (probe) {
          const size_t onFlash = probe.size();
          probe.close();
          if (onFlash < parsed) resumeFrom = static_cast<uint32_t>(onFlash);
        }
      }
    }

    mbedtls_sha256_init(&gBleUpload.sha);
    if (mbedtls_sha256_starts(&gBleUpload.sha, 0) != 0) {
      gBleUpload.shaInitialized = true;
      resetBleUploadLocked(true, "digest-init-failed");
      xSemaphoreGive(gBleUploadMutex);
      return "Error: could not initialize OTA transport digest";
    }
    gBleUpload.shaInitialized = true;

    // Rebuild the running digest over the bytes already on flash. The final
    // check at `finish` still covers the whole member, so a resume that picked
    // up mismatched bytes fails there and installs nothing.
    if (resumeFrom > 0) {
      // Cooperative, because this runs on cmd_exec_task (pinned to core 0) and
      // walks up to several megabytes. Holding FsLockGuard around the whole
      // loop blocked the main loop for seconds — starving the BLE connection
      // events that resume exists to survive — and starved IDLE0 badly enough
      // to trip the task watchdog, which this build configures to PANIC
      // (CONFIG_ESP_TASK_WDT_PANIC=y, TIMEOUT_S=5,
      // CHECK_IDLE_TASK_CPU0=y in sdkconfig.ota). So: take the filesystem lock
      // per chunk, and yield + feed the watchdog every kRehashYieldBytes.
      //
      // Dropping the FS lock between chunks is safe here. gBleUploadMutex is
      // still held for the whole begin, and /system/ota/ is userPerms=0 /
      // adminPerms=0 (System_Filesystem.cpp), so no other surface can write
      // this file underneath us.
      constexpr uint32_t kRehashYieldBytes = 64u * 1024u;
      const uint32_t rehashStartMs = millis();
      uint8_t buffer[512];
      uint32_t hashed = 0;
      uint32_t sinceYield = 0;
      File in;
      {
        FsLockGuard guard("ota.ble.rehash.open");
        in = VFS::openGuarded(path, "r", auth);
      }
      bool ok = static_cast<bool>(in);
      while (ok && hashed < resumeFrom) {
        const uint32_t remaining = resumeFrom - hashed;
        const size_t want = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        int got = 0;
        {
          FsLockGuard guard("ota.ble.rehash");
          got = in.read(buffer, want);
        }
        if (got <= 0 ||
            mbedtls_sha256_update(&gBleUpload.sha, buffer, static_cast<size_t>(got)) != 0) {
          ok = false;
          break;
        }
        hashed += static_cast<uint32_t>(got);
        sinceYield += static_cast<uint32_t>(got);
        if (sinceYield >= kRehashYieldBytes) {
          sinceYield = 0;
          if (esp_task_wdt_status(nullptr) == ESP_OK) (void)esp_task_wdt_reset();
          vTaskDelay(1);
        }
      }
      {
        FsLockGuard guard("ota.ble.rehash.close");
        if (in) in.close();
      }
      gBleUploadResumeMs = millis() - rehashStartMs;
      // Measured, not assumed: this is the real margin against the 5 s
      // idle-starvation threshold, and it is also surfaced in `otawrite status`.
      ESP_LOGI(kTag, "OTA resume re-hash: %" PRIu32 " of %" PRIu32 " bytes in %" PRIu32
                     " ms (%s)",
               hashed, resumeFrom, gBleUploadResumeMs,
               (ok && hashed == resumeFrom) ? "resuming" : "restarting");
      if (!ok || hashed != resumeFrom) resumeFrom = 0;  // fall back to a clean start
    }

    {
      FsLockGuard guard("ota.ble.open");
      gBleUpload.file = VFS::openGuarded(path, resumeFrom > 0 ? "a" : "w", auth, true);
    }
    if (!gBleUpload.file) {
      resetBleUploadLocked(false, "open-failed");
      xSemaphoreGive(gBleUploadMutex);
      return "Error: could not create the OTA staging member";
    }
    if (resumeFrom == 0) {
      // Restarting: re-seed the digest (the resume attempt may have fed it) and
      // record the contract these bytes are being written under.
      mbedtls_sha256_free(&gBleUpload.sha);
      mbedtls_sha256_init(&gBleUpload.sha);
      (void)mbedtls_sha256_starts(&gBleUpload.sha, 0);
      FsLockGuard guard("ota.ble.meta");
      writeUploadMeta(path, static_cast<uint32_t>(parsed), expectedDigest, auth);
    }

    gBleUpload.active = true;
    gBleUpload.connId = connId;
    snprintf(gBleUpload.member, sizeof(gBleUpload.member), "%s", member.c_str());
    snprintf(gBleUpload.authUser, sizeof(gBleUpload.authUser), "%s", owner.c_str());
    gBleUpload.path = path;
    gBleUpload.expected = static_cast<uint32_t>(parsed);
    gBleUpload.received = resumeFrom;
    gBleUpload.resumedFrom = resumeFrom;
    gBleUploadResumedFrom = resumeFrom;
    gBleUpload.lastActivityMs = millis();
    memcpy(gBleUpload.expectedDigest, expectedDigest, sizeof(expectedDigest));
    {
      char detail[40];
      snprintf(detail, sizeof(detail), "%lu bytes%s",
               (unsigned long)gBleUpload.expected,
               resumeFrom > 0 ? " (resumed)" : "");
      systemEventPost(SYSEVT_OTA_UPLOAD_STARTED, gBleUpload.member, detail);
    }
    const char* result = bleUploadJson(true, false);
    xSemaphoreGive(gBleUploadMutex);
    return result;
  }

  xSemaphoreTake(gBleUploadMutex, portMAX_DELAY);
  if (!gBleUpload.active || gBleUpload.connId != connId) {
    xSemaphoreGive(gBleUploadMutex);
    return "{\"success\":false,\"active\":false,\"error\":\"no active upload for this connection\"}";
  }

  if (action == "status" && args.count() == 1) {
    gBleUpload.lastActivityMs = millis();
    const char* result = bleUploadJson(!gBleUpload.failed, false,
                                       gBleUpload.failed ? gBleUpload.warning : nullptr);
    xSemaphoreGive(gBleUploadMutex);
    return result;
  }

  if (action == "abort" && args.count() == 1) {
    // Explicit operator intent, so the partial and its contract both go — this
    // is the one teardown that must not leave anything resumable behind.
    const char* path = gBleUpload.path;
    resetBleUploadLocked(true, "aborted");
    if (path) {
      const AuthContext auth = VFS::systemAuth(VFS::Scopes::OTA, "hwone.ota_ble_abort");
      FsLockGuard guard("ota.ble.abort.meta");
      const String meta = uploadMetaPath(path);
      if (VFS::existsGuarded(meta, auth)) (void)VFS::removeGuarded(meta, auth);
    }
    xSemaphoreGive(gBleUploadMutex);
    return "{\"success\":true,\"active\":false,\"final\":false}";
  }

  if (action == "finish" && args.count() == 1) {
    if (gBleUpload.failed || gBleUpload.received != gBleUpload.expected) {
      const char* detail = gBleUpload.failed ? gBleUpload.warning : "upload length is incomplete";
      const char* result = bleUploadJson(false, false, detail);
      xSemaphoreGive(gBleUploadMutex);
      return result;
    }
    uint8_t digest[32]{};
    const int digestResult = mbedtls_sha256_finish(&gBleUpload.sha, digest);
    mbedtls_sha256_free(&gBleUpload.sha);
    gBleUpload.shaInitialized = false;
    {
      FsLockGuard guard("ota.ble.finish");
      gBleUpload.file.flush();
      gBleUpload.file.close();
    }
    if (digestResult != 0 ||
        memcmp(digest, gBleUpload.expectedDigest, sizeof(digest)) != 0) {
      memset(digest, 0, sizeof(digest));
      systemEventPost(SYSEVT_OTA_UPLOAD_FINISHED,
                      gBleUpload.member[0] ? gBleUpload.member : "member",
                      "transport SHA-256 mismatch");
      const char* result = bleUploadJson(false, false, "transport SHA-256 mismatch");
      resetBleUploadLocked(true);
      xSemaphoreGive(gBleUploadMutex);
      return result;
    }
    memset(digest, 0, sizeof(digest));
    // The member is complete, so its resume contract has done its job. Drop the
    // sidecar rather than leaving it to be matched against some later upload.
    if (gBleUpload.path) {
      const AuthContext auth = VFS::systemAuth(VFS::Scopes::OTA, "hwone.ota_ble_finish");
      FsLockGuard guard("ota.ble.finish.meta");
      const String meta = uploadMetaPath(gBleUpload.path);
      if (VFS::existsGuarded(meta, auth)) (void)VFS::removeGuarded(meta, auth);
    }
    gBleUpload.active = false;
    {
      char bytes[24];
      snprintf(bytes, sizeof(bytes), "%lu bytes", (unsigned long)gBleUpload.expected);
      systemEventPost(SYSEVT_OTA_UPLOAD_FINISHED,
                      gBleUpload.member[0] ? gBleUpload.member : "member", bytes);
    }
    const char* result = bleUploadJson(true, true);
    xSemaphoreGive(gBleUploadMutex);
    return result;
  }

  xSemaphoreGive(gBleUploadMutex);
  return "Error: Usage: otawrite begin <candidate|manifest> <size> <sha256> | status | finish | abort";
}

bool handleBleUploadFrame(uint16_t connId, const uint8_t* frame, size_t size) {
  static const uint8_t prefix[] = {0x00, 'H', 'W', '1', 'O', 'T', 'A', 0x01};
  if (!frame || size < kBleUploadHeaderSize + 1 ||
      memcmp(frame, prefix, sizeof(prefix)) != 0) return false;
  if (!gBleUploadMutex) return true;
  xSemaphoreTake(gBleUploadMutex, portMAX_DELAY);

  // Authorization on the hot path is a RAM-only check: the connection must
  // still be authenticated as the same operator that passed the full superadmin
  // gate at `begin`. Calling isSuperAdminUser() here instead re-read and
  // re-parsed users.json from LittleFS under the global FS lock for every one
  // of ~11k frames, which is what starved the radio and dropped the link.
  String user;
  if (!gBleUpload.active || gBleUpload.connId != connId ||
      !bleGetAuthenticatedUser(connId, user) ||
      strncmp(user.c_str(), gBleUpload.authUser, sizeof(gBleUpload.authUser)) != 0) {
    gBleUploadFramesRejected++;
    xSemaphoreGive(gBleUploadMutex);
    return true;
  }
  const uint32_t offset = (static_cast<uint32_t>(frame[8]) << 24) |
                          (static_cast<uint32_t>(frame[9]) << 16) |
                          (static_cast<uint32_t>(frame[10]) << 8) |
                          static_cast<uint32_t>(frame[11]);
  const uint8_t* payload = frame + kBleUploadHeaderSize;
  const size_t payloadSize = size - kBleUploadHeaderSize;
  gBleUpload.lastActivityMs = millis();
  if (gBleUpload.failed) {
    xSemaphoreGive(gBleUploadMutex);
    return true;
  }
  if (payloadSize == 0 || payloadSize > kBleUploadChunkMax ||
      offset != gBleUpload.received || payloadSize > gBleUpload.expected - gBleUpload.received) {
    gBleUploadFramesRejected++;
    snprintf(gBleUpload.warning, sizeof(gBleUpload.warning),
             "offset mismatch: device has %" PRIu32 ", frame requested %" PRIu32,
             gBleUpload.received, offset);
    xSemaphoreGive(gBleUploadMutex);
    return true;
  }

  size_t written = 0;
  {
    FsLockGuard guard("ota.ble.chunk");
    written = gBleUpload.file.write(payload, payloadSize);
  }
  if (written != payloadSize ||
      mbedtls_sha256_update(&gBleUpload.sha, payload, payloadSize) != 0) {
    gBleUpload.failed = true;
    snprintf(gBleUpload.warning, sizeof(gBleUpload.warning),
             "OTA staging write failed at offset %" PRIu32, gBleUpload.received);
    xSemaphoreGive(gBleUploadMutex);
    return true;
  }
  gBleUpload.received += static_cast<uint32_t>(payloadSize);
  gBleUploadFramesAccepted++;
  gBleUpload.warning[0] = '\0';
  xSemaphoreGive(gBleUploadMutex);
  return true;
}

void housekeepBleUpload() {
  if (!gBleUploadMutex || xSemaphoreTake(gBleUploadMutex, 0) != pdTRUE) return;
  if (gBleUpload.active) {
    // Runs on every main-loop lap (~83/s). It used to call isSuperAdminUser()
    // here, so an open upload meant reading and parsing users.json off flash 83
    // times a second from this task while the BLE task wrote chunks under the
    // same lock — the likeliest source of the 200-266 ms IO stalls observed
    // during transfers. The RAM-only identity check below is equivalent for
    // liveness: privilege is established at `begin` and re-checked at `finish`.
    String user;
    const bool sessionAlive =
        bleGetAuthenticatedUser(gBleUpload.connId, user) &&
        strncmp(user.c_str(), gBleUpload.authUser, sizeof(gBleUpload.authUser)) == 0;
    const bool idle = millis() - gBleUpload.lastActivityMs > kBleUploadIdleTimeoutMs;
    if (!sessionAlive || idle) {
      // Keep the partial: it is resumable, and naming which guard fired is the
      // difference between diagnosing a dropped link and guessing at one.
      resetBleUploadLocked(false, !sessionAlive ? "session-lost" : "idle-timeout");
    }
  }
  xSemaphoreGive(gBleUploadMutex);
}
#endif  // ENABLE_BLUETOOTH

#if !ENABLE_BLUETOOTH
const char* cmdOtaWrite(const String&) {
  return "Error: otawrite requires Bluetooth support in this firmware build";
}
#endif

void postResultEvent(const hw1_ota_record_t& record) {
  char detail[SYSEVT_DETAIL_LEN];
  snprintf(detail, sizeof(detail), "%s seq=%" PRIu32 " %.48s",
           resultName(record.last_result.code), record.last_result.sequence,
           record.last_result.detail);
  systemEventPost(SYSEVT_OTA_RESULT, phaseName(record.phase), detail);
}

bool bootRecoveryForRecord(hw1_ota_record_t& record, char* reason,
                           size_t reasonSize) {
  const esp_partition_t* factory = nullptr;
  if (!verifyFactoryUpdater(&factory, nullptr, 0, reason, reasonSize)) return false;
  if (!transitionAndCommit(record, HW1_OTA_EVENT_ARM_RECOVERY_BOOT, nullptr,
                           reason, reasonSize)) {
    return false;
  }
  const esp_err_t err = esp_ota_set_boot_partition(factory);
  if (err == ESP_OK) return true;

  hw1_ota_transition_args_t failure{
      .verified_manifest = nullptr,
      .result_code = HW1_OTA_RESULT_BOOT_SWITCH_ERROR,
      .native_error = static_cast<int32_t>(err),
      .detail = "could not select factory recovery image",
  };
  char ignored[96];
  (void)transitionAndCommit(record, HW1_OTA_EVENT_FAIL, &failure,
                            ignored, sizeof(ignored));
  snprintf(reason, reasonSize, "Error: could not select factory recovery image: %s",
           esp_err_to_name(err));
  return false;
}

bool resumeRecoveryBootIntent(hw1_ota_record_t& record, char* reason,
                              size_t reasonSize) {
  // A direct-recovery request can lose power after hw1_ota_begin() but before
  // ARM_RECOVERY_BOOT is committed. A staged-file REQUESTED record is only a
  // validated stage and must still wait for an explicit `otaupdate confirm`.
  if (record.phase == HW1_OTA_PHASE_REQUESTED &&
      record.source == HW1_OTA_SOURCE_RECOVERY_UPLOAD) {
    return bootRecoveryForRecord(record, reason, reasonSize);
  }
  if (record.phase != HW1_OTA_PHASE_RECOVERY_BOOT_ARMED) return false;

  const esp_partition_t* factory = nullptr;
  if (!verifyFactoryUpdater(&factory, nullptr, 0, reason, reasonSize)) {
    hw1_ota_transition_args_t failure{
        .verified_manifest = nullptr,
        .result_code = HW1_OTA_RESULT_BOOT_SWITCH_ERROR,
        .native_error = 0,
        .detail = "armed recovery image is unavailable on intent replay",
    };
    char ignored[96];
    (void)transitionAndCommit(record, HW1_OTA_EVENT_FAIL, &failure,
                              ignored, sizeof(ignored));
    return false;
  }
  const esp_err_t err = esp_ota_set_boot_partition(factory);
  if (err == ESP_OK) return true;

  hw1_ota_transition_args_t failure{
      .verified_manifest = nullptr,
      .result_code = HW1_OTA_RESULT_BOOT_SWITCH_ERROR,
      .native_error = static_cast<int32_t>(err),
      .detail = "recovery boot intent replay could not select factory",
  };
  char ignored[96];
  (void)transitionAndCommit(record, HW1_OTA_EVENT_FAIL, &failure,
                            ignored, sizeof(ignored));
  snprintf(reason, reasonSize, "Error: could not replay recovery boot intent: %s",
           esp_err_to_name(err));
  return false;
}

bool parseConfirmedOptions(const String& input, const char* allowedOption,
                           bool& optionPresent) {
  String args = input;
  args.trim();
  optionPresent = false;
  if (args == "confirm") return true;
  const String expected = String("confirm ") + allowedOption;
  if (allowedOption && args == expected) {
    optionPresent = true;
    return true;
  }
  return false;
}

const char* cmdOtaStatus(const String& argsInput) {
  String args = argsInput;
  args.trim();
  if (args.length() && !args.equalsIgnoreCase("json")) {
    return "Error: Usage: otastatus [json]";
  }

  static char output[1536];
  hw1_ota_record_t record{};
  hw1_ota_nvs_info_t info{};
  char journalReason[128]{};
  const bool journalOk = loadRecord(record, &info, true,
                                    journalReason, sizeof(journalReason));
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t imageState = ESP_OTA_IMG_UNDEFINED;
  if (running) (void)esp_ota_get_state_partition(running, &imageState);
  const AuthContext auth = VFS::systemAuth(VFS::Scopes::OTA, "hwone.ota_status");
  bool candidate = false;
  bool manifest = false;
  {
    FsLockGuard guard("ota.status");
    candidate = VFS::existsGuarded(kCandidatePath, auth);
    manifest = VFS::existsGuarded(kManifestPath, auth);
  }

  if (args.equalsIgnoreCase("json")) {
    JsonDocument doc;
    doc["v"] = 1;
    doc["available"] = true;
    doc["board"] = HW1_OTA_BOARD_ID;
    doc["layout"] = HW1_OTA_LAYOUT_ID;
    doc["runningPartition"] = running ? running->label : "unknown";
    doc["runningImageState"] = static_cast<int>(imageState);
    doc["journalOk"] = journalOk;
    doc["journalSequence"] = record.sequence;
    doc["phase"] = journalOk ? phaseName(record.phase) : "unavailable";
    doc["operationId"] = record.operation_id;
    doc["candidateStaged"] = candidate && manifest;
    doc["recoveryCredentialConfigured"] = credentialConfigured();
    doc["resultCode"] = resultName(record.last_result.code);
    doc["resultSequence"] = record.last_result.sequence;
    doc["resultDetail"] = record.last_result.detail;
    doc["resultPending"] = hw1_ota_result_pending(&record);
    if (!journalOk) doc["journalError"] = journalReason;
    {
      // Why the LAST trial image was rolled back. Survives the rollback and the
      // trip through recovery, so this is often the only surviving account of a
      // failure whose image no longer exists to be inspected.
      OtaProbationCause cause = OTA_PROBATION_NONE;
      uint32_t abortUptimeMs = 0;
      char abortDetail[64]{};
      if (otaSafetyTakeProbationAbort(&cause, &abortUptimeMs, abortDetail,
                                      sizeof(abortDetail), false)) {
        doc["lastProbationAbort"] = otaSafetyProbationCauseName(cause);
        doc["lastProbationAbortDetail"] = abortDetail;
        doc["lastProbationAbortUptimeMs"] = abortUptimeMs;
      }
    }
    serializeJson(doc, output, sizeof(output));
    return output;
  }

  snprintf(output, sizeof(output),
           "OTA: native ESP-IDF recovery updater\n"
           "Board/layout: %s / %s\n"
           "Running: %s (image state %d)\n"
           "Journal: %s, seq=%" PRIu32 ", phase=%s, operation=%" PRIu64 "\n"
           "Staged pair: %s; recovery credential: %s\n"
           "Last result: %s seq=%" PRIu32 " %s%s%s",
           HW1_OTA_BOARD_ID, HW1_OTA_LAYOUT_ID,
           running ? running->label : "unknown", static_cast<int>(imageState),
           journalOk ? "OK" : journalReason, record.sequence,
           journalOk ? phaseName(record.phase) : "unavailable", record.operation_id,
           (candidate && manifest) ? "yes" : "no",
           credentialConfigured() ? "configured" : "NOT configured",
           resultName(record.last_result.code), record.last_result.sequence,
           record.last_result.detail,
           (candidate != manifest) ? "\nWarning: incomplete staged pair" : "",
           hw1_ota_result_pending(&record) ? "\nResult has not yet been acknowledged" : "");
  return output;
}

const char* cmdOtaPin(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (commandIsAutomation()) return "Error: otapin is forbidden from automations";
  // Physical-presence gate, matching cmdOtaResetJournal below.
  //
  // This closes a backwards asymmetry: otaresetjournal - which only rewrites two
  // recoverable journal keys - was serial-only, while otapin, which decides
  // whether recovery can be reached at all, was reachable from WEB, BLUETOOTH,
  // MQTT, ESPNOW, G2_HIJACK, LOCAL_DISPLAY and VOICE on nothing but
  // requiresSuperAdmin. Since start_network() refuses to raise the SoftAP with
  // no credential (updater/main/updater_main.c), one hijacked super-admin
  // session anywhere could run `otapin clear confirm` and take recovery offline
  // from the other side of the planet, leaving the physical console - which is
  // deliberately UNGATED while no credential exists - as the only way back in.
  // Setting a NEW value is equally sensitive: it hands the attacker the WPA2 PSK
  // and HTTP token for the next recovery boot.
  //
  // Cable-only costs nothing operationally. The credential is provisioned once
  // during cable bring-up, and every remote step that matters (otawrite,
  // otastage, otaupdate) still works over Bluetooth afterwards - only CHANGING
  // the credential now needs someone at the device. It also leaves the
  // documented lockout escape intact: the recovery console's `cancel` boots the
  // main app on the same USB connection the operator is already holding.
  const auto* context = static_cast<CommandContext*>(currentCommandContext());
  if (!context || context->origin != ORIGIN_SERIAL) {
    return "Error: otapin is available only on the physical serial console";
  }
  String passphrase = argsInput;
  passphrase.trim();
  static char error[160];
  if (passphrase.equalsIgnoreCase("clear confirm")) {
    if (!clearCredential(error, sizeof(error))) return error;
    systemEventPost(SYSEVT_OTA_CREDENTIAL_CHANGED, "cleared",
                    currentAuthContext().user.c_str());
    return "Recovery credential cleared. Recovery mode will remain offline until otapin is set again.";
  }
  if (passphrase.length() < kMinimumRecoveryPassphrase ||
      passphrase.length() > kMaximumRecoveryPassphrase) {
    secureClearString(passphrase);
    return "Error: Usage: otapin <12..63 printable characters> | otapin clear confirm";
  }
  for (size_t i = 0; i < passphrase.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(passphrase[i]);
    if (c < 0x20 || c > 0x7e) {
      secureClearString(passphrase);
      return "Error: recovery credential must contain printable ASCII only";
    }
  }
  const bool stored = storeCredential(passphrase, error, sizeof(error));
  secureClearString(passphrase);
  if (!stored) return error;
  // subject distinguishes set from cleared, which the command audit log cannot:
  // the redaction rule renders `otapin <secret>` and `otapin clear confirm`
  // identically as `otapin ***`.
  systemEventPost(SYSEVT_OTA_CREDENTIAL_CHANGED, "set",
                  currentAuthContext().user.c_str());
  return "Persistent recovery credential set for WPA2 and HTTP authentication. It is never shown by status.";
}

const char* cmdOtaStage(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (commandIsAutomation()) return "Error: otastage is forbidden from automations";
  bool allowDowngrade = false;
  if (!parseConfirmedOptions(argsInput, "allow-downgrade", allowDowngrade)) {
    return "Error: Usage: otastage confirm [allow-downgrade]";
  }

  static char response[240];
  hw1_ota_record_t record{};
  if (!loadRecord(record, nullptr, true, response, sizeof(response))) return response;
  if (hw1_ota_result_pending(&record)) {
    return "Error: review otastatus and run otaack <result-sequence> confirm before starting another OTA";
  }
  if (record.phase != HW1_OTA_PHASE_IDLE &&
      record.phase != HW1_OTA_PHASE_SUCCEEDED &&
      record.phase != HW1_OTA_PHASE_FAILED &&
      record.phase != HW1_OTA_PHASE_CANCELED) {
    return "Error: another OTA operation is active; inspect it with otastatus";
  }
  const AuthContext auth = VFS::systemAuth(VFS::Scopes::OTA, "hwone.ota_stage");
  FsLockGuard guard("ota.stage");
  if (!VFS::existsGuarded(kCandidatePart, auth) ||
      !VFS::existsGuarded(kManifestPart, auth)) {
    return "Error: upload candidate.part and manifest.part to /system/ota first";
  }

  CandidateInfo candidate{};
  if (!validateCandidate(kCandidatePart, kManifestPart, allowDowngrade,
                         candidate, response, sizeof(response))) {
    // The signature/contract refusal reason is the whole value here - a
    // rejected candidate is either an operator mistake or someone feeding the
    // device an image it should not take.
    systemEventPost(SYSEVT_OTA_STAGE_REJECTED,
                    candidate.verified.manifest.version[0]
                        ? candidate.verified.manifest.version
                        : "unknown",
                    response);
    return response;
  }

  if (VFS::existsGuarded(kCandidatePath, auth) &&
      !VFS::removeGuarded(kCandidatePath, auth)) {
    return "Error: could not replace prior staged candidate";
  }
  if (VFS::existsGuarded(kManifestPath, auth) &&
      !VFS::removeGuarded(kManifestPath, auth)) {
    return "Error: could not replace prior staged manifest";
  }
  if (!VFS::renameGuarded(kCandidatePart, kCandidatePath, auth) ||
      !VFS::renameGuarded(kManifestPart, kManifestPath, auth)) {
    return "Error: could not promote the validated staged pair; re-upload both .part files";
  }

  CandidateInfo promoted{};
  if (!validateCandidate(kCandidatePath, kManifestPath, allowDowngrade,
                         promoted, response, sizeof(response)) ||
      memcmp(promoted.digest, candidate.digest, sizeof(candidate.digest)) != 0) {
    return response[0] ? response : "Error: promoted candidate changed during staging";
  }

  uint64_t operationId = (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
  if (operationId == 0) operationId = 1;
  const uint16_t flags = allowDowngrade ? HW1_OTA_REQUEST_ALLOW_DOWNGRADE : 0;
  const hw1_ota_status_t begin = hw1_ota_begin(
      &record, operationId, HW1_OTA_SOURCE_STAGED_FILE, flags, &promoted.verified);
  if (begin != HW1_OTA_OK) {
    snprintf(response, sizeof(response),
             "Error: another OTA operation is active (state error %d)", (int)begin);
    return response;
  }
  if (!commitRecord(record, response, sizeof(response))) return response;

  // Only mention the credential when it is actually missing. This line used to
  // say "after setting otapin" unconditionally, which told operators of an
  // already-provisioned device to do work they had done months earlier - and
  // now that otapin is serial-only, it would send a remote operator to a
  // command their transport refuses.
  systemEventPost(SYSEVT_OTA_STAGED, promoted.verified.manifest.version,
                  allowDowngrade ? "allow-downgrade" : "normal");
  snprintf(response, sizeof(response),
           "Staged and journaled %s (%" PRIu32 " bytes). %s",
           promoted.verified.manifest.version, promoted.size,
           credentialConfigured()
               ? "Run otaupdate confirm."
               : "Set the recovery credential with otapin on the serial console, "
                 "then run otaupdate confirm.");
  return response;
}

const char* cmdOtaUpdate(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (commandIsAutomation()) return "Error: otaupdate is forbidden from automations";
  bool forcePower = false;
  if (!parseConfirmedOptions(argsInput, "force-power", forcePower)) {
    return "Error: Usage: otaupdate confirm [force-power]";
  }
  if (!credentialConfigured()) {
    return "Error: set a recovery credential first - run otapin <12..63 characters> on the serial console";
  }

  static char response[240];
  hw1_ota_record_t record{};
  if (!loadRecord(record, nullptr, false, response, sizeof(response))) return response;
  if (record.phase != HW1_OTA_PHASE_REQUESTED || !record.candidate_present ||
      record.source != HW1_OTA_SOURCE_STAGED_FILE) {
    return "Error: no journaled staged update; run otastage confirm first";
  }

  CandidateInfo candidate{};
  {
    FsLockGuard guard("ota.update.revalidate");
    if (!validateCandidate(kCandidatePath, kManifestPath,
                           (record.request_flags & HW1_OTA_REQUEST_ALLOW_DOWNGRADE) != 0,
                           candidate, response, sizeof(response)) ||
        !hw1_ota_record_candidate_matches_verified(&record,
                                                    &candidate.verified)) {
      hw1_ota_transition_args_t failure{
          .verified_manifest = nullptr,
          .result_code = HW1_OTA_RESULT_INCOMPATIBLE_IMAGE,
          .native_error = 0,
          .detail = "staged candidate failed launch-time revalidation",
      };
      char ignored[96];
      (void)transitionAndCommit(record, HW1_OTA_EVENT_FAIL, &failure,
                                ignored, sizeof(ignored));
      return response[0] ? response : "Error: staged candidate no longer matches journal";
    }
  }

  if (!powerIsSafe(forcePower, response, sizeof(response))) return response;
  if (forcePower) {
    record.request_flags |= HW1_OTA_REQUEST_FORCED_POWER_OVERRIDE;
    if (!commitRecord(record, response, sizeof(response))) return response;
  }
  if (!bootRecoveryForRecord(record, response, sizeof(response))) return response;

  systemEventPost(SYSEVT_OTA_RESULT, "recovery_armed", candidate.verified.manifest.version);
  systemEventPost(SYSEVT_OTA_RECOVERY_ENTERED, "operator",
                  candidate.verified.manifest.version);
  rebootDevice("ota", "signed OTA update armed; rebooting to factory recovery", 750);
  return "Rebooting to recovery updater";
}

const char* cmdOtaRecovery(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (commandIsAutomation()) return "Error: otarecovery is forbidden from automations";
  bool allowDowngrade = false;
  if (!parseConfirmedOptions(argsInput, "allow-downgrade", allowDowngrade)) {
    return "Error: Usage: otarecovery confirm [allow-downgrade]";
  }
  if (!credentialConfigured()) {
    return "Error: set a recovery credential first - run otapin <12..63 characters> on the serial console";
  }

  static char response[220];
  hw1_ota_record_t record{};
  if (!loadRecord(record, nullptr, true, response, sizeof(response))) return response;
  if (hw1_ota_result_pending(&record)) {
    return "Error: review otastatus and run otaack <result-sequence> confirm before starting another OTA";
  }
  uint64_t operationId = (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
  if (operationId == 0) operationId = 1;
  const uint16_t flags = allowDowngrade ? HW1_OTA_REQUEST_ALLOW_DOWNGRADE : 0;
  const hw1_ota_status_t begin = hw1_ota_begin(
      &record, operationId, HW1_OTA_SOURCE_RECOVERY_UPLOAD, flags, nullptr);
  if (begin != HW1_OTA_OK) {
    snprintf(response, sizeof(response),
             "Error: another OTA operation is active (state error %d)", (int)begin);
    return response;
  }
  if (!commitRecord(record, response, sizeof(response)) ||
      !bootRecoveryForRecord(record, response, sizeof(response))) {
    return response;
  }

  systemEventPost(SYSEVT_OTA_RESULT, "recovery_armed", "direct upload requested");
  systemEventPost(SYSEVT_OTA_RECOVERY_ENTERED, "operator", "direct upload");
  rebootDevice("ota-recovery", "operator requested authenticated OTA recovery", 750);
  return "Rebooting to recovery updater";
}

const char* cmdOtaCancel(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (commandIsAutomation()) return "Error: otacancel is forbidden from automations";
  String args = argsInput;
  args.trim();
  if (args != "confirm") return "Error: Usage: otacancel confirm";

  static char response[192];
  hw1_ota_record_t record{};
  if (!loadRecord(record, nullptr, false, response, sizeof(response))) return response;
  if (record.phase != HW1_OTA_PHASE_REQUESTED) {
    return "Error: only a staged/requested OTA can be canceled safely";
  }
  hw1_ota_transition_args_t canceled{
      .verified_manifest = nullptr,
      .result_code = HW1_OTA_RESULT_CANCELED,
      .native_error = 0,
      .detail = "operator canceled before recovery boot was armed",
  };
  if (!transitionAndCommit(record, HW1_OTA_EVENT_CANCEL, &canceled,
                           response, sizeof(response))) {
    return response;
  }
  removeAllOtaFiles();
  postResultEvent(record);
  return "Canceled the staged OTA request; upload a new .part pair when ready";
}

const char* cmdOtaResetJournal(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (commandIsAutomation()) return "Error: otaresetjournal is forbidden from automations";
  const auto* context = static_cast<CommandContext*>(currentCommandContext());
  if (!context || context->origin != ORIGIN_SERIAL) {
    return "Error: otaresetjournal is available only on the physical serial console";
  }
  String args = argsInput;
  args.trim();
  if (args != "confirm") return "Error: Usage: otaresetjournal confirm";

  static char response[192];
  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(HW1_OTA_NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err == ESP_OK) {
    const esp_err_t a = nvs_erase_key(handle, HW1_OTA_NVS_SLOT_A_KEY);
    const esp_err_t b = nvs_erase_key(handle, HW1_OTA_NVS_SLOT_B_KEY);
    if (a != ESP_OK && a != ESP_ERR_NVS_NOT_FOUND) err = a;
    if (err == ESP_OK && b != ESP_OK && b != ESP_ERR_NVS_NOT_FOUND) err = b;
  }
  if (err == ESP_OK) err = nvs_commit(handle);
  if (handle) nvs_close(handle);
  if (err != ESP_OK) {
    snprintf(response, sizeof(response),
             "Error: could not reset OTA journal keys: %s", esp_err_to_name(err));
    return response;
  }
  systemEventPost(SYSEVT_OTA_RESULT, "journal_reset",
                  "physical serial reset of hw1up tx_a/tx_b only");
  return "OTA journal reset; recovery credential and all other NVS data were preserved";
}

const char* cmdOtaAcknowledge(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (commandIsAutomation()) return "Error: otaack is forbidden from automations";
  String args = argsInput;
  args.trim();
  const int separator = args.indexOf(' ');
  if (separator <= 0 || args.substring(separator + 1) != "confirm") {
    return "Error: Usage: otaack <result-sequence> confirm";
  }
  const String sequenceText = args.substring(0, separator);
  char* parseEnd = nullptr;
  const uint64_t parsedSequence = strtoull(sequenceText.c_str(), &parseEnd, 10);
  if (parsedSequence == 0 || parsedSequence > UINT32_MAX ||
      !parseEnd || *parseEnd != '\0') {
    return "Error: result sequence must be a nonzero decimal uint32";
  }
  const uint32_t sequence = static_cast<uint32_t>(parsedSequence);

  static char response[192];
  hw1_ota_record_t record{};
  if (!loadRecord(record, nullptr, false, response, sizeof(response))) return response;
  if (!hw1_ota_result_pending(&record)) return "No pending OTA result to acknowledge";
  if (record.last_result.sequence != sequence) {
    snprintf(response, sizeof(response),
             "Error: pending OTA result is sequence %" PRIu32
             ", not %" PRIu32 "; rerun otastatus",
             record.last_result.sequence, sequence);
    return response;
  }
  const hw1_ota_status_t status = hw1_ota_acknowledge_result(&record, sequence);
  if (status != HW1_OTA_OK) {
    snprintf(response, sizeof(response),
             "Error: OTA result acknowledgement failed (%d)", (int)status);
    return response;
  }
  if (!commitRecord(record, response, sizeof(response))) return response;
  snprintf(response, sizeof(response), "Acknowledged OTA result sequence %" PRIu32,
           sequence);
  return response;
}

#else  // !HW1_OTA_LAYOUT

const char* unavailable(const String&) {
  return "Error: OTA recovery is unavailable in this build. Rebuild with HW_OTA_LAYOUT=1 on a supported 16 MB FeatherS3.";
}

const char* unavailableStatus(const String&) {
  return "OTA recovery is unavailable in this build. Rebuild with HW_OTA_LAYOUT=1 on a supported 16 MB FeatherS3.";
}

#endif  // HW1_OTA_LAYOUT

}  // namespace

bool otaBleHandleEncryptedFrame(uint16_t connId, const uint8_t* frame, size_t size) {
#if HW1_OTA_LAYOUT && ENABLE_BLUETOOTH
  return handleBleUploadFrame(connId, frame, size);
#else
  (void)connId;
  (void)frame;
  (void)size;
  return false;
#endif
}

void otaBleUploadHousekeeping() {
#if HW1_OTA_LAYOUT && ENABLE_BLUETOOTH
  housekeepBleUpload();
#endif
}

void otaSystemCrashLoopEscapeEarly() {
#if HW1_OTA_LAYOUT
  if (crashRecordConsecutive() < kCrashLoopEscapeCount ||
      crashRecordPrevPhase() == CRASH_PHASE_UNKNOWN ||
      !crashRecordPreviousBuildMatches()) {
    return;
  }
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running || running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0) return;

  hw1_ota_record_t record{};
  char reason[128]{};
  if (!loadRecord(record, nullptr, true, reason, sizeof(reason))) return;
  const esp_partition_t* factory = nullptr;
  if (!verifyFactoryUpdater(&factory, nullptr, 0, reason, sizeof(reason))) return;

  // Replay the two durable intent points here, not only after filesystem init:
  // this function exists specifically for images that may crash before setup
  // reaches otaSystemInitAfterStorage().
  if (record.phase == HW1_OTA_PHASE_REQUESTED &&
      record.source == HW1_OTA_SOURCE_RECOVERY_UPLOAD) {
    if (!bootRecoveryForRecord(record, reason, sizeof(reason))) return;
    ESP_EARLY_LOGE(kTag, "Replaying interrupted crash-loop recovery request");
    esp_restart();
  }
  if (record.phase == HW1_OTA_PHASE_RECOVERY_BOOT_ARMED) {
    if (esp_ota_set_boot_partition(factory) != ESP_OK) return;
    ESP_EARLY_LOGE(kTag, "Replaying interrupted crash-loop recovery boot switch");
    esp_restart();
  }

  if (record.phase == HW1_OTA_PHASE_TRIAL_BOOT_ARMED ||
      record.phase == HW1_OTA_PHASE_TRIAL_RUNNING) {
    hw1_ota_transition_args_t rollback{
        .verified_manifest = nullptr,
        .result_code = HW1_OTA_RESULT_ROLLBACK_DETECTED,
        .native_error = 0,
        .detail = "trial image repeatedly crashed before reaching RUNNING",
    };
    if (!transitionAndCommit(record, HW1_OTA_EVENT_ROLLBACK_OBSERVED,
                             &rollback, reason, sizeof(reason))) {
      return;
    }
  } else {
    // Rollback only protects the first trial boot. A defect that appears after
    // commit (for example a later autostart path) still needs a way out of a
    // permanent crash loop, so create a direct-recovery operation and park in
    // the updater for an authenticated replacement upload.
    if (record.phase == HW1_OTA_PHASE_REQUESTED) {
      hw1_ota_transition_args_t canceled{
          .verified_manifest = nullptr,
          .result_code = HW1_OTA_RESULT_CANCELED,
          .native_error = 0,
          .detail = "unlaunched OTA request replaced by crash-loop recovery",
      };
      if (!transitionAndCommit(record, HW1_OTA_EVENT_CANCEL, &canceled,
                               reason, sizeof(reason))) {
        return;
      }
    }
    if (record.phase != HW1_OTA_PHASE_IDLE &&
        record.phase != HW1_OTA_PHASE_SUCCEEDED &&
        record.phase != HW1_OTA_PHASE_FAILED &&
        record.phase != HW1_OTA_PHASE_CANCELED) {
      return;
    }
    uint64_t operationId = (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
    if (operationId == 0) operationId = 1;
    const uint16_t emergencyFlags = hw1_ota_result_pending(&record)
                                        ? HW1_OTA_REQUEST_SUPERSEDE_PENDING_RESULT
                                        : 0;
    if (hw1_ota_begin(&record, operationId, HW1_OTA_SOURCE_RECOVERY_UPLOAD,
                      emergencyFlags, nullptr) != HW1_OTA_OK ||
        !commitRecord(record, reason, sizeof(reason)) ||
        !transitionAndCommit(record, HW1_OTA_EVENT_ARM_RECOVERY_BOOT, nullptr,
                             reason, sizeof(reason))) {
      return;
    }
  }
  if (esp_ota_set_boot_partition(factory) != ESP_OK) return;
  ESP_EARLY_LOGE(kTag, "Main-image crash loop detected; entering factory recovery");
  esp_restart();
#endif
}

bool otaSystemRecoverFromStorageFailure() {
#if HW1_OTA_LAYOUT
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running || running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0) return false;

  hw1_ota_record_t record{};
  char reason[160]{};
  if (!loadRecord(record, nullptr, true, reason, sizeof(reason))) {
    ESP_LOGE(kTag, "%s", reason);
    return false;
  }

  if (record.phase == HW1_OTA_PHASE_RECOVERY_BOOT_ARMED ||
      (record.phase == HW1_OTA_PHASE_REQUESTED &&
       record.source == HW1_OTA_SOURCE_RECOVERY_UPLOAD)) {
    if (!resumeRecoveryBootIntent(record, reason, sizeof(reason))) {
      ESP_LOGE(kTag, "%s", reason);
      return false;
    }
  } else {
    if (record.phase == HW1_OTA_PHASE_REQUESTED) {
      hw1_ota_transition_args_t canceled{
          .verified_manifest = nullptr,
          .result_code = HW1_OTA_RESULT_CANCELED,
          .native_error = 0,
          .detail = "staged request replaced because LittleFS would not mount",
      };
      if (!transitionAndCommit(record, HW1_OTA_EVENT_CANCEL, &canceled,
                               reason, sizeof(reason))) {
        ESP_LOGE(kTag, "%s", reason);
        return false;
      }
    }
    if (record.phase != HW1_OTA_PHASE_IDLE &&
        record.phase != HW1_OTA_PHASE_SUCCEEDED &&
        record.phase != HW1_OTA_PHASE_FAILED &&
        record.phase != HW1_OTA_PHASE_CANCELED) {
      ESP_LOGE(kTag, "Error: storage failed during active OTA phase %s", phaseName(record.phase));
      return false;
    }
    uint64_t operationId = (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
    if (operationId == 0) operationId = 1;
    const uint16_t emergencyFlags = hw1_ota_result_pending(&record)
                                        ? HW1_OTA_REQUEST_SUPERSEDE_PENDING_RESULT
                                        : 0;
    if (hw1_ota_begin(&record, operationId, HW1_OTA_SOURCE_RECOVERY_UPLOAD,
                      emergencyFlags, nullptr) != HW1_OTA_OK ||
        !commitRecord(record, reason, sizeof(reason)) ||
        !bootRecoveryForRecord(record, reason, sizeof(reason))) {
      ESP_LOGE(kTag, "%s", reason[0] ? reason : "Error: could not arm recovery");
      return false;
    }
  }

  ESP_LOGE(kTag, "LittleFS unavailable; entering factory recovery without formatting");
  esp_restart();
  return true;
#else
  return false;
#endif
}

void otaSystemInitAfterStorage() {
#if HW1_OTA_LAYOUT
  // Report a previous probation abort now that the filesystem is up. This is
  // the first point where the reason can reach a durable, operator-readable
  // sink; before this it existed only on a UART nobody was attached to, and the
  // rolled-back image it describes is already gone.
  {
    OtaProbationCause abortCause = OTA_PROBATION_NONE;
    uint32_t abortUptimeMs = 0;
    char abortDetail[64]{};
    // consume=false: v0.99.81 shipped this as consume=true, which zeroed the RTC
    // slot during boot init - so `otastatus`, which reads it non-destructively,
    // always found it empty and the release note's claim that a rollback cause
    // "appears in otastatus" was never true on this path. The breadcrumb is now
    // left in place; the next abort overwrites it, and otaSafetyMarkProbation-
    // AbortReported() keeps this from re-logging the same rollback every boot.
    if (otaSafetyTakeProbationAbort(&abortCause, &abortUptimeMs, abortDetail,
                                    sizeof(abortDetail), false) &&
        !otaSafetyProbationAbortReported()) {
      char event[SYSEVT_DETAIL_LEN];
      snprintf(event, sizeof(event), "rollback: %s after %lums (%.40s)",
               otaSafetyProbationCauseName(abortCause),
               (unsigned long)abortUptimeMs, abortDetail);
      ESP_LOGE(kTag, "Previous trial image was rolled back - %s", event);
      logSystemEvent("OTA", event);
      char rolledDetail[SYSEVT_DETAIL_LEN];
      snprintf(rolledDetail, sizeof(rolledDetail), "%lums: %.40s",
               (unsigned long)abortUptimeMs, abortDetail);
      systemEventPost(SYSEVT_OTA_ROLLED_BACK,
                      otaSafetyProbationCauseName(abortCause), rolledDetail);
      otaSafetyMarkProbationAbortReported();
    }
  }

  hw1_ota_record_t record{};
  char reason[160]{};
  if (!loadRecord(record, nullptr, true, reason, sizeof(reason))) {
    ESP_LOGE(kTag, "%s", reason);
    return;
  }
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t imageState = ESP_OTA_IMG_UNDEFINED;
  if (running) (void)esp_ota_get_state_partition(running, &imageState);

  if (running && running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
      ((record.phase == HW1_OTA_PHASE_REQUESTED &&
        record.source == HW1_OTA_SOURCE_RECOVERY_UPLOAD) ||
       record.phase == HW1_OTA_PHASE_RECOVERY_BOOT_ARMED)) {
    if (resumeRecoveryBootIntent(record, reason, sizeof(reason))) {
      ESP_LOGW(kTag, "Replaying durable recovery-boot intent after interrupted switch");
      rebootDevice("ota-resume", "replaying signed OTA recovery boot intent", 250);
      return;
    }
    ESP_LOGE(kTag, "%s", reason[0] ? reason : "recovery boot intent replay failed");
  }

  if (running && running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
      (imageState == ESP_OTA_IMG_NEW || imageState == ESP_OTA_IMG_PENDING_VERIFY) &&
      (record.phase == HW1_OTA_PHASE_TRIAL_BOOT_ARMED ||
       record.phase == HW1_OTA_PHASE_IMAGE_VERIFIED)) {
    if (!transitionAndCommit(record, HW1_OTA_EVENT_TRIAL_STARTED, nullptr,
                             reason, sizeof(reason))) {
      ESP_LOGE(kTag, "%s", reason);
    } else {
      systemEventPost(SYSEVT_OTA_TRIAL_STARTED, SelfDevice::firmwareVersion(),
                      "unverified image on probation");
    }
  } else if (running && running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
             imageState == ESP_OTA_IMG_VALID &&
             record.phase == HW1_OTA_PHASE_TRIAL_RUNNING) {
    hw1_ota_transition_args_t success{
        .verified_manifest = nullptr,
        .result_code = HW1_OTA_RESULT_SUCCESS,
        .native_error = 0,
        .detail = "trial image was already marked valid",
    };
    if (transitionAndCommit(record, HW1_OTA_EVENT_MARK_VALID, &success,
                            reason, sizeof(reason))) {
      removeStagedFiles();
    } else {
      ESP_LOGE(kTag, "%s", reason);
    }
  }

  if (hw1_ota_result_pending(&record)) {
    postResultEvent(record);
  }
#endif
}

bool otaSystemCanMarkImageValid() {
#if HW1_OTA_LAYOUT
  hw1_ota_record_t record{};
  char reason[128]{};
  if (!loadRecord(record, nullptr, false, reason, sizeof(reason))) {
    ESP_LOGE(kTag, "Refusing to validate image without trial journal: %s",
             reason[0] ? reason : "journal unavailable");
    return false;
  }
  const bool authenticatedTrial =
      record.phase == HW1_OTA_PHASE_TRIAL_RUNNING && record.candidate_present;
  // Selecting an already accepted ota_0 from the factory updater necessarily
  // creates a fresh NEW/PENDING_VERIFY entry.  Let that existing image earn a
  // new health probation after a successful/cancelled recovery visit, or after
  // the updater explicitly returns a still-valid old main from a failed
  // transaction. A normal signed OTA still requires the exact freshly
  // authenticated candidate journal above.
  const bool recoveryReturn = record.phase == HW1_OTA_PHASE_SUCCEEDED ||
                              record.phase == HW1_OTA_PHASE_CANCELED ||
                              record.phase == HW1_OTA_PHASE_FAILED;
  if (!authenticatedTrial && !recoveryReturn) {
    ESP_LOGE(kTag, "Refusing to validate image in incompatible OTA phase %s",
             phaseName(record.phase));
    return false;
  }
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_app_desc_t description{};
  if (!running || running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
      esp_ota_get_partition_description(running, &description) != ESP_OK) {
    ESP_LOGE(kTag, "Refusing to validate image outside a readable ota_0 partition");
    return false;
  }
  char project[sizeof(description.project_name) + 1]{};
  char version[sizeof(description.version) + 1]{};
  if (!fieldToString(description.project_name, sizeof(description.project_name),
                     project, sizeof(project)) ||
      !fieldToString(description.version, sizeof(description.version), version,
                     sizeof(version))) {
    ESP_LOGE(kTag, "Refusing to validate image with an unreadable app identity");
    return false;
  }
  if (authenticatedTrial &&
      (strcmp(project, record.candidate.manifest.project_name) != 0 ||
       strcmp(version, record.candidate.manifest.version) != 0)) {
    ESP_LOGE(kTag, "Refusing to validate image that does not match the trial journal");
    return false;
  }
  // A canceled/failed transaction can legitimately return to the previously
  // installed main image while its journal still describes the candidate
  // that was abandoned.  SUCCEEDED, by contrast, must return to that exact
  // authenticated candidate.
  const bool succeededCandidateMismatch =
      record.phase == HW1_OTA_PHASE_SUCCEEDED && record.candidate_present &&
      (strcmp(project, record.candidate.manifest.project_name) != 0 ||
       strcmp(version, record.candidate.manifest.version) != 0);
  if (recoveryReturn &&
      (strcmp(project, "hardwareone-idf") != 0 ||
       !String(version).endsWith(kRequiredVersionSuffix) ||
       succeededCandidateMismatch)) {
    ESP_LOGE(kTag, "Refusing incompatible recovery-return image");
    return false;
  }
  return true;
#else
  return true;
#endif
}

void otaSystemOnImageMarkedValid(bool provisioningShortcut) {
#if !HW1_OTA_LAYOUT
  (void)provisioningShortcut;  // no OTA journal in this build
#else
  hw1_ota_record_t record{};
  char reason[160]{};
  if (!loadRecord(record, nullptr, false, reason, sizeof(reason))) {
    ESP_LOGE(kTag, "Image valid but OTA journal is not in trial_running: %s", reason);
    return;
  }
  if (record.phase == HW1_OTA_PHASE_SUCCEEDED ||
      record.phase == HW1_OTA_PHASE_CANCELED ||
      record.phase == HW1_OTA_PHASE_FAILED) {
    ESP_LOGI(kTag, "Existing main image passed recovery-return probation");
    if (hw1_ota_result_pending(&record)) postResultEvent(record);
    return;
  }
  if (record.phase != HW1_OTA_PHASE_TRIAL_RUNNING) {
    ESP_LOGE(kTag, "Image valid but OTA journal is in incompatible phase %s",
             phaseName(record.phase));
    return;
  }
  hw1_ota_transition_args_t success{
      .verified_manifest = nullptr,
      .result_code = HW1_OTA_RESULT_SUCCESS,
      .native_error = 0,
      .detail = "healthy 60-second probation completed",
  };
  if (!transitionAndCommit(record, HW1_OTA_EVENT_MARK_VALID, &success,
                           reason, sizeof(reason))) {
    ESP_LOGE(kTag, "%s", reason);
    return;
  }
  removeStagedFiles();
  postResultEvent(record);
  systemEventPost(SYSEVT_OTA_ACCEPTED, SelfDevice::firmwareVersion(),
                  provisioningShortcut ? "provisioning" : "probation");
#endif
}

#if HW1_OTA_LAYOUT
const CommandEntry otaCommands[] = {
    {"otastatus", "Show signed recovery OTA state. (add 'json')", false,
     cmdOtaStatus, "Usage: otastatus [json]"},
    {"otapin", "Set the persistent recovery WPA2/HTTP credential (serial console only).", true,
     cmdOtaPin, "Usage: otapin <12..63 printable characters> | otapin clear confirm", true},
    {"otawrite", "Stage exact OTA members over encrypted Bluetooth.", true,
     cmdOtaWrite,
     "Usage: otawrite begin <candidate|manifest> <size> <sha256> | status | finish | abort", true},
    {"otastage", "Validate and journal uploaded candidate.part + manifest.part.", true,
     cmdOtaStage, "Usage: otastage confirm [allow-downgrade]", true},
    {"otaupdate", "Revalidate staged firmware and reboot into recovery apply.", true,
     cmdOtaUpdate, "Usage: otaupdate confirm [force-power]", true},
    {"otarecovery", "Reboot into authenticated recovery for direct upload.", true,
     cmdOtaRecovery, "Usage: otarecovery confirm [allow-downgrade]", true},
    {"otacancel", "Cancel a staged request before recovery boot is armed.", true,
     cmdOtaCancel, "Usage: otacancel confirm", true},
    {"otaack", "Acknowledge the durable OTA result after reviewing it.", true,
     cmdOtaAcknowledge, "Usage: otaack <result-sequence> confirm", true},
    {"otaresetjournal", "Serial-only repair of the two OTA transaction keys.", true,
     cmdOtaResetJournal, "Usage: otaresetjournal confirm", true},
};
#else
const CommandEntry otaCommands[] = {
    {"otastatus", "Explain OTA availability for this build.", false,
     unavailableStatus, "Usage: otastatus"},
    {"otapin", "Unavailable without the opt-in OTA layout.", true,
     unavailable, nullptr, true},
    {"otawrite", "Unavailable without the opt-in OTA layout.", true,
     unavailable, nullptr, true},
    {"otastage", "Unavailable without the opt-in OTA layout.", true,
     unavailable, nullptr, true},
    {"otaupdate", "Unavailable without the opt-in OTA layout.", true,
     unavailable, nullptr, true},
    {"otarecovery", "Unavailable without the opt-in OTA layout.", true,
     unavailable, nullptr, true},
    {"otacancel", "Unavailable without the opt-in OTA layout.", true,
     unavailable, nullptr, true},
    {"otaack", "Unavailable without the opt-in OTA layout.", true,
     unavailable, nullptr, true},
    {"otaresetjournal", "Unavailable without the opt-in OTA layout.", true,
     unavailable, nullptr, true},
};
#endif

const size_t otaCommandsCount = sizeof(otaCommands) / sizeof(otaCommands[0]);
