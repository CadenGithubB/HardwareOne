#include "System_ESPNow_Sensors.h"

#if ENABLE_ESPNOW

#include <atomic>
#include <memory>
#include <ArduinoJson.h>
#include <esp_random.h>
#include <freertos/semphr.h>

#include "OLED_Display.h"
#include "System_AuthIdentity.h"      // currentAuthContext() — D1 local-console-only gate
#include "System_Clock.h"             // the arrival stamp of a reading
#include "System_Command.h"
#include "System_Debug.h"
#include "System_ESPNow.h"
#include "System_ESPNow_BondForward.h"  // the bond master forwards its partner's readings
#include "System_ESPNow_Deadline.h"   // wrap-safe lease deadlines
#include "System_ESPNow_ForwardRx.h"  // whose is a received reading
#include "System_ESPNow_Handlers_Crypto.h"  // espnowHandshakeAutoStart
#include "System_ESPNow_HandshakeBackoff.h" // WarnLimiter
#include "System_ESPNow_Identity.h"   // peerIdentityFindByMac / espnowIdentityFormatPubHex — fingerprint auth
#include "System_ESPNow_PowerEvents.h" // what this device sends and keeps of battery state and events (status)
#include "System_ESPNow_Wire.h"       // V4PayloadSensorStatus, SENSOR_READING_MAX_BYTES
#include "System_SensorJsonKeys.h"    // the senders' key names (getRemoteGpsData reads them)
#include "System_MemUtil.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"
#include "System_Utils.h"
#include "i2csensor_bno055.h"
#include "i2csensor_ds3231.h"
#include "i2csensor_mlx90640.h"
#include "i2csensor_pa1010d.h"
#include "i2csensor_rda5807.h"
#include "i2csensor_seesaw.h"
#include "i2csensor_sths34pf80.h"
#include "i2csensor_vl53l4cx.h"

#if ENABLE_GAMEPAD_SENSOR
#include "i2csensor_seesaw.h"
#elif ENABLE_ANO_ENCODER
#include "i2csensor_ano_encoder.h"   // anoEncoderBuildDataJSON — the ANO input builder
#elif ENABLE_GPIO_ENCODER
#include "Input_GPIOEncoder.h"
#endif
#include "i2csensor_pa1010d.h"
#include "i2csensor_bno055.h"
#include "i2csensor_mlx90640.h"
#include "i2csensor_vl53l4cx.h"
#include "i2csensor_rda5807.h"
#include "i2csensor_ds3231.h"
#include "i2csensor_sths34pf80.h"
#include "System_Camera_DVP.h"
#include "System_Microphone.h"

// ==========================
// Readings received from other devices: the table and the per-sender records
// ==========================

static_assert(REMOTE_SENSOR_PEERS_MAX == MESH_PEER_MAX, "one record per paired device");
static_assert(SENSOR_READING_MAX_BYTES == espnowsensor::kReadingMaxBytes, "one maximum");

// SENSOR_RX_STATE_HOST_BEGIN (the host suite espnow_sensor_ingest compiles from here ...)
// One slot: the latest reading of one sensor of one sender.
struct RemoteSensorData {
  char jsonData[SENSOR_READING_MAX_BYTES + 1];  // the reading's text, terminated
  uint16_t jsonLength;
  uint32_t lastUpdate;      // millis() when the latest reading arrived
  uint32_t lastSeen;        // millis() of the last reading or status frame
  int64_t arrivalEpochMs;   // this device's UTC clock at that arrival; 0: not set
  uint8_t arrivalClock;     // Clock::SyncSource then
  uint16_t counter;         // the sender's counter of that reading
  bool connected;           // the sender has this sensor (a status frame or a reading said so)
  bool enabled;             // the sender is streaming it
  bool valid;               // a reading has arrived (fresh while within REMOTE_SENSOR_TTL_MS)
  bool aged;                // the reading was found older than REMOTE_SENSOR_TTL_MS (remoteSensorAgeTick)
};

// What the receiver keeps per paired sender: its slots (one per sensor type,
// so a sender can never take a slot from another one), the counter state and
// the counts of each of its sensors, and the one reading of it that may be
// arriving in parts. The counter state is not in the slot because a sensor
// that was silent for a minute is no longer listed, and a radio outage is
// exactly when the counter must be remembered. A record is given up only for
// a sender that is no longer paired. (Battery state and events of a sender
// are kept by System_ESPNow_PowerEvents.cpp.) A sender is its origin and, for
// readings a bond master forwards for its partner, that bond master (origin,
// via): what a bond master says about its partner is never filed under the
// partner's own record. espnowfwd::recordReusable says when a record may go.
struct SensorPeerRecord {
  bool inUse;
  uint8_t mac[6];           // the origin
  uint8_t via[6];           // all zero, or the bond master that forwards for it
  uint8_t link;             // espnowfwd::kLink*
  char name[32];
  espnowsensor::CounterTrack track[REMOTE_SENSOR_MAX];
  espnowsensor::CounterCounts counts[REMOTE_SENSOR_MAX];
  uint32_t incomplete;     // readings given up because a part never came
  uint32_t malformed;      // parts or readings refused for their content
  espnowsensor::PartAssembly assembly;                    // espnow_task only
  uint8_t assemblyBuf[espnowsensor::kReadingMaxBytes];   // espnow_task only
  // REMOTE_SENSOR_MAX slots, indexed by sensor type. Made on this sender's
  // first reading or status frame that is kept (PSRAM), and kept for the
  // next sender when the record is given up.
  RemoteSensorData* slots;
};
// The table: REMOTE_SENSOR_PEERS_MAX records (PSRAM). Only a device that is
// sent readings and keeps them has one (a mesh master or backup master, or a
// bond master): it is made by the first frame that passes the role rule, not
// at ESP-NOW start. "Full" means sixteen paired senders.
static SensorPeerRecord* gSensorPeers = nullptr;

// Guards the slots, the counter state and the counts. Held for one copy of a
// reading, or one sender's record, at most: the records and slots are in
// PSRAM, and a spinlock masks interrupts while it is held. Nothing is logged,
// allocated, cleared in bulk or sent under it.
static portMUX_TYPE gRemoteSensorMux = portMUX_INITIALIZER_UNLOCKED;

// Frames the receiver refused, whoever sent them (under gRemoteSensorMux).
struct RemoteSensorRefusals {
  uint32_t noRecord;       // no per-sender record left (more than 16 paired senders)
  uint32_t noMemory;       // the table or a sender's slots could not be made
  uint32_t badType;        // a sensor type this firmware does not know
  uint32_t notReceiver;    // this device's role does not keep readings
  uint32_t malformed;      // too short to be a reading or status frame, or a forwarded mark that makes no sense
  uint32_t forwardRefused; // forwarded, but no active link of the sender names it
};
static RemoteSensorRefusals gRemoteSensorRefusals = {};
// One console line a minute while readings are refused for this device's
// role, and one while they are refused because nothing more can be kept.
static WarnLimiter gSensorRoleRefusalWarn = {};
static WarnLimiter gSensorFullWarn = {};
// SENSOR_RX_STATE_HOST_END

// Master flag to enable/disable all sensor ESP-NOW communication (status + data)
// Must be explicitly enabled before any sensor broadcasts will be sent
static bool gSensorBroadcastEnabled = false;

// Sensor streaming state (worker devices only)
static bool gSensorStreamingEnabled[REMOTE_SENSOR_MAX] = {false};

// Broadcaster task state
static TaskHandle_t gSensorBroadcasterTask = nullptr;
// Serializes the broadcaster lifecycle. Explicit stop is cooperative: the task
// finishes its current builder/send, publishes a join semaphore, then parks so
// the lifecycle owner can delete it without stranding a mutex/runtime guard.
static portMUX_TYPE gSensorBcastTaskMux = portMUX_INITIALIZER_UNLOCKED;
static std::atomic<bool> gSensorBroadcasterStopRequested{false};
static std::atomic<bool> gSensorStreamingShutdown{true};
static bool gSensorBroadcasterExternalStop = false;  // under gSensorBcastTaskMux
static StaticSemaphore_t gSensorBroadcasterStopDoneStorage;
static SemaphoreHandle_t gSensorBroadcasterStopDone = nullptr;
// Serializes the whole create/publish and stop/join/free transactions. A
// portMUX only protects tiny handle/flag snapshots; it cannot be held across
// allocation, xTaskCreate, a join wait, or free.
static StaticSemaphore_t gSensorBroadcasterLifecycleMutexStorage;
static SemaphoreHandle_t gSensorBroadcasterLifecycleMutex =
    xSemaphoreCreateMutexStatic(&gSensorBroadcasterLifecycleMutexStorage);

// Per-sensor "last transmit" timestamps so each sensor paces independently.
// Reset to 0 in startSensorDataStreaming() so the first tick after enable
// always satisfies the interval check (no separate force-send flag needed).
static unsigned long gLastTxMs[REMOTE_SENSOR_MAX] = {0};

// ---- Secure sensor fetcher lease state (worker) ----
// Per-sensor lease deadline (millis). 0 = no lease: manual/local streaming that
// never auto-expires. >0 = a leased subscription that self-stops in the broadcaster
// task once millis() passes it (docs/ESPNOW_SENSOR_FETCHER_DESIGN.md, H3 fix).
static uint32_t gSensorLeaseExpiresAt[REMOTE_SENSOR_MAX] = {0};
// Per-sensor requested cadence (ms) from the leasing controller. 0 = use the global
// gSettings.sensorBroadcastIntervalMs. The broadcaster floors both at spec.minIntervalMs.
static uint32_t gSensorReqIntervalMs[REMOTE_SENSOR_MAX] = {0};
// The authorized controller's MAC: the encrypted-unicast SENSOR_ENVELOPE reply target.
// All-zero = no controller set → leased/manual streaming emits nothing on the air (D2).
// Written and read under gSensorBcastTaskMux, like the lease stamps it belongs to:
// the subscription apply (espnow_task) and the broadcaster's lapse branch both
// change them.
static uint8_t gSensorControllerMac[6] = {0};
// Per sensor: the stream was switched on by hand on this device
// (espnowsensorstream, or the bond partner's stream control). Such a stream
// goes to the master named on this device and is not stopped by the end of a
// lease that ran meanwhile. Under gSensorBcastTaskMux.
static bool gSensorByHand[REMOTE_SENSOR_MAX] = {false};
// The masters named on this device (by key) that are paired, as addresses.
// Refreshed once a second on espnow_task (sensorReceiverTargetsRefresh), read
// by any task as a copy. Under gSensorBcastTaskMux.
static SensorReceiverTargets gSensorTargets = {};

// SENSOR_TX_STATE_HOST_BEGIN
// ---- The sender's counters (System_ESPNow_SensorCore.h) ----
// One counter per sensor, stepped once per reading whoever it goes to, and
// who the last reading of each sensor went to (for the stream-start flag).
// Under gSensorBcastTaskMux.
static uint16_t gSensorTxCounter[REMOTE_SENSOR_MAX] = {0};
static espnowsensor::StreamTarget gSensorTxTarget[REMOTE_SENSOR_MAX] = {};
// What was sent and why not, per sensor. Written by the task that sends (the
// broadcaster); a reader may be one count behind.
struct SensorTxStats {
  uint32_t sent;
  uint32_t notSent[espnowsensor::kDropReasonCount];
};
EXT_RAM_BSS_ATTR static SensorTxStats gSensorTxStats[REMOTE_SENSOR_MAX];  // PSRAM: tasks only
// For the "not sent" console line: the broadcaster only counts here, and
// sensorLinkTick (espnow_task) prints and starts the handshake. The broadcaster
// task's stack has no room for a console line.
static std::atomic<uint32_t> gSensorDropSinceLine[REMOTE_SENSOR_MAX];
static std::atomic<uint8_t> gSensorDropLastReason[REMOTE_SENSOR_MAX];
static WarnLimiter gSensorDropWarn[REMOTE_SENSOR_MAX] = {};  // espnow_task only
// The device readings go to has no session with this device (under gSensorBcastTaskMux).
static bool gSensorNeedSession = false;
static uint8_t gSensorNeedSessionMac[6] = {0};

// The boot tag of every reading, status, battery and event frame this device
// sends in this boot: the low 16 bits of the persistent boot counter, so two
// boots in a row never share one. A random value when that counter is 0 (its
// NVS write failed).
static uint16_t sensorBootTag() {
  static uint16_t tag = 0;
  static bool have = false;
  if (!have) {
    tag = gBootCounter != 0 ? (uint16_t)(gBootCounter & 0xFFFF) : (uint16_t)(esp_random() & 0xFFFF);
    have = true;
  }
  return tag;
}
// SENSOR_TX_STATE_HOST_END

uint16_t espnowSensorBootTag() { return sensorBootTag(); }

// The lease-expiry decision for one streaming slot. Wrap-safe: a lease stamped
// just before the 49.7-day millis() wrap still lapses on time, and one whose
// deadline wrapped past zero does not lapse at once. 0 = manual / no lease.
static bool sensorLeaseLapsed(int i, uint32_t nowMs) {
  return gSensorLeaseExpiresAt[i] != 0 && !espnowDeadlinePending(gSensorLeaseExpiresAt[i], nowMs);
}

// ==========================
// Sensor JSON builder dispatch
// ==========================
//
// Each sensor exports a builder of the shape `int build(char*, size_t)` that
// reads from its own native cache (under the sensor's mutex) and writes JSON
// into the supplied buffer. The broadcaster calls these on demand — there is
// no intermediate "wire cache" to keep in sync, which makes the entire class
// of "first toggle ON shows nothing" bugs structurally impossible.
//
// minIntervalMs controls per-sensor pacing. The input device (gamepad / ANO)
// wants ~10 Hz so button presses feel responsive. TOF, IMU and presence sit at
// 2 Hz — they read as motion, so 1 Hz looks laggy. GPS, FM radio and RTC change
// slowly enough that 1 Hz is plenty. The table below is the authority.
//
// bufBytes is the size required for this sensor's largest possible JSON
// output. The broadcaster allocates ONE shared PSRAM buffer sized to the
// max across all sensors and reuses it forever — no per-tick allocations.

typedef int (*SensorJsonBuilder)(char* buf, size_t bufSize);

struct SensorBroadcastSpec {
  SensorJsonBuilder builder;
  uint16_t minIntervalMs;
  uint16_t bufBytes;
};

static const SensorBroadcastSpec gSensorSpecs[REMOTE_SENSOR_MAX] = {
#if ENABLE_THERMAL_SENSOR
  // DISABLED: the 768-pixel frame is far over the largest reading (472 bytes,
  // System_ESPNow_SensorCore.h). Re-enable over the mesh by pointing this at
  // thermalBuildSummaryJson (~90 B).
  [REMOTE_SENSOR_THERMAL]    = { nullptr, 0, 0 },
#else
  [REMOTE_SENSOR_THERMAL]    = { nullptr, 0, 0 },
#endif
#if ENABLE_TOF_SENSOR
  [REMOTE_SENSOR_TOF]        = { tofBuildDataJson,            500,  1024 },
#else
  [REMOTE_SENSOR_TOF]        = { nullptr, 0, 0 },
#endif
#if ENABLE_IMU_SENSOR
  [REMOTE_SENSOR_IMU]        = { imuBuildDataJson,            500,  512  },
#else
  [REMOTE_SENSOR_IMU]        = { nullptr, 0, 0 },
#endif
#if ENABLE_GPS_SENSOR
  [REMOTE_SENSOR_GPS]        = { gpsBuildDataJson,            1000, 256  },
#else
  [REMOTE_SENSOR_GPS]        = { nullptr, 0, 0 },
#endif
#if ENABLE_GAMEPAD_SENSOR
  [REMOTE_SENSOR_INPUT]    = { gamepadBuildDataJson,        100,  128  },
#elif ENABLE_ANO_ENCODER
  // ANO rotary encoder is the OTHER input device (mutually exclusive with the
  // gamepad — INPUT_DEVICE_TYPE picks exactly one). Same REMOTE_SENSOR_INPUT
  // slot, same 10 Hz cadence + 128 B budget; anoEncoderBuildDataJson emits
  // {"valid":..,"connected":..,"ts":..,"pos":N,"axis":0|1,"buttons":B}
  // (~90 B typical, still under the 128 B budget). Without this branch an
  // ANO build left the input builder nullptr, so the broadcaster's
  // `if (!spec.builder) continue;` silently dropped every input frame —
  // streaming "turned on" but nothing reached the peer.
  [REMOTE_SENSOR_INPUT]    = { anoEncoderBuildDataJson,     100,  128  },
#elif ENABLE_GPIO_ENCODER
  [REMOTE_SENSOR_INPUT]    = { gpioEncoderBuildDataJson,    100,  256  },
#else
  [REMOTE_SENSOR_INPUT]    = { nullptr, 0, 0 },
#endif
#if ENABLE_FM_RADIO
  [REMOTE_SENSOR_FMRADIO]    = { fmRadioBuildDataJson,        1000, 512  },
#else
  [REMOTE_SENSOR_FMRADIO]    = { nullptr, 0, 0 },
#endif
  [REMOTE_SENSOR_CAMERA]     = { nullptr, 0, 0 },
  [REMOTE_SENSOR_MICROPHONE] = { nullptr, 0, 0 },
#if ENABLE_RTC_SENSOR
  [REMOTE_SENSOR_RTC]        = { rtcBuildDataJson,            1000, 256  },
#else
  [REMOTE_SENSOR_RTC]        = { nullptr, 0, 0 },
#endif
#if ENABLE_PRESENCE_SENSOR
  [REMOTE_SENSOR_PRESENCE]   = { presenceBuildDataJson,       500,  256  },
#else
  [REMOTE_SENSOR_PRESENCE]   = { nullptr, 0, 0 },
#endif
  [REMOTE_SENSOR_APDS]       = { nullptr, 0, 0 },
};

// Shared broadcaster buffer (PSRAM). Allocated once on first broadcaster start,
// sized to the largest sensor's bufBytes. Reused across all sensors and all ticks.
static char* gBroadcasterBuf = nullptr;
static size_t gBroadcasterBufSize = 0;

// ==========================
// Initialization
// ==========================

bool remoteSensorTableEnsure() {
  if (gSensorPeers) return true;
  const size_t peerBytes = (size_t)REMOTE_SENSOR_PEERS_MAX * sizeof(SensorPeerRecord);
  SensorPeerRecord* peers = (SensorPeerRecord*)ps_alloc(
      peerBytes, AllocPref::PreferPsram, "espnow.sensorPeers");
  if (!peers) {
    ERROR_SYSTEMF("[REMOTE_SENSORS] Failed to allocate %u bytes of per-sender records", (unsigned)peerBytes);
    return false;
  }
  memset(peers, 0, peerBytes);
  taskENTER_CRITICAL(&gRemoteSensorMux);
  gSensorPeers = peers;
  taskEXIT_CRITICAL(&gRemoteSensorMux);
  return true;
}

bool initRemoteSensorSystem() {
  // A new ESP-NOW start begins with no reading listed. Nothing is allocated
  // here: the table is made by the first reading or status frame this device
  // keeps. The per-sender counter state is kept: the senders' counters did
  // not restart with this device's radio.
  // One sender per hold, and only the flags that list a slot (the text stays
  // behind them, unread).
  for (int i = 0; gSensorPeers && i < REMOTE_SENSOR_PEERS_MAX; i++) {
    taskENTER_CRITICAL(&gRemoteSensorMux);
    SensorPeerRecord& peer = gSensorPeers[i];
    peer.assembly.open = false;
    for (int k = 0; peer.slots && k < REMOTE_SENSOR_MAX; k++) {
      peer.slots[k].connected = false;
      peer.slots[k].enabled = false;
      peer.slots[k].valid = false;
      peer.slots[k].jsonLength = 0;
    }
    taskEXIT_CRITICAL(&gRemoteSensorMux);
  }

  if (!gSensorBroadcasterLifecycleMutex ||
      xSemaphoreTake(gSensorBroadcasterLifecycleMutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }
  if (!gSensorBroadcasterStopDone) {
    gSensorBroadcasterStopDone =
        xSemaphoreCreateBinaryStatic(&gSensorBroadcasterStopDoneStorage);
  }
  if (!gSensorBroadcasterStopDone) {
    xSemaphoreGive(gSensorBroadcasterLifecycleMutex);
    return false;
  }
  gSensorStreamingShutdown.store(false, std::memory_order_release);
  xSemaphoreGive(gSensorBroadcasterLifecycleMutex);

  DEBUG_ESPNOWF("[REMOTE_SENSORS] System initialized");
  return true;
}

// ==========================
// Helper Functions
// ==========================

// The names come from the one table (System_ESPNow_SensorCore.h).
const char* sensorTypeToString(RemoteSensorType type) {
  return espnowsensor::sensorName((int)type);
}

bool sensorTypeFromString(const char* str, RemoteSensorType& out) {
  uint8_t id = 0;
  if (!espnowsensor::sensorIdFromName(str, id)) return false;
  out = (RemoteSensorType)id;
  return true;
}

RemoteSensorType stringToSensorType(const char* str) {
  RemoteSensorType type = REMOTE_SENSOR_THERMAL;  // Default
  sensorTypeFromString(str, type);
  return type;
}

#if ENABLE_BONDED_MODE
bool* bondStreamSetting(RemoteSensorType type) {
  switch (type) {
    case REMOTE_SENSOR_THERMAL:    return &gSettings.bondStreamThermal;
    case REMOTE_SENSOR_TOF:        return &gSettings.bondStreamTof;
    case REMOTE_SENSOR_IMU:        return &gSettings.bondStreamImu;
    case REMOTE_SENSOR_GPS:        return &gSettings.bondStreamGps;
    case REMOTE_SENSOR_INPUT:      return &gSettings.bondStreamInput;
    case REMOTE_SENSOR_FMRADIO:    return &gSettings.bondStreamFmradio;
    case REMOTE_SENSOR_RTC:        return &gSettings.bondStreamRtc;
    case REMOTE_SENSOR_PRESENCE:   return &gSettings.bondStreamPresence;
    case REMOTE_SENSOR_CAMERA:
    case REMOTE_SENSOR_MICROPHONE:
    case REMOTE_SENSOR_APDS:
    case REMOTE_SENSOR_MAX:        break;
  }
  return nullptr;
}
#endif  // ENABLE_BONDED_MODE

// ---- What the receive path asks of the forwarding code (the host suite
// espnow_sensor_ingest has its own versions) ----
// Whose is a reading part from `src` (System_ESPNow_ForwardRx.cpp).
static ForwardResolve sensorWhose(const uint8_t* src, const char* srcName, bool fromBond, bool bitSet, uint8_t fwdLink,
                                  uint8_t sensorType, ForwardOrigin* out) {
  return espnowForwardResolve(src, srcName, fromBond, /*hasBit=*/true, bitSet, fwdLink, sensorType, out);
}
static void sensorResumeTaken(const uint8_t via[6], uint8_t sensorType) { espnowForwardResumeTaken(via, sensorType); }
static bool sensorLinkActive(const uint8_t via[6], const uint8_t origin[6]) {
  return espnowForwardLinkActive(via, origin);
}
// A bond master hands a reading of its partner it kept to its forwarder, and
// counts one it could not keep.
static void sensorForwardReading(const uint8_t src[6], const espnowsensor::ReadingHeader& h, const uint8_t* text,
                                 size_t len) {
  bondForwardReading(src, h, text, len);
}
static void sensorNotKept(uint8_t sensorType) { bondForwardNotKept(sensorType); }
// Whose is a sensor status frame from `src`: the sender's own, or its bond
// partner's that it forwards (the link byte alone marks it).
static ForwardResolve sensorStatusWhose(const uint8_t* src, const char* srcName, bool fromBond, uint8_t fwdLink,
                                        ForwardOrigin* out) {
  return espnowForwardResolve(src, srcName, fromBond, /*hasBit=*/false, /*bitSet=*/false, fwdLink,
                              espnowfwd::kStreamIdStatus, out);
}
// A bond master hands its partner's status it kept to its forwarder.
static void sensorForwardStatus(const uint8_t src[6], const espnowfwd::StatusFrame& f) { bondForwardStatus(src, f); }
static void sensorViaName(const uint8_t mac[6], char* out, size_t cap) {
  String name = getEspNowDeviceName(mac);
  if (name.length() == 0) name = macToDisplayStr(mac);
  strlcpy(out, name.c_str(), cap);
}

// SENSOR_RX_HOST_BEGIN
// ---- The table (every function named ...Locked runs under gRemoteSensorMux) ----

// A slot whose sensor was heard from within the presence time.
static bool slotPresent(const RemoteSensorData& e, uint32_t nowMs) {
  return e.connected && (uint32_t)(nowMs - e.lastSeen) <= REMOTE_SENSOR_PRESENCE_TTL_MS;
}

// A slot whose reading arrived within the fresh time.
static bool slotFresh(const RemoteSensorData& e, uint32_t nowMs) {
  return e.valid && !e.aged && (uint32_t)(nowMs - e.lastUpdate) <= REMOTE_SENSOR_TTL_MS;
}

static const uint8_t kSensorNoVia[6] = { 0, 0, 0, 0, 0, 0 };

// The record is that of (mac, via); via nullptr: the origin's own record.
static bool sensorRecordIs(const SensorPeerRecord& r, const uint8_t* mac, const uint8_t* via) {
  return memcmp(r.mac, mac, 6) == 0 && memcmp(r.via, via ? via : kSensorNoVia, 6) == 0;
}

static SensorPeerRecord* findPeerRecordLocked(const uint8_t* mac, const uint8_t* via) {
  if (!gSensorPeers || !mac) return nullptr;
  for (int i = 0; i < REMOTE_SENSOR_PEERS_MAX; i++) {
    if (gSensorPeers[i].inUse && sensorRecordIs(gSensorPeers[i], mac, via)) return &gSensorPeers[i];
  }
  return nullptr;
}

// The slot of one sensor of one sender, or nullptr when the sender has none.
static RemoteSensorData* findCacheEntryLocked(const uint8_t* deviceMac, const uint8_t* via, RemoteSensorType sensorType,
                                              const SensorPeerRecord** peerOut = nullptr) {
  if (sensorType < 0 || sensorType >= REMOTE_SENSOR_MAX) return nullptr;
  SensorPeerRecord* peer = findPeerRecordLocked(deviceMac, via);
  if (!peer || !peer->slots) return nullptr;
  if (peerOut) *peerOut = peer;
  return &peer->slots[sensorType];
}

// The record of a sender (origin, via), made when there is none: a free one,
// else one that may be given up (espnowfwd::recordReusable: an own record
// whose device is no longer paired, a forwarded one whose via is no longer
// paired or no longer forwards for it). Its slots stay with the record and
// are cleared for the new sender. nullptr when none may be given up. Called
// without the lock (the pairing lookup must not run under it); espnow_task
// only.
static SensorPeerRecord* sensorPeerRecordFor(const ForwardOrigin& origin) {
  if (!gSensorPeers) return nullptr;
  const uint8_t* mac = origin.mac;
  const uint8_t* via = origin.link == espnowfwd::kLinkForwarded ? origin.via : kSensorNoVia;
  SensorPeerRecord* freeRecord = nullptr;
  for (int i = 0; i < REMOTE_SENSOR_PEERS_MAX; i++) {
    if (gSensorPeers[i].inUse && sensorRecordIs(gSensorPeers[i], mac, via)) {
      // The same origin may come over the mesh and over the bond link.
      if (gSensorPeers[i].link != origin.link) {
        taskENTER_CRITICAL(&gRemoteSensorMux);
        gSensorPeers[i].link = origin.link;
        taskEXIT_CRITICAL(&gRemoteSensorMux);
      }
      return &gSensorPeers[i];
    }
    if (!gSensorPeers[i].inUse && !freeRecord) freeRecord = &gSensorPeers[i];
  }
  if (!freeRecord) {
    for (int i = 0; i < REMOTE_SENSOR_PEERS_MAX; i++) {
      const SensorPeerRecord& r = gSensorPeers[i];
      const bool forwarded = r.link == espnowfwd::kLinkForwarded;
      if (espnowfwd::recordReusable(r.link, forwarded ? false : peerIdentityFindByMac(r.mac) != nullptr,
                                    forwarded ? peerIdentityFindByMac(r.via) != nullptr : false,
                                    forwarded ? sensorLinkActive(r.via, r.mac) : false)) {
        freeRecord = &gSensorPeers[i];
        break;
      }
    }
  }
  if (!freeRecord) return nullptr;
  // Taken out of the table first: every reader skips a record that is not in
  // use, so it is cleared (about 6.5 KB of PSRAM) without the lock, and put
  // back in under it.
  taskENTER_CRITICAL(&gRemoteSensorMux);
  freeRecord->inUse = false;
  taskEXIT_CRITICAL(&gRemoteSensorMux);
  RemoteSensorData* slots = freeRecord->slots;
  memset(freeRecord, 0, sizeof(*freeRecord));
  if (slots) memset(slots, 0, (size_t)REMOTE_SENSOR_MAX * sizeof(RemoteSensorData));
  freeRecord->slots = slots;
  memcpy(freeRecord->mac, mac, 6);
  memcpy(freeRecord->via, via, 6);
  freeRecord->link = origin.link;
  taskENTER_CRITICAL(&gRemoteSensorMux);
  freeRecord->inUse = true;
  taskEXIT_CRITICAL(&gRemoteSensorMux);
  return freeRecord;
}

// The sender's slots, made when it has none (about 5.5 KB of PSRAM, once per
// sender). False when there is no memory for them. espnow_task only, without
// the lock.
static bool sensorPeerSlotsEnsure(SensorPeerRecord* peer) {
  if (!peer) return false;
  if (peer->slots) return true;
  const size_t bytes = (size_t)REMOTE_SENSOR_MAX * sizeof(RemoteSensorData);
  RemoteSensorData* slots = (RemoteSensorData*)ps_alloc(bytes, AllocPref::PreferPsram, "espnow.remoteSensors");
  if (!slots) return false;
  memset(slots, 0, bytes);
  taskENTER_CRITICAL(&gRemoteSensorMux);
  peer->slots = slots;
  taskEXIT_CRITICAL(&gRemoteSensorMux);
  return true;
}

static void fillRemoteSensorInfo(const SensorPeerRecord& peer, RemoteSensorType sensorType, uint32_t nowMs,
                                 RemoteSensorInfo* out) {
  const RemoteSensorData& e = peer.slots[sensorType];
  memcpy(out->deviceMac, peer.mac, 6);
  memcpy(out->viaMac, peer.via, 6);
  out->link = peer.link;
  memcpy(out->deviceName, peer.name, sizeof(out->deviceName));
  out->sensorType = sensorType;
  out->enabled = e.enabled;
  out->fresh = slotFresh(e, nowMs);
  out->jsonLength = e.jsonLength;
  out->lastUpdate = e.lastUpdate;
  out->lastSeen = e.lastSeen;
  out->arrivalEpochMs = e.arrivalEpochMs;
  out->arrivalClock = e.arrivalClock;
  out->counter = e.counter;
}

// Slot index = sender record * REMOTE_SENSOR_MAX + sensor type.
int remoteSensorSlotCount() { return gSensorPeers ? REMOTE_SENSOR_SLOTS_MAX : 0; }

bool remoteSensorInfoAt(int index, RemoteSensorInfo* out) {
  if (!out || index < 0 || index >= REMOTE_SENSOR_SLOTS_MAX) return false;
  const uint32_t nowMs = (uint32_t)millis();
  const RemoteSensorType type = (RemoteSensorType)(index % REMOTE_SENSOR_MAX);
  bool present = false;
  taskENTER_CRITICAL(&gRemoteSensorMux);
  if (gSensorPeers) {
    const SensorPeerRecord& peer = gSensorPeers[index / REMOTE_SENSOR_MAX];
    if (peer.inUse && peer.slots && slotPresent(peer.slots[type], nowMs)) {
      fillRemoteSensorInfo(peer, type, nowMs, out);
      present = true;
    }
  }
  taskEXIT_CRITICAL(&gRemoteSensorMux);
  return present;
}

bool remoteSensorInfoFor(const uint8_t* deviceMac, const uint8_t* via, RemoteSensorType sensorType,
                         RemoteSensorInfo* out) {
  if (!out || !deviceMac) return false;
  const uint32_t nowMs = (uint32_t)millis();
  bool present = false;
  taskENTER_CRITICAL(&gRemoteSensorMux);
  const SensorPeerRecord* peer = nullptr;
  const RemoteSensorData* e = findCacheEntryLocked(deviceMac, via, sensorType, &peer);
  if (e && slotPresent(*e, nowMs)) {
    fillRemoteSensorInfo(*peer, sensorType, nowMs, out);
    present = true;
  }
  taskEXIT_CRITICAL(&gRemoteSensorMux);
  return present;
}

size_t remoteSensorCopyText(const uint8_t* deviceMac, const uint8_t* via, RemoteSensorType sensorType, char* out,
                            size_t outSize) {
  if (!out || outSize == 0) return 0;
  out[0] = '\0';
  if (!deviceMac) return 0;
  size_t n = 0;
  taskENTER_CRITICAL(&gRemoteSensorMux);
  const RemoteSensorData* e = findCacheEntryLocked(deviceMac, via, sensorType);
  if (e && e->valid && e->jsonLength > 0 && (size_t)e->jsonLength < outSize) {
    n = e->jsonLength;
    memcpy(out, e->jsonData, n);
    out[n] = '\0';
  }
  taskEXIT_CRITICAL(&gRemoteSensorMux);
  return n;
}

// ---- Receive ----

static void remoteSensorRefuse(uint32_t RemoteSensorRefusals::*counter) {
  taskENTER_CRITICAL(&gRemoteSensorMux);
  (gRemoteSensorRefusals.*counter)++;
  taskEXIT_CRITICAL(&gRemoteSensorMux);
}

// "porch", or "porch via glasses" for a forwarded sender.
static void sensorRecordWords(const ForwardOrigin& origin, char* out, size_t cap) {
  strlcpy(out, origin.name[0] ? origin.name : "?", cap);
  if (origin.link != espnowfwd::kLinkForwarded) return;
  char via[32];
  sensorViaName(origin.via, via, sizeof(via));
  strlcat(out, " via ", cap);
  strlcat(out, via, cap);
}

// Nothing more can be kept: sixteen paired senders already send, or there is
// no memory. Counted, and said once a minute.
static void remoteSensorRefuseFull(uint32_t RemoteSensorRefusals::*counter, const char* senderName, uint32_t nowMs) {
  remoteSensorRefuse(counter);
  uint32_t held = 0;
  if (!warnLimiterTake(gSensorFullWarn, nowMs, 60000, &held)) return;
  if (counter == &RemoteSensorRefusals::noMemory) {
    BROADCAST_PRINTF("[ESP-NOW] Readings from %s refused: this device cannot keep readings, it is out of memory"
                     " (%lu more in the last minute)", senderName ? senderName : "?", (unsigned long)held);
  } else {
    BROADCAST_PRINTF("[ESP-NOW] Readings from %s refused: this device already keeps the readings of %d paired"
                     " devices (%lu more in the last minute)",
                     senderName ? senderName : "?", REMOTE_SENSOR_PEERS_MAX, (unsigned long)held);
  }
}

// The one place a complete reading is stored: the counter rule, then the
// slot, in one step under the lock. A reading the counter rule drops (a
// repeat, or one older than the one already kept) changes neither the stored
// text nor its arrival stamp. Returns true when the reading was stored.
// (A later reading log, such as a file on the SD card, is fed from here, after
// the lock is released: `verdict` and the stamp are what it needs.)
// `resume`: the first reading of this sensor since a bond master's link
// began (espnowfwd::attribute): judged as a stream start, so a pause of the
// forwarding is not counted as loss.
static bool remoteSensorCommit(SensorPeerRecord* peer, const char* senderName,
                               const espnowsensor::ReadingHeader& h, const uint8_t* text, size_t textLen,
                               bool resume, uint32_t nowMs, espnowsensor::CounterVerdict* verdictOut) {
  const RemoteSensorType type = (RemoteSensorType)h.sensorType;
  // The receiver stamps the reading with its own clock; a time the sender
  // carries is not trusted. UTC, and 0 while this device's clock is not set.
  const bool clockSet = Clock::isSynced();
  const int64_t arrivalEpochMs = clockSet ? Clock::epochMillis() : 0;
  const uint8_t arrivalClock = (uint8_t)Clock::syncSource();
  bool stored = false;
  espnowsensor::CounterVerdict verdict = espnowsensor::CounterVerdict::Repeat;
  taskENTER_CRITICAL(&gRemoteSensorMux);
  RemoteSensorData* entry = &peer->slots[type];
  strlcpy(peer->name, senderName ? senderName : "", sizeof(peer->name));
  verdict = espnowsensor::counterJudge(peer->track[type], peer->counts[type], h.bootTag, h.counter,
                                       resume || (h.flags & espnowsensor::kReadingFlagStreamStart) != 0, nowMs);
  entry->connected = true;
  entry->lastSeen = nowMs;
  entry->enabled = true;   // a reading means the sensor is streaming
  if (espnowsensor::counterVerdictKeeps(verdict)) {
    memcpy(entry->jsonData, text, textLen);
    entry->jsonData[textLen] = '\0';
    entry->jsonLength = (uint16_t)textLen;
    entry->lastUpdate = nowMs;
    entry->arrivalEpochMs = arrivalEpochMs;
    entry->arrivalClock = arrivalClock;
    entry->counter = h.counter;
    entry->valid = true;
    entry->aged = false;
    stored = true;
  }
  taskEXIT_CRITICAL(&gRemoteSensorMux);
  if (verdictOut) *verdictOut = verdict;
  return stored;
}

void remoteSensorIngestPart(const uint8_t* senderMac, const char* senderName,
                            const uint8_t* payload, size_t payloadLen, bool fromBond) {
  const uint32_t nowMs = (uint32_t)millis();
  espnowsensor::ReadingHeader h = {};
  if (!senderMac || !espnowsensor::parseReadingHeader(payload, payloadLen, h)) {
    remoteSensorRefuse(&RemoteSensorRefusals::malformed);
    return;
  }
  // A sensor type this firmware does not know indexes nothing.
  if (h.sensorType >= REMOTE_SENSOR_MAX) {
    remoteSensorRefuse(&RemoteSensorRefusals::badType);
    return;
  }
  // Whose reading it is: the sender's own, or its bond partner's that it
  // forwards (refused unless a link of the sender names it). A forwarded
  // mark on the bond link, or a bit and byte that disagree, is malformed.
  ForwardOrigin origin;
  switch (sensorWhose(senderMac, senderName, fromBond, (h.flags & espnowsensor::kReadingFlagForwarded) != 0,
                      h.fwdLink, h.sensorType, &origin)) {
    case ForwardResolve::Own:
    case ForwardResolve::Forwarded:
      break;
    case ForwardResolve::Refused:
      remoteSensorRefuse(&RemoteSensorRefusals::forwardRefused);
      return;
    case ForwardResolve::Malformed:
      remoteSensorRefuse(&RemoteSensorRefusals::malformed);
      return;
  }
  // A bond master keeps its partner's readings whatever its mesh role; a mesh
  // frame is kept by the roles that receive (master and backup master).
  if (!fromBond && !espnowsensor::roleReceives(gSettings.meshRole)) {
    remoteSensorRefuse(&RemoteSensorRefusals::notReceiver);
    uint32_t held = 0;
    if (warnLimiterTake(gSensorRoleRefusalWarn, nowMs, 60000, &held)) {
      BROADCAST_PRINTF("[ESP-NOW] Readings from %s refused: this device is not a mesh master or backup master"
                       " (%lu more in the last minute)",
                       senderName ? senderName : "?", (unsigned long)held);
    }
    return;
  }
  // Only now, on a device that keeps readings, is the table made.
  if (!remoteSensorTableEnsure()) {
    if (fromBond) sensorNotKept(h.sensorType);
    remoteSensorRefuseFull(&RemoteSensorRefusals::noMemory, senderName, nowMs);
    return;
  }
  SensorPeerRecord* peer = sensorPeerRecordFor(origin);
  if (!peer) {
    if (fromBond) sensorNotKept(h.sensorType);
    char who[72];
    sensorRecordWords(origin, who, sizeof(who));
    remoteSensorRefuseFull(&RemoteSensorRefusals::noRecord, who, nowMs);
    return;
  }
  const uint8_t* text = nullptr;
  size_t textLen = 0;
  const espnowsensor::PartResult part =
      espnowsensor::partAccept(peer->assembly, peer->assemblyBuf, sizeof(peer->assemblyBuf), h,
                               payload + espnowsensor::kReadingHeaderLen,
                               payloadLen - espnowsensor::kReadingHeaderLen, nowMs, &text, &textLen);
  // The text is put into this device's own JSON replies as it came, so it
  // must be one JSON object and nothing else.
  const bool badText = part.verdict == espnowsensor::PartVerdict::Complete &&
                       !espnowsensor::readingTextIsObject(text, textLen);
  if (part.incomplete || badText || part.verdict == espnowsensor::PartVerdict::Malformed) {
    taskENTER_CRITICAL(&gRemoteSensorMux);
    peer->incomplete += part.incomplete;
    if (badText || part.verdict == espnowsensor::PartVerdict::Malformed) peer->malformed++;
    taskEXIT_CRITICAL(&gRemoteSensorMux);
  }
  if (part.verdict != espnowsensor::PartVerdict::Complete || badText) return;
  // The sender's slots are made by its first reading that is whole and well formed.
  if (!sensorPeerSlotsEnsure(peer)) {
    if (fromBond) sensorNotKept(h.sensorType);
    remoteSensorRefuseFull(&RemoteSensorRefusals::noMemory, senderName, nowMs);
    return;
  }
  espnowsensor::CounterVerdict verdict;
  // The resume of a forwarded stream is taken by the reading the counter rule
  // judges (the last part of a reading), and only once that rule ran.
  const bool stored = remoteSensorCommit(peer, origin.name, h, text, textLen, origin.resumePending, nowMs, &verdict);
  if (origin.resumePending) sensorResumeTaken(origin.via, h.sensorType);
  // A bond master hands its partner's reading on while `text` is valid; only a
  // reading it kept (a repeat or a late one is not forwarded).
  if (fromBond && stored) sensorForwardReading(senderMac, h, text, textLen);
  DEBUG_ESPNOW_MESHF("[SENSOR_RX] %s %s counter=%u %s (%u bytes)%s", senderName ? senderName : "?",
                     sensorTypeToString((RemoteSensorType)h.sensorType), (unsigned)h.counter,
                     espnowsensor::counterVerdictName(verdict), (unsigned)textLen, stored ? "" : " not stored");
}

static void sensorStatusIngest(const uint8_t* senderMac, const char* senderName, const uint8_t* payload,
                               size_t payloadLen, bool fromBond) {
  espnowfwd::StatusFrame frame = {};
  if (!senderMac || !espnowfwd::parseStatusFrame(payload, payloadLen, frame)) {
    remoteSensorRefuse(&RemoteSensorRefusals::malformed);
    return;
  }
  // Whose status it is: the sender's own, or its bond partner's that it
  // forwards (refused unless a link of the sender names it). A link byte on
  // the bond link is malformed.
  ForwardOrigin origin;
  switch (sensorStatusWhose(senderMac, senderName, fromBond, frame.fwdLink, &origin)) {
    case ForwardResolve::Own:
    case ForwardResolve::Forwarded:
      break;
    case ForwardResolve::Refused:
      remoteSensorRefuse(&RemoteSensorRefusals::forwardRefused);
      return;
    case ForwardResolve::Malformed:
      remoteSensorRefuse(&RemoteSensorRefusals::malformed);
      return;
  }
  // The status frame goes to the device that receives the sender's readings;
  // a bond master keeps its partner's whatever its mesh role.
  if (!fromBond && !espnowsensor::roleReceives(gSettings.meshRole)) {
    remoteSensorRefuse(&RemoteSensorRefusals::notReceiver);
    return;
  }
  // It makes the table and the sender's record as a reading does, so the
  // receiver lists a device's sensors (present, streaming or not) before any
  // reading arrives. A full table or no memory is refused, counted and said
  // as for a reading. It never touches a reading, its freshness or its counts.
  const uint32_t nowMs = (uint32_t)millis();
  if (!remoteSensorTableEnsure()) {
    remoteSensorRefuseFull(&RemoteSensorRefusals::noMemory, senderName, nowMs);
    return;
  }
  SensorPeerRecord* peer = sensorPeerRecordFor(origin);
  if (!peer) {
    char who[72];
    sensorRecordWords(origin, who, sizeof(who));
    remoteSensorRefuseFull(&RemoteSensorRefusals::noRecord, who, nowMs);
    return;
  }
  if (!sensorPeerSlotsEnsure(peer)) {
    remoteSensorRefuseFull(&RemoteSensorRefusals::noMemory, senderName, nowMs);
    return;
  }
  struct {
    uint32_t presentMask;
    uint32_t streamingMask;
  } status = { frame.presentMask, frame.streamingMask };
  // Bits of types this firmware does not know are ignored, so nothing is
  // indexed with a number from the frame.
  taskENTER_CRITICAL(&gRemoteSensorMux);
  strlcpy(peer->name, origin.name, sizeof(peer->name));
  for (int i = 0; i < REMOTE_SENSOR_MAX; i++) {
    const uint32_t bit = espnowsensor::sensorBit((uint8_t)i);
    RemoteSensorData* entry = &peer->slots[i];
    if (status.presentMask & bit) {
      // Presence and the streaming flag only. Whether data is fresh comes
      // from the reading's own arrival time.
      entry->connected = true;
      entry->enabled = (status.streamingMask & bit) != 0;
      entry->lastSeen = nowMs;
    } else if (entry->connected) {
      entry->enabled = false;  // no longer there; it stops being listed after the presence time
    }
  }
  taskEXIT_CRITICAL(&gRemoteSensorMux);
  // A bond master hands its partner's status on (to the master it forwards
  // readings to, with the sensors it forwards).
  if (fromBond) sensorForwardStatus(senderMac, frame);
}

void remoteSensorIngestStatus(const uint8_t* senderMac, const char* senderName,
                              const uint8_t* payload, size_t payloadLen) {
  sensorStatusIngest(senderMac, senderName, payload, payloadLen, /*fromBond=*/false);
}

void remoteSensorIngestBondStatus(const uint8_t* senderMac, const char* senderName,
                                  const uint8_t* payload, size_t payloadLen) {
  sensorStatusIngest(senderMac, senderName, payload, payloadLen, /*fromBond=*/true);
}

// The slots are judged by 32-bit uptime differences, which come round again
// after 49.7 days: a sensor last heard from exactly that long ago would be
// listed again, and its old reading shown as fresh. This step (espnow_task,
// every 10 s) marks what has aged while the differences are still true: a
// sensor not heard from within the presence time is no longer listed, and a
// reading older than the fresh time is no longer fresh, until the next frame.
// One sender per hold.
static void remoteSensorAgeTick(uint32_t nowMs) {
  static uint32_t sLastMs = 0;
  if (!gSensorPeers || (uint32_t)(nowMs - sLastMs) < 10000) return;
  sLastMs = nowMs;
  for (int i = 0; i < REMOTE_SENSOR_PEERS_MAX; i++) {
    taskENTER_CRITICAL(&gRemoteSensorMux);
    SensorPeerRecord& peer = gSensorPeers[i];
    for (int k = 0; peer.inUse && peer.slots && k < REMOTE_SENSOR_MAX; k++) {
      RemoteSensorData& e = peer.slots[k];
      if (e.connected && (uint32_t)(nowMs - e.lastSeen) > REMOTE_SENSOR_PRESENCE_TTL_MS) {
        e.connected = false;
        e.enabled = false;
      }
      if (e.valid && !e.aged && (uint32_t)(nowMs - e.lastUpdate) > REMOTE_SENSOR_TTL_MS) e.aged = true;
    }
    taskEXIT_CRITICAL(&gRemoteSensorMux);
  }
}
// SENSOR_RX_HOST_END


// ==========================
// Worker → Master Broadcasting
// ==========================

bool sensorTypeInThisBuild(RemoteSensorType sensorType) {
  return sensorType >= 0 && sensorType < REMOTE_SENSOR_MAX && gSensorSpecs[sensorType].builder != nullptr;
}

// The masters named on this device, as addresses: every paired device whose
// key is the master or the backup master fingerprint set here. espnow_task
// only (the pairing table is read); the result is kept for the other tasks.
void sensorReceiverTargetsRefresh() {
  SensorReceiverTargets found = {};
  found.masterNamed = gSettings.espnowMasterFingerprint.length() == 64;
  found.backupNamed = gSettings.espnowBackupMasterFingerprint.length() == 64;
  if (found.masterNamed || found.backupNamed) {
    const uint8_t slots = peerIdentitySlotCount();
    for (uint8_t i = 0; i < slots; i++) {
      const PeerIdentity* pid = peerIdentityAt(i);
      if (!pid) continue;
      const int kind = espnowNamedMasterKind(pid->mac);
      if (kind == 1 && !found.havePrimary) {
        found.havePrimary = true;
        memcpy(found.primary, pid->mac, 6);
      } else if (kind == 2 && !found.haveBackup) {
        found.haveBackup = true;
        memcpy(found.backup, pid->mac, 6);
      }
    }
  }
  taskENTER_CRITICAL(&gSensorBcastTaskMux);
  gSensorTargets = found;
  taskEXIT_CRITICAL(&gSensorBcastTaskMux);
}

void sensorReceiverTargets(SensorReceiverTargets* out) {
  if (!out) return;
  taskENTER_CRITICAL(&gSensorBcastTaskMux);
  *out = gSensorTargets;
  taskEXIT_CRITICAL(&gSensorBcastTaskMux);
}

// The inputs of the target rule that this file keeps: the controller and the
// lease of `sensorType`, whether it was switched on by hand, and the master
// named here. A controller counts for a sensor only while that sensor's own
// lease runs (a stream switched on by hand beside a leased one goes to the
// named master). With sensorType < 0 (the status frame): the controller while
// one is set. Under gSensorBcastTaskMux.
static void sensorTargetInputsLocked(int sensorType, espnowsensor::TargetInputs& in) {
  bool haveController = false;
  memcpy(in.controller, gSensorControllerMac, 6);
  for (int b = 0; b < 6; b++) {
    if (in.controller[b] != 0) { haveController = true; break; }
  }
  if (sensorType >= 0 && sensorType < REMOTE_SENSOR_MAX) {
    in.leaseRunning = haveController && gSensorLeaseExpiresAt[sensorType] != 0;
    in.byHand = gSensorByHand[sensorType];
  } else {
    in.leaseRunning = haveController;
    in.byHand = true;
  }
  in.masterNamed = gSensorTargets.masterNamed;
  in.havePrimary = gSensorTargets.havePrimary;
  memcpy(in.primary, gSensorTargets.primary, 6);
}

// This device is a bond worker (bond mode on, role worker).
static bool sensorIsBondWorker() {
#if ENABLE_BONDED_MODE
  return gSettings.bondModeEnabled && isBondWorker();
#else
  return false;
#endif
}

// Where a reading of `sensorType` goes now (espnowsensor::readingTarget has
// the rule). With sensorType < 0: where the status frame goes, which is the
// controller while one is set, else the named master.
static espnowsensor::TargetChoice sensorReadingTarget(int sensorType) {
  espnowsensor::TargetInputs in = {};
#if ENABLE_BONDED_MODE
  in.bondWorker = gSettings.bondModeEnabled && isBondWorker();
  if (in.bondWorker) {
    in.bondOnline = isBondModeOnline() && parseMacAddress(gSettings.bondPeerMac, in.bondPartner);
  }
#endif
  in.broadcastOn = gSensorBroadcastEnabled;
  in.meshOn = meshEnabled();
  in.meshRole = gSettings.meshRole;
  taskENTER_CRITICAL(&gSensorBcastTaskMux);
  sensorTargetInputsLocked(sensorType, in);
  taskEXIT_CRITICAL(&gSensorBcastTaskMux);
  return espnowsensor::readingTarget(in);
}

// The controller is forgotten when the last lease ends, so a stream that was
// switched on by hand goes to the named master again and the status frame
// with it. Under gSensorBcastTaskMux; "no lease left" is looked at here, under
// the same lock the subscription apply stamps a lease with. True when it was
// cleared: *previous is then the controller that was set.
static bool sensorControllerClearIfNoLeaseLocked(uint8_t previous[6]) {
  for (int i = 0; i < REMOTE_SENSOR_MAX; i++) {
    if (gSensorLeaseExpiresAt[i] != 0) return false;
  }
  bool had = false;
  for (int b = 0; b < 6; b++) {
    if (gSensorControllerMac[b] != 0) { had = true; break; }
  }
  if (!had) return false;
  if (previous) memcpy(previous, gSensorControllerMac, 6);
  memset(gSensorControllerMac, 0, sizeof(gSensorControllerMac));
  return true;
}

// Tells the device that receives this device's readings which sensors are
// here and which are streaming (SENSOR_STATUS, one frame with two masks),
// so it can list a sensor that is present but not streaming. Sent when a
// stream starts or stops and every 5 s by the broadcaster. Nothing is sent
// to a bond partner (it has the bond status frame), without a receiver, or
// before the session with it has settled. `alsoTo`: a controller whose last
// lease just ended is told once more, so it sees the stream stop.
static void announceSensorStatus(const uint8_t* alsoTo = nullptr) {
  // A bond worker tells nothing to a mesh master, not even a controller whose
  // lease ended: it tells its bond master (BOND_SENSOR_STATUS), which may
  // forward it to the master it forwards readings to.
  const bool bondWorker = sensorIsBondWorker();
  if (!espnowfwd::meshDataAllowed(bondWorker)) alsoTo = nullptr;
  const espnowsensor::TargetChoice target = sensorReadingTarget(-1);
  const bool haveTarget = target.why == espnowsensor::DropReason::None && target.viaBond == bondWorker;
  if (!haveTarget && !alsoTo) return;
  struct Item { bool connected; RemoteSensorType type; };
  // The connected globals are real driver state, or `false` from the disabled
  // branch of the sensor's header, so naming them without a condition is safe.
  const Item items[] = {
    { gThermalConnected,  REMOTE_SENSOR_THERMAL },
    { gTofConnected,      REMOTE_SENSOR_TOF },
    { gImuConnected,      REMOTE_SENSOR_IMU },
    { gGpsConnected,      REMOTE_SENSOR_GPS },
    { gInputConnected,    REMOTE_SENSOR_INPUT },
    { gFmRadioConnected,  REMOTE_SENSOR_FMRADIO },
    { gRtcConnected,      REMOTE_SENSOR_RTC },
    { gPresenceConnected, REMOTE_SENSOR_PRESENCE },
  };
  uint32_t presentMask = 0;
  uint32_t streamingMask = 0;
  for (const auto& it : items) {
    if (!it.connected) continue;
    presentMask |= espnowsensor::sensorBit((uint8_t)it.type);
    // The streaming state, not the sensor-on state: "is it streaming to me"
    // is the only axis the receiver can observe.
    if (gSensorStreamingEnabled[it.type]) streamingMask |= espnowsensor::sensorBit((uint8_t)it.type);
  }
  const uint32_t nowMs = (uint32_t)millis();
  if (haveTarget && target.viaBond) {
    // Over the bond link only (the bond send rules wait for its session).
    (void)v4_send_bond_sensor_status(target.mac, presentMask, streamingMask, sensorBootTag());
  } else if (haveTarget && espnowSessionSettled(target.mac, nowMs)) {
    (void)v4_send_sensor_status(target.mac, presentMask, streamingMask, sensorBootTag());
  }
  if (alsoTo && !(haveTarget && memcmp(alsoTo, target.mac, 6) == 0) && espnowSessionSettled(alsoTo, nowMs)) {
    (void)v4_send_sensor_status(alsoTo, presentMask, streamingMask, sensorBootTag());
  }
}

// A bond worker tells its bond master which sensors it has once per bond
// session (and on every change after that, announceSensorStatus), so the bond
// master, and the master it forwards readings to, list them before any
// reading. espnow_task (sensorLinkTick).
static void sensorBondStatusTick(uint32_t nowMs) {
  static uint16_t sStatusSessionId = 0;
  if (!sensorIsBondWorker()) return;
  const espnowsensor::TargetChoice target = sensorReadingTarget(-1);
  const uint16_t sid =
      target.why == espnowsensor::DropReason::None && target.viaBond ? espnowSessionIdOf(target.mac) : 0;
  if (sid == 0 || sid == sStatusSessionId || !espnowSessionSettled(target.mac, nowMs)) return;
  sStatusSessionId = sid;
  announceSensorStatus();
}

void sensorPresenceChanged(RemoteSensorType sensorType, bool present) {
  DEBUG_ESPNOW_METADATAF("[SENSOR_STATUS_TX] %s %s", sensorTypeToString(sensorType), present ? "started" : "stopped");
  announceSensorStatus();
}

// Forward declarations
// Callers hold gSensorBroadcasterLifecycleMutex across these transactions.
static bool startSensorBroadcasterLocked();
static bool stopSensorBroadcasterLocked(uint32_t timeoutMs);

void startSensorDataStreaming(RemoteSensorType sensorType) {
  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] startSensorDataStreaming() called with type=%d (%s)", sensorType, sensorTypeToString(sensorType));
  
  if (sensorType >= REMOTE_SENSOR_MAX) {
    DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] ERROR: Invalid sensor type %d (max=%d)", sensorType, REMOTE_SENSOR_MAX);
    return;
  }
  // Serialize every flag update and the full task create/publish transaction
  // against closeespnow's stop/join/free transaction.
  if (!gSensorBroadcasterLifecycleMutex ||
      xSemaphoreTake(gSensorBroadcasterLifecycleMutex, portMAX_DELAY) != pdTRUE) {
    return;
  }
  if (gSensorStreamingShutdown.load(std::memory_order_acquire)) {
    xSemaphoreGive(gSensorBroadcasterLifecycleMutex);
    DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] start rejected during ESP-NOW shutdown");
    return;
  }

#if ENABLE_BONDED_MODE
  // Bond master: send STREAM_CTRL to worker — master doesn't have the sensors locally
  if (gSettings.bondModeEnabled && isBondMaster()) {
    DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] Bond master: sending STREAM_CTRL %s ON to worker", sensorTypeToString(sensorType));
    bool sent = sendBondStreamCtrl(sensorType, true);
    // The user's stream now: the end of a mesh master's lease leaves it on.
    bondForwardStreamByHand((uint8_t)sensorType, true);
    if (sent) {
      // Update local flag so UI reflects the requested streaming state
      gSensorStreamingEnabled[sensorType] = true;
      BROADCAST_PRINTF("[ESP-NOW] Requested worker to stream %s sensor data", sensorTypeToString(sensorType));
    } else {
      BROADCAST_PRINTF("[ESP-NOW] Failed to send stream request to worker (peer offline?)");
    }
    xSemaphoreGive(gSensorBroadcasterLifecycleMutex);
    return;
  }
#endif
  
  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] Setting streaming flag for %s to TRUE", sensorTypeToString(sensorType));
  
  // Ensure master broadcast flag is enabled so sensor data reaches the cache.
  // We already own the lifecycle mutex; write directly rather than re-entering
  // the public setter.
  if (!gSensorBroadcastEnabled) gSensorBroadcastEnabled = true;
  
  // Publish the flag before task creation so a newly scheduled broadcaster
  // cannot observe an empty set and self-delete in the start-up window.
  gSensorStreamingEnabled[sensorType] = true;
  // The first reading of this stream says so, so the receiver does not take
  // the readings this sensor sent elsewhere meanwhile as lost.
  // Switched on by hand: it goes to the master named on this device, and the
  // end of a lease does not stop it.
  taskENTER_CRITICAL(&gSensorBcastTaskMux);
  gSensorByHand[sensorType] = true;
  espnowsensor::streamRestart(gSensorTxTarget[sensorType]);
  taskEXIT_CRITICAL(&gSensorBcastTaskMux);
  // Reset lastTxMs before task creation so the first broadcaster lap observes
  // the intended immediate-send state.
  gLastTxMs[sensorType] = 0;

  // Start broadcaster task if not already running
  bool taskPresent;
  taskENTER_CRITICAL(&gSensorBcastTaskMux);
  taskPresent = gSensorBroadcasterTask != nullptr;
  taskEXIT_CRITICAL(&gSensorBcastTaskMux);
  if (!taskPresent) {
    if (!startSensorBroadcasterLocked()) {
      gSensorStreamingEnabled[sensorType] = false;
      taskENTER_CRITICAL(&gSensorBcastTaskMux);
      gSensorByHand[sensorType] = false;
      taskEXIT_CRITICAL(&gSensorBcastTaskMux);
      xSemaphoreGive(gSensorBroadcasterLifecycleMutex);
      BROADCAST_PRINTF("[ESP-NOW] ERROR: Failed to start sensor broadcaster task");
      return;
    }
  }
  xSemaphoreGive(gSensorBroadcasterLifecycleMutex);

  // Tell the receiver right away that this sensor is now streaming, so its
  // dot/cards flip without waiting for the next 5 s announce or first reading.
  announceSensorStatus();

  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] Streaming enabled: %s (flag=%d)",
         sensorTypeToString(sensorType), gSensorStreamingEnabled[sensorType]);
  
  DEBUG_ESPNOWF("[REMOTE_SENSORS] Started streaming for %s",
         sensorTypeToString(sensorType));
  
  BROADCAST_PRINTF("[ESP-NOW] Started streaming %s sensor data", sensorTypeToString(sensorType));
}

void stopSensorDataStreaming(RemoteSensorType sensorType) {
  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] stopSensorDataStreaming() called with type=%d (%s)", sensorType, sensorTypeToString(sensorType));
  
  if (sensorType >= REMOTE_SENSOR_MAX) {
    DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] ERROR: Invalid sensor type %d (max=%d)", sensorType, REMOTE_SENSOR_MAX);
    return;
  }
  
  if (!gSensorBroadcasterLifecycleMutex ||
      xSemaphoreTake(gSensorBroadcasterLifecycleMutex, portMAX_DELAY) != pdTRUE) {
    return;
  }

#if ENABLE_BONDED_MODE
  // Bond master: send STREAM_CTRL OFF to worker. Lifecycle serialization keeps
  // the local UI flag from racing closeespnow's all-flags clear.
  if (gSettings.bondModeEnabled && isBondMaster()) {
    DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] Bond master: sending STREAM_CTRL %s OFF to worker", sensorTypeToString(sensorType));
    sendBondStreamCtrl(sensorType, false);
    // A stop by hand ends a mesh master's lease on it too.
    bondForwardStreamByHand((uint8_t)sensorType, false);
    gSensorStreamingEnabled[sensorType] = false;
    xSemaphoreGive(gSensorBroadcasterLifecycleMutex);
    BROADCAST_PRINTF("[ESP-NOW] Requested worker to stop streaming %s sensor data", sensorTypeToString(sensorType));
    return;
  }
#endif

  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] Setting streaming flag for %s to FALSE", sensorTypeToString(sensorType));
  // A stop by hand ends the stream whoever started it: its lease goes too,
  // and with the last lease the controller.
  uint8_t oldController[6];
  bool controllerCleared;
  taskENTER_CRITICAL(&gSensorBcastTaskMux);
  gSensorStreamingEnabled[sensorType] = false;
  gSensorByHand[sensorType] = false;
  gSensorLeaseExpiresAt[sensorType] = 0;
  gSensorReqIntervalMs[sensorType] = 0;
  controllerCleared = sensorControllerClearIfNoLeaseLocked(oldController);
  taskEXIT_CRITICAL(&gSensorBcastTaskMux);
  bool anyEnabled = false;
  for (int i = 0; i < REMOTE_SENSOR_MAX; i++) {
    if (gSensorStreamingEnabled[i]) {
      anyEnabled = true;
      break;
    }
  }
  xSemaphoreGive(gSensorBroadcasterLifecycleMutex);
  // Tell the receiver at once that this sensor stopped streaming (not at the
  // next 5 s announce): this is what makes the remote dot go red and stay.
  announceSensorStatus(controllerCleared ? oldController : nullptr);

  // If this was the last sensor, the broadcaster observes the cleared flags on
  // its next 50 ms lap and self-deletes. Do not synchronously join it here:
  // subscription updates can run on espnow_task, whose RX drain must not block.
  if (!anyEnabled) {
    DEBUG_ESPNOWF(
           "[SENSOR_BROADCASTER] All sensors disabled; task retirement requested");
  }
  
  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] Streaming disabled: %s (flag=%d)",
         sensorTypeToString(sensorType), gSensorStreamingEnabled[sensorType]);
  
  DEBUG_ESPNOWF("[REMOTE_SENSORS] Stopped streaming for %s",
         sensorTypeToString(sensorType));
  
  BROADCAST_PRINTF("[ESP-NOW] Stopped streaming %s sensor data", sensorTypeToString(sensorType));
}

 bool isSensorDataStreamingEnabled(RemoteSensorType sensorType) {
   if (sensorType >= REMOTE_SENSOR_MAX) return false;
   return gSensorStreamingEnabled[sensorType];
 }

// ==========================
// Secure sensor fetcher — authorization + subscription apply (worker side)
// ==========================

// L4: a configured fingerprint matches only if it is EXACTLY 64 hex chars and
// case-insensitively equals the live 64-hex pubkey. Empty/malformed → never match.
static bool sensorFingerprintMatches(const char* configured, const char* liveHex64) {
  if (!configured || strlen(configured) != 64) return false;
  return strcasecmp(configured, liveHex64) == 0;
}

int espnowNamedMasterKind(const uint8_t mac[6]) {
  const PeerIdentity* id = peerIdentityFindByMac(mac);
  if (!id) return 0;  // not a securely-paired peer
  char liveHex[65];
  espnowIdentityFormatPubHex(id->longTermPub, liveHex, sizeof(liveHex));
  if (sensorFingerprintMatches(gSettings.espnowMasterFingerprint.c_str(), liveHex))       return 1;
  if (sensorFingerprintMatches(gSettings.espnowBackupMasterFingerprint.c_str(), liveHex)) return 2;
  return 0;
}

bool espnowSensorControlAuthorized(const uint8_t mac[6]) {
  return espnowNamedMasterKind(mac) != 0;
}

bool espnowHasNamedMaster() {
  return gSettings.espnowMasterFingerprint.length() == 64 ||
         gSettings.espnowBackupMasterFingerprint.length() == 64;
}

void espnowApplySensorSubscription(uint8_t mode, uint32_t sensorMask,
                                   uint32_t intervalMs, uint32_t leaseMs,
                                   const uint8_t* controllerMac) {
  if (mode > SENSOR_REQ_ONESHOT) {  // reject unknown/out-of-range modes (3..255)
    DEBUG_ESPNOW_STREAMF("[SENSOR_SUB] ignoring unknown mode %u", mode);
    return;
  }
  // The deferred apply still runs on espnow_task. Never block that RX drainer
  // behind closeespnow's task join: shutdown publishes its atomic gate before
  // taking this mutex, and an ordinary short-lived creator conflict is retried
  // on the next super-loop lap using the already-staged request fields.
  if (gSensorStreamingShutdown.load(std::memory_order_acquire)) return;
  if (!gSensorBroadcasterLifecycleMutex ||
      xSemaphoreTake(gSensorBroadcasterLifecycleMutex, 0) != pdTRUE) {
    if (!gSensorStreamingShutdown.load(std::memory_order_acquire) && gEspNow) {
      gEspNow->meshSensorReqPending = true;
    }
    return;
  }
  if (gSensorStreamingShutdown.load(std::memory_order_acquire)) {
    xSemaphoreGive(gSensorBroadcasterLifecycleMutex);
    return;
  }

  // The controller and the lease stamps change together, in one step that the
  // broadcaster's lapse branch cannot cut in two (it takes the same lock).
  uint32_t enabledMask = 0;
  uint8_t oldController[6];
  bool controllerCleared = false;
  taskENTER_CRITICAL(&gSensorBcastTaskMux);
  // Subscribe/oneshot record the controller as the encrypted-reply target.
  if ((mode == SENSOR_REQ_SUBSCRIBE || mode == SENSOR_REQ_ONESHOT) && controllerMac) {
    memcpy(gSensorControllerMac, controllerMac, 6);
  }
  for (int i = 0; i < REMOTE_SENSOR_MAX; i++) {
    if (!(sensorMask & (1u << i))) continue;
    if (mode == SENSOR_REQ_UNSUBSCRIBE) {
      // The lease ends. A stream that was switched on by hand goes on,
      // to the named master.
      gSensorLeaseExpiresAt[i] = 0;
      gSensorReqIntervalMs[i]  = 0;
      if (!espnowsensor::streamSurvivesLeaseEnd(gSensorByHand[i])) gSensorStreamingEnabled[i] = false;
    } else {
      // Subscribe or oneshot: publish every task-observed field before creating
      // the broadcaster, then stamp the lease.
      gSensorBroadcastEnabled = true;
      // A stream that starts here begins with the stream-start flag; a
      // renewal of a running one does not.
      if (!gSensorStreamingEnabled[i]) espnowsensor::streamRestart(gSensorTxTarget[i]);
      gSensorStreamingEnabled[i] = true;
      gLastTxMs[i] = 0;
      enabledMask |= 1u << i;
      gSensorReqIntervalMs[i] = intervalMs;
      uint32_t leaseFor = (mode == SENSOR_REQ_ONESHOT)
                            ? (uint32_t)(intervalMs ? intervalMs : 500)  // ~one push then expire
                            : (uint32_t)leaseMs;
      // Never 0: 0 is reserved for "manual / no lease".
      gSensorLeaseExpiresAt[i] = espnowDeadlineAfter((uint32_t)millis(), leaseFor);
    }
  }
  if (mode == SENSOR_REQ_UNSUBSCRIBE) controllerCleared = sensorControllerClearIfNoLeaseLocked(oldController);
  taskEXIT_CRITICAL(&gSensorBcastTaskMux);

  bool taskReady = true;
  if (enabledMask) {
    bool taskPresent;
    taskENTER_CRITICAL(&gSensorBcastTaskMux);
    taskPresent = gSensorBroadcasterTask != nullptr;
    taskEXIT_CRITICAL(&gSensorBcastTaskMux);
    taskReady = taskPresent || startSensorBroadcasterLocked();
    if (!taskReady) {
      taskENTER_CRITICAL(&gSensorBcastTaskMux);
      for (int i = 0; i < REMOTE_SENSOR_MAX; ++i) {
        if (!(enabledMask & (1u << i))) continue;
        gSensorStreamingEnabled[i] = false;
        gSensorLeaseExpiresAt[i] = 0;
        gSensorReqIntervalMs[i] = 0;
      }
      controllerCleared = sensorControllerClearIfNoLeaseLocked(oldController);
      taskEXIT_CRITICAL(&gSensorBcastTaskMux);
    }
  }
  xSemaphoreGive(gSensorBroadcasterLifecycleMutex);

  // The radio notice goes out after the lifecycle state is released. A
  // concurrent close may reject it through the runtime gate; the local state
  // is coherent either way.
  announceSensorStatus(controllerCleared ? oldController : nullptr);
  DEBUG_ESPNOW_STREAMF("[SENSOR_SUB] applied mode=%u mask=0x%lx interval=%u lease=%u",
                       mode, (unsigned long)sensorMask, intervalMs, leaseMs);
}

// ==========================
// Sensor Broadcast Control
// ==========================

void setSensorBroadcastEnabled(bool enabled) {
  gSensorBroadcastEnabled = enabled;
  DEBUG_ESPNOW_STREAMF("[SENSOR_BROADCAST] Sensor broadcasting %s", enabled ? "ENABLED" : "DISABLED");
}

bool isSensorBroadcastEnabled() {
  return gSensorBroadcastEnabled;
}

// SENSOR_TX_HOST_BEGIN
// A reading that was not sent is counted with its reason, never dropped
// silently. The console line comes from sensorLinkTick.
static void sensorNoteDrop(RemoteSensorType sensorType, espnowsensor::DropReason why) {
  if (sensorType >= REMOTE_SENSOR_MAX || why == espnowsensor::DropReason::None) return;
  gSensorTxStats[sensorType].notSent[(size_t)why]++;
  gSensorDropLastReason[sensorType].store((uint8_t)why, std::memory_order_relaxed);
  gSensorDropSinceLine[sensorType].fetch_add(1, std::memory_order_relaxed);
}

bool sensorSendReadingTo(const uint8_t dst[6], bool viaBond, RemoteSensorType sensorType,
                         const char* text, size_t len, espnowsensor::DropReason* why) {
  auto fail = [&](espnowsensor::DropReason reason) {
    if (why) *why = reason;
    return false;
  };
  if (!dst || !text || len == 0 || sensorType >= REMOTE_SENSOR_MAX) return fail(espnowsensor::DropReason::NoReceiver);
  // No session: nothing is parked for later (the next reading is newer
  // anyway). sensorLinkTick starts the handshake; a bond partner's session
  // is the bond heartbeat's business.
  if (!espnowSessionUp(dst)) {
    if (!viaBond) {
      taskENTER_CRITICAL(&gSensorBcastTaskMux);
      memcpy(gSensorNeedSessionMac, dst, 6);
      gSensorNeedSession = true;
      taskEXIT_CRITICAL(&gSensorBcastTaskMux);
    }
    return fail(espnowsensor::DropReason::NoSession);
  }
  // From here the reading has a number: one that is dropped below shows as a
  // gap at the receiver.
  espnowsensor::ReadingHeader h = {};
  h.sensorType = (uint8_t)sensorType;
  h.bootTag = sensorBootTag();
  taskENTER_CRITICAL(&gSensorBcastTaskMux);
  h.counter = espnowsensor::counterNext(gSensorTxCounter[sensorType]);
  if (espnowsensor::streamStartWanted(gSensorTxTarget[sensorType], dst)) {
    h.flags |= espnowsensor::kReadingFlagStreamStart;
  }
  taskEXIT_CRITICAL(&gSensorBcastTaskMux);

  // 190 bytes of text per frame to a direct peer, 118 through a relay. Should
  // the route change before the frame leaves, the reading may be lost, and
  // the receiver then counts it lost.
  espnowsensor::PartPlan plan;
  if (!espnowsensor::planParts(len, v4_single_frame_cap(dst), plan)) return fail(espnowsensor::DropReason::TooLong);
  uint8_t frame[espnowsensor::kFrameCapDirect];
  h.partCount = plan.count;
  for (uint8_t i = 0; i < plan.count; i++) {
    size_t offset = 0, n = 0;
    espnowsensor::partSpan(plan, len, i, offset, n);
    h.partIndex = i;
    espnowsensor::packReadingHeader(frame, h);
    memcpy(frame + espnowsensor::kReadingHeaderLen, text + offset, n);
    if (!v4_send_sensor_part(dst, viaBond, frame, (uint16_t)(espnowsensor::kReadingHeaderLen + n))) {
      return fail(espnowsensor::DropReason::QueueFull);
    }
    if (i == 0) {
      taskENTER_CRITICAL(&gSensorBcastTaskMux);
      espnowsensor::streamSentTo(gSensorTxTarget[sensorType], dst);
      taskEXIT_CRITICAL(&gSensorBcastTaskMux);
    }
  }
  if (why) *why = espnowsensor::DropReason::None;
  return true;
}

// Sends one reading to where readings go now (called by the broadcaster task).
static void transmitSensorData(RemoteSensorType sensorType, const char* jsonData, size_t jsonLen) {
  const espnowsensor::TargetChoice target = sensorReadingTarget((int)sensorType);
  if (target.why != espnowsensor::DropReason::None) {
    sensorNoteDrop(sensorType, target.why);
    return;
  }
  espnowsensor::DropReason why = espnowsensor::DropReason::None;
  if (sensorSendReadingTo(target.mac, target.viaBond, sensorType, jsonData, jsonLen, &why)) {
    gSensorTxStats[sensorType].sent++;
  } else {
    sensorNoteDrop(sensorType, why);
  }
}

void sensorLinkTick(uint32_t nowMs) {
  static uint32_t sLastLookMs = 0;
  if ((uint32_t)(nowMs - sLastLookMs) < 1000) return;
  sLastLookMs = nowMs;

  // Which paired devices are the masters named on this device: looked up
  // here, on espnow_task, and kept for the tasks that send.
  sensorReceiverTargetsRefresh();
  // What was received ages here, before its uptime stamps come round.
  remoteSensorAgeTick(nowMs);

  // The handshake is started here, on espnow_task, and not by the sender: the
  // broadcaster task has neither the stack nor the business. Backed off like
  // every automatic start (System_ESPNow_HandshakeBackoff.h).
  uint8_t mac[6];
  bool need;
  taskENTER_CRITICAL(&gSensorBcastTaskMux);
  need = gSensorNeedSession;
  gSensorNeedSession = false;
  memcpy(mac, gSensorNeedSessionMac, 6);
  taskEXIT_CRITICAL(&gSensorBcastTaskMux);
  if (need && !espnowSessionUp(mac)) (void)espnowHandshakeAutoStart(mac);

  // A bond worker tells its bond master which sensors it has once per bond
  // session (sensorBondStatusTick).
  sensorBondStatusTick(nowMs);

  // One line a minute per sensor while its readings are not sent.
  for (int i = 0; i < REMOTE_SENSOR_MAX; i++) {
    if (gSensorDropSinceLine[i].load(std::memory_order_relaxed) == 0) continue;
    if (!warnLimiterTake(gSensorDropWarn[i], nowMs, 60000, nullptr)) continue;
    const uint32_t dropped = gSensorDropSinceLine[i].exchange(0, std::memory_order_relaxed);
    const espnowsensor::DropReason why =
        (espnowsensor::DropReason)gSensorDropLastReason[i].load(std::memory_order_relaxed);
    BROADCAST_PRINTF("[ESP-NOW] %s readings not sent: %s (%lu since the last such line)",
                     sensorTypeToString((RemoteSensorType)i), espnowsensor::dropReasonText(why),
                     (unsigned long)dropped);
  }
}

// What espnowsensorstream says after a stream was switched on: where its
// readings go now, or why nothing is sent yet. `targetName` names choice.mac
// (used when choice.why is None).
static void sensorStreamOnReply(RemoteSensorType sensorType, const espnowsensor::TargetChoice& choice,
                                bool sessionUp, const char* targetName, char* out, size_t cap) {
  const char* sensor = sensorTypeToString(sensorType);
  const char* target = (targetName && targetName[0]) ? targetName : "the receiving device";
  if (choice.why == espnowsensor::DropReason::None) {
    if (sessionUp) {
      snprintf(out, cap, "OK: %s readings are sent to %s%s.", sensor, target, choice.viaBond ? " (bond partner)" : "");
    } else {
      snprintf(out, cap, "OK: %s streaming is on, but nothing is sent yet: %s has no session with this device.",
               sensor, target);
    }
    return;
  }
  const char* why;
  switch (choice.why) {
    case espnowsensor::DropReason::NoReceiver:
      why = "no master is named on this device (espnowmasterfingerprint)";
      break;
    case espnowsensor::DropReason::MeshOff:
      why = "mesh mode is off (espnowmode mesh)";
      break;
    default:
      why = espnowsensor::dropReasonText(choice.why);
      break;
  }
  snprintf(out, cap, "OK: %s streaming is on, but nothing is sent yet: %s.", sensor, why);
}
// SENSOR_TX_HOST_END

// Broadcaster task — wakes every 50ms; for each streaming-enabled sensor whose
// per-sensor interval has elapsed, calls its builder on demand and transmits.
// No intermediate "wire cache": the builder reads the sensor's own native cache
// under that sensor's mutex, so there is no propagation step that could be
// skipped or gated incorrectly.
static void sensorBroadcasterTask(void* param) {
  (void)param;

  DEBUG_ESPNOWF("[SENSOR_BCAST_TASK] Started on core %d", xPortGetCoreID());

  for (;;) {
    if (gSensorBroadcasterStopRequested.load(std::memory_order_acquire) ||
        gSensorStreamingShutdown.load(std::memory_order_acquire)) {
      break;
    }
    const unsigned long now = millis();

    // Stack HWM diagnostic. uxTaskGetStackHighWaterMark returns the *free* stack
    // ever observed (low-water mark), not used — peak_used = total - free.
    //
    // UNITS: BYTES, not words, despite the "words" in the log line below and in
    // upstream FreeRTOS's own docs. This port sets portSTACK_TYPE = uint8_t, so
    // StackType_t is 1 byte and the count comes back unscaled. SENSOR_BCAST_STACK_WORDS
    // is bytes too (see System_TaskUtils.h), so the subtraction is right — but read
    // any number here as bytes or you'll credit this task with 4x the headroom it
    // has. 4 KB total, not 16 KB.
    //
    // Logged every 5s under DEBUG_ESPNOW_CORE so we can size the stack to
    // measured peak + margin instead of guessing.
    static unsigned long lastHwmLog = 0;
    if (now - lastHwmLog >= 5000) {
      lastHwmLog = now;
      UBaseType_t hwmFreeWords = uxTaskGetStackHighWaterMark(nullptr);
      DEBUG_ESPNOWF(
             "[SENSOR_BCAST] stack free=%u words peak_used=%u of %u",
             (unsigned)hwmFreeWords,
             (unsigned)(SENSOR_BCAST_STACK_WORDS - hwmFreeWords),
             (unsigned)SENSOR_BCAST_STACK_WORDS);
    }

    // Presence heartbeat: tell the receiver every ~5 s which sensors are here
    // and which are streaming, so it keeps a present-but-not-streaming sensor
    // as a red card (mirrors the local model). One small SENSOR_STATUS frame.
    static unsigned long lastPresenceAnnounce = 0;
    if (now - lastPresenceAnnounce >= 5000) {
      lastPresenceAnnounce = now;
      announceSensorStatus();
    }

    for (int i = 0; i < REMOTE_SENSOR_MAX; i++) {
      if (!gSensorStreamingEnabled[i]) continue;

      // Lease expiry (secure fetcher, H3): if this sensor's lease has lapsed, clear
      // ONLY the flags here — never call stopSensorDataStreaming/stopSensorBroadcaster
      // from inside this task (that self-vTaskDeletes our own handle and wedges the
      // worker until reboot). The self-teardown after the loop handles task exit.
      // The lapse is decided and applied under the lock the subscription
      // apply takes, so a renewal that lands at this moment is not undone.
      // A stream that was switched on by hand goes on without the lease, to
      // the named master; the controller is forgotten with its last lease.
      bool lapsed = false;
      bool stopped = false;
      uint8_t oldController[6];
      bool controllerCleared = false;
      taskENTER_CRITICAL(&gSensorBcastTaskMux);
      if (sensorLeaseLapsed(i, (uint32_t)now)) {
        gSensorLeaseExpiresAt[i]   = 0;
        gSensorReqIntervalMs[i]    = 0;
        if (!espnowsensor::streamSurvivesLeaseEnd(gSensorByHand[i])) {
          gSensorStreamingEnabled[i] = false;
          stopped = true;
        }
        controllerCleared = sensorControllerClearIfNoLeaseLocked(oldController);
        lapsed = true;
      }
      taskEXIT_CRITICAL(&gSensorBcastTaskMux);
      if (lapsed) {
        announceSensorStatus(controllerCleared ? oldController : nullptr);
        if (stopped) continue;
      }

      const SensorBroadcastSpec& spec = gSensorSpecs[i];

      // Honor BOTH the per-sensor minimum and the applicable cadence — the leasing
      // controller's requested interval if set, else the user-tunable global — taking
      // whichever is slower so we throttle but never exceed a sensor's native cadence.
      unsigned long interval = spec.builder ? spec.minIntervalMs : 1000;
      unsigned long requested = gSensorReqIntervalMs[i] ? gSensorReqIntervalMs[i]
                                                        : gSettings.sensorBroadcastIntervalMs;
      if (requested > interval) interval = requested;
      if (now - gLastTxMs[i] < interval) continue;

      // A type this firmware cannot read (it was asked for all the same): one
      // counted drop per interval, so the reason is visible.
      if (!spec.builder) {
        sensorNoteDrop((RemoteSensorType)i, espnowsensor::DropReason::NotInBuild);
        gLastTxMs[i] = now;
        continue;
      }

      // Buffer is shared across all sensors and was sized at task start to fit
      // the largest spec.bufBytes. Builder returns bytes written or 0 when it
      // has no reading, which is not a drop.
      if (!gBroadcasterBuf || gBroadcasterBufSize < spec.bufBytes) continue;
      int len = spec.builder(gBroadcasterBuf, gBroadcasterBufSize);
      if (len <= 0) continue;

      DEBUG_ESPNOW_STREAMF("[BCAST_TX] %s len=%d", sensorTypeToString((RemoteSensorType)i), len);
      transmitSensorData((RemoteSensorType)i, gBroadcasterBuf, (size_t)len);
      gLastTxMs[i] = now;
    }

    if (gSensorBroadcasterStopRequested.load(std::memory_order_acquire) ||
        gSensorStreamingShutdown.load(std::memory_order_acquire)) {
      break;
    }

    // H3: if every sensor has stopped (e.g. all leases lapsed above), tear the task
    // down cleanly. The lifecycle mux decides whether this is an unowned natural
    // exit (self-delete) or an explicit stop whose owner will join/delete us.
    bool anyStillEnabled = false;
    for (int i = 0; i < REMOTE_SENSOR_MAX; i++) {
      if (gSensorStreamingEnabled[i]) { anyStillEnabled = true; break; }
    }
    if (!anyStillEnabled) {
      // Serialize the final recheck/detach with a concurrent start. Use a
      // zero-timeout take: closeespnow holds this mutex while it requests and
      // joins an explicit stop, so blocking here would deadlock that join.
      if (!gSensorBroadcasterLifecycleMutex ||
          xSemaphoreTake(gSensorBroadcasterLifecycleMutex, 0) != pdTRUE) {
        vTaskDelay(1);
        continue;
      }
      anyStillEnabled = false;
      for (int i = 0; i < REMOTE_SENSOR_MAX; ++i) {
        if (gSensorStreamingEnabled[i]) {
          anyStillEnabled = true;
          break;
        }
      }
      if (anyStillEnabled ||
          gSensorBroadcasterStopRequested.load(std::memory_order_acquire) ||
          gSensorStreamingShutdown.load(std::memory_order_acquire)) {
        xSemaphoreGive(gSensorBroadcasterLifecycleMutex);
        continue;
      }
      TaskHandle_t self = xTaskGetCurrentTaskHandle();
      taskENTER_CRITICAL(&gSensorBcastTaskMux);
      const bool externallyOwned = gSensorBroadcasterExternalStop;
      if (!externallyOwned && gSensorBroadcasterTask == self) {
        gSensorBroadcasterTask = nullptr;
      }
      taskEXIT_CRITICAL(&gSensorBcastTaskMux);
      xSemaphoreGive(gSensorBroadcasterLifecycleMutex);
      if (!externallyOwned) {
        DEBUG_ESPNOWF("[SENSOR_BCAST_TASK] all sensors stopped — self-teardown");
        vTaskDelete(nullptr);  // natural exit; no external join owner
      }
      break;
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }

  // Explicit cooperative stop. All builders/sends and their guards have
  // returned. Publish the join point and park; the lifecycle owner retains the
  // handle and performs the actual delete before any shared buffer is freed.
  xSemaphoreGive(gSensorBroadcasterStopDone);
  vTaskSuspend(nullptr);
}

// Start the broadcaster task
static bool startSensorBroadcasterLocked() {
  if (gSensorStreamingShutdown.load(std::memory_order_acquire)) return false;

  taskENTER_CRITICAL(&gSensorBcastTaskMux);
  if (gSensorBroadcasterTask) {
    const bool running = !gSensorBroadcasterExternalStop;
    taskEXIT_CRITICAL(&gSensorBcastTaskMux);
    return running;
  }
  if (gSensorBroadcasterExternalStop) {
    taskEXIT_CRITICAL(&gSensorBcastTaskMux);
    return false;
  }
  taskEXIT_CRITICAL(&gSensorBcastTaskMux);

  if (!gSensorBroadcasterStopDone) {
    gSensorBroadcasterStopDone =
        xSemaphoreCreateBinaryStatic(&gSensorBroadcasterStopDoneStorage);
  }
  if (!gSensorBroadcasterStopDone) return false;
  (void)xSemaphoreTake(gSensorBroadcasterStopDone, 0);
  gSensorBroadcasterStopRequested.store(false, std::memory_order_release);

  // Allocate the shared broadcaster buffer in PSRAM, sized to the largest
  // sensor's bufBytes. One allocation, reused forever — no per-tick churn.
  if (!gBroadcasterBuf) {
    size_t maxBuf = 0;
    for (int i = 0; i < REMOTE_SENSOR_MAX; i++) {
      if (gSensorSpecs[i].bufBytes > maxBuf) maxBuf = gSensorSpecs[i].bufBytes;
    }
    if (maxBuf == 0) maxBuf = 256;  // defensive; should not happen if any sensor is enabled
    gBroadcasterBuf = (char*)ps_alloc(maxBuf, AllocPref::RequirePsram,
                                      "espnow.sensor.broadcast");
    if (!gBroadcasterBuf) {
      DEBUG_ESPNOWF("[SENSOR_BROADCASTER] Failed to alloc %u-byte PSRAM buffer", (unsigned)maxBuf);
      return false;
    }
    gBroadcasterBufSize = maxBuf;
  }

  TaskHandle_t createdTask = nullptr;
  taskStackRecord("sensor_bcast", SENSOR_BCAST_STACK_WORDS);
  BaseType_t ret = xTaskCreatePinnedToCore(
    sensorBroadcasterTask,
    "sensor_bcast",
    SENSOR_BCAST_STACK_WORDS,
    nullptr,
    TASK_PRIORITY_HIGH,
    &createdTask,
    1      // Core 1 (opposite of ESP-NOW callback which is core 0)
  );

  if (ret == pdPASS) {
    taskENTER_CRITICAL(&gSensorBcastTaskMux);
    gSensorBroadcasterTask = createdTask;
    taskEXIT_CRITICAL(&gSensorBcastTaskMux);
    DEBUG_ESPNOWF("[SENSOR_BROADCASTER] Task started");
    return true;
  } else {
    taskENTER_CRITICAL(&gSensorBcastTaskMux);
    gSensorBroadcasterTask = nullptr;
    taskEXIT_CRITICAL(&gSensorBcastTaskMux);
    gSensorBroadcasterStopRequested.store(true, std::memory_order_release);
    DEBUG_ESPNOWF("[SENSOR_BROADCASTER] Failed to create task");
    return false;
  }
}

// Stop the broadcaster task
static bool stopSensorBroadcasterLocked(uint32_t timeoutMs) {
  TaskHandle_t task = nullptr;
  SemaphoreHandle_t stopDone = nullptr;
  taskENTER_CRITICAL(&gSensorBcastTaskMux);
  task = gSensorBroadcasterTask;
  stopDone = gSensorBroadcasterStopDone;
  if (!task) {
    taskEXIT_CRITICAL(&gSensorBcastTaskMux);
    return true;
  }
  if (task == xTaskGetCurrentTaskHandle()) {
    taskEXIT_CRITICAL(&gSensorBcastTaskMux);
    DEBUG_ESPNOWF("[SENSOR_BROADCASTER] stop rejected from broadcaster task");
    return false;
  }
  // A retrying close may adopt the same explicit stop and wait for the task's
  // already-published join token. The lifecycle mutex permits only one owner.
  gSensorBroadcasterExternalStop = true;
  gSensorBroadcasterStopRequested.store(true, std::memory_order_release);
  taskEXIT_CRITICAL(&gSensorBcastTaskMux);

  (void)xTaskAbortDelay(task);
  if (!stopDone || xSemaphoreTake(stopDone, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
    DEBUG_ESPNOWF(
           "[SENSOR_BROADCASTER] stop TIMEOUT after %ums; resources retained for retry",
           (unsigned)timeoutMs);
    return false;
  }

  vTaskDelete(task);
  taskENTER_CRITICAL(&gSensorBcastTaskMux);
  if (gSensorBroadcasterTask == task) gSensorBroadcasterTask = nullptr;
  gSensorBroadcasterExternalStop = false;
  taskEXIT_CRITICAL(&gSensorBcastTaskMux);
  DEBUG_ESPNOWF("[SENSOR_BROADCASTER] Task stopped");
  return true;
}

bool shutdownSensorDataStreamingForEspNowClose() {
  // Close admission first, then clear every local/bond-master UI and lease bit
  // in one short critical section. Do not call stopSensorDataStreaming(): it
  // emits one radio status frame per sensor (and a bond master emits STREAM_CTRL).
  gSensorStreamingShutdown.store(true, std::memory_order_release);
  if (!gSensorBroadcasterLifecycleMutex ||
      xSemaphoreTake(gSensorBroadcasterLifecycleMutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }
  taskENTER_CRITICAL(&gSensorBcastTaskMux);
  gSensorBroadcastEnabled = false;
  memset(gSensorStreamingEnabled, 0, sizeof(gSensorStreamingEnabled));
  memset(gSensorLeaseExpiresAt, 0, sizeof(gSensorLeaseExpiresAt));
  memset(gSensorReqIntervalMs, 0, sizeof(gSensorReqIntervalMs));
  memset(gSensorControllerMac, 0, sizeof(gSensorControllerMac));
  memset(gSensorByHand, 0, sizeof(gSensorByHand));
  memset(gLastTxMs, 0, sizeof(gLastTxMs));
  gSensorNeedSession = false;
  taskEXIT_CRITICAL(&gSensorBcastTaskMux);

  if (!stopSensorBroadcasterLocked(2000)) {
    xSemaphoreGive(gSensorBroadcasterLifecycleMutex);
    return false;
  }

  // The task has reached its explicit join and has been deleted; no builder can
  // still be using the shared buffer. Detach before free so a future init/start
  // allocates a fresh correctly-sized buffer.
  taskENTER_CRITICAL(&gSensorBcastTaskMux);
  char* buffer = gBroadcasterBuf;
  gBroadcasterBuf = nullptr;
  gBroadcasterBufSize = 0;
  taskEXIT_CRITICAL(&gSensorBcastTaskMux);
  if (buffer) free(buffer);
  xSemaphoreGive(gSensorBroadcasterLifecycleMutex);
  return true;
}

// How the clock that stamped an arrival was set: "none" (not set), "carried"
// (set, but by no source since boot), or the source's name.
static const char* arrivalClockName(int64_t arrivalEpochMs, uint8_t arrivalClock) {
  if (arrivalEpochMs == 0) return "none";
  if (arrivalClock == (uint8_t)Clock::SYNC_NONE) return "carried";
  return Clock::syncSourceName((Clock::SyncSource)arrivalClock);
}

// `,"link":"..","via":{"mac":"..","name":".."}|null` of a JSON row.
static void sensorLinkJson(uint8_t link, const uint8_t via[6], char* out, size_t cap) {
  if (link != espnowfwd::kLinkForwarded) {
    snprintf(out, cap, ",\"link\":\"%s\",\"via\":null", espnowfwd::linkKey(link));
    return;
  }
  char name[32];
  char nameJson[72];
  sensorViaName(via, name, sizeof(name));
  espnowsensor::jsonTextEscape(name, nameJson, sizeof(nameJson));
  snprintf(out, cap, ",\"link\":\"forwarded\",\"via\":{\"mac\":\"" MAC_FMT "\",\"name\":\"%s\"}", MAC_ARGS(via),
           nameJson);
}

String getRemoteSensorDataJson(const uint8_t* deviceMac, const uint8_t* via, RemoteSensorType sensorType) {
  RemoteSensorInfo info;
  if (sensorType < 0 || sensorType >= REMOTE_SENSOR_MAX ||
      !remoteSensorInfoFor(deviceMac, via, sensorType, &info)) {
    // Unknown to us = not connected. Mirrors a local sensor that isn't present.
    return "{\"connected\":false,\"enabled\":false,\"fresh\":false,\"data\":null}";
  }
  // The sender's counts for this sensor (zero when it has sent no reading yet).
  espnowsensor::CounterCounts counts = {};
  uint32_t incomplete = 0;
  uint16_t bootTag = 0;
  taskENTER_CRITICAL(&gRemoteSensorMux);
  for (int i = 0; gSensorPeers && i < REMOTE_SENSOR_PEERS_MAX; i++) {
    if (gSensorPeers[i].inUse && sensorRecordIs(gSensorPeers[i], deviceMac, via)) {
      counts = gSensorPeers[i].counts[sensorType];
      incomplete = gSensorPeers[i].incomplete;
      bootTag = gSensorPeers[i].track[sensorType].bootTag;
      break;
    }
  }
  taskEXIT_CRITICAL(&gRemoteSensorMux);

  // Freshness is independent of enabled/connected: a sensor can be connected +
  // enabled but momentarily stale, or connected + disabled (red dot, no data).
  const uint32_t now = (uint32_t)millis();
  String out;
  out.reserve(info.jsonLength + 520);
  char head[480];
  char linkJson[160];
  sensorLinkJson(info.link, info.viaMac, linkJson, sizeof(linkJson));
  int n = snprintf(head, sizeof(head), "{\"connected\":true,\"enabled\":%s,\"fresh\":%s%s",
                   info.enabled ? "true" : "false", info.fresh ? "true" : "false", linkJson);
  if (info.jsonLength > 0) {
    // The arrival is this device's own stamp: UTC epoch (null while its clock
    // was not set) and its uptime. bootTag and counter are the sender's: with
    // its MAC and the sensor they name one reading, also at a second master.
    // `lost` minus `late` never arrived; `incomplete` (all sensors of this
    // sender) is part of `lost`.
    char epoch[24] = "null";
    if (info.arrivalEpochMs != 0) snprintf(epoch, sizeof(epoch), "%lld", (long long)info.arrivalEpochMs);
    n += snprintf(head + n, sizeof(head) - (size_t)n,
                  ",\"arrivalEpochMs\":%s,\"arrivalUptimeMs\":%lu,\"agoMs\":%lu,\"clock\":\"%s\",\"bootTag\":%u,\"counter\":%u"
                  ",\"received\":%lu,\"lost\":%lu,\"late\":%lu,\"repeated\":%lu,\"restarts\":%lu,\"resumes\":%lu"
                  ",\"incomplete\":%lu",
                  epoch, (unsigned long)info.lastUpdate, (unsigned long)(now - info.lastUpdate),
                  arrivalClockName(info.arrivalEpochMs, info.arrivalClock), (unsigned)bootTag, (unsigned)info.counter,
                  (unsigned long)counts.received, (unsigned long)counts.lost, (unsigned long)counts.late,
                  (unsigned long)counts.repeated, (unsigned long)counts.restarts, (unsigned long)counts.resumes,
                  (unsigned long)incomplete);
  }
  out += head;
  out += ",\"data\":";
  bool haveText = false;
  if (info.fresh && info.jsonLength > 0) {
    char* text = (char*)ps_alloc(SENSOR_READING_MAX_BYTES + 1, AllocPref::PreferPsram, "espnow.sensor.reply");
    if (text) {
      // The stored text was checked to be one JSON object when it arrived.
      if (remoteSensorCopyText(deviceMac, via, sensorType, text, SENSOR_READING_MAX_BYTES + 1) > 0) {
        out += text;
        haveText = true;
      }
      free(text);
    }
  }
  if (!haveText) out += "null";
  out += "}";
  return out;
}

int formatRemoteSensorReadable(const char* json, char* out, size_t outSize, int maxLines) {
  if (!out || outSize == 0) return 0;
  out[0] = '\0';
  if (!json || !json[0]) { strlcpy(out, "(no data)", outSize); return 1; }

  PSRAM_JSON_DOC(doc);
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    strlcpy(out, "(unreadable)", outSize); return 1;
  }
  JsonObjectConst root = doc.as<JsonObjectConst>();
  if (root.isNull()) { strlcpy(out, "(no fields)", outSize); return 1; }

  size_t pos = 0;
  int lines = 0;
  for (JsonPairConst kv : root) {
    if (lines >= maxLines) break;
    const char* key = kv.key().c_str();
    if (!key || !key[0]) continue;
    // Skip bookkeeping/noise keys that aren't useful on a small screen. This
    // includes the shared sensor-reading envelope's metadata (valid/connected/
    // ts/age) — the value keys are what matter on a tiny remote readout.
    if (!strcmp(key, "ts") || !strcmp(key, "seq") || !strcmp(key, "val") ||
        !strcmp(key, "valid") || !strcmp(key, "connected") || !strcmp(key, "age"))
      continue;

    char valbuf[20];
    JsonVariantConst v = kv.value();
    if (v.is<bool>()) {
      snprintf(valbuf, sizeof(valbuf), "%s", v.as<bool>() ? "yes" : "no");
    } else if (v.is<const char*>()) {
      const char* s = v.as<const char*>();
      snprintf(valbuf, sizeof(valbuf), "%s", s ? s : "");
    } else if (v.is<float>()) {  // any JSON number (int or real)
      double d = v.as<double>();
      if (d == (double)(long long)d) snprintf(valbuf, sizeof(valbuf), "%lld", (long long)d);
      else                           snprintf(valbuf, sizeof(valbuf), "%.2f", d);
    } else {
      continue;  // arrays / nested objects / null — skip on a small screen
    }

    int n = snprintf(out + pos, outSize - pos, "%s%s: %s", lines ? "\n" : "", key, valbuf);
    if (n < 0) break;
    if ((size_t)n >= outSize - pos) { out[outSize - 1] = '\0'; lines++; break; }
    pos += (size_t)n;
    lines++;
  }
  if (lines == 0) { strlcpy(out, "(no fields)", outSize); return 1; }
  return lines;
}

String getRemoteDevicesListJson() {
  PSRAM_JSON_DOC(doc);
  JsonArray devices = doc["devices"].to<JsonArray>();

  // List every CONNECTED (present) remote sensor — including disabled ones, so the
  // web shows them as red cards instead of dropping them. Each sensor carries its
  // own enabled/fresh state so the dot mirrors the local model. A sensor not
  // heard from (status or reading) for the presence time is left out; its slot
  // is reused by the writer when it needs one.
  const int slots = remoteSensorSlotCount();
  for (int i = 0; i < slots; i++) {
    RemoteSensorInfo e;
    if (!remoteSensorInfoAt(i, &e)) continue;
    const bool fresh = e.fresh;

    // Find or create device entry: one per sender (origin, via).
    String macStr = macToDisplayStr(e.deviceMac);
    const bool forwarded = e.link == espnowfwd::kLinkForwarded;
    String viaStr = forwarded ? macToDisplayStr(e.viaMac) : String("");
    JsonObject deviceObj;
    bool found = false;
    for (JsonObject dev : devices) {
      const char* devVia = dev["via"].is<JsonObject>() ? (dev["via"]["mac"] | "") : "";
      if (strcmp(dev["mac"], macStr.c_str()) == 0 && strcmp(devVia, viaStr.c_str()) == 0) {
        deviceObj = dev;
        found = true;
        break;
      }
    }
    if (!found) {
      deviceObj = devices.add<JsonObject>();
      deviceObj["mac"] = macStr;
      deviceObj["name"] = e.deviceName;
      deviceObj["link"] = espnowfwd::linkKey(e.link);
      if (forwarded) {
        char viaName[32];
        sensorViaName(e.viaMac, viaName, sizeof(viaName));
        JsonObject via = deviceObj["via"].to<JsonObject>();
        via["mac"] = viaStr;
        via["name"] = viaName;
      } else {
        deviceObj["via"] = nullptr;
      }
      deviceObj["sensors"].to<JsonArray>();
    }

    // Add sensor as an object {type, enabled, fresh} (was a bare type string).
    JsonArray sensors = deviceObj["sensors"];
    JsonObject s = sensors.add<JsonObject>();
    s["type"] = sensorTypeToString(e.sensorType);
    s["enabled"] = e.enabled;
    s["fresh"] = fresh;
  }
  
  String result;
  serializeJson(doc, result);
  return result;
}


// ==========================
// Thermal Data Optimization
// ==========================

#if ENABLE_THERMAL_SENSOR
int buildThermalDataJsonInteger(char* buf, size_t bufSize) {
  if (!buf || bufSize == 0) return 0;
  std::unique_ptr<int16_t, decltype(&ps_free)> frame(
      static_cast<int16_t*>(ps_alloc(THERMAL_RAW_FRAME_PIXELS * sizeof(int16_t),
                                    AllocPref::PreferPsram, "thermal.espnow.snapshot")), ps_free);
  ThermalCache reading;
  if (!frame || !thermalFrameSnapshot(reading, frame.get(), THERMAL_RAW_FRAME_PIXELS, false, 100)) {
    int n = snprintf(buf, bufSize, "{\"error\":\"Sensor data temporarily unavailable\"}");
    return n < 0 || static_cast<size_t>(n) >= bufSize ? 0 : n;
  }
  if (!reading.thermalFrameAvailable) {
    int n = snprintf(buf, bufSize, "{\"val\":0,\"error\":\"Sensor stopped\"}");
    return n < 0 || static_cast<size_t>(n) >= bufSize ? 0 : n;
  }
  int pos = snprintf(buf, bufSize,
      "{\"val\":%d,\"seq\":%lu,\"mn\":%d,\"mx\":%d,\"w\":%d,\"h\":%d,\"data\":[",
      reading.thermalDataValid ? 1 : 0, (unsigned long)reading.thermalSeq,
      (int)reading.thermalMinTemp, (int)reading.thermalMaxTemp,
      reading.thermalFrameWidth, reading.thermalFrameHeight);
  if (pos < 0 || static_cast<size_t>(pos) >= bufSize) return 0;
  for (size_t i = 0; i < THERMAL_RAW_FRAME_PIXELS; ++i) {
    int n = snprintf(buf + pos, bufSize - pos, "%d%s", frame.get()[i] / 100,
                     i + 1 < THERMAL_RAW_FRAME_PIXELS ? "," : "");
    if (n < 0 || static_cast<size_t>(n) >= bufSize - pos) return 0;
    pos += n;
  }
  int n = snprintf(buf + pos, bufSize - pos, "]}");
  return n < 0 || static_cast<size_t>(n) >= bufSize - pos ? 0 : pos + n;
}
#else
int buildThermalDataJsonInteger(char* buf, size_t bufSize) {
  if (!buf || bufSize == 0) return 0;
  int pos = snprintf(buf, bufSize, "{\"error\":\"Thermal sensor not compiled\"}");
  return (pos > 0) ? pos : 0;
}
#endif

// ============================================================================
// CLI Commands for Sensor Streaming (merged from espnow_sensor_commands.cpp)
// ============================================================================

// The name a user knows a paired device by, or its address.
static void sensorPeerDisplayName(const uint8_t* mac, char* out, size_t cap) {
  String name = getEspNowDeviceName(mac);
  if (name.length() == 0) name = macToDisplayStr(mac);
  strlcpy(out, name.c_str(), cap);
}

const char* cmd_espnow_sensorstream(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // D1: sensor-stream control is LOCAL-CONSOLE-ONLY. The only remote path to control a
  // worker's streaming is the fingerprint-gated SENSOR_REQ opcode — the generic
  // credentialed remote-exec route (espnowremote … espnowsensorstream …) must not be
  // able to drive a worker's sensors. See docs/ESPNOW_SENSOR_FETCHER_DESIGN.md.
  if (currentAuthContext().transport == SOURCE_ESPNOW) {
    return "Error: espnowsensorstream is local-console-only. Remote sensor control is via "
           "the secure fetcher (SENSOR_REQ), not remote command execution.";
  }

  // Parse: <sensor> <on|off>
  CommandArgs a(argsInput);
  if (!a.hasMinArgs(2)) return "Error: invalid arguments — Usage: espnowsensorstream <sensor> <on|off>";

  String sensorName = a.arg(0);
  normalizeCliArg(sensorName);
  RemoteSensorType sensorType = REMOTE_SENSOR_THERMAL;
  if (!sensorTypeFromString(sensorName.c_str(), sensorType)) {
    return "Error: invalid arguments — Usage: espnowsensorstream <sensor> <on|off>";
  }
  int boolResult = parseBoolArg(a.arg(1));
  if (boolResult < 0) return "Error: invalid arguments — Usage: espnowsensorstream <sensor> <on|off>";
  const bool enable = (boolResult == 1);
  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM_CMD] %s %s", sensorTypeToString(sensorType), enable ? "on" : "off");

  // The acting master receives readings and sends none of its own.
  if (gSettings.meshRole == MESH_ROLE_MASTER) {
    return "Error: this device is the mesh master. It receives readings and does not send its own.";
  }

  // A sensor this firmware cannot read would stream nothing: say so now. (A
  // bond master switches its partner's sensor on, not its own.)
  bool asksPartner = false;
#if ENABLE_BONDED_MODE
  asksPartner = gSettings.bondModeEnabled && isBondMaster();
#endif
  EXT_RAM_BSS_ATTR static char reply[200];
  if (enable && !asksPartner && !sensorTypeInThisBuild(sensorType)) {
    snprintf(reply, sizeof(reply), "Error: this firmware cannot read the %s sensor, so it has no readings to send",
             sensorTypeToString(sensorType));
    return reply;
  }

  if (!enable) {
    stopSensorDataStreaming(sensorType);
    snprintf(reply, sizeof(reply), asksPartner ? "OK: the bond partner was asked to stop sending %s readings."
                                               : "OK: %s streaming is off.",
             sensorTypeToString(sensorType));
    return reply;
  }
  startSensorDataStreaming(sensorType);
  if (asksPartner) {
    snprintf(reply, sizeof(reply), "OK: the bond partner was asked to send %s readings.", sensorTypeToString(sensorType));
    return reply;
  }
  if (!isSensorDataStreamingEnabled(sensorType)) return "Error: the stream could not be started";
  // Where its readings go now, or why nothing is sent yet.
  const espnowsensor::TargetChoice target = sensorReadingTarget((int)sensorType);
  char targetName[32] = "";
  bool sessionUp = false;
  if (target.why == espnowsensor::DropReason::None) {
    sensorPeerDisplayName(target.mac, targetName, sizeof(targetName));
    sessionUp = espnowSessionUp(target.mac);
  }
  sensorStreamOnReply(sensorType, target, sessionUp, targetName, reply, sizeof(reply));
  return reply;
}

// What the receiver holds and refused, as one snapshot.
struct RemoteSensorTotals {
  int senders;   // paired senders this device keeps something of
  int sensors;   // sensors listed (heard from within the presence time)
  RemoteSensorRefusals refusals;
  uint32_t received, lost, late, incomplete, malformed;
};

static RemoteSensorTotals remoteSensorTotals() {
  RemoteSensorTotals t = {};
  const uint32_t nowMs = (uint32_t)millis();
  taskENTER_CRITICAL(&gRemoteSensorMux);
  t.refusals = gRemoteSensorRefusals;
  taskEXIT_CRITICAL(&gRemoteSensorMux);
  // One sender per hold (the records are in PSRAM).
  for (int i = 0; gSensorPeers && i < REMOTE_SENSOR_PEERS_MAX; i++) {
    taskENTER_CRITICAL(&gRemoteSensorMux);
    const SensorPeerRecord& peer = gSensorPeers[i];
    if (peer.inUse) {
      t.senders++;
      t.incomplete += peer.incomplete;
      t.malformed += peer.malformed;
      for (int k = 0; k < REMOTE_SENSOR_MAX; k++) {
        if (peer.slots && slotPresent(peer.slots[k], nowMs)) t.sensors++;
        t.received += peer.counts[k].received;
        t.lost += peer.counts[k].lost;
        t.late += peer.counts[k].late;
      }
    }
    taskEXIT_CRITICAL(&gRemoteSensorMux);
  }
  return t;
}

// One sender as the receiver knows it, for the status lists.
struct RemoteSensorPeerSummary {
  uint8_t mac[6];
  uint8_t via[6];
  uint8_t link;
  char name[32];
  uint16_t presentMask;    // sensors listed
  uint16_t streamingMask;  // ... that the sender says it streams
  uint16_t freshMask;      // ... with a reading within REMOTE_SENSOR_TTL_MS
  uint16_t readMask;       // ... with a reading kept at all (a status frame alone lists a sensor)
  uint32_t received, lost, late, incomplete, malformed;
};

// False for a record that is not in use.
static bool remoteSensorPeerSummaryAt(int index, RemoteSensorPeerSummary* out) {
  if (!out || index < 0 || index >= REMOTE_SENSOR_PEERS_MAX) return false;
  const uint32_t nowMs = (uint32_t)millis();
  bool have = false;
  memset(out, 0, sizeof(*out));
  taskENTER_CRITICAL(&gRemoteSensorMux);
  if (gSensorPeers && gSensorPeers[index].inUse) {
    const SensorPeerRecord& peer = gSensorPeers[index];
    have = true;
    memcpy(out->mac, peer.mac, 6);
    memcpy(out->via, peer.via, 6);
    out->link = peer.link;
    memcpy(out->name, peer.name, sizeof(out->name));
    out->incomplete = peer.incomplete;
    out->malformed = peer.malformed;
    for (int k = 0; k < REMOTE_SENSOR_MAX; k++) {
      out->received += peer.counts[k].received;
      out->lost += peer.counts[k].lost;
      out->late += peer.counts[k].late;
      if (!peer.slots || !slotPresent(peer.slots[k], nowMs)) continue;
      const RemoteSensorData& e = peer.slots[k];
      out->presentMask |= (uint16_t)(1u << k);
      if (e.enabled) out->streamingMask |= (uint16_t)(1u << k);
      if (slotFresh(e, nowMs)) out->freshMask |= (uint16_t)(1u << k);
      if (e.valid) out->readMask |= (uint16_t)(1u << k);
    }
  }
  taskEXIT_CRITICAL(&gRemoteSensorMux);
  return have;
}

static const uint8_t* summaryVia(const RemoteSensorPeerSummary& s) {
  return s.link == espnowfwd::kLinkForwarded ? s.via : nullptr;
}

// The summary of (mac, via); via nullptr: the origin's own record.
static bool remoteSensorPeerSummaryFor(const uint8_t* mac, const uint8_t* via, RemoteSensorPeerSummary* out) {
  for (int i = 0; i < REMOTE_SENSOR_PEERS_MAX; i++) {
    if (!remoteSensorPeerSummaryAt(i, out) || memcmp(out->mac, mac, 6) != 0) continue;
    if (memcmp(out->via, via ? via : kSensorNoVia, 6) == 0) return true;
  }
  return false;
}

static const char* sensorRoleWords(uint8_t role) {
  if (role == MESH_ROLE_MASTER) return "master. It keeps what it is sent and sends none of its own";
  if (role == MESH_ROLE_BACKUP_MASTER) return "backup master. It sends its own to the master and keeps what it is sent";
  return "worker. It sends its own to the master and keeps nothing";
}

// SENSOR_STATUS_JSON_HOST_BEGIN
// Serializes a status reply into `buf` (`cap` bytes), whole and terminated.
// When `list` is given, its last entries are left out until the reply fits,
// and `moreKey` says whether any was: the key is always there (false, else
// true), and its room is counted before anything is cut. Returns the length,
// or 0 (buf "") when the reply does not fit even so.
static size_t sensorStatusJsonFit(JsonDocument& doc, JsonArray list, const char* moreKey, char* buf, size_t cap) {
  if (!buf || cap == 0) return 0;
  buf[0] = '\0';
  bool cut = false;
  if (!list.isNull() && moreKey) {
    doc[moreKey] = false;  // "false" is the longer of the two: room for either
    while (measureJson(doc) >= cap && list.size() > 0) {
      list.remove(list.size() - 1);
      cut = true;
    }
    doc[moreKey] = cut;
  }
  if (measureJson(doc) >= cap) return 0;
  const size_t n = serializeJson(doc, buf, cap);
  if (n >= cap) {
    buf[0] = '\0';
    return 0;
  }
  buf[n] = '\0';
  return n;
}
// SENSOR_STATUS_JSON_HOST_END

static const char kSensorStatusTooLarge[] = "{\"schema\":1,\"error\":\"the reply does not fit its buffer\"}";

// espnowsensorstatus json <name_or_mac> [via <name_or_mac>]: everything this
// device keeps of one sender. With 'via', the bond partner that device
// forwards for (a forwarded partner is also found by the name its link shows).
static const char* sensorStatusOneDeviceJson(const String& whoArgs) {
  String who = whoArgs;
  String viaWho;
  const int viaAt = whoArgs.indexOf(" via ");
  if (viaAt >= 0) {
    who = whoArgs.substring(0, viaAt);
    viaWho = whoArgs.substring(viaAt + 5);
    who.trim();
    viaWho.trim();
    if (who.length() == 0 || viaWho.length() == 0) {
      return "Usage: espnowsensorstatus [json [<name_or_mac> [via <name_or_mac>]]]";
    }
  }
  uint8_t mac[6];
  uint8_t viaMac[6];
  const uint8_t* via = nullptr;
  if (viaWho.length() > 0) {
    if (!resolveDeviceNameOrMac(viaWho, viaMac)) return "Error: no such device (use its name or its MAC address)";
    uint8_t foundVia[6];
    if (!resolveDeviceNameOrMac(who, mac) && !espnowForwardFindOrigin(who.c_str(), viaMac, mac, foundVia)) {
      return "Error: no such device (use its name or its MAC address)";
    }
    via = viaMac;
  } else if (!resolveDeviceNameOrMac(who, mac)) {
    // A bond partner another device forwards for, by the name its link shows.
    if (!espnowForwardFindOrigin(who.c_str(), nullptr, mac, viaMac)) {
      return "Error: no such device (use its name or its MAC address)";
    }
    via = viaMac;
  }
  EXT_RAM_BSS_ATTR static char jbuf[4096];
  PSRAM_JSON_DOC(doc);
  doc["schema"] = 1;
  doc["mac"] = macToDisplayStr(mac);
  char name[33];
  if (via) {
    espnowForwardOriginName(via, mac, name, sizeof(name));
    if (!name[0]) sensorPeerDisplayName(mac, name, sizeof(name));
  } else {
    sensorPeerDisplayName(mac, name, sizeof(name));
  }
  doc["name"] = name;
  doc["paired"] = peerIdentityFindByMac(mac) != nullptr;
  doc["link"] = via ? "forwarded" : nullptr;
  if (via) {
    char viaName[32];
    sensorViaName(via, viaName, sizeof(viaName));
    JsonObject viaObj = doc["via"].to<JsonObject>();
    viaObj["mac"] = macToDisplayStr(via);
    viaObj["name"] = viaName;
  } else {
    doc["via"] = nullptr;
  }
  RemoteSensorPeerSummary summary;
  const bool known = remoteSensorPeerSummaryFor(mac, via, &summary);
  if (!via && known) doc["link"] = espnowfwd::linkKey(summary.link);
  doc["incomplete"] = known ? summary.incomplete : 0;
  doc["malformed"] = known ? summary.malformed : 0;
  // Per sensor: the keys of GET /api/sensors/remote without the reading's text.
  JsonObject sensors = doc["sensors"].to<JsonObject>();
  const uint32_t nowMs = (uint32_t)millis();
  for (int i = 0; known && i < REMOTE_SENSOR_MAX; i++) {
    const RemoteSensorType type = (RemoteSensorType)i;
    RemoteSensorInfo info;
    const bool listed = remoteSensorInfoFor(mac, via, type, &info);
    espnowsensor::CounterCounts counts = {};
    uint16_t bootTag = 0;
    taskENTER_CRITICAL(&gRemoteSensorMux);
    const SensorPeerRecord* peer = findPeerRecordLocked(mac, via);
    if (peer) {
      counts = peer->counts[i];
      bootTag = peer->track[i].bootTag;
    }
    taskEXIT_CRITICAL(&gRemoteSensorMux);
    if (!listed && counts.received == 0) continue;
    JsonObject row = sensors[sensorTypeToString((RemoteSensorType)i)].to<JsonObject>();
    row["connected"] = listed;
    row["enabled"] = listed && info.enabled;
    row["fresh"] = listed && info.fresh;
    if (listed && info.jsonLength > 0) {
      if (info.arrivalEpochMs != 0) row["arrivalEpochMs"] = info.arrivalEpochMs;
      else row["arrivalEpochMs"] = nullptr;
      row["arrivalUptimeMs"] = info.lastUpdate;
      row["agoMs"] = (uint32_t)(nowMs - info.lastUpdate);
      row["clock"] = arrivalClockName(info.arrivalEpochMs, info.arrivalClock);
      row["bootTag"] = bootTag;
      row["counter"] = info.counter;
    }
    row["received"] = counts.received;
    row["lost"] = counts.lost;
    row["late"] = counts.late;
    row["repeated"] = counts.repeated;
    row["restarts"] = counts.restarts;
    row["resumes"] = counts.resumes;
  }
  // Battery state and events of the same sender (null: nothing kept).
  EXT_RAM_BSS_ATTR static char part[640];
  if (espnowBatteryJsonFor(mac, via, part, sizeof(part)) > 0) doc["battery"] = serialized(part);
  else doc["battery"] = nullptr;
  // Its event counts for an admin only (as espnowevents); for anyone else
  // the key is left out and eventCounts says why.
  EXT_RAM_BSS_ATTR static char eventsPart[320];
  if (!espnowEventCountsVisible()) doc["eventCounts"] = "admin only";
  else if (espnowEventCountsJsonFor(mac, via, eventsPart, sizeof(eventsPart)) > 0) doc["events"] = serialized(eventsPart);
  else doc["events"] = nullptr;
  if (sensorStatusJsonFit(doc, JsonArray(), nullptr, jbuf, sizeof(jbuf)) == 0) return kSensorStatusTooLarge;
  return jbuf;
}

const char* cmd_espnow_sensorstatus(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (argLeadingTokenIsJson(argsInput)) {
    String rest = argsInput;
    rest.trim();
    rest = rest.substring(4);
    rest.trim();
    if (rest.length() > 0) return sensorStatusOneDeviceJson(rest);
  }

  const uint8_t role = gSettings.meshRole;
  const bool sends = espnowsensor::roleSends(role);
  const bool receives = espnowsensor::roleReceives(role);
  const RemoteSensorTotals totals = remoteSensorTotals();
  SensorReceiverTargets targets;
  sensorReceiverTargets(&targets);
  const uint32_t nowMs = (uint32_t)millis();
  PowerEventsSendView sending;
  espnowPowerEventsSendView(&sending);
  bool bondWorker = false;
#if ENABLE_BONDED_MODE
  bondWorker = gSettings.bondModeEnabled && isBondWorker();
#endif

  if (argWantsJson(argsInput)) {
    EXT_RAM_BSS_ATTR static char jbuf[4096];
    PSRAM_JSON_DOC(doc);
    doc["schema"]    = 1;
    doc["broadcast"] = isSensorBroadcastEnabled();
    doc["role"] = espnowsensor::roleKey(role);
    // The masters named on this device that are paired: where a sending
    // device's readings (switched on by hand), battery state and events go.
    JsonArray targetList = doc["targets"].to<JsonArray>();
    auto addTarget = [&](const char* kind, bool have, const uint8_t* mac) {
      if (!have) return;
      JsonObject row = targetList.add<JsonObject>();
      char name[32];
      sensorPeerDisplayName(mac, name, sizeof(name));
      row["kind"] = kind;
      row["mac"] = macToDisplayStr(mac);
      row["name"] = name;
      row["session"] = espnowSessionUp(mac);
    };
    addTarget("primary", targets.havePrimary, targets.primary);
    addTarget("backup", targets.haveBackup, targets.backup);
    doc["masterNamed"] = targets.masterNamed || targets.backupNamed;
    if (sends) {
      // Which sensors are streaming, and what was sent and what not (a
      // sensor with nothing sent and nothing dropped is left out of "sent").
      // The names are those of the one sensor table.
      JsonObject streaming = doc["streaming"].to<JsonObject>();
      JsonObject sent = doc["sent"].to<JsonObject>();
      for (int i = 0; i < REMOTE_SENSOR_MAX; i++) {
        const char* name = sensorTypeToString((RemoteSensorType)i);
        streaming[name] = isSensorDataStreamingEnabled((RemoteSensorType)i);
        const SensorTxStats stats = gSensorTxStats[i];
        uint32_t notSent = 0;
        for (size_t r = 1; r < espnowsensor::kDropReasonCount; r++) notSent += stats.notSent[r];
        if (stats.sent == 0 && notSent == 0) continue;
        JsonObject row = sent[name].to<JsonObject>();
        row["sent"] = stats.sent;
        row["notSent"] = notSent;
        for (size_t r = 1; r < espnowsensor::kDropReasonCount; r++) {
          if (stats.notSent[r]) row[espnowsensor::dropReasonKey((espnowsensor::DropReason)r)] = stats.notSent[r];
        }
      }
    }
    // Battery state and events this device sends (espnowbattery, espnowevents
    // show what it received).
    JsonObject battery = doc["battery"].to<JsonObject>();
    battery["sends"] = sending.batteryWhy == espnowsensor::PushWhyNot::Sends;
    if (espnowsensor::pushWhyNotKey(sending.batteryWhy)) battery["whyNot"] = espnowsensor::pushWhyNotKey(sending.batteryWhy);
    else battery["whyNot"] = nullptr;
    battery["intervalS"] = sending.batteryIntervalS;
    battery["sent"] = sending.batterySent;
    if (sending.batteryEverSent) battery["lastSentAgoS"] = (uint32_t)(nowMs - sending.batteryLastSentMs) / 1000;
    else battery["lastSentAgoS"] = nullptr;
    JsonObject events = doc["events"].to<JsonObject>();
    events["sends"] = sending.eventsWhy == espnowsensor::PushWhyNot::Sends;
    if (espnowsensor::pushWhyNotKey(sending.eventsWhy)) events["whyNot"] = espnowsensor::pushWhyNotKey(sending.eventsWhy);
    else events["whyNot"] = nullptr;
    events["floor"] = espnowsensor::eventPushKey(sending.eventFloor);
    events["sent"] = sending.eventsSent;
    events["leftOut"] = sending.eventsLeftOut;
    // What this device received and refused (all zero on a device nothing is sent to).
    JsonObject table = doc["table"].to<JsonObject>();
    table["senders"] = totals.senders;
    table["sendersMax"] = REMOTE_SENSOR_PEERS_MAX;
    table["sensors"] = totals.sensors;
    table["received"] = totals.received;
    table["lost"] = totals.lost;
    table["late"] = totals.late;
    table["incomplete"] = totals.incomplete;
    table["malformed"] = totals.malformed + totals.refusals.malformed;
    table["refusedFull"] = totals.refusals.noRecord;
    table["refusedNoMemory"] = totals.refusals.noMemory;
    table["badType"] = totals.refusals.badType;
    table["notReceiver"] = totals.refusals.notReceiver;
    table["forwardRefused"] = totals.refusals.forwardRefused;
    // Devices that forward their bond partner's data here
    // ('espnowforwardlinks json' lists them).
    doc["forwardLinks"] = (uint32_t)espnowForwardLinkCount();
    JsonArray devices;  // only on a device that keeps readings
    if (receives || totals.senders) {
      // The senders and their sensors ({"devices":[...]} as GET
      // /api/sensors/remote lists them), each with its counts.
      PSRAM_JSON_DOC(tmp);
      if (deserializeJson(tmp, getRemoteDevicesListJson()) == DeserializationError::Ok &&
          tmp["devices"].is<JsonArray>()) {
        doc["devices"] = tmp["devices"];
      } else {
        doc["devices"].to<JsonArray>();
      }
      devices = doc["devices"].as<JsonArray>();
      for (JsonObject dev : devices) {
        uint8_t mac[6];
        uint8_t viaMac[6];
        RemoteSensorPeerSummary summary;
        const char* macText = dev["mac"] | "";
        const bool forwarded = dev["via"].is<JsonObject>();
        const char* viaText = forwarded ? (dev["via"]["mac"] | "") : "";
        if (!parseMacAddress(String(macText), mac)) continue;
        if (forwarded && !parseMacAddress(String(viaText), viaMac)) continue;
        if (!remoteSensorPeerSummaryFor(mac, forwarded ? viaMac : nullptr, &summary)) continue;
        dev["received"] = summary.received;
        dev["lost"] = summary.lost;
        dev["late"] = summary.late;
      }
    }
    // The reply holds 4 KB. Devices that do not fit are left out and
    // `devicesMore` says so; 'espnowsensorstatus json <name_or_mac>' has one
    // device in full.
    if (sensorStatusJsonFit(doc, devices, "devicesMore", jbuf, sizeof(jbuf)) == 0) return kSensorStatusTooLarge;
    return jbuf;
  }

  broadcastOutput("[ESP-NOW] Readings, battery state and events between this device and the mesh");
  BROADCAST_PRINTF("  Role: %s.", sensorRoleWords(role));
  if (sends) {
    if (bondWorker) broadcastOutput("  Readings go to the bond partner.");
    auto sayTarget = [&](const char* lead, const char* what, const uint8_t* mac) {
      char name[32];
      sensorPeerDisplayName(mac, name, sizeof(name));
      BROADCAST_PRINTF("  %s %s (%s named on this device), %s", lead, name, what,
                       espnowSessionUp(mac) ? "session up" : "no session");
    };
    if (targets.havePrimary) sayTarget("Sending to:", "master", targets.primary);
    if (targets.haveBackup) sayTarget(targets.havePrimary ? "           " : "Sending to:", "backup master", targets.backup);
    if (!targets.havePrimary && !targets.haveBackup) {
      BROADCAST_PRINTF("  Sending to: nobody. %s.",
                       (targets.masterNamed || targets.backupNamed)
                           ? "The master named on this device is not paired with it"
                           : "No master is named on this device (espnowmasterfingerprint)");
    }
    // Every sensor type of the one table, with what was sent and why not.
    BROADCAST_PRINTF("  Readings (sensor sending is %s):", isSensorBroadcastEnabled() ? "on" : "off");
    for (int i = 0; i < REMOTE_SENSOR_MAX; i++) {
      const RemoteSensorType type = (RemoteSensorType)i;
      const SensorTxStats stats = gSensorTxStats[i];
      uint32_t notSent = 0;
      size_t topReason = 0;
      for (size_t r = 1; r < espnowsensor::kDropReasonCount; r++) {
        notSent += stats.notSent[r];
        if (stats.notSent[r] > stats.notSent[topReason]) topReason = r;
      }
      if (!sensorTypeInThisBuild(type)) {
        BROADCAST_PRINTF("    %s: not in this firmware", sensorTypeToString(type));
      } else if (notSent) {
        BROADCAST_PRINTF("    %s: %s, sent %lu, not sent %lu (mostly: %s)", sensorTypeToString(type),
                         isSensorDataStreamingEnabled(type) ? "on" : "off", (unsigned long)stats.sent,
                         (unsigned long)notSent, espnowsensor::dropReasonText((espnowsensor::DropReason)topReason));
      } else {
        BROADCAST_PRINTF("    %s: %s, sent %lu", sensorTypeToString(type),
                         isSensorDataStreamingEnabled(type) ? "on" : "off", (unsigned long)stats.sent);
      }
    }
  }
  if (sending.batteryWhy == espnowsensor::PushWhyNot::Sends || sending.batteryWhy == espnowsensor::PushWhyNot::NoSession) {
    if (sending.batteryEverSent) {
      BROADCAST_PRINTF("  Battery state: sent every %u s, %lu sent, last %lu s ago.", (unsigned)sending.batteryIntervalS,
                       (unsigned long)sending.batterySent, (unsigned long)((uint32_t)(nowMs - sending.batteryLastSentMs) / 1000));
    } else {
      BROADCAST_PRINTF("  Battery state: sent every %u s, none sent yet%s.", (unsigned)sending.batteryIntervalS,
                       sending.batteryWhy == espnowsensor::PushWhyNot::NoSession ? " (no session with the master)" : "");
    }
  } else if (sends || sending.batteryWhy != espnowsensor::PushWhyNot::IsMaster) {
    BROADCAST_PRINTF("  Battery state: not sent: %s.", espnowsensor::pushWhyNotText(sending.batteryWhy));
  }
  if (sending.eventsWhy == espnowsensor::PushWhyNot::Sends || sending.eventsWhy == espnowsensor::PushWhyNot::NoSession) {
    BROADCAST_PRINTF("  Events: %s%s are sent, %lu sent, %lu left out%s.", espnowsensor::eventPushKey(sending.eventFloor),
                     sending.eventFloor == espnowsensor::kEventPushVerbose ? " (all)" : " and above",
                     (unsigned long)sending.eventsSent, (unsigned long)sending.eventsLeftOut,
                     sending.eventsWhy == espnowsensor::PushWhyNot::NoSession ? " (no session with the master)" : "");
  } else if (sends || sending.eventsWhy != espnowsensor::PushWhyNot::IsMaster) {
    BROADCAST_PRINTF("  Events: not sent: %s.", espnowsensor::pushWhyNotText(sending.eventsWhy));
  }

  // What this device keeps, one line per sender (its event count for an
  // admin only, as espnowevents; said once for anyone else).
  bool senderListed = false;
  for (int i = 0; i < REMOTE_SENSOR_PEERS_MAX; i++) {
    RemoteSensorPeerSummary summary;
    if (!remoteSensorPeerSummaryAt(i, &summary)) continue;
    senderListed = true;
    char line[200];
    size_t pos = 0;
    for (int k = 0; k < REMOTE_SENSOR_MAX && pos + 24 < sizeof(line); k++) {
      if (!(summary.presentMask & (1u << k))) continue;
      // A sensor listed by the sender's status frame alone has no reading yet.
      const char* state = !(summary.streamingMask & (1u << k)) ? "present"
                        : (summary.freshMask & (1u << k)) ? "fresh"
                        : (summary.readMask & (1u << k)) ? "no reading for 30 s" : "streaming, no reading yet";
      pos += (size_t)snprintf(line + pos, sizeof(line) - pos, "%s%s %s", pos ? ", " : "",
                              sensorTypeToString((RemoteSensorType)k), state);
    }
    if (pos == 0) strlcpy(line, "no sensor heard from in the last minute", sizeof(line));
    char more[96] = "";
    espnowPowerEventsPeerText(summary.mac, summaryVia(summary), more, sizeof(more));
    char who[80];
    strlcpy(who, summary.name[0] ? summary.name : macToDisplayStr(summary.mac).c_str(), sizeof(who));
    if (summary.link == espnowfwd::kLinkForwarded) {
      char viaName[32];
      sensorViaName(summary.via, viaName, sizeof(viaName));
      strlcat(who, " via ", sizeof(who));
      strlcat(who, viaName, sizeof(who));
    }
    BROADCAST_PRINTF("  From %s: %s; %lu readings kept, %lu lost (%lu of them came late)%s%s",
                     who, line,
                     (unsigned long)summary.received, (unsigned long)summary.lost, (unsigned long)summary.late,
                     more[0] ? "; " : "", more);
  }
  if (senderListed && !espnowEventCountsVisible()) broadcastOutput("  Event counts: admin only.");
  const size_t forwardLinks = espnowForwardLinkCount();
  if (forwardLinks) {
    BROADCAST_PRINTF("  Forwarded by other devices: %u link%s (espnowforwardlinks)", (unsigned)forwardLinks,
                     forwardLinks == 1 ? "" : "s");
  }
  const uint32_t refusedAny = totals.refusals.noRecord + totals.refusals.noMemory + totals.refusals.badType +
                              totals.refusals.notReceiver + totals.refusals.malformed + totals.malformed +
                              totals.refusals.forwardRefused;
  if (receives || totals.senders || refusedAny) {
    BROADCAST_PRINTF("  Received: %d of %d senders, %d sensors listed, %lu readings kept, %lu lost (%lu of them came late)",
                     totals.senders, REMOTE_SENSOR_PEERS_MAX, totals.sensors, (unsigned long)totals.received,
                     (unsigned long)totals.lost, (unsigned long)totals.late);
  }
  if (refusedAny) {
    BROADCAST_PRINTF("  Refused: %lu because %d devices already send, %lu for lack of memory, %lu of an unknown "
                     "sensor type, %lu malformed, %lu because of this device's role, %lu forwarded without a link",
                     (unsigned long)totals.refusals.noRecord, REMOTE_SENSOR_PEERS_MAX,
                     (unsigned long)totals.refusals.noMemory, (unsigned long)totals.refusals.badType,
                     (unsigned long)(totals.malformed + totals.refusals.malformed),
                     (unsigned long)totals.refusals.notReceiver, (unsigned long)totals.refusals.forwardRefused);
  }
  if (receives) broadcastOutput("  Battery state and events it received: espnowbattery, espnowevents (admin only).");

  return "OK: sensor status displayed";
}

// Enable/disable all sensor ESP-NOW communication (status + data broadcasts)
const char* cmd_espnow_sensorbroadcast(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Parse: <on|off>  (dispatcher strips "espnow sensorbroadcast" prefix)
  String args = argsInput;
  normalizeCliArg(args);
  
  if (args.length() == 0) {
    // No argument - show current status
    bool enabled = isSensorBroadcastEnabled();
    BROADCAST_PRINTF("[ESP-NOW] Sensor broadcast is %s", enabled ? "ENABLED" : "DISABLED");
    return enabled ? "Sensor broadcast: on" : "Sensor broadcast: off";
  }
  
  if (argIsOn(args)) {
    setSensorBroadcastEnabled(true);
    broadcastOutput("[ESP-NOW] Sensor broadcast ENABLED - status and data will be sent to master");
    return "OK: Sensor broadcast enabled";
  } else if (argIsOff(args)) {
    setSensorBroadcastEnabled(false);
    broadcastOutput("[ESP-NOW] Sensor broadcast DISABLED - no sensor data will be sent");
    return "OK: Sensor broadcast disabled";
  } else {
    return "Error: invalid arguments — Usage: espnow sensorbroadcast <on|off>";
  }
}

// ==========================
// Remote GPS Data Access
// ==========================

#include <ArduinoJson.h>


bool getRemoteGpsData(RemoteGpsData* outData) {
  if (!outData) return false;
  
  memset(outData, 0, sizeof(RemoteGpsData));
  outData->valid = false;

  // Find the most recent fresh GPS reading from any remote device
  RemoteSensorInfo best;
  bool haveBest = false;
  const int slots = remoteSensorSlotCount();
  for (int i = 0; i < slots; i++) {
    RemoteSensorInfo e;
    if (!remoteSensorInfoAt(i, &e)) continue;
    if (!e.fresh || e.sensorType != REMOTE_SENSOR_GPS || e.jsonLength == 0) continue;
    if (!haveBest || e.lastUpdate > best.lastUpdate) {
      best = e;
      haveBest = true;
    }
  }
  if (!haveBest) return false;

  char* text = (char*)ps_alloc(SENSOR_READING_MAX_BYTES + 1, AllocPref::PreferPsram, "espnow.sensor.gps");
  if (!text) return false;
  const size_t textLen = remoteSensorCopyText(best.deviceMac, remoteSensorInfoVia(best), REMOTE_SENSOR_GPS, text,
                                              SENSOR_READING_MAX_BYTES + 1);
  
  // Parse the JSON data: {"valid":true,"connected":true,"ts":..,"fix":1,"quality":1,"sats":8,"lat":37.123,"lon":-122.456,"alt":100.5,"speed":0.5}
  PSRAM_JSON_DOC(doc);
  DeserializationError err = textLen ? deserializeJson(doc, text, textLen)
                                     : DeserializationError(DeserializationError::EmptyInput);
  free(text);
  if (err) {
    return false;
  }
  
  // Extract GPS values
  outData->hasFix = doc[HW1_SK_FIX] | 0;
  outData->fixQuality = doc[HW1_SK_QUALITY] | 0;
  outData->satellites = doc[HW1_SK_SATS] | 0;
  outData->latitude = doc[HW1_SK_LAT] | 0.0f;
  outData->longitude = doc[HW1_SK_LON] | 0.0f;
  outData->altitude = doc[HW1_SK_ALT] | 0.0f;
  outData->speed = doc[HW1_SK_SPEED] | 0.0f;
  outData->lastUpdate = best.lastUpdate;
  strlcpy(outData->deviceName, best.deviceName, sizeof(outData->deviceName));
  
  // Only valid if GPS has a fix
  outData->valid = outData->hasFix;
  
  return outData->valid;
}

#endif // ENABLE_ESPNOW
