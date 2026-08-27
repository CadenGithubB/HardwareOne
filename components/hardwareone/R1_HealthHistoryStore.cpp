#include "R1_HealthHistoryStore.h"

#if ENABLE_R1_HEALTH

#include <Arduino.h>
#include <FS.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "System_CaptureCrypto.h"
#include "System_Settings.h"
#include "System_VFS.h"

namespace {

constexpr const char* kHistoryDir = "/logging_captures/r1-health-history";
constexpr const char* kSchemaLine = "#HW1R1H v1";
constexpr const char* kPeerPseudonymDomain = "r1-history-peer-v1";
constexpr uint32_t kRetryInitialMs = 2000;
constexpr uint32_t kRetryMaxMs = 60000;

StaticSemaphore_t sStoreMutexStorage;
SemaphoreHandle_t sStoreMutex = xSemaphoreCreateMutexStatic(&sStoreMutexStorage);
StaticSemaphore_t sStoreIoMutexStorage;
SemaphoreHandle_t sStoreIoMutex = xSemaphoreCreateMutexStatic(&sStoreIoMutexStorage);
EXT_RAM_BSS_ATTR R1HealthHistoryDay sStagedDay;
uint32_t sStagedGeneration = 0;
bool sCommitPending = false;
uint32_t sNextRetryMs = 0;
uint32_t sRetryDelayMs = kRetryInitialMs;
R1HealthHistoryStoreStatus sStatus = {};

// Store work runs from the normal tick, never from the BLE notify callback.
// Large fixed buffers live in PSRAM, not scarce internal DRAM or task stacks.
EXT_RAM_BSS_ATTR char sLineBuf[CAPCRYPT_MAX_SEALED];
EXT_RAM_BSS_ATTR char sCryptoBuf[CAPCRYPT_MAX_SEALED];
EXT_RAM_BSS_ATTR R1HealthHistoryDay sCommitSnapshot;
EXT_RAM_BSS_ATTR R1HealthHistoryDay sExistingScratch;

SemaphoreHandle_t storeMutex() {
  return sStoreMutex;
}

class StoreLock {
 public:
  StoreLock() : locked_(xSemaphoreTake(storeMutex(), portMAX_DELAY) == pdTRUE) {}
  ~StoreLock() { if (locked_) xSemaphoreGive(storeMutex()); }
  bool locked() const { return locked_; }
 private:
  bool locked_;
};

class StoreIoLock {
 public:
  StoreIoLock() : locked_(xSemaphoreTake(sStoreIoMutex, portMAX_DELAY) == pdTRUE) {}
  ~StoreIoLock() { if (locked_) xSemaphoreGive(sStoreIoMutex); }
  bool locked() const { return locked_; }
 private:
  bool locked_;
};

uint32_t fnvBytes(uint32_t h, const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

template <typename T>
uint32_t fnvValue(uint32_t h, const T& value) {
  return fnvBytes(h, &value, sizeof(value));
}

uint32_t contentHash(const R1HealthHistoryDay& day) {
  uint32_t h = 2166136261u;
  h = fnvValue(h, day.schemaVersion);
  h = fnvBytes(h, day.peerId, strlen(day.peerId));
  h = fnvValue(h, day.protocolProfile);
  h = fnvValue(h, day.dayStart);
  h = fnvValue(h, day.timezoneMinutes);

  const R1HealthHourlyMetricDay* metrics[] = {
      &day.heartRate, &day.hrv, &day.spo2,
  };
  for (const R1HealthHourlyMetricDay* metric : metrics) {
    h = fnvValue(h, metric->have);
    h = fnvValue(h, metric->latestValid);
    h = fnvValue(h, metric->count);
    h = fnvValue(h, metric->sourceSerial);
    h = fnvValue(h, metric->sourceCrc32);
    h = fnvValue(h, metric->latestTimestamp);
    h = fnvValue(h, metric->latestValue);
    for (size_t i = 0; i < R1_HEALTH_HOURLY_SLOTS; ++i) {
      const R1HealthHourlyBucket& b = metric->slots[i];
      h = fnvValue(h, b.valid);
      if (!b.valid) continue;
      h = fnvValue(h, b.hourSlot);
      h = fnvValue(h, b.average);
      h = fnvValue(h, b.minimum);
      h = fnvValue(h, b.maximum);
      h = fnvValue(h, b.bucketEpoch);
    }
  }

  h = fnvValue(h, day.activity.have);
  h = fnvValue(h, day.activity.fullDayVerified);
  h = fnvValue(h, day.activity.count);
  h = fnvValue(h, day.activity.sourceSerial);
  h = fnvValue(h, day.activity.sourceCrc32);
  for (size_t i = 0; i < R1_HEALTH_ACTIVITY_SLOTS; ++i) {
    const R1HealthActivityBucket& b = day.activity.slots[i];
    h = fnvValue(h, b.valid);
    if (!b.valid) continue;
    h = fnvValue(h, b.tenMinuteSlot);
    h = fnvValue(h, b.steps);
    h = fnvValue(h, b.activeKcal);
    h = fnvValue(h, b.restingKcal);
    h = fnvValue(h, b.totalKcal);
    h = fnvValue(h, b.bucketEpoch);
  }
  h = fnvValue(h, day.sleepState);
  h = fnvValue(h, day.fetchState);
  h = fnvValue(h, day.lastSuccessEpoch);
  h = fnvValue(h, day.lastPartialEpoch);
  h = fnvValue(h, day.fetchError);
  return h;
}

bool historyFetchTerminal(R1HealthHistoryFetchState state) {
  return state == R1_HISTORY_COMPLETE || state == R1_HISTORY_PARTIAL ||
         state == R1_HISTORY_ERROR || state == R1_HISTORY_UNSUPPORTED;
}

bool canonicalPeerId(const char* id) {
  if (!id || strlen(id) != 17) return false;
  for (size_t i = 0; i < 17; ++i) {
    if ((i % 3) == 2) {
      if (id[i] != ':') return false;
    } else if (!isxdigit(static_cast<unsigned char>(id[i]))) {
      return false;
    }
  }
  return true;
}

bool makePath(const char* peerId, uint32_t dayStart, int16_t timezoneMinutes,
              char* out, size_t cap) {
  if (!out || cap == 0 || !canonicalPeerId(peerId) || dayStart == 0) return false;
  char peerPseudonym[33];
  if (!captureCryptoPseudonym(kPeerPseudonymDomain, peerId,
                              peerPseudonym, sizeof(peerPseudonym))) return false;
  const int n = snprintf(out, cap, "%s/%s-%010lu-%+05d.r1h",
                         kHistoryDir, peerPseudonym,
                         static_cast<unsigned long>(dayStart),
                         static_cast<int>(timezoneMinutes));
  return n > 0 && static_cast<size_t>(n) < cap;
}

void setStatusError(R1HealthHistoryStoreError error) {
  StoreLock lock;
  if (!lock.locked()) return;
  sStatus.error = error;
  sStatus.available = VFS::isLittleFSReady();
}

bool ensureHistoryDir(const AuthContext& auth) {
  if (!VFS::isLittleFSReady()) return false;
  if (!VFS::existsGuarded("/logging_captures", auth) &&
      !VFS::mkdirGuarded("/logging_captures", auth)) return false;
  if (!VFS::existsGuarded(kHistoryDir, auth) &&
      !VFS::mkdirGuarded(kHistoryDir, auth)) return false;
  return true;
}

bool lineWriteRaw(File& file, const char* line, size_t& bytesWritten) {
  const size_t len = strlen(line);
  if (bytesWritten + len + 1 > R1_HEALTH_HISTORY_MAX_FILE_BYTES) return false;
  if (file.write(reinterpret_cast<const uint8_t*>(line), len) != len) return false;
  if (file.write(static_cast<uint8_t>('\n')) != 1) return false;
  bytesWritten += len + 1;
  return true;
}

bool lineWriteData(File& file, const char* line, bool sealed, size_t& bytesWritten) {
  if (!sealed) return lineWriteRaw(file, line, bytesWritten);
  const int n = captureCryptoSealLine(line, strlen(line), sCryptoBuf, sizeof(sCryptoBuf));
  if (n < 0) return false;
  return lineWriteRaw(file, sCryptoBuf, bytesWritten);
}

bool readSchemaHeader(File& file, bool& sealed) {
  sealed = false;
  file.seek(0);
  String line = file.readStringUntil('\n');
  line.trim();
  if (captureCryptoIsMagicLine(line.c_str())) {
    sealed = true;
    line = file.readStringUntil('\n');
    line.trim();
  }
  return line == kSchemaLine;
}

bool emitMetric(File& file, char tag, const R1HealthHourlyMetricDay& metric,
                bool sealed, size_t& bytesWritten) {
  snprintf(sLineBuf, sizeof(sLineBuf), "M,%c,%u,%u,%u,%u,%08lx,%lu,%u",
           tag, metric.have ? 1u : 0u, metric.latestValid ? 1u : 0u,
           static_cast<unsigned>(metric.count),
           static_cast<unsigned>(metric.sourceSerial),
           static_cast<unsigned long>(metric.sourceCrc32),
           static_cast<unsigned long>(metric.latestTimestamp),
           static_cast<unsigned>(metric.latestValue));
  if (!lineWriteData(file, sLineBuf, sealed, bytesWritten)) return false;
  for (size_t i = 0; i < R1_HEALTH_HOURLY_SLOTS; ++i) {
    const R1HealthHourlyBucket& b = metric.slots[i];
    if (!b.valid) continue;
    snprintf(sLineBuf, sizeof(sLineBuf), "H,%c,%u,%lu,%u,%u,%u", tag,
             static_cast<unsigned>(b.hourSlot),
             static_cast<unsigned long>(b.bucketEpoch),
             static_cast<unsigned>(b.average),
             static_cast<unsigned>(b.minimum),
             static_cast<unsigned>(b.maximum));
    if (!lineWriteData(file, sLineBuf, sealed, bytesWritten)) return false;
  }
  return true;
}

void recoverInterruptedSwaps(const AuthContext& auth) {
  if (!VFS::existsGuarded(kHistoryDir, auth)) return;
  File dir = VFS::openGuarded(kHistoryDir, "r", auth);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
  File entry = dir.openNextFile();
  while (entry) {
    String path = entry.name();
    entry.close();
    if (!path.startsWith("/")) path = String(kHistoryDir) + "/" + path;
    if (path.endsWith(".bak")) {
      const String primary = path.substring(0, path.length() - 4);
      if (VFS::existsGuarded(primary, auth)) {
        VFS::removeGuarded(path, auth);
      } else {
        VFS::renameGuarded(path, primary, auth);
      }
    } else if (path.endsWith(".tmp")) {
      // A temp is never authoritative: either the primary or its backup is.
      VFS::removeGuarded(path, auth);
    }
    entry = dir.openNextFile();
  }
  dir.close();
}

bool parseDayFromName(const char* name, uint32_t& dayStart) {
  if (!name) return false;
  const char* base = strrchr(name, '/');
  base = base ? base + 1 : name;
  char peerPseudonym[33] = {};
  unsigned long day = 0;
  int tz = 0;
  if (sscanf(base, "%32[0-9a-f]-%10lu-%5d.r1h",
             peerPseudonym, &day, &tz) != 3 || strlen(peerPseudonym) != 32)
    return false;
  (void)tz;
  dayStart = static_cast<uint32_t>(day);
  return dayStart != 0;
}

void enforceRetention(const AuthContext& auth) {
  const time_t now = time(nullptr);
  const bool haveClock = now >= 1609459200;
  const uint32_t cutoff = haveClock && now > static_cast<time_t>(R1_HEALTH_HISTORY_RETENTION_DAYS) * 86400
      ? static_cast<uint32_t>(now - static_cast<time_t>(R1_HEALTH_HISTORY_RETENTION_DAYS) * 86400)
      : 0;

  // Bounded global file budget. If over budget, each pass removes the oldest
  // primary; no large directory list or heap-backed sort is needed.
  for (;;) {
    uint16_t count = 0;
    uint32_t oldestDay = UINT32_MAX;
    String oldestPath;
    File dir = VFS::openGuarded(kHistoryDir, "r", auth);
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
    File entry = dir.openNextFile();
    while (entry) {
      String path = entry.name();
      const bool regular = !entry.isDirectory() && path.endsWith(".r1h");
      entry.close();
      if (regular) {
        if (!path.startsWith("/")) path = String(kHistoryDir) + "/" + path;
        uint32_t day = 0;
        if (parseDayFromName(path.c_str(), day)) {
          if (cutoff != 0 && day < cutoff) {
            VFS::removeGuarded(path, auth);
          } else {
            ++count;
            if (day < oldestDay) { oldestDay = day; oldestPath = path; }
          }
        }
      }
      entry = dir.openNextFile();
    }
    dir.close();
    if (count <= R1_HEALTH_HISTORY_MAX_FILES || oldestPath.length() == 0) break;
    if (!VFS::removeGuarded(oldestPath, auth)) break;
  }
}

R1HealthHistoryStoreError loadDayAtPath(
    const char* path, const AuthContext& auth, const char* peerId,
    uint32_t dayStart, int16_t timezoneMinutes, int8_t expectedSealed,
    R1HealthHistoryDay& out);

bool existingMatches(const String& path, const AuthContext& auth,
                     const R1HealthHistoryDay& day, bool sealed) {
  if (!VFS::existsGuarded(path, auth)) return false;
  const R1HealthHistoryStoreError error = loadDayAtPath(
      path.c_str(), auth, day.peerId, day.dayStart, day.timezoneMinutes,
      sealed ? 1 : 0, sExistingScratch);
  // The manifest hash is an integrity check, not an equality shortcut. The
  // semantic object must also match, so a collision can only cause a rewrite.
  return error == R1_HISTORY_STORE_OK &&
         contentHash(sExistingScratch) == contentHash(day) &&
         memcmp(&sExistingScratch, &day, sizeof(day)) == 0;
}

R1HealthHistoryStoreError commitDay(const R1HealthHistoryDay& day,
                                    bool& skipped, bool& encrypted,
                                    uint32_t& committedHash) {
  skipped = false;
  // Fail closed for corrupt/future values: only exact Off permits plaintext.
  encrypted = gSettings.captureEncryptMode != 0;
  committedHash = contentHash(day);

  R1HealthHistoryStoreError validation = R1_HISTORY_STORE_OK;
  if (!r1HealthHistoryDayValidate(day, &validation)) return validation;
  // The filename always uses a keyed local peer pseudonym, even when payload
  // sealing is disabled, so an unkeyed/reversible MAC hash never reaches disk.
  if (!captureCryptoEnsureKey()) return R1_HISTORY_STORE_KEY_UNAVAILABLE;

  // trusted: internal health-history persistence, confined to capture tree.
  const AuthContext auth = VFS::systemAuth(VFS::Scopes::CAPTURES,
                                           "R1 history commit");
  if (!ensureHistoryDir(auth)) return R1_HISTORY_STORE_NOT_READY;
  recoverInterruptedSwaps(auth);

  char primaryBuf[160];
  if (!makePath(day.peerId, day.dayStart, day.timezoneMinutes,
                primaryBuf, sizeof(primaryBuf))) return R1_HISTORY_STORE_BAD_DAY;
  const String primary(primaryBuf);
  if (existingMatches(primary, auth, day, encrypted)) {
    skipped = true;
    return R1_HISTORY_STORE_OK;
  }

  const String temp = primary + ".tmp";
  const String backup = primary + ".bak";
  VFS::removeGuarded(temp, auth);

  File file = VFS::openGuarded(temp, "w", auth, true);
  if (!file) return R1_HISTORY_STORE_OPEN_FAILED;
  size_t bytesWritten = 0;
  bool ok = true;
  if (encrypted) ok = lineWriteRaw(file, CAPCRYPT_MAGIC_LINE, bytesWritten);
  if (ok) ok = lineWriteRaw(file, kSchemaLine, bytesWritten);
  if (ok) {
    // In sealed mode equality/integrity metadata is AEAD-protected too; the
    // plaintext prefix contains only the generic encryption mark and schema.
    snprintf(sLineBuf, sizeof(sLineBuf), "I,%08lx",
             static_cast<unsigned long>(committedHash));
    ok = lineWriteData(file, sLineBuf, encrypted, bytesWritten);
  }
  if (ok) {
    snprintf(sLineBuf, sizeof(sLineBuf),
             "D,%s,%u,%lu,%d,%u,%u,%lu,%lu,%u",
             day.peerId, static_cast<unsigned>(day.protocolProfile),
             static_cast<unsigned long>(day.dayStart),
             static_cast<int>(day.timezoneMinutes),
             static_cast<unsigned>(day.sleepState),
             static_cast<unsigned>(day.fetchState),
             static_cast<unsigned long>(day.lastSuccessEpoch),
             static_cast<unsigned long>(day.lastPartialEpoch),
             static_cast<unsigned>(day.fetchError));
    ok = lineWriteData(file, sLineBuf, encrypted, bytesWritten);
  }
  if (ok) ok = emitMetric(file, 'H', day.heartRate, encrypted, bytesWritten);
  if (ok) ok = emitMetric(file, 'V', day.hrv, encrypted, bytesWritten);
  if (ok) ok = emitMetric(file, 'O', day.spo2, encrypted, bytesWritten);
  if (ok) {
    snprintf(sLineBuf, sizeof(sLineBuf), "AM,%u,%u,%u,%u,%08lx",
             day.activity.have ? 1u : 0u,
             day.activity.fullDayVerified ? 1u : 0u,
             static_cast<unsigned>(day.activity.count),
             static_cast<unsigned>(day.activity.sourceSerial),
             static_cast<unsigned long>(day.activity.sourceCrc32));
    ok = lineWriteData(file, sLineBuf, encrypted, bytesWritten);
  }
  for (size_t i = 0; ok && i < R1_HEALTH_ACTIVITY_SLOTS; ++i) {
    const R1HealthActivityBucket& b = day.activity.slots[i];
    if (!b.valid) continue;
    snprintf(sLineBuf, sizeof(sLineBuf), "A,%u,%lu,%u,%u,%u,%u",
             static_cast<unsigned>(b.tenMinuteSlot),
             static_cast<unsigned long>(b.bucketEpoch),
             static_cast<unsigned>(b.steps),
             static_cast<unsigned>(b.activeKcal),
             static_cast<unsigned>(b.restingKcal),
             static_cast<unsigned>(b.totalKcal));
    ok = lineWriteData(file, sLineBuf, encrypted, bytesWritten);
  }
  file.flush();
  file.close();
  if (!ok) {
    VFS::removeGuarded(temp, auth);
    return bytesWritten >= R1_HEALTH_HISTORY_MAX_FILE_BYTES
        ? R1_HISTORY_STORE_TOO_LARGE : R1_HISTORY_STORE_WRITE_FAILED;
  }

  // Power-loss-safe publish for filesystems with differing overwrite rules:
  // durable temp -> old primary to backup -> temp to primary -> cleanup backup.
  const bool hadPrimary = VFS::existsGuarded(primary, auth);
  if (hadPrimary) {
    VFS::removeGuarded(backup, auth);
    if (!VFS::renameGuarded(primary, backup, auth)) {
      VFS::removeGuarded(temp, auth);
      return R1_HISTORY_STORE_RENAME_FAILED;
    }
  }
  if (!VFS::renameGuarded(temp, primary, auth)) {
    if (hadPrimary && !VFS::existsGuarded(primary, auth)) {
      VFS::renameGuarded(backup, primary, auth);
    }
    VFS::removeGuarded(temp, auth);
    return R1_HISTORY_STORE_RENAME_FAILED;
  }
  if (hadPrimary) VFS::removeGuarded(backup, auth);
  enforceRetention(auth);
  return R1_HISTORY_STORE_OK;
}

R1HealthHourlyMetricDay* metricForTag(R1HealthHistoryDay& day, char tag) {
  switch (tag) {
    case 'H': return &day.heartRate;
    case 'V': return &day.hrv;
    case 'O': return &day.spo2;
    default: return nullptr;
  }
}

bool decodeStoredLine(const String& line, bool sealed, const char*& decoded) {
  if (line.length() >= sizeof(sLineBuf)) return false;
  if (!sealed) {
    memcpy(sLineBuf, line.c_str(), line.length() + 1);
    decoded = sLineBuf;
    return true;
  }
  const int n = captureCryptoOpenLine(line.c_str(), line.length(),
                                      sLineBuf, sizeof(sLineBuf));
  if (n < 0) return false;
  decoded = sLineBuf;
  return true;
}

struct ParseState {
  bool sawIntegrity;
  bool sawDay;
  bool sawActivityMeta;
  uint8_t metricMetaMask;
  uint32_t storedHash;
};

uint8_t metricTagBit(char tag) {
  switch (tag) {
    case 'H': return 1u << 0;
    case 'V': return 1u << 1;
    case 'O': return 1u << 2;
    default: return 0;
  }
}

bool parseStoredLine(const char* line, R1HealthHistoryDay& day,
                     ParseState& state) {
  if (!line || !line[0]) return true;
  if (line[0] == 'I' && line[1] == ',') {
    unsigned long hash = 0;
    char tail = 0;
    if (state.sawIntegrity || strlen(line) != 10 ||
        sscanf(line, "I,%8lx%c", &hash, &tail) != 1) return false;
    state.sawIntegrity = true;
    state.storedHash = static_cast<uint32_t>(hash);
    return true;
  }
  if (line[0] == 'D' && line[1] == ',') {
    unsigned profile = 0, sleep = 0, fetch = 0, error = 0;
    unsigned long start = 0, success = 0, partial = 0;
    int tz = 0;
    char peer[18] = {};
    char tail = 0;
    if (state.sawDay ||
        sscanf(line, "D,%17[^,],%u,%lu,%d,%u,%u,%lu,%lu,%u%c",
               peer, &profile, &start, &tz, &sleep, &fetch,
               &success, &partial, &error, &tail) != 9 ||
        profile != R1_HISTORY_LAYOUT_DAILY_V1 ||
        sleep > R1_HISTORY_SLEEP_PRESENT || fetch > R1_HISTORY_UNSUPPORTED ||
        error > UINT8_MAX || tz < R1_HEALTH_TIMEZONE_MINUTES_MIN ||
        tz > R1_HEALTH_TIMEZONE_MINUTES_MAX) return false;
    state.sawDay = true;
    snprintf(day.peerId, sizeof(day.peerId), "%s", peer);
    day.protocolProfile = static_cast<uint8_t>(profile);
    day.dayStart = static_cast<uint32_t>(start);
    day.timezoneMinutes = static_cast<int16_t>(tz);
    day.sleepState = static_cast<R1HealthSleepState>(sleep);
    day.fetchState = static_cast<R1HealthHistoryFetchState>(fetch);
    day.lastSuccessEpoch = static_cast<uint32_t>(success);
    day.lastPartialEpoch = static_cast<uint32_t>(partial);
    day.fetchError = static_cast<uint8_t>(error);
    return true;
  }
  if (line[0] == 'M' && line[1] == ',') {
    char tag = 0;
    unsigned have = 0, latestValid = 0, count = 0, serial = 0, latest = 0;
    unsigned long crc = 0, latestTs = 0;
    char tail = 0;
    if (sscanf(line, "M,%c,%u,%u,%u,%u,%lx,%lu,%u%c", &tag, &have,
               &latestValid, &count, &serial, &crc, &latestTs, &latest,
               &tail) != 8 || have > 1 || latestValid > 1 ||
        count > R1_HEALTH_HOURLY_SLOTS || serial > UINT16_MAX ||
        latest > UINT16_MAX) return false;
    const uint8_t tagBit = metricTagBit(tag);
    if (tagBit == 0 || (state.metricMetaMask & tagBit) != 0) return false;
    R1HealthHourlyMetricDay* metric = metricForTag(day, tag);
    if (!metric) return false;
    state.metricMetaMask |= tagBit;
    metric->have = have != 0;
    metric->latestValid = latestValid != 0;
    metric->count = static_cast<uint8_t>(count);
    metric->sourceSerial = static_cast<uint16_t>(serial);
    metric->sourceCrc32 = static_cast<uint32_t>(crc);
    metric->latestTimestamp = static_cast<uint32_t>(latestTs);
    metric->latestValue = static_cast<uint16_t>(latest);
    return true;
  }
  if (line[0] == 'H' && line[1] == ',') {
    char tag = 0;
    unsigned slot = 0, average = 0, minimum = 0, maximum = 0;
    unsigned long epoch = 0;
    char tail = 0;
    if (sscanf(line, "H,%c,%u,%lu,%u,%u,%u%c", &tag, &slot, &epoch,
               &average, &minimum, &maximum, &tail) != 6 ||
        slot >= R1_HEALTH_HOURLY_SLOTS || average > UINT16_MAX ||
        minimum > UINT16_MAX || maximum > UINT16_MAX) return false;
    R1HealthHourlyMetricDay* metric = metricForTag(day, tag);
    if (!metric || metric->slots[slot].valid) return false;
    R1HealthHourlyBucket& b = metric->slots[slot];
    b.valid = true;
    b.hourSlot = static_cast<uint8_t>(slot);
    b.bucketEpoch = static_cast<uint32_t>(epoch);
    b.average = static_cast<uint16_t>(average);
    b.minimum = static_cast<uint16_t>(minimum);
    b.maximum = static_cast<uint16_t>(maximum);
    return true;
  }
  if (strncmp(line, "AM,", 3) == 0) {
    unsigned have = 0, full = 0, count = 0, serial = 0;
    unsigned long crc = 0;
    char tail = 0;
    if (state.sawActivityMeta ||
        sscanf(line, "AM,%u,%u,%u,%u,%lx%c", &have, &full, &count,
               &serial, &crc, &tail) != 5 || have > 1 || full > 1 ||
        count > R1_HEALTH_ACTIVITY_SLOTS || serial > UINT16_MAX) return false;
    state.sawActivityMeta = true;
    day.activity.have = have != 0;
    day.activity.fullDayVerified = full != 0;
    day.activity.count = static_cast<uint16_t>(count);
    day.activity.sourceSerial = static_cast<uint16_t>(serial);
    day.activity.sourceCrc32 = static_cast<uint32_t>(crc);
    return true;
  }
  if (line[0] == 'A' && line[1] == ',') {
    unsigned slot = 0, steps = 0, active = 0, resting = 0, total = 0;
    unsigned long epoch = 0;
    char tail = 0;
    if (sscanf(line, "A,%u,%lu,%u,%u,%u,%u%c", &slot, &epoch, &steps,
               &active, &resting, &total, &tail) != 6 ||
        slot >= R1_HEALTH_ACTIVITY_SLOTS || steps > UINT16_MAX ||
        active > UINT16_MAX || resting > UINT16_MAX || total > UINT16_MAX ||
        day.activity.slots[slot].valid) return false;
    R1HealthActivityBucket& b = day.activity.slots[slot];
    b.valid = true;
    b.tenMinuteSlot = static_cast<uint8_t>(slot);
    b.bucketEpoch = static_cast<uint32_t>(epoch);
    b.steps = static_cast<uint16_t>(steps);
    b.activeKcal = static_cast<uint16_t>(active);
    b.restingKcal = static_cast<uint16_t>(resting);
    b.totalKcal = static_cast<uint16_t>(total);
    return true;
  }
  return false;
}

R1HealthHistoryStoreError loadDayAtPath(
    const char* path, const AuthContext& auth, const char* peerId,
    uint32_t dayStart, int16_t timezoneMinutes, int8_t expectedSealed,
    R1HealthHistoryDay& out) {
  r1HealthHistoryDayClear(out);
  File file = VFS::openGuarded(path, "r", auth);
  if (!file) return R1_HISTORY_STORE_NOT_FOUND;
  if (file.size() > R1_HEALTH_HISTORY_MAX_FILE_BYTES) {
    file.close();
    return R1_HISTORY_STORE_READ_FAILED;
  }
  bool sealed = false;
  if (!readSchemaHeader(file, sealed)) {
    file.close();
    return R1_HISTORY_STORE_SCHEMA_MISMATCH;
  }
  if (expectedSealed >= 0 && sealed != (expectedSealed != 0)) {
    file.close();
    return R1_HISTORY_STORE_SCHEMA_MISMATCH;
  }
  if (sealed && !captureCryptoEnsureKey()) {
    file.close();
    return R1_HISTORY_STORE_KEY_UNAVAILABLE;
  }

  bool ok = true;
  ParseState state{};
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    const char* decoded = nullptr;
    if (!decodeStoredLine(line, sealed, decoded) ||
        !parseStoredLine(decoded, out, state)) {
      ok = false;
      break;
    }
  }
  file.close();
  if (!ok) return sealed ? R1_HISTORY_STORE_DECRYPT_FAILED
                         : R1_HISTORY_STORE_READ_FAILED;
  if (!state.sawIntegrity || !state.sawDay || !state.sawActivityMeta ||
      state.metricMetaMask != 0x07) return R1_HISTORY_STORE_SCHEMA_MISMATCH;
  if (strcmp(out.peerId, peerId) != 0 || out.dayStart != dayStart ||
      out.timezoneMinutes != timezoneMinutes)
    return R1_HISTORY_STORE_BAD_IDENTITY;
  R1HealthHistoryStoreError validation = R1_HISTORY_STORE_OK;
  if (!r1HealthHistoryDayValidate(out, &validation)) return validation;
  if (!historyFetchTerminal(out.fetchState)) return R1_HISTORY_STORE_NOT_TERMINAL;
  if (contentHash(out) != state.storedHash) return R1_HISTORY_STORE_READ_FAILED;
  return R1_HISTORY_STORE_OK;
}

}  // namespace

const char* r1HealthHistoryFetchStateName(R1HealthHistoryFetchState state) {
  switch (state) {
    case R1_HISTORY_FETCH_IDLE: return "idle";
    case R1_HISTORY_FETCHING: return "fetching";
    case R1_HISTORY_COMPLETE: return "complete";
    case R1_HISTORY_PARTIAL: return "partial";
    case R1_HISTORY_ERROR: return "error";
    case R1_HISTORY_UNSUPPORTED: return "unsupported";
    default: return "unknown";
  }
}

const char* r1HealthHistoryStoreErrorName(R1HealthHistoryStoreError error) {
  switch (error) {
    case R1_HISTORY_STORE_OK: return "none";
    case R1_HISTORY_STORE_NOT_READY: return "not_ready";
    case R1_HISTORY_STORE_BAD_IDENTITY: return "bad_identity";
    case R1_HISTORY_STORE_BAD_DAY: return "bad_day";
    case R1_HISTORY_STORE_BAD_BUCKET: return "bad_bucket";
    case R1_HISTORY_STORE_NOT_TERMINAL: return "not_terminal";
    case R1_HISTORY_STORE_KEY_UNAVAILABLE: return "key_unavailable";
    case R1_HISTORY_STORE_OPEN_FAILED: return "open_failed";
    case R1_HISTORY_STORE_WRITE_FAILED: return "write_failed";
    case R1_HISTORY_STORE_TOO_LARGE: return "too_large";
    case R1_HISTORY_STORE_RENAME_FAILED: return "rename_failed";
    case R1_HISTORY_STORE_READ_FAILED: return "read_failed";
    case R1_HISTORY_STORE_NOT_FOUND: return "not_found";
    case R1_HISTORY_STORE_SCHEMA_MISMATCH: return "schema_mismatch";
    case R1_HISTORY_STORE_DECRYPT_FAILED: return "decrypt_failed";
    default: return "unknown";
  }
}

void r1HealthHistoryDayClear(R1HealthHistoryDay& day) {
  memset(&day, 0, sizeof(day));
  day.schemaVersion = R1_HEALTH_HISTORY_SCHEMA_VERSION;
  day.sleepState = R1_HISTORY_SLEEP_UNKNOWN;
  day.fetchState = R1_HISTORY_FETCH_IDLE;
}

bool r1HealthHistoryDayValidate(const R1HealthHistoryDay& day,
                                R1HealthHistoryStoreError* error) {
  auto fail = [&](R1HealthHistoryStoreError e) {
    if (error) *error = e;
    return false;
  };
  if (day.schemaVersion != R1_HEALTH_HISTORY_SCHEMA_VERSION)
    return fail(R1_HISTORY_STORE_SCHEMA_MISMATCH);
  if (!canonicalPeerId(day.peerId)) return fail(R1_HISTORY_STORE_BAD_IDENTITY);
  if (day.dayStart == 0 ||
      day.timezoneMinutes < R1_HEALTH_TIMEZONE_MINUTES_MIN ||
      day.timezoneMinutes > R1_HEALTH_TIMEZONE_MINUTES_MAX)
    return fail(R1_HISTORY_STORE_BAD_DAY);
  if (day.protocolProfile != R1_HISTORY_LAYOUT_DAILY_V1)
    return fail(R1_HISTORY_STORE_BAD_DAY);
  if (day.sleepState > R1_HISTORY_SLEEP_PRESENT ||
      day.fetchState > R1_HISTORY_UNSUPPORTED)
    return fail(R1_HISTORY_STORE_BAD_DAY);

  const R1HealthHourlyMetricDay* metrics[] = {
      &day.heartRate, &day.hrv, &day.spo2,
  };
  for (const R1HealthHourlyMetricDay* metric : metrics) {
    uint8_t count = 0;
    for (size_t i = 0; i < R1_HEALTH_HOURLY_SLOTS; ++i) {
      const R1HealthHourlyBucket& b = metric->slots[i];
      if (!b.valid) continue;
      ++count;
      if (b.hourSlot != i || b.bucketEpoch != day.dayStart + i * 3600u ||
          b.minimum > b.average || b.average > b.maximum)
        return fail(R1_HISTORY_STORE_BAD_BUCKET);
    }
    if (count != metric->count || (metric->have != (count != 0)))
      return fail(R1_HISTORY_STORE_BAD_BUCKET);
  }

  uint16_t activityCount = 0;
  for (size_t i = 0; i < R1_HEALTH_ACTIVITY_SLOTS; ++i) {
    const R1HealthActivityBucket& b = day.activity.slots[i];
    if (!b.valid) continue;
    ++activityCount;
    if (b.tenMinuteSlot != i || b.bucketEpoch != day.dayStart + i * 600u ||
        b.totalKcal < b.activeKcal ||
        b.restingKcal != static_cast<uint16_t>(b.totalKcal - b.activeKcal))
      return fail(R1_HISTORY_STORE_BAD_BUCKET);
  }
  // fullDayVerified means "the ring's activity-daily message for this day was
  // received in full and CRC-verified"; it does NOT imply all 144 slots are
  // present. Official-app HCI captures (2026-08-03) and bench sweeps show the
  // ring answers with a sparse delta — only slots that have data and have not
  // yet been acknowledged — so a verified day is routinely far short of 144.
  if (activityCount != day.activity.count ||
      (day.activity.have != (activityCount != 0)))
    return fail(R1_HISTORY_STORE_BAD_BUCKET);
  if (day.fetchState == R1_HISTORY_COMPLETE &&
      (!day.activity.have || !day.activity.fullDayVerified))
    return fail(R1_HISTORY_STORE_BAD_DAY);
  if (error) *error = R1_HISTORY_STORE_OK;
  return true;
}

void r1HealthHistoryActivitySummary(const R1HealthHistoryDay& day,
                                    R1HealthActivitySummary& out) {
  memset(&out, 0, sizeof(out));
  out.available = day.activity.have;
  out.fullDayVerified = day.activity.fullDayVerified;
  out.bucketCount = day.activity.count;
  for (size_t i = 0; i < R1_HEALTH_ACTIVITY_SLOTS; ++i) {
    const R1HealthActivityBucket& b = day.activity.slots[i];
    if (!b.valid) continue;
    out.steps += b.steps;
    out.activeKcal += b.activeKcal;
    out.restingKcal += b.restingKcal;
    out.totalKcal += b.totalKcal;
    if (out.firstBucketEpoch == 0 || b.bucketEpoch < out.firstBucketEpoch)
      out.firstBucketEpoch = b.bucketEpoch;
    if (b.bucketEpoch > out.lastBucketEpoch) out.lastBucketEpoch = b.bucketEpoch;
  }
}

bool r1HealthHistoryStoreStage(const R1HealthHistoryDay& day,
                               uint32_t sourceGeneration) {
  R1HealthHistoryStoreError error = R1_HISTORY_STORE_OK;
  if (!r1HealthHistoryDayValidate(day, &error)) {
    setStatusError(error);
    return false;
  }
  if (sourceGeneration == 0 || !historyFetchTerminal(day.fetchState)) {
    setStatusError(R1_HISTORY_STORE_NOT_TERMINAL);
    return false;
  }
  // Coordinator/main-task only. Large day copies deliberately use a normal
  // mutex; the ring notify callback only marks the G2 model dirty.
  StoreLock lock;
  if (!lock.locked()) return false;
  if (sStagedGeneration == sourceGeneration) return true;
  memcpy(&sStagedDay, &day, sizeof(day));
  sStagedGeneration = sourceGeneration;
  sCommitPending = true;
  sNextRetryMs = 0;
  sRetryDelayMs = kRetryInitialMs;
  sStatus.commitPending = true;
  sStatus.stagedGeneration = sourceGeneration;
  return true;
}

void r1HealthHistoryStoreTick(void) {
  static bool initialized = false;
  if (!initialized) {
    // trusted: recover only the confined R1 history subtree.
    const AuthContext auth = VFS::systemAuth(VFS::Scopes::CAPTURES,
                                             "R1 history recovery");
    StoreIoLock io;
    if (io.locked() && ensureHistoryDir(auth)) {
      recoverInterruptedSwaps(auth);
      enforceRetention(auth);
      initialized = true;
    }
    {
      StoreLock lock;
      if (lock.locked()) {
        sStatus.initialized = initialized;
        sStatus.available = initialized;
        sStatus.encryptionRequired = gSettings.captureEncryptMode != 0;
        if (!initialized) sStatus.error = R1_HISTORY_STORE_NOT_READY;
      }
    }
    if (!initialized) return;
  }

  uint32_t generation = 0;
  const uint32_t nowMs = millis();
  {
    StoreLock lock;
    if (!lock.locked()) return;
    if (!sCommitPending) {
      sStatus.commitPending = false;
      return;
    }
    if (sNextRetryMs != 0 && (int32_t)(nowMs - sNextRetryMs) < 0) {
      sStatus.commitPending = true;
      return;
    }
    memcpy(&sCommitSnapshot, &sStagedDay, sizeof(sCommitSnapshot));
    generation = sStagedGeneration;
  }

  bool skipped = false, encrypted = false;
  uint32_t hash = 0;
  R1HealthHistoryStoreError error = R1_HISTORY_STORE_NOT_READY;
  {
    StoreIoLock io;
    if (io.locked()) error = commitDay(sCommitSnapshot, skipped, encrypted, hash);
  }

  {
    StoreLock lock;
    if (!lock.locked()) return;
    sStatus.error = error;
    sStatus.available = true;
    sStatus.encryptionRequired = gSettings.captureEncryptMode != 0;
    sStatus.lastCommitSkipped = skipped;
    if (error == R1_HISTORY_STORE_OK) {
      sStatus.lastCommitEncrypted = encrypted;
      sStatus.lastContentHash = hash;
      sStatus.lastCommitEpoch = static_cast<uint32_t>(time(nullptr));
      sStatus.committedGeneration = generation;
      sRetryDelayMs = kRetryInitialMs;
      sNextRetryMs = 0;
      if (sStagedGeneration == generation) sCommitPending = false;
    } else {
      // Failed generations remain retryable; do not acknowledge or discard.
      sCommitPending = true;
      sNextRetryMs = nowMs + sRetryDelayMs;
      if (sRetryDelayMs < kRetryMaxMs) {
        sRetryDelayMs *= 2;
        if (sRetryDelayMs > kRetryMaxMs) sRetryDelayMs = kRetryMaxMs;
      }
    }
    // A newer coordinator pass may have staged another generation while I/O ran.
    sStatus.commitPending = sCommitPending;
    sStatus.stagedGeneration = sStagedGeneration;
  }
}

R1HealthHistoryStoreError r1HealthHistoryStoreLoadExact(
    const char* peerId, uint32_t dayStart, int16_t timezoneMinutes,
    R1HealthHistoryDay& out) {
  r1HealthHistoryDayClear(out);
  if (!canonicalPeerId(peerId) || dayStart == 0 ||
      timezoneMinutes < R1_HEALTH_TIMEZONE_MINUTES_MIN ||
      timezoneMinutes > R1_HEALTH_TIMEZONE_MINUTES_MAX) {
    setStatusError(R1_HISTORY_STORE_BAD_IDENTITY);
    return R1_HISTORY_STORE_BAD_IDENTITY;
  }
  if (!captureCryptoEnsureKey()) {
    setStatusError(R1_HISTORY_STORE_KEY_UNAVAILABLE);
    return R1_HISTORY_STORE_KEY_UNAVAILABLE;
  }
  char pathBuf[160];
  if (!makePath(peerId, dayStart, timezoneMinutes, pathBuf, sizeof(pathBuf))) {
    setStatusError(R1_HISTORY_STORE_BAD_IDENTITY);
    return R1_HISTORY_STORE_BAD_IDENTITY;
  }
  // trusted: restore exact health-history key into the in-RAM health model.
  const AuthContext auth = VFS::systemAuth(VFS::Scopes::CAPTURES,
                                           "R1 history load");
  R1HealthHistoryStoreError loadError = R1_HISTORY_STORE_NOT_READY;
  {
    StoreIoLock io;
    if (io.locked()) {
      loadError = loadDayAtPath(pathBuf, auth, peerId, dayStart,
                                timezoneMinutes, -1, out);
    }
  }
  if (loadError != R1_HISTORY_STORE_OK) {
    if (loadError != R1_HISTORY_STORE_NOT_FOUND) setStatusError(loadError);
    r1HealthHistoryDayClear(out);
    return loadError;
  }
  {
    StoreLock lock;
    if (lock.locked()) {
      sStatus.error = R1_HISTORY_STORE_OK;
      sStatus.available = true;
      sStatus.encryptionRequired = gSettings.captureEncryptMode != 0;
    }
  }
  return R1_HISTORY_STORE_OK;
}

void r1HealthHistoryStoreGetStatus(R1HealthHistoryStoreStatus& out) {
  StoreLock lock;
  if (!lock.locked()) {
    memset(&out, 0, sizeof(out));
    out.error = R1_HISTORY_STORE_NOT_READY;
    return;
  }
  out = sStatus;
  out.commitPending = sCommitPending || sStatus.commitPending;
}

#endif  // ENABLE_R1_HEALTH
