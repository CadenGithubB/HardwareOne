#include "System_BuildConfig.h"

#if ENABLE_WEB_ESPNOW

#include <Arduino.h>
#include <LittleFS.h>

#include "System_User.h"
#include "System_VFS.h"
#include "WebPage_ESPNow.h"
#include "WebServer_Server.h"
#include "WebServer_Utils.h"

// Forward declarations
extern void streamPageWithContent(httpd_req_t* req, const String& activePage, const String& username, void (*contentStreamer)(httpd_req_t*, const String&));
extern void streamBeginHtml(httpd_req_t* req, const char* title, bool isPublic, const String& username, const String& activePage);
extern void streamEndHtml(httpd_req_t* req);

static void streamEspNowContent(httpd_req_t* req, const String& username) {
  streamBeginHtml(req, "ESP-NOW", false, username, "espnow");
  httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
  streamEspNowInner(req);
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
  streamEndHtml(req);
}

static esp_err_t handleEspNowPage(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  streamPageWithContent(req, "espnow", ctx.user, streamEspNowContent);
  return ESP_OK;
}

// =============================================================================
// ESP-NOW API Endpoints
// =============================================================================

#if ENABLE_ESPNOW

#include "System_ESPNow.h"
#include "System_ESPNow_Sessions.h"  // SendStatus snapshot for delivery-tracking JSON
#include "System_MemUtil.h"

extern void* ps_alloc(size_t size, AllocPref pref, const char* tag);
extern esp_err_t handleEspNowMetadata(httpd_req_t* req);

static inline esp_err_t webEspnowSendChunk(httpd_req_t* req, const char* s) {
  return httpd_resp_send_chunk(req, s, HTTPD_RESP_USE_STRLEN);
}

static inline esp_err_t webEspnowSendChunkf(httpd_req_t* req, const char* fmt, ...) {
  char buf[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  return httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t webEspnowSendJsonEscapedString(httpd_req_t* req, const char* s) {
  esp_err_t err = webEspnowSendChunk(req, "\"");
  if (err != ESP_OK) return err;

  char out[128];
  size_t outLen = 0;

  auto flush = [&]() -> esp_err_t {
    if (outLen == 0) return ESP_OK;
    out[outLen] = '\0';
    esp_err_t e = httpd_resp_send_chunk(req, out, outLen);
    outLen = 0;
    return e;
  };

  for (const char* p = s; p && *p; ++p) {
    const unsigned char c = (unsigned char)(*p);
    const char* seq = nullptr;
    char tmp[8];
    size_t seqLen = 0;

    switch (c) {
      case '\\': seq = "\\\\"; seqLen = 2; break;
      case '"': seq = "\\\""; seqLen = 2; break;
      case '\b': seq = "\\b"; seqLen = 2; break;
      case '\f': seq = "\\f"; seqLen = 2; break;
      case '\n': seq = "\\n"; seqLen = 2; break;
      case '\r': seq = "\\r"; seqLen = 2; break;
      case '\t': seq = "\\t"; seqLen = 2; break;
      default:
        if (c < 0x20) {
          snprintf(tmp, sizeof(tmp), "\\u%04X", (unsigned)c);
          seq = tmp;
          seqLen = 6;
        } else {
          tmp[0] = (char)c;
          tmp[1] = '\0';
          seq = tmp;
          seqLen = 1;
        }
        break;
    }

    if (seqLen >= sizeof(out)) {
      err = flush();
      if (err != ESP_OK) return err;
      err = httpd_resp_send_chunk(req, seq, seqLen);
      if (err != ESP_OK) return err;
      continue;
    }

    if (outLen + seqLen > (sizeof(out) - 1)) {
      err = flush();
      if (err != ESP_OK) return err;
    }
    memcpy(out + outLen, seq, seqLen);
    outLen += seqLen;
  }

  err = flush();
  if (err != ESP_OK) return err;
  return webEspnowSendChunk(req, "\"");
}

// Persistent staging buffer for handleEspNowMessages. Lazy-initialized on first
// request, then reused for every subsequent poll for the lifetime of the device.
// Pre-2026-05 this was a per-request ps_alloc+free pair; the web UI polls this
// endpoint every 500 ms while the ESP-NOW page is open, which made it the
// noisiest single PSRAM allocator in memreport (134 allocs / 4.2 MB cumulative
// over one tracked 15-min window — the page is only open, and only polling, for
// part of any such window). Static buffer eliminates that churn.
//
// Sizing is PSRAM-aware to match the underlying per-peer ring capacity. Note
// PeerMessageHistory holds TWO rings of MESSAGES_PER_DEVICE (received + sent),
// so a peer's population is twice the depth quoted below:
//   PSRAM build:    MESSAGES_PER_DEVICE = 250, so a SINGLE peer can hold 500
//                   records — 10× this cap on its own, before any second peer.
//                   Cap at 50 per poll → ~15.8 KB in PSRAM. A backlog drains
//                   across polls via the existing ?since= cursor, but only
//                   losslessly in the single-peer case — see the caveat below.
//   No-PSRAM build: MESSAGES_PER_DEVICE = 16, so one peer holds up to 32
//                   records and the cap of 15 sits BELOW even a single ring's
//                   depth. Cap at 15 → ~4.7 KB. PreferPSRAM falls back to DRAM
//                   here; smaller cap keeps DRAM commitment modest (~1.5% of
//                   326 KB internal heap).
//
// Cursor caveat (multi-peer): getAllMessages fills peer-by-peer until the cap,
// then sorts only the subset it returned — it can miss the true global-newest
// (its own comment in System_ESPNow.cpp concedes this). The client advances
// lastSeqNum to the MAX seq it saw in the batch, so when the cap truncates a
// later peer's lower-seq records those records fall permanently below the
// cursor and are never fetched. Pagination is lossless only when the capped set
// is seq-contiguous — i.e. one peer, or the ?mac= filtered path.
//
// Concurrency: ESP-IDF httpd runs handlers sequentially on a single worker task
// (see comment at WebServer_Server.cpp:274), so no mutex is needed — only one
// handler invocation can touch this buffer at a time. If httpd ever switches
// to a threaded model, this needs a per-task buffer or a guard.
//
// Failure mode: if the lazy ps_alloc fails (PSRAM exhausted, fragmented, etc.),
// the handler sends an empty {"messages":[]} response and the next request will
// retry the allocation. Once it succeeds, it sticks.
#if CONFIG_SPIRAM_SUPPORT || CONFIG_ESP32S3_SPIRAM_SUPPORT
  static constexpr int kWebMessagesBufCount = 50;  // ~15.8 KB in PSRAM
#else
  static constexpr int kWebMessagesBufCount = 15;  // ~4.7 KB DRAM (no-PSRAM fallback)
#endif
static ReceivedTextMessage* gWebMessagesBuf = nullptr;

/**
 * @brief Fetch ESP-NOW text messages since lastSeq — BOTH directions
 * @param req HTTP request (query params: ?since=<seqNum>, optional &mac=<MAC>)
 * @return ESP_OK
 *
 * Sent and received records come back interleaved in one array; `sent` is what
 * tells them apart and is load-bearing for rendering (see the direction comment
 * at its emit site). Multi-fragment text is emitted per fragment, never as a
 * reassembled lump — reqId/piece/of let the client group the pieces and join
 * them itself.
 *
 * A second top-level "deliveries" array carries the tracked-send snapshot; the
 * client matches it to bubbles by msgId.
 *
 * {
 *   "messages": [
 *     {"seq":123,"reqId":7,"piece":1,"of":1,"mac":"XX:XX:XX:XX:XX:XX",
 *      "name":"device","msg":"text","enc":true,"ts":12345,"type":0,
 *      "sent":false,"sendState":1},
 *     ...
 *   ],
 *   "deliveries": [
 *     {"msgId":42,"state":"delivered","mac":"XX:XX:XX:XX:XX:XX","ageMs":1200},
 *     ...
 *   ]
 * }
 */
static esp_err_t handleEspNowMessages(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  
  httpd_resp_set_type(req, "application/json");
  
  // Check if ESP-NOW is initialized
  if (!gEspNow || !gEspNow->initialized) {
    httpd_resp_send(req, "{\"messages\":[]}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  // Parse query parameters: ?since=<seqNum>&mac=<MAC_ADDRESS>
  uint32_t sinceSeq = 0;
  uint8_t filterMac[6] = {0};
  bool hasMacFilter = false;
  
  char queryBuf[128];
  if (httpd_req_get_url_query_str(req, queryBuf, sizeof(queryBuf)) == ESP_OK) {
    char paramBuf[32];
    
    // Parse 'since' parameter
    if (httpd_query_key_value(queryBuf, "since", paramBuf, sizeof(paramBuf)) == ESP_OK) {
      sinceSeq = (uint32_t)strtoul(paramBuf, nullptr, 10);
    }
    
    // Parse optional 'mac' parameter for per-device filtering
    if (httpd_query_key_value(queryBuf, "mac", paramBuf, sizeof(paramBuf)) == ESP_OK) {
      // Inline URL-decode (httpd_query_key_value does NOT decode %XX sequences)
      char decoded[32];
      char* dst = decoded;
      char* dEnd = decoded + sizeof(decoded) - 1;
      for (const char* s = paramBuf; *s && dst < dEnd; ) {
        if (*s == '%' && s[1] && s[2]) {
          char hex[3] = {s[1], s[2], 0};
          *dst++ = (char)strtol(hex, nullptr, 16);
          s += 3;
        } else {
          *dst++ = *s++;
        }
      }
      *dst = '\0';
      // Parse MAC address (format: AA:BB:CC:DD:EE:FF or AABBCCDDEEFF)
      if (strlen(decoded) >= 12) {
        hasMacFilter = true;
        sscanf(decoded, "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
               &filterMac[0], &filterMac[1], &filterMac[2],
               &filterMac[3], &filterMac[4], &filterMac[5]);
      }
    }
  }
  
  // Lazy-init the persistent staging buffer on first request. The buffer is
  // never freed — every subsequent poll reuses it. See gWebMessagesBuf comment
  // above for the rationale (eliminates per-request 32 KB PSRAM alloc churn).
  // Tag is ".static" so memreport distinguishes it from the old per-request
  // tag; the new tag should show (1x) cumulatively, not climbing.
  if (!gWebMessagesBuf) {
    gWebMessagesBuf = (ReceivedTextMessage*)ps_alloc(sizeof(ReceivedTextMessage) * kWebMessagesBufCount,
                                                     AllocPref::PreferPSRAM,
                                                     "web.esnow.msgs.static");
    if (!gWebMessagesBuf) {
      // PSRAM alloc failed — send empty response, next request will retry.
      httpd_resp_send(req, "{\"messages\":[]}", HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
  }
  ReceivedTextMessage* messages = gWebMessagesBuf;
  
  // Get messages from per-device buffers. Cap is kWebMessagesBufCount (see
  // gWebMessagesBuf comment block above for the PSRAM-aware sizing rationale).
  int msgCount = 0;
  if (hasMacFilter) {
    // Get messages from specific peer
    msgCount = getPeerMessages(filterMac, messages, kWebMessagesBufCount, sinceSeq);
  } else {
    // Get all messages from all peers
    msgCount = getAllMessages(messages, kWebMessagesBufCount, sinceSeq);
  }

  esp_err_t err = webEspnowSendChunk(req, "{\"messages\":[");
  for (int i = 0; i < msgCount && err == ESP_OK; i++) {
    ReceivedTextMessage& msg = messages[i];

    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             msg.senderMac[0], msg.senderMac[1], msg.senderMac[2],
             msg.senderMac[3], msg.senderMac[4], msg.senderMac[5]);

    if (i > 0) err = webEspnowSendChunk(req, ",");
    if (err != ESP_OK) break;

    err = webEspnowSendChunk(req, "{");
    if (err != ESP_OK) break;

    err = webEspnowSendChunkf(req, "\"seq\":%lu,", (unsigned long)msg.seqNum);
    if (err != ESP_OK) break;
    err = webEspnowSendChunkf(req, "\"reqId\":%lu,", (unsigned long)msg.reqId);
    if (err != ESP_OK) break;
    err = webEspnowSendChunkf(req, "\"piece\":%u,\"of\":%u,", msg.piece, msg.pieceTotal);
    if (err != ESP_OK) break;
    err = webEspnowSendChunkf(req, "\"mac\":\"%s\",", macStr);
    if (err != ESP_OK) break;

    err = webEspnowSendChunk(req, "\"name\":");
    if (err != ESP_OK) break;
    err = webEspnowSendJsonEscapedString(req, msg.senderName);
    if (err != ESP_OK) break;
    err = webEspnowSendChunk(req, ",\"msg\":");
    if (err != ESP_OK) break;
    err = webEspnowSendJsonEscapedString(req, msg.message);
    if (err != ESP_OK) break;

    err = webEspnowSendChunkf(req, ",\"enc\":%s", msg.encrypted ? "true" : "false");
    if (err != ESP_OK) break;
    err = webEspnowSendChunkf(req, ",\"ts\":%lu", (unsigned long)msg.timestamp);
    if (err != ESP_OK) break;
    err = webEspnowSendChunkf(req, ",\"type\":%d", (int)msg.msgType);
    if (err != ESP_OK) break;
    // Direction: true = we sent it (now in the shared sent[] history), false =
    // received. The client renders sent on the right and de-dupes its own
    // optimistic bubble by reqId; without this flag a sent echo would wrongly
    // render as a received message.
    err = webEspnowSendChunkf(req, ",\"sent\":%s", msg.isSent ? "true" : "false");
    if (err != ESP_OK) break;
    // Durable per-message delivery state (0 pending,1 delivered,2 timeout,3 failed)
    // so sent rows render Delivered even for messages sent from another interface
    // or after a reload — not only freshly-sent web bubbles via the deliveries[] flip.
    err = webEspnowSendChunkf(req, ",\"sendState\":%u", msg.sendState);
    if (err != ESP_OK) break;

    err = webEspnowSendChunk(req, "}");
  }

  // NOTE: `messages` points at gWebMessagesBuf (the persistent buffer above).
  // Do NOT free it — the buffer is reused on every poll for the device's
  // lifetime. Pre-2026-05 a free() lived here paired with a per-request
  // ps_alloc; both are gone now.

  // Phase 3.5 task #49 — append the tracked-send snapshot so the web UI can
  // flip chat bubbles from ✓ Sent to ✓✓ Delivered (or ✗ Failed/Timeout) on
  // the same poll cadence as message delivery. Full snapshot of the 16-slot
  // ring; client filters by msgId. Budget ~64-85 B per entry — the quoted MAC
  // alone is 26 B of that, and the key names dominate the rest — so a fully
  // populated ring is ~1.0-1.4 KB per poll.
  if (err == ESP_OK) {
    err = webEspnowSendChunk(req, "],\"deliveries\":[");
    bool firstDelivery = true;
    uint8_t ssSlots = sendStatusSlotCount();
    uint32_t nowMs = (uint32_t)millis();
    for (uint8_t i = 0; i < ssSlots && err == ESP_OK; i++) {
      const SendStatus* s = sendStatusAt(i);
      if (!s || !s->inUse) continue;
      static const char* kStateNames[] = { "pending", "delivered", "timeout", "failed" };
      const char* stateName = (s->state < 4) ? kStateNames[s->state] : "unknown";
      char macStr[18];
      snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               s->peerMac[0], s->peerMac[1], s->peerMac[2],
               s->peerMac[3], s->peerMac[4], s->peerMac[5]);
      if (!firstDelivery) {
        err = webEspnowSendChunk(req, ",");
        if (err != ESP_OK) break;
      }
      firstDelivery = false;
      uint32_t ageMs = nowMs - s->registeredAtMs;
      err = webEspnowSendChunkf(req,
        "{\"msgId\":%lu,\"state\":\"%s\",\"mac\":\"%s\",\"ageMs\":%lu}",
        (unsigned long)s->msgId, stateName, macStr, (unsigned long)ageMs);
    }
    if (err == ESP_OK) err = webEspnowSendChunk(req, "]}");
  }
  httpd_resp_send_chunk(req, NULL, 0);
  return err;
}

/**
 * @brief Get remote device capability summary (cached from bond requestcap)
 * @return JSON with remote capability info including human-readable names
 */
static esp_err_t handleEspNowRemoteCap(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  
  httpd_resp_set_type(req, "application/json");
  
  if (!gEspNow || !gEspNow->lastRemoteCapValid) {
    httpd_resp_send(req, "{\"valid\":false}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  CapabilitySummary& cap = gEspNow->lastRemoteCap;
  
  // Get human-readable capability lists
  String featureList = getCapabilityListLong(cap.featureMask, FEATURE_NAMES);
  String serviceList = getCapabilityListLong(cap.serviceMask, SERVICE_NAMES);
  String sensorList = getCapabilityListLong(cap.sensorMask, SENSOR_NAMES);
  
  // Build fwHash hex string
  char fwHashHex[33];
  for (int i = 0; i < 16; i++) {
    snprintf(fwHashHex + (i * 2), 3, "%02x", cap.fwHash[i]);
  }
  
  // Build MAC string
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           cap.mac[0], cap.mac[1], cap.mac[2],
           cap.mac[3], cap.mac[4], cap.mac[5]);
  
  // Stream JSON response (larger due to human-readable strings)
  esp_err_t err = webEspnowSendChunk(req, "{\"valid\":true,");
  if (err != ESP_OK) goto done;
  
  err = webEspnowSendChunkf(req, "\"deviceName\":\"%s\",", cap.deviceName);
  if (err != ESP_OK) goto done;
  err = webEspnowSendChunkf(req, "\"mac\":\"%s\",", macStr);
  if (err != ESP_OK) goto done;
  err = webEspnowSendChunkf(req, "\"role\":%d,", (int)cap.role);
  if (err != ESP_OK) goto done;
  err = webEspnowSendChunkf(req, "\"roleName\":\"%s\",", cap.role == 1 ? "master" : "worker");
  if (err != ESP_OK) goto done;
  err = webEspnowSendChunkf(req, "\"fwHash\":\"%s\",", fwHashHex);
  if (err != ESP_OK) goto done;
  
  // Raw masks
  err = webEspnowSendChunkf(req, "\"featureMask\":%lu,", (unsigned long)cap.featureMask);
  if (err != ESP_OK) goto done;
  err = webEspnowSendChunkf(req, "\"serviceMask\":%lu,", (unsigned long)cap.serviceMask);
  if (err != ESP_OK) goto done;
  err = webEspnowSendChunkf(req, "\"sensorMask\":%lu,", (unsigned long)cap.sensorMask);
  if (err != ESP_OK) goto done;
  
  // Human-readable lists
  err = webEspnowSendChunk(req, "\"features\":");
  if (err != ESP_OK) goto done;
  err = webEspnowSendJsonEscapedString(req, featureList.c_str());
  if (err != ESP_OK) goto done;
  err = webEspnowSendChunk(req, ",\"services\":");
  if (err != ESP_OK) goto done;
  err = webEspnowSendJsonEscapedString(req, serviceList.c_str());
  if (err != ESP_OK) goto done;
  err = webEspnowSendChunk(req, ",\"sensors\":");
  if (err != ESP_OK) goto done;
  err = webEspnowSendJsonEscapedString(req, sensorList.c_str());
  if (err != ESP_OK) goto done;
  
  // Hardware info
  err = webEspnowSendChunkf(req, ",\"flashSizeMB\":%lu,", (unsigned long)cap.flashSizeMB);
  if (err != ESP_OK) goto done;
  err = webEspnowSendChunkf(req, "\"psramSizeMB\":%lu,", (unsigned long)cap.psramSizeMB);
  if (err != ESP_OK) goto done;
  err = webEspnowSendChunkf(req, "\"wifiChannel\":%d,", (int)cap.wifiChannel);
  if (err != ESP_OK) goto done;
  err = webEspnowSendChunkf(req, "\"uptimeSeconds\":%lu,", (unsigned long)cap.uptimeSeconds);
  if (err != ESP_OK) goto done;
  err = webEspnowSendChunkf(req, "\"ageMs\":%lu}", (unsigned long)(millis() - gEspNow->lastRemoteCapTime));
  
done:
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

/**
 * @brief List cached remote manifests or get specific manifest content
 * Query params: ?fwHash=<hash> to get specific manifest
 * Without params: returns list of available manifests
 */
static esp_err_t handleEspNowRemoteManifest(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  
  httpd_resp_set_type(req, "application/json");
  
  extern bool filesystemReady;
  if (!filesystemReady) {
    httpd_resp_send(req, "{\"error\":\"Filesystem not ready\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  const char* manifestDir = "/system/manifests";
  
  // Check for fwHash query param
  char queryBuf[64];
  char fwHashParam[40] = {0};
  if (httpd_req_get_url_query_str(req, queryBuf, sizeof(queryBuf)) == ESP_OK) {
    httpd_query_key_value(queryBuf, "fwHash", fwHashParam, sizeof(fwHashParam));
  }
  
  // If fwHash provided, return that specific manifest
  if (strlen(fwHashParam) > 0) {
    char pathBuf[80];
    snprintf(pathBuf, sizeof(pathBuf), "%s/%s.json", manifestDir, fwHashParam);
    String path = pathBuf;
    File f = VFS::openGuarded(path, "r", ctx);
    if (!f) {
      httpd_resp_send(req, "{\"error\":\"Manifest not found\"}", HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
    
    // Stream the manifest file content
    webEspnowSendChunk(req, "{\"fwHash\":\"");
    webEspnowSendChunk(req, fwHashParam);
    webEspnowSendChunk(req, "\",\"manifest\":");
    
    char buf[256];
    while (f.available()) {
      int len = f.readBytes(buf, sizeof(buf) - 1);
      buf[len] = '\0';
      httpd_resp_send_chunk(req, buf, len);
    }
    f.close();
    
    webEspnowSendChunk(req, "}");
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
  }
  
  // No fwHash - list all cached manifests
  if (!VFS::existsGuarded(manifestDir, ctx)) {
    httpd_resp_send(req, "{\"manifests\":[]}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  File dir = VFS::openGuarded(manifestDir, "r", ctx);
  if (!dir || !dir.isDirectory()) {
    httpd_resp_send(req, "{\"manifests\":[]}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  webEspnowSendChunk(req, "{\"manifests\":[");
  bool first = true;
  File entry;
  while ((entry = dir.openNextFile())) {
    if (!entry.isDirectory()) {
      String name = entry.name();
      if (name.endsWith(".json")) {
        String fwHash = name.substring(0, name.length() - 5);
        if (!first) webEspnowSendChunk(req, ",");
        first = false;
        webEspnowSendChunkf(req, "{\"fwHash\":\"%s\",\"size\":%d}", 
                           fwHash.c_str(), (int)entry.size());
      }
    }
    entry.close();
  }
  dir.close();
  
  webEspnowSendChunk(req, "]}");
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

#endif // ENABLE_ESPNOW

// =============================================================================
// Register ESP-NOW Handlers
// =============================================================================

#if ENABLE_ESPNOW
#include <ArduinoJson.h>
#include "System_Utils.h"
#include "System_CommandTypes.h"
#include "System_MemUtil.h"

static bool espnowRunInternal(const char* cmd, char* out, size_t outSize) {
  AuthContext sys;
  sys.transport = SOURCE_INTERNAL;
  sys.user = "system";
  sys.ip = "local";
  sys.path = cmd;
  out[0] = '\0';
  return executeCommand(sys, cmd, out, outSize);
}

// GET /api/espnow/status — same result slots as the old CLI batch, so the
// page can keep its apply logic. Runs as internal system (not the caller),
// so guests can load the page without /api/cli.
static esp_err_t handleEspNowStatus(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  httpd_resp_set_type(req, "application/json");

  static const char* CMDS[] = {
    "espnowstatus",
    "espnowmode",
    "bondstatus",
    "espnowlist",
    "espnowmeshes listjson",
    "espnowdeviceinfo",
    "espnowmeshrole",
    "espnowmeshstatus",
  };

  PSRAM_JSON_DOC(doc);
  doc["success"] = true;
  JsonArray results = doc["results"].to<JsonArray>();
  EXT_RAM_BSS_ATTR static char cmdBuf[CMD_RESULT_MAX];
  for (size_t i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
    espnowRunInternal(CMDS[i], cmdBuf, sizeof(cmdBuf));
    results.add(cmdBuf);
  }
  String out;
  serializeJson(doc, out);
  httpd_resp_send(req, out.c_str(), out.length());
  return ESP_OK;
}
#endif

void registerEspNowHandlers(httpd_handle_t server) {
  static const httpd_uri_t espnowPage = { .uri = "/espnow", .method = HTTP_GET, .handler = handleEspNowPage, .user_ctx = NULL };
  httpd_register_uri_handler(server, &espnowPage);
  
#if ENABLE_ESPNOW
  static const httpd_uri_t espnowStatus = { .uri = "/api/espnow/status", .method = HTTP_GET, .handler = handleEspNowStatus, .user_ctx = NULL };
  httpd_register_uri_handler(server, &espnowStatus);

  static const httpd_uri_t espnowMessages = { .uri = "/api/espnow/messages", .method = HTTP_GET, .handler = handleEspNowMessages, .user_ctx = NULL };
  httpd_register_uri_handler(server, &espnowMessages);
  
  static const httpd_uri_t espnowRemoteCap = { .uri = "/api/espnow/remotecap", .method = HTTP_GET, .handler = handleEspNowRemoteCap, .user_ctx = NULL };
  httpd_register_uri_handler(server, &espnowRemoteCap);
  
  static const httpd_uri_t espnowRemoteManifest = { .uri = "/api/espnow/remotemanifest", .method = HTTP_GET, .handler = handleEspNowRemoteManifest, .user_ctx = NULL };
  httpd_register_uri_handler(server, &espnowRemoteManifest);
  
  static const httpd_uri_t espnowMetadata = { .uri = "/api/espnow/metadata", .method = HTTP_GET, .handler = handleEspNowMetadata, .user_ctx = NULL };
  httpd_register_uri_handler(server, &espnowMetadata);
#endif
}

#endif // ENABLE_HTTP_SERVER
