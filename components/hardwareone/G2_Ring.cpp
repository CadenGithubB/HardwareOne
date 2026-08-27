// =============================================================================
// Even Realities R1 Ring — BLE central
// =============================================================================
// See G2_Ring.h. Connect, pairAuth / time / advStart, subscribe to notify,
// poll vitals, decode into the R1 cache. Shares bleCentralTx with G2 temples.

#include "G2_Ring.h"
#include <esp_attr.h>

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEAdvertisedDevice.h>

#include "System_Debug.h"
#include "System_Command.h"
#include "System_CLIConfirm.h"
#include "System_AuthIdentity.h"
#include "System_TaskUtils.h"  // APP_CORE / PRO_CORE task-placement constants
#include "System_Utils.h"
#include "System_MemUtil.h"
#include "WebServer_Server.h"  // broadcastEventToAllSessions() for SSE push
#include "System_Settings.h"   // gSettings + setSetting() for MAC persistence
#include "BLE_Events.h"        // CompactJson + blePushEvent
#include "BLE_Peers.h"         // peer registry
#include "Bluetooth.h"         // explicit BLE role/transition ownership
#include "BLE_CentralTx.h"     // controller-level TX gate (shared with G2)
#include "G2_Glasses.h"        // g2SetAllTemplesConnPriority, g2WaitForBothConnected
#if ENABLE_MICROPHONE
#include "HAL_Audio.h"         // audioGetSource/audioCaptureOwnedBy — audio arbitration
#include "System_Microphone.h" // micRecordingBusy — real recording lifecycle
#endif
#include "System_R1_Protocol.h"  // R1Encoder + decoder (real wire format)
#include "System_G2_Protocol.h"  // g2BuildRingRawDataPush + G2RingPushFields
#include "G2_Health.h"           // history append + daily backfill
#include "System_Events.h"       // systemEventPost — ring connect/disconnect bus events
#include "System_Clock.h"        // isSynced/isValidEpoch — ring-clock custody gates

#include <freertos/FreeRTOS.h>
#include <time.h>
#include <sys/time.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <string.h>
#include <ctype.h>

static_assert(static_cast<uint8_t>(G2_RING_PROFILE_FW_2_2_7_0005) ==
                  static_cast<uint8_t>(R1_PROFILE_FW_2_2_7_0005),
              "public/internal R1 profile 2.2.7 IDs must match");
static_assert(static_cast<uint8_t>(G2_RING_PROFILE_FW_2_2_9_0003) ==
                  static_cast<uint8_t>(R1_PROFILE_FW_2_2_9_0003),
              "public/internal R1 profile 2.2.9 IDs must match");

#if !CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#error "G2/R1 transport slabs require external BSS in PSRAM"
#endif

// =============================================================================
// Shared state with the G2 scanner
// =============================================================================
// The glasses' scan callback (G2_Glasses.cpp) also classifies EVEN R1_*
// adverts and stashes the advertisedDevice here. That saves us from running
// a second scan pass for the ring in the common case — when the user runs
// `openg2`, the glasses scan will surface both the temples and the ring in
// one pass.
BLEAdvertisedDevice* gRingAdvertisedDevice = nullptr;
String               gRingDeviceName;
String               gRingDeviceAddress;
volatile bool        gRingScanFound       = false;

// =============================================================================
// BLE peer registry binding
// =============================================================================
static bool ringPeerConnectSavedThunk() { return g2RingConnectSaved(); }
static BlePeerConnectAdmission ringPeerConnectSavedAdmissionThunk(
    const BlePeerConnectRequest& request);
static bool ringPublishedConnected();
static void ringPeerDisconnectThunk() {
  // Intentional tear-down (registry / CLI paths that use ops->disconnect).
  g2RingDisconnect(/*userInitiated=*/true);
}
static bool ringPeerIsConnectedThunk()  { return ringPublishedConnected(); }
static const BlePeerOps ringPeerOps = {
  ringPeerConnectSavedThunk,
  ringPeerDisconnectThunk,
  ringPeerIsConnectedThunk,
  ringPeerConnectSavedAdmissionThunk,
};
static const BlePeerSpec ringPeerSpec = {
  BLE_PEER_R1_RING,
  "r1-ring",
  "R1 Ring",
  /*macCount=*/1,
  /*connectable=*/true,
  &ringPeerOps,
};

// =============================================================================
// Private module state
// =============================================================================

struct G2RingState {
  BLEClient*               client          = nullptr;
  BLERemoteCharacteristic* writeChar       = nullptr;
  BLERemoteCharacteristic* notifyChar      = nullptr;
  bool                     initialized     = false;
  bool                     connected       = false;
  bool                     clientStale     = false;
  uint16_t                 mtu             = 23;
  uint32_t                 connectedSince  = 0;
  uint32_t                 packetsReceived = 0;
  uint32_t                 packetsSent     = 0;
  SemaphoreHandle_t        writeMutex      = nullptr;
};
static G2RingState gRing;

// Fixed, task-level transition fence for final UP versus DOWN publication.
// It adds no task or stack. Never hold it across BLE/GATT calls, central-TX,
// or the Ring write mutex.
static StaticSemaphore_t gRingCompletionMutexStorage;
static SemaphoreHandle_t gRingCompletionMutex = nullptr;
static portMUX_TYPE gRingCompletionInitMux = portMUX_INITIALIZER_UNLOCKED;
static bool gRingUpEventPublished = false;

static SemaphoreHandle_t ringCompletionMutex() {
  portENTER_CRITICAL(&gRingCompletionInitMux);
  if (!gRingCompletionMutex) {
    gRingCompletionMutex =
        xSemaphoreCreateRecursiveMutexStatic(&gRingCompletionMutexStorage);
  }
  SemaphoreHandle_t mutex = gRingCompletionMutex;
  portEXIT_CRITICAL(&gRingCompletionInitMux);
  return mutex;
}

class RingCompletionGuard {
 public:
  explicit RingCompletionGuard(TickType_t wait = portMAX_DELAY) {
    SemaphoreHandle_t mutex = ringCompletionMutex();
    locked_ = mutex && xSemaphoreTakeRecursive(mutex, wait) == pdTRUE;
  }
  ~RingCompletionGuard() {
    if (locked_) xSemaphoreGiveRecursive(gRingCompletionMutex);
  }
  explicit operator bool() const { return locked_; }
 private:
  bool locked_ = false;
};

static bool ringPublishedConnected() {
  RingCompletionGuard completion;
  return completion && gRingUpEventPublished;
}

static constexpr uint32_t RING_CENTRAL_TX_MS = 50;
static constexpr uint32_t RING_WRITE_MUTEX_MS = 1500;
static constexpr uint32_t RING_TRANSACTION_TIMEOUT_MS = 5000;
static constexpr uint32_t RING_SETUP_TIMEOUT_MS = 7000;
#if ENABLE_MICROPHONE
// A WAV/EvenAI recording has a hard 60 s capture cap. Keep its already-queued
// ring job alive through finalization, but bound the wait so a wedged recorder
// cannot pin the shared BLE-connect worker forever. The full saved-ring path
// remains below the separate 240 s in-flight watchdog.
static constexpr uint32_t RING_AUDIO_DEFER_TIMEOUT_MS = 90000;
static constexpr uint32_t RING_AUDIO_DEFER_POLL_MS = 100;
#endif
static constexpr uint32_t RING_OWNER_STACK_BYTES = 6144;
static constexpr uint8_t  RING_INTENT_QUEUE_DEPTH = 12;
static constexpr uint8_t  RING_TRANSACTION_HISTORY_DEPTH = 24;
static constexpr uint8_t  RING_RX_QUEUE_DEPTH = 8;
static constexpr uint8_t  RING_PACKET_ACK_DEPTH = 8;
static constexpr uint8_t  RING_RX_DUPLICATE_DEPTH = 16;

enum RingIntentKind : uint8_t {
  RING_INTENT_RAW = 0,
  RING_INTENT_PAIR_AUTH,
  RING_INTENT_DEVICE_INFO,
  RING_INTENT_SYNC_TIME,
  RING_INTENT_ADV_START,
  RING_INTENT_HEALTH_QUERY,
  RING_INTENT_HEALTH_COLLECTION_SET,
  RING_INTENT_LOW_POWER_QUERY,
  RING_INTENT_LOW_POWER_SET,
};

enum RingControlTarget : uint8_t {
  RING_CONTROL_NONE = 0,
  RING_CONTROL_HEALTH,
  RING_CONTROL_LOW_POWER,
};

struct RingIntent {
  G2RingTransactionHandle handle{};
  RingIntentKind kind = RING_INTENT_RAW;
  uint8_t module = 0;
  uint8_t cmd = 0;
  uint8_t subCmd = 0;
  uint8_t statusType = R1_STATUS_TYPE_NOTIFY;
  uint8_t statusMethod = R1_STATUS_METHOD_GET;
  uint8_t statusAck = R1_STATUS_ACK_OK;
  uint8_t coalesceKey = 0;
  RingControlTarget control = RING_CONTROL_NONE;
  G2RingDesiredState desired = G2_RING_PRESERVE;
  bool expectsPayload = false;
  bool verifyAfterAck = false;
  uint8_t payloadSlot = 0xFF;
  uint16_t payloadLen = 0;
  uint32_t epoch = 0;
  int16_t tzMin = 0;   // configured tz (minutes), captured at enqueue for SYNC_TIME
  uint32_t timeoutMs = RING_TRANSACTION_TIMEOUT_MS;
};

static constexpr uint8_t RING_RAW_PAYLOAD_SLOTS = 4;

struct RingActiveTransaction {
  bool valid = false;
  bool frameReady = false;
  bool written = false;
  bool commandAcked = false;
  bool typedDailyDataSeen = false;
  // A decodable, matching positive sleep/daily header was observed. This is
  // deliberately weaker than typedDailyDataSeen: it prevents a false EMPTY
  // conclusion, but can never trigger ACK, ingestion, PRESENT, or VERIFIED.
  bool sleepDataCandidateSeen = false;
  bool readbackPhase = false;
  RingIntent intent{};
  R1Frame frame{};
  uint32_t deadlineMs = 0;
};

// Daily metric data and the matching command ACK are independent notifications
// and captures do not prove a fixed arrival order. These two helpers encode the
// ordering contract without touching transport globals so boot-time assertions
// can exercise both permutations directly.
static bool ringRememberTypedDailyData(RingActiveTransaction& transaction) {
  transaction.typedDailyDataSeen = true;
  return transaction.commandAcked;
}

static bool ringRememberSuccessfulCommandAck(
    RingActiveTransaction& transaction, uint32_t ackedAtMs) {
  const bool firstAck = !transaction.commandAcked;
  transaction.commandAcked = true;
  if (firstAck && transaction.intent.module == R1_MODULE_HEALTH &&
      transaction.intent.subCmd == R1_SUB_DAILY &&
      transaction.intent.expectsPayload) {
    // The pre-ACK deadline bounds command completion. Once ACKed, daily
    // payload/no-data observation gets its own complete bounded window.
    transaction.deadlineMs = ackedAtMs + transaction.intent.timeoutMs;
  }
  return transaction.typedDailyDataSeen;
}

// Integrity-invalid bytes are normally ignored wholesale. Sleep/no-data is
// the one conservative exception: if r1Decode recovered a complete header
// that proves this is a positive data candidate for the active same-generation
// sleep/daily request, remember only that something answered. The caller must
// still reject the frame before packet ACK, dedupe, typed ingestion, or any
// PRESENT/VERIFIED publication.
static bool ringRememberRejectedSleepCandidate(
    RingActiveTransaction& transaction, uint32_t currentGeneration,
    const R1Decoded& decoded) {
  if (!transaction.valid || !transaction.written ||
      transaction.intent.handle.generation != currentGeneration ||
      transaction.intent.module != R1_MODULE_HEALTH ||
      transaction.intent.cmd != R1_CMD_SLEEP ||
      transaction.intent.subCmd != R1_SUB_DAILY ||
      !transaction.intent.expectsPayload ||
      decoded.module != R1_MODULE_HEALTH || decoded.cmd != R1_CMD_SLEEP ||
      decoded.subCmd != R1_SUB_DAILY ||
      decoded.statusType != R1_STATUS_TYPE_NOTIFY ||
      decoded.statusMethod != R1_STATUS_METHOD_SET ||
      decoded.statusAck != R1_STATUS_ACK_OK || decoded.payloadLength == 0) {
    return false;
  }
  transaction.sleepDataCandidateSeen = true;
  return true;
}

struct RingSetupOwner {
  bool active = false;
  bool frameReady = false;
  bool written = false;
  uint32_t generation = 0;
  G2RingSetupState stage = G2_RING_SETUP_IDLE;
  R1Frame frame{};
  uint32_t deadlineMs = 0;
  uint8_t darkProbesSent = 0;    // in-setup profile-safe HR solicits (cap 3)
  uint32_t darkProbeLastMs = 0;  // last solicit attempt, rate-limits retries
};

struct RingRxFrame {
  uint32_t generation;
  uint16_t length;
  uint8_t bytes[R1_MAX_FRAME];
};

struct RingPacketAckPending {
  uint32_t generation;
  R1PacketAckDescriptor descriptor;
};
static_assert(sizeof(RingPacketAckPending) == 12,
              "packet ACK queue entries must stay compact");

struct RingActivePacketAck {
  bool valid = false;
  bool frameReady = false;
  RingPacketAckPending pending{};
  R1Frame frame{};
};

struct RingRxFingerprint {
  uint32_t generation;
  uint32_t crc32;
  uint16_t serial;
  uint8_t module;
  uint8_t cmd;
  uint8_t subCmd;
  uint8_t statusByte;
};

struct RingHistoryCoordinator {
  bool active = false;
  bool force = false;
  bool exclusive = false;
  uint8_t metricIndex = 0;
  uint8_t verifiedCount = 0;
  uint8_t firstError = G2_RING_ERR_NONE;
  uint32_t generation = 0;
  G2RingTransactionHandle transaction{};
};

static RingIntent sIntentQueue[RING_INTENT_QUEUE_DEPTH];
static uint8_t sIntentQueueHead = 0;
static uint8_t sIntentQueueCount = 0;
// Ownership is transport metadata and remains in internal DRAM. The 1 KiB of
// raw command payload is task-context data, so place it unconditionally in the
// configured external-BSS segment. No PSRAM byte is touched while
// sTransportMux is held.
static bool sRawPayloadUsed[RING_RAW_PAYLOAD_SLOTS];
EXT_RAM_BSS_ATTR static uint8_t
    sRawPayloadBytes[RING_RAW_PAYLOAD_SLOTS][R1_MAX_PAYLOAD];
// INTERNAL, deliberately (reverted from PSRAM 2026-08-19): this ring is scanned
// inside portENTER_CRITICAL(&sTransportMux) — ringFindTransactionLocked /
// ringAllocateTransactionLocked / ringUpdateTransaction below, and
// ringMarkGenerationDisconnected — so PSRAM placement would put cache-missing
// reads under masked interrupts, violating the invariant documented above.
// If these 672 B are ever needed, store the slot index in
// G2RingTransactionHandle so the locked touch becomes a single entry first.
static G2RingTransactionStatus sTransactionHistory[RING_TRANSACTION_HISTORY_DEPTH];
static uint8_t sTransactionCursor = 0;
static uint32_t sNextTransactionId = 1;
static portMUX_TYPE sTransportMux = portMUX_INITIALIZER_UNLOCKED;

static constexpr uint8_t RING_RX_SLAB_COUNT = RING_RX_QUEUE_DEPTH + 1;
static_assert(RING_RX_SLAB_COUNT <= UINT8_MAX,
              "RX slab indices must fit in a uint8_t queue item");
enum RingRxSlabState : uint8_t {
  RING_RX_SLAB_FREE = 0,
  RING_RX_SLAB_WRITING,
  RING_RX_SLAB_QUEUED,
  RING_RX_SLAB_PROCESSING,
};
// The FreeRTOS queue carries only slab indices. Its control object, one-byte
// items, and per-slot ownership state stay in internal DRAM; complete frames
// live in external BSS. One extra slab preserves the previous capacity of
// eight queued frames plus one frame being processed by the owner. The
// callback reserves under sRxSlabMux, fills PSRAM after unlocking, then
// zero-wait enqueues the index.
EXT_RAM_BSS_ATTR static RingRxFrame sRxSlabs[RING_RX_SLAB_COUNT];
static uint8_t sRxSlabState[RING_RX_SLAB_COUNT];
static portMUX_TYPE sRxSlabMux = portMUX_INITIALIZER_UNLOCKED;
static StaticQueue_t sRxQueueStorage;
static uint8_t sRxQueueBytes[RING_RX_QUEUE_DEPTH * sizeof(uint8_t)];
static QueueHandle_t sRxQueue = nullptr;
static TaskHandle_t sRingOwnerTask = nullptr;
static SemaphoreHandle_t sSetupDone = nullptr;
static StaticSemaphore_t sSetupDoneStorage;

static volatile uint32_t sLinkGeneration = 0;
static volatile bool sLinkOnline = false;
static volatile bool sSetupRequested = false;
static R1ProtocolProfile sR1Profile = R1_PROFILE_UNKNOWN;
// PSRAM: r1_owner task only. NOLOAD section — the NSDMI image (payloadSlot=0xFF,
// timeoutMs=5000) is DISCARDED at link time; g2RingInit restores the defaults
// before the owner task exists, and every activation fully reassigns the struct.
EXT_RAM_BSS_ATTR static RingActiveTransaction sActiveTransaction;
EXT_RAM_BSS_ATTR static RingSetupOwner sSetupOwner;  // PSRAM: r1_owner task only, never under sTransportMux (verified 2026-08-19)
EXT_RAM_BSS_ATTR static RingPacketAckPending sPacketAckQueue[RING_PACKET_ACK_DEPTH];  // PSRAM: r1_owner only
static uint8_t sPacketAckCount = 0;
EXT_RAM_BSS_ATTR static RingActivePacketAck sActivePacketAck;  // PSRAM: r1_owner only
EXT_RAM_BSS_ATTR static RingRxFingerprint sRxFingerprints[RING_RX_DUPLICATE_DEPTH];  // PSRAM: r1_owner only
static uint8_t sRxFingerprintCursor = 0;
static volatile uint32_t sRxQueueDropped = 0;
static G2RingControlStatus sControlStatus;

static uint32_t ringNextLinkGeneration(uint32_t generation) {
  ++generation;
  return generation == 0 ? 1 : generation;
}

// Activity-daily is sized for a full 144-slot day (~2.3 KB) — too large for the
// ring owner task's stack. Every activity parse runs serialized on that task
// (single-frame validity check, single-frame ingest, and reassembly finalize
// never overlap), so one PSRAM-backed scratch serves all three.
EXT_RAM_BSS_ATTR static R1ActivityDailyResult sRingActivityScratch;

// Large ring frames (activity-daily today) exceed one BLE notification and
// arrive fragmented as [countdown:1][crc32:4][chunk]: countdown decrements to 0
// on the last fragment, every fragment repeats the whole-model CRC32, and the
// chunks concatenate into the model (version…payload). We stitch them into
// sRingReasmBuf, then validate the CRC32 and parse exactly once.
struct RingReassembly {
  bool active;
  uint32_t crc32;         // whole-model CRC32 shared by every fragment
  uint8_t nextCountdown;  // countdown expected on the next fragment
  uint32_t generation;    // link generation this session belongs to
  size_t len;             // model bytes accumulated so far
};
static RingReassembly sRingReasm;
EXT_RAM_BSS_ATTR static uint8_t sRingReasmBuf[R1_ACTIVITY_REASSEMBLED_MODEL_MAX];
static RingHistoryCoordinator sHistoryCoordinator;
static bool sHistoryRefreshRequested = false;
static bool sHistoryRefreshForce = false;
static uint32_t sHistoryRefreshGeneration = 0;
// Poll Now requests fresh transaction handles instead of coalescing onto a
// standalone DAILY intent admitted before the tap.
static bool sHistoryRefreshExclusive = false;
// Protected by sTransportMux. Producers use this bit to coalesce refreshes
// without reading the owner-only coordinator object across cores.
static bool sHistorySweepActive = false;
// The owner has atomically consumed a request but has not yet finished its
// throttle/freshness checks or published Active. Poll Now must treat this as a
// pre-existing sweep; otherwise it can accidentally attribute that sweep's
// completion to a later tap.
static bool sHistorySweepClaimed = false;
static uint32_t sHistorySweepClaimedGeneration = 0;
// Terminal publication for user-visible Health refresh handles. A caller
// snapshots this before admission and waits for it to move, so "Refreshed" is
// tied to a completed DAILY sweep rather than to queue admission.
static uint32_t sHistorySweepCompletionSequence = 0;
static bool sHistorySweepLastSuccessful = false;
static uint8_t sHistorySweepLastError = G2_RING_ERR_NONE;
static uint8_t sHistorySweepLastVerifiedCount = 0;
static uint32_t sLastHistoryAttemptMs = 0;

enum RingHealthPageRefreshPhase : uint8_t {
  RING_HEALTH_REFRESH_IDLE = 0,
  RING_HEALTH_REFRESH_WAIT_PRIOR,
  RING_HEALTH_REFRESH_WAIT_DAILY,
  RING_HEALTH_REFRESH_QUEUE_DEVICE_STATUS,
  RING_HEALTH_REFRESH_WAIT_DEVICE_STATUS,
  RING_HEALTH_REFRESH_TERMINAL,
};

struct RingHealthPageRefresh {
  RingHealthPageRefreshPhase phase = RING_HEALTH_REFRESH_IDLE;
  uint32_t id = 0;
  uint32_t generation = 0;
  uint32_t dailyBaselineSequence = 0;
  uint32_t deadlineMs = 0;
  bool dailyCompleted = false;
  bool dailySuccessful = false;
  uint8_t dailyError = G2_RING_ERR_NONE;
  G2RingTransactionHandle deviceStatus{};
  uint8_t dailyVerifiedCount = 0;
  bool deviceStatusCompleted = false;
  bool deviceStatusVerified = false;
  uint8_t deviceStatusError = G2_RING_ERR_NONE;
  bool terminalSuccessful = false;
  bool terminalPartial = false;
  uint8_t terminalError = G2_RING_ERR_NONE;
};

static RingHealthPageRefresh sHealthPageRefresh;
static RingHealthPageRefresh sHealthPageRefreshTerminalHistory[2];
static uint8_t sHealthPageRefreshTerminalCursor = 0;
static uint32_t sNextHealthPageRefreshId = 1;
static constexpr uint32_t RING_HEALTH_PAGE_REFRESH_TIMEOUT_MS = 75000;

static uint8_t ringHealthPageRefreshResolvedError(
    const RingHealthPageRefresh& refresh, uint8_t fallback) {
  if (refresh.dailyCompleted && !refresh.dailySuccessful) {
    return refresh.dailyError != G2_RING_ERR_NONE
               ? refresh.dailyError
               : (uint8_t)G2_RING_ERR_TIMEOUT;
  }
  if (refresh.deviceStatusCompleted && !refresh.deviceStatusVerified) {
    return refresh.deviceStatusError != G2_RING_ERR_NONE
               ? refresh.deviceStatusError
               : (uint8_t)G2_RING_ERR_TIMEOUT;
  }
  return fallback;
}

// The encoder is deliberately private to ringOwnerTask. No producer builds a
// frame or allocates a serial; producers enqueue fixed-size semantic intents.
static R1Encoder gR1Encoder;

static void ringOwnerTask(void* arg);
static void ringClockCustodyReset();

static void ringWakeOwner() {
  TaskHandle_t owner = sRingOwnerTask;
  if (owner) xTaskNotifyGive(owner);
}

static int ringReserveRxSlab() {
  int slot = -1;
  portENTER_CRITICAL(&sRxSlabMux);
  for (uint8_t i = 0; i < RING_RX_SLAB_COUNT; ++i) {
    if (sRxSlabState[i] == RING_RX_SLAB_FREE) {
      sRxSlabState[i] = RING_RX_SLAB_WRITING;
      slot = i;
      break;
    }
  }
  portEXIT_CRITICAL(&sRxSlabMux);
  return slot;
}

static bool ringPublishRxSlab(uint8_t slot) {
  if (slot >= RING_RX_SLAB_COUNT) return false;
  bool publish = false;
  portENTER_CRITICAL(&sRxSlabMux);
  if (sRxSlabState[slot] == RING_RX_SLAB_WRITING) {
    sRxSlabState[slot] = RING_RX_SLAB_QUEUED;
    publish = true;
  }
  portEXIT_CRITICAL(&sRxSlabMux);
  return publish;
}

static bool ringClaimQueuedRxSlab(uint8_t slot) {
  if (slot >= RING_RX_SLAB_COUNT) return false;
  bool claimed = false;
  portENTER_CRITICAL(&sRxSlabMux);
  if (sRxSlabState[slot] == RING_RX_SLAB_QUEUED) {
    sRxSlabState[slot] = RING_RX_SLAB_PROCESSING;
    claimed = true;
  }
  portEXIT_CRITICAL(&sRxSlabMux);
  return claimed;
}

static bool ringRxSlabAllFree() {
  bool allFree = true;
  portENTER_CRITICAL(&sRxSlabMux);
  for (uint8_t i = 0; i < RING_RX_SLAB_COUNT; ++i) {
    if (sRxSlabState[i] != RING_RX_SLAB_FREE) {
      allFree = false;
      break;
    }
  }
  portEXIT_CRITICAL(&sRxSlabMux);
  return allFree;
}

static void ringReleaseRxSlab(uint8_t slot) {
  if (slot >= RING_RX_SLAB_COUNT) return;
  portENTER_CRITICAL(&sRxSlabMux);
  sRxSlabState[slot] = RING_RX_SLAB_FREE;
  portEXIT_CRITICAL(&sRxSlabMux);
}

// Called only while gRing.initialized is false, before any notify callback can
// be registered. It makes a failed partial init retry deterministic without
// touching external memory or inventing a disconnect-time ABA reset.
static void ringResetRxIngressForInit() {
  if (sRxQueue) xQueueReset(sRxQueue);
  portENTER_CRITICAL(&sRxSlabMux);
  memset(sRxSlabState, 0, sizeof(sRxSlabState));
  portEXIT_CRITICAL(&sRxSlabMux);
}

static bool ringSnapshotLink(uint32_t& generation) {
  portENTER_CRITICAL(&sTransportMux);
  const bool online = sLinkOnline;
  generation = sLinkGeneration;
  portEXIT_CRITICAL(&sTransportMux);
  return online;
}

static R1ProtocolProfile ringSnapshotProtocolProfile() {
  portENTER_CRITICAL(&sTransportMux);
  const R1ProtocolProfile profile = sR1Profile;
  portEXIT_CRITICAL(&sTransportMux);
  return profile;
}

static int ringReserveRawPayload(const uint8_t* payload, size_t payloadLen);
static void ringReleaseRawPayload(uint8_t slot);
static bool ringRawPayloadOwned(uint8_t slot);
static void ringExtractTelemetryCache(const R1Decoded& decoded,
                                      uint32_t generation);
static void ringReasmFinalize(uint32_t generation);
static bool ringTelemetrySnapshotSelfTest();

static bool ringTransactionOrderingSelfTest() {
  bool ok = true;

  // Cache reset and setup must derive the exact same non-zero generation,
  // including wrap. The runtime setup path additionally refuses publication
  // if its prepared telemetry generation does not equal this value.
  ok &= ringNextLinkGeneration(0) == 1;
  ok &= ringNextLinkGeneration(41) == 42;
  ok &= ringNextLinkGeneration(UINT32_MAX) == 1;
  ok &= ringTelemetrySnapshotSelfTest();
  ok &= g2HealthHistoryPeerCustodySelfTest();

  RingActiveTransaction dataFirst{};
  dataFirst.intent.module = R1_MODULE_HEALTH;
  dataFirst.intent.cmd = R1_CMD_HEARTRATE;
  dataFirst.intent.subCmd = R1_SUB_DAILY;
  dataFirst.intent.expectsPayload = true;
  dataFirst.intent.timeoutMs = 137;
  dataFirst.deadlineMs = 17;
  ok &= !ringRememberTypedDailyData(dataFirst);
  ok &= dataFirst.typedDailyDataSeen && !dataFirst.commandAcked &&
        dataFirst.deadlineMs == 17;
  ok &= ringRememberSuccessfulCommandAck(dataFirst, 1000);
  ok &= dataFirst.commandAcked && dataFirst.deadlineMs == 1137;
  // A duplicate ACK must not extend the bounded observation window forever.
  ok &= ringRememberSuccessfulCommandAck(dataFirst, 2000);
  ok &= dataFirst.deadlineMs == 1137;

  RingActiveTransaction ackFirst{};
  ackFirst.intent.module = R1_MODULE_HEALTH;
  ackFirst.intent.cmd = R1_CMD_SPO2;
  ackFirst.intent.subCmd = R1_SUB_DAILY;
  ackFirst.intent.expectsPayload = true;
  ackFirst.intent.timeoutMs = 211;
  ackFirst.deadlineMs = 23;
  ok &= !ringRememberSuccessfulCommandAck(ackFirst, 3000);
  ok &= ackFirst.commandAcked && !ackFirst.typedDailyDataSeen &&
        ackFirst.deadlineMs == 3211;
  ok &= ringRememberTypedDailyData(ackFirst);

  RingActiveTransaction emptySleep{};
  emptySleep.intent.module = R1_MODULE_HEALTH;
  emptySleep.intent.cmd = R1_CMD_SLEEP;
  emptySleep.intent.subCmd = R1_SUB_DAILY;
  emptySleep.intent.expectsPayload = true;
  emptySleep.intent.timeoutMs = 499;
  emptySleep.sleepDataCandidateSeen = true;
  emptySleep.deadlineMs = 31;
  ok &= !ringRememberSuccessfulCommandAck(emptySleep, 4000);
  ok &= emptySleep.commandAcked && !emptySleep.typedDailyDataSeen &&
        emptySleep.sleepDataCandidateSeen && emptySleep.deadlineMs == 4499;

  RingActiveTransaction rejectedSleep{};
  rejectedSleep.valid = true;
  rejectedSleep.written = true;
  rejectedSleep.intent.handle.generation = 7;
  rejectedSleep.intent.module = R1_MODULE_HEALTH;
  rejectedSleep.intent.cmd = R1_CMD_SLEEP;
  rejectedSleep.intent.subCmd = R1_SUB_DAILY;
  rejectedSleep.intent.expectsPayload = true;
  R1Decoded candidate{};
  candidate.module = R1_MODULE_HEALTH;
  candidate.cmd = R1_CMD_SLEEP;
  candidate.subCmd = R1_SUB_DAILY;
  candidate.statusType = R1_STATUS_TYPE_NOTIFY;
  candidate.statusMethod = R1_STATUS_METHOD_SET;
  candidate.statusAck = R1_STATUS_ACK_OK;
  candidate.payloadLength = 1;
  ok &= !ringRememberRejectedSleepCandidate(rejectedSleep, 6, candidate);
  ok &= !rejectedSleep.sleepDataCandidateSeen;
  ok &= ringRememberRejectedSleepCandidate(rejectedSleep, 7, candidate);
  ok &= rejectedSleep.sleepDataCandidateSeen;
  rejectedSleep.sleepDataCandidateSeen = false;
  candidate.payloadLength = 0;
  ok &= !ringRememberRejectedSleepCandidate(rejectedSleep, 7, candidate);
  ok &= !rejectedSleep.sleepDataCandidateSeen;

  DEBUG_RING_LIFECYCLEF("[RING] transaction ordering self-test: %s",
                        ok ? "PASS" : "FAIL");
  return ok;
}

// Exercises the production storage primitives before the BLE callback or
// owner task can run. This is deliberately hardware-independent: it validates
// fixed-pool copy/ownership, FIFO publication, queue-full rollback, the ninth
// processing slab that preserves prior burst capacity, and complete cleanup.
static bool ringStorageSelfTest() {
  bool ok = true;
  if (!sRxQueue) return false;
  ringResetRxIngressForInit();

  uint8_t rawSource[RING_RAW_PAYLOAD_SLOTS][4]{};
  int rawSlots[RING_RAW_PAYLOAD_SLOTS] = {-1, -1, -1, -1};
  for (uint8_t i = 0; i < RING_RAW_PAYLOAD_SLOTS; ++i) {
    rawSource[i][0] = (uint8_t)(0x40 + i);
    rawSource[i][1] = (uint8_t)(0x50 + i);
    rawSlots[i] = ringReserveRawPayload(rawSource[i], sizeof(rawSource[i]));
    if (rawSlots[i] < 0) {
      ok = false;
      continue;
    }
    rawSource[i][0] = 0;
    ok &= sRawPayloadBytes[rawSlots[i]][0] == (uint8_t)(0x40 + i) &&
          sRawPayloadBytes[rawSlots[i]][1] == (uint8_t)(0x50 + i);
  }
  const uint8_t overflowPayload = 0xEE;
  ok &= ringReserveRawPayload(&overflowPayload, 1) < 0;
  if (rawSlots[1] >= 0) {
    ringReleaseRawPayload((uint8_t)rawSlots[1]);
    rawSlots[1] = ringReserveRawPayload(&overflowPayload, 1);
    ok &= rawSlots[1] >= 0 &&
          sRawPayloadBytes[rawSlots[1]][0] == overflowPayload;
  }
  for (uint8_t i = 0; i < RING_RAW_PAYLOAD_SLOTS; ++i) {
    if (rawSlots[i] >= 0) ringReleaseRawPayload((uint8_t)rawSlots[i]);
  }
  for (uint8_t i = 0; i < RING_RAW_PAYLOAD_SLOTS; ++i) {
    ok &= !ringRawPayloadOwned(i);
  }

  // Fill the depth-eight descriptor queue in FIFO order.
  for (uint8_t marker = 0; marker < RING_RX_QUEUE_DEPTH; ++marker) {
    const int reserved = ringReserveRxSlab();
    if (reserved < 0) {
      ok = false;
      break;
    }
    const uint8_t slot = (uint8_t)reserved;
    sRxSlabs[slot].generation = (uint32_t)(10 + marker);
    sRxSlabs[slot].length = 1;
    sRxSlabs[slot].bytes[0] = marker;
    if (!ringPublishRxSlab(slot) ||
        xQueueSend(sRxQueue, &slot, 0) != pdTRUE) {
      ringReleaseRxSlab(slot);
      ok = false;
      break;
    }
  }
  ok &= uxQueueMessagesWaiting(sRxQueue) == RING_RX_QUEUE_DEPTH;

  // The ninth slab can be filled, but queue publication must fail while all
  // eight descriptors wait; rollback must make that slot immediately reusable.
  int reserved = ringReserveRxSlab();
  ok &= reserved >= 0;
  if (reserved >= 0) {
    const uint8_t overflowSlot = (uint8_t)reserved;
    sRxSlabs[overflowSlot].generation = 99;
    sRxSlabs[overflowSlot].length = 1;
    sRxSlabs[overflowSlot].bytes[0] = 0xFE;
    const bool published = ringPublishRxSlab(overflowSlot);
    ok &= published && xQueueSend(sRxQueue, &overflowSlot, 0) != pdTRUE;
    ringReleaseRxSlab(overflowSlot);
  }

  // Hold the oldest slab in PROCESSING while refilling its queue position.
  uint8_t processingSlot = UINT8_MAX;
  ok &= xQueueReceive(sRxQueue, &processingSlot, 0) == pdTRUE;
  ok &= ringClaimQueuedRxSlab(processingSlot);
  if (processingSlot < RING_RX_SLAB_COUNT) {
    ok &= sRxSlabs[processingSlot].bytes[0] == 0 &&
          sRxSlabs[processingSlot].generation == 10;
  }
  reserved = ringReserveRxSlab();
  ok &= reserved >= 0;
  if (reserved >= 0) {
    const uint8_t refillSlot = (uint8_t)reserved;
    sRxSlabs[refillSlot].generation = 18;
    sRxSlabs[refillSlot].length = 1;
    sRxSlabs[refillSlot].bytes[0] = 8;
    if (!ringPublishRxSlab(refillSlot) ||
        xQueueSend(sRxQueue, &refillSlot, 0) != pdTRUE) {
      ringReleaseRxSlab(refillSlot);
      ok = false;
    }
  }
  ok &= ringReserveRxSlab() < 0;
  if (processingSlot < RING_RX_SLAB_COUNT) ringReleaseRxSlab(processingSlot);

  for (uint8_t expected = 1; expected <= 8; ++expected) {
    uint8_t slot = UINT8_MAX;
    ok &= xQueueReceive(sRxQueue, &slot, 0) == pdTRUE;
    if (slot >= RING_RX_SLAB_COUNT || !ringClaimQueuedRxSlab(slot)) {
      ok = false;
      continue;
    }
    ok &= sRxSlabs[slot].bytes[0] == expected &&
          sRxSlabs[slot].generation == (uint32_t)(10 + expected);
    ringReleaseRxSlab(slot);
  }
  ok &= uxQueueMessagesWaiting(sRxQueue) == 0 && ringRxSlabAllFree();

  // Leave retryable init in a known-empty state even when an assertion fails.
  ringResetRxIngressForInit();
  for (uint8_t i = 0; i < RING_RAW_PAYLOAD_SLOTS; ++i) {
    ringReleaseRawPayload(i);
  }
  DEBUG_RING_LIFECYCLEF("[RING] Storage self-test: %s", ok ? "PASS" : "FAIL");
  return ok;
}

static G2RingTransactionStatus* ringFindTransactionLocked(
    const G2RingTransactionHandle& handle) {
  if (handle.id == 0 || handle.generation == 0) return nullptr;
  for (uint8_t i = 0; i < RING_TRANSACTION_HISTORY_DEPTH; ++i) {
    G2RingTransactionStatus& status = sTransactionHistory[i];
    if (status.handle.id == handle.id &&
        status.handle.generation == handle.generation) {
      return &status;
    }
  }
  return nullptr;
}

static G2RingTransactionStatus* ringAllocateTransactionLocked(
    const RingIntent& intent) {
  for (uint8_t n = 0; n < RING_TRANSACTION_HISTORY_DEPTH; ++n) {
    const uint8_t i = (uint8_t)((sTransactionCursor + n) %
                                RING_TRANSACTION_HISTORY_DEPTH);
    G2RingTransactionStatus& status = sTransactionHistory[i];
    if (status.state != G2_RING_TX_INVALID && status.completedAtMs == 0) {
      continue;
    }
    status = G2RingTransactionStatus{};
    status.handle = intent.handle;
    status.state = G2_RING_TX_QUEUED;
    status.module = intent.module;
    status.cmd = intent.cmd;
    status.subCmd = intent.subCmd;
    status.queuedAtMs = millis();
    sTransactionCursor = (uint8_t)((i + 1) % RING_TRANSACTION_HISTORY_DEPTH);
    return &status;
  }
  return nullptr;
}

// `handle` BY VALUE deliberately: callers pass references into PSRAM-resident
// sActiveTransaction (e.g. :3029), and this function reads the handle inside
// portENTER_CRITICAL(&sTransportMux) below — the copy must happen BEFORE the
// spinlock so no PSRAM byte is touched under it (8-byte POD, cheap).
static void ringUpdateTransaction(G2RingTransactionHandle handle,
                                  G2RingTransactionState state,
                                  uint8_t errorCode = G2_RING_ERR_NONE,
                                  uint8_t ackCode = 0,
                                  bool completed = false) {
  const uint32_t now = millis();
  portENTER_CRITICAL(&sTransportMux);
  G2RingTransactionStatus* status = ringFindTransactionLocked(handle);
  if (status) {
    status->state = state;
    status->errorCode = errorCode;
    status->ackCode = ackCode;
    if (state == G2_RING_TX_WRITTEN && status->writtenAtMs == 0) {
      status->writtenAtMs = now;
    }
    if (completed) status->completedAtMs = now;
  }
  portEXIT_CRITICAL(&sTransportMux);
}

static int ringReserveRawPayload(const uint8_t* payload, size_t payloadLen) {
  if (payloadLen == 0) return -1;
  int slot = -1;
  portENTER_CRITICAL(&sTransportMux);
  for (uint8_t i = 0; i < RING_RAW_PAYLOAD_SLOTS; ++i) {
    if (!sRawPayloadUsed[i]) {
      sRawPayloadUsed[i] = true;
      slot = i;
      break;
    }
  }
  portEXIT_CRITICAL(&sTransportMux);
  if (slot >= 0) memcpy(sRawPayloadBytes[slot], payload, payloadLen);
  return slot;
}

static void ringReleaseRawPayload(uint8_t slot) {
  if (slot >= RING_RAW_PAYLOAD_SLOTS) return;
  portENTER_CRITICAL(&sTransportMux);
  sRawPayloadUsed[slot] = false;
  portEXIT_CRITICAL(&sTransportMux);
}

static bool ringRawPayloadOwned(uint8_t slot) {
  if (slot >= RING_RAW_PAYLOAD_SLOTS) return false;
  portENTER_CRITICAL(&sTransportMux);
  const bool owned = sRawPayloadUsed[slot];
  portEXIT_CRITICAL(&sTransportMux);
  return owned;
}

static bool ringEnqueueIntent(RingIntent intent,
                              G2RingTransactionHandle* outHandle = nullptr,
                              bool wakeOwner = true) {
  if (outHandle) *outHandle = G2RingTransactionHandle{};
  if (!gRing.connected || !gRing.writeChar || gRing.clientStale) {
    return false;
  }

  portENTER_CRITICAL(&sTransportMux);
  if (!sLinkOnline) {
    portEXIT_CRITICAL(&sTransportMux);
    return false;
  }
  const uint32_t generation = sLinkGeneration;
  if (intent.coalesceKey != 0) {
    for (uint8_t i = 0; i < sIntentQueueCount; ++i) {
      const uint8_t index = (uint8_t)((sIntentQueueHead + i) %
                                      RING_INTENT_QUEUE_DEPTH);
      if (sIntentQueue[index].handle.generation == generation &&
          sIntentQueue[index].coalesceKey == intent.coalesceKey) {
        if (outHandle) *outHandle = sIntentQueue[index].handle;
        portEXIT_CRITICAL(&sTransportMux);
        return true;
      }
    }
  }
  if (sIntentQueueCount >= RING_INTENT_QUEUE_DEPTH) {
    portEXIT_CRITICAL(&sTransportMux);
    return false;
  }
  uint32_t id = sNextTransactionId++;
  if (id == 0) id = sNextTransactionId++;
  intent.handle = { id, generation };
  if (!ringAllocateTransactionLocked(intent)) {
    portEXIT_CRITICAL(&sTransportMux);
    return false;
  }
  const uint8_t tail = (uint8_t)((sIntentQueueHead + sIntentQueueCount) %
                                 RING_INTENT_QUEUE_DEPTH);
  sIntentQueue[tail] = intent;
  ++sIntentQueueCount;
  if (outHandle) *outHandle = intent.handle;
  portEXIT_CRITICAL(&sTransportMux);
  if (wakeOwner) ringWakeOwner();
  return true;
}

static bool ringPopIntent(RingIntent& out) {
  portENTER_CRITICAL(&sTransportMux);
  if (sIntentQueueCount == 0) {
    portEXIT_CRITICAL(&sTransportMux);
    return false;
  }
  out = sIntentQueue[sIntentQueueHead];
  sIntentQueueHead = (uint8_t)((sIntentQueueHead + 1) %
                               RING_INTENT_QUEUE_DEPTH);
  --sIntentQueueCount;
  portEXIT_CRITICAL(&sTransportMux);
  return true;
}

static void ringMarkGenerationDisconnected(uint32_t generation) {
  const uint32_t now = millis();
  portENTER_CRITICAL(&sTransportMux);
  for (uint8_t i = 0; i < RING_TRANSACTION_HISTORY_DEPTH; ++i) {
    G2RingTransactionStatus& status = sTransactionHistory[i];
    if (status.handle.generation == generation &&
        status.state != G2_RING_TX_INVALID && status.completedAtMs == 0) {
      status.state = G2_RING_TX_DISCONNECTED;
      status.errorCode = G2_RING_ERR_DISCONNECTED;
      status.completedAtMs = now;
    }
  }
  // Compact only this generation out of the descriptor ring. A fast reconnect
  // may already have accepted new-generation intents before the owner observes
  // the old barrier; those must survive.
  uint8_t kept = 0;
  for (uint8_t i = 0; i < sIntentQueueCount; ++i) {
    const uint8_t readIndex = (uint8_t)((sIntentQueueHead + i) %
                                        RING_INTENT_QUEUE_DEPTH);
    const RingIntent intent = sIntentQueue[readIndex];
    if (intent.handle.generation == generation) {
      if (intent.payloadSlot < RING_RAW_PAYLOAD_SLOTS) {
        sRawPayloadUsed[intent.payloadSlot] = false;
      }
      continue;
    }
    const uint8_t writeIndex = (uint8_t)((sIntentQueueHead + kept) %
                                         RING_INTENT_QUEUE_DEPTH);
    if (writeIndex != readIndex) sIntentQueue[writeIndex] = intent;
    ++kept;
  }
  sIntentQueueCount = kept;
  if (kept == 0) sIntentQueueHead = 0;
  if (sControlStatus.generation == generation) {
    sControlStatus.healthPending = false;
    sControlStatus.lowPowerPending = false;
    sControlStatus.healthLastError = G2_RING_ERR_DISCONNECTED;
    sControlStatus.lowPowerLastError = G2_RING_ERR_DISCONNECTED;
    if (sControlStatus.setupState != G2_RING_SETUP_READY) {
      sControlStatus.setupState = G2_RING_SETUP_ERROR;
      sControlStatus.setupLastError = G2_RING_ERR_DISCONNECTED;
    }
  }
  portEXIT_CRITICAL(&sTransportMux);
}

static void ringTransportDisconnected() {
  portENTER_CRITICAL(&sTransportMux);
  sLinkOnline = false;
  portEXIT_CRITICAL(&sTransportMux);
  ringWakeOwner();
}

// Lock order: bleCentralTx → gRing.writeMutex (never reverse). All ring
// GATT writes go through here so image bursts on the glasses cannot race
// ring polls into the shared BT controller queue.
// dropClientPtr=true only when the underlying object is (about to be) freed
// by someone else — i.e. before BLEDevice::deinit, which deletes m_pClient.
// With the BLE stack still live, pass false: deleting here would race the
// async DISCONNECT_EVT (the object is still registered for events until that
// handler unregisters it), and nulling without delete leaks the client and
// its ~10-14 KB GATT cache. Keeping the pointer with clientStale=true lets
// the next connect's stale-replacement branch reap it safely.
static void ringClockCustodyReset();  // ring-clock custody state (defined below)

static void ringClearGattPointers(bool dropClientPtr) {
  ringTransportDisconnected();
  gRing.connected  = false;
  gRing.writeChar  = nullptr;
  gRing.notifyChar = nullptr;
  if (dropClientPtr) gRing.client = nullptr;
  gRing.clientStale = true;
  ringClockCustodyReset();  // defined below — no cross-link state survives
}

enum RingWriteResult : uint8_t {
  RING_WRITE_OK,
  RING_WRITE_BUSY,
  RING_WRITE_DISCONNECTED,
  RING_WRITE_FAILED,
};

// Lock order: bleCentralTx → gRing.writeMutex. Only ringOwnerTask calls
// this function, so no producer can race an encoder serial or GATT write.
static RingWriteResult ringOwnerWrite(const uint8_t* data, size_t len) {
  if (!data || len == 0 || !sLinkOnline || !gRing.connected ||
      !gRing.writeChar || !gRing.writeMutex || gRing.clientStale) {
    return RING_WRITE_DISCONNECTED;
  }
  if (!bleCentralTxTake(RING_CENTRAL_TX_MS)) return RING_WRITE_BUSY;
  if (xSemaphoreTake(gRing.writeMutex, pdMS_TO_TICKS(RING_WRITE_MUTEX_MS)) !=
      pdTRUE) {
    bleCentralTxGive();
    return RING_WRITE_BUSY;
  }
  if (!sLinkOnline || !gRing.connected || !gRing.writeChar || gRing.clientStale) {
    xSemaphoreGive(gRing.writeMutex);
    bleCentralTxGive();
    return RING_WRITE_DISCONNECTED;
  }
  const bool ok =
      gRing.writeChar->writeValue(const_cast<uint8_t*>(data), len, false);
  if (ok) ++gRing.packetsSent;
  xSemaphoreGive(gRing.writeMutex);
  bleCentralTxGive();
  return ok ? RING_WRITE_OK : RING_WRITE_FAILED;
}

// Compatibility hook used by paced G2 send paths. Pre-encoded queues no
// longer exist; wake the semantic transaction owner instead.
void g2RingTryDrainPendingTx() { ringWakeOwner(); }

bool g2RingGetTransactionStatus(const G2RingTransactionHandle& handle,
                                G2RingTransactionStatus& out) {
  bool found = false;
  portENTER_CRITICAL(&sTransportMux);
  G2RingTransactionStatus* status = ringFindTransactionLocked(handle);
  if (status) {
    out = *status;
    found = true;
  }
  portEXIT_CRITICAL(&sTransportMux);
  return found;
}

void g2RingGetControlStatus(G2RingControlStatus& out) {
  portENTER_CRITICAL(&sTransportMux);
  out = sControlStatus;
  portEXIT_CRITICAL(&sTransportMux);
}

bool g2RingSubmitRawTransaction(
    uint8_t module, uint8_t cmd, uint8_t subCmd,
    uint8_t statusType, uint8_t statusMethod, uint8_t statusAck,
    const uint8_t* payload, size_t payloadLen,
    G2RingTransactionHandle* outHandle) {
  if (payloadLen > R1_MAX_PAYLOAD || (payloadLen != 0 && !payload) ||
      statusType > 1 || statusMethod > 1 || statusAck > 3) {
    if (outHandle) *outHandle = G2RingTransactionHandle{};
    return false;
  }
  RingIntent intent{};
  intent.kind = RING_INTENT_RAW;
  intent.module = module;
  intent.cmd = cmd;
  intent.subCmd = subCmd;
  intent.statusType = statusType;
  intent.statusMethod = statusMethod;
  intent.statusAck = statusAck;
  intent.expectsPayload = (statusType == R1_STATUS_TYPE_NOTIFY &&
                           statusMethod == R1_STATUS_METHOD_GET);
  intent.payloadLen = (uint16_t)payloadLen;
  if (payloadLen) {
    const int slot = ringReserveRawPayload(payload, payloadLen);
    if (slot < 0) return false;
    intent.payloadSlot = (uint8_t)slot;
  }
  const bool queued = ringEnqueueIntent(intent, outHandle);
  if (!queued && intent.payloadSlot < RING_RAW_PAYLOAD_SLOTS) {
    ringReleaseRawPayload(intent.payloadSlot);
  }
  return queued;
}

static bool ringEnqueueHealthDataQuery(uint8_t cmd, uint8_t subCmd,
                                       uint8_t coalesceKey,
                                       G2RingTransactionHandle* outHandle = nullptr) {
  const R1ProtocolProfile profile = ringSnapshotProtocolProfile();
  if (((subCmd == R1_SUB_POINT || subCmd == R1_SUB_MEASURE) &&
       !r1ProfileSupportsPointMeasureQuery(profile)) ||
      !r1ProfileSupportsHealthQuery(profile, cmd, subCmd)) {
    if (outHandle) *outHandle = G2RingTransactionHandle{};
    return false;
  }
  RingIntent intent{};
  intent.kind = RING_INTENT_HEALTH_QUERY;
  intent.module = R1_MODULE_HEALTH;
  intent.cmd = cmd;
  intent.subCmd = subCmd;
  intent.coalesceKey = coalesceKey;
  intent.expectsPayload = true;
  return ringEnqueueIntent(intent, outHandle);
}

static bool ringEnqueueSystemQuery(uint8_t subCmd, uint8_t coalesceKey,
                                   G2RingTransactionHandle* outHandle = nullptr) {
  const R1ProtocolProfile profile = ringSnapshotProtocolProfile();
  if ((subCmd == R1_SUB_WEAR_STATUS &&
       !r1ProfileSupportsWearStatus(profile)) ||
      (subCmd == R1_SUB_USER_INFO &&
       !r1ProfileSupportsUserInfo(profile))) {
    if (outHandle) *outHandle = G2RingTransactionHandle{};
    return false;
  }
  RingIntent intent{};
  intent.kind = RING_INTENT_RAW;
  intent.module = R1_MODULE_SYSTEM;
  intent.cmd = R1_CMD_SYSTEM;
  intent.subCmd = subCmd;
  intent.coalesceKey = coalesceKey;
  intent.expectsPayload = true;
  return ringEnqueueIntent(intent, outHandle);
}

static bool ringConfiguredTimezoneMinutes(int16_t& out);  // defined just below

static bool ringEnqueueTimeSync(uint32_t epoch,
                                G2RingTransactionHandle* outHandle = nullptr) {
  RingIntent intent{};
  intent.kind = RING_INTENT_SYNC_TIME;
  intent.module = R1_MODULE_SYSTEM;
  intent.cmd = R1_CMD_SYSTEM;
  intent.subCmd = R1_SUB_SYSTEM_TIME;
  intent.statusMethod = R1_STATUS_METHOD_SET;
  intent.epoch = epoch;
  // Capture the configured tz alongside the epoch so the frame the owner task
  // later builds carries THIS instant's offset, and completion records exactly
  // what was sent — the tick keys tz-change re-pushes off that recorded value.
  if (!ringConfiguredTimezoneMinutes(intent.tzMin)) return false;
  return ringEnqueueIntent(intent, outHandle);
}

static bool ringConfiguredTimezoneMinutes(int16_t& out) {
  const int timezoneMinutes = Clock::tzOffsetMinutes();
  if (timezoneMinutes < -720 || timezoneMinutes > 840) return false;
  out = static_cast<int16_t>(timezoneMinutes);
  return true;
}

static bool ringEnqueueAdvStart(
    G2RingTransactionHandle* outHandle = nullptr) {
  RingIntent intent{};
  intent.kind = RING_INTENT_ADV_START;
  intent.module = R1_MODULE_SYSTEM;
  intent.cmd = R1_CMD_SYSTEM;
  intent.subCmd = R1_SUB_ADV_START;
  return ringEnqueueIntent(intent, outHandle);
}

static bool ringControlSupported(R1ProtocolProfile profile,
                                 RingControlTarget control,
                                 G2RingDesiredState desired) {
  if (profile == R1_PROFILE_UNKNOWN) return true;
  if (control == RING_CONTROL_HEALTH) {
    return desired == G2_RING_PRESERVE ||
           r1ProfileSupportsHealthCollectionSet(profile,
                                                 desired == G2_RING_ON);
  }
  if (control == RING_CONTROL_LOW_POWER) {
    return r1ProfileSupportsLowPower(profile);
  }
  return true;
}

// Caller holds sTransportMux. This is the stable local result for a desired
// operation that the exact active profile does not support: no transaction is
// left armed and no observation is invented.
static void ringSetControlUnsupportedLocked(RingControlTarget control) {
  if (control == RING_CONTROL_HEALTH) {
    sControlStatus.healthPending = false;
    sControlStatus.healthObserved = G2_RING_OBS_UNKNOWN;
    sControlStatus.healthObservedAtMs = 0;
    sControlStatus.healthLastError = G2_RING_ERR_FEATURE_UNSUPPORTED;
    sControlStatus.healthTransaction = G2RingTransactionHandle{};
  } else if (control == RING_CONTROL_LOW_POWER) {
    sControlStatus.lowPowerPending = false;
    sControlStatus.lowPowerObserved = G2_RING_OBS_UNKNOWN;
    sControlStatus.lowPowerObservedAtMs = 0;
    sControlStatus.lowPowerLastError = G2_RING_ERR_FEATURE_UNSUPPORTED;
    sControlStatus.lowPowerTransaction = G2RingTransactionHandle{};
  }
}

static bool ringSetControlUnsupportedIfKnown(RingControlTarget control,
                                             G2RingDesiredState desired) {
  bool unsupported = false;
  portENTER_CRITICAL(&sTransportMux);
  if (sR1Profile != R1_PROFILE_UNKNOWN &&
      !ringControlSupported(sR1Profile, control, desired)) {
    ringSetControlUnsupportedLocked(control);
    unsupported = true;
  }
  portEXIT_CRITICAL(&sTransportMux);
  return unsupported;
}

static bool ringQueueControlIntent(RingControlTarget control,
                                   G2RingDesiredState desired,
                                   G2RingTransactionHandle* outHandle) {
  if (outHandle) *outHandle = G2RingTransactionHandle{};
  if (ringSetControlUnsupportedIfKnown(control, desired)) return false;
  // 0x0E GET/readback has no capture evidence. Preserve is literally no
  // write, and an explicit SET can become ACKED but not verified.
  if (control == RING_CONTROL_HEALTH && desired == G2_RING_PRESERVE) {
    portENTER_CRITICAL(&sTransportMux);
    sControlStatus.healthPending = false;
    sControlStatus.healthObserved = G2_RING_OBS_UNKNOWN;
    sControlStatus.healthLastError = G2_RING_ERR_NONE;
    sControlStatus.healthTransaction = G2RingTransactionHandle{};
    portEXIT_CRITICAL(&sTransportMux);
    return true;
  }
  RingIntent intent{};
  intent.module = R1_MODULE_SYSTEM;
  intent.cmd = R1_CMD_SYSTEM;
  intent.control = control;
  intent.desired = desired;
  intent.expectsPayload = (desired == G2_RING_PRESERVE);
  intent.verifyAfterAck = control == RING_CONTROL_LOW_POWER &&
                          desired != G2_RING_PRESERVE;
  if (control == RING_CONTROL_HEALTH) {
    intent.kind = RING_INTENT_HEALTH_COLLECTION_SET;
    intent.expectsPayload = false;
    intent.subCmd = R1_SUB_HEALTH_SETTINGS;
  } else {
    intent.kind = desired == G2_RING_PRESERVE
                      ? RING_INTENT_LOW_POWER_QUERY
                      : RING_INTENT_LOW_POWER_SET;
    intent.subCmd = R1_SUB_SYSTEM_SETTINGS;
  }
  intent.statusMethod = desired == G2_RING_PRESERVE
                            ? R1_STATUS_METHOD_GET
                            : R1_STATUS_METHOD_SET;
  intent.epoch = (uint32_t)time(nullptr);

  G2RingTransactionHandle handle{};
  const bool queued = ringEnqueueIntent(intent, &handle,
                                        /*wakeOwner=*/false);
  portENTER_CRITICAL(&sTransportMux);
  if (control == RING_CONTROL_HEALTH) {
    sControlStatus.healthPending = true;
    sControlStatus.healthLastError = queued ? G2_RING_ERR_NONE
                                            : G2_RING_ERR_QUEUE_FULL;
    sControlStatus.healthTransaction = handle;
  } else {
    sControlStatus.lowPowerPending = true;
    sControlStatus.lowPowerLastError = queued ? G2_RING_ERR_NONE
                                              : G2_RING_ERR_QUEUE_FULL;
    sControlStatus.lowPowerTransaction = handle;
  }
  portEXIT_CRITICAL(&sTransportMux);
  if (outHandle) *outHandle = handle;
  ringWakeOwner();
  return queued;
}

static bool ringControlCanQueueNow() {
  G2RingSetupState setup;
  portENTER_CRITICAL(&sTransportMux);
  setup = sControlStatus.setupState;
  portEXIT_CRITICAL(&sTransportMux);
  return sLinkOnline && setup == G2_RING_SETUP_READY;
}

bool g2RingSetHealthCollectionDesired(G2RingDesiredState desired,
                                      G2RingTransactionHandle* outHandle) {
  if (outHandle) *outHandle = G2RingTransactionHandle{};
  if (desired != G2_RING_PRESERVE && desired != G2_RING_OFF &&
      desired != G2_RING_ON) return false;
  setSetting(gSettings.ringHealthCollectionDesired, (int)desired);
  G2RingTransactionHandle current{};
  portENTER_CRITICAL(&sTransportMux);
  if (sControlStatus.healthPending) current = sControlStatus.healthTransaction;
  sControlStatus.healthDesired = desired;
  if (sR1Profile != R1_PROFILE_UNKNOWN &&
      !ringControlSupported(sR1Profile, RING_CONTROL_HEALTH, desired)) {
    ringSetControlUnsupportedLocked(RING_CONTROL_HEALTH);
    current = G2RingTransactionHandle{};
  } else {
    sControlStatus.healthPending = true;
    sControlStatus.healthLastError = G2_RING_ERR_NONE;
  }
  if (current.id == 0 && sControlStatus.healthPending) {
    sControlStatus.healthTransaction = G2RingTransactionHandle{};
  }
  portEXIT_CRITICAL(&sTransportMux);
  if (outHandle) *outHandle = current;
  ringWakeOwner();
  return true;  // accepted; the owner collapses rapid changes to the latest
}

bool g2RingSetLowPowerDesired(G2RingDesiredState desired,
                              G2RingTransactionHandle* outHandle) {
  if (outHandle) *outHandle = G2RingTransactionHandle{};
  if (desired != G2_RING_PRESERVE && desired != G2_RING_OFF &&
      desired != G2_RING_ON) return false;
  setSetting(gSettings.ringLowPowerDesired, (int)desired);
  G2RingTransactionHandle current{};
  portENTER_CRITICAL(&sTransportMux);
  if (sControlStatus.lowPowerPending) current = sControlStatus.lowPowerTransaction;
  sControlStatus.lowPowerDesired = desired;
  if (sR1Profile != R1_PROFILE_UNKNOWN &&
      !ringControlSupported(sR1Profile, RING_CONTROL_LOW_POWER, desired)) {
    ringSetControlUnsupportedLocked(RING_CONTROL_LOW_POWER);
    current = G2RingTransactionHandle{};
  } else {
    sControlStatus.lowPowerPending = true;
    sControlStatus.lowPowerLastError = G2_RING_ERR_NONE;
  }
  if (current.id == 0 && sControlStatus.lowPowerPending) {
    sControlStatus.lowPowerTransaction = G2RingTransactionHandle{};
  }
  portEXIT_CRITICAL(&sTransportMux);
  if (outHandle) *outHandle = current;
  ringWakeOwner();
  return true;
}

bool g2RingRefreshControlStatus(G2RingTransactionHandle* outHealth,
                                G2RingTransactionHandle* outLowPower) {
  if (outHealth) *outHealth = G2RingTransactionHandle{};
  if (outLowPower) *outLowPower = G2RingTransactionHandle{};
  if (!ringControlCanQueueNow()) return false;
  // Health 0x0E has no proven GET; leave observed Unknown and do not probe.
  portENTER_CRITICAL(&sTransportMux);
  sControlStatus.healthObserved = G2_RING_OBS_UNKNOWN;
  sControlStatus.healthObservedAtMs = 0;
  portEXIT_CRITICAL(&sTransportMux);
  const bool lowPower = ringQueueControlIntent(RING_CONTROL_LOW_POWER,
                                                G2_RING_PRESERVE, outLowPower);
  return lowPower;
}

bool g2RingRequestHistoryRefresh(bool force) {
  if (!sLinkOnline || !gRing.connected) return false;
  portENTER_CRITICAL(&sTransportMux);
  if (!force && sControlStatus.healthDesired == G2_RING_OFF) {
    portEXIT_CRITICAL(&sTransportMux);
    return false;
  }
  if (sHistorySweepActive) {
    // One sweep already owns the fetch session. Treat this request as
    // satisfied by it; never leave a second force/normal request armed to
    // erase the terminal pending session before main-loop persistence.
    portEXIT_CRITICAL(&sTransportMux);
    return true;
  }
  sHistoryRefreshRequested = true;
  sHistoryRefreshForce = sHistoryRefreshForce || force;
  sHistoryRefreshGeneration = sLinkGeneration;
  portEXIT_CRITICAL(&sTransportMux);
  ringWakeOwner();
  return true;
}

bool g2RingHealthPageRefreshSupported(void) {
  return r1ProfileSupportsHealthPageRefresh(ringSnapshotProtocolProfile());
}

bool g2RingStartHealthPageRefresh(G2RingHealthRefreshHandle& out) {
  out = G2RingHealthRefreshHandle{};
  if (!gRing.connected) return false;
  const uint32_t now = millis();
  bool wakeOwner = false;
  portENTER_CRITICAL(&sTransportMux);
  if (!sLinkOnline || sLinkGeneration == 0 ||
      !r1ProfileSupportsHealthPageRefresh(sR1Profile)) {
    portEXIT_CRITICAL(&sTransportMux);
    return false;
  }
  if (sHealthPageRefresh.generation == sLinkGeneration &&
      sHealthPageRefresh.phase != RING_HEALTH_REFRESH_IDLE &&
      sHealthPageRefresh.phase != RING_HEALTH_REFRESH_TERMINAL) {
    // Repeated taps coalesce to the same exact composite request.
    out.id = sHealthPageRefresh.id;
    out.generation = sHealthPageRefresh.generation;
    portEXIT_CRITICAL(&sTransportMux);
    return true;
  }

  uint32_t id = sNextHealthPageRefreshId++;
  if (id == 0) id = sNextHealthPageRefreshId++;
  if (sHealthPageRefresh.id != 0) {
    RingHealthPageRefresh prior = sHealthPageRefresh;
    if (prior.phase != RING_HEALTH_REFRESH_TERMINAL) {
      prior.terminalSuccessful = false;
      prior.terminalPartial = prior.dailyVerifiedCount != 0 ||
                              prior.deviceStatusVerified;
      prior.terminalError = G2_RING_ERR_DISCONNECTED;
      prior.phase = RING_HEALTH_REFRESH_TERMINAL;
    }
    sHealthPageRefreshTerminalHistory[
        sHealthPageRefreshTerminalCursor] = prior;
    sHealthPageRefreshTerminalCursor =
        (uint8_t)((sHealthPageRefreshTerminalCursor + 1) % 2);
  }
  sHealthPageRefresh = RingHealthPageRefresh{};
  sHealthPageRefresh.id = id;
  sHealthPageRefresh.generation = sLinkGeneration;
  sHealthPageRefresh.dailyBaselineSequence =
      sHistorySweepCompletionSequence;
  sHealthPageRefresh.deadlineMs =
      now + RING_HEALTH_PAGE_REFRESH_TIMEOUT_MS;
  out.id = id;
  out.generation = sLinkGeneration;

  if (sHistorySweepActive || sHistorySweepClaimed ||
      sHistoryRefreshRequested) {
    // A tap never adopts a sweep whose earlier stages predate it. Wait for the
    // ordinary sweep to finish/skip, then queue this request's forced sweep.
    sHealthPageRefresh.phase = RING_HEALTH_REFRESH_WAIT_PRIOR;
  } else {
    sHealthPageRefresh.phase = RING_HEALTH_REFRESH_WAIT_DAILY;
    sHistoryRefreshRequested = true;
    sHistoryRefreshForce = true;
    sHistoryRefreshExclusive = true;
    sHistoryRefreshGeneration = sLinkGeneration;
    wakeOwner = true;
  }
  portEXIT_CRITICAL(&sTransportMux);

  // "Poll Now" is explicit user intent, so it bypasses the normal ten-minute
  // freshness throttle. The official 2.2.9 app uses this DAILY lane; POINT
  // and MEASURE remain absent from the composite.
  if (wakeOwner) ringWakeOwner();
  return true;
}

bool g2RingGetHealthPageRefreshStatus(
    const G2RingHealthRefreshHandle& handle,
    G2RingHealthRefreshStatus& out) {
  out = G2RingHealthRefreshStatus{};
  if (handle.id == 0 || handle.generation == 0) return false;

  portENTER_CRITICAL(&sTransportMux);
  if (sHealthPageRefresh.id != handle.id ||
      sHealthPageRefresh.generation != handle.generation) {
    for (uint8_t i = 0; i < 2; ++i) {
      const RingHealthPageRefresh& archived =
          sHealthPageRefreshTerminalHistory[i];
      if (archived.id == handle.id &&
          archived.generation == handle.generation &&
          archived.phase == RING_HEALTH_REFRESH_TERMINAL) {
        out.completed = true;
        out.successful = archived.terminalSuccessful;
        out.partial = archived.terminalPartial;
        out.errorCode = archived.terminalError;
        portEXIT_CRITICAL(&sTransportMux);
        return true;
      }
    }
    portEXIT_CRITICAL(&sTransportMux);
    return false;
  }
  if (sHealthPageRefresh.phase == RING_HEALTH_REFRESH_TERMINAL) {
    out.completed = true;
    out.successful = sHealthPageRefresh.terminalSuccessful;
    out.partial = sHealthPageRefresh.terminalPartial;
    out.errorCode = sHealthPageRefresh.terminalError;
    portEXIT_CRITICAL(&sTransportMux);
    return true;
  }
  if (sHealthPageRefresh.dailyCompleted &&
      sHealthPageRefresh.deviceStatusCompleted) {
    // Both correlated children are terminal; preserve their immutable result
    // if link-down publication wins the race with the coordinator's next lap.
    out.completed = true;
    out.successful = sHealthPageRefresh.dailySuccessful &&
                     sHealthPageRefresh.deviceStatusVerified;
    out.partial = !out.successful &&
        (sHealthPageRefresh.dailyVerifiedCount != 0 ||
         sHealthPageRefresh.deviceStatusVerified);
    out.errorCode = out.successful
        ? (uint8_t)G2_RING_ERR_NONE
        : ringHealthPageRefreshResolvedError(
              sHealthPageRefresh, G2_RING_ERR_TIMEOUT);
    portEXIT_CRITICAL(&sTransportMux);
    return true;
  }
  if (!sLinkOnline || sLinkGeneration != handle.generation) {
    out.completed = true;
    out.partial = sHealthPageRefresh.dailyVerifiedCount != 0 ||
                  sHealthPageRefresh.deviceStatusVerified;
    out.errorCode = G2_RING_ERR_DISCONNECTED;
    portEXIT_CRITICAL(&sTransportMux);
    return true;
  }
  out.pending = true;
  portEXIT_CRITICAL(&sTransportMux);
  return true;
}

// =============================================================================
// Telemetry cache — what we've decoded from ring notify frames so far
// =============================================================================
// The `point` queries return cached samples — the ring's auto-recording
// cadence updates them every few minutes, not on demand. So we mirror them
// into our own cache and let the spoof-push task forward whatever's fresh
// to the glasses periodically. Each metric tracks its own "is valid" flag
// so a partial cache (e.g. HR known but HRV not yet seen) translates into
// a partial proto frame that omits the missing fields.
struct R1TelemetryCache {
  uint8_t  hr;           uint32_t hrTs;        uint32_t hrRxMs;       bool hrValid;
  int16_t  hrv;          uint32_t hrvTs;       uint32_t hrvRxMs;      bool hrvValid;
  uint8_t  spo2;         uint32_t spo2Ts;      uint32_t spo2RxMs;     bool spo2Valid;
  int16_t  tempTenths;   uint32_t tempTs;      uint32_t tempRxMs;     bool tempValid;
  uint8_t  battery;                            uint32_t batteryRxMs;  bool batteryValid;
  uint8_t  wear;                               uint32_t wearRxMs;     bool wearValid;
};
static R1TelemetryCache gR1Cache;

enum RingTelemetrySource : uint8_t {
  RING_TELEMETRY_NONE = 0,
  RING_TELEMETRY_DIRECT,
  RING_TELEMETRY_FORWARDED,
};
static RingTelemetrySource sTelemetrySource = RING_TELEMETRY_NONE;

// A normal task mutex, rather than sTransportMux, is required here because a
// cache mutation also appends to G2_Health's mutex-protected live series.
// Holding this fence across both operations gives a new-link reset one atomic
// boundary: an old owner frame either finishes before the reset (and is then
// cleared), or observes the new generation after it (and is discarded).
static StaticSemaphore_t sTelemetryMutexStorage;
static SemaphoreHandle_t sTelemetryMutex = nullptr;
static portMUX_TYPE sTelemetryMutexInitMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t sTelemetryGeneration = 0;  // protected by sTelemetryMutex

static SemaphoreHandle_t ringTelemetryMutex() {
  portENTER_CRITICAL(&sTelemetryMutexInitMux);
  if (!sTelemetryMutex) {
    sTelemetryMutex = xSemaphoreCreateMutexStatic(&sTelemetryMutexStorage);
  }
  SemaphoreHandle_t mutex = sTelemetryMutex;
  portEXIT_CRITICAL(&sTelemetryMutexInitMux);
  return mutex;
}

class RingTelemetryGuard {
 public:
  RingTelemetryGuard() {
    SemaphoreHandle_t mutex = ringTelemetryMutex();
    locked_ = mutex && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE;
  }
  ~RingTelemetryGuard() {
    if (locked_) xSemaphoreGive(sTelemetryMutex);
  }
  bool locked() const { return locked_; }

 private:
  bool locked_ = false;
};

struct RingTelemetrySnapshot {
  R1TelemetryCache cache{};
  RingTelemetrySource source = RING_TELEMETRY_NONE;
  uint32_t telemetryGeneration = 0;
  uint32_t linkGeneration = 0;
  bool online = false;
  R1ProtocolProfile profile = R1_PROFILE_UNKNOWN;
};

using RingTelemetryInterleaveHook = void (*)();

// Keep the telemetry fence held until the link identity has been copied. A
// reconnect publishes its generation while holding this same fence before the
// transport critical section, so the returned cache and link identity cannot
// straddle two generations. Disconnect may still win at the transport lock;
// in that case `online` is false and the caller suppresses every valid bit.
static bool ringSnapshotTelemetry(
    RingTelemetrySnapshot& out,
    RingTelemetryInterleaveHook interleaveHook = nullptr) {
  RingTelemetryGuard telemetry;
  if (!telemetry.locked()) return false;
  out.cache = gR1Cache;
  out.source = sTelemetrySource;
  out.telemetryGeneration = sTelemetryGeneration;
  // Self-test seam: production callers pass null. It deterministically forces
  // the former cache-copy/reconnect/link-copy ordering while this function's
  // generation admission is exercised at boot.
  if (interleaveHook) interleaveHook();
  portENTER_CRITICAL(&sTransportMux);
  out.linkGeneration = sLinkGeneration;
  out.online = sLinkOnline;
  out.profile = sR1Profile;
  portEXIT_CRITICAL(&sTransportMux);
  return true;
}

static bool ringTelemetryDirectIdentityCurrent(
    const RingTelemetrySnapshot& snapshot) {
  return snapshot.online && snapshot.telemetryGeneration != 0 &&
         snapshot.telemetryGeneration == snapshot.linkGeneration;
}

// Caller holds sTelemetryMutex. Keep value-cache and live-series provenance
// identical across a direct/forwarded ownership change. The nested call takes
// only the G2_Health series mutex, preserving telemetry -> series lock order.
static void ringTransitionTelemetrySourceLocked(RingTelemetrySource source) {
  if (sTelemetrySource == source) return;
  gR1Cache = R1TelemetryCache{};
  g2HealthResetLiveTelemetry();
  sTelemetrySource = source;
}

// Caller holds sTelemetryMutex. Direct ownership wins while its transport is
// online; stale/replayed temple bridge traffic must not erase or mix with that
// cache or the live graph. Once offline, a source transition starts clean.
static bool ringPrepareForwardedTelemetryMutationLocked() {
  portENTER_CRITICAL(&sTransportMux);
  const bool directLinkOnline = sLinkOnline;
  portEXIT_CRITICAL(&sTransportMux);
  if (directLinkOnline) return false;
  ringTransitionTelemetrySourceLocked(RING_TELEMETRY_FORWARDED);
  return true;
}

// Freshest trusted ring timestamp accepted from a typed health sample. Failure
// states, zero-time POINT frames, and malformed/out-of-range payloads cannot
// move ring-clock custody. The telemetry mutex protects this pair together
// with the cache and its per-link reset.
static volatile uint32_t sRingTsSeen     = 0;
static volatile uint32_t sRingTsSeenRxMs = 0;

// Last wear enum we posted a bus event for (1=notWear, 2=wear). 0 = none yet
// this link. Unknown (0) samples do not clear this — avoids re-firing on
// intermittent unknowns. Reset on disconnect so the next session can edge.
static uint8_t sRingWearPosted = 0;

static bool ringResetTelemetryForNewLink() {
  // Publish a clean cache before the new link becomes visible. Point layouts
  // are profile-scoped, and retaining valid bits across a peer/profile change
  // could make a 2.2.9 session surface another session's 2.2.7 samples.
  uint32_t currentGeneration = 0;
  portENTER_CRITICAL(&sTransportMux);
  currentGeneration = sLinkGeneration;
  portEXIT_CRITICAL(&sTransportMux);

  RingTelemetryGuard telemetry;
  if (!telemetry.locked()) return false;
  // Fence the cache against a PROCESSING slab from the just-ended link before
  // ringStartGenerationAndSetup publishes the same next generation. Ordinary
  // disconnect does not drain that slab, so the generation check in the
  // extractor is required even though the reset itself is serialized.
  sTelemetryGeneration = ringNextLinkGeneration(currentGeneration);
  gR1Cache = R1TelemetryCache{};
  sTelemetrySource = RING_TELEMETRY_NONE;
  sRingWearPosted = 0;
  g2HealthResetLiveTelemetry();
  return true;
}

// Update wear cache + edge-fire ring_worn / ring_not_worn (G2 worn analogue).
// wear: 0=unknown (ignored for events), 1=notWear, 2=wear. Caller holds the
// telemetry mutex and has already proved the frame's link generation.
static void ringNoteWear(uint8_t wear, uint32_t rxMs) {
  if (wear > 2) return;
  gR1Cache.wear      = wear;
  gR1Cache.wearRxMs  = rxMs;
  gR1Cache.wearValid = true;  // sample received (0=unknown still counts)
  if (wear == 0 || wear == sRingWearPosted) return;

  sRingWearPosted = wear;
  const char* name =
      gRingDeviceName.length() > 0 ? gRingDeviceName.c_str() : "R1";
  if (wear == 2) {
    systemEventPost(SYSEVT_RING_WORN, name);
  } else {
    systemEventPost(SYSEVT_RING_NOT_WORN, name);
  }
}

// Age in seconds for UI/JSON. Prefer ring epoch when both clocks look
// synced (Clock::isValidEpoch); else millis since local receive. −1 = unknown.
static int32_t ringSampleAgeSec(uint32_t ringTs, uint32_t rxMs) {
  const time_t now = time(nullptr);
  if (Clock::isValidEpoch((time_t)ringTs) && Clock::isValidEpoch(now)) {
    int64_t a = (int64_t)now - (int64_t)ringTs;
    if (a < 0) a = 0;
    if (a > 0x7fffffffLL) a = 0x7fffffffLL;
    return (int32_t)a;
  }
  if (rxMs != 0) {
    return (int32_t)((millis() - rxMs) / 1000u);
  }
  return -1;
}

static void ringPopulateTelemetryOutput(
    const RingTelemetrySnapshot* snapshot, G2RingTelemetry& out) {
  out = G2RingTelemetry{};
  out.hrAgeSec = -1;
  out.hrvAgeSec = -1;
  out.spo2AgeSec = -1;
  out.tempAgeSec = -1;
  out.batteryAgeSec = -1;
  out.wearAgeSec = -1;
  if (!snapshot) return;

  const bool directIdentityCurrent =
      ringTelemetryDirectIdentityCurrent(*snapshot);
  out.connected = directIdentityCurrent;
  const bool directCacheCurrent =
      snapshot->source == RING_TELEMETRY_DIRECT && directIdentityCurrent;
  const bool forwardedCacheCurrent =
      snapshot->source == RING_TELEMETRY_FORWARDED && !snapshot->online;
  if (!directCacheCurrent && !forwardedCacheCurrent) return;

  const R1TelemetryCache& cache = snapshot->cache;
  const bool liveVitalsSupported =
      forwardedCacheCurrent ||
      (r1ProfileSupportsPointIngestion(snapshot->profile) &&
       r1ProfileSupportsPointMeasureQuery(snapshot->profile)) ||
      r1ProfileSupportsHealthPageRefresh(snapshot->profile);
  const bool wearDataSupported = forwardedCacheCurrent ||
      r1ProfileSupportsWearStatus(snapshot->profile) ||
      r1ProfileSupportsDeviceStatusIngestion(snapshot->profile);
  out.hr = cache.hr;
  out.hrValid = liveVitalsSupported && cache.hrValid;
  out.hrv = cache.hrv;
  out.hrvValid = liveVitalsSupported && cache.hrvValid;
  out.spo2 = cache.spo2;
  out.spo2Valid = liveVitalsSupported && cache.spo2Valid;
  out.tempTenths = cache.tempTenths;
  out.tempValid = liveVitalsSupported && cache.tempValid;
  out.battery = cache.battery;
  out.batteryValid = cache.batteryValid;
  out.wear = cache.wear;
  out.wearValid = wearDataSupported && cache.wearValid;
  out.hrAgeSec = out.hrValid
      ? ringSampleAgeSec(cache.hrTs, cache.hrRxMs) : -1;
  out.hrvAgeSec = out.hrvValid
      ? ringSampleAgeSec(cache.hrvTs, cache.hrvRxMs) : -1;
  out.spo2AgeSec = out.spo2Valid
      ? ringSampleAgeSec(cache.spo2Ts, cache.spo2RxMs) : -1;
  out.tempAgeSec = out.tempValid
      ? ringSampleAgeSec(cache.tempTs, cache.tempRxMs) : -1;
  out.batteryAgeSec = out.batteryValid
      ? ringSampleAgeSec(0, cache.batteryRxMs) : -1;
  out.wearAgeSec = out.wearValid
      ? ringSampleAgeSec(0, cache.wearRxMs) : -1;
  // Raw local receive stamps, copied through unprocessed — history consumers
  // need a monotonic axis that ring-clock custody can't step (see G2_Ring.h).
  out.hrRxMs = cache.hrRxMs;
  out.hrvRxMs = cache.hrvRxMs;
  out.spo2RxMs = cache.spo2RxMs;
  out.tempRxMs = cache.tempRxMs;
  out.batteryRxMs = cache.batteryRxMs;
}

static void ringTelemetryReconnectInterleaveSelfTestHook() {
  portENTER_CRITICAL(&sTransportMux);
  sLinkGeneration = ringNextLinkGeneration(sLinkGeneration);
  sLinkOnline = true;
  sR1Profile = R1_PROFILE_FW_2_2_9_0003;
  portEXIT_CRITICAL(&sTransportMux);
}

// Runs before callback registration or owner creation. The hook forces the
// exact former failure shape: an old direct-generation valid cache is copied,
// then a newly-online generation/profile is observed. Production admission
// must reject it. An offline direct cache must expose no valid data, while a
// source-identified sid-0x90 forwarded cache intentionally remains visible.
static bool ringTelemetrySnapshotSelfTest() {
  static const uint8_t kStaleReasmModel[] = {
    0x64, R1_MODULE_HEALTH, 0x64, 0x34, 0x12, 0x02,
    R1_CMD_ACTIVITY, R1_SUB_DAILY, 37, 0, 0, 0,
    2, 0xC4,0xFF, 0x10,0xC7,0x55,0x69,
    0,   100,0, 5,0, 10,0,
    143, 200,0, 20,0, 30,0,
    0x1F,0x28,0,0,
  };
  static_assert(sizeof(kStaleReasmModel) == 37,
                "stale reassembly self-test model size");
  R1TelemetryCache savedCache{};
  RingTelemetrySource savedSource = RING_TELEMETRY_NONE;
  RingReassembly savedReassembly = sRingReasm;
  uint8_t savedReassemblyBytes[sizeof(kStaleReasmModel)]{};
  memcpy(savedReassemblyBytes, sRingReasmBuf,
         sizeof(savedReassemblyBytes));
  const uint8_t savedPacketAckCount = sPacketAckCount;
  uint32_t savedTelemetryGeneration = 0;
  uint32_t savedLinkGeneration = 0;
  bool savedLinkOnline = false;
  R1ProtocolProfile savedProfile = R1_PROFILE_UNKNOWN;
  {
    RingTelemetryGuard telemetry;
    if (!telemetry.locked()) return false;
    savedCache = gR1Cache;
    savedSource = sTelemetrySource;
    savedTelemetryGeneration = sTelemetryGeneration;
    gR1Cache = R1TelemetryCache{};
    gR1Cache.hr = 73;
    gR1Cache.hrRxMs = 1234;
    gR1Cache.hrValid = true;
    gR1Cache.battery = 88;
    gR1Cache.batteryRxMs = 2345;
    gR1Cache.batteryValid = true;
    gR1Cache.wear = 2;
    gR1Cache.wearRxMs = 3456;
    gR1Cache.wearValid = true;
    sTelemetrySource = RING_TELEMETRY_DIRECT;
    sTelemetryGeneration = 41;
    portENTER_CRITICAL(&sTransportMux);
    savedLinkGeneration = sLinkGeneration;
    savedLinkOnline = sLinkOnline;
    savedProfile = sR1Profile;
    sLinkGeneration = 41;
    sLinkOnline = true;
    sR1Profile = R1_PROFILE_FW_2_2_7_0005;
    portEXIT_CRITICAL(&sTransportMux);
  }

  bool ok = true;
#if ENABLE_R1_HEALTH
  g2HealthResetLiveTelemetry();
  g2HealthNoteSample(HEALTH_METRIC_HR, 73, 0);
  ok &= g2HealthHistoryCount(HEALTH_METRIC_HR) == 1;
#endif
  {
    RingTelemetryGuard telemetry;
    if (!telemetry.locked()) {
      ok = false;
    } else {
      ok &= !ringPrepareForwardedTelemetryMutationLocked() &&
            sTelemetrySource == RING_TELEMETRY_DIRECT &&
            gR1Cache.hrValid && gR1Cache.hr == 73;
    }
  }
#if ENABLE_R1_HEALTH
  ok &= g2HealthHistoryCount(HEALTH_METRIC_HR) == 1;
#endif
  RingTelemetrySnapshot reconnected{};
  ok &= ringSnapshotTelemetry(
      reconnected, ringTelemetryReconnectInterleaveSelfTestHook);
  G2RingTelemetry output{};
  ringPopulateTelemetryOutput(&reconnected, output);
  ok &= reconnected.telemetryGeneration == 41 &&
        reconnected.linkGeneration == 42 && reconnected.online &&
        !output.connected && !output.hrValid && !output.hrvValid &&
        !output.spo2Valid && !output.tempValid && !output.batteryValid &&
        !output.wearValid && output.hr == 0 && output.battery == 0 &&
        output.wear == 0 && output.hrRxMs == 0 &&
        output.batteryRxMs == 0 && output.hrAgeSec == -1 &&
        output.batteryAgeSec == -1 && output.wearAgeSec == -1;

  {
    RingTelemetryGuard telemetry;
    if (!telemetry.locked()) {
      ok = false;
    } else {
      sTelemetryGeneration = 42;
      portENTER_CRITICAL(&sTransportMux);
      sLinkGeneration = 42;
      sLinkOnline = false;
      sR1Profile = R1_PROFILE_FW_2_2_7_0005;
      portEXIT_CRITICAL(&sTransportMux);
    }
  }
  RingTelemetrySnapshot disconnected{};
  ok &= ringSnapshotTelemetry(disconnected);
  ringPopulateTelemetryOutput(&disconnected, output);
  ok &= !output.connected && !output.hrValid && !output.batteryValid &&
        !output.wearValid && output.hrRxMs == 0 &&
        output.batteryRxMs == 0 && output.hrAgeSec == -1 &&
        output.batteryAgeSec == -1 && output.wearAgeSec == -1;

  // Model a fully received old activity page paused immediately before
  // finalize while disconnect and forwarded publication win on other tasks.
  memcpy(sRingReasmBuf, kStaleReasmModel, sizeof(kStaleReasmModel));
  sRingReasm.active = true;
  sRingReasm.crc32 = r1Crc32(kStaleReasmModel, sizeof(kStaleReasmModel));
  sRingReasm.nextCountdown = 0;
  sRingReasm.generation = 42;
  sRingReasm.len = sizeof(kStaleReasmModel);

  {
    RingTelemetryGuard telemetry;
    if (!telemetry.locked()) {
      ok = false;
    } else {
      ok &= ringPrepareForwardedTelemetryMutationLocked() &&
            sTelemetrySource == RING_TELEMETRY_FORWARDED &&
            !gR1Cache.hrValid && !gR1Cache.batteryValid;
      gR1Cache.hr = 73;
      gR1Cache.hrRxMs = 1234;
      gR1Cache.hrValid = true;
      gR1Cache.battery = 88;
      gR1Cache.batteryRxMs = 2345;
      gR1Cache.batteryValid = true;
    }
  }
#if ENABLE_R1_HEALTH
  ok &= g2HealthHistoryCount(HEALTH_METRIC_HR) == 0;
  g2HealthNoteSample(HEALTH_METRIC_HR, 74, 0);
  ok &= g2HealthHistoryCount(HEALTH_METRIC_HR) == 1;
#endif
  RingTelemetrySnapshot forwarded{};
  ok &= ringSnapshotTelemetry(forwarded);
  ringPopulateTelemetryOutput(&forwarded, output);
  ok &= !output.connected && output.hrValid && output.hr == 73 &&
        output.batteryValid && output.battery == 88 && !output.wearValid &&
        output.hrRxMs == 1234 && output.batteryRxMs == 2345;

  // The owner may already have admitted this direct frame before disconnect.
  // Once offline forwarded data owns the cache, the same old link generation
  // must not be sufficient to flip provenance or clear its live series.
  R1Decoded lateDirect{};
  lateDirect.module = R1_MODULE_HEALTH;
  lateDirect.cmd = R1_CMD_HEARTRATE;
  lateDirect.subCmd = R1_SUB_POINT;
  lateDirect.payloadLength = 9;
  lateDirect.payload[7] = 75;
  ringExtractTelemetryCache(lateDirect, 42);
  RingTelemetrySnapshot afterLateDirect{};
  ok &= ringSnapshotTelemetry(afterLateDirect);
  ringPopulateTelemetryOutput(&afterLateDirect, output);
  ok &= afterLateDirect.source == RING_TELEMETRY_FORWARDED &&
        !output.connected && output.hrValid && output.hr == 73 &&
        output.batteryValid && output.battery == 88;
#if ENABLE_R1_HEALTH
  ok &= g2HealthHistoryCount(HEALTH_METRIC_HR) == 1;
#endif

  const uint8_t ackCountBeforeStaleFinalize = sPacketAckCount;
  ringReasmFinalize(42);
  RingTelemetrySnapshot afterStaleFinalize{};
  ok &= ringSnapshotTelemetry(afterStaleFinalize);
  ringPopulateTelemetryOutput(&afterStaleFinalize, output);
  ok &= sPacketAckCount == ackCountBeforeStaleFinalize &&
        afterStaleFinalize.source == RING_TELEMETRY_FORWARDED &&
        !output.connected && output.hrValid && output.hr == 73 &&
        output.batteryValid && output.battery == 88;
#if ENABLE_R1_HEALTH
  ok &= g2HealthHistoryCount(HEALTH_METRIC_HR) == 1;
#endif

  {
    RingTelemetryGuard telemetry;
    if (!telemetry.locked()) {
      ok = false;
    } else {
      ringTransitionTelemetrySourceLocked(RING_TELEMETRY_DIRECT);
      ok &= sTelemetrySource == RING_TELEMETRY_DIRECT &&
            !gR1Cache.hrValid && !gR1Cache.batteryValid;
    }
  }
#if ENABLE_R1_HEALTH
  ok &= g2HealthHistoryCount(HEALTH_METRIC_HR) == 0;
#endif

  {
    RingTelemetryGuard telemetry;
    if (!telemetry.locked()) {
      ok = false;
    } else {
      gR1Cache = savedCache;
      sTelemetrySource = savedSource;
      sTelemetryGeneration = savedTelemetryGeneration;
      portENTER_CRITICAL(&sTransportMux);
      sLinkGeneration = savedLinkGeneration;
      sLinkOnline = savedLinkOnline;
      sR1Profile = savedProfile;
      portEXIT_CRITICAL(&sTransportMux);
    }
  }
  sRingReasm = savedReassembly;
  memcpy(sRingReasmBuf, savedReassemblyBytes,
         sizeof(savedReassemblyBytes));
  sPacketAckCount = savedPacketAckCount;
  DEBUG_RING_LIFECYCLEF("[RING] telemetry snapshot self-test: %s",
                        ok ? "PASS" : "FAIL");
  return ok;
}

// =============================================================================
// Ring-clock custody
// =============================================================================
// The ring keeps its own battery-backed clock and survives our reboots, so
// after a dark boot (no RTC, no WiFi/NTP) it is often the only component in
// reach that still knows the real time. Custody, not origination: the ring
// only ever holds what some synced host once pushed into it — but that is
// exactly what a DS3231 with a coin cell is, too. Two rules keep the chain
// alive:
//   1. NEVER overwrite a GOOD ring clock with a dark epoch. The connect
//      ritual's systemTime frame is mandatory (RE'd sequence — frames are
//      not dropped from it), so when the host is dark the TIME stage holds
//      while the setup owner itself solicits a profile-safe HR response (the ring
//      volunteers no timestamps unpolled, and the tick's own solicit is
//      gated on setup being finished). A plausible stamp is adopted by
//      g2RingTimeSyncTick() and echoed back. The legacy POINT layout can also
//      prove a pre-2020 ring clock and safely complete with the host's dark
//      epoch. The 2.2.9 DAILY layout has no capture-proven unanchored-clock
//      interpretation, so a factory-dark 2.2.9 ring remains fail-closed with
//      clock-unavailable instead of guessing.
//   2. Whenever the host clock disagrees with what the ring was last told
//      by more than 2 minutes, send a corrective systemTime. This covers
//      the dark push corrected by NTP, an adoption-echo (which carries the
//      ring's last MEASUREMENT time, stale by minutes) trued up by NTP,
//      and a manual timeset — one drift rule instead of one-shot flags.
// sRingSetupDone gates the corrective push so the tick can never inject a
// frame into the middle of the setup ritual.
static volatile bool sRingSetupDone        = false;  // standard setup finished this link
static volatile uint32_t sLastPushedEpoch  = 0;      // epoch of the last systemTime SET (0 = none yet)
static volatile uint32_t sLastPushedAtMs   = 0;      // millis() when that SET was written
static volatile int16_t  sLastPushedTzMin  = INT16_MIN;  // tz (minutes) last SET (sentinel = none)
static volatile uint8_t sDarkProbesSent    = 0;      // post-setup profile-safe HR solicits (cap 3)
// Adoption is once per boot. Without this, a user who deliberately sets a
// pre-2020 clock (`timeset 1999-…`, which leaves Clock::isSynced() false)
// would have it silently overwritten by the ring within one 500 ms tick.
static bool sAdoptedThisBoot = false;

// Drop every per-link custody fact. Called from both teardown paths
// (ringClearGattPointers) and again at the top of the setup ritual, so a
// stamp, a push record, or a probe budget from a PREVIOUS link can never be
// mistaken for this one's. Deliberately does NOT clear sAdoptedThisBoot —
// that latch is per boot, not per link.
static void ringClockCustodyReset() {
  RingTelemetryGuard telemetry;
  if (!telemetry.locked()) return;
  sRingSetupDone   = false;
  sLastPushedEpoch = 0;
  sLastPushedAtMs  = 0;
  sLastPushedTzMin = INT16_MIN;
  sDarkProbesSent  = 0;
  sRingTsSeen      = 0;
  sRingTsSeenRxMs  = 0;
}

// Freshest ring-stamped epoch across accepted point/daily samples, adjusted by
// time-since-receive so it reads as "now". 0 when no cached sample carries a
// plausible date — a factory-fresh or fully-drained ring reports ~1970 and
// must never be adopted. Upper bound mirrors rtcEarlyBootSync's 2099 cap.
// Longest receive-age we will age-adjust across. A stamp whose rxMs is older
// than this is not evidence of "now" any more (and a zero/stale rxMs would
// project by the whole uptime), so we refuse rather than guess.
static constexpr uint32_t kRingTsMaxAgeMs = 30u * 60u * 1000u;  // 30 min

static time_t ringBestKnownEpoch(void) {
  uint32_t ts = 0, rx = 0;
  {
    RingTelemetryGuard telemetry;
    if (!telemetry.locked()) return 0;
    if (sRingTsSeen != 0) {
      ts = sRingTsSeen;
      rx = sRingTsSeenRxMs;
    }
    if (gR1Cache.hrValid   && gR1Cache.hrTs   > ts) { ts = gR1Cache.hrTs;   rx = gR1Cache.hrRxMs;   }
    if (gR1Cache.hrvValid  && gR1Cache.hrvTs  > ts) { ts = gR1Cache.hrvTs;  rx = gR1Cache.hrvRxMs;  }
    if (gR1Cache.spo2Valid && gR1Cache.spo2Ts > ts) { ts = gR1Cache.spo2Ts; rx = gR1Cache.spo2RxMs; }
    if (gR1Cache.tempValid && gR1Cache.tempTs > ts) { ts = gR1Cache.tempTs; rx = gR1Cache.tempRxMs; }
  }
  if (!Clock::isPlausibleEpoch((time_t)ts)) return 0;
  if (rx == 0) return 0;  // never received locally — cannot age-adjust it

  const uint32_t ageMs = (uint32_t)millis() - rx;   // unsigned: wrap-safe
  if (ageMs > kRingTsMaxAgeMs) return 0;            // too stale to call "now"

  const time_t projected = (time_t)ts + (time_t)(ageMs / 1000u);
  // Bound the RESULT too, not just the raw stamp: the age adjustment is what
  // could push a sane-looking stamp past the ceiling.
  if (!Clock::isPlausibleEpoch(projected)) return 0;
  return projected;
}

// Adopt the ring's clock when the host has no time source of its own.
// MAIN-LOOP CONTEXT ONLY: the clock-step chores this triggers include
// users.json I/O — too heavy for the ring owner or the connect
// worker's small stack, which is why setup WAITS on this instead of
// calling it directly. All post-step duties (TIME_SYNCED event, boot
// anchor, pending-user resolve, scheduler wake, RTC write-back) flow
// through Clock::clockStepped(); the tick that calls us drains them on
// the next lap via Clock::clockDutiesTick().
static bool ringAdoptClockIfDark(void) {
  if (sAdoptedThisBoot) return false;  // once per boot; see the latch's note
  if (Clock::isSynced()) return false;
  const time_t adopted = ringBestKnownEpoch();
  if (adopted == 0) return false;
  sAdoptedThisBoot = true;

  const time_t before = time(nullptr);  // pre-step, for the duty helper
  struct timeval tv = { .tv_sec = adopted, .tv_usec = 0 };
  settimeofday(&tv, nullptr);

  char tsStr[24] = "";
  struct tm tmNow;
  if (localtime_r(&adopted, &tmNow)) {
    strftime(tsStr, sizeof(tsStr), "%Y-%m-%d %H:%M", &tmNow);
  }
  INFO_RINGF("Adopted ring clock: %s (no local time source)", tsStr);

  Clock::clockStepped(Clock::SYNC_RING, before);
  return true;
}

// Main-loop tick (called next to timeAnchorsTick). Two duties: dark-clock
// adoption from the ring cache, and the one-shot corrective systemTime push
// once the clock is valid and the setup ritual has finished. Both branches
// are cheap gated no-ops in the steady state.
void g2RingTimeSyncTick(void) {
  static uint32_t sLastMs = 0;
  if (!everyMs(&sLastMs, 500)) return;

  ringAdoptClockIfDark();

  // Post-setup solicit: still dark with a live link — use the exact profile's
  // supported HR request (2.2.9 DAILY, legacy POINT). Capped per link: if the
  // ring's own clock is dark too, re-asking won't help. A factory-dark 2.2.9
  // DAILY response remains deliberately insufficient to prove its epoch.
  if (!Clock::isSynced() && gRing.connected && sRingSetupDone &&
      sDarkProbesSent < 3) {
    static uint32_t sProbeMs = 0;
    if (!everyMs(&sProbeMs, 5000)) return;
    const R1ProtocolProfile profile = ringSnapshotProtocolProfile();
    const bool queued = profile == R1_PROFILE_FW_2_2_9_0003
                            ? g2RingQueryDaily(R1_CMD_HEARTRATE)
                            : g2RingPollVital(0);
    if (queued) {
      sDarkProbesSent = (uint8_t)(sDarkProbesSent + 1);
      DEBUG_RING_SETUPF("[RING] dark-clock: queued hr/%s solicit %u/3",
                profile == R1_PROFILE_FW_2_2_9_0003 ? "daily" : "point",
                (unsigned)sDarkProbesSent);
    }
  }

  // Corrective push: our clock is valid and disagrees with the projection
  // of what the ring was last told by >2 min — covers the dark push trued
  // by NTP, a stale adoption-echo (ring's last MEASUREMENT time) trued by
  // NTP, and a manual timeset. Self-quenching: drift ≈ 0 after each push.
  if (gRing.connected && sRingSetupDone && Clock::isSynced()) {
    const time_t now = time(nullptr);
    const uint32_t pushedEpoch = sLastPushedEpoch;
    const uint32_t pushedAtMs  = sLastPushedAtMs;
    const int64_t expected = (int64_t)pushedEpoch +
        (int64_t)(((uint32_t)millis() - pushedAtMs) / 1000u);
    int64_t drift = (int64_t)now - expected;
    if (drift < 0) drift = -drift;
    // Also re-push when the configured timezone changed since the ring was last
    // told: the R1 keys its local daily pages off this offset, and a pure tz
    // edit never moves the epoch, so the drift test alone can't catch it.
    int16_t tzMin = 0;
    const bool tzChanged =
        ringConfiguredTimezoneMinutes(tzMin) && tzMin != sLastPushedTzMin;
    if (pushedEpoch == 0 || drift > 120 || tzChanged) {
      static G2RingTransactionHandle pending{};
      if (pending.id != 0) {
        G2RingTransactionStatus status{};
        if (g2RingGetTransactionStatus(pending, status) &&
            status.completedAtMs == 0) {
          return;
        }
        pending = G2RingTransactionHandle{};
      }
      if (ringEnqueueTimeSync((uint32_t)now, &pending)) {
        DEBUG_RING_SETUPF("[RING] queued corrective systemTime epoch=%lu drift=%llds "
                  "tz=%d->%d tx=%lu",
                  (unsigned long)now,
                  (long long)(pushedEpoch == 0 ? 0 : drift),
                  (int)sLastPushedTzMin, (int)tzMin,
                  (unsigned long)pending.id);
      }
    }
  }
}

// Spoof-push task state. The task wakes every `gSpoofIntervalSec` seconds,
// polls the ring for fresh point samples, and synthesises a sid=0x90
// RingDataPackage frame to the glasses so their UI displays ring telemetry
// as if the official bridge were active.
static volatile bool   gSpoofEnabled       = false;
static uint32_t        gSpoofIntervalSec   = 30;
static TaskHandle_t    gSpoofTaskHandle    = nullptr;

// Per-family in-flight flag — set by the public g2RingConnect* wrappers
// before submitting to the unified BLE-connect worker (see G2_Glasses.h
// `g2SubmitBleConnect`), cleared by the worker's dispatch (via the
// g2RingConnectMarkComplete helper below, since the flag is file-static
// and the worker lives in a different TU). Producers check this to reject
// duplicate submissions. Group B retired the per-call task spawn; the
// handle that used to track each transient task is gone.
static volatile bool  gRingConnectTaskActive = false;
// Stamped whenever the flag flips true, so reject paths can report how long
// the in-flight attempt has been running — a normal saved-MAC attempt tops
// out around 53s (20s glasses wait + 3s settle + ~30s connect timeout);
// anything much past that means the worker is stuck.
static volatile uint32_t gRingConnectTaskSinceMs = 0;
static portMUX_TYPE gRingConnectMux = portMUX_INITIALIZER_UNLOCKED;
// Every Ring central job carries the generation it was admitted under.
// Explicit disconnect/stack teardown advances it before touching the link,
// fencing queued and in-flight manual jobs as well as scheduler requests.
static uint32_t gRingConnectCancelGeneration = 1;
static bool gRingDisconnecting = false;

static uint32_t ringConnectCancelGenerationSnapshot() {
  portENTER_CRITICAL(&gRingConnectMux);
  const uint32_t generation = gRingConnectCancelGeneration;
  portEXIT_CRITICAL(&gRingConnectMux);
  return generation;
}

static bool ringConnectGenerationCurrent(uint32_t expected) {
  if (expected == 0) return false;
  portENTER_CRITICAL(&gRingConnectMux);
  const bool current = expected == gRingConnectCancelGeneration;
  portEXIT_CRITICAL(&gRingConnectMux);
  return current;
}

// Caller holds RingCompletionGuard. Keep the task-level mutex outside this
// brief spinlock so cancel generation, scheduler commit, and event publication
// are one ordered transaction.
static void ringConnectCancelAdvanceLocked() {
  portENTER_CRITICAL(&gRingConnectMux);
  ++gRingConnectCancelGeneration;
  if (gRingConnectCancelGeneration == 0) gRingConnectCancelGeneration = 1;
  portEXIT_CRITICAL(&gRingConnectMux);
}

static void ringCopyAddressText(
    char (&dst)[BLE_PEER_ADDRESS_TEXT_CAPACITY], const char* src) {
  memset(dst, 0, sizeof(dst));
  if (src) strncpy(dst, src, sizeof(dst) - 1);
}

static void ringRequestToJob(const BlePeerConnectRequest& request,
                             BleConnectJob& job) {
  job.peerIntentGeneration = request.intentGeneration;
  job.peerIdentityGeneration = request.identityGeneration;
  job.peerAutoReconnect = request.autoReconnect;
  job.peerExplicitReseek = request.explicitReseek;
  ringCopyAddressText(job.mac, request.savedTarget.mac1);
  ringCopyAddressText(job.mac2, request.savedTarget.mac2);
  job.addressType1 = request.savedTarget.addressType1;
  job.addressType2 = request.savedTarget.addressType2;
  job.addressType1Known = request.savedTarget.addressType1Known;
  job.addressType2Known = request.savedTarget.addressType2Known;
}

void g2RingConnectMarkComplete() {
  portENTER_CRITICAL(&gRingConnectMux);
  gRingConnectTaskActive = false;
  portEXIT_CRITICAL(&gRingConnectMux);
}
bool g2RingConnectInFlight() {
  portENTER_CRITICAL(&gRingConnectMux);
  const bool active = gRingConnectTaskActive;
  portEXIT_CRITICAL(&gRingConnectMux);
  return active;
}

// Failed-connect visibility. Every failure broadcasts on regular output, but
// the persisted bus event is throttled to the first failure of a streak plus
// one per 10 min — an unreachable ring retries every ≤180s and would
// otherwise flood events.log. Streak resets on successful connect.
static uint8_t  sRingConnFailStreak    = 0;
static uint32_t sRingConnFailLastEvtMs = 0;

static void ringNoteConnectFailure(const char* reason, uint32_t elapsedMs) {
  if (sRingConnFailStreak < 255) sRingConnFailStreak++;
  const uint32_t now = millis();
  if (sRingConnFailStreak > 1 && (now - sRingConnFailLastEvtMs) < 600000UL) return;
  sRingConnFailLastEvtMs = now;
  char detail[48];
  snprintf(detail, sizeof(detail), "%s fail#%u %lus", reason,
           (unsigned)sRingConnFailStreak, (unsigned long)(elapsedMs / 1000));
  systemEventPost(SYSEVT_RING_RECONNECT_FAILED,
                  gRingDeviceName.length() > 0 ? gRingDeviceName.c_str() : "R1",
                  detail);
}

// Gate shared by the three public connect entry points. Returns true when
// clear to submit. It never clears an in-flight flag beneath a live job: a
// duplicate create-connection is exactly the failure this coordinator exists
// to prevent. 360s covers the public 300s manual scan plus scheduling jitter;
// an overdue job is reported as reboot-required if it cannot finish safely.
static bool ringConnectGateOpen(uint32_t* generationOut) {
  if (generationOut) *generationOut = 0;
  if (!isG2ClientInitialized() || isBleServerInitialized() ||
      bleRoleTransitionState() != BleRoleTransition::IDLE) {
    DEBUG_RING_LIFECYCLEF("[RING] Connect deferred: G2 central role is not ready");
    return false;
  }
  portENTER_CRITICAL(&gRingConnectMux);
  if (!gRingConnectTaskActive && !gRingDisconnecting) {
    gRingConnectTaskActive = true;
    gRingConnectTaskSinceMs = millis();
    if (generationOut) *generationOut = gRingConnectCancelGeneration;
    portEXIT_CRITICAL(&gRingConnectMux);
    return true;
  }
  const uint32_t since = gRingConnectTaskSinceMs;
  portEXIT_CRITICAL(&gRingConnectMux);
  const uint32_t inflightS = (millis() - since) / 1000;
  if (inflightS > 360) {
    WARN_RINGF("WATCHDOG: central job stuck for %lus — "
               "duplicate admission blocked; reboot may be required",
               (unsigned long)inflightS);
    char d[40];
    snprintf(d, sizeof(d), "watchdog blocked after %lus",
             (unsigned long)inflightS);
    systemEventPost(SYSEVT_RING_RECONNECT_FAILED,
                    gRingDeviceName.length() > 0 ? gRingDeviceName.c_str() : "R1",
                    d);
    return false;
  }
  WARN_RINGF("Connect skipped — attempt already in flight (%lus)",
             (unsigned long)inflightS);
  if (inflightS > 120) ringNoteConnectFailure("stuck", inflightS * 1000);
  return false;
}

// Push a `ring-status` SSE on every meaningful transition: connect-ok,
// disconnect, scan-found. Compact keys because the SSE queue's data field
// is capped at 128 chars (EVENT_DATA_MAX in WebServer_Server.h) — the G2
// payload hits ~90 bytes, so we match that shape.
//
// The Bluetooth web page does NOT listen to this event — it polls
// /api/ble/status instead (see "Why not pure SSE" in WebPage_Bluetooth.h),
// so this event's only consumers are external SSE subscribers.
//
// Schema:
//   u  = up     (bool)  — BLE link live
//   n  = name   (str)   — advert name ("EVEN R1_XXXXXX")
//   a  = addr   (str)   — MAC
//   m  = mtu    (int)
//   rx = rx     (int)   — cumulative packet count
//   s  = scan   (str)   — "found" | "not-found"
//   w  = reason (str)   — short tag for the transition
static void ringPushStatusEvent(const char* reason) {
  // Length-bounded + escape-safe via CompactJson. See BLE_Events.h.
  char buf[128];
  CompactJson j(buf, sizeof(buf));
  j.kv("u",  (bool)gRing.connected)
   .kv("n",  gRingDeviceName.length()    ? gRingDeviceName.c_str()    : "")
   .kv("a",  gRingDeviceAddress.length() ? gRingDeviceAddress.c_str() : "")
   .kv("m",  (unsigned)gRing.mtu)
   .kv("rx", (unsigned long)gRing.packetsReceived)
   .kv("s",  gRingScanFound ? "found" : "not-found")
   .kv("w",  reason ? reason : "");
  blePushEvent("ring-status", j);
}

struct RingDownTransition {
  bool wasConnected = false;
  bool downEventPublished = false;
};

// Completion mutex is outermost; no BLE method or TX/write lock is used here.
// Callers may physically disconnect and clear GATT pointers only after return.
static RingDownTransition ringBeginLogicalDown(
    const char* reason, bool userInitiated = false) {
  RingDownTransition transition;
  RingCompletionGuard completion;
  if (!completion) return transition;

  if (userInitiated) blePeerNoteUserDisconnect(BLE_PEER_R1_RING);
  ringConnectCancelAdvanceLocked();
  transition.wasConnected = gRing.connected;
  ringTransportDisconnected();
  gRing.connected = false;
  transition.downEventPublished = gRingUpEventPublished;
  gRingUpEventPublished = false;
  if (transition.downEventPublished) {
    WARN_RINGF("Dropped BLE link — ring is no longer connected");
    ringPushStatusEvent(reason ? reason : "disconnect");
    systemEventPost(
        SYSEVT_RING_DISCONNECTED,
        gRingDeviceName.length() > 0 ? gRingDeviceName.c_str() : "R1",
        gRingDeviceAddress.length() > 0 ? gRingDeviceAddress.c_str() : nullptr);
    sRingWearPosted = 0;
    blePeerNoteLinkLost(BLE_PEER_R1_RING);
  } else if (transition.wasConnected) {
    // Setup may have made the transport usable before final UP publication.
    // Surface the live status change, but never emit a durable DOWN without a
    // corresponding published UP.
    ringPushStatusEvent(reason ? reason : "disconnect");
  }
  return transition;
}

// =============================================================================
// Telemetry cache extraction
// =============================================================================
// Pull HR / HRV / SpO2 from health/{cmd}/point notifies, and the (likely)
// battery byte from system/system/{deviceStatus,heartbeatPack}. Mirrors the
// hypothesis encoded in r1AnnotatePayload(): for HR the BPM is extra_value
// (LE i16 at offset 7..8 when payloadLength >= 9). For HRV/SpO2 we follow
// the same slot — confidence is lower but it's the best guess we have until
// captures show otherwise. Temperature uses `value` (offset 0..1) per the
// existing annotator comment, but we don't push temp through the spoof yet.
//
// Each metric only updates the cache when the response is the expected
// shape (length / opcode), so a malformed/refused frame can't poison the
// cache with garbage. Caller (the spoof task) checks each `*Valid` flag
// before serialising — partial cache → partial proto frame.
static bool ringDeviceStatusPayloadValid(const R1Decoded& d);

static bool ringHealthTimestampPlausible(uint32_t timestamp) {
  if (!Clock::isPlausibleEpoch((time_t)timestamp)) return false;
  const time_t now = time(nullptr);
  // Historical samples are expected; a sample more than a day in the future
  // is not. Skip the relative check until the host has an authoritative sync;
  // a merely plausible stale RTC value must not veto a good ring clock.
  return !Clock::isSynced() || !Clock::isValidEpoch(now) ||
         (int64_t)timestamp <= (int64_t)now + 24 * 60 * 60;
}

static void ringCacheDailyLatest(const R1CommonDailyResult& daily) {
  if (!r1ProfileSupportsHealthPageRefresh(daily.profile) ||
      !ringHealthTimestampPlausible(daily.latestTimestamp)) {
    return;
  }
  const uint32_t rx = millis();
  if (daily.metric == R1_DAILY_METRIC_HEART_RATE) {
    if (daily.latestValue == 0 || daily.latestValue >= 250 ||
        (gR1Cache.hrValid && gR1Cache.hrTs != 0 &&
         daily.latestTimestamp <= gR1Cache.hrTs)) {
      return;
    }
    gR1Cache.hr = daily.latestValue;
    gR1Cache.hrTs = daily.latestTimestamp;
    gR1Cache.hrRxMs = rx;
    gR1Cache.hrValid = true;
    g2HealthNoteSample(HEALTH_METRIC_HR, daily.latestValue,
                       daily.latestTimestamp);
  } else if (daily.metric == R1_DAILY_METRIC_SPO2) {
    if (daily.latestValue < 70 || daily.latestValue > 100 ||
        (gR1Cache.spo2Valid && gR1Cache.spo2Ts != 0 &&
         daily.latestTimestamp <= gR1Cache.spo2Ts)) {
      return;
    }
    gR1Cache.spo2 = daily.latestValue;
    gR1Cache.spo2Ts = daily.latestTimestamp;
    gR1Cache.spo2RxMs = rx;
    gR1Cache.spo2Valid = true;
    g2HealthNoteSample(HEALTH_METRIC_SPO2, daily.latestValue,
                       daily.latestTimestamp);
  } else {
    return;
  }
  // Publish receive time first: sRingTsSeen is the pair's validity flag.
  if (daily.latestTimestamp > sRingTsSeen) {
    sRingTsSeenRxMs = rx;
    sRingTsSeen = daily.latestTimestamp;
  }
}

static void ringCacheDailyLatest(const R1HrvDailyResult& daily) {
  if (!r1ProfileSupportsHealthPageRefresh(daily.profile) ||
      !ringHealthTimestampPlausible(daily.latestTimestamp) ||
      daily.latestValue == 0 ||
      daily.latestValue >= 1000 ||
      (gR1Cache.hrvValid && gR1Cache.hrvTs != 0 &&
       daily.latestTimestamp <= gR1Cache.hrvTs)) {
    return;
  }
  const uint32_t rx = millis();
  gR1Cache.hrv = static_cast<int16_t>(daily.latestValue);
  gR1Cache.hrvTs = daily.latestTimestamp;
  gR1Cache.hrvRxMs = rx;
  gR1Cache.hrvValid = true;
  g2HealthNoteSample(HEALTH_METRIC_HRV,
                     static_cast<int16_t>(daily.latestValue),
                     daily.latestTimestamp);
  if (daily.latestTimestamp > sRingTsSeen) {
    sRingTsSeenRxMs = rx;
    sRingTsSeen = daily.latestTimestamp;
  }
}

static void ringExtractTelemetryCache(const R1Decoded& d,
                                      uint32_t generation) {
  RingTelemetryGuard telemetry;
  if (!telemetry.locked()) return;
  uint32_t linkGeneration = 0;
  bool linkOnline = false;
  R1ProtocolProfile profile = R1_PROFILE_UNKNOWN;
  portENTER_CRITICAL(&sTransportMux);
  linkGeneration = sLinkGeneration;
  linkOnline = sLinkOnline;
  profile = sR1Profile;
  portEXIT_CRITICAL(&sTransportMux);
  if (!linkOnline || generation == 0 ||
      generation != sTelemetryGeneration || generation != linkGeneration) {
    DEBUG_RING_HEALTHF(
        "[RING] stale telemetry ignored frame-gen=%lu cache-gen=%lu "
        "link-gen=%lu online=%d",
        (unsigned long)generation, (unsigned long)sTelemetryGeneration,
        (unsigned long)linkGeneration, (int)linkOnline);
    return;
  }
  // health/{hr,hrv,spo2,temp}/point. The installed 2.2.9 app retains this
  // consumer but emitted no POINT request or sample in either new-firmware
  // capture, so 2.2.9 acceptance of the legacy-shaped 7/8/9-byte layouts is
  // provisional and may never be used to justify an outbound 2.2.9 query.
  if (d.module == R1_MODULE_HEALTH && d.subCmd == R1_SUB_POINT) {
    const bool supportedCmd = d.cmd == R1_CMD_HEARTRATE ||
        d.cmd == R1_CMD_HRV || d.cmd == R1_CMD_SPO2 ||
        d.cmd == R1_CMD_TEMPERATURE;
    if (!r1ProfileSupportsPointIngestion(profile) || !supportedCmd ||
        (d.payloadLength != 7 && d.payloadLength != 8 &&
         d.payloadLength != 9) ||
        d.statusMethod != R1_STATUS_METHOD_SET ||
        d.statusAck != R1_STATUS_ACK_OK ||
        (d.statusType != R1_STATUS_TYPE_NOTIFY &&
         d.statusType != R1_STATUS_TYPE_ACK)) {
      return;
    }
    const uint8_t* p = d.payload;
    // App enum: 0=fail, 1=success, 2=notWear, 3=notStationary, 4=timeout.
    const bool strictPassive229 =
        profile == R1_PROFILE_FW_2_2_9_0003;
    const int16_t value =
        (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    const uint32_t ts = (uint32_t)p[2] | ((uint32_t)p[3] << 8) |
                        ((uint32_t)p[4] << 16) |
                        ((uint32_t)p[5] << 24);
    if (!strictPassive229 && ts > sRingTsSeen) {
      // Preserve legacy dark-clock custody: a structurally trusted 2.2.7
      // response may donate its clock even when state says not-worn/failure.
      const uint32_t rx = millis();
      sRingTsSeenRxMs = rx;
      sRingTsSeen = ts;
    }
    // No failure/not-worn/timeout state may publish a vital value. The
    // unobserved 2.2.9 layout additionally requires a plausible resolved time.
    if (p[6] != 1 ||
        (strictPassive229 && !ringHealthTimestampPlausible(ts))) {
      return;
    }
    long extra = 0;
    bool hasExtra = false;
    if (d.payloadLength == 9) {
      extra = (long)(int16_t)((uint16_t)p[7] | ((uint16_t)p[8] << 8));
      hasExtra = true;
    } else if (d.payloadLength == 8) {
      extra = (long)p[7];
      hasExtra = true;
    }

    long candidate = value;
    bool candidateValid = false;
    switch (d.cmd) {
      case R1_CMD_HEARTRATE:
        candidate = extra;
        candidateValid = hasExtra && candidate > 0 && candidate < 250;
        break;
      case R1_CMD_HRV:
        candidate = hasExtra && extra > 0 && extra < 1000 ? extra : value;
        candidateValid = candidate > 0 && candidate < 1000;
        break;
      case R1_CMD_SPO2:
        candidate = hasExtra && extra >= 70 && extra <= 100 ? extra : value;
        candidateValid = candidate >= 70 && candidate <= 100;
        break;
      case R1_CMD_TEMPERATURE:
        candidateValid = candidate >= 150 && candidate <= 450;
        break;
      default:
        break;
    }
    if (!candidateValid) return;

    // A validated direct sample establishes direct-source ownership even when
    // a newer direct sample is already cached and wins the monotonic check.
    ringTransitionTelemetrySourceLocked(RING_TELEMETRY_DIRECT);
    const uint32_t rx = millis();
    if (ts > sRingTsSeen) {
      // Publish rxMs first: sRingTsSeen is the pair's validity flag.
      sRingTsSeenRxMs = rx;
      sRingTsSeen = ts;
    }
    switch (d.cmd) {
      case R1_CMD_HEARTRATE:
        if (!gR1Cache.hrValid || gR1Cache.hrTs == 0 || ts > gR1Cache.hrTs) {
          gR1Cache.hr = (uint8_t)candidate;
          gR1Cache.hrTs = ts;
          gR1Cache.hrRxMs = rx;
          gR1Cache.hrValid = true;
          g2HealthNoteSample(HEALTH_METRIC_HR, (int16_t)candidate, ts);
        }
        break;
      case R1_CMD_HRV:
        if (!gR1Cache.hrvValid || gR1Cache.hrvTs == 0 || ts > gR1Cache.hrvTs) {
          gR1Cache.hrv = (int16_t)candidate;
          gR1Cache.hrvTs = ts;
          gR1Cache.hrvRxMs = rx;
          gR1Cache.hrvValid = true;
          g2HealthNoteSample(HEALTH_METRIC_HRV, (int16_t)candidate, ts);
        }
        break;
      case R1_CMD_SPO2:
        if (!gR1Cache.spo2Valid || gR1Cache.spo2Ts == 0 ||
            ts > gR1Cache.spo2Ts) {
          gR1Cache.spo2 = (uint8_t)candidate;
          gR1Cache.spo2Ts = ts;
          gR1Cache.spo2RxMs = rx;
          gR1Cache.spo2Valid = true;
          g2HealthNoteSample(HEALTH_METRIC_SPO2, (int16_t)candidate, ts);
        }
        break;
      case R1_CMD_TEMPERATURE:
        if (!gR1Cache.tempValid || gR1Cache.tempTs == 0 ||
            ts > gR1Cache.tempTs) {
          gR1Cache.tempTenths = (int16_t)candidate;
          gR1Cache.tempTs = ts;
          gR1Cache.tempRxMs = rx;
          gR1Cache.tempValid = true;
          g2HealthNoteSample(HEALTH_METRIC_TEMP, (int16_t)candidate, ts);
        }
        break;
      default:
        break;
    }
    return;
  }

  // Never mix a sid-0x90 forwarded cache with accepted direct-link fields.
  // New-link reset normally leaves NONE here; this also closes the narrow
  // subscribe-to-setup window if a direct daily/status frame arrives first.
  if (d.module == R1_MODULE_SYSTEM && d.cmd == R1_CMD_SYSTEM &&
      d.subCmd == R1_SUB_DEVICE_STATUS &&
      (!r1ProfileSupportsDeviceStatusIngestion(profile) ||
       !ringDeviceStatusPayloadValid(d))) {
    return;
  }
  ringTransitionTelemetrySourceLocked(RING_TELEMETRY_DIRECT);

  // Capability-approved daily history → Trends + thin live backfill. The
  // protocol parser owns layout/range validation; Unknown profiles never
  // reach this path successfully.
  if (d.module == R1_MODULE_HEALTH && d.subCmd == R1_SUB_DAILY &&
      d.cmd == R1_CMD_ACTIVITY) {
    // Single-frame activity (<=35 slots). Larger days arrive fragmented and are
    // ingested by the reassembly finalize instead. Parse into the shared PSRAM
    // scratch to keep the full-day-sized result off this task's stack.
    if (r1ParseActivityDaily(profile, d, sRingActivityScratch) == R1_PARSE_OK) {
      (void)g2HealthApplyActivityDaily(sRingActivityScratch);
    }
    return;
  }
  if (d.module == R1_MODULE_HEALTH && d.subCmd == R1_SUB_DAILY &&
      (d.cmd == R1_CMD_HEARTRATE || d.cmd == R1_CMD_HRV ||
       d.cmd == R1_CMD_SPO2)) {
    uint8_t values[R1_COMMON_DAILY_MAX_RECORDS]{};
    uint8_t count = 0;
    uint32_t startTs = 0;
    uint32_t endTs = 0;
    G2HealthMetric metric = HEALTH_METRIC_HR;

    if (d.cmd == R1_CMD_HRV) {
      R1HrvDailyResult daily{};
      if (r1ParseHrvDaily(profile, d, daily) == R1_PARSE_OK) {
        ringCacheDailyLatest(daily);
        (void)g2HealthApplyHrvDaily(daily);
        if (daily.dayMode == R1_DAILY_DAY_EPOCH) {
          count = daily.count;
          startTs = count ? daily.records[0].bucketEpoch : daily.dayStart;
          endTs = daily.latestTimestamp;
          metric = HEALTH_METRIC_HRV;
          for (uint8_t i = 0;
               i < count && i < R1_COMMON_DAILY_MAX_RECORDS; ++i) {
            values[i] = daily.records[i].average > 255
                            ? 255
                            : (uint8_t)daily.records[i].average;
          }
        }
      }
    } else {
      R1CommonDailyResult daily{};
      if (r1ParseCommonDaily(profile, d, daily) == R1_PARSE_OK) {
        ringCacheDailyLatest(daily);
        (void)g2HealthApplyCommonDaily(daily);
        if (daily.dayMode == R1_DAILY_DAY_EPOCH) {
          count = daily.count;
          startTs = count ? daily.records[0].bucketEpoch : daily.dayStart;
          endTs = daily.latestTimestamp;
          metric = d.cmd == R1_CMD_SPO2 ? HEALTH_METRIC_SPO2
                                        : HEALTH_METRIC_HR;
          for (uint8_t i = 0; i < count; ++i)
            values[i] = daily.records[i].average;
        }
      }
    }

    if (count > 0) {
      if (endTs > sRingTsSeen) {
        sRingTsSeenRxMs = millis();
        sRingTsSeen = endTs;
      }
      g2HealthApplyTrendDaily(metric, values, count, startTs, endTs);
      g2HealthApplyDailyBackfill(metric, values, count, startTs, endTs);
    }
    return;
  }

  // system/system/{deviceStatus, heartbeatPack} — byte 0 is the (likely)
  // battery percent; byte 1 is wear (0/1/2). See annotateDeviceStatus.
  if (d.module == R1_MODULE_SYSTEM && d.cmd == R1_CMD_SYSTEM &&
      (d.subCmd == R1_SUB_DEVICE_STATUS || d.subCmd == R1_SUB_HEARTBEAT) &&
      d.payloadLength >= 1) {
    const uint32_t rx = millis();
    const bool deviceStatus = d.subCmd == R1_SUB_DEVICE_STATUS;
    const uint8_t b = d.payload[0];
    if ((deviceStatus && b <= 100) ||
        (!deviceStatus && b > 0 && b <= 100)) {
      gR1Cache.battery      = b;
      gR1Cache.batteryRxMs  = rx;
      gR1Cache.batteryValid = true;
      g2HealthNoteSample(HEALTH_METRIC_BATTERY, (int16_t)b, 0);
    }
    const bool wearIngestion = deviceStatus
        ? r1ProfileSupportsDeviceStatusIngestion(profile)
        : r1ProfileSupportsWearStatus(profile);
    if (wearIngestion &&
        d.payloadLength >= 2 && d.payload[1] <= 2) {
      ringNoteWear(d.payload[1], rx);
    }
    return;
  }

  // system/system/wearStatus — dedicated 1-byte wear probe.
  if (d.module == R1_MODULE_SYSTEM && d.cmd == R1_CMD_SYSTEM &&
      d.subCmd == R1_SUB_WEAR_STATUS && d.payloadLength >= 1 &&
      d.payload[0] <= 2 && r1ProfileSupportsWearStatus(profile)) {
    ringNoteWear(d.payload[0], millis());
  }
}

// =============================================================================
// Forwarded-telemetry sink (called from G2_Glasses sid=0x90/0x91 RX)
// =============================================================================
// When the right temple's bridge is active (see ringbridge CLI), the temple
// connects to the ring directly and forwards RingDataPackage frames to us
// on sid=0x90 (UX_RING_ROW_DATA_ID). Decode the protobuf wrapper, walk into
// the nested RingRawData (field 4, wire-type 2), and copy each present field
// into gR1Cache so the same display / spoof / status code sees fresh data
// regardless of who's holding the ring's BLE link.
//
// Schema: docs/g2_proto/ring.proto. Field tags inside RingRawData:
//   1 battery, 2 chargeStates, 3 hr, 4 hrTs, 5 spo2, 6 spo2Ts,
//   7 hrv, 8 hrvTs, 9 temp, 10 tempTs, 11 actKcal, 12 actKcalTs,
//   13 allKcal, 14 allKcalTs, 15 steps, 16 stepsTs, 17 errorCode.
//
// We currently mirror battery / hr+hrTs / spo2+spo2Ts / hrv+hrvTs into the
// cache. Extend if the spoof builder ever grows new fields.
void g2RingNoteForwardedTelemetry(const uint8_t* pb, size_t pbLen) {
  if (!pb || pbLen == 0) return;

  // Walk the outer RingDataPackage to find field 4 (rawData, len-delim) or
  // field 3 (event, len-delim — currently unused; only logged).
  uint64_t cmdId = 0; bool hasCmdId = false;
  const uint8_t* raw = nullptr;
  size_t          rawLen = 0;
  size_t pos = 0;
  while (pos < pbLen) {
    uint32_t field; uint8_t wire;
    if (!g2PbReadTag(pb, pbLen, &pos, &field, &wire)) return;
    if (field == 1 && wire == G2_PB_WIRE_VARINT) {
      if (!g2PbReadVarint(pb, pbLen, &pos, &cmdId)) return;
      hasCmdId = true;
    } else if (field == 4 && wire == G2_PB_WIRE_LEN_DELIM) {
      uint64_t sl;
      if (!g2PbReadVarint(pb, pbLen, &pos, &sl)) return;
      if (pos + sl > pbLen) return;
      raw    = pb + pos;
      rawLen = (size_t)sl;
      pos += (size_t)sl;
    } else {
      if (!g2PbSkipField(pb, pbLen, &pos, wire)) return;
    }
  }
  if (!raw || rawLen == 0) return;
  // commandId == 2 means RAW_DATA per ring.proto — but the temple has been
  // observed to omit field 1 entirely on some firmware versions (proto3
  // default suppression), so we don't gate the parse on hasCmdId.
  (void)hasCmdId;

  // Serialize a complete forwarded-cache mutation (including live-series
  // appends) with direct-link resets. A frame already being parsed finishes
  // before the reset and is cleared; it cannot leave a half-old cache behind.
  RingTelemetryGuard telemetry;
  if (!telemetry.locked()) return;
  if (!ringPrepareForwardedTelemetryMutationLocked()) {
    DEBUG_RING_HEALTHF(
        "[RING] forwarded telemetry ignored while direct link is online");
    return;
  }

  // Parse the nested RingRawData. Only update the cache for fields whose
  // values fall in the realistic range — drops obviously-corrupt frames.
  uint64_t v;
  uint32_t now = (uint32_t)time(nullptr);
  size_t p = 0;
  while (p < rawLen) {
    uint32_t f; uint8_t w;
    if (!g2PbReadTag(raw, rawLen, &p, &f, &w)) return;
    if (w != G2_PB_WIRE_VARINT) {
      if (!g2PbSkipField(raw, rawLen, &p, w)) return;
      continue;
    }
    if (!g2PbReadVarint(raw, rawLen, &p, &v)) return;
    long sv = (long)(int32_t)v;  // RingRawData fields are int32
    const uint32_t rx = millis();
    switch (f) {
      case 1:  // battery
        if (sv > 0 && sv <= 100) {
          gR1Cache.battery      = (uint8_t)sv;
          gR1Cache.batteryRxMs  = rx;
          gR1Cache.batteryValid = true;
          g2HealthNoteSample(HEALTH_METRIC_BATTERY, (int16_t)sv, 0);
        }
        break;
      case 3:  // hr
        if (sv > 0 && sv < 250) {
          gR1Cache.hr      = (uint8_t)sv;
          gR1Cache.hrRxMs  = rx;
          gR1Cache.hrValid = true;
          g2HealthNoteSample(HEALTH_METRIC_HR, (int16_t)sv, now);
        }
        break;
      case 4:  // hrTs
        // Re-stamp rxMs with the stamp: proto3 omits zero fields, so a frame
        // can carry hrTs WITHOUT hr and would otherwise advance the ring
        // timestamp while leaving rxMs from an older frame — which makes the
        // age-adjusted projection in ringBestKnownEpoch() read too far ahead.
        if (gR1Cache.hrValid) {
          gR1Cache.hrTs   = (sv > 0) ? (uint32_t)sv : now;
          gR1Cache.hrRxMs = rx;
        }
        break;
      case 5:  // spo2
        if (sv >= 70 && sv <= 100) {
          gR1Cache.spo2      = (uint8_t)sv;
          gR1Cache.spo2RxMs  = rx;
          gR1Cache.spo2Valid = true;
          g2HealthNoteSample(HEALTH_METRIC_SPO2, (int16_t)sv, now);
        }
        break;
      case 6:  // spo2Ts — see case 4 on why rxMs moves with the stamp
        if (gR1Cache.spo2Valid) {
          gR1Cache.spo2Ts   = (sv > 0) ? (uint32_t)sv : now;
          gR1Cache.spo2RxMs = rx;
        }
        break;
      case 7:  // hrv
        if (sv > 0 && sv < 1000) {
          gR1Cache.hrv      = (int16_t)sv;
          gR1Cache.hrvRxMs  = rx;
          gR1Cache.hrvValid = true;
          g2HealthNoteSample(HEALTH_METRIC_HRV, (int16_t)sv, now);
        }
        break;
      case 8:  // hrvTs — see case 4 on why rxMs moves with the stamp
        if (gR1Cache.hrvValid) {
          gR1Cache.hrvTs   = (sv > 0) ? (uint32_t)sv : now;
          gR1Cache.hrvRxMs = rx;
        }
        break;
      case 9:  // temp (°C × 10 in RingRawData)
        if (sv >= 150 && sv <= 450) {
          gR1Cache.tempTenths = (int16_t)sv;
          gR1Cache.tempRxMs   = rx;
          gR1Cache.tempValid  = true;
          g2HealthNoteSample(HEALTH_METRIC_TEMP, (int16_t)sv, now);
        }
        break;
      case 10:  // tempTs — see case 4 on why rxMs moves with the stamp
        if (gR1Cache.tempValid) {
          gR1Cache.tempTs   = (sv > 0) ? (uint32_t)sv : now;
          gR1Cache.tempRxMs = rx;
        }
        break;
      default:
        break;  // chargeStates / kcal / steps not cached today
    }
  }

  DEBUG_RING_HEALTHF("[RING] cache (forwarded) batt=%s%u hr=%s%u hrv=%s%d spo2=%s%u temp=%s%d",
            gR1Cache.batteryValid ? "" : "?", (unsigned)gR1Cache.battery,
            gR1Cache.hrValid      ? "" : "?", (unsigned)gR1Cache.hr,
            gR1Cache.hrvValid     ? "" : "?", (int)gR1Cache.hrv,
            gR1Cache.spo2Valid    ? "" : "?", (unsigned)gR1Cache.spo2,
            gR1Cache.tempValid    ? "" : "?", (int)gR1Cache.tempTenths);
}

// =============================================================================
// Ring envelope decoding (no-write; receive-only)
// =============================================================================
// The BLE callback only copies into a fixed queue. The owner validates CRC32
// and the declared model length before correlation or typed ingestion, then
// logs the trusted decode here in normal task context.

// Decode and print a single ring notify frame using the real R1 wire format
// (See System_R1_Protocol.h for the full envelope spec.) `data` is the raw
// BLE characteristic value — the ring writes one logical frame per notify
// in our usage so far.
//
static uint8_t ringAckErrorCode(uint8_t ack) {
  switch (ack) {
    case R1_STATUS_ACK_ERROR:      return G2_RING_ERR_ACK_ERROR;
    case R1_STATUS_ACK_REFUSE:     return G2_RING_ERR_ACK_REFUSED;
    case R1_STATUS_ACK_NOTSUPPORT: return G2_RING_ERR_ACK_NOT_SUPPORTED;
    default:                       return G2_RING_ERR_NONE;
  }
}

static G2RingProtocolProfile ringPublicProfile(R1ProtocolProfile profile) {
  switch (profile) {
    case R1_PROFILE_FW_2_2_7_0005:
      return G2_RING_PROFILE_FW_2_2_7_0005;
    case R1_PROFILE_FW_2_2_9_0003:
      return G2_RING_PROFILE_FW_2_2_9_0003;
    default:
      return G2_RING_PROFILE_UNKNOWN;
  }
}

static void ringSetSetupState(G2RingSetupState state,
                              uint8_t error = G2_RING_ERR_NONE) {
  portENTER_CRITICAL(&sTransportMux);
  sControlStatus.setupState = state;
  sControlStatus.setupLastError = error;
  sControlStatus.protocolProfile = ringPublicProfile(sR1Profile);
  sControlStatus.protocolProfileKnown = sR1Profile != R1_PROFILE_UNKNOWN;
  portEXIT_CRITICAL(&sTransportMux);
}

static void ringFinishSetup(bool success, uint8_t error) {
  sSetupOwner.active = false;
  sSetupOwner.frameReady = false;
  sSetupRequested = false;
  sRingSetupDone = success;
  ringSetSetupState(success ? G2_RING_SETUP_READY : G2_RING_SETUP_ERROR,
                    success ? (uint8_t)G2_RING_ERR_NONE : error);
  if (sSetupDone) xSemaphoreGive(sSetupDone);
  DEBUG_RING_SETUPF("[RING] setup %s profile=%s error=%s",
            success ? "ready" : "failed",
            r1ProtocolProfileName(sR1Profile),
            g2RingTransactionErrorName(error));
}

static void ringAdvanceSetup(G2RingSetupState next) {
  sSetupOwner.stage = next;
  sSetupOwner.frameReady = false;
  sSetupOwner.written = false;
  sSetupOwner.deadlineMs = millis() + RING_SETUP_TIMEOUT_MS;
  ringSetSetupState(next);
  ringWakeOwner();
}

static void ringCompleteActive(G2RingTransactionState state,
                               uint8_t error = G2_RING_ERR_NONE,
                               uint8_t ackCode = 0) {
  if (!sActiveTransaction.valid) return;
  const RingIntent intent = sActiveTransaction.intent;
  const bool verifiedCompositeDailyStage =
      state == G2_RING_TX_VERIFIED && sHistoryCoordinator.active &&
      sHistoryCoordinator.transaction.id == intent.handle.id &&
      sHistoryCoordinator.transaction.generation == intent.handle.generation;
  const uint8_t compositeDailyProgress = verifiedCompositeDailyStage
      ? (uint8_t)(sHistoryCoordinator.verifiedCount + 1)
      : 0;
  ringUpdateTransaction(intent.handle, state, error, ackCode,
                        /*completed=*/true);
  portENTER_CRITICAL(&sTransportMux);
  if (sControlStatus.generation == intent.handle.generation &&
      intent.control == RING_CONTROL_HEALTH) {
    const bool superseded = sControlStatus.healthDesired != intent.desired;
    if (superseded && !ringControlSupported(
            sR1Profile, RING_CONTROL_HEALTH,
            sControlStatus.healthDesired)) {
      ringSetControlUnsupportedLocked(RING_CONTROL_HEALTH);
    } else {
      sControlStatus.healthPending = superseded;
      sControlStatus.healthLastError =
          superseded ? (uint8_t)G2_RING_ERR_NONE : error;
    }
    if (superseded && sControlStatus.healthPending) {
      sControlStatus.healthTransaction = G2RingTransactionHandle{};
    }
  } else if (sControlStatus.generation == intent.handle.generation &&
             intent.control == RING_CONTROL_LOW_POWER) {
    const bool superseded = sControlStatus.lowPowerDesired != intent.desired;
    if (superseded && !ringControlSupported(
            sR1Profile, RING_CONTROL_LOW_POWER,
            sControlStatus.lowPowerDesired)) {
      ringSetControlUnsupportedLocked(RING_CONTROL_LOW_POWER);
    } else {
      sControlStatus.lowPowerPending = superseded;
      sControlStatus.lowPowerLastError =
          superseded ? (uint8_t)G2_RING_ERR_NONE : error;
    }
    if (superseded && sControlStatus.lowPowerPending) {
      sControlStatus.lowPowerTransaction = G2RingTransactionHandle{};
    }
  }
  if (sHealthPageRefresh.phase ==
          RING_HEALTH_REFRESH_WAIT_DEVICE_STATUS &&
      sHealthPageRefresh.deviceStatus.id == intent.handle.id &&
      sHealthPageRefresh.deviceStatus.generation == intent.handle.generation) {
    // Publish terminal child evidence in the same owner completion operation.
    // A disconnect callback can otherwise arrive before the composite's next
    // service lap and erase a deviceStatus result that already completed.
    sHealthPageRefresh.deviceStatusCompleted = true;
    sHealthPageRefresh.deviceStatusVerified =
        state == G2_RING_TX_VERIFIED;
    sHealthPageRefresh.deviceStatusError =
        state == G2_RING_TX_VERIFIED
            ? (uint8_t)G2_RING_ERR_NONE
            : (error != G2_RING_ERR_NONE
                   ? error
                   : (uint8_t)G2_RING_ERR_TIMEOUT);
  }
  if (verifiedCompositeDailyStage &&
      sHealthPageRefresh.phase == RING_HEALTH_REFRESH_WAIT_DAILY &&
      sHealthPageRefresh.generation == intent.handle.generation &&
      compositeDailyProgress > sHealthPageRefresh.dailyVerifiedCount) {
    sHealthPageRefresh.dailyVerifiedCount = compositeDailyProgress;
  }
  portEXIT_CRITICAL(&sTransportMux);
  if (intent.payloadSlot < RING_RAW_PAYLOAD_SLOTS) {
    ringReleaseRawPayload(intent.payloadSlot);
  }
  sActiveTransaction = RingActiveTransaction{};
}

static bool ringFingerprintEqual(const RingRxFingerprint& a,
                                 const RingRxFingerprint& b) {
  return a.generation == b.generation && a.crc32 == b.crc32 &&
         a.serial == b.serial && a.module == b.module && a.cmd == b.cmd &&
         a.subCmd == b.subCmd && a.statusByte == b.statusByte;
}

static bool ringRememberRx(const R1Decoded& d, uint32_t generation) {
  const RingRxFingerprint fp = {
    generation, d.crc32Received, d.serial, d.module, d.cmd, d.subCmd,
    d.statusByte
  };
  for (uint8_t i = 0; i < RING_RX_DUPLICATE_DEPTH; ++i) {
    if (ringFingerprintEqual(fp, sRxFingerprints[i])) return false;
  }
  sRxFingerprints[sRxFingerprintCursor] = fp;
  sRxFingerprintCursor = (uint8_t)((sRxFingerprintCursor + 1) %
                                   RING_RX_DUPLICATE_DEPTH);
  return true;
}

static void ringQueuePacketAck(const R1PacketAckDescriptor& descriptor,
                               uint32_t generation) {
  // Only the protocol factory can stamp this descriptor, after validating the
  // complete decoded frame. Avoid duplicate pending entries while still ACKing
  // a retransmit after its prior ACK has left this reserved lane.
  if (!descriptor.valid()) return;
  for (uint8_t i = 0; i < sPacketAckCount; ++i) {
    const RingPacketAckPending& pending = sPacketAckQueue[i];
    if (pending.generation == generation &&
        pending.descriptor.receivedSerial() == descriptor.receivedSerial() &&
        pending.descriptor.module() == descriptor.module() &&
        pending.descriptor.cmd() == descriptor.cmd() &&
        pending.descriptor.subCmd() == descriptor.subCmd()) return;
  }
  if (sActivePacketAck.valid &&
      sActivePacketAck.pending.generation == generation &&
      sActivePacketAck.pending.descriptor.receivedSerial() ==
          descriptor.receivedSerial() &&
      sActivePacketAck.pending.descriptor.module() == descriptor.module() &&
      sActivePacketAck.pending.descriptor.cmd() == descriptor.cmd() &&
      sActivePacketAck.pending.descriptor.subCmd() == descriptor.subCmd()) {
    return;
  }
  if (sPacketAckCount >= RING_PACKET_ACK_DEPTH) {
    DEBUG_RING_PROTOCOLF("[RING] packetAck lane full; RX ser=%u left unacked",
              (unsigned)descriptor.receivedSerial());
    return;
  }
  sPacketAckQueue[sPacketAckCount++] =
      RingPacketAckPending{generation, descriptor};
}

static bool ringApplyControlObservation(const R1Decoded& d,
                                        RingControlTarget& observedTarget,
                                        G2RingObservedState& observed) {
  observedTarget = RING_CONTROL_NONE;
  observed = G2_RING_OBS_UNKNOWN;
  if (sR1Profile == R1_PROFILE_UNKNOWN) return false;

  if (d.subCmd == R1_SUB_SYSTEM_SETTINGS) {
    R1LowPowerStatus status{};
    if (r1ParseLowPowerStatus(sR1Profile, d, status) != R1_PARSE_OK) {
      return false;
    }
    observedTarget = RING_CONTROL_LOW_POWER;
    observed = status.enabled ? G2_RING_OBS_ON : G2_RING_OBS_OFF;
    portENTER_CRITICAL(&sTransportMux);
    sControlStatus.lowPowerObserved = observed;
    sControlStatus.lowPowerObservedAtMs = millis();
    portEXIT_CRITICAL(&sTransportMux);
    return true;
  }
  return false;
}

static bool ringDecodedMatches(uint8_t module, uint8_t cmd, uint8_t subCmd,
                               const R1Decoded& d) {
  return d.module == module && d.cmd == cmd && d.subCmd == subCmd;
}

static void ringHandleSetupRx(const R1Decoded& d) {
  if (!sSetupOwner.active || !sSetupOwner.written ||
      sSetupOwner.generation != sLinkGeneration) return;
  if (!ringDecodedMatches(R1_MODULE_SYSTEM, R1_CMD_SYSTEM,
                          sSetupOwner.stage == G2_RING_SETUP_AUTH
                              ? R1_SUB_PAIR_AUTH
                              : sSetupOwner.stage == G2_RING_SETUP_DEVICE_INFO
                                    ? R1_SUB_DEVICE_INFO
                                    : sSetupOwner.stage == G2_RING_SETUP_TIME
                                          ? R1_SUB_SYSTEM_TIME
                                          : R1_SUB_ADV_START,
                          d)) return;

  const bool exactSerial = d.serial == sSetupOwner.frame.serial;
  if (d.statusType == R1_STATUS_TYPE_ACK && exactSerial &&
      d.statusAck != R1_STATUS_ACK_OK) {
    ringFinishSetup(false, ringAckErrorCode(d.statusAck));
    return;
  }

  if (sSetupOwner.stage == G2_RING_SETUP_DEVICE_INFO) {
    R1DeviceInfo info{};
    const R1ParseError parsed = r1ParseDeviceInfo(d, info);
    if (parsed != R1_PARSE_OK || !exactSerial) return;
    ringSetSetupState(G2_RING_SETUP_PROFILE);
    portENTER_CRITICAL(&sTransportMux);
    sR1Profile = info.profile;
    sControlStatus.protocolProfile = ringPublicProfile(sR1Profile);
    sControlStatus.protocolProfileKnown = sR1Profile != R1_PROFILE_UNKNOWN;
    if (!ringControlSupported(sR1Profile, RING_CONTROL_LOW_POWER,
                              sControlStatus.lowPowerDesired)) {
      ringSetControlUnsupportedLocked(RING_CONTROL_LOW_POWER);
    } else {
      sControlStatus.lowPowerPending = true;
      sControlStatus.lowPowerLastError = G2_RING_ERR_NONE;
      sControlStatus.lowPowerTransaction = G2RingTransactionHandle{};
    }
    if (!ringControlSupported(sR1Profile, RING_CONTROL_HEALTH,
                              sControlStatus.healthDesired)) {
      ringSetControlUnsupportedLocked(RING_CONTROL_HEALTH);
    } else {
      sControlStatus.healthPending =
          sControlStatus.healthDesired != G2_RING_PRESERVE;
      sControlStatus.healthLastError = G2_RING_ERR_NONE;
      sControlStatus.healthTransaction = G2RingTransactionHandle{};
    }
    portEXIT_CRITICAL(&sTransportMux);
    DEBUG_RING_SETUPF("[RING] deviceInfo fw='%s' hw='%s' profile=%s",
              info.firmware, info.hardware, r1ProtocolProfileName(sR1Profile));
    if (sR1Profile == R1_PROFILE_UNKNOWN) {
      ringFinishSetup(false, G2_RING_ERR_PROFILE_UNKNOWN);
      return;
    }
    ringAdvanceSetup(G2_RING_SETUP_TIME);
    return;
  }

  if (d.statusType != R1_STATUS_TYPE_ACK || !exactSerial ||
      d.statusAck != R1_STATUS_ACK_OK) return;
  switch (sSetupOwner.stage) {
    case G2_RING_SETUP_AUTH:
      ringAdvanceSetup(G2_RING_SETUP_DEVICE_INFO);
      break;
    case G2_RING_SETUP_TIME:
      ringAdvanceSetup(G2_RING_SETUP_ADV_START);
      break;
    case G2_RING_SETUP_ADV_START:
      ringFinishSetup(true, G2_RING_ERR_NONE);
      break;
    default:
      break;
  }
}

static bool ringDeviceStatusPayloadValid(const R1Decoded& d) {
  // Captured 2.2.9 contract: the GET is answered by one ACK/SET/OK envelope
  // with exactly seven payload bytes. Only battery and wear have established
  // meanings; the remaining five bytes stay opaque.
  return d.module == R1_MODULE_SYSTEM && d.cmd == R1_CMD_SYSTEM &&
         d.subCmd == R1_SUB_DEVICE_STATUS &&
         d.statusType == R1_STATUS_TYPE_ACK &&
         d.statusMethod == R1_STATUS_METHOD_SET &&
         d.statusAck == R1_STATUS_ACK_OK && d.payloadLength == 7 &&
         d.payload[0] <= 100 && d.payload[1] <= 2;
}

static bool ringDailyPayloadValid(const R1Decoded& d) {
  if (d.module == R1_MODULE_SYSTEM && d.cmd == R1_CMD_SYSTEM &&
      d.subCmd == R1_SUB_DEVICE_STATUS) {
    return r1ProfileSupportsDeviceStatusIngestion(sR1Profile) &&
           ringDeviceStatusPayloadValid(d);
  }
  if (d.module != R1_MODULE_HEALTH || d.subCmd != R1_SUB_DAILY) return true;
  if (d.cmd == R1_CMD_HRV) {
    R1HrvDailyResult result{};
    return r1ParseHrvDaily(sR1Profile, d, result) == R1_PARSE_OK;
  }
  if (d.cmd == R1_CMD_ACTIVITY) {
    // Serialized on the ring owner task; reuses the shared PSRAM scratch (the
    // parsed value is discarded — only the OK/rejected verdict is needed here).
    return r1ParseActivityDaily(sR1Profile, d, sRingActivityScratch) == R1_PARSE_OK;
  }
  if (d.cmd == R1_CMD_HEARTRATE || d.cmd == R1_CMD_SPO2) {
    R1CommonDailyResult result{};
    return r1ParseCommonDaily(sR1Profile, d, result) == R1_PARSE_OK;
  }
  return false;
}

static void ringHandleActiveRx(const R1Decoded& d,
                               RingControlTarget observedTarget,
                               G2RingObservedState observed) {
  if (!sActiveTransaction.valid ||
      !sActiveTransaction.written ||
      sActiveTransaction.intent.handle.generation != sLinkGeneration) return;
  RingIntent& intent = sActiveTransaction.intent;
  if (!ringDecodedMatches(intent.module, intent.cmd, intent.subCmd, d)) return;

  const bool exactSerial = d.serial == sActiveTransaction.frame.serial;
  const bool dailyDataNotify =
      intent.module == R1_MODULE_HEALTH && intent.subCmd == R1_SUB_DAILY &&
      d.statusType == R1_STATUS_TYPE_NOTIFY &&
      d.statusMethod == R1_STATUS_METHOD_SET &&
      d.statusAck == R1_STATUS_ACK_OK && d.payloadLength > 0;
  const bool unparsedSleepData =
      intent.module == R1_MODULE_HEALTH && intent.cmd == R1_CMD_SLEEP &&
      dailyDataNotify;
  if (unparsedSleepData) {
    // Presence prevents a false no-data conclusion even when the unproven
    // payload layout stays blocked. Only a profile with explicit sleep-data
    // ingestion support may publish PRESENT into typed history.
    sActiveTransaction.sleepDataCandidateSeen = true;
    if (r1ProfileSupportsSleepDataIngestion(sR1Profile)) {
      g2HealthHistorySetSleepState(R1_HISTORY_SLEEP_PRESENT);
    }
  }
  if (dailyDataNotify) {
    // Typed validation is the admission point for the buffered completion
    // fact. Invalid/unknown pages neither satisfy the transaction nor affect
    // packet-ACK policy. The outer RX path separately applies the same typed
    // gate before queueing a packet ACK.
    if (ringDailyPayloadValid(d) &&
        ringRememberTypedDailyData(sActiveTransaction)) {
      ringCompleteActive(G2_RING_TX_VERIFIED);
    }
    return;
  }
  if (!exactSerial) return;
  if (d.statusType == R1_STATUS_TYPE_ACK && exactSerial &&
      d.statusAck != R1_STATUS_ACK_OK) {
    ringCompleteActive(G2_RING_TX_REFUSED, ringAckErrorCode(d.statusAck),
                       d.statusAck);
    return;
  }

  if (d.statusType == R1_STATUS_TYPE_ACK && exactSerial &&
      d.statusAck == R1_STATUS_ACK_OK) {
    const bool healthDailyAck = intent.module == R1_MODULE_HEALTH &&
        intent.subCmd == R1_SUB_DAILY;
    // Captures prove the command half of a DAILY transaction as an empty
    // ACK/SET/OK envelope. A wrong-method or payload-bearing lookalike cannot
    // supply the missing half after data-first arrival.
    if (healthDailyAck &&
        (d.statusMethod != R1_STATUS_METHOD_SET || d.payloadLength != 0)) {
      return;
    }
    if (ringRememberSuccessfulCommandAck(sActiveTransaction, millis())) {
      // A typed-valid independent daily page arrived first. The successful
      // matching command ACK supplies the missing half of the transaction.
      ringCompleteActive(G2_RING_TX_VERIFIED, G2_RING_ERR_NONE, d.statusAck);
      return;
    }
    if (intent.verifyAfterAck && !sActiveTransaction.readbackPhase) {
      ringUpdateTransaction(intent.handle, G2_RING_TX_ACKED,
                            G2_RING_ERR_NONE, d.statusAck);
      sActiveTransaction.readbackPhase = true;
      sActiveTransaction.frameReady = false;
      sActiveTransaction.written = false;
      sActiveTransaction.commandAcked = false;
      // Only low-power has a capture-proven GET/readback contract.
      intent.kind = RING_INTENT_LOW_POWER_QUERY;
      intent.statusMethod = R1_STATUS_METHOD_GET;
      intent.expectsPayload = true;
      intent.verifyAfterAck = false;
      sActiveTransaction.deadlineMs = millis() + intent.timeoutMs;
      ringWakeOwner();
      return;
    }
    if (!intent.expectsPayload) {
      ringCompleteActive(G2_RING_TX_ACKED, G2_RING_ERR_NONE, d.statusAck);
      return;
    }
    ringUpdateTransaction(intent.handle, G2_RING_TX_ACKED,
                          G2_RING_ERR_NONE, d.statusAck);
    if (d.payloadLength == 0) return;
  }

  if (!intent.expectsPayload || d.payloadLength == 0) return;
  if (intent.control != RING_CONTROL_NONE) {
    if (observedTarget != intent.control) return;
    if (intent.desired != G2_RING_PRESERVE) {
      const G2RingObservedState wanted = intent.desired == G2_RING_ON
                                             ? G2_RING_OBS_ON
                                             : G2_RING_OBS_OFF;
      if (observed != wanted) {
        ringCompleteActive(G2_RING_TX_REFUSED, G2_RING_ERR_VERIFY_MISMATCH);
        return;
      }
    }
  }
  if (!ringDailyPayloadValid(d)) return;
  ringCompleteActive(G2_RING_TX_VERIFIED);
}

static void ringReasmReset(void) {
  sRingReasm.active = false;
  sRingReasm.crc32 = 0;
  sRingReasm.nextCountdown = 0;
  sRingReasm.generation = 0;
  sRingReasm.len = 0;
}

// Complete the in-flight history-sweep transaction for an activity-daily NOTIFY
// proven via reassembly. The normal ringHandleActiveRx path never sees it (the
// fragments don't decode as single frames), so mirror its typed-daily ordering
// state: remember a proven model that arrives first, or finish immediately when
// the matching command ACK is already present.
static void ringCompleteReassembledDaily(uint8_t moduleId, uint8_t cmd,
                                         uint8_t subCmd) {
  if (!sActiveTransaction.valid || !sActiveTransaction.written ||
      sActiveTransaction.intent.handle.generation != sLinkGeneration) return;
  const RingIntent& intent = sActiveTransaction.intent;
  if (intent.module != moduleId || intent.cmd != cmd ||
      intent.subCmd != subCmd) return;
  if (!intent.expectsPayload) return;
  if (ringRememberTypedDailyData(sActiveTransaction)) {
    ringCompleteActive(G2_RING_TX_VERIFIED);
  }
}

// Caller is the owner task. The telemetry fence also serializes setup's
// history-peer publication, so an old model cannot be parsed for one peer and
// applied after setup switches to another. Transport identity/profile are
// copied inside that fence before any buffer parse or history/ACK mutation.
static bool ringReasmFinalizeCurrentLocked(
    uint32_t generation, R1ProtocolProfile& profile) {
  uint32_t linkGeneration = 0;
  bool linkOnline = false;
  portENTER_CRITICAL(&sTransportMux);
  linkGeneration = sLinkGeneration;
  linkOnline = sLinkOnline;
  profile = sR1Profile;
  portEXIT_CRITICAL(&sTransportMux);
  return linkOnline && generation != 0 &&
         generation == sTelemetryGeneration &&
         generation == linkGeneration && sRingReasm.active &&
         generation == sRingReasm.generation &&
         r1ProfileSupportsActivityReassembly(profile);
}

// Validate + ingest a fully stitched activity-daily model.
static void ringReasmFinalize(uint32_t generation) {
  RingTelemetryGuard telemetry;
  if (!telemetry.locked()) return;
  R1ProtocolProfile profile = R1_PROFILE_UNKNOWN;
  if (!ringReasmFinalizeCurrentLocked(generation, profile)) {
    DEBUG_RING_PROTOCOLF(
        "[RING] stale reasm activity/daily dropped gen=%lu cache-gen=%lu",
        (unsigned long)generation, (unsigned long)sTelemetryGeneration);
    return;
  }
  const R1ParseError e = r1ParseReassembledActivityDaily(
      profile, sRingReasmBuf, sRingReasm.len, sRingReasm.crc32,
      sRingActivityScratch);
  if (e != R1_PARSE_OK) {
    // Wire-RE diagnostics: the fragment framing is reverse-engineered, so on
    // rejection show how the stitched length/CRC compare to the header's
    // declared model. `declared` should equal the model size; `crc@declared`
    // and `crc@accum` bracket where a padding/off-by-one byte would land.
    // The two CRC passes feed only the DEBUG_RING_PROTOCOLF — same gate.
    if (getLogLevel() >= LOG_LEVEL_DEBUG && isDebugFlagSet(DEBUG_RING | DEBUG_RING_PROTOCOL)) {
      const size_t declared = sRingReasm.len >= 10
          ? ((size_t)sRingReasmBuf[8] | ((size_t)sRingReasmBuf[9] << 8)) : 0;
      const uint32_t crcDeclared = (declared >= 12 && declared <= sRingReasm.len)
          ? r1Crc32(sRingReasmBuf, declared) : 0;
      const uint32_t crcAccum = r1Crc32(sRingReasmBuf, sRingReasm.len);
      DEBUG_RING_PROTOCOLF("[RING] reasm activity/daily rejected: %s accum=%u declared=%u "
                "hdrCrc=%08lX crc@declared=%08lX crc@accum=%08lX hdr=[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
                r1ParseErrorName(e), (unsigned)sRingReasm.len, (unsigned)declared,
                (unsigned long)sRingReasm.crc32, (unsigned long)crcDeclared,
                (unsigned long)crcAccum,
                sRingReasmBuf[0], sRingReasmBuf[1], sRingReasmBuf[2],
                sRingReasmBuf[3], sRingReasmBuf[4], sRingReasmBuf[5],
                sRingReasmBuf[6], sRingReasmBuf[7], sRingReasmBuf[8],
                sRingReasmBuf[9]);
      // TEMP-REASM-DIAG: full stitched model so the exact truncation is visible.
      for (size_t off = 0; off < sRingReasm.len; off += 32) {
        char row[3 * 32 + 1];
        size_t rp = 0;
        const size_t end = (sRingReasm.len - off) < 32 ? (sRingReasm.len - off) : 32;
        for (size_t i = 0; i < end && rp + 3 < sizeof(row); i++)
          rp += snprintf(row + rp, sizeof(row) - rp, "%02X ", sRingReasmBuf[off + i]);
        if (rp > 0) row[rp - 1] = '\0'; else row[0] = '\0';
        DEBUG_RING_PROTOCOLF("[RING] MODELRAW len=%u off=%u [%s]",
                  (unsigned)sRingReasm.len, (unsigned)off, row);
      }
    }
    return;
  }
  // Disconnect is allowed to mark transport offline without waiting for the
  // task mutex. Recheck after the potentially long parse and before history.
  R1ProtocolProfile currentProfile = R1_PROFILE_UNKNOWN;
  if (!ringReasmFinalizeCurrentLocked(generation, currentProfile) ||
      currentProfile != profile) return;
  DEBUG_RING_PROTOCOLF("[RING] reasm activity/daily OK: count=%u ser=%u (%u B model)",
            (unsigned)sRingActivityScratch.count,
            (unsigned)sRingActivityScratch.sourceSerial,
            (unsigned)sRingReasm.len);
  (void)g2HealthApplyActivityDaily(sRingActivityScratch);
  // Peer publication cannot cross the telemetry fence, but disconnect can.
  // Never mint flow-control/transaction success after it has won.
  if (!ringReasmFinalizeCurrentLocked(generation, currentProfile) ||
      currentProfile != profile) return;
  // Flow control: the ring expects a packetAck for each accepted daily NOTIFY.
  R1PacketAckDescriptor descriptor;
  if (r1MakeDailyPacketAckDescriptor(profile,
                                     sRingActivityScratch.sourceSerial,
                                     R1_CMD_ACTIVITY, descriptor)) {
    ringQueuePacketAck(descriptor, generation);
  }
  ringCompleteReassembledDaily(R1_MODULE_HEALTH, R1_CMD_ACTIVITY, R1_SUB_DAILY);
}

// Feed one inbound frame to the fragment reassembler. Returns true when the
// frame was consumed as a fragment (the caller must NOT also treat it as a
// standalone frame); false for an ordinary single-notification frame.
static bool ringReasmConsume(const uint8_t* data, size_t len,
                             uint32_t generation) {
  const R1ProtocolProfile profile = ringSnapshotProtocolProfile();
  if (!r1ProfileSupportsActivityReassembly(profile)) return false;
  if (!data || len < 5) return false;  // too short to carry [countdown][crc32]
  const uint8_t countdown = data[0];
  const uint32_t crc32 = (uint32_t)data[1] | ((uint32_t)data[2] << 8) |
                         ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
  const uint8_t* chunk = data + 5;
  const size_t chunkLen = len - 5;

  // TEMP-REASM-DIAG: dump the WHOLE fragment frame (32-byte rows) so the
  // fragment framing can be reverse-engineered off hardware. Gated on
  // debugringdump; remove once the activity-daily reassembly is understood.
  if (getLogLevel() >= LOG_LEVEL_DEBUG && isDebugFlagSet(DEBUG_RING | DEBUG_RING_DUMP)) {
    for (size_t off = 0; off < len; off += 32) {
      char row[3 * 32 + 1];
      size_t rp = 0;
      const size_t end = (len - off) < 32 ? (len - off) : 32;
      for (size_t i = 0; i < end && rp + 3 < sizeof(row); i++)
        rp += snprintf(row + rp, sizeof(row) - rp, "%02X ", data[off + i]);
      if (rp > 0) row[rp - 1] = '\0'; else row[0] = '\0';
      DEBUG_RING_DUMPF("[RING] FRAGRAW cd=%u len=%u off=%u [%s]",
                (unsigned)countdown, (unsigned)len, (unsigned)off, row);
    }
  }

  auto append = [&](void) -> bool {
    if (sRingReasm.len + chunkLen > sizeof(sRingReasmBuf)) {
      DEBUG_RING_PROTOCOLF("[RING] reasm overflow (%u+%u > %u) — abandoned",
                (unsigned)sRingReasm.len, (unsigned)chunkLen,
                (unsigned)sizeof(sRingReasmBuf));
      ringReasmReset();
      return false;
    }
    memcpy(sRingReasmBuf + sRingReasm.len, chunk, chunkLen);
    sRingReasm.len += chunkLen;
    return true;
  };

  if (sRingReasm.active) {
    const bool continues = crc32 == sRingReasm.crc32 &&
                           countdown == sRingReasm.nextCountdown &&
                           generation == sRingReasm.generation;
    if (continues) {
      DEBUG_RING_PROTOCOLF("[RING] reasm frag cont cd=%u crc=%08lX chunk=%u [%02X %02X %02X %02X] (total=%u)",
                (unsigned)countdown, (unsigned long)crc32, (unsigned)chunkLen,
                chunk[0], chunkLen > 1 ? chunk[1] : 0,
                chunkLen > 2 ? chunk[2] : 0, chunkLen > 3 ? chunk[3] : 0,
                (unsigned)(sRingReasm.len + chunkLen));
      if (!append()) return true;  // overflow already reset; frame consumed
      if (countdown == 0) {
        ringReasmFinalize(generation);
        ringReasmReset();
      } else {
        sRingReasm.nextCountdown = (uint8_t)(countdown - 1);
      }
      return true;
    }
    // Desync: a lost, interleaved, or mismatched fragment. Abandon this session
    // and re-evaluate the current frame from scratch below.
    DEBUG_RING_PROTOCOLF("[RING] reasm desync (want cd=%u crc=%08lX; got cd=%u crc=%08lX) — abandoned",
              (unsigned)sRingReasm.nextCountdown,
              (unsigned long)sRingReasm.crc32, (unsigned)countdown,
              (unsigned long)crc32);
    ringReasmReset();
  }

  // A standalone frame carries transferType 0. Only countdown > 0 starts a new
  // multi-fragment message (a final fragment's countdown 0 is handled above,
  // while a session is active).
  if (countdown == 0) return false;

  DEBUG_RING_PROTOCOLF("[RING] reasm frag start cd=%u crc=%08lX chunk=%u [%02X %02X %02X %02X %02X %02X %02X %02X]",
            (unsigned)countdown, (unsigned long)crc32, (unsigned)chunkLen,
            chunk[0], chunkLen > 1 ? chunk[1] : 0, chunkLen > 2 ? chunk[2] : 0,
            chunkLen > 3 ? chunk[3] : 0, chunkLen > 4 ? chunk[4] : 0,
            chunkLen > 5 ? chunk[5] : 0, chunkLen > 6 ? chunk[6] : 0,
            chunkLen > 7 ? chunk[7] : 0);
  ringReasmReset();
  sRingReasm.active = true;
  sRingReasm.crc32 = crc32;
  sRingReasm.nextCountdown = (uint8_t)(countdown - 1);
  sRingReasm.generation = generation;
  sRingReasm.len = 0;
  (void)append();
  return true;
}

// True when this frame is the next fragment of an in-flight reassembly session,
// not a standalone frame. Needed to disambiguate the collision below: a final
// fragment (countdown 0) whose remaining chunk is a single byte is
// [00][crc32×4][b] == exactly 6 bytes with data[0]==0x00 — byte-for-byte
// identical to a Ring1Error frame. The only thing that separates them is that a
// continuation carries the CRC32 (and next-expected countdown) of the session
// already in progress, whereas a Ring1Error carries the offending *request's*
// CRC32. Mirrors the `continues` test inside ringReasmConsume().
static bool ringReasmFrameContinues(const uint8_t* data, size_t len,
                                    uint32_t generation) {
  const R1ProtocolProfile profile = ringSnapshotProtocolProfile();
  if (!r1ProfileSupportsActivityReassembly(profile)) return false;
  if (!sRingReasm.active || len < 5) return false;
  const uint32_t crc32 = (uint32_t)data[1] | ((uint32_t)data[2] << 8) |
                         ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
  return crc32 == sRingReasm.crc32 &&
         data[0] == sRingReasm.nextCountdown &&
         generation == sRingReasm.generation;
}

static void ringProcessRxFrame(const uint8_t* data, size_t len,
                               uint32_t generation) {
  uint32_t currentGeneration = 0;
  if (!data || len == 0 || !ringSnapshotLink(currentGeneration) ||
      generation != currentGeneration) return;
  gRing.packetsReceived++;

  // Ring1Error short-frame fast-path. The ring emits this 6-byte form on
  // some malformed-input rejections instead of a full envelope: a single
  // transfer-type byte (0x00) followed by the offending request's CRC32
  // and a 1-byte errorCode. r1Decode rejects anything < 17 B, so without
  // this branch the meaning is lost (only visible as raw bytes under
  // `debugringdump`). Format observed in the community RE decoder at
  // docs/FlutterApp-main/lib/src/protocol/r1_messages.dart:390 with a
  // pinned test fixture `00 D4 C9 BA 70 07` → errorCode=0x07.
  //
  // Guard: never let this swallow the final fragment of an active reassembly.
  // A cd=0 last fragment with a 1-byte chunk is indistinguishable from a
  // Ring1Error by length alone; ringReasmFrameContinues() only diverts a frame
  // whose CRC + next-countdown prove it belongs to the in-flight session, so a
  // genuine Ring1Error (which carries the request CRC, not the fragment CRC)
  // still takes this path.
  if (len == 6 && data[0] == 0x00 &&
      !ringReasmFrameContinues(data, len, generation)) {
    const uint32_t crc32 = (uint32_t)data[1]
                         | ((uint32_t)data[2] << 8)
                         | ((uint32_t)data[3] << 16)
                         | ((uint32_t)data[4] << 24);
    const uint8_t errorCode = data[5];
    DEBUG_RING_PROTOCOLF("[RING] RX Ring1Error errorCode=0x%02X crc32=0x%08lX",
              (unsigned)errorCode, (unsigned long)crc32);
    auto frameCrc32 = [](const R1Frame& frame) -> uint32_t {
      if (frame.length < 5) return 0;
      return (uint32_t)frame.bytes[1] |
             ((uint32_t)frame.bytes[2] << 8) |
             ((uint32_t)frame.bytes[3] << 16) |
             ((uint32_t)frame.bytes[4] << 24);
    };
    if (sSetupOwner.active && sSetupOwner.written &&
        sSetupOwner.generation == generation &&
        frameCrc32(sSetupOwner.frame) == crc32) {
      ringFinishSetup(false, G2_RING_ERR_ACK_ERROR);
    } else if (sActiveTransaction.valid && sActiveTransaction.written &&
               sActiveTransaction.intent.handle.generation == generation &&
               frameCrc32(sActiveTransaction.frame) == crc32) {
      ringCompleteActive(G2_RING_TX_REFUSED, G2_RING_ERR_ACK_ERROR,
                         errorCode);
    }
    return;
  }

  // Multi-notification fragments (activity-daily today) are stitched here; a
  // consumed fragment never proceeds to the single-frame decoder below.
  if (ringReasmConsume(data, len, generation)) return;

  R1Decoded d;
  bool sensitivePayload = false;
  if (r1Decode(data, len, d)) {
    // CRC16 mismatch is expected on every ring→phone frame (the firmware
    // emits a wrong CRC16 — see R1Decoded crc16Valid comment). Only flag
    // CRC?! when CRC32 (the real integrity check) actually fails.
    // Name lookups live inside the macro's argument list so they are only
    // evaluated when the line will actually print.
    DEBUG_RING_PROTOCOLF("[RING] RX %s/%s/%s ser=%u status=%s/%s/%s pLen=%u%s%s",
              r1ModuleName(d.module),
              r1CmdName(d.module, d.cmd),
              r1SubCmdName(d.module, d.cmd, d.subCmd),
              (unsigned)d.serial,
              r1StatusTypeName(d.statusType),
              r1StatusMethodName(d.statusMethod),
              r1StatusAckName(d.statusAck),
              (unsigned)d.payloadLength,
              d.crc32Valid ? "" : " CRC32?!",
              d.modelLengthValid ? "" : " LEN?!");

    sensitivePayload = d.module == R1_MODULE_SYSTEM &&
        d.cmd == R1_CMD_SYSTEM &&
        (d.subCmd == R1_SUB_DEVICE_SN ||
         d.subCmd == R1_SUB_GET_ALGO_KEY_STATUS ||
         d.subCmd == R1_SUB_SET_ALGO_KEY ||
         d.subCmd == R1_SUB_NV_RECOVER ||
         d.subCmd == R1_SUB_USER_INFO);

    // Hex-dump non-trivial, non-sensitive payloads inline. Identifiers,
    // credentials/recovery material, and demographics are never emitted.
    // health responses by eye while we RE the layout. Cap at 64 bytes —
    // larger frames already get the full hex dump from the verbose path.
    // Gated on the same test DEBUG_RING_DUMPF applies so the hex formatting
    // is skipped, not just the emission, when the flag is off.
    if (getLogLevel() >= LOG_LEVEL_DEBUG && isDebugFlagSet(DEBUG_RING | DEBUG_RING_DUMP)) {
      if (d.payloadLength > 0 && !sensitivePayload) {
        const size_t showMax = 64;
        const size_t show = d.payloadLength > showMax ? showMax : d.payloadLength;
        char pbuf[3 * showMax + 4];
        size_t off = 0;
        for (size_t i = 0; i < show && off + 3 < sizeof(pbuf); i++) {
          off += snprintf(pbuf + off, sizeof(pbuf) - off, "%02X ", d.payload[i]);
        }
        if (off > 0) pbuf[off - 1] = '\0';
        DEBUG_RING_DUMPF("[RING]   payload[%u]=[%s%s]",
                  (unsigned)d.payloadLength, pbuf,
                  d.payloadLength > showMax ? " ..." : "");

      } else if (d.payloadLength > 0) {
        DEBUG_RING_DUMPF("[RING]   payload[%u]=<redacted>",
                  (unsigned)d.payloadLength);
      }
    }

    const R1ParseError integrity = r1ValidateDecoded(d);
    if (integrity != R1_PARSE_OK) {
      // A decodable matching sleep-data header is enough to disprove "no
      // response", but not enough to trust its body. Record only candidate
      // presence so ACK + timeout cannot publish EMPTY. No packet ACK,
      // dedupe, ingestion, PRESENT, or transaction success occurs here.
      (void)ringRememberRejectedSleepCandidate(sActiveTransaction,
                                                generation, d);
      DEBUG_RING_PROTOCOLF("[RING] RX rejected before ingestion: %s",
                r1ParseErrorName(integrity));
    } else {
      const bool packetAckEligible =
          d.module == R1_MODULE_HEALTH && d.subCmd == R1_SUB_DAILY &&
          d.statusType == R1_STATUS_TYPE_NOTIFY &&
          d.statusMethod == R1_STATUS_METHOD_SET &&
          d.statusAck == R1_STATUS_ACK_OK &&
          r1ProfileSupportsDailyPacketAck(sR1Profile, d.cmd);
      // Preserve the established 2.2.7 ACK timing. On 2.2.9, however, the
      // capture proves ACKs only for successfully decoded metric pages; never
      // acknowledge a CRC-valid page that typed validation will discard.
      const bool packetAckPayloadValid =
          sR1Profile != R1_PROFILE_FW_2_2_9_0003 ||
          ringDailyPayloadValid(d);
      if (packetAckEligible && packetAckPayloadValid) {
        R1PacketAckDescriptor descriptor;
        if (r1PacketAckDescriptorFromDecoded(sR1Profile, d, descriptor)) {
          ringQueuePacketAck(descriptor, generation);
        }
      }

      const bool firstCopy = ringRememberRx(d, generation);
      if (!firstCopy) {
        DEBUG_RING_PROTOCOLF("[RING] RX duplicate ser=%u crc32=0x%08lX (typed ingestion skipped)",
                  (unsigned)d.serial, (unsigned long)d.crc32Received);
      } else {
        ringHandleSetupRx(d);
        RingControlTarget observedTarget = RING_CONTROL_NONE;
        G2RingObservedState observed = G2_RING_OBS_UNKNOWN;
        (void)ringApplyControlObservation(d, observedTarget, observed);
        ringHandleActiveRx(d, observedTarget, observed);

        // Typed consumers run only after the integrity/profile gate above.
        ringExtractTelemetryCache(d, generation);
        // r1AnnotatePayload re-parses the page purely to label one
        // DEBUG_RING_PROTOCOLF line — same gate as the macro.
        if (getLogLevel() >= LOG_LEVEL_DEBUG && isDebugFlagSet(DEBUG_RING | DEBUG_RING_PROTOCOL)) {
          if (d.payloadLength > 0) {
            char abuf[256];
            const size_t alen = r1AnnotatePayload(sR1Profile, d, abuf,
                                                  sizeof(abuf));
            if (alen > 0) DEBUG_RING_PROTOCOLF("[RING]   parsed: %s", abuf);
          }
        }
      }
    }
  } else {
    DEBUG_RING_PROTOCOLF("[RING] RX undecodeable len=%u (<17 B for envelope)",
              (unsigned)len);
  }

  // Raw byte dumps are gated by RING_DUMP (off unless the ring parent or
  // debugringdump is on); redaction below still applies regardless.
  if (sensitivePayload) {
    DEBUG_RING_DUMPF("[RING] RX bytes=<redacted sensitive payload> len=%u",
              (unsigned)len);
    return;
  }
  // Hex dump capped at 64 bytes — typical ring packets are <40 B; the
  // largest captured fixture (nvRecover) is ~80 B. Truncated lines get
  // a "…" suffix so the log doesn't grow unbounded on a future surprise.
  // Same gate as DEBUG_RING_DUMPF so the formatting is skipped too.
  if (getLogLevel() >= LOG_LEVEL_DEBUG && isDebugFlagSet(DEBUG_RING | DEBUG_RING_DUMP)) {
    const size_t showMax = 64;
    const size_t show = len > showMax ? showMax : len;
    char buf[3 * showMax + 4];
    size_t off = 0;
    for (size_t i = 0; i < show && off + 3 < sizeof(buf); i++) {
      off += snprintf(buf + off, sizeof(buf) - off, "%02X ", data[i]);
    }
    if (off > 0) buf[off - 1] = '\0';
    DEBUG_RING_DUMPF("[RING] RX bytes=[%s%s]", buf, len > showMax ? " ..." : "");
  }
}

// Notify callback shim — Arduino BLE hands us (char*, data, len, isNotify).
static void ringNotifyThunk(BLERemoteCharacteristic* /*c*/, uint8_t* data,
                            size_t len, bool /*isNotify*/) {
  QueueHandle_t queue = sRxQueue;
  uint32_t generation = 0;
  if (!queue || !data || len == 0 || len > R1_MAX_FRAME ||
      !ringSnapshotLink(generation)) {
    sRxQueueDropped = sRxQueueDropped + 1;
    return;
  }

  const int reserved = ringReserveRxSlab();
  if (reserved < 0) {
    sRxQueueDropped = sRxQueueDropped + 1;
    return;
  }
  const uint8_t slot = (uint8_t)reserved;
  // External-memory writes are deliberately outside both short critical
  // sections. Queue publication follows the complete frame copy, so the owner
  // never observes a partially initialized slab.
  RingRxFrame& frame = sRxSlabs[slot];
  frame.generation = generation;
  frame.length = (uint16_t)len;
  memcpy(frame.bytes, data, len);
  if (!ringPublishRxSlab(slot) || xQueueSend(queue, &slot, 0) != pdTRUE) {
    ringReleaseRxSlab(slot);
    sRxQueueDropped = sRxQueueDropped + 1;
    return;
  }
  ringWakeOwner();
}

// =============================================================================
// Serialized transaction owner + ACK-driven standard setup
// =============================================================================

static bool ringDeadlinePassed(uint32_t deadlineMs) {
  return deadlineMs != 0 && (int32_t)(millis() - deadlineMs) >= 0;
}

static void ringFinishHistorySweep(bool successful, uint8_t error) {
  const bool hadActiveSweep = sHistoryCoordinator.active;
  const bool exclusive = sHistoryCoordinator.exclusive;
  const uint8_t verifiedCount = sHistoryCoordinator.verifiedCount;
  const uint32_t sweepGeneration = sHistoryCoordinator.generation;
  if (hadActiveSweep) {
    g2HealthHistoryFetchFinished(successful, error);
  }
  sHistoryCoordinator = RingHistoryCoordinator{};
  portENTER_CRITICAL(&sTransportMux);
  if (hadActiveSweep) {
    ++sHistorySweepCompletionSequence;
    sHistorySweepLastSuccessful = successful;
    sHistorySweepLastError = error;
    sHistorySweepLastVerifiedCount = verifiedCount;
    if (exclusive &&
        sHealthPageRefresh.phase == RING_HEALTH_REFRESH_WAIT_DAILY &&
        sHealthPageRefresh.generation == sweepGeneration) {
      // Publish the tap-owned child's terminal result atomically with the
      // generic sweep. A link drop before the coordinator's next lap must not
      // replace a completed child result with a generic disconnect.
      sHealthPageRefresh.dailyCompleted = true;
      sHealthPageRefresh.dailySuccessful = successful;
      sHealthPageRefresh.dailyError = error;
      sHealthPageRefresh.dailyVerifiedCount = verifiedCount;
    }
  }
  // Defensive clear as well as the producer-side active coalesce: no request
  // observed during this fetch may launch a second session immediately after
  // its terminal pending snapshot was published.
  if (sHistoryRefreshGeneration == sweepGeneration) {
    sHistoryRefreshRequested = false;
    sHistoryRefreshForce = false;
    sHistoryRefreshExclusive = false;
    sHistoryRefreshGeneration = 0;
  }
  sHistorySweepActive = false;
  if (sHistorySweepClaimedGeneration == sweepGeneration) {
    sHistorySweepClaimed = false;
    sHistorySweepClaimedGeneration = 0;
  }
  portEXIT_CRITICAL(&sTransportMux);
}

static uint32_t ringStartGenerationAndSetup() {
  if (!gRing.connected || !gRing.writeChar || !sSetupDone) return 0;
  while (xSemaphoreTake(sSetupDone, 0) == pdTRUE) {}
  ringClockCustodyReset();

  RingTelemetryGuard telemetry;
  if (!telemetry.locked()) return 0;
  if (gRingDeviceAddress.length() > 0) {
    // Bind peer publication to the same telemetry fence used by direct daily
    // ingestion and reassembly finalization. Secure-store load/merge remains
    // on the main loop; this is only the RAM identity seed.
    g2HealthSetHistoryPeerId(gRingDeviceAddress.c_str());
  }
  // A sid-0x90 frame can arrive after new-link reset while setup is not yet
  // published online. Reassert direct ownership, clearing both its cache and
  // live-series writes, before generation/online become visible atomically.
  ringTransitionTelemetrySourceLocked(RING_TELEMETRY_DIRECT);
  portENTER_CRITICAL(&sTransportMux);
  const uint32_t generation = ringNextLinkGeneration(sLinkGeneration);
  if (sTelemetryGeneration != generation) {
    portEXIT_CRITICAL(&sTransportMux);
    ERROR_RINGF(
        "Setup refused — telemetry generation mismatch cache=%lu link=%lu",
        (unsigned long)sTelemetryGeneration, (unsigned long)generation);
    return 0;
  }
  const G2RingDesiredState healthDesired = sControlStatus.healthDesired;
  const G2RingDesiredState lowPowerDesired = sControlStatus.lowPowerDesired;
  sControlStatus = G2RingControlStatus{};
  sControlStatus.generation = generation;
  sControlStatus.setupState = G2_RING_SETUP_IDLE;
  sControlStatus.protocolProfile = G2_RING_PROFILE_UNKNOWN;
  sControlStatus.healthDesired = healthDesired;
  sControlStatus.lowPowerDesired = lowPowerDesired;
  // Health Preserve emits no 0x0E frame (GET is unproven). Low-power begins
  // pending until deviceInfo selects the exact profile; unsupported profiles
  // are then settled locally without a queue entry or encoder serial.
  sControlStatus.healthPending = healthDesired != G2_RING_PRESERVE;
  sControlStatus.lowPowerPending = true;
  // A new generation has no runtime profile yet. Clear the prior link's exact
  // identity in the same critical section as the new control state so setters
  // cannot transiently apply a stale 2.2.9 unsupported result to a 2.2.7
  // reconnect before the owner begins setup.
  sR1Profile = R1_PROFILE_UNKNOWN;
  sLinkGeneration = generation;
  sLinkOnline = true;
  sSetupRequested = true;
  portEXIT_CRITICAL(&sTransportMux);
  ringWakeOwner();
  return generation;
}

static bool ringRunStandardSetup() {
  const uint32_t generation = ringStartGenerationAndSetup();
  if (generation == 0) return false;
  const TickType_t wait = pdMS_TO_TICKS(RING_SETUP_TIMEOUT_MS * 4U + 4000U);
  if (xSemaphoreTake(sSetupDone, wait) != pdTRUE) {
    DEBUG_RING_SETUPF("[RING] setup coordinator wait timed out gen=%lu",
              (unsigned long)generation);
    return false;
  }
  G2RingControlStatus status{};
  g2RingGetControlStatus(status);
  return sLinkOnline && status.generation == generation &&
         status.setupState == G2_RING_SETUP_READY;
}

static void ringBeginOwnedGeneration(uint32_t generation) {
  // Owner-only state: the connect worker never touches reassembly memory.
  ringReasmReset();
  gR1Encoder.resetSerial();
  portENTER_CRITICAL(&sTransportMux);
  sR1Profile = R1_PROFILE_UNKNOWN;
  portEXIT_CRITICAL(&sTransportMux);
  sSetupOwner = RingSetupOwner{};
  sSetupOwner.active = true;
  sSetupOwner.generation = generation;
  sSetupOwner.stage = G2_RING_SETUP_AUTH;
  sSetupOwner.deadlineMs = millis() + RING_SETUP_TIMEOUT_MS;
  sActiveTransaction = RingActiveTransaction{};
  sActivePacketAck = RingActivePacketAck{};
  sPacketAckCount = 0;
  memset(sRxFingerprints, 0, sizeof(sRxFingerprints));
  sRxFingerprintCursor = 0;
  ringSetSetupState(G2_RING_SETUP_AUTH);
  DEBUG_RING_SETUPF("[RING] setup begin gen=%lu", (unsigned long)generation);
}

static void ringDropOwnedGeneration(uint32_t generation) {
  // A half-open model belongs to the generation being relinquished. Reset it
  // here on the sole owner task, never concurrently from the connect worker.
  ringReasmReset();
  if (sActiveTransaction.valid &&
      sActiveTransaction.intent.handle.generation == generation) {
    ringCompleteActive(G2_RING_TX_DISCONNECTED, G2_RING_ERR_DISCONNECTED);
  }
  ringMarkGenerationDisconnected(generation);
  if (sHistoryCoordinator.active &&
      sHistoryCoordinator.generation == generation) {
    ringFinishHistorySweep(false, G2_RING_ERR_DISCONNECTED);
  }
  portENTER_CRITICAL(&sTransportMux);
  if (sHistoryRefreshGeneration == generation) {
    sHistoryRefreshRequested = false;
    sHistoryRefreshForce = false;
    sHistoryRefreshExclusive = false;
    sHistoryRefreshGeneration = 0;
  }
  if (sHistorySweepClaimedGeneration == generation) {
    sHistorySweepClaimed = false;
    sHistorySweepClaimedGeneration = 0;
  }
  if (sHealthPageRefresh.generation == generation &&
      sHealthPageRefresh.phase != RING_HEALTH_REFRESH_IDLE &&
      sHealthPageRefresh.phase != RING_HEALTH_REFRESH_TERMINAL) {
    const bool completedBeforeDisconnect =
        sHealthPageRefresh.dailyCompleted &&
        sHealthPageRefresh.dailySuccessful &&
        sHealthPageRefresh.deviceStatusCompleted &&
        sHealthPageRefresh.deviceStatusVerified;
    sHealthPageRefresh.terminalSuccessful = completedBeforeDisconnect;
    sHealthPageRefresh.terminalPartial = !completedBeforeDisconnect &&
        (sHealthPageRefresh.dailyVerifiedCount != 0 ||
         sHealthPageRefresh.deviceStatusVerified);
    sHealthPageRefresh.terminalError = completedBeforeDisconnect
        ? (uint8_t)G2_RING_ERR_NONE
        : ringHealthPageRefreshResolvedError(
              sHealthPageRefresh, G2_RING_ERR_DISCONNECTED);
    sHealthPageRefresh.phase = RING_HEALTH_REFRESH_TERMINAL;
  }
  portEXIT_CRITICAL(&sTransportMux);
  sActivePacketAck = RingActivePacketAck{};
  sPacketAckCount = 0;
  if (sSetupOwner.active && sSetupOwner.generation == generation &&
      generation == sLinkGeneration && !sLinkOnline) {
    ringFinishSetup(false, G2_RING_ERR_DISCONNECTED);
  }
  sSetupOwner = RingSetupOwner{};
  gR1Encoder.resetSerial();
}

static bool ringServicePacketAck() {
  if (!sActivePacketAck.valid && sPacketAckCount > 0) {
    sActivePacketAck.valid = true;
    sActivePacketAck.pending = sPacketAckQueue[0];
    for (uint8_t i = 1; i < sPacketAckCount; ++i) {
      sPacketAckQueue[i - 1] = sPacketAckQueue[i];
    }
    --sPacketAckCount;
  }
  if (!sActivePacketAck.valid) return false;
  if (sActivePacketAck.pending.generation != sLinkGeneration) {
    sActivePacketAck = RingActivePacketAck{};
    return true;
  }
  if (!sActivePacketAck.frameReady) {
    sActivePacketAck.frame = gR1Encoder.buildPacketAck(
        sR1Profile, sActivePacketAck.pending.descriptor);
    if (sActivePacketAck.frame.length == 0) {
      sActivePacketAck = RingActivePacketAck{};
      return true;
    }
    sActivePacketAck.frameReady = true;
  }
  const RingWriteResult result = ringOwnerWrite(
      sActivePacketAck.frame.bytes, sActivePacketAck.frame.length);
  if (result == RING_WRITE_OK) {
    DEBUG_RING_TXNF("[RING] TX packetAck ser=%u rxSer=%u",
              (unsigned)sActivePacketAck.frame.serial,
              (unsigned)sActivePacketAck.pending.descriptor.receivedSerial());
    sActivePacketAck = RingActivePacketAck{};
  } else if (result == RING_WRITE_DISCONNECTED) {
    sActivePacketAck = RingActivePacketAck{};
  }
  return true;
}

// Dark-clock solicit from INSIDE the setup ritual. Custody rule 1 makes the
// TIME stage hold rather than push an invalid epoch, waiting for
// g2RingTimeSyncTick() to adopt the ring's clock — but the tick can only
// adopt a stamp it has SEEN, the ring volunteers nothing unpolled, and the
// tick's own solicit is gated on sRingSetupDone. Without this probe the wait
// is circular and every dark-boot connect dies at the stage deadline with
// clock-unavailable. So the setup owner asks for a profile-safe HR sample here
// (2.2.9 DAILY, legacy POINT). A plausible response stamp feeds sRingTsSeen no
// matter what setup stage is active, and adoption follows within one 500 ms
// tick—comfortably inside the stage's 7 s deadline (HW legs answered in
// ~250 ms). Only the capture-established legacy POINT layout can prove a dark
// ring below. A silent ring, or a factory-dark 2.2.9 DAILY response whose
// unanchored timestamp cannot be interpreted safely, ends fail-closed with
// clock-unavailable.
// Probes consume encoder serials, so systemTime rides a higher serial on dark
// boots only (known-good on HW).
static void ringSetupServiceDarkProbe() {
  if (sSetupOwner.darkProbesSent >= 3) return;
  const uint32_t nowMs = millis();
  if (sSetupOwner.darkProbeLastMs != 0 &&
      (uint32_t)(nowMs - sSetupOwner.darkProbeLastMs) < 1500u) return;
  const uint8_t subCmd = sR1Profile == R1_PROFILE_FW_2_2_9_0003
                             ? R1_SUB_DAILY
                             : R1_SUB_POINT;
  const R1Frame probe =
      gR1Encoder.buildHealthQuery(sR1Profile, R1_CMD_HEARTRATE,
                                  subCmd);
  if (probe.length == 0) return;
  const RingWriteResult result = ringOwnerWrite(probe.bytes, probe.length);
  if (result == RING_WRITE_DISCONNECTED) {
    ringFinishSetup(false, G2_RING_ERR_DISCONNECTED);
    return;
  }
  sSetupOwner.darkProbeLastMs = nowMs;  // rate-limit BUSY/FAILED retries too
  if (result != RING_WRITE_OK) return;
  sSetupOwner.darkProbesSent = (uint8_t)(sSetupOwner.darkProbesSent + 1);
  DEBUG_RING_SETUPF("[RING] setup dark-clock probe TX hr/%s ser=%u (%u/3)",
            subCmd == R1_SUB_DAILY ? "daily" : "point",
            (unsigned)probe.serial, (unsigned)sSetupOwner.darkProbesSent);
}

// TRUE only when an accepted probe answered THIS link with a pre-2020 stamp:
// positive evidence the ring's clock is as dark as ours. Current 2.2.9 DAILY
// parsing intentionally cannot create that evidence from an unanchored clock.
// Custody rule 1 protects a
// GOOD ring clock from a dark host — echoing darkness at darkness destroys
// nothing, so the TIME stage completes the ritual with our dark epoch
// instead of dropping the link (the July design's explicit fallback). The
// drift-based corrective push then trues ring and host together the moment
// any real source lands mid-session. A SILENT ring proves nothing and stays
// fail-closed. sRingTsSeen is per-link (ringClockCustodyReset), so the stamp
// judged here always came from this connection's own probes.
static bool ringSetupRingProvenDark() {
  if (sSetupOwner.darkProbesSent == 0) return false;
  const uint32_t ts = sRingTsSeen;
  return ts != 0 && !Clock::isValidEpoch((time_t)ts);
}

static void ringServiceSetup() {
  if (!sSetupOwner.active) return;
  if (ringDeadlinePassed(sSetupOwner.deadlineMs)) {
    const uint8_t error = sSetupOwner.stage == G2_RING_SETUP_TIME &&
                                  !Clock::isValidEpoch(time(nullptr))
                              ? G2_RING_ERR_CLOCK_UNAVAILABLE
                              : G2_RING_ERR_TIMEOUT;
    ringFinishSetup(false, error);
    return;
  }

  if (!sSetupOwner.frameReady) {
    switch (sSetupOwner.stage) {
      case G2_RING_SETUP_AUTH:
        sSetupOwner.frame = gR1Encoder.buildPairAuth();
        break;
      case G2_RING_SETUP_DEVICE_INFO:
        sSetupOwner.frame = gR1Encoder.buildDeviceInfoQuery();
        break;
      case G2_RING_SETUP_TIME: {
        const time_t now = time(nullptr);
        if (!Clock::isValidEpoch(now)) {
          if (!ringSetupRingProvenDark()) {
            // Hold while soliciting the ring's clock; adoption on the main
            // loop makes the epoch valid, a dark answer flips the branch
            // below, and a silent ring rides the stage deadline into
            // clock-unavailable (fail closed — never push blind).
            ringSetupServiceDarkProbe();
            return;
          }
          DEBUG_RING_SETUPF("[RING] ring clock is dark too (ts=%lu) — completing "
                    "ritual with dark epoch; drift push trues both when "
                    "time arrives",
                    (unsigned long)sRingTsSeen);
        }
        int16_t timezoneMinutes = 0;
        if (!ringConfiguredTimezoneMinutes(timezoneMinutes)) {
          ringFinishSetup(false, G2_RING_ERR_ENCODE);
          return;
        }
        sSetupOwner.frame = gR1Encoder.buildSyncTime(timezoneMinutes,
                                                     (uint32_t)now);
        break;
      }
      case G2_RING_SETUP_ADV_START: {
        uint8_t right[6]{};
        uint8_t left[6]{};
        const bool haveRight = g2GetRightTempleMac(right);
        const bool haveLeft = g2GetLeftTempleMac(left);
        portENTER_CRITICAL(&sTransportMux);
        sControlStatus.advIdentityKnown = haveRight && haveLeft;
        portEXIT_CRITICAL(&sTransportMux);
        if (!haveRight || !haveLeft) {
          ringFinishSetup(false, G2_RING_ERR_IDENTITY_UNKNOWN);
          return;
        }
        sSetupOwner.frame = gR1Encoder.buildAdvStart(sR1Profile, right, left);
        break;
      }
      default:
        ringFinishSetup(false, G2_RING_ERR_ENCODE);
        return;
    }
    if (sSetupOwner.frame.length == 0) {
      ringFinishSetup(false, sR1Profile == R1_PROFILE_UNKNOWN &&
                                     sSetupOwner.stage == G2_RING_SETUP_ADV_START
                                 ? G2_RING_ERR_PROFILE_UNKNOWN
                                 : G2_RING_ERR_ENCODE);
      return;
    }
    sSetupOwner.frameReady = true;
  }

  if (!sSetupOwner.written) {
    const RingWriteResult result = ringOwnerWrite(
        sSetupOwner.frame.bytes, sSetupOwner.frame.length);
    if (result == RING_WRITE_DISCONNECTED) {
      ringFinishSetup(false, G2_RING_ERR_DISCONNECTED);
      return;
    }
    if (result != RING_WRITE_OK) return;
    sSetupOwner.written = true;
    if (sSetupOwner.stage == G2_RING_SETUP_TIME) {
      sLastPushedEpoch = (uint32_t)time(nullptr);
      sLastPushedAtMs = millis();
      // Record the tz the ritual just sent so the tick does not immediately
      // re-push it as a "timezone changed" correction. Connect-time: stable.
      int16_t tzMin = 0;
      if (ringConfiguredTimezoneMinutes(tzMin)) sLastPushedTzMin = tzMin;
    }
    DEBUG_RING_SETUPF("[RING] setup TX stage=%s ser=%u len=%u",
              g2RingSetupStateName(sSetupOwner.stage),
              (unsigned)sSetupOwner.frame.serial,
              (unsigned)sSetupOwner.frame.length);
  }
}

static R1Frame ringBuildIntentFrame(const RingIntent& intent,
                                    uint8_t& error) {
  error = G2_RING_ERR_ENCODE;
  switch (intent.kind) {
    case RING_INTENT_RAW: {
      const uint8_t* payload = nullptr;
      if (intent.payloadLen > 0) {
        if (intent.payloadSlot >= RING_RAW_PAYLOAD_SLOTS ||
            !ringRawPayloadOwned(intent.payloadSlot)) return R1Frame{};
        payload = sRawPayloadBytes[intent.payloadSlot];
      }
      return gR1Encoder.build(intent.module, intent.cmd, intent.subCmd,
                              intent.statusType, intent.statusMethod,
                              intent.statusAck, payload, intent.payloadLen);
    }
    case RING_INTENT_PAIR_AUTH:
      return gR1Encoder.buildPairAuth();
    case RING_INTENT_DEVICE_INFO:
      return gR1Encoder.buildDeviceInfoQuery();
    case RING_INTENT_SYNC_TIME: {
      if (!Clock::isValidEpoch((time_t)intent.epoch)) {
        error = G2_RING_ERR_CLOCK_UNAVAILABLE;
        return R1Frame{};
      }
      // Official-app captures send the configured signed minute offset here;
      // the ring then keys local daily pages to local midnight expressed in
      // UTC. Sending zero forced HardwareOne fetches onto UTC day boundaries.
      // tz was captured + validated at enqueue (ringEnqueueTimeSync), so the
      // frame reflects the offset in effect when the push was decided — not
      // whatever the setting happens to be by the time this retry-safe build
      // runs (which may be a later owner-task pass than the enqueue).
      return gR1Encoder.buildSyncTime(intent.tzMin, intent.epoch);
    }
    case RING_INTENT_ADV_START: {
      uint8_t right[6]{};
      uint8_t left[6]{};
      if (!g2GetRightTempleMac(right) || !g2GetLeftTempleMac(left)) {
        error = G2_RING_ERR_IDENTITY_UNKNOWN;
        return R1Frame{};
      }
      if (sR1Profile == R1_PROFILE_UNKNOWN) {
        error = G2_RING_ERR_PROFILE_UNKNOWN;
        return R1Frame{};
      }
      return gR1Encoder.buildAdvStart(sR1Profile, right, left);
    }
    case RING_INTENT_HEALTH_QUERY:
      if (sR1Profile == R1_PROFILE_UNKNOWN) {
        error = G2_RING_ERR_PROFILE_UNKNOWN;
        return R1Frame{};
      }
      if (!r1ProfileSupportsHealthQuery(sR1Profile, intent.cmd,
                                        intent.subCmd)) {
        error = G2_RING_ERR_FEATURE_UNSUPPORTED;
        return R1Frame{};
      }
      return gR1Encoder.buildHealthQuery(sR1Profile, intent.cmd,
                                         intent.subCmd);
    case RING_INTENT_HEALTH_COLLECTION_SET:
      if (sR1Profile == R1_PROFILE_UNKNOWN) {
        error = G2_RING_ERR_PROFILE_UNKNOWN;
        return R1Frame{};
      }
      if (!r1ProfileSupportsHealthCollectionSet(
              sR1Profile, intent.desired == G2_RING_ON)) {
        error = G2_RING_ERR_FEATURE_UNSUPPORTED;
        return R1Frame{};
      }
      if (!Clock::isValidEpoch((time_t)intent.epoch)) {
        error = G2_RING_ERR_CLOCK_UNAVAILABLE;
        return R1Frame{};
      }
      return gR1Encoder.buildHealthCollectionSet(
          sR1Profile, intent.epoch, intent.desired == G2_RING_ON);
    case RING_INTENT_LOW_POWER_QUERY:
      if (sR1Profile == R1_PROFILE_UNKNOWN) {
        error = G2_RING_ERR_PROFILE_UNKNOWN;
        return R1Frame{};
      }
      if (!r1ProfileSupportsLowPower(sR1Profile)) {
        error = G2_RING_ERR_FEATURE_UNSUPPORTED;
        return R1Frame{};
      }
      return gR1Encoder.buildLowPowerQuery(sR1Profile);
    case RING_INTENT_LOW_POWER_SET:
      if (sR1Profile == R1_PROFILE_UNKNOWN) {
        error = G2_RING_ERR_PROFILE_UNKNOWN;
        return R1Frame{};
      }
      if (!r1ProfileSupportsLowPower(sR1Profile)) {
        error = G2_RING_ERR_FEATURE_UNSUPPORTED;
        return R1Frame{};
      }
      if (!Clock::isValidEpoch((time_t)intent.epoch)) {
        error = G2_RING_ERR_CLOCK_UNAVAILABLE;
        return R1Frame{};
      }
      return gR1Encoder.buildLowPowerSet(
          sR1Profile, intent.epoch, intent.desired == G2_RING_ON);
    default:
      return R1Frame{};
  }
}

static void ringServiceNormalTransaction() {
  if (!sActiveTransaction.valid) {
    RingIntent intent{};
    if (!ringPopIntent(intent)) return;
    if (intent.handle.generation != sLinkGeneration) {
      ringUpdateTransaction(intent.handle, G2_RING_TX_DISCONNECTED,
                            G2_RING_ERR_DISCONNECTED);
      if (intent.payloadSlot < RING_RAW_PAYLOAD_SLOTS)
        ringReleaseRawPayload(intent.payloadSlot);
      return;
    }
    sActiveTransaction = RingActiveTransaction{};
    sActiveTransaction.valid = true;
    sActiveTransaction.intent = intent;
    sActiveTransaction.deadlineMs = millis() + intent.timeoutMs;
  }

  if (!sActiveTransaction.frameReady) {
    uint8_t error = G2_RING_ERR_ENCODE;
    sActiveTransaction.frame = ringBuildIntentFrame(
        sActiveTransaction.intent, error);
    if (sActiveTransaction.frame.length == 0) {
      ringCompleteActive(G2_RING_TX_REFUSED, error);
      return;
    }
    sActiveTransaction.frameReady = true;
  }

  if (!sActiveTransaction.written) {
    const RingWriteResult result = ringOwnerWrite(
        sActiveTransaction.frame.bytes, sActiveTransaction.frame.length);
    if (result == RING_WRITE_DISCONNECTED) {
      ringCompleteActive(G2_RING_TX_DISCONNECTED, G2_RING_ERR_DISCONNECTED);
      return;
    }
    if (result == RING_WRITE_OK) {
      sActiveTransaction.written = true;
      sActiveTransaction.deadlineMs = millis() +
                                      sActiveTransaction.intent.timeoutMs;
      if (!sActiveTransaction.readbackPhase) {
        ringUpdateTransaction(sActiveTransaction.intent.handle,
                              G2_RING_TX_WRITTEN);
      }
      if (sActiveTransaction.intent.kind == RING_INTENT_SYNC_TIME) {
        sLastPushedEpoch = sActiveTransaction.intent.epoch;
        sLastPushedAtMs = millis();
        sLastPushedTzMin = sActiveTransaction.intent.tzMin;
      }
      DEBUG_RING_TXNF("[RING] TX transaction id=%lu gen=%lu ser=%u %s/%s/%s%s",
                (unsigned long)sActiveTransaction.intent.handle.id,
                (unsigned long)sActiveTransaction.intent.handle.generation,
                (unsigned)sActiveTransaction.frame.serial,
                r1ModuleName(sActiveTransaction.intent.module),
                r1CmdName(sActiveTransaction.intent.module,
                          sActiveTransaction.intent.cmd),
                r1SubCmdName(sActiveTransaction.intent.module,
                             sActiveTransaction.intent.cmd,
                             sActiveTransaction.intent.subCmd),
                sActiveTransaction.readbackPhase ? " readback" : "");
    }
  }

  if (sActiveTransaction.valid &&
      ringDeadlinePassed(sActiveTransaction.deadlineMs)) {
    const RingIntent& intent = sActiveTransaction.intent;
    if (intent.module == R1_MODULE_HEALTH && intent.cmd == R1_CMD_SLEEP &&
        intent.subCmd == R1_SUB_DAILY && sActiveTransaction.commandAcked &&
        !sActiveTransaction.sleepDataCandidateSeen) {
      // Capture-proven empty-command ACK plus a full response window with no
      // data is the only supported evidence for an empty sleep metric.
      g2HealthHistorySetSleepState(R1_HISTORY_SLEEP_EMPTY);
      ringCompleteActive(G2_RING_TX_VERIFIED, G2_RING_ERR_NONE);
      return;
    }
    ringCompleteActive(G2_RING_TX_TIMEOUT, G2_RING_ERR_TIMEOUT);
  }
}

static void ringServiceControlReconciliation() {
  bool healthPending = false;
  bool lowPending = false;
  G2RingDesiredState healthDesired = G2_RING_PRESERVE;
  G2RingDesiredState lowDesired = G2_RING_PRESERVE;
  portENTER_CRITICAL(&sTransportMux);
  healthPending = sControlStatus.healthPending &&
                  sControlStatus.healthTransaction.id == 0;
  lowPending = sControlStatus.lowPowerPending &&
               sControlStatus.lowPowerTransaction.id == 0;
  healthDesired = sControlStatus.healthDesired;
  lowDesired = sControlStatus.lowPowerDesired;
  portEXIT_CRITICAL(&sTransportMux);
  if (healthPending) {
    (void)ringQueueControlIntent(RING_CONTROL_HEALTH, healthDesired, nullptr);
  }
  if (lowPending) {
    (void)ringQueueControlIntent(RING_CONTROL_LOW_POWER, lowDesired, nullptr);
  }
}

static void ringServiceHistoryCoordinator() {
  static constexpr uint8_t kMetrics[] = {
    R1_CMD_HEARTRATE, R1_CMD_HRV, R1_CMD_SPO2, R1_CMD_SLEEP,
    R1_CMD_ACTIVITY,
  };

  G2RingDesiredState healthDesired;
  portENTER_CRITICAL(&sTransportMux);
  healthDesired = sControlStatus.healthDesired;
  portEXIT_CRITICAL(&sTransportMux);
  if (sHistoryCoordinator.active && !sHistoryCoordinator.force &&
      healthDesired == G2_RING_OFF) {
    DEBUG_RING_HEALTHF("[RING] normal history sweep cancelled: health collection desired Off");
    ringFinishHistorySweep(false, G2_RING_ERR_NONE);
    return;
  }

  if (!sHistoryCoordinator.active) {
    bool requested = false;
    bool force = false;
    bool exclusive = false;
    uint32_t requestGeneration = 0;
    portENTER_CRITICAL(&sTransportMux);
    requested = sHistoryRefreshRequested;
    force = sHistoryRefreshForce;
    exclusive = sHistoryRefreshExclusive;
    requestGeneration = sHistoryRefreshGeneration;
    if (requested) {
      sHistoryRefreshRequested = false;
      sHistoryRefreshForce = false;
      sHistoryRefreshExclusive = false;
      sHistoryRefreshGeneration = 0;
      sHistorySweepClaimed = true;
      sHistorySweepClaimedGeneration = requestGeneration;
    }
    portEXIT_CRITICAL(&sTransportMux);
    if (!requested) return;
    if (!force && healthDesired == G2_RING_OFF) {
      DEBUG_RING_HEALTHF("[RING] normal history refresh skipped: health collection desired Off");
      portENTER_CRITICAL(&sTransportMux);
      sHistorySweepClaimed = false;
      sHistorySweepClaimedGeneration = 0;
      portEXIT_CRITICAL(&sTransportMux);
      return;
    }
    if (!force) {
      const uint32_t nowMs = millis();
      if (sLastHistoryAttemptMs != 0 &&
          (uint32_t)(nowMs - sLastHistoryAttemptMs) < 10U * 60U * 1000U) {
        DEBUG_RING_HEALTHF("[RING] normal history refresh throttled (<10 min since attempt)");
        portENTER_CRITICAL(&sTransportMux);
        sHistorySweepClaimed = false;
        sHistorySweepClaimedGeneration = 0;
        portEXIT_CRITICAL(&sTransportMux);
        return;
      }
      G2HealthHistorySummary summary{};
      g2HealthHistoryGetSummary(summary);
      const uint32_t newest = summary.lastSuccessEpoch > summary.lastPartialEpoch
                                  ? summary.lastSuccessEpoch
                                  : summary.lastPartialEpoch;
      const time_t now = time(nullptr);
      if (newest != 0 && Clock::isValidEpoch(now) &&
          Clock::isValidEpoch((time_t)newest)) {
        const uint32_t age = (uint32_t)now >= newest
                                 ? (uint32_t)now - newest
                                 : 0;
        if (age < 10U * 60U) {
          DEBUG_RING_HEALTHF("[RING] normal history refresh skipped: RAM history is %lus old",
                    (unsigned long)age);
          portENTER_CRITICAL(&sTransportMux);
          sHistorySweepClaimed = false;
          sHistorySweepClaimedGeneration = 0;
          portEXIT_CRITICAL(&sTransportMux);
          return;
        }
      }
    }
    sHistoryCoordinator = RingHistoryCoordinator{};
    sHistoryCoordinator.active = true;
    sHistoryCoordinator.force = force;
    sHistoryCoordinator.exclusive = exclusive;
    sHistoryCoordinator.generation = requestGeneration;
    sLastHistoryAttemptMs = millis();
    portENTER_CRITICAL(&sTransportMux);
    sHistorySweepActive = true;
    sHistorySweepClaimed = false;
    sHistorySweepClaimedGeneration = 0;
    // Coalesce a request that raced the gate checks/start transition.
    if (sHistoryRefreshGeneration == requestGeneration) {
      sHistoryRefreshRequested = false;
      sHistoryRefreshForce = false;
      sHistoryRefreshExclusive = false;
      sHistoryRefreshGeneration = 0;
    }
    portEXIT_CRITICAL(&sTransportMux);
    g2HealthHistoryFetchStarted();
    DEBUG_RING_HEALTHF("[RING] history sweep started%s", force ? " (force)" : "");
  }

  if (sHistoryCoordinator.generation != sLinkGeneration || !sLinkOnline) {
    ringFinishHistorySweep(false, G2_RING_ERR_DISCONNECTED);
    return;
  }

  if (sHistoryCoordinator.transaction.id != 0) {
    G2RingTransactionStatus status{};
    if (!g2RingGetTransactionStatus(sHistoryCoordinator.transaction, status) ||
        status.completedAtMs == 0) return;
    if (status.state == G2_RING_TX_VERIFIED) {
      ++sHistoryCoordinator.verifiedCount;
      // Publish composite progress stage-by-stage so a disconnect before the
      // sweep's terminal snapshot is still reported as a useful partial
      // refresh rather than as if no trusted response arrived.
      portENTER_CRITICAL(&sTransportMux);
      if (sHealthPageRefresh.phase == RING_HEALTH_REFRESH_WAIT_DAILY &&
          sHealthPageRefresh.generation == sHistoryCoordinator.generation &&
          sHistoryCoordinator.verifiedCount >
              sHealthPageRefresh.dailyVerifiedCount) {
        sHealthPageRefresh.dailyVerifiedCount =
            sHistoryCoordinator.verifiedCount;
      }
      portEXIT_CRITICAL(&sTransportMux);
    } else if (sHistoryCoordinator.firstError == G2_RING_ERR_NONE) {
      sHistoryCoordinator.firstError =
          status.errorCode != G2_RING_ERR_NONE
              ? status.errorCode
              : (uint8_t)G2_RING_ERR_TIMEOUT;
    }
    ++sHistoryCoordinator.metricIndex;
    sHistoryCoordinator.transaction = G2RingTransactionHandle{};
  }

  if (sHistoryCoordinator.metricIndex >= sizeof(kMetrics)) {
    const bool success = sHistoryCoordinator.firstError == G2_RING_ERR_NONE;
    const uint8_t error = sHistoryCoordinator.firstError;
    DEBUG_RING_HEALTHF("[RING] history sweep %s error=%s",
              success ? "complete" : "partial",
              g2RingTransactionErrorName(error));
    ringFinishHistorySweep(success, error);
    return;
  }

  G2RingTransactionHandle handle{};
  const uint8_t cmd = kMetrics[sHistoryCoordinator.metricIndex];
  const uint8_t coalesceKey = sHistoryCoordinator.exclusive
                                  ? 0
                                  : (uint8_t)(0x20 + cmd);
  if (ringEnqueueHealthDataQuery(cmd, R1_SUB_DAILY,
                                 coalesceKey, &handle)) {
    sHistoryCoordinator.transaction = handle;
  }
}

static void ringFinishHealthPageRefresh(uint32_t id, uint32_t generation,
                                        bool successful, bool partial,
                                        uint8_t error) {
  portENTER_CRITICAL(&sTransportMux);
  if (sHealthPageRefresh.id == id &&
      sHealthPageRefresh.generation == generation &&
      sHealthPageRefresh.phase != RING_HEALTH_REFRESH_TERMINAL) {
    sHealthPageRefresh.terminalSuccessful = successful;
    sHealthPageRefresh.terminalPartial = partial && !successful;
    sHealthPageRefresh.terminalError =
        successful ? (uint8_t)G2_RING_ERR_NONE : error;
    sHealthPageRefresh.phase = RING_HEALTH_REFRESH_TERMINAL;
  }
  portEXIT_CRITICAL(&sTransportMux);
}

// Poll Now is one generation-bound composite operation. The ordinary history
// coordinator remains the sole owner of DAILY sequencing, while this layer
// ensures a tap cannot adopt a sweep that began earlier and cannot report
// success until its own DAILY sweep and its correlated deviceStatus both
// finish with typed-verified responses.
static void ringServiceHealthPageRefreshCoordinator() {
  RingHealthPageRefresh refresh{};
  bool linkOnline = false;
  R1ProtocolProfile profile = R1_PROFILE_UNKNOWN;
  portENTER_CRITICAL(&sTransportMux);
  refresh = sHealthPageRefresh;
  linkOnline = sLinkOnline && sLinkGeneration == refresh.generation;
  profile = sR1Profile;
  portEXIT_CRITICAL(&sTransportMux);

  if (refresh.phase == RING_HEALTH_REFRESH_IDLE ||
      refresh.phase == RING_HEALTH_REFRESH_TERMINAL) {
    return;
  }
  if (!linkOnline) {
    ringFinishHealthPageRefresh(
        refresh.id, refresh.generation, false,
        refresh.dailyVerifiedCount != 0 || refresh.deviceStatusVerified,
        G2_RING_ERR_DISCONNECTED);
    return;
  }
  if (!r1ProfileSupportsHealthPageRefresh(profile)) {
    ringFinishHealthPageRefresh(
        refresh.id, refresh.generation, false,
        refresh.dailyVerifiedCount != 0 || refresh.deviceStatusVerified,
        G2_RING_ERR_FEATURE_UNSUPPORTED);
    return;
  }
  if (refresh.phase == RING_HEALTH_REFRESH_WAIT_PRIOR) {
    if (ringDeadlinePassed(refresh.deadlineMs)) {
      ringFinishHealthPageRefresh(refresh.id, refresh.generation, false,
                                  false, G2_RING_ERR_TIMEOUT);
      return;
    }
    bool admitted = false;
    portENTER_CRITICAL(&sTransportMux);
    if (sHealthPageRefresh.id == refresh.id &&
        sHealthPageRefresh.generation == refresh.generation &&
        sHealthPageRefresh.phase == RING_HEALTH_REFRESH_WAIT_PRIOR &&
        !sHistorySweepActive && !sHistorySweepClaimed &&
        !sHistoryRefreshRequested) {
      sHealthPageRefresh.dailyBaselineSequence =
          sHistorySweepCompletionSequence;
      // Waiting behind an earlier legal sweep must not consume this request's
      // own five-stage DAILY + deviceStatus execution budget.
      sHealthPageRefresh.deadlineMs =
          millis() + RING_HEALTH_PAGE_REFRESH_TIMEOUT_MS;
      sHealthPageRefresh.phase = RING_HEALTH_REFRESH_WAIT_DAILY;
      sHistoryRefreshRequested = true;
      sHistoryRefreshForce = true;
      sHistoryRefreshExclusive = true;
      sHistoryRefreshGeneration = refresh.generation;
      admitted = true;
    }
    portEXIT_CRITICAL(&sTransportMux);
    if (admitted) {
      DEBUG_RING_HEALTHF(
          "[RING] Health-page refresh id=%lu admitted after prior sweep",
          (unsigned long)refresh.id);
      ringWakeOwner();
    }
    return;
  }

  if (refresh.phase == RING_HEALTH_REFRESH_WAIT_DAILY) {
    bool dailyFinished = false;
    bool dailySuccessful = false;
    uint8_t dailyError = G2_RING_ERR_NONE;
    uint8_t dailyVerified = 0;
    portENTER_CRITICAL(&sTransportMux);
    if (sHealthPageRefresh.id == refresh.id &&
        sHealthPageRefresh.generation == refresh.generation &&
        sHealthPageRefresh.phase == RING_HEALTH_REFRESH_WAIT_DAILY &&
        sHistorySweepCompletionSequence !=
            sHealthPageRefresh.dailyBaselineSequence) {
      dailyFinished = true;
      dailySuccessful = sHistorySweepLastSuccessful;
      dailyError = sHistorySweepLastError;
      dailyVerified = sHistorySweepLastVerifiedCount;
      sHealthPageRefresh.dailyCompleted = true;
      sHealthPageRefresh.dailySuccessful = dailySuccessful;
      sHealthPageRefresh.dailyError = dailyError;
      sHealthPageRefresh.dailyVerifiedCount = dailyVerified;
      sHealthPageRefresh.phase = RING_HEALTH_REFRESH_QUEUE_DEVICE_STATUS;
    }
    portEXIT_CRITICAL(&sTransportMux);
    if (dailyFinished) {
      DEBUG_RING_HEALTHF(
          "[RING] Health-page refresh id=%lu DAILY done ok=%d verified=%u error=%s",
          (unsigned long)refresh.id, (int)dailySuccessful,
          (unsigned)dailyVerified, g2RingTransactionErrorName(dailyError));
      ringWakeOwner();
    } else if (ringDeadlinePassed(refresh.deadlineMs)) {
      uint8_t verified = refresh.dailyVerifiedCount;
      // Preserve stages already verified by this still-active sweep.
      if (sHistoryCoordinator.active &&
          sHistoryCoordinator.generation == refresh.generation &&
          sHistoryCoordinator.verifiedCount > verified) {
        verified = sHistoryCoordinator.verifiedCount;
      }
      ringFinishHealthPageRefresh(refresh.id, refresh.generation, false,
                                  verified != 0, G2_RING_ERR_TIMEOUT);
    }
    return;
  }

  if (refresh.phase == RING_HEALTH_REFRESH_QUEUE_DEVICE_STATUS) {
    if (ringDeadlinePassed(refresh.deadlineMs)) {
      ringFinishHealthPageRefresh(
          refresh.id, refresh.generation, false,
          refresh.dailyVerifiedCount != 0, G2_RING_ERR_TIMEOUT);
      return;
    }
    G2RingTransactionHandle handle{};
    // This key is private to the composite request. It must not coalesce with
    // a dashboard/logging deviceStatus that predates the tap.
    if (!ringEnqueueSystemQuery(R1_SUB_DEVICE_STATUS, 0x45, &handle)) return;

    portENTER_CRITICAL(&sTransportMux);
    if (sHealthPageRefresh.id == refresh.id &&
        sHealthPageRefresh.generation == refresh.generation &&
        sHealthPageRefresh.phase ==
            RING_HEALTH_REFRESH_QUEUE_DEVICE_STATUS) {
      sHealthPageRefresh.deviceStatus = handle;
      sHealthPageRefresh.phase = RING_HEALTH_REFRESH_WAIT_DEVICE_STATUS;
    }
    portEXIT_CRITICAL(&sTransportMux);
    DEBUG_RING_HEALTHF(
        "[RING] Health-page refresh id=%lu deviceStatus tx=%lu",
        (unsigned long)refresh.id, (unsigned long)handle.id);
    return;
  }

  if (refresh.phase != RING_HEALTH_REFRESH_WAIT_DEVICE_STATUS ||
      refresh.deviceStatus.id == 0) {
    return;
  }

  G2RingTransactionStatus status{};
  if (!g2RingGetTransactionStatus(refresh.deviceStatus, status) ||
      status.completedAtMs == 0) {
    if (ringDeadlinePassed(refresh.deadlineMs)) {
      ringFinishHealthPageRefresh(
          refresh.id, refresh.generation, false,
          refresh.dailyVerifiedCount != 0, G2_RING_ERR_TIMEOUT);
    }
    return;
  }
  const bool deviceVerified = status.state == G2_RING_TX_VERIFIED;
  const bool successful = refresh.dailySuccessful && deviceVerified;
  const bool partial = !successful &&
      (refresh.dailyVerifiedCount != 0 || deviceVerified);
  uint8_t error = G2_RING_ERR_NONE;
  if (!successful) {
    if (!refresh.dailySuccessful) {
      error = refresh.dailyError != G2_RING_ERR_NONE
                  ? refresh.dailyError
                  : (uint8_t)G2_RING_ERR_TIMEOUT;
    } else {
      error = status.errorCode != G2_RING_ERR_NONE
                  ? status.errorCode
                  : (uint8_t)G2_RING_ERR_TIMEOUT;
    }
  }
  portENTER_CRITICAL(&sTransportMux);
  if (sHealthPageRefresh.id == refresh.id &&
      sHealthPageRefresh.generation == refresh.generation) {
    sHealthPageRefresh.deviceStatusCompleted = true;
    sHealthPageRefresh.deviceStatusVerified = deviceVerified;
    sHealthPageRefresh.deviceStatusError = deviceVerified
        ? (uint8_t)G2_RING_ERR_NONE
        : error;
  }
  portEXIT_CRITICAL(&sTransportMux);
  ringFinishHealthPageRefresh(refresh.id, refresh.generation, successful,
                              partial, error);
  DEBUG_RING_HEALTHF(
      "[RING] Health-page refresh id=%lu done ok=%d partial=%d error=%s",
      (unsigned long)refresh.id, (int)successful, (int)partial,
      g2RingTransactionErrorName(error));
}

static void ringOwnerTask(void* /*arg*/) {
  uint32_t ownedGeneration = 0;
  for (;;) {
    uint32_t currentGeneration = 0;
    const bool online = ringSnapshotLink(currentGeneration);
    if (ownedGeneration != 0 &&
        (!online || currentGeneration != ownedGeneration)) {
      ringDropOwnedGeneration(ownedGeneration);
      ownedGeneration = 0;
    }
    if (online && currentGeneration != 0 &&
        ownedGeneration != currentGeneration && sSetupRequested) {
      ownedGeneration = currentGeneration;
      ringBeginOwnedGeneration(ownedGeneration);
    }

    // Packet ACK is a protocol flow-control lane, not ordinary traffic. Retry
    // an already-pending ACK before admitting more RX. If the GATT writer is
    // busy, leave queued frames in their slabs and retry on the next lap;
    // otherwise a sustained notify burst could outrun the depth-eight lane.
    uint32_t serviceGeneration = 0;
    const bool serviceLinkOnline = ringSnapshotLink(serviceGeneration);
    if (ownedGeneration != 0 && serviceLinkOnline &&
        serviceGeneration == ownedGeneration &&
        (sActivePacketAck.valid || sPacketAckCount != 0)) {
      (void)ringServicePacketAck();
      if (sActivePacketAck.valid || sPacketAckCount != 0) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        continue;
      }
    }

    uint8_t rxSlot = 0;
    uint8_t rxBudget = RING_RX_QUEUE_DEPTH;
    while (rxBudget-- > 0 && sRxQueue &&
           xQueueReceive(sRxQueue, &rxSlot, 0) == pdTRUE) {
      if (!ringClaimQueuedRxSlab(rxSlot)) {
        sRxQueueDropped = sRxQueueDropped + 1;
        continue;
      }

      // The PROCESSING state retains exclusive ownership while the owner reads
      // directly from PSRAM. Release on every stale/offline/invalid path only
      // after parsing (when eligible) is complete.
      const RingRxFrame& rx = sRxSlabs[rxSlot];
      uint32_t rxGeneration = 0;
      if (rx.length <= R1_MAX_FRAME && rx.generation == ownedGeneration &&
          ringSnapshotLink(rxGeneration) && rxGeneration == ownedGeneration) {
        ringProcessRxFrame(rx.bytes, rx.length, rx.generation);
      }
      ringReleaseRxSlab(rxSlot);

      // ringProcessRxFrame may have admitted a daily-data packet ACK. Yield
      // immediately after releasing the PSRAM slab so the reserved lane is
      // serviced before another eligible notify can consume an entry.
      if (sActivePacketAck.valid || sPacketAckCount != 0) break;
    }

    uint32_t currentServiceGeneration = 0;
    if (ownedGeneration != 0 &&
        ringSnapshotLink(currentServiceGeneration) &&
        ownedGeneration == currentServiceGeneration) {
      if (ringServicePacketAck()) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        continue;
      }
      if (sSetupOwner.active) {
        ringServiceSetup();
      } else {
        G2RingSetupState setupState;
        portENTER_CRITICAL(&sTransportMux);
        setupState = sControlStatus.setupState;
        portEXIT_CRITICAL(&sTransportMux);
        if (setupState == G2_RING_SETUP_READY) {
          ringServiceControlReconciliation();
          ringServiceHistoryCoordinator();
          ringServiceHealthPageRefreshCoordinator();
          ringServiceNormalTransaction();
        }
      }
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
  }
}

// =============================================================================
// Client callbacks
// =============================================================================

class RingClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient* /*c*/) override {
    DEBUG_RING_LIFECYCLEF("[RING] BLE onConnect callback fired");
  }
  void onDisconnect(BLEClient* c) override {
    if (c != gRing.client) {
      DEBUG_RING_LIFECYCLEF("[RING] Ignoring stale-client disconnect callback");
      return;
    }
    const RingDownTransition down = ringBeginLogicalDown("disconnect");
    g2CancelActiveRingScan();
    DEBUG_RING_LIFECYCLEF("[RING] BLE onDisconnect — connected-was=%d",
              down.wasConnected ? 1 : 0);
    // Drop all GATT handles; client object may still exist until reconnect
    // replaces it, but must not be used for writes after this.
    gRing.writeChar   = nullptr;
    gRing.notifyChar  = nullptr;
    gRing.clientStale = true;
    // No corrective time push into a dead link, and no ring timestamp from
    // THIS link surviving into the next one (this is the common teardown —
    // link lost — so it must clear as much as ringClearGattPointers does).
    ringClockCustodyReset();
  }
};

// =============================================================================
// Ring-only scan
// =============================================================================
// The shared G2 scan callback (G2_Glasses.cpp) also stashes ring adverts as a
// side effect, but that scan early-terminates the moment both temples are
// found. The R1's slower advert cycle (battery-saving) often falls outside
// that ~3-5s window, so the ring goes undetected and `ringconnect` bails.
//
// This dedicated scan uses a separate callback that ONLY watches for
// "EVEN R1_XXXXXX" adverts. It runs for the full requested timeout (no
// early termination tied to glasses) so the ring has more chances to emit.
//
// Caveat: scanning while the glasses are connected splits BLE-controller
// time between scan and the active connections, lowering scan duty-cycle.
// If the ring stays elusive: physically tap it to wake, check battery, or
// run a longer scan (e.g. `ringscan 60`). If still nothing, the ring may
// already be paired with another central (phone) and refusing connectable
// advertising.

static portMUX_TYPE gRingScanCallbackMux = portMUX_INITIALIZER_UNLOCKED;
static bool gRingScanCallbackAdmission = false;
static uint16_t gRingScanCallbacksInFlight = 0;
static uint32_t gRingScanEpoch = 0;
static uint32_t gRingScanCancelGeneration = 0;
static bool gRingScanCallbackQuarantined = false;

class RingScanCallbackClaim {
 public:
  RingScanCallbackClaim() {
    portENTER_CRITICAL(&gRingScanCallbackMux);
    if (gRingScanCallbackAdmission && !gRingScanCallbackQuarantined &&
        gRingScanCallbacksInFlight < UINT16_MAX) {
      ++gRingScanCallbacksInFlight;
      held_ = true;
      epoch_ = gRingScanEpoch;
      cancelGeneration_ = gRingScanCancelGeneration;
    }
    portEXIT_CRITICAL(&gRingScanCallbackMux);
  }
  ~RingScanCallbackClaim() {
    if (!held_) return;
    portENTER_CRITICAL(&gRingScanCallbackMux);
    if (gRingScanCallbacksInFlight > 0) --gRingScanCallbacksInFlight;
    portEXIT_CRITICAL(&gRingScanCallbackMux);
  }
  explicit operator bool() const { return held_; }
  bool current() const {
    if (!held_) return false;
    portENTER_CRITICAL(&gRingScanCallbackMux);
    const bool value = gRingScanCallbackAdmission &&
        !gRingScanCallbackQuarantined && epoch_ == gRingScanEpoch;
    portEXIT_CRITICAL(&gRingScanCallbackMux);
    return value && ringConnectGenerationCurrent(cancelGeneration_);
  }
 private:
  bool held_ = false;
  uint32_t epoch_ = 0;
  uint32_t cancelGeneration_ = 0;
};

static uint32_t ringScanCallbackBegin(uint32_t cancelGeneration) {
  const bool lifecycleFaulted = bleStackLifecycleFaulted();
  if (lifecycleFaulted ||
      !ringConnectGenerationCurrent(cancelGeneration)) return 0;
  portENTER_CRITICAL(&gRingScanCallbackMux);
  if (gRingScanCallbackQuarantined &&
      !gRingScanCallbackAdmission && gRingScanCallbacksInFlight == 0) {
    ++gRingScanEpoch;
    if (gRingScanEpoch == 0) gRingScanEpoch = 1;
    gRingScanCallbackQuarantined = false;
  }
  if (gRingScanCallbackQuarantined || gRingScanCallbackAdmission ||
      gRingScanCallbacksInFlight != 0) {
    portEXIT_CRITICAL(&gRingScanCallbackMux);
    return 0;
  }
  ++gRingScanEpoch;
  if (gRingScanEpoch == 0) gRingScanEpoch = 1;
  gRingScanCancelGeneration = cancelGeneration;
  gRingScanCallbackAdmission = true;
  const uint32_t epoch = gRingScanEpoch;
  portEXIT_CRITICAL(&gRingScanCallbackMux);
  return epoch;
}

static bool ringScanCallbackEnd(uint32_t epoch, uint32_t timeoutMs = 500) {
  if (epoch == 0) return false;
  portENTER_CRITICAL(&gRingScanCallbackMux);
  if (gRingScanEpoch == epoch) gRingScanCallbackAdmission = false;
  portEXIT_CRITICAL(&gRingScanCallbackMux);
  const uint32_t deadline = millis() + timeoutMs;
  for (;;) {
    portENTER_CRITICAL(&gRingScanCallbackMux);
    const bool idle = gRingScanCallbacksInFlight == 0;
    portEXIT_CRITICAL(&gRingScanCallbackMux);
    if (idle) return true;
    if ((int32_t)(millis() - deadline) >= 0) {
      portENTER_CRITICAL(&gRingScanCallbackMux);
      gRingScanCallbackQuarantined = true;
      portEXIT_CRITICAL(&gRingScanCallbackMux);
      bleStackSetLifecycleFault(true);
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

class RingScanCallbacks : public BLEAdvertisedDeviceCallbacks {
public:
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    RingScanCallbackClaim callbackClaim;
    if (!callbackClaim || !callbackClaim.current()) return;
    if (!advertisedDevice.haveName()) return;
    String name = advertisedDevice.getName().c_str();
    // Inline mirror of classifyRingName() in G2_Glasses.cpp:
    //   /^EVEN\s+R1_[0-9A-F]{6}$/i
    if (name.length() < 10) return;
    const char* s = name.c_str();
    if (strncasecmp(s, "EVEN", 4) != 0) return;
    s += 4;
    if (*s != ' ' && *s != '\t') return;
    while (*s == ' ' || *s == '\t') s++;
    if (strncmp(s, "R1_", 3) != 0) return;
    s += 3;
    int hex = 0;
    while (*s && hex < 7) {
      if (!isxdigit((unsigned char)*s)) return;
      s++; hex++;
    }
    if (hex != 6 || *s != '\0') return;

    if (gRingAdvertisedDevice) return;  // already stashed (perhaps by G2 scan)
    BLEAdvertisedDevice* copy =
        new (std::nothrow) BLEAdvertisedDevice(advertisedDevice);
    if (!copy) return;
    if (!callbackClaim.current() || gRingAdvertisedDevice) {
      delete copy;
      return;
    }
    gRingAdvertisedDevice = copy;
    gRingDeviceName       = name;
    gRingDeviceAddress    = advertisedDevice.getAddress().toString().c_str();
    gRingScanFound        = true;
    DEBUG_RING_LIFECYCLEF("[RING] ringscan: Found %s @ %s (RSSI %d) — stashed",
              name.c_str(), gRingDeviceAddress.c_str(),
              advertisedDevice.getRSSI());
    BLEDevice::getScan()->stop();  // got it; bail out of remaining timeout
  }
};
static RingScanCallbacks gRingScanCallbacks;

bool g2RingScan(uint32_t timeoutSec, uint32_t cancelGeneration) {
  if (cancelGeneration == 0) {
    cancelGeneration = ringConnectCancelGenerationSnapshot();
  }
  if (!ringConnectGenerationCurrent(cancelGeneration)) return false;
  if (!g2RingInit()) return false;
  if (gRingAdvertisedDevice) {
    DEBUG_RING_LIFECYCLEF("[RING] ringscan: advert already stashed (%s @ %s); skipping",
              gRingDeviceName.c_str(), gRingDeviceAddress.c_str());
    return true;
  }
  if (timeoutSec == 0) timeoutSec = 1;
  if (timeoutSec > 300) timeoutSec = 300;

  DEBUG_RING_LIFECYCLEF("[RING] ringscan: scanning for EVEN R1_* (timeout=%us)",
            (unsigned)timeoutSec);

  BLEScan* scan = BLEDevice::getScan();
  if (!scan) {
    DEBUG_RING_LIFECYCLEF("[RING] ringscan: BLEDevice::getScan() returned null");
    return false;
  }

  scan->setAdvertisedDeviceCallbacks(&gRingScanCallbacks);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  // BLEScan::start(durationSec, continueScanning=false) blocks the calling
  // task until the scan completes — either by `timeoutSec` elapsing or by
  // a callback calling stop() (which we do on first ring match).
  const uint32_t scanEpoch = ringScanCallbackBegin(cancelGeneration);
  if (scanEpoch == 0) {
    DEBUG_RING_LIFECYCLEF("[RING] ringscan: callback generation unavailable");
    return false;
  }
  scan->start(timeoutSec, /*continueScanning*/ false);
  if (!ringScanCallbackEnd(scanEpoch)) {
    DEBUG_RING_LIFECYCLEF("[RING] ringscan: callback did not quiesce; host quarantined");
    return false;
  }
  scan->clearResults();

  if (!ringConnectGenerationCurrent(cancelGeneration)) {
    DEBUG_RING_LIFECYCLEF("[RING] ringscan: cancelled by disconnect/teardown");
    return false;
  }

  if (gRingAdvertisedDevice) {
    INFO_RINGF("ringscan: found %s @ %s",
               gRingDeviceName.c_str(), gRingDeviceAddress.c_str());
    return true;
  }

  DEBUG_RING_LIFECYCLEF("[RING] ringscan: timed out after %us — ring not advertising. "
            "Try: tap the ring to wake it, check battery, move it closer, "
            "or run 'ringscan 60' for a longer window.",
            (unsigned)timeoutSec);
  return false;
}

bool g2RingScanAsync(uint32_t timeoutSec) {
  if (!g2RingInit()) return false;
  uint32_t cancelGeneration = 0;
  if (!ringConnectGateOpen(&cancelGeneration)) return false;
  if (timeoutSec == 0) timeoutSec = 1;
  if (timeoutSec > 300) timeoutSec = 300;

  BleConnectJob job{};
  job.kind = BleConnectKind::RING_SCAN_ONLY;
  job.scanSeconds = (uint16_t)timeoutSec;
  job.cancelEpoch = cancelGeneration;
  if (!g2SubmitBleConnect(job)) {
    ERROR_RINGF("Scan submit FAILED — central worker unavailable");
    g2RingConnectMarkComplete();
    return false;
  }
  return true;
}

// =============================================================================
// Connect flow
// =============================================================================

// RAII guard: drop glasses connection priority to BALANCED for the
// duration of a ring connect attempt, then restore HIGH on exit. Both
// glasses links at HIGH priority (~12 ms intervals) saturate the single-
// radio BLE controller; trying to add a 3rd connection (the ring) under
// that load reliably times out (`Unknown ESP_ERR error` after 30s) and
// can starve an existing temple link to supervision-timeout (rsn=0x8).
// BALANCED (~50 ms intervals) gives the controller ~4× more idle time
// to scan and complete the new connection. Restored on every exit path
// — destructor fires regardless of how ringDoConnect returns.
// Refcounted: asks the arbiter in G2_Glasses.cpp for BALANCED and releases on
// every exit path. Nesting-safe — if something else is also holding BALANCED,
// whichever guard leaves first no longer drags the links back to HIGH under the
// other one. (That was a real defect while both guards set absolute values.)
struct GlassesPriorityGuard {
  GlassesPriorityGuard()  { g2ConnPriRequestBalanced("ring-connect"); }
  ~GlassesPriorityGuard() { g2ConnPriReleaseBalanced("ring-connect"); }
  GlassesPriorityGuard(const GlassesPriorityGuard&) = delete;
  GlassesPriorityGuard& operator=(const GlassesPriorityGuard&) = delete;
};

// `savedMac`: when non-empty, do a directed connect to that MAC without
// requiring a prior scan-cached gRingAdvertisedDevice. Used by the boot
// auto-reconnect path. When empty, behaves as before (uses the cached
// advertisement from the most recent G2 scan).
// Promoted from `static` to a public helper as part of Group B so the
// unified BLE-connect worker (in G2_Glasses.cpp) can dispatch RING_*
// jobs to it. Internal callers (the now-deleted ring*TaskBody functions)
// are gone; only the worker calls this.
static bool ringRequestCurrent(const BlePeerConnectRequest* request,
                               uint32_t cancelGeneration) {
  if (!ringConnectGenerationCurrent(cancelGeneration)) return false;
  if (!request) return true;
  if (!request->autoReconnect && !request->explicitReseek) {
    return blePeerIntentIsCurrent(BLE_PEER_R1_RING,
                                  request->intentGeneration,
                                  request->identityGeneration);
  }
  return blePeerConnectRequestIsCurrent(BLE_PEER_R1_RING, *request);
}

static bool ringAbortSupersededRequest(
    const BlePeerConnectRequest* request, uint32_t cancelGeneration,
    const char* phase) {
  if (ringRequestCurrent(request, cancelGeneration)) return false;
  DEBUG_RING_LIFECYCLEF("[RING] Connect superseded/cancelled during %s — closing new link",
            phase ? phase : "connect");
  ringTransportDisconnected();
  if (gRing.client && gRing.client->isConnected()) gRing.client->disconnect();
  ringClearGattPointers(/*dropClientPtr=*/false);
  return true;
}

bool ringPerformConnect(const String& savedMac /* = String() */,
                        const BlePeerConnectRequest* expectedRequest,
                        uint32_t cancelGeneration) {
  if (cancelGeneration == 0) {
    cancelGeneration = ringConnectCancelGenerationSnapshot();
  }
  if (!ringRequestCurrent(expectedRequest, cancelGeneration)) {
    DEBUG_RING_LIFECYCLEF("[RING] Connect request stale/cancelled before dispatch");
    return false;
  }
#if ENABLE_MICROPHONE
  // BALANCED ring-connect intervals deliver only ~15 G2 mic notifications/s
  // versus the 20/s required by a live recorder, so an actual G2 recording
  // gets priority. `audioCaptureActive()` is deliberately NOT the authority:
  // mic autostart/openmic holds that HAL lease indefinitely even while the
  // recorder is IDLE. ESP-SR is a separate continuous consumer and remains a
  // throughput-critical reason to wait.
  auto g2AudioThroughputCritical = []() {
    return audioGetSource() == AUDIO_SRC_G2_LEFT &&
           (micRecordingBusy() || audioCaptureOwnedBy("sr"));
  };

  if (g2AudioThroughputCritical()) {
    const uint32_t waitStartedMs = millis();
    INFO_RINGF("connect queued: active G2 audio session — "
               "waiting to start safely");
    while (g2AudioThroughputCritical()) {
      if (!ringRequestCurrent(expectedRequest, cancelGeneration)) return false;
      const uint32_t waitedMs = (uint32_t)(millis() - waitStartedMs);
      if (waitedMs >= RING_AUDIO_DEFER_TIMEOUT_MS) {
        WARN_RINGF("queued connect expired after %lus waiting "
                   "for G2 audio to become idle",
                   (unsigned long)(waitedMs / 1000u));
        ringNoteConnectFailure("audio-wait", waitedMs);
        return false;
      }
      vTaskDelay(pdMS_TO_TICKS(RING_AUDIO_DEFER_POLL_MS));
    }
    const uint32_t waitedMs = (uint32_t)(millis() - waitStartedMs);
    INFO_RINGF("G2 audio idle — starting queued connect after "
               "%lu.%03lus",
               (unsigned long)(waitedMs / 1000u),
               (unsigned long)(waitedMs % 1000u));
  }
  // A recorder can still start immediately after this check. Preserving an
  // already-started voice request would require a cross-subsystem admission
  // fence; the existing delivered-rate watchdog surfaces that narrow race.
#endif
  // Drop glasses to BALANCED for the entire connect attempt. Auto-restored
  // on any return path. Cheap no-op if no glasses are connected.
  GlassesPriorityGuard prio_guard;

  if (!ringRequestCurrent(expectedRequest, cancelGeneration)) return false;

  const bool useSavedMac = (savedMac.length() > 0);
  if (!useSavedMac && !gRingAdvertisedDevice) {
    // No prior scan stashed an advert. Self-scan rather than bailing —
    // matches the doc comment in G2_Ring.h:75 ("Scan for the ring advert
    // or use one already discovered"). The shared G2 scan early-terminates
    // on glasses-found which often misses the ring's slower advert cycle.
    DEBUG_RING_LIFECYCLEF("[RING] connect: no advertisedDevice stashed; running "
              "dedicated ring scan (15s)...");
    if (!g2RingScan(15, cancelGeneration) || !gRingAdvertisedDevice) {
      DEBUG_RING_LIFECYCLEF("[RING] connect: ring not visible — try 'ringscan 60' with "
                "the ring close + woken (tap it) first, then retry "
                "'ringconnect'");
      return false;
    }
    if (!ringRequestCurrent(expectedRequest, cancelGeneration)) return false;
  }
  if (gRing.connected) {
    DEBUG_RING_LIFECYCLEF("[RING] connect: already connected");
    return true;
  }

  if (useSavedMac) {
    DEBUG_RING_LIFECYCLEF("[RING] Connecting by saved MAC %s (heap=%u)",
              savedMac.c_str(), (unsigned)ESP.getFreeHeap());
  } else {
    DEBUG_RING_LIFECYCLEF("[RING] Connecting to %s @ %s (heap=%u)",
              gRingDeviceName.c_str(), gRingDeviceAddress.c_str(),
              (unsigned)ESP.getFreeHeap());
  }

  // Replace stale client from a previous unexpected drop. Same pattern
  // as the glasses path — Arduino BLE's BLEClient doesn't reliably survive
  // peer-initiated disconnects.
  if (gRing.client && gRing.clientStale) {
    DEBUG_RING_LIFECYCLEF("[RING] Replacing stale BLEClient from prior drop");
    // Nulling without delete orphans the client and its cached GATT tree
    // (measured ~10-14 KB per drop on the glasses path) — but freeing it is
    // only legal once nothing can still reach it:
    //
    // Gate 1 — the library. After a MANUAL ringdisconnect, clientStale was
    // set by us, not by onDisconnect, so the async DISCONNECT_EVT (which
    // unregisters the gattc app and pulls the client out of BLEDevice's
    // routing map) may still be in flight — the R1's power-save conn
    // interval makes 50-500 ms normal. That same handler drops getGattcIf()
    // to ESP_GATT_IF_NONE; poll for it, bounded.
    //
    // Gate 2 — app tasks. The serialized owner holds writeMutex across its
    // writeChar dereference (the char lives inside this client's GATT
    // cache); 2 s dwarfs a healthy sender's hold (one writeValue).
    //
    // If either gate fails, leak the object rather than free memory the BLE
    // dispatcher or a wedged writer may still touch.
    const bool locked = gRing.writeMutex &&
        xSemaphoreTake(gRing.writeMutex, pdMS_TO_TICKS(2000)) == pdTRUE;
    if (!locked) {
      DEBUG_RING_LIFECYCLEF("[RING] Stale client retirement deferred: owner write active");
      return false;
    }
    const bool retired = bleRetireClientForReplacement(
        gRing.client, BLE_CLIENT_RETIRE_R1,
        (uint8_t)BLE_PEER_R1_RING, "RING");
    xSemaphoreGive(gRing.writeMutex);
    if (!retired) return false;
    gRing.clientStale = false;
  }
  if (!gRing.client) {
    gRing.clientStale = false;
    gRing.client = BLEDevice::createClient();
    if (!gRing.client) {
      DEBUG_RING_LIFECYCLEF("[RING] BLEDevice::createClient() returned null");
      return false;
    }
    // Static: the callbacks keep no per-connection state (they write gRing
    // globals), ~BLEClient never frees this pointer, and a heap `new` per
    // replacement cycle just leaked alongside the client.
    static RingClientCallbacks sRingClientCallbacks;
    gRing.client->setClientCallbacks(&sRingClientCallbacks);
  }

  const uint32_t t0 = millis();
  bool connOk;
  if (useSavedMac) {
    // Direct address connect — Arduino BLE supports this without a prior
    // advertisement scan. The peer must be advertising and in range.
    //
    // Address type matters: BLEClient::connect defaults to PUBLIC, but the
    // R1 ring uses a Random Static address (BT spec: top two bits of the
    // MSB are 0b11, i.e. first byte ≥ 0xC0). Asking the controller to open
    // a Public link to a Random address always times out at 30s. The
    // cached-advert path doesn't hit this because BLEAdvertisedDevice
    // carries the discovered address type.
    BLEAddress addr(savedMac.c_str());
    uint8_t addrType = expectedRequest &&
            expectedRequest->savedTarget.addressType1Known
        ? expectedRequest->savedTarget.addressType1
        : static_cast<uint8_t>(BLE_ADDR_TYPE_PUBLIC);
    if (!expectedRequest ||
        !expectedRequest->savedTarget.addressType1Known) {
      unsigned msb = 0;
      if (sscanf(savedMac.c_str(), "%2x", &msb) == 1 && (msb & 0xC0) == 0xC0) {
        addrType = BLE_ADDR_TYPE_RANDOM;
      }
    }
    // Explicit timeout: the default is portMAX_DELAY, which turned a lost
    // OPEN event into a forever-wedged connect worker (root-cause candidate
    // for the 2026-07-28 2h no-reconnect window). 35s clears the ring's
    // normal ~30s direct-connect timeout with margin.
    connOk = gRing.client->connect(addr, addrType, 35000);
    if (connOk) {
      // Populate the cached descriptors so subsequent disconnect / status
      // paths print sensible names.
      gRingDeviceAddress = savedMac;
      if (gRingDeviceName.length() == 0) gRingDeviceName = "saved-ring";
    }
  } else {
    // Same 35s bound as the saved-MAC path (the plain connect(device)
    // overload hardwires the portMAX_DELAY default).
    connOk = gRing.client->connectTimeout(gRingAdvertisedDevice, 35000);
  }
  // Classify BOTH overloads above (the saved-MAC path takes a BLEAddress, so
  // bleConnectWatched's advertised-device wrapper can't cover it — and saved
  // MAC is exactly the unattended auto-reconnect path where a wedge builds).
  bleNoteClientConnectOutcome(gRing.client, connOk, millis() - t0, "RING");
  if (!connOk) {
    const uint32_t dt = millis() - t0;
    ERROR_RINGF("BLE connect FAILED after %u ms (%s)",
                (unsigned)dt,
                useSavedMac ? savedMac.c_str() : "scan target");
    ringNoteConnectFailure("link", dt);
    (void)bleQuarantineClientAfterFailedConnect(
        gRing.client, BLE_CLIENT_RETIRE_R1,
        (uint8_t)BLE_PEER_R1_RING, "RING");
    return false;
  }
  if (ringAbortSupersededRequest(expectedRequest, cancelGeneration, "OPEN")) {
    return false;
  }
  DEBUG_RING_LIFECYCLEF("[RING] BLE connect OK in %u ms", (unsigned)(millis() - t0));

  // Prefer the same high local ATT MTU as glasses (G2_BLE_LOCAL_MTU_PREF).
  // Never setMTU(64) — that lowered the process-global local MTU and broke
  // subsequent glasses discovery (PDU size: 64). Peer may still negotiate
  // this link down to ~64; that value lands in gRing.mtu.
  gRing.mtu = bleNegotiateConnMtu(gRing.client, G2_BLE_LOCAL_MTU_PREF, 1000, "RING");

  DEBUG_RING_LIFECYCLEF("[RING] Looking up service %s", G2RING_SERVICE_UUID);
  BLERemoteService* svc = gRing.client->getService(BLEUUID(G2RING_SERVICE_UUID));
  if (!svc) {
    ERROR_RINGF("Connect FAILED — ring service not found");
    ringNoteConnectFailure("service", millis() - t0);
    DEBUG_RING_LIFECYCLEF("[RING] Service %s NOT FOUND (listing all services below)",
              G2RING_SERVICE_UUID);
    auto* services = gRing.client->getServices();
    if (services) {
      for (const auto& entry : *services) {
        DEBUG_RING_LIFECYCLEF("[RING]   svc: %s", entry.first.c_str());
      }
    }
    gRing.client->disconnect();
    return false;
  }
  DEBUG_RING_LIFECYCLEF("[RING] Service found, getting characteristics");

  gRing.writeChar  = svc->getCharacteristic(BLEUUID(G2RING_CHAR_WRITE_UUID));
  gRing.notifyChar = svc->getCharacteristic(BLEUUID(G2RING_CHAR_NOTIFY_UUID));
  DEBUG_RING_LIFECYCLEF("[RING] writeChar=%p notifyChar=%p", gRing.writeChar, gRing.notifyChar);

  if (!gRing.notifyChar) {
    ERROR_RINGF("Connect FAILED — notify characteristic not found");
    ringNoteConnectFailure("notify-char", millis() - t0);
    DEBUG_RING_LIFECYCLEF("[RING] Notify char %s NOT FOUND (listing all chars):",
              G2RING_CHAR_NOTIFY_UUID);
    // Listing exists only to feed DEBUG_RING_LIFECYCLEF — same gate.
    if (getLogLevel() >= LOG_LEVEL_DEBUG && isDebugFlagSet(DEBUG_RING | DEBUG_RING_LIFECYCLE)) {
      auto* chars = svc->getCharacteristics();
      if (chars) {
        for (const auto& entry : *chars) {
          DEBUG_RING_LIFECYCLEF("[RING]   char: %s", entry.first.c_str());
        }
      }
    }
    gRing.client->disconnect();
    return false;
  }
  DEBUG_RING_LIFECYCLEF("[RING] Chars: write.canWriteNR=%d notify.canNotify=%d canIndicate=%d",
            gRing.writeChar  ? gRing.writeChar->canWriteNoResponse() : 0,
            gRing.notifyChar ? gRing.notifyChar->canNotify()         : 0,
            gRing.notifyChar ? gRing.notifyChar->canIndicate()       : 0);

  if (!gRing.writeChar || !gRing.notifyChar->canNotify()) {
    ERROR_RINGF("Connect FAILED — mandatory write/notify capability missing");
    ringNoteConnectFailure("notify-capability", millis() - t0);
    if (gRing.client) gRing.client->disconnect();
    ringClearGattPointers(/*dropClientPtr=*/false);
    return false;
  }
  DEBUG_RING_LIFECYCLEF("[RING] Subscribing to notifications on %s",
            G2RING_CHAR_NOTIFY_UUID);
  const BLERemoteNotifyResult ringNotify =
      gRing.notifyChar->registerForNotify(ringNotifyThunk);
  if (!ringNotify.success) {
    ERROR_RINGF("Connect FAILED — notify registration api=%ld evt=%ld "
                "descApi=%ld descEvt=%ld timeout=%d",
                (long)ringNotify.apiStatus,
                (long)ringNotify.eventStatus,
                (long)ringNotify.descriptorApiStatus,
                (long)ringNotify.descriptorEventStatus,
                (int)ringNotify.timedOut);
    ringNoteConnectFailure("notify-register", millis() - t0);
    if (gRing.client) gRing.client->disconnect();
    ringClearGattPointers(/*dropClientPtr=*/false);
    return false;
  }

  if (ringAbortSupersededRequest(expectedRequest, cancelGeneration,
                                 "discovery")) return false;

  if (!ringResetTelemetryForNewLink()) {
    ERROR_RINGF("Connect FAILED — telemetry reset fence unavailable");
    ringNoteConnectFailure("telemetry-reset", millis() - t0);
    ringTransportDisconnected();
    if (gRing.client) gRing.client->disconnect();
    ringClearGattPointers(/*dropClientPtr=*/false);
    return false;
  }
  gRing.connected      = true;
  // Belt for the invalidate path: g2RingInvalidateLink nulls the client and
  // leaves clientStale=true with nothing left to reap, so the fresh-client
  // branch above never runs the reset — without this line the next connect
  // would be mute (all TX gates check clientStale).
  gRing.clientStale    = false;
  gRing.connectedSince = millis();
  sRingConnFailStreak  = 0;

  // pairAuth → systemTime → advStart unlocks the notify/telemetry stream
  // (a bare subscribe without pairAuth stays muted).
  if (!ringRunStandardSetup()) {
    G2RingControlStatus setup{};
    g2RingGetControlStatus(setup);
    ERROR_RINGF("Connect FAILED — setup %s (%s)",
                g2RingSetupStateName(setup.setupState),
                g2RingTransactionErrorName(setup.setupLastError));
    ringNoteConnectFailure("setup", millis() - t0);
    ringTransportDisconnected();
    if (gRing.client) gRing.client->disconnect();
    ringClearGattPointers(/*dropClientPtr=*/false);
    return false;
  }

  if (ringAbortSupersededRequest(expectedRequest, cancelGeneration,
                                 "setup")) return false;

  bool completionCurrent = false;
  bool persistNeeded = false;
  {
    RingCompletionGuard completion;
    if (completion && ringConnectGenerationCurrent(cancelGeneration)) {
      const bool learnsTarget = expectedRequest &&
          !expectedRequest->autoReconnect &&
          !expectedRequest->explicitReseek;
      completionCurrent = learnsTarget
          ? blePeerCommitLearnedTargetIfCurrent(
                BLE_PEER_R1_RING,
                expectedRequest->intentGeneration,
                expectedRequest->identityGeneration,
                gRingDeviceAddress, String(),
                /*replaceMask=*/0x01,
                /*completeTopology=*/true, &persistNeeded)
          : (!expectedRequest ||
             blePeerNoteLinkUpIfCurrent(BLE_PEER_R1_RING,
                                        *expectedRequest));
      if (completionCurrent) {
        if (!expectedRequest) blePeerNoteLinkUp(BLE_PEER_R1_RING);
        gRingUpEventPublished = true;
        INFO_RINGF("Connected to %s (setup verified, profile %s)",
                   gRingDeviceName.c_str(),
                   r1ProtocolProfileName(sR1Profile));
        ringPushStatusEvent("connect-ok");
        // Mirror G2_Glasses: SSE status push + typed bus event for
        // automations / `events` / system_events consumers.
        systemEventPost(
            SYSEVT_RING_CONNECTED,
            gRingDeviceName.length() > 0 ? gRingDeviceName.c_str() : "R1",
            gRingDeviceAddress.length() > 0
                ? gRingDeviceAddress.c_str() : nullptr);
      }
    }
  }
  if (!completionCurrent) {
    (void)ringAbortSupersededRequest(expectedRequest, cancelGeneration,
                                     "completion");
    return false;
  }
  if (persistNeeded) (void)writeSettingsJson();
  return true;
}

// Ring connect dispatch:
// All three connect entry points (scan, saved, MAC) flow through the
// unified BLE-connect worker (see `g2SubmitBleConnect` in G2_Glasses.h).
// The worker dispatches the BleConnectJob `kind` to ringPerformConnect()
// above:
//   - BleConnectKind::RING_SCAN   — discover by name, connect first match
//   - BleConnectKind::RING_SAVED  — read saved MAC + wait for glasses,
//                                   then 3 s settle before connect
//   - BleConnectKind::RING_MAC    — connect to MAC supplied in the job's
//                                   payload (no static handoff)

// =============================================================================
// Public API
// =============================================================================

bool g2RingInit() {
  if (gRing.initialized) return true;
  bleCentralTxInit();
  if (!gRing.writeMutex) {
    gRing.writeMutex = xSemaphoreCreateMutex();
  }
  if (!gRing.writeMutex) return false;
  if (!sRxQueue) {
    sRxQueue = xQueueCreateStatic(RING_RX_QUEUE_DEPTH, sizeof(uint8_t),
                                  sRxQueueBytes, &sRxQueueStorage);
  }
  if (!sSetupDone) {
    sSetupDone = xSemaphoreCreateBinaryStatic(&sSetupDoneStorage);
  }
  if (!sRxQueue || !sSetupDone || !ringTelemetryMutex()) return false;
  ringResetRxIngressForInit();
  if (!ringStorageSelfTest()) {
    DEBUG_RING_LIFECYCLEF("[RING] Module disabled: storage self-test failed");
    return false;
  }
  if (!ringTransactionOrderingSelfTest()) {
    DEBUG_RING_LIFECYCLEF(
        "[RING] Module disabled: transaction ordering self-test failed");
    return false;
  }

  // Clamp erased/corrupt persisted values to the fail-safe Preserve policy.
  if (gSettings.ringHealthCollectionDesired < (int)G2_RING_PRESERVE ||
      gSettings.ringHealthCollectionDesired > (int)G2_RING_ON) {
    setSetting(gSettings.ringHealthCollectionDesired,
               (int)G2_RING_PRESERVE);
  }
  if (gSettings.ringLowPowerDesired < (int)G2_RING_PRESERVE ||
      gSettings.ringLowPowerDesired > (int)G2_RING_ON) {
    setSetting(gSettings.ringLowPowerDesired, (int)G2_RING_PRESERVE);
  }
  sControlStatus.healthDesired =
      (G2RingDesiredState)gSettings.ringHealthCollectionDesired;
  sControlStatus.lowPowerDesired =
      (G2RingDesiredState)gSettings.ringLowPowerDesired;

  // Register with the peer registry. ringPeerSpec is a file-static (see
  // top of this file); registration just publishes a stable pointer.
  bleRegisterPeer(ringPeerSpec);

  // Cheap one-time validation that our CRC ports match the FlutterApp's
  // captured wire bytes. Logs PASS/FAIL inline; on FAIL the encoder is
  // unsafe to use against real hardware (we'd send malformed frames).
  if (!r1ProtocolSelfTest()) {
    DEBUG_RING_LIFECYCLEF("[RING] Module disabled: protocol self-test failed");
    return false;
  }
  if (!sRingOwnerTask) {
    // sActiveTransaction sits in NOLOAD PSRAM and boots zeroed — its NSDMI
    // defaults were discarded. Re-establish them strictly before the owner
    // task can exist. Guarded so a re-entrant init after a mid-way failure
    // never writes the struct while a live owner task uses it.
    sActiveTransaction = RingActiveTransaction{};
  }
  if (!sRingOwnerTask &&
      xTaskCreateLogged(ringOwnerTask, "r1_owner", RING_OWNER_STACK_BYTES,
                        nullptr, 4,
                        &sRingOwnerTask, "r1.owner", PRO_CORE) != pdPASS) {
    DEBUG_RING_LIFECYCLEF("[RING] Module disabled: owner task creation failed");
    return false;
  }
  gRing.initialized = true;

  DEBUG_RING_LIFECYCLEF("[RING] Module initialised (full R1 protocol — auth + telemetry)");
  return true;
}

bool g2RingConnect() {
  if (!g2RingInit()) return false;
  uint32_t cancelGeneration = 0;
  // Group B: submit to the unified worker. Active flag flips true here so
  // duplicate g2RingConnect* calls reject before we ever hit the queue;
  // the worker's dispatch clears it after ringPerformConnect returns.
  BleConnectJob job{};
  job.kind = BleConnectKind::RING_SCAN;
  {
    RingCompletionGuard completion;
    if (!completion || !ringConnectGateOpen(&cancelGeneration)) return false;
    job.cancelEpoch = cancelGeneration;
    if (!blePeerBeginManualLearn(BLE_PEER_R1_RING,
                                 job.peerIntentGeneration,
                                 job.peerIdentityGeneration)) {
      DEBUG_RING_LIFECYCLEF("[RING] Connect rejected — no current peer owner authority");
      g2RingConnectMarkComplete();
      return false;
    }
  }
  if (!g2SubmitBleConnect(job)) {
    ERROR_RINGF("Connect submit FAILED (scan) — worker queue full or BLE not ready");
    ringNoteConnectFailure("submit", 0);
    g2RingConnectMarkComplete();
    return false;
  }
  return true;
}

bool g2RingConnectSaved() {
  if (!g2RingInit()) return false;
  uint32_t cancelGeneration = 0;
  BlePeerConnectRequest request;
  BleConnectJob job{};
  job.kind = BleConnectKind::RING_SAVED;
  {
    RingCompletionGuard completion;
    if (!completion || !ringConnectGateOpen(&cancelGeneration)) return false;
    job.cancelEpoch = cancelGeneration;
    if (!blePeerBeginManualConnectRequest(BLE_PEER_R1_RING, request)) {
      DEBUG_RING_LIFECYCLEF("[RING] g2RingConnectSaved: no owned saved MAC, skipping");
      g2RingConnectMarkComplete();
      return false;
    }
    ringRequestToJob(request, job);
  }
  if (!g2SubmitBleConnect(job)) {
    ERROR_RINGF("Connect submit FAILED (saved) — worker queue full or BLE not ready");
    ringNoteConnectFailure("submit", 0);
    g2RingConnectMarkComplete();
    return false;
  }
  return true;
}

static BlePeerConnectAdmission ringPeerConnectSavedAdmissionThunk(
    const BlePeerConnectRequest& request) {
  if (!isG2ClientInitialized() || isBleServerInitialized() ||
      bleRoleTransitionState() != BleRoleTransition::IDLE) {
    return BlePeerConnectAdmission::ROLE_BLOCKED;
  }
  if (ringPublishedConnected()) {
    return BlePeerConnectAdmission::ALREADY_UP;
  }
  if (!request.savedTarget.mac1[0]) {
    return BlePeerConnectAdmission::NO_TARGET;
  }
  if (!blePeerConnectRequestIsCurrent(BLE_PEER_R1_RING, request)) {
    return BlePeerConnectAdmission::ROLE_BLOCKED;
  }
  uint32_t cancelGeneration = 0;
  if (!ringConnectGateOpen(&cancelGeneration)) {
    return BlePeerConnectAdmission::COALESCED;
  }

  BleConnectJob job{};
  job.kind = BleConnectKind::RING_SAVED;
  job.cancelEpoch = cancelGeneration;
  ringRequestToJob(request, job);
  if (!g2SubmitBleConnect(job)) {
    g2RingConnectMarkComplete();
    return BlePeerConnectAdmission::BUSY;
  }
  return BlePeerConnectAdmission::STARTED;
}

bool g2RingConnectMac(const String& mac) {
  String m = mac;
  m.trim();
  // Loose validation — full BLEAddress parsing happens inside ringPerformConnect.
  // Just reject obviously-wrong inputs here (need at least "aa:bb:cc:dd:ee:ff"
  // = 17 chars; BleConnectJob.mac is a 18-byte buffer including NUL).
  if (m.length() < 17 || m.length() > 17) {
    DEBUG_RING_LIFECYCLEF("[RING] connect-mac: invalid MAC '%s' (need aa:bb:cc:dd:ee:ff)",
              m.c_str());
    return false;
  }
  if (!g2RingInit()) return false;
  uint32_t cancelGeneration = 0;
  BleConnectJob job{};
  job.kind = BleConnectKind::RING_MAC;
  {
    RingCompletionGuard completion;
    if (!completion || !ringConnectGateOpen(&cancelGeneration)) return false;
    job.cancelEpoch = cancelGeneration;
    if (!blePeerBeginManualLearn(BLE_PEER_R1_RING,
                                 job.peerIntentGeneration,
                                 job.peerIdentityGeneration)) {
      DEBUG_RING_LIFECYCLEF("[RING] Direct connect rejected — no current peer owner authority");
      g2RingConnectMarkComplete();
      return false;
    }
  }
  // 17 chars + NUL fits exactly in the 18-byte mac buffer.
  memcpy(job.mac, m.c_str(), 17);
  job.mac[17] = '\0';
  if (!g2SubmitBleConnect(job)) {
    ERROR_RINGF("Connect submit FAILED (mac) — worker queue full or BLE not ready");
    ringNoteConnectFailure("submit", 0);
    g2RingConnectMarkComplete();
    return false;
  }
  return true;
}

bool g2RingPrepareForStackTeardown(uint32_t timeoutMs) {
  (void)ringBeginLogicalDown("disconnect");
  // A manual scan can occupy the sole central worker for up to 300 seconds.
  // Stop it only after releasing the completion mutex.
  g2CancelActiveRingScan();
  portENTER_CRITICAL(&gRingScanCallbackMux);
  const bool scanCallbacksIdle = !gRingScanCallbackAdmission &&
      gRingScanCallbacksInFlight == 0;
  portEXIT_CRITICAL(&gRingScanCallbackMux);
  if (!scanCallbacksIdle) {
    DEBUG_RING_LIFECYCLEF("[RING] stack teardown deferred: scan callback active");
    return false;
  }
  // Logical down was published before the callback/owner barriers. Any
  // producer which has not entered a write now fails its cheap online check.
  ringWakeOwner();

  const uint32_t started = millis();
  if (!bleCentralTxTake(timeoutMs)) {
    DEBUG_RING_LIFECYCLEF("[RING] stack teardown deferred: central TX gate busy");
    return false;
  }

  bool writeLocked = false;
  if (gRing.writeMutex) {
    const uint32_t elapsed = millis() - started;
    const uint32_t left = elapsed < timeoutMs ? timeoutMs - elapsed : 0;
    writeLocked = xSemaphoreTake(gRing.writeMutex, pdMS_TO_TICKS(left)) == pdTRUE;
    if (!writeLocked) {
      DEBUG_RING_LIFECYCLEF("[RING] stack teardown deferred: owner write still active");
      bleCentralTxGive();
      return false;
    }
  }

  // A notify callback may have published a slab immediately before the
  // offline transition. It never dereferences GATT pointers, but waiting for
  // the owner to release all published/processing slabs gives teardown an
  // explicit callback/owner hand-off barrier instead of relying on a delay.
  while (!ringRxSlabAllFree() && (millis() - started) < timeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  const bool rxDrained = ringRxSlabAllFree();
  if (rxDrained) {
    ringClearGattPointers(/*dropClientPtr=*/true);
  }

  if (writeLocked) xSemaphoreGive(gRing.writeMutex);
  bleCentralTxGive();
  if (!rxDrained) {
    DEBUG_RING_LIFECYCLEF("[RING] stack teardown deferred: RX owner did not quiesce");
  }
  return rxDrained;
}

void g2RingInvalidateLink() {
  // No GATT calls — stack may already be dead.
  const bool wasConnected = gRing.connected;
  // Drop the pointer only after the boot-lifetime owner has crossed the same
  // TX/write exclusion used by normal traffic. A raw pointer clear here used
  // to race ringOwnerWrite() during BLEDevice::deinit().
  if (!g2RingPrepareForStackTeardown(2000)) {
    WARN_RINGF("Link invalidation deferred — owner still active; "
               "full BLE teardown is unsafe");
    return;
  }
  if (wasConnected) {
    DEBUG_RING_LIFECYCLEF("[RING] Link invalidated (BLE stack teardown)");
  }
}

void g2RingDisconnect(bool userInitiated) {
  // Linearize explicit user-down before any pending scan/OPEN can publish a
  // new link. The worker checks this generation at every blocking boundary.
  {
    RingCompletionGuard completion;
    if (!completion) return;
    portENTER_CRITICAL(&gRingConnectMux);
    gRingDisconnecting = true;
    portEXIT_CRITICAL(&gRingConnectMux);
    (void)ringBeginLogicalDown("disconnect", userInitiated);
  }
  g2CancelActiveRingScan();
  // Producer admission is now closed. The owner is a boot-lifetime task, so a
  // connected boolean alone is not a teardown barrier.
  if (!bleCentralTxTake(2000)) {
    DEBUG_RING_LIFECYCLEF("[RING] Disconnect deferred: central TX gate busy");
    portENTER_CRITICAL(&gRingConnectMux);
    gRingDisconnecting = false;
    portEXIT_CRITICAL(&gRingConnectMux);
    return;
  }
  const bool writeLocked = !gRing.writeMutex ||
      xSemaphoreTake(gRing.writeMutex, pdMS_TO_TICKS(2000)) == pdTRUE;
  if (!writeLocked) {
    DEBUG_RING_LIFECYCLEF("[RING] Disconnect deferred: owner write still active");
    bleCentralTxGive();
    portENTER_CRITICAL(&gRingConnectMux);
    gRingDisconnecting = false;
    portEXIT_CRITICAL(&gRingConnectMux);
    return;
  }
  // After BLEDevice::deinit the client pointer is dangling — callers must use
  // g2RingPrepareForStackTeardown before host teardown so it is nulled first.
  if (gRing.client && !gRing.clientStale && gRing.client->isConnected()) {
    DEBUG_RING_LIFECYCLEF("[RING] Disconnecting");
    gRing.client->disconnect();  // onDisconnect clears chars when stack is live
  }
  // Keep the client object for the next connect's stale-replacement reap:
  // deleting it here would race the async DISCONNECT_EVT still headed for it,
  // and the old null-without-delete leaked it (~10-14 KB) AND stranded
  // clientStale=true forever — the delete branch was the only reset, so a
  // manual ringdisconnect → ringconnect came up connected-but-mute (every TX
  // gate checks clientStale).
  ringClearGattPointers(/*dropClientPtr=*/false);
  if (gRing.writeMutex) xSemaphoreGive(gRing.writeMutex);
  bleCentralTxGive();
  portENTER_CRITICAL(&gRingConnectMux);
  gRingDisconnecting = false;
  portEXIT_CRITICAL(&gRingConnectMux);
}

bool g2RingIsConnected() {
  return gRing.connected;
}

bool g2RingPointPollingSupported(void) {
  const R1ProtocolProfile profile = ringSnapshotProtocolProfile();
  return r1ProfileSupportsPointMeasureQuery(profile) &&
         r1ProfileSupportsPointIngestion(profile);
}

bool g2RingPollVital(uint8_t which) {
  if (!gRing.connected || !gRing.writeChar) return false;
  const char* tag = "?";
  G2RingTransactionHandle handle{};
  bool queued = false;
  switch (which) {
    case 0:
      tag = "hrPoint";
      queued = ringEnqueueHealthDataQuery(R1_CMD_HEARTRATE, R1_SUB_POINT,
                                          1, &handle);
      break;
    case 1:
      tag = "hrvPoint";
      queued = ringEnqueueHealthDataQuery(R1_CMD_HRV, R1_SUB_POINT,
                                          2, &handle);
      break;
    case 2:
      tag = "spo2Point";
      queued = ringEnqueueHealthDataQuery(R1_CMD_SPO2, R1_SUB_POINT,
                                          3, &handle);
      break;
    case 3:
      tag = "tempPoint";
      queued = ringEnqueueHealthDataQuery(R1_CMD_TEMPERATURE, R1_SUB_POINT,
                                          4, &handle);
      break;
    case 4:
      tag = "devStatus";
      queued = ringEnqueueSystemQuery(R1_SUB_DEVICE_STATUS, 5, &handle);
      break;
    default: return false;
  }
  if (queued) {
    DEBUG_RING_TXNF("[RING] page poll queued %s tx=%lu gen=%lu", tag,
              (unsigned long)handle.id, (unsigned long)handle.generation);
  }
  return queued;
}

bool g2RingQueryDaily(uint8_t cmd) {
  if (!gRing.connected || !gRing.writeChar) return false;
  if (cmd != R1_CMD_HEARTRATE && cmd != R1_CMD_HRV && cmd != R1_CMD_SPO2 &&
      cmd != R1_CMD_TEMPERATURE && cmd != R1_CMD_ACTIVITY && cmd != R1_CMD_SLEEP) {
    return false;
  }
  G2RingTransactionHandle handle{};
  const bool queued = ringEnqueueHealthDataQuery(
      cmd, R1_SUB_DAILY, (uint8_t)(0x20 + cmd), &handle);
  if (queued) {
    DEBUG_RING_TXNF("[RING] daily query queued cmd=0x%02X tx=%lu gen=%lu",
              (unsigned)cmd, (unsigned long)handle.id,
              (unsigned long)handle.generation);
  }
  return queued;
}

void g2RingPollVitalForLogging(void) {
  if (!gRing.connected || !gRing.writeChar ||
      !g2RingPointPollingSupported()) return;
  static uint8_t cursor = 0;
  static uint32_t lastMs = 0;
  const uint32_t now = millis();
  if (lastMs != 0 && (long)(now - lastMs) < 700) return;
  (void)g2RingPollVital(cursor);
  cursor = (uint8_t)((cursor + 1) % G2_RING_POLL_VITAL_COUNT);
  lastMs = now;
}

void g2RingGetTelemetry(G2RingTelemetry& out) {
  RingTelemetrySnapshot snapshot{};
  const bool available = ringSnapshotTelemetry(snapshot);
  ringPopulateTelemetryOutput(available ? &snapshot : nullptr, out);
}

void g2RingGetStatus(char* buf, size_t cap) {
  if (!buf || cap == 0) return;
  const uint32_t nowMs = millis();
  const uint32_t upMs  = gRing.connected && gRing.connectedSince
                         ? (nowMs - gRing.connectedSince) : 0;
  const uint32_t stackFree = sRingOwnerTask
      ? (uint32_t)uxTaskGetStackHighWaterMark(sRingOwnerTask)
      : RING_OWNER_STACK_BYTES;
  const uint32_t stackUsed = stackFree < RING_OWNER_STACK_BYTES
      ? RING_OWNER_STACK_BYTES - stackFree
      : 0;
  snprintf(buf, cap,
           "ring=%s name='%s' addr=%s mtu=%u rx=%lu up=%u.%03us "
           "stack=%lu/%luB (scan=%s pending=%u)",
           gRing.connected ? "up" : "down",
           gRingDeviceName.length() ? gRingDeviceName.c_str() : "<unknown>",
           gRingDeviceAddress.length() ? gRingDeviceAddress.c_str() : "--",
           (unsigned)gRing.mtu,
           (unsigned long)gRing.packetsReceived,
           (unsigned)(upMs / 1000), (unsigned)(upMs % 1000),
           (unsigned long)stackUsed,
           (unsigned long)RING_OWNER_STACK_BYTES,
           gRingScanFound ? "found" : "not-found",
           (unsigned)gRingConnectTaskActive);
}

// =============================================================================
// Spoof-push to glasses
// =============================================================================
// The glasses' built-in UI normally consumes ring telemetry via a phone-app
// bridge: the phone connects to both the ring and the glasses, polls the
// ring, then forwards each metric in a sid=0x90 RingDataPackage frame to
// the right temple. We're impersonating that bridge — we already have the
// ring connected directly to us, so we poll the ring ourselves and synthesize
// the same sid=0x90 frame to the glasses. The glasses don't know whether
// the data came from a real bridge or our spoof.
//
// Only sent to the right temple — the left one ignores ring frames in the
// official protocol. Send is best-effort: dropped silently if the right
// temple isn't connected.
static bool ringSpoofSendOnce(uint32_t magic) {
  if (!gRing.connected) {
    DEBUG_RING_BRIDGEF("[RING] spoof: ring not connected, skipping push");
    return false;
  }
  // Keep the cache generation stable through the actual glasses send. A
  // reconnect reset cannot publish a new link between selecting old values
  // and transmitting them as if they belonged to that new session.
  RingTelemetryGuard telemetry;
  if (!telemetry.locked() || !gRing.connected) return false;

  G2RingPushFields f = {};
  uint32_t now = (uint32_t)time(nullptr);

  if (gR1Cache.batteryValid) {
    f.battery_valid = true;
    f.battery       = (int32_t)gR1Cache.battery;
  }
  const bool liveVitalsSupported = g2RingPointPollingSupported() ||
      g2RingHealthPageRefreshSupported();
  if (liveVitalsSupported && gR1Cache.hrValid) {
    f.hr_valid = true;
    f.hr       = (int32_t)gR1Cache.hr;
    f.hrTs     = (int32_t)gR1Cache.hrTs;
  }
  if (liveVitalsSupported && gR1Cache.hrvValid) {
    f.hrv_valid = true;
    f.hrv       = (int32_t)gR1Cache.hrv;
    f.hrvTs     = (int32_t)gR1Cache.hrvTs;
  }
  if (liveVitalsSupported && gR1Cache.spo2Valid) {
    f.spo2_valid = true;
    f.spo2       = (int32_t)gR1Cache.spo2;
    f.spo2Ts     = (int32_t)gR1Cache.spo2Ts;
  }

  if (!f.battery_valid && !f.hr_valid && !f.hrv_valid && !f.spo2_valid) {
    DEBUG_RING_BRIDGEF("[RING] spoof: cache empty (no fresh telemetry yet) "
              "— nothing to push at t=%lu", (unsigned long)now);
    return false;
  }

  uint8_t env[256];
  uint8_t seq = g2AllocSeq();
  size_t envLen = g2BuildRingRawDataPush(seq, magic, f, env, sizeof(env));
  if (envLen == 0) {
    DEBUG_RING_BRIDGEF("[RING] spoof: builder failed (envelope buffer too small?)");
    return false;
  }
  bool ok = g2SendToRightTemple(env, envLen);
  DEBUG_RING_BRIDGEF("[RING] spoof TX seq=0x%02X envLen=%u %s — "
            "batt=%s%d hr=%s%d hrv=%s%d spo2=%s%d",
            seq, (unsigned)envLen, ok ? "sent" : "DROPPED(R-temple down?)",
            f.battery_valid ? "" : "?", f.battery,
            f.hr_valid      ? "" : "?", f.hr,
            f.hrv_valid     ? "" : "?", f.hrv,
            f.spo2_valid    ? "" : "?", f.spo2);
  return ok;
}

// Background task — wakes every gSpoofIntervalSec, refreshes whatever the
// active profile permits, then synthesizes the sid=0x90 push to the right
// temple from the accepted cache.
//
// Legacy cadence per wake: hr → 700ms → hrv → 700ms → spo2 → 700ms
// → deviceStatus → 600ms → push. On 2.2.9, POINT is deliberately absent,
// so this legacy bridge task refreshes deviceStatus only and pushes any
// already-accepted DAILY/passive vitals in the cache.
//
// We keep using vTaskDelay between writes (the user feedback rule about
// avoiding per-action tasks): one persistent task, multiple sequenced
// writes inside it. DRAM cost is one task TCB + 4KB stack.
static void ringSpoofTaskBody(void* /*arg*/) {
  DEBUG_RING_BRIDGEF("[RING] spoof task: started, interval=%us", (unsigned)gSpoofIntervalSec);
  uint32_t magicCounter = 0;
  while (gSpoofEnabled) {
    const uint32_t cycleStartedMs = millis();
    if (gRing.connected && gRing.writeChar) {
      // Poll each metric. Ignore queue failures — cache simply won't
      // refresh for that metric this cycle.
      auto sendQ = [](uint8_t cmd, uint8_t sub, uint8_t ckey, const char* tag) {
        G2RingTransactionHandle handle{};
        if (ringEnqueueHealthDataQuery(cmd, sub, ckey, &handle)) {
          DEBUG_RING_BRIDGEF("[RING] spoof poll queued %s tx=%lu", tag,
                    (unsigned long)handle.id);
        }
      };
      if (g2RingPointPollingSupported()) {
        sendQ(R1_CMD_HEARTRATE, R1_SUB_POINT, 1, "hrPoint");
        vTaskDelay(pdMS_TO_TICKS(700));
        sendQ(R1_CMD_HRV,       R1_SUB_POINT, 2, "hrvPoint");
        vTaskDelay(pdMS_TO_TICKS(700));
        sendQ(R1_CMD_SPO2,      R1_SUB_POINT, 3, "spo2Point");
        vTaskDelay(pdMS_TO_TICKS(700));
      } else {
        DEBUG_RING_BRIDGEF(
            "[RING] spoof: point refresh unsupported on active profile");
      }

      // Battery comes from deviceStatus — generic system-module probe.
      {
        G2RingTransactionHandle handle{};
        if (ringEnqueueSystemQuery(R1_SUB_DEVICE_STATUS,
                                   /*coalesceKey=*/5, &handle)) {
          DEBUG_RING_BRIDGEF("[RING] spoof poll queued deviceStatus tx=%lu",
                    (unsigned long)handle.id);
        }
      }
      vTaskDelay(pdMS_TO_TICKS(600));

      // Vary magic per cycle so a glasses-side dedup doesn't collapse
      // identical-payload pushes.
      uint32_t mag = G2_MAGIC_RING_RAW_PUSH + (magicCounter++ & 0x7F);
      ringSpoofSendOnce(mag);
    } else {
      DEBUG_RING_BRIDGEF("[RING] spoof task: ring not connected — skipping cycle");
    }

    // Sleep the remainder of the interval. Small ticks so an `off` flip
    // gets noticed quickly without bashing the scheduler.
    const uint32_t targetMs = gSpoofIntervalSec * 1000u;
    const uint32_t elapsedMs = (uint32_t)(millis() - cycleStartedMs);
    uint32_t remain = elapsedMs < targetMs ? targetMs - elapsedMs : 0;
    while (remain > 0 && gSpoofEnabled) {
      uint32_t step = remain > 500 ? 500 : remain;
      vTaskDelay(pdMS_TO_TICKS(step));
      remain -= step;
    }
  }
  DEBUG_RING_BRIDGEF("[RING] spoof task: exiting");
  gSpoofTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

static bool ringSpoofStart(uint32_t intervalSec) {
  if (gSpoofEnabled) return false;
  if (intervalSec < 10)  intervalSec = 10;
  if (intervalSec > 600) intervalSec = 600;
  gSpoofIntervalSec = intervalSec;
  gSpoofEnabled     = true;
  taskStackRecord("ring_spoof", 4096);
  BaseType_t rc = xTaskCreatePinnedToCore(ringSpoofTaskBody, "ring_spoof",
                              /*stack*/ 4096, nullptr,
                              /*prio*/  4,    &gSpoofTaskHandle, APP_CORE);
  if (rc != pdPASS) {
    DEBUG_RING_BRIDGEF("[RING] spoof task: xTaskCreate failed (rc=%d)", (int)rc);
    gSpoofEnabled    = false;
    gSpoofTaskHandle = nullptr;
    return false;
  }
  return true;
}

static void ringSpoofStop() {
  // Task self-deletes on next loop iteration. We just flip the flag and
  // let it wind down — avoids a vTaskDelete race against the task body's
  // ring writes.
  gSpoofEnabled = false;
}

// =============================================================================
// CLI commands
// =============================================================================

static const char* cmd_ringstatus(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  EXT_RAM_BSS_ATTR static char buf[320];
  if (argWantsJson(args)) {
    const uint32_t upMs = (gRing.connected && gRing.connectedSince) ? (millis() - gRing.connectedSince) : 0;
    const uint32_t stackFree = sRingOwnerTask
        ? (uint32_t)uxTaskGetStackHighWaterMark(sRingOwnerTask)
        : RING_OWNER_STACK_BYTES;
    const uint32_t stackUsed = stackFree < RING_OWNER_STACK_BYTES
        ? RING_OWNER_STACK_BYTES - stackFree
        : 0;
    CompactJson j(buf, sizeof(buf));
    j.kv("schema", 1)
     .kv("connected", (bool)gRing.connected)
     .kv("connectPending", (bool)gRingConnectTaskActive)
     .kv("name", gRingDeviceName.length() ? gRingDeviceName.c_str() : "")
     .kv("addr", gRingDeviceAddress.length() ? gRingDeviceAddress.c_str() : "")
     .kv("mtu", (unsigned)gRing.mtu)
     .kv("rx", (unsigned long)gRing.packetsReceived)
     .kv("upMs", (unsigned long)upMs)
     .kv("stackBytes", (unsigned long)RING_OWNER_STACK_BYTES)
     .kv("stackUsedMax", (unsigned long)stackUsed)
     .kv("stackFreeMin", (unsigned long)stackFree)
     .kv("scanFound", (bool)gRingScanFound);
    return j.c_str();
  }
  g2RingGetStatus(buf, sizeof(buf));
  return buf;
}

// ringconnect [mac]
//   No args: scan-then-connect (uses the new self-scan in ringDoConnect).
//   With MAC: skip the scan entirely, attempt a direct BLE connection to
//   that address. Use this when the ring is bonded to another central
//   (e.g. your phone via the Even app) and so doesn't broadcast where
//   our scan can see it. The BLE controller can sometimes still establish
//   a connection by MAC if the peripheral is in directed-advertising mode
//   or accepts multi-central. Failure modes:
//     - "BLE connect FAILED after Nms" → ring not reachable / not advertising
//       to us / already at its central-count limit
//     - Hangs ~30s then times out → no response from peer at that address
static const char* cmd_ringconnect(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  // This authenticated, user-originated command is the Ring pairing intent.
  // Automatic reconnect and passive MAC persistence must never manufacture
  // owner authority on their own.
  bleStampPairedByIfBlank(BLE_PEER_R1_RING);
  CommandArgs ca(args);
  String a0 = ca.arg(0);
  a0.toLowerCase();

  // `ringconnect reconnect` — glasses R1 "Reconnect Ring" path. Same heap
  // guard + disconnect settle as the old tap-inline helper; runs on
  // cmd_exec (which can afford the 500 ms delay) instead of g2_tap_disp.
  if (a0 == "reconnect") {
    const uint32_t freeNow = ESP.getFreeHeap();
    if (freeNow < 16 * 1024) {
      return "Error: RING: reconnect aborted — DRAM free < 16 KB "
             "(reboot or recover heap; Arduino BLE leak per reconnect)";
    }
    if (g2RingIsConnected()) {
      g2RingDisconnect();
      vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!g2RingConnect()) {
      return g2RingIsConnected() ? "RING: already connected"
                                 : "Error: RING: reconnect failed to start connect task";
    }
    return "RING: reconnect queued — use ringstatus to watch";
  }

  if (ca.count() >= 1) {
    String mac = ca.arg(0);
    if (!g2RingConnectMac(mac)) {
      return "Error: RING: direct-MAC connect failed (already running? MAC format?)";
    }
    EXT_RAM_BSS_ATTR static char buf[200];
    snprintf(buf, sizeof(buf),
             "RING: direct-connect to %s queued (no scan) — watch ringstatus / log",
             mac.c_str());
    return buf;
  }
  if (!g2RingConnect()) {
    return gRing.connected ? "RING: already connected"
                           : "RING: connect failed (see log)";
  }
  return "RING: connect queued — use ringstatus to watch";
}

static const char* cmd_ringdisconnect(const String& /*args*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  g2RingDisconnect(/*userInitiated=*/true);
  return "RING: disconnect requested";
}

// ringscan [seconds]
// Dedicated ring-only scan. It is queued on the same central-operation
// worker as G2/Ring connects so it cannot replace BLEScan's process-global
// callback during an in-flight create-connection.
static const char* cmd_ringscan(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(args);
  uint32_t seconds = 30;
  if (ca.count() >= 1) {
    long n = ca.argInt(0, 30);
    if (n > 0 && n <= 300) seconds = (uint32_t)n;
  }
  if (!g2RingScanAsync(seconds)) {
    return "Error: RING: scan already queued/running or BLE worker unavailable";
  }
  EXT_RAM_BSS_ATTR static char buf[128];
  snprintf(buf, sizeof(buf),
           "RING: %us scan queued — use ringstatus, then ringconnect",
           (unsigned)seconds);
  return buf;
}

struct RingPendingRawSet {
  bool active = false;
  bool originalAdmin = false;
  AuthContext auth{};
  uint8_t module = 0;
  uint8_t cmd = 0;
  uint8_t subCmd = 0;
  uint8_t statusType = 0;
  uint8_t statusMethod = 0;
  uint8_t statusAck = 0;
  uint16_t payloadLen = 0;
  uint8_t payload[R1_MAX_PAYLOAD]{};
};
EXT_RAM_BSS_ATTR static RingPendingRawSet sPendingRawSet;  // PSRAM: ring command state, task-context only

static void ringRawSetConfirmAccepted(void* opaque) {
  const RingPendingRawSet* candidate =
      static_cast<const RingPendingRawSet*>(opaque);
  if (candidate) sPendingRawSet = *candidate;
}

static const char* ringConfirmRawSet(void* /*userData*/) {
  EXT_RAM_BSS_ATTR static char reply[180];  // PSRAM: cmd_exec confirm callback, lock-free
  if (!sPendingRawSet.active || !sPendingRawSet.originalAdmin) {
    sPendingRawSet.active = false;
    return "Error: raw R1 SET authorization expired";
  }
  G2RingTransactionHandle handle{};
  const bool queued = g2RingSubmitRawTransaction(
      sPendingRawSet.module, sPendingRawSet.cmd, sPendingRawSet.subCmd,
      sPendingRawSet.statusType, sPendingRawSet.statusMethod,
      sPendingRawSet.statusAck,
      sPendingRawSet.payloadLen ? sPendingRawSet.payload : nullptr,
      sPendingRawSet.payloadLen, &handle);
  sPendingRawSet.active = false;
  if (!queued) return "Error: raw R1 SET was not queued";
  snprintf(reply, sizeof(reply),
           "RING: raw SET queued (tx=%lu gen=%lu); inspect transaction status for ACK",
           (unsigned long)handle.id, (unsigned long)handle.generation);
  return reply;
}

static const char* ringCancelRawSet(void* /*userData*/) {
  sPendingRawSet.active = false;
  return "RING: raw SET cancelled";
}

// ringquery <subject> [type]
//
// Submit an R1 health/status transaction to the serialized owner.
//
//   subject: wear         — wearStatus probe (response is 1 B: 0=unknown 1=notWear 2=wear)
//            hr | hrv | spo2 | temp | activity | sleep — health-data request
//            raw <mod> <cmd> <sub> — admin-only diagnostic escape hatch;
//                                    every SET requires interactive confirm
//
//   type (for the 6 health subjects only):
//            daily     (default) — aggregated daily history
//            point               — recent measurement points
//            measure             — start a real-time sampling session
//            (the ring rejects unsupported pairs with status=ack/refuse —
//            you'll see that in the decoded log)
static const char* cmd_ringquery(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  EXT_RAM_BSS_ATTR static char ret[200];
  if (!gRing.connected || !gRing.writeChar) {
    return "Error: RING: not connected (run 'ringconnect' first)";
  }

  CommandArgs ca(args);
  if (ca.count() < 1) {
    return "Error: invalid arguments — Usage: ringquery <wear|hr|hrv|spo2|temp|activity|sleep|raw> [type]\n"
           "       ringquery hr [daily|point|measure]   (same shape for hrv/spo2/temp/activity/sleep)\n"
           "       ringquery raw <module> <cmd> <subCmd> [hex_payload] [status=NN]\n"
           "         admin only; SET status requires yes/no confirmation.\n"
           "         decimal mod/cmd/sub; hex payload optional; status byte hex (default 00 = notify/get/ok).\n"
           "         Common status bytes: 00=notify/get/ok (queries), 02=notify/set/ok (writes/SET),\n"
           "         03=ack/set/ok (echoing an ack). Bit layout: bit0 type(0=notify,1=ack), bit1 method\n"
           "         (0=get,1=set), bits2-3 ack(0=ok,1=err,2=refuse,3=notSup).";
  }
  String subject = ca.arg(0); subject.toLowerCase();

  G2RingTransactionHandle handle{};
  bool queued = false;
  const char* tag = "?";

  auto resolveSubCmd = [&](const String& s) -> int {
    if (s.length() == 0)        return R1_SUB_DAILY;     // default
    String t = s; t.toLowerCase();
    if (t == "daily")           return R1_SUB_DAILY;
    if (t == "point")           return R1_SUB_POINT;
    if (t == "measure")         return R1_SUB_MEASURE;
    return -1;
  };

  if (subject == "wear") {
    queued = ringEnqueueSystemQuery(R1_SUB_WEAR_STATUS, 6, &handle);
    tag = "wearStatus";
  } else if (subject == "raw") {
    if (!currentExecIsAdmin()) {
      return "Error: ringquery raw requires admin authorization";
    }
    if (ca.count() < 4) return "Error: ringquery raw: need <module> <cmd> <subCmd> [hex_payload]";
    long mod = ca.argInt(1, -1);
    long cmd = ca.argInt(2, -1);
    long sub = ca.argInt(3, -1);
    if (mod < 0 || mod > 0xFF || cmd < 0 || cmd > 0xFF || sub < 0 || sub > 0xFF) {
      return "Error: ringquery raw: module/cmd/subCmd must be 0..255";
    }

    // Optional hex payload (e.g. "01" or "01FF" or "0x01 0xFF" — strip
    // 0x prefixes, spaces, and colons, then parse pairs of hex digits).
    //
    // Optional `status=NN` token: a single full status-byte override, hex.
    // Default 0x00 = notify/get/ok (the read-only path). Bit layout:
    //   bit 0   : type    0=notify, 1=ack
    //   bit 1   : method  0=get,    1=set
    //   bits 2-3: ack     0=ok, 1=error, 2=refuse, 3=notSupport
    // Common values:
    //   0x00 notify/get/ok  — default, used to read most opcodes
    //   0x02 notify/set/ok  — write/enable opcodes (e.g. touchSwitch with payload)
    //   0x03 ack/set/ok     — echoing the ring's own ack shape
    // Discovery context: until this knob existed, every probe was notify/get,
    // which is silently ignored or returns ack/set/error on write-only opcodes
    // (userInfo, touchSwitch, etc.). See R1_RING_PROTOCOL.md §6 for per-opcode
    // markings.
    uint8_t statusByte = 0x00;
    uint8_t payload[R1_MAX_PAYLOAD];
    size_t payloadLen = 0;
    if (ca.count() >= 5) {
      // Pass 1: peel off any `status=NN` token, concatenate the rest as the
      // payload hex string. Order-insensitive — `status=` can appear before,
      // after, or between payload-hex tokens.
      //
      // Detection uses indexOf('=') + case-insensitive key compare rather
      // than startsWith() because an earlier version of this code lost the
      // status= token silently (likely from String::startsWith with an
      // implicit const-char* prefix conversion). indexOf is unambiguous.
      String raw;
      for (int i = 4; i < ca.count(); i++) {
        String arg = ca.arg(i);
        int eqPos = arg.indexOf('=');
        bool isStatusToken = false;
        if (eqPos > 0) {
          String key = arg.substring(0, eqPos);
          key.toLowerCase();
          if (key == "status") {
            isStatusToken = true;
            String val = arg.substring(eqPos + 1);
            val.toLowerCase();
            if (val.startsWith("0x")) val = val.substring(2);
            if (val.length() == 0 || val.length() > 2) {
              return "Error: ringquery raw: status=NN must be 1-2 hex digits (00..FF)";
            }
            char* end = nullptr;
            long parsed = strtol(val.c_str(), &end, 16);
            if (end == val.c_str() || parsed < 0 || parsed > 0xFF) {
              return "Error: ringquery raw: status=NN must be a hex byte 00..FF";
            }
            statusByte = (uint8_t)parsed;
          }
        }
        if (!isStatusToken) {
          arg.replace("0x", "");
          arg.replace("0X", "");
          raw += arg;
        }
      }
      // Debug breadcrumb so future "status= didn't take" mysteries can be
      // verified at a glance instead of guessing.
    DEBUG_RING_DUMPF("[RING] raw-args: count=%d payloadHex='%s' statusByte=0x%02X",
                ca.count(), raw.c_str(), (unsigned)statusByte);
      // Pass 2: parse the hex payload from whatever remains. Strip everything
      // that isn't a hex digit (handles "0x", spaces, colons in the input).
      String clean;
      for (size_t i = 0; i < raw.length(); i++) {
        char c = raw.charAt(i);
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F')) {
          clean += c;
        }
      }
      if (clean.length() > 0) {
        if (clean.length() & 1) {
          return "Error: ringquery raw: hex payload must be an even number of hex digits";
        }
        payloadLen = clean.length() / 2;
        if (payloadLen > R1_MAX_PAYLOAD) {
          return "Error: ringquery raw: payload too large";
        }
        for (size_t i = 0; i < payloadLen; i++) {
          char hi = clean.charAt(2 * i);
          char lo = clean.charAt(2 * i + 1);
          auto nyb = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return 0;
          };
          payload[i] = (uint8_t)((nyb(hi) << 4) | nyb(lo));
        }
      }
    }

    if ((statusByte & 0xF0) != 0) {
      return "Error: ringquery raw: status upper bits must be zero (00..0F)";
    }
    // Decompose the status byte into the three fields the encoder takes.
    const uint8_t statusType   = (statusByte >> 0) & 0x01;
    const uint8_t statusMethod = (statusByte >> 1) & 0x01;
    const uint8_t statusAck    = (statusByte >> 2) & 0x03;

    if (statusMethod == R1_STATUS_METHOD_SET &&
        mod == R1_MODULE_SYSTEM && cmd == R1_CMD_SYSTEM &&
        sub == R1_SUB_USER_INFO) {
      return "Error: raw userInfo SET is intentionally unsupported";
    }

    tag = "raw";
    DEBUG_RING_DUMPF("[RING] raw: mod=%lu cmd=%lu sub=%lu status=0x%02X (%s/%s/%s) payloadLen=%u",
              (unsigned long)mod, (unsigned long)cmd, (unsigned long)sub,
              (unsigned)statusByte,
              r1StatusTypeName(statusType),
              r1StatusMethodName(statusMethod),
              r1StatusAckName(statusAck),
              (unsigned)payloadLen);
    if (statusMethod == R1_STATUS_METHOD_SET) {
      RingPendingRawSet candidate{};
      candidate.active = true;
      candidate.originalAdmin = currentExecIsAdmin();
      candidate.auth = currentAuthContext();
      candidate.module = (uint8_t)mod;
      candidate.cmd = (uint8_t)cmd;
      candidate.subCmd = (uint8_t)sub;
      candidate.statusType = statusType;
      candidate.statusMethod = statusMethod;
      candidate.statusAck = statusAck;
      candidate.payloadLen = (uint16_t)payloadLen;
      if (payloadLen) memcpy(candidate.payload, payload, payloadLen);
      String prompt = "Send raw R1 SET ";
      prompt += String((unsigned)mod);
      prompt += "/";
      prompt += String((unsigned)cmd);
      prompt += "/";
      prompt += String((unsigned)sub);
      prompt += "? This may change ring state.";
      if (!cliRequestConfirm(prompt, String("ringquery ") + args,
                             ringConfirmRawSet, ringCancelRawSet, nullptr,
                             ringRawSetConfirmAccepted, &candidate)) {
        return "Error: another interactive mode is active; raw SET not queued";
      }
      return cliConfirmPromptResponse();
    }
    queued = g2RingSubmitRawTransaction(
        (uint8_t)mod, (uint8_t)cmd, (uint8_t)sub,
        statusType, statusMethod, statusAck,
        payloadLen ? payload : nullptr, payloadLen, &handle);
  } else {
    int cmdId = -1;
    if      (subject == "hr"       || subject == "heartrate")   cmdId = R1_CMD_HEARTRATE;
    else if (subject == "hrv")                                  cmdId = R1_CMD_HRV;
    else if (subject == "spo2")                                 cmdId = R1_CMD_SPO2;
    else if (subject == "temp"     || subject == "temperature") cmdId = R1_CMD_TEMPERATURE;
    else if (subject == "activity")                             cmdId = R1_CMD_ACTIVITY;
    else if (subject == "sleep")                                cmdId = R1_CMD_SLEEP;
    else {
      snprintf(ret, sizeof(ret), "ringquery: unknown subject '%s'", subject.c_str());
      return ret;
    }
    int subCmd = resolveSubCmd(ca.arg(1));
    if (subCmd < 0) return "ringquery: type must be daily|point|measure";
    const uint8_t coalesceKey = subCmd == R1_SUB_DAILY
                                    ? (uint8_t)(0x20 + cmdId)
                                    : 0;
    queued = ringEnqueueHealthDataQuery((uint8_t)cmdId, (uint8_t)subCmd,
                                        coalesceKey, &handle);
    tag = subject.c_str();
  }

  if (!queued) return "Error: RING: transaction was not queued";
  DEBUG_RING_TXNF("[RING] query queued '%s' tx=%lu gen=%lu",
            tag, (unsigned long)handle.id, (unsigned long)handle.generation);
  snprintf(ret, sizeof(ret),
           "RING: query '%s' queued (tx=%lu gen=%lu). Inspect status/log for the response.",
           tag, (unsigned long)handle.id, (unsigned long)handle.generation);
  return ret;
}

// ringtoglasses on [interval_sec] | off | now | status
//
// Spoof-push ring telemetry into the glasses' built-in UI by synthesizing
// sid=0x90 RingDataPackage frames to the right temple. We poll the ring
// directly (we already have it connected), cache decoded HR/HRV/SpO2/battery
// from the notifies, and forward whatever's fresh to the glasses on a
// periodic schedule. The glasses can't tell the data isn't coming from a
// real phone-app bridge.
//
//   on [sec]   start the background pusher. Default 30s, clamp [10..600].
//   off        stop pushing
//   now        send a one-shot push using whatever's currently in the cache
//              (won't poll first — useful to test the codec without waiting
//              a whole interval)
//   status     print enable flag, interval, and current cache contents
// DEPRECATED: unregistered from g2RingCommands — see registry table for why.
// Kept as compiled reference so the spoof-push plumbing isn't lost.
__attribute__((unused))
static const char* cmd_ringtoglasses(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  EXT_RAM_BSS_ATTR static char ret[300];
  CommandArgs ca(args);
  String sub = ca.arg(0); sub.toLowerCase();

  if (sub == "" || sub == "status") {
    G2RingTelemetry telemetry{};
    g2RingGetTelemetry(telemetry);
    snprintf(ret, sizeof(ret),
             "ringtoglasses: %s interval=%us cache: hr=%s%u hrv=%s%d spo2=%s%u batt=%s%u",
             gSpoofEnabled ? "ON" : "off",
             (unsigned)gSpoofIntervalSec,
             telemetry.hrValid      ? "" : "?", (unsigned)telemetry.hr,
             telemetry.hrvValid     ? "" : "?", (int)telemetry.hrv,
             telemetry.spo2Valid    ? "" : "?", (unsigned)telemetry.spo2,
             telemetry.batteryValid ? "" : "?", (unsigned)telemetry.battery);
    return ret;
  }

  if (sub == "on") {
    if (gSpoofEnabled) return "ringtoglasses: already running (use 'off' to stop first)";
    long secs = (ca.count() >= 2) ? ca.argInt(1, 30) : 30;
    if (secs < 10)  secs = 10;
    if (secs > 600) secs = 600;
    if (!ringSpoofStart((uint32_t)secs)) {
      return "Error: ringtoglasses: failed to start task (out of memory?)";
    }
    snprintf(ret, sizeof(ret),
             "ringtoglasses: started (interval=%lds). First push in ~%lds.",
             secs, secs);
    return ret;
  }

  if (sub == "off") {
    if (!gSpoofEnabled) return "ringtoglasses: not running";
    ringSpoofStop();
    return "ringtoglasses: stop requested (task winds down on next cycle)";
  }

  if (sub == "now") {
    if (!ringSpoofSendOnce(G2_MAGIC_RING_RAW_PUSH)) {
      return "Error: ringtoglasses: one-shot push failed (cache empty? right temple down?)";
    }
    return "ringtoglasses: one-shot push sent (see [RING] spoof TX log)";
  }

  return "Error: invalid arguments — Usage: ringtoglasses <on [sec]|off|now|status>";
}

// =============================================================================
// Bridge keepalive task
// =============================================================================
// Phase 5 of docs/g2_proto/Ring_Bridge_Sequence.h: while the bridge is active,
// the host maintains a 30-second heartbeat on sid=0x80 cmd=14
// (BASE_CONNECT_HEARTBEAT, empty body) to keep the temple's bridge state from
// timing out. Without this the bridge has been observed to fail to complete
// its bond with the ring — the temple ack's our RING_CONNECT_INFO trigger but
// then sits silent.
//
// Started by `ringbridge on`, stopped by `ringbridge off` (or any path that
// flips gBridgeRequested to false). Sends to BOTH temples — the doc is
// ambiguous about whether only the right one needs it; both is cheap and
// matches the existing g2devcfg-heartbeat broadcast pattern.
static volatile bool   gBridgeHbEnabled    = false;
static TaskHandle_t    gBridgeHbTaskHandle = nullptr;

static void ringBridgeHeartbeatBody(void* /*arg*/) {
  DEBUG_RING_BRIDGEF("[RING] bridge-heartbeat task: started (30s cadence)");
  while (gBridgeHbEnabled) {
    uint8_t env[40];
    size_t envLen = g2BuildDevCfgHeartbeat(g2AllocSeq(),
                                           G2_MAGIC_DEVCFG_HEARTBEAT,
                                           env, sizeof(env));
    if (envLen > 0) {
      bool ok = g2SendToRightTemple(env, envLen);
      DEBUG_RING_BRIDGEF("[RING] bridge-heartbeat → R: %s",
                ok ? "sent" : "DROPPED (R-temple down)");
    }
    // Sleep 30s in 500ms slices so an `off` flip lands quickly.
    uint32_t remain = 30000;
    while (remain > 0 && gBridgeHbEnabled) {
      uint32_t step = remain > 500 ? 500 : remain;
      vTaskDelay(pdMS_TO_TICKS(step));
      remain -= step;
    }
  }
  DEBUG_RING_BRIDGEF("[RING] bridge-heartbeat task: exiting");
  gBridgeHbTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

static bool ringBridgeHeartbeatStart() {
  if (gBridgeHbEnabled) return true;
  gBridgeHbEnabled = true;
  taskStackRecord("ring_bridge_hb", 3072);
  BaseType_t rc = xTaskCreatePinnedToCore(ringBridgeHeartbeatBody, "ring_bridge_hb",
                              /*stack*/ 3072, nullptr,
                              /*prio*/  3,    &gBridgeHbTaskHandle, APP_CORE);
  if (rc != pdPASS) {
    DEBUG_RING_BRIDGEF("[RING] bridge-heartbeat: xTaskCreate failed (rc=%d)", (int)rc);
    gBridgeHbEnabled    = false;
    gBridgeHbTaskHandle = nullptr;
    return false;
  }
  return true;
}

static void ringBridgeHeartbeatStop() {
  // Task self-deletes on next loop iteration.
  gBridgeHbEnabled = false;
}

// =============================================================================
// Bridge progress mirror — populated by parseSid80Rx via a public hook
// =============================================================================
// The right temple reports bridge-attempt progress via the connRet field on
// sid=0x80 RING_CONNECT_INFO polls. We capture the most recent value here so
// `ringbridge status` can show it without forcing the user to scroll logs.
//
// connRet values seen in the wild (from R1_RE_Reference / live captures):
//   0  unknown / not reported
//   1  scanning (temple is looking for the ring)
//   8  fail-terminal (bond attempt gave up)
//   19 scanning? (alternative scanning code)
//   62 unknown error (sometimes seen during bridge teardown)
//   <other>  see connRetHint() in G2_Glasses.cpp for the full table
struct R1BridgeProgress {
  uint32_t lastConnRet;     uint32_t lastConnRetMs;     bool hasConnRet;
  uint32_t lastConnectRing; uint32_t lastConnectRingMs; bool hasConnectRing;
};
static R1BridgeProgress gBridgeProgress;

void g2RingNoteBridgePoll(uint64_t connRet, bool hasConnRet,
                          uint64_t connectRing, bool hasConnectRing) {
  uint32_t now = millis();
  if (hasConnRet) {
    gBridgeProgress.lastConnRet   = (uint32_t)connRet;
    gBridgeProgress.lastConnRetMs = now;
    gBridgeProgress.hasConnRet    = true;
  }
  if (hasConnectRing) {
    gBridgeProgress.lastConnectRing   = (uint32_t)connectRing;
    gBridgeProgress.lastConnectRingMs = now;
    gBridgeProgress.hasConnectRing    = true;
  }
}

// ringbridge on | off | status
//
// Hand the ring's BLE link off to the right temple's built-in bridge
// firmware so the glasses' OWN health UI displays ring data natively. The
// temple cannot bridge while we hold the ring (it's a single-central
// peripheral), so `on` first drops our link + disables auto-reconnect,
// then sends sid=0x80 cmd=6 RING_CONNECT_INFO with connectRing=true to the
// right temple. The temple then scans for the ring, bonds, and starts
// forwarding telemetry to us as sid=0x90 RingDataPackage frames — which
// our handleEnvelope→g2RingNoteForwardedTelemetry path catches and pushes
// into the same gR1Cache the direct path uses.
//
// `off` reverses the handoff: send connectRing=false to the temple to
// release the ring, re-enable auto-reconnect (so a future boot grabs it
// directly), and optionally `ringconnect` ourselves.
//
// `status` reports the current mode + cache state.
//
// We persist the mode in `gBridgeRequested` (RAM-only — survives the rest
// of the session but a reboot returns to the bleautoreconnect default).
static volatile bool gBridgeRequested = false;

// DEPRECATED: unregistered from g2RingCommands — see registry table for why.
// Kept as compiled reference so the RING_CONNECT_INFO + heartbeat plumbing
// isn't lost.
__attribute__((unused))
static const char* cmd_ringbridge(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  EXT_RAM_BSS_ATTR static char ret[300];
  CommandArgs ca(args);
  String sub = ca.arg(0); sub.toLowerCase();
  BlePeerSavedTargetSnapshot savedRing{};
  (void)blePeerSavedTargetSnapshot(BLE_PEER_R1_RING, savedRing);

  if (sub == "" || sub == "status") {
    G2RingTelemetry telemetry{};
    g2RingGetTelemetry(telemetry);
    // Decode the most recent connRet for a friendly hint.
    const char* connRetWord = "?";
    char ageBuf[24] = {0};
    if (gBridgeProgress.hasConnRet) {
      switch (gBridgeProgress.lastConnRet) {
        case 0:  connRetWord = "idle";          break;
        case 1:  connRetWord = "scanning";      break;
        case 8:  connRetWord = "fail-terminal"; break;
        case 19: connRetWord = "scanning?";     break;
        case 62: connRetWord = "err?";          break;
        default: connRetWord = "?";             break;
      }
      uint32_t ageMs = millis() - gBridgeProgress.lastConnRetMs;
      snprintf(ageBuf, sizeof(ageBuf), " %us-ago", (unsigned)(ageMs / 1000));
    }
    snprintf(ret, sizeof(ret),
             "ringbridge: mode=%s autoReconnect=%s ring-link=%s heartbeat=%s "
             "tempStatus: connectRing=%s%u connRet=%u(%s)%s "
             "fwd-cache: hr=%s%u hrv=%s%d spo2=%s%u batt=%s%u",
             gBridgeRequested ? "BRIDGE (temple owns ring)" : "DIRECT (we own ring)",
             blePeerAutoReconnectEnabled(BLE_PEER_R1_RING) ? "on" : "off",
             gRing.connected ? "up" : "down",
             gBridgeHbEnabled ? "on" : "off",
             gBridgeProgress.hasConnectRing ? "" : "?",
             (unsigned)gBridgeProgress.lastConnectRing,
             (unsigned)gBridgeProgress.lastConnRet, connRetWord, ageBuf,
             telemetry.hrValid      ? "" : "?", (unsigned)telemetry.hr,
             telemetry.hrvValid     ? "" : "?", (int)telemetry.hrv,
             telemetry.spo2Valid    ? "" : "?", (unsigned)telemetry.spo2,
             telemetry.batteryValid ? "" : "?", (unsigned)telemetry.battery);
    return ret;
  }

  if (sub == "on") {
    // Need a saved ring MAC + name to tell the temple what to bond with.
    // Prefer the live values if we're connected; fall back to the persisted
    // peer-registry entry.
    String mac;
    String name;
    if (gRingDeviceAddress.length() > 0) {
      mac  = gRingDeviceAddress;
      name = gRingDeviceName;
    } else {
      mac = savedRing.target.mac1;
    }
    mac.trim(); name.trim();
    if (mac.length() < 17) {
      return "Error: ringbridge on: no ring MAC known — `ringconnect` once first "
             "(or `ringscan`) so we have something to tell the temple.";
    }
    if (name.length() == 0) {
      // FlutterApp comments suggest the name doesn't actually matter
      // operationally (the temple uses MAC for the connect), but the proto
      // requires non-empty bytes. Use a placeholder if we don't know it.
      name = "EVEN R1";
    }

    // Parse "f8:29:ca:ba:ac:1c" → byte array (BLE address order = high byte
    // first). g2BuildDevCfgRingConnect reverses internally to wire order.
    uint8_t macBle[6] = {0};
    {
      const char* s = mac.c_str();
      for (int i = 0; i < 6; i++) {
        unsigned v = 0;
        if (sscanf(s + i * 3, "%2x", &v) != 1) {
          return "Error: ringbridge on: failed to parse ring MAC";
        }
        macBle[i] = (uint8_t)v;
      }
    }

    // FlutterApp's ring_bridge_coordinator.dart sequence:
    //   if (!g2.isConnected || !r1.isConnected) return;
    //   await r1.sendAdvStart(g2RightId: g2.rightId!);
    //   await g2.sendConnectRing(...);
    // Two important properties:
    //   1. The ring MUST already be connected to us (the host). The R1 ring
    //      supports multi-central — the temple bonds with it in PARALLEL while
    //      we keep our link. An earlier version of this firmware disconnected
    //      from the ring before triggering, which appears to have killed the
    //      pairing context the ring needed to accept the temple's bond.
    //   2. Firmware 2.2.7.0005 requires both temple identities. The profiled
    //      encoder emits reversed-right followed by reversed-left; missing
    //      either identity fails closed.
    if (!gRing.connected || !gRing.writeChar) {
      return "Error: ringbridge on: ring is not currently connected — run "
             "`ringconnect` first, then re-issue `ringbridge on`. The R1 must "
             "stay connected to us throughout the bridge (the temple bonds in "
             "parallel; multi-central is supported).";
    }

    {
      G2RingTransactionHandle advHandle{};
      if (!ringEnqueueAdvStart(&advHandle)) {
        DEBUG_RING_BRIDGEF("[RING] ringbridge: WARN dual-MAC advStart not queued; "
                  "trigger will go without refresh");
      } else {
        // Dormant diagnostic path: wait only for the owner-correlated ACK;
        // encoder serial allocation and the GATT write remain owner-exclusive.
        for (uint8_t i = 0; i < 30; ++i) {
          G2RingTransactionStatus status{};
          if (g2RingGetTransactionStatus(advHandle, status) &&
              status.completedAtMs != 0) break;
          vTaskDelay(pdMS_TO_TICKS(100));
        }
      }
    }

    // Build + send RING_CONNECT_INFO to the right temple. seq=0 — we don't
    // expect to correlate replies with this since we're not tracking the
    // bridge-attempt response yet (the temple will report progress via the
    // sid=0x80 connRet polls we already log).
    uint8_t env[80];
    size_t envLen = g2BuildDevCfgRingConnect(g2AllocSeq(),
                                             G2_MAGIC_DEVCFG_RING_CONNECT,
                                             /*connect=*/true,
                                             macBle, name.c_str(),
                                             env, sizeof(env));
    if (envLen == 0) return "Error: ringbridge on: failed to build RING_CONNECT_INFO frame";
    if (!g2SendToRightTemple(env, envLen)) {
      return "Error: ringbridge on: send to right temple failed (R-temple down?)";
    }
    gBridgeRequested = true;
    // Start the 30s keepalive so the temple's bridge state doesn't time out.
    ringBridgeHeartbeatStart();
    snprintf(ret, sizeof(ret),
             "ringbridge: ON requested. We are STAYING connected to the ring "
             "(R1 supports multi-central — temple bonds in parallel). Temple "
             "should bond %s within ~10-20s; watch for [G2-R] sid=0x80 RX "
             "RING_CONNECT_INFO connRet=N transitions, then [G2-R] sid=0x90 "
             "forward frames once data is flowing. Re-run `ringbridge status` "
             "to check progress.",
             mac.c_str());
    return ret;
  }

  if (sub == "off") {
    // Same payload shape as `on`, but connectRing=false — releases the
    // ring on the temple side. Use cached MAC/name.
    String mac  = gRingDeviceAddress.length() ? gRingDeviceAddress
                                              : String(savedRing.target.mac1);
    String name = gRingDeviceName.length() ? gRingDeviceName : String("EVEN R1");
    mac.trim();
    if (mac.length() < 17) {
      return "Error: ringbridge off: no MAC known to address the release — "
             "send `g2devcfg ring <mac> <name>` manually if you need to.";
    }
    uint8_t macBle[6] = {0};
    {
      const char* s = mac.c_str();
      for (int i = 0; i < 6; i++) {
        unsigned v = 0;
        if (sscanf(s + i * 3, "%2x", &v) != 1) {
          return "Error: ringbridge off: failed to parse ring MAC";
        }
        macBle[i] = (uint8_t)v;
      }
    }
    uint8_t env[80];
    size_t envLen = g2BuildDevCfgRingConnect(g2AllocSeq(),
                                             G2_MAGIC_DEVCFG_RING_CONNECT,
                                             /*connect=*/false,
                                             macBle, name.c_str(),
                                             env, sizeof(env));
    if (envLen == 0) return "Error: ringbridge off: failed to build release frame";
    if (!g2SendToRightTemple(env, envLen)) {
      return "Error: ringbridge off: send to right temple failed (R-temple down?)";
    }
    gBridgeRequested = false;
    ringBridgeHeartbeatStop();
    return "ringbridge: OFF requested. Temple should drop the ring within "
           "~5-10s. Our own ring link is unaffected (we kept it during the "
           "bridge), so direct `ringquery ...` commands continue to work.";
  }

  return "Error: invalid arguments — Usage: ringbridge <on|off|status>";
}

extern const CommandEntry g2RingCommands[] = {
  { "ringstatus",     "Show R1 ring connection status",            false, cmd_ringstatus     },
  { "ringscan",       "Scan for the R1 ring: ringscan [seconds] (default 30, max 300)", false, cmd_ringscan, "Usage: ringscan [seconds] (1..300, default 30)" },
  { "ringconnect",    "Connect to the R1 ring: ringconnect [mac|reconnect]", true, cmd_ringconnect, "Usage: ringconnect [mac|reconnect]  (no arg = scan-then-connect; mac = direct; reconnect = drop+settle+connect)" },
  { "ringdisconnect", "Disconnect from the R1 ring",               true, cmd_ringdisconnect },
  { "ringquery",      "Queue an R1 query: ringquery <wear|hr|hrv|spo2|temp|activity|sleep|raw> [type]", false, cmd_ringquery, "Usage: ringquery <wear|hr|hrv|spo2|temp|activity|sleep> [daily|point|measure] | raw <module> <cmd> <subCmd> [hex_payload] [status=NN]  (raw is admin-only; SET requires confirmation)" },
  // NOTE: `ringtoglasses` and `ringbridge` are UNREGISTERED on purpose.
  // Both targeted getting ring data onto the G2's built-in health UI, and
  // both are dead ends. See R1_RING_PROTOCOL.md §13 for the full writeup:
  //   * ringtoglasses (postman) — pushed RingDataPackage on sid=0x90 to the
  //     right temple. Temple silently ignores; the community RE codebases
  //     never send in that direction either (sid=144/145 are temple→host,
  //     not host→temple).
  //   * ringbridge   (matchmaker) — issued RING_CONNECT_INFO so the temple
  //     bonds with the ring directly. Fails with connRet=8 (terminal)
  //     because the R1 firmware accepts only one BLE central at a time
  //     and we still hold the link. Releasing the ESP32's link doesn't
  //     help either (verified empirically; see §13).
  // The implementations of `cmd_ringtoglasses` / `cmd_ringbridge` and their
  // helpers (spoof task, bridge-heartbeat task, telemetry cache,
  // g2BuildRingRawDataPush) are kept compiled-in but unreachable, so any
  // future investigator can re-register them and pick up where we stopped
  // without re-implementing the plumbing.
};
extern const size_t g2RingCommandsCount =
    sizeof(g2RingCommands) / sizeof(g2RingCommands[0]);

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
