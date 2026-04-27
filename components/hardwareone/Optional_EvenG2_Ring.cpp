// =============================================================================
// Even Realities R1 Ring — BLE central (info-only / Path 1)
// =============================================================================
// See Optional_EvenG2_Ring.h for the what-and-why. In short: connect, subscribe
// to notify, dump everything the ring pushes so we can inventory what's
// available without the server-pkey auth dance. No commands are sent to the
// ring — we are strictly a listener in this first implementation.

#include "Optional_EvenG2_Ring.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEAdvertisedDevice.h>

#include "System_Debug.h"
#include "System_Command.h"
#include "System_Utils.h"
#include "System_MemUtil.h"
#include "WebServer_Server.h"  // broadcastEventToAllSessions() for SSE push
#include "System_Settings.h"   // gSettings + setSetting() for MAC persistence
#include "BLE_Events.h"        // CompactJson + blePushEvent
#include "BLE_Peers.h"         // peer registry

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string.h>
#include <ctype.h>

// =============================================================================
// Shared state with the G2 scanner
// =============================================================================
// The glasses' scan callback (Optional_EvenG2.cpp) also classifies EVEN R1_*
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
static void ringPeerDisconnectThunk()   { g2RingDisconnect(); }
static bool ringPeerIsConnectedThunk()  { return g2RingIsConnected(); }
static const BlePeerOps ringPeerOps = {
  ringPeerConnectSavedThunk,
  ringPeerDisconnectThunk,
  ringPeerIsConnectedThunk,
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

// Runtime verbose flag — toggle with `ringverbose on/off` if we ever need
// to silence the byte-dump once we've characterised the stream. Default is
// on because we're still learning what the ring emits.
static bool gRingDumpVerbose = true;

// Async connect-task state (mirrors the glasses' g2ConnectTask pattern to
// keep the CLI handler from blocking on scan+connect).
static volatile bool  gRingConnectTaskActive = false;
static TaskHandle_t   gRingConnectTaskHandle = nullptr;

// Push a `ring-status` SSE to any open browser so the Bluetooth page's
// Ring card flips state without a manual refresh. Fired on every
// meaningful transition: connect-ok, disconnect, scan-found. Compact
// keys because the SSE queue's data field is capped at 128 chars
// (EVENT_DATA_MAX in WebServer_Server.h) — the G2 payload hits ~90
// bytes, so we match that shape.
//
// Schema (client must match parseRingStatusEvent in WebPage_Bluetooth.h):
//   u  = up     (bool)  — BLE link live
//   n  = name   (str)   — advert name ("EVEN R1_BAAC1C")
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

// =============================================================================
// Ring envelope decoding (no-write; receive-only)
// =============================================================================
//
// Reference wire format (ble/ring.ts header comment):
//   [0]      0x00 frame marker
//   [1..4]   4-byte anti-replay hash (random per packet, unique per session)
//   [5]      0x64
//   [6]      seqGroup (0x01 default; 0x02 for some group-addressed cmds)
//   [7]      0x64
//   [8]      seq u8 (monotonic per-session counter)
//   [9..10]  flags u16 BE  0x0000=REQUEST 0x0001=SET 0x0002=PUSH 0x0003=RESPONSE
//   [11]     0x00
//   [12]     cmd
//   [13]     sub
//   [14]     0x00
//   [15..]   payload
//
// We log each incoming packet with a one-line header (decoded fields) and
// a separate hex dump of the payload so the user can pattern-match against
// the command catalogue.

static const char* ringFlagName(uint16_t flags) {
  switch (flags) {
    case G2RING_FLAG_REQUEST:  return "REQ";
    case G2RING_FLAG_SET:      return "SET";
    case G2RING_FLAG_PUSH:     return "PUSH";
    case G2RING_FLAG_RESPONSE: return "RESP";
    default:                   return "?";
  }
}

static const char* ringCmdName(uint8_t cmd) {
  switch (cmd) {
    case G2RING_CMD_HEARTRATE:   return "HEARTRATE";
    case G2RING_CMD_FIRMWARE:    return "FIRMWARE";
    case G2RING_CMD_TEMPERATURE: return "TEMPERATURE";
    case G2RING_CMD_HRV:         return "HRV";
    case G2RING_CMD_ACTIVITY:    return "ACTIVITY";
    case G2RING_CMD_SLEEP:       return "SLEEP";
    case G2RING_CMD_PAIR_AUTH:   return "PAIR_AUTH";
    case G2RING_CMD_HEALTH_SET:  return "HEALTH_SETTING";
    case G2RING_CMD_LINK_G2:     return "LINK_TO_GLASSES";
    case G2RING_CMD_ALGO_KEY:    return "ALGO_KEY";
    case G2RING_CMD_CONFIG1:     return "CONFIG1";
    case G2RING_CMD_CONFIG2:     return "CONFIG2";
    case G2RING_CMD_SERIAL:      return "SERIAL";
    case G2RING_CMD_PHONE_STAT:  return "PHONE_STATUS";
    case G2RING_CMD_PHONE_ACK:   return "PHONE_ACK";
    default:                     return "?";
  }
}

// Decode and print a single ring notify frame. `data` is the raw BLE
// characteristic value (no BLE-side framing — the ring talks straight
// binary per the reference's buildRingPacket).
static void ringDumpFrame(const uint8_t* data, size_t len) {
  if (!data || len == 0) return;
  gRing.packetsReceived++;

  // Header sanity: reference always starts with 0x00, [5]=[7]=0x64, [11]=[14]=0x00.
  // Log header checks so we notice if the firmware diverges from the reference.
  if (len < 15) {
    DEBUG_G2F("[RING] short notify len=%u (<15 hdr) — raw dump follows",
              (unsigned)len);
  } else {
    const uint8_t marker = data[0];
    const uint32_t hash  = ((uint32_t)data[1] << 24) | ((uint32_t)data[2] << 16) |
                           ((uint32_t)data[3] << 8)  |  (uint32_t)data[4];
    const uint8_t s0    = data[5];
    const uint8_t grp   = data[6];
    const uint8_t s1    = data[7];
    const uint8_t seq   = data[8];
    const uint16_t flags = ((uint16_t)data[9] << 8) | data[10];
    const uint8_t pad0  = data[11];
    const uint8_t cmd   = data[12];
    const uint8_t sub   = data[13];
    const uint8_t pad1  = data[14];
    const size_t pLen   = len - 15;
    const bool sane     = (marker == 0x00 && s0 == 0x64 && s1 == 0x64 &&
                           pad0 == 0x00  && pad1 == 0x00);
    DEBUG_G2F("[RING] RX %s len=%u hash=%08lX grp=%02X seq=%u flags=%04X(%s) "
              "cmd=%02X(%s) sub=%02X pLen=%u%s",
              sane ? "OK" : "HDR?!",
              (unsigned)len, (unsigned long)hash,
              grp, seq, flags, ringFlagName(flags),
              cmd, ringCmdName(cmd), sub, (unsigned)pLen,
              sane ? "" : " (header bytes don't match reference)");
  }

  if (!gRingDumpVerbose) return;
  // Full hex dump in rows of 16 bytes. Kept concise by capping at ~48 bytes
  // per notify — anything larger we truncate with "…" so log lines stay
  // bounded. (Typical ring packets are <40 B; health pushes are the
  // largest and top out around 30-ish.)
  const size_t showMax = 64;
  const size_t show = len > showMax ? showMax : len;
  char buf[3 * showMax + 4];
  size_t off = 0;
  for (size_t i = 0; i < show && off + 3 < sizeof(buf); i++) {
    off += snprintf(buf + off, sizeof(buf) - off, "%02X ", data[i]);
  }
  if (off > 0) buf[off - 1] = '\0';
  DEBUG_G2F("[RING] RX bytes=[%s%s]", buf, len > showMax ? " ..." : "");
}

// Notify callback shim — Arduino BLE hands us (char*, data, len, isNotify).
static void ringNotifyThunk(BLERemoteCharacteristic* /*c*/, uint8_t* data,
                            size_t len, bool /*isNotify*/) {
  ringDumpFrame(data, len);
}

// =============================================================================
// Client callbacks
// =============================================================================

class RingClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient* /*c*/) override {
    DEBUG_G2F("[RING] BLE onConnect callback fired");
  }
  void onDisconnect(BLEClient* /*c*/) override {
    const bool wasConnected = gRing.connected;
    DEBUG_G2F("[RING] BLE onDisconnect — connected-was=%d", wasConnected ? 1 : 0);
    gRing.connected   = false;
    gRing.writeChar   = nullptr;
    gRing.notifyChar  = nullptr;
    gRing.clientStale = true;
    if (wasConnected) {
      BROADCAST_PRINTF("[RING] Dropped BLE link — ring is no longer connected");
      ringPushStatusEvent("disconnect");
    }
  }
};

// =============================================================================
// Connect flow
// =============================================================================

// `savedMac`: when non-empty, do a directed connect to that MAC without
// requiring a prior scan-cached gRingAdvertisedDevice. Used by the boot
// auto-reconnect path. When empty, behaves as before (uses the cached
// advertisement from the most recent G2 scan).
static bool ringDoConnect(const String& savedMac = String()) {
  const bool useSavedMac = (savedMac.length() > 0);
  if (!useSavedMac && !gRingAdvertisedDevice) {
    DEBUG_G2F("[RING] connect: no advertisedDevice stashed — run 'g2scan' "
              "(or 'openg2') first with the ring in range, then retry");
    return false;
  }
  if (gRing.connected) {
    DEBUG_G2F("[RING] connect: already connected");
    return true;
  }

  if (useSavedMac) {
    DEBUG_G2F("[RING] Connecting by saved MAC %s (heap=%u)",
              savedMac.c_str(), (unsigned)ESP.getFreeHeap());
  } else {
    DEBUG_G2F("[RING] Connecting to %s @ %s (heap=%u)",
              gRingDeviceName.c_str(), gRingDeviceAddress.c_str(),
              (unsigned)ESP.getFreeHeap());
  }

  // Replace stale client from a previous unexpected drop. Same pattern
  // as the glasses path — Arduino BLE's BLEClient doesn't reliably survive
  // peer-initiated disconnects.
  if (gRing.client && gRing.clientStale) {
    DEBUG_G2F("[RING] Replacing stale BLEClient from prior drop");
    gRing.client      = nullptr;
    gRing.clientStale = false;
  }
  if (!gRing.client) {
    gRing.client = BLEDevice::createClient();
    if (!gRing.client) {
      DEBUG_G2F("[RING] BLEDevice::createClient() returned null");
      return false;
    }
    gRing.client->setClientCallbacks(new RingClientCallbacks());
  }

  const uint32_t t0 = millis();
  bool connOk;
  if (useSavedMac) {
    // Direct address connect — Arduino BLE supports this without a prior
    // advertisement scan. The peer must be advertising and in range.
    BLEAddress addr(savedMac.c_str());
    connOk = gRing.client->connect(addr);
    if (connOk) {
      // Populate the cached descriptors so subsequent disconnect / status
      // paths print sensible names.
      gRingDeviceAddress = savedMac;
      if (gRingDeviceName.length() == 0) gRingDeviceName = "saved-ring";
    }
  } else {
    connOk = gRing.client->connect(gRingAdvertisedDevice);
  }
  if (!connOk) {
    DEBUG_G2F("[RING] BLE connect FAILED after %u ms",
              (unsigned)(millis() - t0));
    return false;
  }
  DEBUG_G2F("[RING] BLE connect OK in %u ms", (unsigned)(millis() - t0));

  // Bump MTU. The HRV init packet is 29 bytes, so a default 23-byte MTU
  // would fragment it; we negotiate higher to keep writes single-packet.
  // (Moot for Path 1 since we never write, but cheap and future-proof.)
  gRing.client->setMTU(64);
  gRing.mtu = gRing.client->getMTU();
  DEBUG_G2F("[RING] MTU negotiated to %u", (unsigned)gRing.mtu);

  DEBUG_G2F("[RING] Looking up service %s", G2RING_SERVICE_UUID);
  BLERemoteService* svc = gRing.client->getService(BLEUUID(G2RING_SERVICE_UUID));
  if (!svc) {
    DEBUG_G2F("[RING] Service %s NOT FOUND (listing all services below)",
              G2RING_SERVICE_UUID);
    auto* services = gRing.client->getServices();
    if (services) {
      for (const auto& entry : *services) {
        DEBUG_G2F("[RING]   svc: %s", entry.first.c_str());
      }
    }
    gRing.client->disconnect();
    return false;
  }
  DEBUG_G2F("[RING] Service found, getting characteristics");

  gRing.writeChar  = svc->getCharacteristic(BLEUUID(G2RING_CHAR_WRITE_UUID));
  gRing.notifyChar = svc->getCharacteristic(BLEUUID(G2RING_CHAR_NOTIFY_UUID));
  DEBUG_G2F("[RING] writeChar=%p notifyChar=%p", gRing.writeChar, gRing.notifyChar);

  if (!gRing.notifyChar) {
    DEBUG_G2F("[RING] Notify char %s NOT FOUND (listing all chars):",
              G2RING_CHAR_NOTIFY_UUID);
    auto* chars = svc->getCharacteristics();
    if (chars) {
      for (const auto& entry : *chars) {
        DEBUG_G2F("[RING]   char: %s", entry.first.c_str());
      }
    }
    gRing.client->disconnect();
    return false;
  }
  DEBUG_G2F("[RING] Chars: write.canWriteNR=%d notify.canNotify=%d canIndicate=%d",
            gRing.writeChar  ? gRing.writeChar->canWriteNoResponse() : 0,
            gRing.notifyChar ? gRing.notifyChar->canNotify()         : 0,
            gRing.notifyChar ? gRing.notifyChar->canIndicate()       : 0);

  if (gRing.notifyChar->canNotify()) {
    DEBUG_G2F("[RING] Subscribing to notifications on %s", G2RING_CHAR_NOTIFY_UUID);
    gRing.notifyChar->registerForNotify(ringNotifyThunk);
  } else {
    DEBUG_G2F("[RING] WARN: notifyChar cannot notify — no incoming data");
  }

  gRing.connected      = true;
  gRing.connectedSince = millis();
  BROADCAST_PRINTF("[RING] Connected to %s (info-only mode — no auth)",
                   gRingDeviceName.c_str());
  ringPushStatusEvent("connect-ok");

  // Persist ring MAC into the BLE peer registry. bleSavePeerMac is a
  // no-op when the value already matches — auto-reconnect is gated
  // separately by the peer's autoConnect flag.
  if (gRingDeviceAddress.length() > 0) {
    bleSavePeerMac(BLE_PEER_R1_RING, gRingDeviceAddress);
  }
  return true;
}

// Background connect task — returns immediately from the CLI path.
static void ringConnectTaskBody(void* /*arg*/) {
  ringDoConnect();
  gRingConnectTaskActive = false;
  gRingConnectTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

// Saved-MAC variant: reads the registered ring MAC and connects directly.
static void ringConnectSavedTaskBody(void* /*arg*/) {
  String mac = gBlePeerData[BLE_PEER_R1_RING].mac1;
  mac.trim();
  if (mac.length() == 0) {
    DEBUG_G2F("[RING] auto-reconnect: no saved MAC — skipping");
  } else {
    ringDoConnect(mac);
  }
  gRingConnectTaskActive = false;
  gRingConnectTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

// =============================================================================
// Public API
// =============================================================================

bool g2RingInit() {
  if (gRing.initialized) return true;
  if (!gRing.writeMutex) {
    gRing.writeMutex = xSemaphoreCreateMutex();
  }
  gRing.initialized = true;

  // Register with the peer registry. ringPeerSpec is a file-static (see
  // top of this file); registration just publishes a stable pointer.
  bleRegisterPeer(ringPeerSpec);

  DEBUG_G2F("[RING] Module initialised (Path 1 info-only mode)");
  return true;
}

bool g2RingConnect() {
  if (!g2RingInit()) return false;
  if (gRingConnectTaskActive) {
    DEBUG_G2F("[RING] Connect task already running");
    return false;
  }
  gRingConnectTaskActive = true;
  BaseType_t rc = xTaskCreate(ringConnectTaskBody, "ring_connect",
                              /*stack*/ 5120, nullptr,
                              /*prio*/  5,    &gRingConnectTaskHandle);
  if (rc != pdPASS) {
    DEBUG_G2F("[RING] xTaskCreate failed (rc=%d)", (int)rc);
    gRingConnectTaskActive = false;
    gRingConnectTaskHandle = nullptr;
    return false;
  }
  return true;
}

bool g2RingConnectSaved() {
  if (!g2RingInit()) return false;
  if (gRingConnectTaskActive) {
    DEBUG_G2F("[RING] Connect task already running");
    return false;
  }
  if (gBlePeerData[BLE_PEER_R1_RING].mac1.length() == 0) {
    DEBUG_G2F("[RING] g2RingConnectSaved: no saved MAC, skipping");
    return false;
  }
  gRingConnectTaskActive = true;
  BaseType_t rc = xTaskCreate(ringConnectSavedTaskBody, "ring_reconnect",
                              /*stack*/ 5120, nullptr,
                              /*prio*/  5,    &gRingConnectTaskHandle);
  if (rc != pdPASS) {
    DEBUG_G2F("[RING] xTaskCreate (saved) failed (rc=%d)", (int)rc);
    gRingConnectTaskActive = false;
    gRingConnectTaskHandle = nullptr;
    return false;
  }
  return true;
}

void g2RingDisconnect() {
  if (gRing.client && gRing.client->isConnected()) {
    DEBUG_G2F("[RING] Disconnecting");
    gRing.client->disconnect();
  }
  gRing.connected = false;
}

bool g2RingIsConnected() {
  return gRing.connected;
}

void g2RingGetStatus(char* buf, size_t cap) {
  if (!buf || cap == 0) return;
  const uint32_t nowMs = millis();
  const uint32_t upMs  = gRing.connected && gRing.connectedSince
                         ? (nowMs - gRing.connectedSince) : 0;
  snprintf(buf, cap,
           "ring=%s name='%s' addr=%s mtu=%u rx=%lu up=%u.%03us (scan=%s)",
           gRing.connected ? "up" : "down",
           gRingDeviceName.length() ? gRingDeviceName.c_str() : "<unknown>",
           gRingDeviceAddress.length() ? gRingDeviceAddress.c_str() : "--",
           (unsigned)gRing.mtu,
           (unsigned long)gRing.packetsReceived,
           (unsigned)(upMs / 1000), (unsigned)(upMs % 1000),
           gRingScanFound ? "found" : "not-found");
}

// =============================================================================
// CLI commands
// =============================================================================

static const char* cmd_ringstatus(const String& /*args*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char buf[256];
  g2RingGetStatus(buf, sizeof(buf));
  return buf;
}

static const char* cmd_ringconnect(const String& /*args*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!g2RingConnect()) {
    return gRing.connected ? "RING: already connected"
                           : "RING: connect failed (see log; run 'g2scan' first?)";
  }
  return "RING: connect task started — use ringstatus to watch";
}

static const char* cmd_ringdisconnect(const String& /*args*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  g2RingDisconnect();
  return "RING: disconnect requested";
}

static const char* cmd_ringverbose(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(args);
  String a = ca.arg(0); a.toLowerCase();
  if (a == "on")        gRingDumpVerbose = true;
  else if (a == "off")  gRingDumpVerbose = false;
  else                  gRingDumpVerbose = !gRingDumpVerbose;
  return gRingDumpVerbose ? "RING verbose: on (full hex dump of every notify)"
                          : "RING verbose: off (header decode only)";
}

extern const CommandEntry g2RingCommands[] = {
  { "ringstatus",     "Show R1 ring connection status",            false, cmd_ringstatus     },
  { "ringconnect",    "Connect to a scanned R1 ring (ringstatus after)", false, cmd_ringconnect    },
  { "ringdisconnect", "Disconnect from the R1 ring",               false, cmd_ringdisconnect },
  { "ringverbose",    "Toggle full hex dump of ring notify frames", false, cmd_ringverbose    },
};
extern const size_t g2RingCommandsCount =
    sizeof(g2RingCommands) / sizeof(g2RingCommands[0]);

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
