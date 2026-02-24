// =============================================================================
// EVEN REALITIES G2 GLASSES - BLE CLIENT IMPLEMENTATION
// =============================================================================

#include "Optional_EvenG2.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include "Optional_Bluetooth.h"
#include "System_Debug.h"
#include "System_Command.h"
#include "System_Notifications.h"
#include <cstring>
#include <ctime>

// =============================================================================
// GLOBALS
// =============================================================================

static G2ClientState* gG2State = nullptr;
static BLEClient* pG2Client = nullptr;
static BLEScan* pG2Scan = nullptr;
static BLERemoteCharacteristic* pG2WriteChar = nullptr;
static BLERemoteCharacteristic* pG2NotifyChar = nullptr;
static BLEAdvertisedDevice* pG2FoundDevice = nullptr;

// Scan result storage
static bool gScanComplete = false;
static String gFoundDeviceName;
static String gFoundDeviceAddress;

// =============================================================================
// CRC-16/CCITT IMPLEMENTATION
// =============================================================================

uint16_t g2CalcCRC16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;  // Init value
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int j = 0; j < 8; j++) {
      if (crc & 0x8000) {
        crc = (crc << 1) ^ 0x1021;  // Polynomial
      } else {
        crc <<= 1;
      }
      crc &= 0xFFFF;
    }
  }
  return crc;
}

// =============================================================================
// VARINT ENCODING (protobuf-style)
// =============================================================================

size_t g2EncodeVarint(uint32_t value, uint8_t* buffer) {
  size_t pos = 0;
  while (value > 0x7F) {
    buffer[pos++] = (value & 0x7F) | 0x80;
    value >>= 7;
  }
  buffer[pos++] = value & 0x7F;
  return pos;
}

// =============================================================================
// PACKET BUILDING
// =============================================================================

static uint8_t gPacketBuffer[600];  // Max packet size with MTU 512

bool g2SendPacket(uint8_t serviceHi, uint8_t serviceLo, const uint8_t* payload, size_t payloadLen) {
  if (!gG2State || gG2State->state != G2_STATE_CONNECTED || !pG2WriteChar) {
    return false;
  }
  
  if (payloadLen > 500) {
    DEBUG_G2F("[G2] Payload too large");
    return false;
  }
  
  // Build header (8 bytes)
  gPacketBuffer[0] = G2_PACKET_MAGIC;      // 0xAA
  gPacketBuffer[1] = G2_PACKET_TYPE_CMD;   // 0x21
  gPacketBuffer[2] = gG2State->seqNumber++; // Sequence
  gPacketBuffer[3] = payloadLen + 2;        // Length (payload + CRC)
  gPacketBuffer[4] = 0x01;                  // Packet total
  gPacketBuffer[5] = 0x01;                  // Packet serial
  gPacketBuffer[6] = serviceHi;
  gPacketBuffer[7] = serviceLo;
  
  // Copy payload
  memcpy(gPacketBuffer + 8, payload, payloadLen);
  
  // Calculate CRC over payload only
  uint16_t crc = g2CalcCRC16(payload, payloadLen);
  
  // Append CRC (little-endian)
  gPacketBuffer[8 + payloadLen] = crc & 0xFF;
  gPacketBuffer[8 + payloadLen + 1] = (crc >> 8) & 0xFF;
  
  size_t totalLen = 8 + payloadLen + 2;
  
  // Write without response
  pG2WriteChar->writeValue(gPacketBuffer, totalLen, false);
  
  gG2State->packetsSent++;
  return true;
}

// Send a raw pre-built packet (for auth sequence)
static bool g2SendRawPacket(const uint8_t* packet, size_t len) {
  if (!pG2WriteChar) return false;
  pG2WriteChar->writeValue((uint8_t*)packet, len, false);
  if (gG2State) gG2State->packetsSent++;
  return true;
}

// =============================================================================
// AUTH HANDSHAKE (7-packet sequence)
// =============================================================================

static bool g2RunAuthHandshake() {
  if (!gG2State || !pG2WriteChar) {
    DEBUG_G2F("[G2-AUTH] ERROR: State or write char is null");
    return false;
  }
  
  gG2State->state = G2_STATE_AUTHENTICATING;
  gG2State->authAttempts++;
  
  DEBUG_G2F("[G2-AUTH] Starting 7-packet authentication handshake...");
  DEBUG_G2F("[G2-AUTH] Attempt #%d", gG2State->authAttempts);
  
  uint32_t timestamp = (uint32_t)time(nullptr);
  uint8_t tsVarint[6];
  size_t tsLen = g2EncodeVarint(timestamp, tsVarint);
  
  DEBUG_G2F("[G2-AUTH] Using timestamp: %lu (varint len: %d)", (unsigned long)timestamp, (int)tsLen);
  
  // Transaction ID (fixed pattern from protocol)
  static const uint8_t txid[] = {0xE8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};
  
  // Build and send auth packets
  uint8_t pkt[64];
  size_t pktLen;
  
  // Auth 1: Capability query
  DEBUG_G2F("[G2-AUTH] Sending packet 1/7 (capability query)...");
  {
    static const uint8_t auth1[] = {
      0xAA, 0x21, 0x01, 0x0C, 0x01, 0x01, 0x80, 0x00,
      0x08, 0x04, 0x10, 0x0C, 0x1A, 0x04, 0x08, 0x01, 0x10, 0x04
    };
    uint16_t crc = g2CalcCRC16(auth1 + 8, 10);
    memcpy(pkt, auth1, 18);
    pkt[18] = crc & 0xFF;
    pkt[19] = (crc >> 8) & 0xFF;
    g2SendRawPacket(pkt, 20);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  
  // Auth 2: Capability response request
  DEBUG_G2F("[G2-AUTH] Sending packet 2/7 (capability response)...");
  {
    static const uint8_t auth2[] = {
      0xAA, 0x21, 0x02, 0x0A, 0x01, 0x01, 0x80, 0x20,
      0x08, 0x05, 0x10, 0x0E, 0x22, 0x02, 0x08, 0x02
    };
    uint16_t crc = g2CalcCRC16(auth2 + 8, 8);
    memcpy(pkt, auth2, 16);
    pkt[16] = crc & 0xFF;
    pkt[17] = (crc >> 8) & 0xFF;
    g2SendRawPacket(pkt, 18);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  
  // Auth 3: Time sync with transaction ID
  DEBUG_G2F("[G2-AUTH] Sending packet 3/7 (time sync)...");
  {
    uint8_t payload[32];
    size_t pos = 0;
    payload[pos++] = 0x08; payload[pos++] = 0x80; payload[pos++] = 0x01;
    payload[pos++] = 0x10; payload[pos++] = 0x0F;
    payload[pos++] = 0x82; payload[pos++] = 0x08; payload[pos++] = 0x11;
    payload[pos++] = 0x08;
    memcpy(payload + pos, tsVarint, tsLen); pos += tsLen;
    payload[pos++] = 0x10;
    memcpy(payload + pos, txid, sizeof(txid)); pos += sizeof(txid);
    
    pkt[0] = 0xAA; pkt[1] = 0x21; pkt[2] = 0x03;
    pkt[3] = pos + 2; pkt[4] = 0x01; pkt[5] = 0x01;
    pkt[6] = 0x80; pkt[7] = 0x20;
    memcpy(pkt + 8, payload, pos);
    uint16_t crc = g2CalcCRC16(payload, pos);
    pkt[8 + pos] = crc & 0xFF;
    pkt[8 + pos + 1] = (crc >> 8) & 0xFF;
    g2SendRawPacket(pkt, 8 + pos + 2);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  
  // Auth 4-5: Additional capability exchanges
  DEBUG_G2F("[G2-AUTH] Sending packet 4/7 (capability exchange)...");
  {
    static const uint8_t auth4[] = {
      0xAA, 0x21, 0x04, 0x0C, 0x01, 0x01, 0x80, 0x00,
      0x08, 0x04, 0x10, 0x10, 0x1A, 0x04, 0x08, 0x01, 0x10, 0x04
    };
    uint16_t crc = g2CalcCRC16(auth4 + 8, 10);
    memcpy(pkt, auth4, 18);
    pkt[18] = crc & 0xFF;
    pkt[19] = (crc >> 8) & 0xFF;
    g2SendRawPacket(pkt, 20);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  
  DEBUG_G2F("[G2-AUTH] Sending packet 5/7 (capability exchange)...");
  {
    static const uint8_t auth5[] = {
      0xAA, 0x21, 0x05, 0x0C, 0x01, 0x01, 0x80, 0x00,
      0x08, 0x04, 0x10, 0x11, 0x1A, 0x04, 0x08, 0x01, 0x10, 0x04
    };
    uint16_t crc = g2CalcCRC16(auth5 + 8, 10);
    memcpy(pkt, auth5, 18);
    pkt[18] = crc & 0xFF;
    pkt[19] = (crc >> 8) & 0xFF;
    g2SendRawPacket(pkt, 20);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  
  // Auth 6: Final capability
  DEBUG_G2F("[G2-AUTH] Sending packet 6/7 (final capability)...");
  {
    static const uint8_t auth6[] = {
      0xAA, 0x21, 0x06, 0x0A, 0x01, 0x01, 0x80, 0x20,
      0x08, 0x05, 0x10, 0x12, 0x22, 0x02, 0x08, 0x01
    };
    uint16_t crc = g2CalcCRC16(auth6 + 8, 8);
    memcpy(pkt, auth6, 16);
    pkt[16] = crc & 0xFF;
    pkt[17] = (crc >> 8) & 0xFF;
    g2SendRawPacket(pkt, 18);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  
  // Auth 7: Final time sync
  DEBUG_G2F("[G2-AUTH] Sending packet 7/7 (final time sync)...");
  {
    uint8_t payload[32];
    size_t pos = 0;
    payload[pos++] = 0x08; payload[pos++] = 0x80; payload[pos++] = 0x01;
    payload[pos++] = 0x10; payload[pos++] = 0x13;
    payload[pos++] = 0x82; payload[pos++] = 0x08; payload[pos++] = 0x11;
    payload[pos++] = 0x08;
    memcpy(payload + pos, tsVarint, tsLen); pos += tsLen;
    payload[pos++] = 0x10;
    memcpy(payload + pos, txid, sizeof(txid)); pos += sizeof(txid);
    
    pkt[0] = 0xAA; pkt[1] = 0x21; pkt[2] = 0x07;
    pkt[3] = pos + 2; pkt[4] = 0x01; pkt[5] = 0x01;
    pkt[6] = 0x80; pkt[7] = 0x20;
    memcpy(pkt + 8, payload, pos);
    uint16_t crc = g2CalcCRC16(payload, pos);
    pkt[8 + pos] = crc & 0xFF;
    pkt[8 + pos + 1] = (crc >> 8) & 0xFF;
    g2SendRawPacket(pkt, 8 + pos + 2);
  }
  
  DEBUG_G2F("[G2-AUTH] Waiting 500ms for glasses to process...");
  vTaskDelay(pdMS_TO_TICKS(500));  // Wait for glasses to process
  
  gG2State->state = G2_STATE_CONNECTED;
  gG2State->seqNumber = 0x08;  // Continue from auth sequence
  gG2State->msgId = 0x14;      // Continue from auth sequence
  
  DEBUG_G2F("[G2-AUTH] === HANDSHAKE COMPLETE ===");
  DEBUG_G2F("[G2-AUTH] Packets sent: %lu", (unsigned long)gG2State->packetsSent);
  return true;
}

// =============================================================================
// NOTIFICATION HANDLER & GESTURE DECODING
// =============================================================================

// Known service IDs for input events (based on protocol research)
#define G2_SVC_INPUT_HI       0x04
#define G2_SVC_INPUT_LO       0x20
#define G2_SVC_TOUCH_HI       0x05
#define G2_SVC_TOUCH_LO       0x20

// Touch event payload field positions (to be refined with real data)
#define G2_TOUCH_TYPE_OFFSET  8   // Offset in payload where touch type appears
#define G2_TOUCH_DIR_OFFSET   9   // Offset for direction byte

// Verbose logging flag for protocol research
static bool gG2VerboseLog = true;

// Decode touch/gesture from payload
static G2EventType g2DecodeGesture(const uint8_t* payload, size_t len) {
  // This decoding is based on protocol research and will need refinement
  // with actual captured packets from the glasses
  
  if (len < 4) return G2_EVENT_UNKNOWN;
  
  // Look for known patterns in the payload
  // Pattern matching based on reverse-engineered protocol
  
  // Check for swipe patterns (field 0x08 followed by direction code)
  for (size_t i = 0; i < len - 2; i++) {
    if (payload[i] == 0x08) {
      uint8_t val = payload[i + 1];
      switch (val) {
        case 0x01: return G2_EVENT_SWIPE_UP;
        case 0x02: return G2_EVENT_SWIPE_DOWN;
        case 0x03: return G2_EVENT_SWIPE_LEFT;
        case 0x04: return G2_EVENT_SWIPE_RIGHT;
        case 0x05: return G2_EVENT_TAP;
        case 0x06: return G2_EVENT_LONG_PRESS;
        case 0x07: return G2_EVENT_DOUBLE_TAP;
      }
    }
  }
  
  // Alternative pattern: look for touch service response
  // These patterns will be refined once real data is captured
  if (len >= 3) {
    // Check for simple gesture codes at start of payload
    if (payload[0] == 0x10) {
      switch (payload[1]) {
        case 0x01: return G2_EVENT_TAP;
        case 0x02: return G2_EVENT_LONG_PRESS;
        case 0x03: return G2_EVENT_DOUBLE_TAP;
      }
    }
    if (payload[0] == 0x18) {
      switch (payload[1]) {
        case 0x01: return G2_EVENT_SWIPE_UP;
        case 0x02: return G2_EVENT_SWIPE_DOWN;
        case 0x03: return G2_EVENT_SWIPE_LEFT;
        case 0x04: return G2_EVENT_SWIPE_RIGHT;
      }
    }
  }
  
  return G2_EVENT_UNKNOWN;
}

static const char* g2EventTypeToString(G2EventType event) {
  switch (event) {
    case G2_EVENT_SWIPE_UP:    return "SWIPE_UP";
    case G2_EVENT_SWIPE_DOWN:  return "SWIPE_DOWN";
    case G2_EVENT_SWIPE_LEFT:  return "SWIPE_LEFT";
    case G2_EVENT_SWIPE_RIGHT: return "SWIPE_RIGHT";
    case G2_EVENT_TAP:         return "TAP";
    case G2_EVENT_LONG_PRESS:  return "LONG_PRESS";
    case G2_EVENT_DOUBLE_TAP:  return "DOUBLE_TAP";
    default:                   return "UNKNOWN";
  }
}

static void g2NotifyCallback(BLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  if (!gG2State) return;
  
  gG2State->packetsReceived++;
  
  // Parse packet header
  if (length < 10 || pData[0] != G2_PACKET_MAGIC) {
    if (gG2VerboseLog) {
      DEBUG_G2F("[G2] RX invalid (%d bytes)", length);
    }
    return;
  }
  
  uint8_t type = pData[1];
  uint8_t seq = pData[2];
  uint8_t payloadLen = pData[3];
  uint8_t serviceHi = pData[6];
  uint8_t serviceLo = pData[7];
  
  // Log packet for debugging/research
  if (gG2VerboseLog) {
    char hexBuf[80];
    int pos = 0;
    size_t showBytes = (length < 24) ? length : 24;
    for (size_t i = 8; i < showBytes && pos < (int)sizeof(hexBuf) - 4; i++) {
      pos += snprintf(hexBuf + pos, sizeof(hexBuf) - pos, "%02X ", pData[i]);
    }
    if (length > 24 && pos < (int)sizeof(hexBuf) - 4) pos += snprintf(hexBuf + pos, sizeof(hexBuf) - pos, "...");
    hexBuf[pos] = '\0';
    DEBUG_G2F("[G2] RX Svc:%02X-%02X Typ:%02X Seq:%02X Len:%d | %s", 
              serviceHi, serviceLo, type, seq, payloadLen, hexBuf);
  }
  
  // Check if this is an input/touch service
  bool isInputService = (serviceHi == G2_SVC_INPUT_HI && serviceLo == G2_SVC_INPUT_LO) ||
                        (serviceHi == G2_SVC_TOUCH_HI && serviceLo == G2_SVC_TOUCH_LO);
  
  if (isInputService || type == G2_PACKET_TYPE_RSP) {
    // Try to decode gesture from payload
    const uint8_t* payload = pData + 8;
    size_t pLen = (payloadLen > 2) ? payloadLen - 2 : 0;  // Exclude CRC
    
    G2EventType event = g2DecodeGesture(payload, pLen);
    
    if (event != G2_EVENT_UNKNOWN) {
      // NOTE: This callback runs on BLE notify task - defer heavy operations (ISR-safe pattern)
      // Just store event for deferred processing in g2Tick()
      gG2State->deferredGestureEvent = event;
      gG2State->deferredGesturePending = true;
      
      // Fire callback immediately (user callback should be ISR-safe)
      if (gG2State->eventCallback) {
        gG2State->eventCallback(event);
      }
    }
  }
}

// =============================================================================
// SCAN CALLBACK
// =============================================================================

class G2AdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  G2Eye targetEye;
public:
  G2AdvertisedDeviceCallbacks(G2Eye eye) : targetEye(eye) {}
  
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    String name = advertisedDevice.getName().c_str();
    
    // Check if this is an Even G2 device
    // Known patterns: "Even G2_32_L_XXXXXX" (left), "Even G2_32_R_XXXXXX" (right)
    // Also accept "G2" anywhere for flexibility
    bool isEvenG2 = name.startsWith("Even G2") || name.indexOf("G2_") != -1;
    if (!isEvenG2) return;
    
    DEBUG_G2F("[G2] Found device: %s (RSSI: %d)", name.c_str(), advertisedDevice.getRSSI());
    broadcastOutput("[G2] Found: " + name);
    
    // Check for left/right based on target
    bool isLeft = name.indexOf("_L_") != -1;
    bool isRight = name.indexOf("_R_") != -1;
    
    bool match = false;
    if (targetEye == G2_EYE_AUTO) {
      match = true;  // Accept any G2 device
    } else if (targetEye == G2_EYE_LEFT && isLeft) {
      match = true;
    } else if (targetEye == G2_EYE_RIGHT && isRight) {
      match = true;
    }
    
    if (match) {
      gFoundDeviceName = name;
      gFoundDeviceAddress = advertisedDevice.getAddress().toString().c_str();
      
      if (pG2FoundDevice) delete pG2FoundDevice;
      pG2FoundDevice = new BLEAdvertisedDevice(advertisedDevice);
      
      gScanComplete = true;
      pG2Scan->stop();
      DEBUG_G2F("[G2] Target device found: %s @ %s", 
                    gFoundDeviceName.c_str(), gFoundDeviceAddress.c_str());
      broadcastOutput("[G2] Connecting to " + name);
    }
  }
};

// =============================================================================
// CLIENT CALLBACKS
// =============================================================================

class G2ClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient* pClient) override {
    DEBUG_G2F("[G2] Connected to glasses");
  }
  
  void onDisconnect(BLEClient* pClient) override {
    DEBUG_G2F("[G2] Disconnected from glasses");
    if (gG2State) {
      gG2State->state = G2_STATE_IDLE;
    }
    pG2WriteChar = nullptr;
    pG2NotifyChar = nullptr;
  }
};

// =============================================================================
// INITIALIZATION
// =============================================================================

bool initG2Client() {
  DEBUG_G2F("[G2-INIT] === INITIALIZING G2 CLIENT ===");
  
  if (gG2State && gG2State->initialized) {
    DEBUG_G2F("[G2-INIT] Already initialized");
    return true;
  }
  
  // Tear down existing BLE server if running
  if (isBLERunning()) {
    DEBUG_G2F("[G2-INIT] BLE server is running, stopping it...");
    broadcastOutput("[G2] Stopping BLE server mode...");
    deinitBluetooth();
    vTaskDelay(pdMS_TO_TICKS(200));
    DEBUG_G2F("[G2-INIT] BLE server stopped");
  }
  
  // Fully stop and restart the Bluetooth controller to ensure clean state
  // This is necessary because BLEDevice::deinit() doesn't fully release the controller
  DEBUG_G2F("[G2-INIT] Stopping BT controller for clean restart...");
  if (btStarted()) {
    btStop();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  DEBUG_G2F("[G2-INIT] Starting BT controller...");
  if (!btStart()) {
    DEBUG_G2F("[G2-INIT] ERROR: btStart() failed");
    broadcastOutput("[G2] ERROR: BT controller start failed");
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(100));
  
  // Allocate state
  DEBUG_G2F("[G2-INIT] Allocating state structure...");
  gG2State = (G2ClientState*)malloc(sizeof(G2ClientState));
  if (!gG2State) {
    DEBUG_G2F("[G2-INIT] ERROR: Failed to allocate state");
    broadcastOutput("[G2] ERROR: Memory allocation failed");
    return false;
  }
  memset(gG2State, 0, sizeof(G2ClientState));
  DEBUG_G2F("[G2-INIT] State allocated OK");
  
  // Initialize BLE - force reinit since we restarted the controller
  DEBUG_G2F("[G2-INIT] Initializing BLE stack...");
  BLEDevice::init("HardwareOne");
  DEBUG_G2F("[G2-INIT] BLE stack initialized");
  
  // Set global MTU
  DEBUG_G2F("[G2-INIT] Setting MTU to %d...", G2_MTU_TARGET);
  BLEDevice::setMTU(G2_MTU_TARGET);
  
  // Create scan instance
  DEBUG_G2F("[G2-INIT] Getting BLE scan instance...");
  pG2Scan = BLEDevice::getScan();
  if (!pG2Scan) {
    DEBUG_G2F("[G2-INIT] ERROR: Failed to get scan instance");
    broadcastOutput("[G2] ERROR: BLE scan init failed");
    free(gG2State);
    gG2State = nullptr;
    return false;
  }
  
  DEBUG_G2F("[G2-INIT] Configuring scan parameters...");
  pG2Scan->setActiveScan(true);
  pG2Scan->setInterval(100);
  pG2Scan->setWindow(99);
  
  gG2State->initialized = true;
  gG2State->state = G2_STATE_IDLE;
  
  DEBUG_G2F("[G2-INIT] === INITIALIZATION COMPLETE ===");
  broadcastOutput("[G2] Client mode ready");
  return true;
}

void deinitG2Client() {
  if (!gG2State) return;
  
  g2Disconnect();
  
  if (pG2FoundDevice) {
    delete pG2FoundDevice;
    pG2FoundDevice = nullptr;
  }
  
  if (pG2Client) {
    delete pG2Client;
    pG2Client = nullptr;
  }
  
  free(gG2State);
  gG2State = nullptr;
  
  pG2Scan = nullptr;
  pG2WriteChar = nullptr;
  pG2NotifyChar = nullptr;
  
  DEBUG_G2F("[G2] Client deinitialized");
}

bool isG2ClientInitialized() {
  return gG2State && gG2State->initialized;
}

// =============================================================================
// CONNECTION
// =============================================================================

bool g2Connect(G2Eye eye) {
  DEBUG_G2F("[G2] === CONNECTION START ===");
  broadcastOutput("[G2] Starting connection...");
  
  if (!gG2State || !gG2State->initialized) {
    DEBUG_G2F("[G2] Client not initialized, initializing now...");
    broadcastOutput("[G2] Initializing client...");
    if (!initG2Client()) {
      DEBUG_G2F("[G2] ERROR: Failed to initialize client");
      broadcastOutput("[G2] ERROR: Init failed");
      return false;
    }
  }
  
  if (gG2State->state == G2_STATE_CONNECTED) {
    DEBUG_G2F("[G2] Already connected");
    broadcastOutput("[G2] Already connected");
    return true;
  }
  
  gG2State->targetEye = eye;
  gG2State->state = G2_STATE_SCANNING;
  gScanComplete = false;
  gFoundDeviceName = "";
  gFoundDeviceAddress = "";
  
  const char* eyeStr = eye == G2_EYE_LEFT ? "LEFT" : 
                       eye == G2_EYE_RIGHT ? "RIGHT" : "AUTO";
  DEBUG_G2F("[G2] STEP 1: Scanning for %s eye (10 sec timeout)...", eyeStr);
  BROADCAST_PRINTF("[G2] Scanning for %s eye...", eyeStr);
  broadcastOutput("[G2] Make sure glasses are NOT connected to phone!");
  
  // Start scan
  pG2Scan->setAdvertisedDeviceCallbacks(new G2AdvertisedDeviceCallbacks(eye), true);
  pG2Scan->start(10, false);  // 10 seconds, non-blocking
  
  // Wait for scan completion with progress
  uint32_t scanStart = millis();
  int lastSec = -1;
  while (!gScanComplete && millis() - scanStart < 12000) {
    int sec = (millis() - scanStart) / 1000;
    if (sec != lastSec && sec % 2 == 0) {
      DEBUG_G2F("[G2] Scanning... %d sec", sec);
      lastSec = sec;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  
  pG2Scan->stop();  // Ensure scan is stopped
  
  if (!gScanComplete || !pG2FoundDevice) {
    gG2State->state = G2_STATE_IDLE;
    DEBUG_G2F("[G2] ERROR: No G2 glasses found during scan");
    DEBUG_G2F("[G2] Check: Is Bluetooth on? Are glasses powered on? Is phone disconnected?");
    broadcastOutput("[G2] No glasses found!");
    broadcastOutput("[G2] Tips: Power on glasses, disconnect phone app");
    return false;
  }
  
  // Connect
  gG2State->state = G2_STATE_CONNECTING;
  DEBUG_G2F("[G2] STEP 2: Connecting to %s @ %s", 
            gFoundDeviceName.c_str(), gFoundDeviceAddress.c_str());
  BROADCAST_PRINTF("[G2] Connecting to %s...", gFoundDeviceName.c_str());
  
  if (pG2Client) {
    DEBUG_G2F("[G2] Cleaning up old client...");
    delete pG2Client;
  }
  pG2Client = BLEDevice::createClient();
  pG2Client->setClientCallbacks(new G2ClientCallbacks());
  
  DEBUG_G2F("[G2] Attempting BLE connection...");
  if (!pG2Client->connect(pG2FoundDevice)) {
    gG2State->state = G2_STATE_ERROR;
    DEBUG_G2F("[G2] ERROR: BLE connection failed");
    DEBUG_G2F("[G2] Check: Is the device still in range? Phone disconnected?");
    broadcastOutput("[G2] Connection failed!");
    return false;
  }
  DEBUG_G2F("[G2] BLE connection established");
  broadcastOutput("[G2] BLE connected, negotiating MTU...");
  
  // Request MTU
  DEBUG_G2F("[G2] STEP 3: Requesting MTU %d...", G2_MTU_TARGET);
  pG2Client->setMTU(G2_MTU_TARGET);
  gG2State->mtu = pG2Client->getMTU();
  DEBUG_G2F("[G2] MTU negotiated: %d bytes", gG2State->mtu);
  BROADCAST_PRINTF("[G2] MTU: %d bytes", gG2State->mtu);
  
  // Get service
  DEBUG_G2F("[G2] STEP 4: Discovering G2 service...");
  DEBUG_G2F("[G2] Looking for service UUID: %s", G2_SERVICE_UUID);
  BLERemoteService* pService = pG2Client->getService(BLEUUID(G2_SERVICE_UUID));
  if (!pService) {
    DEBUG_G2F("[G2] ERROR: G2 service not found!");
    DEBUG_G2F("[G2] This device may not be an Even G2 glasses");
    broadcastOutput("[G2] ERROR: Service not found");
    g2Disconnect();
    return false;
  }
  DEBUG_G2F("[G2] G2 service found");
  broadcastOutput("[G2] Service found, getting characteristics...");
  
  // Get characteristics
  DEBUG_G2F("[G2] STEP 5: Getting characteristics...");
  DEBUG_G2F("[G2] Write char: %s", G2_CHAR_WRITE_UUID);
  DEBUG_G2F("[G2] Notify char: %s", G2_CHAR_NOTIFY_UUID);
  
  pG2WriteChar = pService->getCharacteristic(BLEUUID(G2_CHAR_WRITE_UUID));
  pG2NotifyChar = pService->getCharacteristic(BLEUUID(G2_CHAR_NOTIFY_UUID));
  
  if (!pG2WriteChar) {
    DEBUG_G2F("[G2] ERROR: Write characteristic not found");
    broadcastOutput("[G2] ERROR: Write char missing");
    g2Disconnect();
    return false;
  }
  DEBUG_G2F("[G2] Write characteristic found");
  
  if (!pG2NotifyChar) {
    DEBUG_G2F("[G2] ERROR: Notify characteristic not found");
    broadcastOutput("[G2] ERROR: Notify char missing");
    g2Disconnect();
    return false;
  }
  DEBUG_G2F("[G2] Notify characteristic found");
  broadcastOutput("[G2] Characteristics OK");
  
  // Subscribe to notifications
  DEBUG_G2F("[G2] STEP 6: Subscribing to notifications...");
  if (pG2NotifyChar->canNotify()) {
    pG2NotifyChar->registerForNotify(g2NotifyCallback);
    DEBUG_G2F("[G2] Notification subscription successful");
    broadcastOutput("[G2] Notifications enabled");
  } else {
    DEBUG_G2F("[G2] WARNING: Notify characteristic doesn't support notifications");
  }
  
  vTaskDelay(pdMS_TO_TICKS(300));
  
  // Run auth handshake
  DEBUG_G2F("[G2] STEP 7: Running authentication handshake...");
  broadcastOutput("[G2] Authenticating (7-packet handshake)...");
  gG2State->state = G2_STATE_AUTHENTICATING;
  
  if (!g2RunAuthHandshake()) {
    DEBUG_G2F("[G2] ERROR: Authentication handshake failed");
    broadcastOutput("[G2] ERROR: Auth failed");
    g2Disconnect();
    return false;
  }
  
  gG2State->deviceName = gFoundDeviceName;
  gG2State->deviceAddress = gFoundDeviceAddress;
  gG2State->connectedSince = millis();
  
  DEBUG_G2F("[G2] === CONNECTION COMPLETE ===");
  DEBUG_G2F("[G2] Connected to: %s", gFoundDeviceName.c_str());
  DEBUG_G2F("[G2] Address: %s", gFoundDeviceAddress.c_str());
  DEBUG_G2F("[G2] MTU: %d", gG2State->mtu);
  
  BROADCAST_PRINTF("[G2] SUCCESS: Connected to %s", gFoundDeviceName.c_str());
  broadcastOutput("[G2] Ready! Try: g2 show \"Hello\"");
  notifyBleDeviceConnected(gFoundDeviceName.c_str());
  
  return true;
}

void g2Disconnect() {
  if (!gG2State) return;
  
  gG2State->state = G2_STATE_DISCONNECTING;
  
  if (pG2Client && pG2Client->isConnected()) {
    pG2Client->disconnect();
  }
  
  pG2WriteChar = nullptr;
  pG2NotifyChar = nullptr;
  
  gG2State->state = G2_STATE_IDLE;
  gG2State->deviceName = "";
  gG2State->deviceAddress = "";
  
  DEBUG_G2F("[G2] Disconnected");
  broadcastOutput("[G2] Disconnected");
  notifyBleDeviceDisconnected("G2");
}

bool isG2Connected() {
  return gG2State && gG2State->state == G2_STATE_CONNECTED && 
         pG2Client && pG2Client->isConnected();
}

G2State getG2State() {
  return gG2State ? gG2State->state : G2_STATE_IDLE;
}

const char* getG2StateString() {
  if (!gG2State) return "uninitialized";
  switch (gG2State->state) {
    case G2_STATE_IDLE:           return "idle";
    case G2_STATE_SCANNING:       return "scanning";
    case G2_STATE_CONNECTING:     return "connecting";
    case G2_STATE_AUTHENTICATING: return "authenticating";
    case G2_STATE_CONNECTED:      return "connected";
    case G2_STATE_DISCONNECTING:  return "disconnecting";
    case G2_STATE_ERROR:          return "error";
    default:                      return "unknown";
  }
}

// =============================================================================
// SCANNING
// =============================================================================

bool g2StartScan(uint32_t durationMs) {
  if (!gG2State || !gG2State->initialized) {
    if (!initG2Client()) return false;
  }
  
  gG2State->state = G2_STATE_SCANNING;
  gScanComplete = false;
  
  pG2Scan->setAdvertisedDeviceCallbacks(new G2AdvertisedDeviceCallbacks(G2_EYE_AUTO), true);
  pG2Scan->start(durationMs / 1000, false);
  
  return true;
}

void g2StopScan() {
  if (pG2Scan) {
    pG2Scan->stop();
  }
  if (gG2State && gG2State->state == G2_STATE_SCANNING) {
    gG2State->state = G2_STATE_IDLE;
  }
}

// =============================================================================
// TELEPROMPTER TEXT DISPLAY
// =============================================================================

// Display config packet (required before text)
static bool g2SendDisplayConfig() {
  if (!isG2Connected()) return false;
  
  // Fixed display config from protocol
  static const uint8_t configData[] = {
    0x08, 0x01, 0x12, 0x13, 0x08, 0x02, 0x10, 0x90,
    0x4E, 0x1D, 0x00, 0xE0, 0x94, 0x44, 0x25, 0x00,
    0x00, 0x00, 0x00, 0x28, 0x00, 0x30, 0x00, 0x12,
    0x13, 0x08, 0x03, 0x10, 0x0D, 0x0F, 0x1D, 0x00,
    0x40, 0x8D, 0x44, 0x25, 0x00, 0x00, 0x00, 0x00,
    0x28, 0x00, 0x30, 0x00, 0x12, 0x12, 0x08, 0x04,
    0x10, 0x00, 0x1D, 0x00, 0x00, 0x88, 0x42, 0x25,
    0x00, 0x00, 0x00, 0x00, 0x28, 0x00, 0x30, 0x00,
    0x12, 0x12, 0x08, 0x05, 0x10, 0x00, 0x1D, 0x00,
    0x00, 0x92, 0x42, 0x25, 0x00, 0x00, 0xA2, 0x42,
    0x28, 0x00, 0x30, 0x00, 0x12, 0x12, 0x08, 0x06,
    0x10, 0x00, 0x1D, 0x00, 0x00, 0xC6, 0x42, 0x25,
    0x00, 0x00, 0xC4, 0x42, 0x28, 0x00, 0x30, 0x00,
    0x18, 0x00
  };
  
  uint8_t payload[128];
  size_t pos = 0;
  
  payload[pos++] = 0x08;
  payload[pos++] = 0x02;  // Type = 2
  payload[pos++] = 0x10;
  pos += g2EncodeVarint(gG2State->msgId++, payload + pos);
  payload[pos++] = 0x22;
  payload[pos++] = sizeof(configData);
  memcpy(payload + pos, configData, sizeof(configData));
  pos += sizeof(configData);
  
  return g2SendPacket(G2_SVC_DISPLAY_CFG_HI, G2_SVC_DISPLAY_CFG_LO, payload, pos);
}

// Teleprompter init packet
static bool g2SendTeleprompterInit(size_t totalLines, bool manualMode) {
  if (!isG2Connected()) return false;
  
  uint8_t mode = manualMode ? 0x00 : 0x01;
  
  // Scale content height based on lines (from protocol: 140 lines = 2665)
  uint32_t contentHeight = (totalLines * 2665) / 140;
  if (contentHeight < 100) contentHeight = 100;
  
  uint8_t displaySettings[32];
  size_t dPos = 0;
  displaySettings[dPos++] = 0x08; displaySettings[dPos++] = 0x01;
  displaySettings[dPos++] = 0x10; displaySettings[dPos++] = 0x00;
  displaySettings[dPos++] = 0x18; displaySettings[dPos++] = 0x00;
  displaySettings[dPos++] = 0x20; displaySettings[dPos++] = 0x8B; displaySettings[dPos++] = 0x02;
  displaySettings[dPos++] = 0x28;
  dPos += g2EncodeVarint(contentHeight, displaySettings + dPos);
  displaySettings[dPos++] = 0x30; displaySettings[dPos++] = 0xE6; displaySettings[dPos++] = 0x01;  // Line height = 230
  displaySettings[dPos++] = 0x38; displaySettings[dPos++] = 0x8E; displaySettings[dPos++] = 0x0A;  // Viewport = 1294
  displaySettings[dPos++] = 0x40; displaySettings[dPos++] = 0x05;  // Font size
  displaySettings[dPos++] = 0x48; displaySettings[dPos++] = mode;
  
  uint8_t settings[64];
  size_t sPos = 0;
  settings[sPos++] = 0x08;
  settings[sPos++] = 0x01;  // Script index
  settings[sPos++] = 0x12;
  settings[sPos++] = dPos;
  memcpy(settings + sPos, displaySettings, dPos);
  sPos += dPos;
  
  uint8_t payload[128];
  size_t pos = 0;
  payload[pos++] = 0x08;
  payload[pos++] = 0x01;  // Type = 1 (init)
  payload[pos++] = 0x10;
  pos += g2EncodeVarint(gG2State->msgId++, payload + pos);
  payload[pos++] = 0x1A;
  payload[pos++] = sPos;
  memcpy(payload + pos, settings, sPos);
  pos += sPos;
  
  return g2SendPacket(G2_SVC_TELEPROMPTER_HI, G2_SVC_TELEPROMPTER_LO, payload, pos);
}

// Send a content page
static bool g2SendContentPage(size_t pageNum, const char* text) {
  if (!isG2Connected()) return false;
  
  // Prepend newline as per protocol
  String textWithNl = String("\n") + text;
  
  uint8_t inner[256];
  size_t iPos = 0;
  inner[iPos++] = 0x08;
  iPos += g2EncodeVarint(pageNum, inner + iPos);
  inner[iPos++] = 0x10;
  inner[iPos++] = 0x0A;  // 10 lines
  inner[iPos++] = 0x1A;
  iPos += g2EncodeVarint(textWithNl.length(), inner + iPos);
  memcpy(inner + iPos, textWithNl.c_str(), textWithNl.length());
  iPos += textWithNl.length();
  
  uint8_t payload[300];
  size_t pos = 0;
  payload[pos++] = 0x08;
  payload[pos++] = 0x03;  // Type = 3 (content)
  payload[pos++] = 0x10;
  pos += g2EncodeVarint(gG2State->msgId++, payload + pos);
  payload[pos++] = 0x2A;
  pos += g2EncodeVarint(iPos, payload + pos);
  memcpy(payload + pos, inner, iPos);
  pos += iPos;
  
  return g2SendPacket(G2_SVC_TELEPROMPTER_HI, G2_SVC_TELEPROMPTER_LO, payload, pos);
}

// Mid-stream marker (required between pages 9 and 10)
static bool g2SendMarker() {
  if (!isG2Connected()) return false;
  
  uint8_t payload[16];
  size_t pos = 0;
  payload[pos++] = 0x08;
  payload[pos++] = 0xFF;
  payload[pos++] = 0x01;  // Type = 255 (varint)
  payload[pos++] = 0x10;
  pos += g2EncodeVarint(gG2State->msgId++, payload + pos);
  payload[pos++] = 0x6A;
  payload[pos++] = 0x04;
  payload[pos++] = 0x08;
  payload[pos++] = 0x00;
  payload[pos++] = 0x10;
  payload[pos++] = 0x06;
  
  return g2SendPacket(G2_SVC_TELEPROMPTER_HI, G2_SVC_TELEPROMPTER_LO, payload, pos);
}

// Sync trigger
static bool g2SendSync() {
  if (!isG2Connected()) return false;
  
  uint8_t payload[16];
  size_t pos = 0;
  payload[pos++] = 0x08;
  payload[pos++] = 0x0E;  // Type = 14
  payload[pos++] = 0x10;
  pos += g2EncodeVarint(gG2State->msgId++, payload + pos);
  payload[pos++] = 0x6A;
  payload[pos++] = 0x00;
  
  return g2SendPacket(G2_SVC_SYNC_HI, G2_SVC_SYNC_LO, payload, pos);
}

// Format text into pages (25 chars/line, 10 lines/page)
static void g2FormatTextToPages(const char* text, String pages[], size_t& pageCount, size_t maxPages) {
  String input = text;
  input.replace("\\n", "\n");
  
  // Wrap text
  String wrapped[200];
  size_t lineCount = 0;
  
  int start = 0;
  while (start < (int)input.length() && lineCount < 200) {
    int nlPos = input.indexOf('\n', start);
    String line;
    if (nlPos == -1) {
      line = input.substring(start);
      start = input.length();
    } else {
      line = input.substring(start, nlPos);
      start = nlPos + 1;
    }
    
    // Wrap long lines at 25 chars
    while (line.length() > 25 && lineCount < 200) {
      int breakAt = 25;
      for (int i = 25; i > 10; i--) {
        if (line[i] == ' ') {
          breakAt = i;
          break;
        }
      }
      wrapped[lineCount++] = line.substring(0, breakAt);
      line = line.substring(breakAt);
      line.trim();
    }
    if (line.length() > 0 && lineCount < 200) {
      wrapped[lineCount++] = line;
    }
  }
  
  // Pad to at least 10 lines
  while (lineCount < 10) {
    wrapped[lineCount++] = " ";
  }
  
  // Build pages (10 lines each)
  pageCount = 0;
  for (size_t i = 0; i < lineCount && pageCount < maxPages; i += 10) {
    String page = "";
    for (size_t j = i; j < i + 10 && j < lineCount; j++) {
      page += wrapped[j];
      if (j < i + 9) page += "\n";
    }
    page += " \n";  // Trailing space + newline
    pages[pageCount++] = page;
  }
  
  // Pad to at least 14 pages (protocol requirement)
  String blankPage = "";
  for (int i = 0; i < 10; i++) {
    blankPage += " ";
    if (i < 9) blankPage += "\n";
  }
  blankPage += " \n";
  
  while (pageCount < 14 && pageCount < maxPages) {
    pages[pageCount++] = blankPage;
  }
}

bool g2ShowText(const char* text) {
  if (!isG2Connected()) {
    DEBUG_G2F("[G2] Not connected");
    return false;
  }
  
  DEBUG_G2F("[G2] Showing text: %s", text);
  
  // Format text into pages
  String pages[50];
  size_t pageCount = 0;
  g2FormatTextToPages(text, pages, pageCount, 50);
  
  // Send display config
  g2SendDisplayConfig();
  vTaskDelay(pdMS_TO_TICKS(300));
  
  // Send teleprompter init
  size_t totalLines = pageCount * 10;
  g2SendTeleprompterInit(totalLines, true);
  vTaskDelay(pdMS_TO_TICKS(500));
  
  // Send content pages 0-9
  for (size_t i = 0; i < 10 && i < pageCount; i++) {
    g2SendContentPage(i, pages[i].c_str());
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  
  // Mid-stream marker
  g2SendMarker();
  vTaskDelay(pdMS_TO_TICKS(100));
  
  // Pages 10-11
  for (size_t i = 10; i < 12 && i < pageCount; i++) {
    g2SendContentPage(i, pages[i].c_str());
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  
  // Sync trigger
  g2SendSync();
  vTaskDelay(pdMS_TO_TICKS(100));
  
  // Remaining pages
  for (size_t i = 12; i < pageCount; i++) {
    g2SendContentPage(i, pages[i].c_str());
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  
  DEBUG_G2F("[G2] Text sent");
  return true;
}

bool g2ShowMultiLine(const char* lines[], size_t lineCount) {
  String combined = "";
  for (size_t i = 0; i < lineCount; i++) {
    if (i > 0) combined += "\n";
    combined += lines[i];
  }
  return g2ShowText(combined.c_str());
}

bool g2ClearDisplay() {
  return g2ShowText(" ");
}

// =============================================================================
// EVENT HANDLING
// =============================================================================

void g2SetEventCallback(G2EventCallback callback) {
  if (gG2State) {
    gG2State->eventCallback = callback;
  }
}

void g2Tick() {
  if (!gG2State) return;
  
  // Handle deferred gesture event (set by notify callback, processed here with proper stack)
  if (gG2State->deferredGesturePending) {
    gG2State->deferredGesturePending = false;
    G2EventType event = gG2State->deferredGestureEvent;
    
    DEBUG_G2F("[G2] GESTURE: %s", g2EventTypeToString(event));
    
    char msg[48];
    snprintf(msg, sizeof(msg), "[G2] Gesture: %s", g2EventTypeToString(event));
    broadcastOutput(msg);
  }
  
  // Connection health check, reconnect logic, etc.
  if (gG2State->state == G2_STATE_CONNECTED) {
    if (pG2Client && !pG2Client->isConnected()) {
      DEBUG_G2F("[G2] Connection lost");
      gG2State->state = G2_STATE_IDLE;
      pG2WriteChar = nullptr;
      pG2NotifyChar = nullptr;
      notifyBleDeviceDisconnected("G2");
    }
  }
}

// =============================================================================
// STATUS
// =============================================================================

void getG2Status(char* buffer, size_t bufferSize) {
  if (!buffer || bufferSize < 64) return;
  
  if (!gG2State || !gG2State->initialized) {
    snprintf(buffer, bufferSize, "G2: not initialized");
    return;
  }
  
  if (gG2State->state == G2_STATE_CONNECTED) {
    uint32_t duration = (millis() - gG2State->connectedSince) / 1000;
    snprintf(buffer, bufferSize, "G2: %s (MTU:%d tx:%lu rx:%lu %lus)",
             gG2State->deviceName.c_str(),
             gG2State->mtu,
             gG2State->packetsSent,
             gG2State->packetsReceived,
             duration);
  } else {
    snprintf(buffer, bufferSize, "G2: %s", getG2StateString());
  }
}

// =============================================================================
// COMMAND HANDLERS
// =============================================================================

static const char* cmd_g2connect(const String& cmd) {
  String arg = cmd;
  arg.replace("openg2", "");
  arg.replace("g2 connect", "");
  arg.replace("g2connect", "");
  arg.trim();
  
  G2Eye eye = G2_EYE_LEFT;  // Default to left
  if (arg.equalsIgnoreCase("right") || arg.equalsIgnoreCase("r")) {
    eye = G2_EYE_RIGHT;
  } else if (arg.equalsIgnoreCase("auto") || arg.equalsIgnoreCase("any")) {
    eye = G2_EYE_AUTO;
  }
  
  if (g2Connect(eye)) {
    return "G2 glasses connected";
  }
  return "G2 connection failed";
}

static const char* cmd_g2disconnect(const String& cmd) {
  g2Disconnect();
  return "G2 glasses disconnected";
}

static const char* cmd_g2status(const String& cmd) {
  static char statusBuf[128];
  getG2Status(statusBuf, sizeof(statusBuf));
  return statusBuf;
}

static const char* cmd_g2show(const String& cmd) {
  String text = cmd;
  // Remove command prefix
  if (text.startsWith("g2 show ")) {
    text = text.substring(8);
  } else if (text.startsWith("g2show ")) {
    text = text.substring(7);
  }
  text.trim();
  
  if (text.length() == 0) {
    return "Usage: g2 show <text>";
  }
  
  if (g2ShowText(text.c_str())) {
    return "Text sent to glasses";
  }
  return "Failed to send text (not connected?)";
}

static const char* cmd_g2scan(const String& cmd) {
  if (g2StartScan(10000)) {
    return "Scanning for G2 glasses (10s)...";
  }
  return "Failed to start scan";
}

static const char* cmd_g2init(const String& cmd) {
  if (initG2Client()) {
    return "G2 client initialized (BLE server mode disabled)";
  }
  return "Failed to initialize G2 client";
}

static const char* cmd_g2deinit(const String& cmd) {
  deinitG2Client();
  return "G2 client deinitialized";
}

static const char* cmd_g2clear(const String& cmd) {
  if (g2ClearDisplay()) {
    return "Display cleared";
  }
  return "Failed to clear display";
}

static const char* cmd_g2verbose(const String& cmd) {
  String arg = cmd;
  arg.replace("g2 verbose", "");
  arg.replace("g2verbose", "");
  arg.trim();
  
  if (arg.equalsIgnoreCase("on") || arg == "1") {
    gG2VerboseLog = true;
    return "G2 verbose logging ON";
  } else if (arg.equalsIgnoreCase("off") || arg == "0") {
    gG2VerboseLog = false;
    return "G2 verbose logging OFF";
  }
  
  return gG2VerboseLog ? "G2 verbose: ON" : "G2 verbose: OFF";
}

// =============================================================================
// GESTURE -> MENU NAVIGATION MAPPING
// =============================================================================

// Forward declaration for OLED menu navigation (if available)
extern void oledMenuUp();
extern void oledMenuDown();
extern void oledMenuSelect();
extern void oledMenuBack();

// Default gesture handler - maps swipes to menu navigation
static void g2DefaultGestureHandler(G2EventType event) {
  switch (event) {
    case G2_EVENT_SWIPE_UP:
      DEBUG_G2F("[G2] -> Menu UP");
      #if ENABLE_OLED_DISPLAY
      oledMenuUp();
      #endif
      break;
      
    case G2_EVENT_SWIPE_DOWN:
      DEBUG_G2F("[G2] -> Menu DOWN");
      #if ENABLE_OLED_DISPLAY
      oledMenuDown();
      #endif
      break;
      
    case G2_EVENT_TAP:
    case G2_EVENT_SWIPE_RIGHT:
      DEBUG_G2F("[G2] -> Menu SELECT");
      #if ENABLE_OLED_DISPLAY
      oledMenuSelect();
      #endif
      break;
      
    case G2_EVENT_LONG_PRESS:
    case G2_EVENT_SWIPE_LEFT:
      DEBUG_G2F("[G2] -> Menu BACK");
      #if ENABLE_OLED_DISPLAY
      oledMenuBack();
      #endif
      break;
      
    case G2_EVENT_DOUBLE_TAP:
      DEBUG_G2F("[G2] -> Voice arm toggle");
      // Could trigger voice arm/disarm here
      break;
      
    default:
      break;
  }
}

// Enable/disable default gesture-to-menu mapping
bool gG2MenuNavEnabled = true;

static const char* cmd_g2nav(const String& cmd) {
  String arg = cmd;
  arg.replace("g2 nav", "");
  arg.replace("g2nav", "");
  arg.trim();
  
  if (arg.equalsIgnoreCase("on") || arg == "1") {
    gG2MenuNavEnabled = true;
    g2SetEventCallback(g2DefaultGestureHandler);
    notifyGestureNavToggled(true);
    return "G2 menu navigation ON";
  } else if (arg.equalsIgnoreCase("off") || arg == "0") {
    gG2MenuNavEnabled = false;
    g2SetEventCallback(nullptr);
    notifyGestureNavToggled(false);
    return "G2 menu navigation OFF";
  }
  
  return gG2MenuNavEnabled ? "G2 nav: ON (swipe=nav, tap=select, long=back)" : "G2 nav: OFF";
}

// =============================================================================
// COMMAND REGISTRY
// =============================================================================

const CommandEntry g2Commands[] = {
  { "openg2",        "Connect to G2 glasses: openg2 [left|right|auto]",    false, cmd_g2connect },
  { "closeg2",       "Disconnect from G2 glasses",                          false, cmd_g2disconnect },
  { "g2status",      "Show G2 glasses connection status",                   false, cmd_g2status },
  { "g2 show",       "Display text on G2 glasses: g2 show <text>",          false, cmd_g2show },
  { "g2 scan",       "Scan for G2 glasses",                                 false, cmd_g2scan },
  { "g2 init",       "Initialize G2 client mode (disables BLE server)",     false, cmd_g2init },
  { "g2 deinit",     "Deinitialize G2 client mode",                         false, cmd_g2deinit },
  { "g2 clear",      "Clear G2 glasses display",                            false, cmd_g2clear },
  { "g2 verbose",    "Toggle verbose packet logging: g2 verbose [on|off]",  false, cmd_g2verbose },
  { "g2 nav",        "Toggle gesture->menu nav: g2 nav [on|off]",           false, cmd_g2nav },
};

const size_t g2CommandsCount = sizeof(g2Commands) / sizeof(g2Commands[0]);

// Auto-register with command system
static CommandModuleRegistrar _g2_cmd_registrar(g2Commands, g2CommandsCount, "even_g2");

#endif // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
