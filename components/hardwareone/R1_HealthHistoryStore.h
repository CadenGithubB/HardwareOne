#ifndef R1_HEALTH_HISTORY_STORE_H
#define R1_HEALTH_HISTORY_STORE_H

#include "System_BuildConfig.h"

#include <stddef.h>
#include <stdint.h>

// Clean v1 daily-history model. This is intentionally separate from the live
// point-sample rings in G2_Health: hourly aggregates and ten-minute activity
// buckets have different timestamps and replacement semantics.
static constexpr uint32_t R1_HEALTH_HISTORY_SCHEMA_VERSION = 1;
static constexpr size_t R1_HEALTH_HOURLY_SLOTS = 24;
static constexpr size_t R1_HEALTH_ACTIVITY_SLOTS = 144;
static constexpr uint16_t R1_HEALTH_HISTORY_RETENTION_DAYS = 30;
static constexpr uint16_t R1_HEALTH_HISTORY_MAX_FILES = 96;
static constexpr size_t R1_HEALTH_HISTORY_MAX_FILE_BYTES = 65536;
static constexpr int16_t R1_HEALTH_TIMEZONE_MINUTES_MIN = -720;  // UTC-12
static constexpr int16_t R1_HEALTH_TIMEZONE_MINUTES_MAX = 840;   // UTC+14

// Persisted wire-layout family, deliberately independent of exact runtime
// firmware identity. ID 1 is already on disk and must never be renumbered;
// raw ID 2 is not a valid stored layout even though runtime profile 2 exists.
enum R1HealthHistoryLayout : uint8_t {
  R1_HISTORY_LAYOUT_UNKNOWN = 0,
  R1_HISTORY_LAYOUT_DAILY_V1 = 1,
};

enum R1HealthHistoryMetric : uint8_t {
  R1_HISTORY_HEART_RATE = 1,
  R1_HISTORY_HRV = 2,
  R1_HISTORY_SPO2 = 3,
};

enum R1HealthHistoryFetchState : uint8_t {
  R1_HISTORY_FETCH_IDLE = 0,
  R1_HISTORY_FETCHING,
  R1_HISTORY_COMPLETE,
  R1_HISTORY_PARTIAL,
  R1_HISTORY_ERROR,
  R1_HISTORY_UNSUPPORTED,
};

enum R1HealthSleepState : uint8_t {
  R1_HISTORY_SLEEP_UNKNOWN = 0,
  R1_HISTORY_SLEEP_EMPTY,
  R1_HISTORY_SLEEP_PRESENT,
};

enum R1HealthHistoryStoreError : uint8_t {
  R1_HISTORY_STORE_OK = 0,
  R1_HISTORY_STORE_NOT_READY,
  R1_HISTORY_STORE_BAD_IDENTITY,
  R1_HISTORY_STORE_BAD_DAY,
  R1_HISTORY_STORE_BAD_BUCKET,
  R1_HISTORY_STORE_NOT_TERMINAL,
  R1_HISTORY_STORE_KEY_UNAVAILABLE,
  R1_HISTORY_STORE_OPEN_FAILED,
  R1_HISTORY_STORE_WRITE_FAILED,
  R1_HISTORY_STORE_TOO_LARGE,
  R1_HISTORY_STORE_RENAME_FAILED,
  R1_HISTORY_STORE_READ_FAILED,
  R1_HISTORY_STORE_NOT_FOUND,
  R1_HISTORY_STORE_SCHEMA_MISMATCH,
  R1_HISTORY_STORE_DECRYPT_FAILED,
};

struct R1HealthHourlyBucket {
  bool valid;
  uint8_t hourSlot;
  uint16_t average;
  uint16_t minimum;
  uint16_t maximum;
  uint32_t bucketEpoch;
};

struct R1HealthHourlyMetricDay {
  bool have;
  bool latestValid;
  uint8_t count;
  uint16_t sourceSerial;
  uint32_t sourceCrc32;
  uint32_t latestTimestamp;
  uint16_t latestValue;
  R1HealthHourlyBucket slots[R1_HEALTH_HOURLY_SLOTS];
};

struct R1HealthActivityBucket {
  bool valid;
  uint8_t tenMinuteSlot;
  uint16_t steps;
  uint16_t activeKcal;
  uint16_t restingKcal;
  uint16_t totalKcal;
  uint32_t bucketEpoch;
};

struct R1HealthActivityDay {
  bool have;
  // True once an activity-daily message has been parsed and CRC-validated in
  // full (single notification or reassembled fragments). This is "message
  // verified", NOT "all 144 slots present": the ring answers with a sparse
  // delta — slots that have data and have not yet been acknowledged — so a
  // verified day is routinely far short of 144 slots. Coverage is `count`.
  bool fullDayVerified;
  uint16_t count;
  uint16_t sourceSerial;
  uint32_t sourceCrc32;
  R1HealthActivityBucket slots[R1_HEALTH_ACTIVITY_SLOTS];
};

struct R1HealthHistoryDay {
  uint32_t schemaVersion;
  char peerId[18];                 // canonical ring MAC; never emitted by status JSON
  // Historical field name retained so schema/hash/source users stay stable.
  // The stored value is R1HealthHistoryLayout, not an exact runtime profile.
  uint8_t protocolProfile;
  uint32_t dayStart;
  int16_t timezoneMinutes;
  R1HealthHourlyMetricDay heartRate;
  R1HealthHourlyMetricDay hrv;
  R1HealthHourlyMetricDay spo2;
  R1HealthActivityDay activity;
  R1HealthSleepState sleepState;
  R1HealthHistoryFetchState fetchState;
  uint32_t lastSuccessEpoch;
  uint32_t lastPartialEpoch;
  uint8_t fetchError;
};

struct R1HealthActivitySummary {
  bool available;
  bool fullDayVerified;
  uint16_t bucketCount;
  uint32_t steps;
  uint32_t activeKcal;
  uint32_t restingKcal;
  uint32_t totalKcal;
  uint32_t firstBucketEpoch;
  uint32_t lastBucketEpoch;
};

struct R1HealthHistoryStoreStatus {
  bool initialized;
  bool available;
  bool commitPending;
  bool encryptionRequired;
  bool lastCommitEncrypted;
  bool lastCommitSkipped;
  uint32_t lastCommitEpoch;
  uint32_t lastContentHash;
  // Source-model generations make persistence acknowledgement explicit.
  // A caller must not clear its dirty generation until committedGeneration
  // reaches that exact value.
  uint32_t stagedGeneration;
  uint32_t committedGeneration;
  R1HealthHistoryStoreError error;
};

static_assert(R1_HEALTH_HOURLY_SLOTS == 24, "R1 history hourly schema is fixed");
static_assert(R1_HEALTH_ACTIVITY_SLOTS == 144, "R1 activity day must retain all ten-minute slots");
static_assert(R1_HISTORY_LAYOUT_DAILY_V1 == 1,
              "persisted R1 daily-v1 layout ID must stay 1");

#if ENABLE_R1_HEALTH

const char* r1HealthHistoryFetchStateName(R1HealthHistoryFetchState state);
const char* r1HealthHistoryStoreErrorName(R1HealthHistoryStoreError error);

void r1HealthHistoryDayClear(R1HealthHistoryDay& day);
bool r1HealthHistoryDayValidate(const R1HealthHistoryDay& day,
                                R1HealthHistoryStoreError* error = nullptr);
void r1HealthHistoryActivitySummary(const R1HealthHistoryDay& day,
                                    R1HealthActivitySummary& out);

// Coordinator/main-task staging. It copies the fixed-capacity RAM day behind
// a normal mutex; do not call it from the ring notify callback. Notify-side
// ingestion only updates the G2 model and marks a tiny dirty/generation flag.
bool r1HealthHistoryStoreStage(const R1HealthHistoryDay& day,
                               uint32_t sourceGeneration);

// Normal/main-task work. Recovers interrupted .bak swaps on first call, then
// commits the latest staged day at most once per content hash. A health/all
// capture-encryption policy fails closed if the capture key is unavailable.
void r1HealthHistoryStoreTick(void);

// Normal-task read path for restoring one exact ring day into RAM.
R1HealthHistoryStoreError r1HealthHistoryStoreLoadExact(
    const char* peerId, uint32_t dayStart, int16_t timezoneMinutes,
    R1HealthHistoryDay& out);

void r1HealthHistoryStoreGetStatus(R1HealthHistoryStoreStatus& out);

#else

inline const char* r1HealthHistoryFetchStateName(R1HealthHistoryFetchState) { return "disabled"; }
inline const char* r1HealthHistoryStoreErrorName(R1HealthHistoryStoreError) { return "disabled"; }
inline void r1HealthHistoryDayClear(R1HealthHistoryDay& day) {
  day = R1HealthHistoryDay{};
  day.schemaVersion = R1_HEALTH_HISTORY_SCHEMA_VERSION;
}
inline bool r1HealthHistoryDayValidate(const R1HealthHistoryDay&,
                                       R1HealthHistoryStoreError* error = nullptr) {
  if (error) *error = R1_HISTORY_STORE_NOT_READY;
  return false;
}
inline void r1HealthHistoryActivitySummary(const R1HealthHistoryDay&,
                                           R1HealthActivitySummary& out) {
  out = R1HealthActivitySummary{};
}
inline bool r1HealthHistoryStoreStage(const R1HealthHistoryDay&, uint32_t) { return false; }
inline void r1HealthHistoryStoreTick(void) {}
inline R1HealthHistoryStoreError r1HealthHistoryStoreLoadExact(
    const char*, uint32_t, int16_t, R1HealthHistoryDay&) {
  return R1_HISTORY_STORE_NOT_READY;
}
inline void r1HealthHistoryStoreGetStatus(R1HealthHistoryStoreStatus& out) {
  out = R1HealthHistoryStoreStatus{};
  out.error = R1_HISTORY_STORE_NOT_READY;
}

#endif  // ENABLE_R1_HEALTH
#endif  // R1_HEALTH_HISTORY_STORE_H
