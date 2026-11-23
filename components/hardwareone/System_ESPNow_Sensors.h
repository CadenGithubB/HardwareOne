#ifndef SYSTEM_ESPNOW_SENSORS_H
#define SYSTEM_ESPNOW_SENSORS_H

#include "System_BuildConfig.h"

// True only when the ESP-NOW radio has been initialized at runtime (i.e. the
// user clicked Initialize). ENABLE_ESPNOW merely means the feature is compiled
// in. Lets the web sensors page tell "not initialized" apart from "active".
// Always false when ESP-NOW is compiled out (System_Utils.cpp).
bool isEspNowInitialized();

#if ENABLE_ESPNOW

#include <Arduino.h>
#include <stdint.h>

#include "System_ESPNow_SensorCore.h"  // RemoteSensorType (the one numbering) and the reading rules
#include "System_ESPNow_ForwardCore.h"  // espnowfwd::kLink* of a record

// ==========================
// Readings received from other devices
// ==========================
//
// A receiver (a mesh master or backup master, or a bond master) keeps the
// latest reading of each sensor of each sender, with its own arrival time
// (UTC epoch when its clock is set, and its uptime), and counts per sender
// and sensor what was received, lost, late and restarted
// (System_ESPNow_SensorCore.h has the rules). Each paired sender has a record
// with one slot per sensor type, so the table is full when sixteen paired
// devices send, and never because one of them has many sensors. The table
// lives in System_ESPNow_Sensors.cpp and is reached through the functions
// below: every reader gets a copy taken under the table's lock, and no reader
// changes the table.

// The paired senders the receiver keeps readings of (the paired-device limit).
#define REMOTE_SENSOR_PEERS_MAX 16
// The slots of the table, one per sender and sensor type: sizes the index
// arrays of the OLED and G2 lists. remoteSensorSlotCount() is this number
// once the table exists, and 0 before.
#define REMOTE_SENSOR_SLOTS_MAX (REMOTE_SENSOR_PEERS_MAX * REMOTE_SENSOR_MAX)
static_assert(REMOTE_SENSOR_SLOTS_MAX <= 255, "slot indices are kept in uint8_t arrays");

#define REMOTE_SENSOR_TTL_MS 30000  // a reading older than this is not fresh
#define REMOTE_SENSOR_PRESENCE_TTL_MS 60000  // a sensor not heard from for this long is no longer listed

// One slot without its text: small enough to copy onto a stack. A slot
// belongs to a sender: its origin (deviceMac) and, for readings a bond master
// forwards for its partner, that bond master (viaMac). Every reader that
// copies a slot's text or looks a slot up again passes the via it listed
// (remoteSensorInfoVia), so a forwarded reading never shows as the origin's
// own.
struct RemoteSensorInfo {
  uint8_t deviceMac[6];
  uint8_t viaMac[6];        // all zero, or the bond master that forwards for deviceMac
  uint8_t link;             // espnowfwd::kLinkMesh, kLinkBond or kLinkForwarded
  char deviceName[32];
  RemoteSensorType sensorType;
  bool enabled;             // the sender says it is streaming this sensor
  bool fresh;               // a reading arrived within REMOTE_SENSOR_TTL_MS
  uint16_t jsonLength;      // length of the latest reading's text (0: none yet)
  uint32_t lastUpdate;      // this device's uptime (ms) when the latest reading arrived
  uint32_t lastSeen;        // ... when a reading or a status frame last arrived
  int64_t arrivalEpochMs;   // this device's UTC clock at that arrival; 0: the clock was not set
  uint8_t arrivalClock;     // Clock::SyncSource of this device at that arrival
  uint16_t counter;         // the sender's counter of the latest reading
};

// Number of slots in the table (0 while this device keeps no readings).
int remoteSensorSlotCount();
// The slot at `index`. False for a free slot and for one whose sensor has not
// been heard from for REMOTE_SENSOR_PRESENCE_TTL_MS.
bool remoteSensorInfoAt(int index, RemoteSensorInfo* out);
// The via of a listed slot: nullptr for the origin's own record.
inline const uint8_t* remoteSensorInfoVia(const RemoteSensorInfo& info) {
  return info.link == espnowfwd::kLinkForwarded ? info.viaMac : nullptr;
}
// The slot of one sensor of one sender (origin, via; via nullptr: the
// origin's own record), by the same rule.
bool remoteSensorInfoFor(const uint8_t* deviceMac, const uint8_t* via, RemoteSensorType sensorType,
                         RemoteSensorInfo* out);
// Copies the latest reading's text of that sensor into `out`, terminated.
// Returns its length; 0 when there is none or `out` is too small for it.
size_t remoteSensorCopyText(const uint8_t* deviceMac, const uint8_t* via, RemoteSensorType sensorType, char* out,
                            size_t outSize);

// ==========================
// Remote Sensor Functions
// ==========================

// Called at ESP-NOW start: forgets the readings listed so far and readies the
// sender's broadcaster state. Allocates nothing.
bool initRemoteSensorSystem();
// The one place the receiver's table (the per-sender records) is made
// (PSRAM). Called by the ingest for the first reading or status frame this
// device keeps, so only a device that is sent readings and keeps them has
// one. True when it exists. Safe to call again.
bool remoteSensorTableEnsure();

// ---- Receive (called by the frame handlers on espnow_task; the receive table
// has already required a paired sender and a session-encrypted frame) ----
// One part of a reading: SENSOR_ENVELOPE, or BOND_SENSOR_DATA (fromBond).
void remoteSensorIngestPart(const uint8_t* senderMac, const char* senderName,
                            const uint8_t* payload, size_t payloadLen, bool fromBond);
// SENSOR_STATUS: which sensors the sender has and is streaming (or, with the
// link byte set, which ones its bond partner has: filed under the partner via
// the sender). It may make the table and the sender's record (a full table
// and no memory are refused as for a reading); it never changes a reading,
// its freshness or its counts.
void remoteSensorIngestStatus(const uint8_t* senderMac, const char* senderName,
                              const uint8_t* payload, size_t payloadLen);
// BOND_SENSOR_STATUS: the bond partner's sensor status over the bond link (the
// receive table's REQ_BOND_PEER). Kept whatever the mesh role, as a reading
// over the bond link is, and handed to the forwarder.
void remoteSensorIngestBondStatus(const uint8_t* senderMac, const char* senderName,
                                  const uint8_t* payload, size_t payloadLen);

// ---- Send ----
// Sends one reading to `dst` now, in 1 to 4 frames (BOND_SENSOR_DATA when
// viaBond). False with *why set when it was not sent. Any task but the radio
// callback.
bool sensorSendReadingTo(const uint8_t dst[6], bool viaBond, RemoteSensorType sensorType,
                         const char* text, size_t len, espnowsensor::DropReason* why);
// espnow_task loop: looks up the masters named on this device, starts a
// session with the device readings go to when it has none, and prints why
// readings are not being sent (one line a minute per sensor).
void sensorLinkTick(uint32_t nowMs);

// The masters named on this device (espnowmasterfingerprint,
// espnowbackupfingerprint) that are paired with it, as addresses. They are
// where a stream switched on by hand, the battery state and the events of
// this device go. A copy; any task.
struct SensorReceiverTargets {
  bool masterNamed;     // a primary master fingerprint is set (well formed)
  bool backupNamed;
  bool havePrimary;     // ... and that device is paired: `primary` is its address
  bool haveBackup;
  uint8_t primary[6];
  uint8_t backup[6];
};
void sensorReceiverTargets(SensorReceiverTargets* out);
// Looks them up again (espnow_task only; sensorLinkTick does it once a second).
void sensorReceiverTargetsRefresh();
// The boot tag of every reading, status, battery and event frame this device
// sends in this boot (the low 16 bits of the boot counter).
uint16_t espnowSensorBootTag();
// True when this firmware can read `sensorType` and send its readings.
bool sensorTypeInThisBuild(RemoteSensorType sensorType);
// A local sensor started or stopped: the receiver of this device's readings
// is told at once (the whole SENSOR_STATUS frame goes out; the arguments say
// what changed). Nothing is sent without a receiver.
void sensorPresenceChanged(RemoteSensorType sensorType, bool present);

// Start streaming sensor data (worker → master, opt-in)
void startSensorDataStreaming(RemoteSensorType sensorType);

// The same start with its result: true when this call took effect (a bond
// master: the request to the worker was queued).
bool tryStartSensorDataStreaming(RemoteSensorType sensorType);

// Stop streaming sensor data
void stopSensorDataStreaming(RemoteSensorType sensorType);

// closeespnow lifecycle hook: reject new local stream starts, clear all worker
// and bond-master stream/lease state without emitting radio messages, join the
// broadcaster task, and release its shared PSRAM buffer. Retry-safe on timeout.
bool shutdownSensorDataStreamingForEspNowClose();

bool isSensorDataStreamingEnabled(RemoteSensorType sensorType);

// True when this build has data to send for the sensor: the broadcaster skips
// a sensor without a data builder, so a stream of it would never send.
bool sensorCanStream(RemoteSensorType sensorType);

// The command reply for a sensor that fails sensorCanStream: the refusal with
// the sensors that do stream in this build.
const char* sensorCannotStreamReply();

// ---- Secure sensor fetcher (docs/ESPNOW_SENSOR_FETCHER_DESIGN.md) ----
// Sensor-request modes carried in V4PayloadSensorReq.mode.
enum SensorReqMode : uint8_t {
  SENSOR_REQ_SUBSCRIBE   = 0,  // start streaming {mask} under a lease
  SENSOR_REQ_UNSUBSCRIBE = 1,  // stop streaming {mask}
  SENSOR_REQ_ONESHOT     = 2,  // one immediate push per {mask} bit, short self-expiring lease
};

// True iff `mac` is a securely-paired peer whose Ed25519 fingerprint matches this
// worker's configured master OR backup-master fingerprint. This is the SOLE on-wire
// authorization for sensor control. Empty/malformed configured fingerprints never match.
bool espnowSensorControlAuthorized(const uint8_t mac[6]);
// The same test with its answer: 0 = not a named master, 1 = the primary
// master named on this device, 2 = the backup.
int espnowNamedMasterKind(const uint8_t mac[6]);
// True when a master or backup fingerprint is set on this device (well formed).
bool espnowHasNamedMaster();

// Apply a (previously authorized) SENSOR_REQ off the RX critical path: start/stop
// streaming per mask, stamp the lease, and record the controller MAC as the
// encrypted-unicast reply target. Called from the espnow_task super-loop (block 9f3).
void espnowApplySensorSubscription(uint8_t mode, uint32_t sensorMask,
                                   uint32_t intervalMs, uint32_t leaseMs,
                                   const uint8_t* controllerMac);

// Unpair / revoke: the sensor leases `mac` holds as controller end now
// (no SENSOR_STATUS is sent to it). True when it was the controller. cmd_exec.
bool espnowSensorLeasesDropFor(const uint8_t mac[6]);

// Before that apply (block 9f3): a subscribe or one-shot keeps only the
// sensors this device has running (what its SENSOR_STATUS lists,
// espnowsensor::sensorReqSplit). The others are counted per sensor
// (espnowsensorstatus) and said once a minute. Returns the mask to apply.
uint32_t espnowSensorReqKeepPresent(uint8_t mode, uint32_t sensorMask, const uint8_t* controllerMac);

// Get remote sensor data for web API (via nullptr: the device's own record).
String getRemoteSensorDataJson(const uint8_t* deviceMac, const uint8_t* via, RemoteSensorType sensorType);

// Format-agnostic readable renderer for a cached sensor's JSON. Walks the
// top-level key:value pairs and writes up to `maxLines` newline-separated
// "key: value" lines into `out` (skipping bookkeeping keys like ts/seq/valid).
// Shared by the OLED Remote-Sensors page (and later the G2 lens) so neither
// hard-codes a per-sensor schema — new sensors / key changes render automatically.
// Returns the number of lines written (>=1; writes a short placeholder on error).
int formatRemoteSensorReadable(const char* json, char* out, size_t outSize, int maxLines);

// Get list of all remote devices with sensors
String getRemoteDevicesListJson();

// Helper: Convert sensor type to string
const char* sensorTypeToString(RemoteSensorType type);

// Helper: Convert string to sensor type; an unknown name gives the thermal type.
RemoteSensorType stringToSensorType(const char* str);
// The same parse with its verdict: false for a name that is not a sensor type.
bool sensorTypeFromString(const char* str, RemoteSensorType& out);

#if ENABLE_BONDED_MODE
// The saved "stream this sensor to the bonded peer" setting of a sensor type,
// or nullptr for a type that has none (camera, microphone, APDS).
bool* bondStreamSetting(RemoteSensorType type);
#endif

// ==========================
// Sensor Broadcast Control
// ==========================

// Enable/disable all sensor ESP-NOW communication (status + data)
void setSensorBroadcastEnabled(bool enabled);
bool isSensorBroadcastEnabled();


// ==========================
// Thermal Data Optimization
// ==========================

// Build thermal data JSON with integer values (no decimals) for remote streaming
// Returns length of JSON string written to buf
int buildThermalDataJsonInteger(char* buf, size_t bufSize);

// ==========================
// Remote GPS Data Access
// ==========================

// Structure for accessing remote GPS data
struct RemoteGpsData {
  bool valid;           // Data is valid and not expired
  bool hasFix;          // GPS has valid fix
  int fixQuality;       // Fix quality (0=invalid, 1=GPS, 2=DGPS)
  int satellites;       // Number of satellites
  float latitude;       // Latitude in degrees (negative = South)
  float longitude;      // Longitude in degrees (negative = West)
  float altitude;       // Altitude in meters
  float speed;          // Speed in knots
  unsigned long lastUpdate; // When data was last received
  char deviceName[32];  // Name of device providing GPS
};

// Get remote GPS data from bonded device or mesh workers
// Returns true if valid GPS data is available from a remote source
bool getRemoteGpsData(RemoteGpsData* outData);

const char* cmd_espnow_sensorbroadcast(const String& argsInput);
const char* cmd_espnow_sensorstatus(const String& argsInput);
const char* cmd_espnow_sensorstream(const String& argsInput);

#endif // ENABLE_ESPNOW
#endif // SYSTEM_ESPNOW_SENSORS_H
