#include "System_BuildConfig.h"

#if ENABLE_WEB_ESPNOW

#include <Arduino.h>

#include "System_User.h"
#include "System_VFS.h"
#include "System_Mutex.h"
#include "WebPage_ESPNow.h"
#include "WebServer_Server.h"
#include "WebServer_Utils.h"
#include "System_JsonText.h"
#if ENABLE_ESPNOW
#include "System_Filesystem.h"
#endif

static void streamEspNowContent(httpd_req_t* req, const String& username) {
  streamCardPage(req, "ESP-NOW", username, "espnow", [&] { streamEspNowInner(req); });
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
#include "System_ESPNow_HistoryNames.h"  // per-request name cache for the history rows
#include "System_ESPNow_Sessions.h"  // SendStatus snapshot for delivery-tracking JSON
#include "System_ESPNow_RemoteCmdCore.h"  // espnowcmd::outcomeKey: a remote command row's `result`
#include "System_ESPNow_AnswerCore.h"     // espnowanswer::statusName: a refused send's `reason`
#include "System_MemUtil.h"

static inline esp_err_t webEspNowSendChunk(httpd_req_t* req, const char* s) {
  return httpd_resp_send_chunk(req, s, HTTPD_RESP_USE_STRLEN);
}

static inline esp_err_t webEspNowSendChunkf(httpd_req_t* req, const char* fmt, ...) {
  char buf[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  return httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t webEspNowSendJsonEscapedString(httpd_req_t* req, const char* s) {
  esp_err_t err = webEspNowSendChunk(req, "\"");
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
    char seq[7];
    const size_t seqLen = jsonEscapeChar((unsigned char)*p, seq);
    if (outLen + seqLen > (sizeof(out) - 1)) {
      err = flush();
      if (err != ESP_OK) return err;
    }
    memcpy(out + outLen, seq, seqLen);
    outLen += seqLen;
  }

  err = flush();
  if (err != ESP_OK) return err;
  return webEspNowSendChunk(req, "\"");
}

// Persistent staging buffer for handleEspNowMessages. Lazy-initialized on first
// request, then reused for every subsequent poll for the lifetime of the device.
// Pre-2026-05 this was a per-request ps_alloc+free pair; the web UI polls this
// endpoint every 500 ms while the ESP-NOW page is open, which made it the
// noisiest single PSRAM allocator in memreport (134 allocs / 4.2 MB cumulative
// over one tracked 15-min window — the page is only open, and only polling, for
// part of any such window). Static buffer eliminates that churn.
//
// Sizing is PSRAM-aware. The history keeps, per peer, a received and a sent
// byte ring (System_ESPNow.h, espnowhistorykb), so how many records a peer holds
// depends on their length: a ring of R bytes holds at most R/20 records (empty
// texts), about R/60 forty-character ones. ReceivedTextMessage is a ~288 B
// copied-out view:
//   PSRAM build:    16 KB rings by default (32 KB maximum), so ONE peer can hold
//                   hundreds of records, far more than this cap. Cap at 50 per
//                   poll -> ~14.4 KB in PSRAM.
//   No-PSRAM build: 2 KB rings by default (4 KB maximum), still above the cap
//                   for short messages. Cap at 15 -> ~4.3 KB. PreferPsram falls
//                   back to DRAM here; the smaller cap keeps the DRAM commitment
//                   modest.
// A backlog drains across polls via the ?since= cursor without loss:
// getAllMessages is a k-way merge by seq across ALL peers and returns the
// lowest seqs above `since` first, ascending, so the client's lastSeqNum (the
// largest seq of the batch) never skips a lower seq of another peer.
// getPeerMessages (?mac=) is ascending by seq for that one peer.
//
// Names: records carry no name. Each row's "name" comes from
// espnowHistoryDisplayName (the peer's current name, the MAC text for an
// unattributed record, "" for a sent one), resolved once per distinct peer and
// kind per request through a name cache on the handler's stack.
//
// Concurrency: ESP-IDF httpd runs handlers sequentially on a single worker task
// (see comment at WebServer_Server.cpp:274), so no mutex is needed — only one
// handler invocation can touch this buffer at a time. If httpd ever switches
// to a threaded model, this needs a per-task buffer or a guard.
//
// Failure mode: if the lazy ps_alloc fails (PSRAM exhausted, fragmented, etc.),
// the handler sends an empty {"messages":[]} response and the next request will
// retry the allocation. Once it succeeds, it sticks.
// Same PSRAM test as ESPNOW_HISTORY_VIEW_MAX (System_ESPNow.h): CONFIG_SPIRAM.
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
  static constexpr int kWebMessagesBufCount = 50;  // ~14.4 KB in PSRAM
#else
  static constexpr int kWebMessagesBufCount = 15;  // ~4.3 KB DRAM (no-PSRAM fallback)
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
 *      "sent":false,"sendState":0,"result":"late"},
 *     {... "sent":true,"sendState":5,"reason":"too_large"},
 *     ...
 *   ],
 *   "deliveries": [
 *     {"msgId":42,"state":"delivered","mac":"XX:XX:XX:XX:XX:XX","ageMs":1200},
 *     {"msgId":43,"state":"refused","mac":"XX:XX:XX:XX:XX:XX","ageMs":900,"reason":"too_large"},
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
                                                     AllocPref::PreferPsram,
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

  // Resolve names after the store call returned (the resolver takes the group
  // lock; the history lock is not held here). Once per distinct peer and kind
  // of this request; ~330 B on the httpd stack, nothing kept between polls.
  espnowhist::NameCache names;

  esp_err_t err = webEspNowSendChunk(req, "{\"messages\":[");
  for (int i = 0; i < msgCount && err == ESP_OK; i++) {
    ReceivedTextMessage& msg = messages[i];

    char macStr[18];
    macToDisplay(msg.peerMac, macStr, sizeof(macStr));

    if (i > 0) err = webEspNowSendChunk(req, ",");
    if (err != ESP_OK) break;

    err = webEspNowSendChunk(req, "{");
    if (err != ESP_OK) break;

    err = webEspNowSendChunkf(req, "\"seq\":%lu,", (unsigned long)msg.seqNum);
    if (err != ESP_OK) break;
    err = webEspNowSendChunkf(req, "\"reqId\":%lu,", (unsigned long)msg.reqId);
    if (err != ESP_OK) break;
    err = webEspNowSendChunkf(req, "\"piece\":%u,\"of\":%u,", msg.piece, msg.pieceTotal);
    if (err != ESP_OK) break;
    err = webEspNowSendChunkf(req, "\"mac\":\"%s\",", macStr);
    if (err != ESP_OK) break;

    err = webEspNowSendChunk(req, "\"name\":");
    if (err != ESP_OK) break;
    {
      // "" for sent rows, the MAC text for unattributed ones, else the peer's
      // current name (the record itself stores no name).
      const char* name = names.get(msg.peerMac, msg.isSent, msg.unattributed,
          [&msg](char* out, size_t cap) { espnowHistoryDisplayName(msg, out, cap); });
      err = webEspNowSendJsonEscapedString(req, name);
    }
    if (err != ESP_OK) break;
    err = webEspNowSendChunk(req, ",\"msg\":");
    if (err != ESP_OK) break;
    err = webEspNowSendJsonEscapedString(req, msg.message);
    if (err != ESP_OK) break;

    err = webEspNowSendChunkf(req, ",\"enc\":%s", msg.encrypted ? "true" : "false");
    if (err != ESP_OK) break;
    err = webEspNowSendChunkf(req, ",\"ts\":%lu", (unsigned long)msg.timestamp);
    if (err != ESP_OK) break;
    err = webEspNowSendChunkf(req, ",\"type\":%d", (int)msg.msgType);
    if (err != ESP_OK) break;
    // Direction: true = we sent it (now in the shared sent[] history), false =
    // received. The client renders sent on the right and de-dupes its own
    // optimistic bubble by reqId; without this flag a sent echo would wrongly
    // render as a received message.
    err = webEspNowSendChunkf(req, ",\"sent\":%s", msg.isSent ? "true" : "false");
    if (err != ESP_OK) break;
    // Durable per-message delivery state of a sent row (SendStatusState: 0 pending
    // "Sent", 1 delivered, 2 timeout and 3 failed "No answer", 4 "Busy", 5
    // "Refused") so sent rows render the right word even for messages sent from
    // another interface or after a reload, not only freshly-sent web bubbles via
    // the deliveries[] flip. 0 on received rows.
    err = webEspNowSendChunkf(req, ",\"sendState\":%u", msg.isSent ? msg.sendState : 0);
    if (err != ESP_OK) break;
    // A refused sent row keeps its reason (the ACK status word), also after a reload.
    if (msg.isSent && msg.sendState == SEND_STATUS_REFUSED && espnowanswer::sharedStatusName(msg.sendStatus)) {
      err = webEspNowSendChunkf(req, ",\"reason\":\"%s\"", espnowanswer::sharedStatusName(msg.sendStatus));
      if (err != ESP_OK) break;
    }
    // A received remote command row that is not a plain result (1.0 step 6):
    // busy, refused, noAnswer, noResult, or late (the result after "No result").
    if (!msg.isSent && espnowcmd::outcomeKey(msg.sendState)) {
      err = webEspNowSendChunkf(req, ",\"result\":\"%s\"", espnowcmd::outcomeKey(msg.sendState));
      if (err != ESP_OK) break;
    }

    err = webEspNowSendChunk(req, "}");
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
    err = webEspNowSendChunk(req, "],\"deliveries\":[");
    bool firstDelivery = true;
    uint8_t ssSlots = sendStatusSlotCount();
    uint32_t nowMs = (uint32_t)millis();
    for (uint8_t i = 0; i < ssSlots && err == ESP_OK; i++) {
      const SendStatus* s = sendStatusAt(i);
      if (!s || !s->inUse) continue;
      static const char* kStateNames[] = { "pending", "delivered", "timeout", "failed", "busy", "refused" };
      const char* stateName = (s->state < 6) ? kStateNames[s->state] : "unknown";
      char macStr[18];
      macToDisplay(s->peerMac, macStr, sizeof(macStr));
      if (!firstDelivery) {
        err = webEspNowSendChunk(req, ",");
        if (err != ESP_OK) break;
      }
      firstDelivery = false;
      uint32_t ageMs = nowMs - s->registeredAtMs;
      // A refusal names its reason (the ACK's status word, 1.0 step 6).
      const char* reason = s->state == SEND_STATUS_REFUSED
                               ? espnowanswer::sharedStatusName(s->status) : nullptr;
      err = webEspNowSendChunkf(req,
        "{\"msgId\":%lu,\"state\":\"%s\",\"mac\":\"%s\",\"ageMs\":%lu%s%s%s}",
        (unsigned long)s->msgId, stateName, macStr, (unsigned long)ageMs,
        reason ? ",\"reason\":\"" : "", reason ? reason : "", reason ? "\"" : "");
    }
    if (err == ESP_OK) err = webEspNowSendChunk(req, "]}");
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
  String sensorList = sensorMaskNames(cap.sensorMask, /*longForm=*/true);
  
  // Build fwHash hex string
  char fwHashHex[33];
  for (int i = 0; i < 16; i++) {
    snprintf(fwHashHex + (i * 2), 3, "%02x", cap.fwHash[i]);
  }
  
  // Build MAC string
  char macStr[18];
  macToDisplay(cap.mac, macStr, sizeof(macStr));
  
  // Stream JSON response (larger due to human-readable strings)
  esp_err_t err = webEspNowSendChunk(req, "{\"valid\":true,");
  if (err != ESP_OK) goto done;
  
  err = webEspNowSendChunkf(req, "\"deviceName\":\"%s\",", cap.deviceName);
  if (err != ESP_OK) goto done;
  err = webEspNowSendChunkf(req, "\"mac\":\"%s\",", macStr);
  if (err != ESP_OK) goto done;
  err = webEspNowSendChunkf(req, "\"role\":%d,", (int)cap.role);
  if (err != ESP_OK) goto done;
  err = webEspNowSendChunkf(req, "\"roleName\":\"%s\",", cap.role == 1 ? "master" : "worker");
  if (err != ESP_OK) goto done;
  err = webEspNowSendChunkf(req, "\"fwHash\":\"%s\",", fwHashHex);
  if (err != ESP_OK) goto done;
  
  // Raw masks
  err = webEspNowSendChunkf(req, "\"featureMask\":%lu,", (unsigned long)cap.featureMask);
  if (err != ESP_OK) goto done;
  err = webEspNowSendChunkf(req, "\"serviceMask\":%lu,", (unsigned long)cap.serviceMask);
  if (err != ESP_OK) goto done;
  err = webEspNowSendChunkf(req, "\"sensorMask\":%lu,", (unsigned long)cap.sensorMask);
  if (err != ESP_OK) goto done;
  
  // Human-readable lists
  err = webEspNowSendChunk(req, "\"features\":");
  if (err != ESP_OK) goto done;
  err = webEspNowSendJsonEscapedString(req, featureList.c_str());
  if (err != ESP_OK) goto done;
  err = webEspNowSendChunk(req, ",\"services\":");
  if (err != ESP_OK) goto done;
  err = webEspNowSendJsonEscapedString(req, serviceList.c_str());
  if (err != ESP_OK) goto done;
  err = webEspNowSendChunk(req, ",\"sensors\":");
  if (err != ESP_OK) goto done;
  err = webEspNowSendJsonEscapedString(req, sensorList.c_str());
  if (err != ESP_OK) goto done;
  
  // Hardware info
  err = webEspNowSendChunkf(req, ",\"flashSizeMB\":%lu,", (unsigned long)cap.flashSizeMb);
  if (err != ESP_OK) goto done;
  err = webEspNowSendChunkf(req, "\"psramSizeMB\":%lu,", (unsigned long)cap.psramSizeMb);
  if (err != ESP_OK) goto done;
  err = webEspNowSendChunkf(req, "\"wifiChannel\":%d,", (int)cap.wifiChannel);
  if (err != ESP_OK) goto done;
  err = webEspNowSendChunkf(req, "\"uptimeSeconds\":%lu,", (unsigned long)cap.uptimeSeconds);
  if (err != ESP_OK) goto done;
  err = webEspNowSendChunkf(req, "\"ageMs\":%lu}", (unsigned long)(millis() - gEspNow->lastRemoteCapTime));
  
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
  
  // If fwHash provided, return that specific manifest. The owner copies the
  // cached file into a buffer under the filesystem lock and validates the hash,
  // so the lock is never held while the response goes out.
  if (strlen(fwHashParam) > 0) {
    char* manifest = nullptr;
    const char* err = nullptr;
    const size_t len = espnowReadCachedManifest(fwHashParam, ctx, &manifest, &err);
    if (len == 0) {
      httpd_resp_send(req, "{\"error\":\"Manifest not found\"}", HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
    
    webEspNowSendChunk(req, "{\"fwHash\":\"");
    webEspNowSendChunk(req, fwHashParam);
    webEspNowSendChunk(req, "\",\"manifest\":");
    httpd_resp_send_chunk(req, manifest, len);
    ps_free(manifest);
    
    webEspNowSendChunk(req, "}");
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
  }
  
  // No fwHash - list all cached manifests
  if (!VFS::existsGuarded(manifestDir, ctx)) {
    httpd_resp_send(req, "{\"manifests\":[]}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  String response = "{\"manifests\":[";
  {
    FsLockGuard fsGuard("web.espnow.manifest_list");
    File dir = VFS::openGuarded(manifestDir, "r", ctx);
    if (!dir || !dir.isDirectory()) {
      httpd_resp_send(req, "{\"manifests\":[]}", HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }

    bool first = true;
    File entry;
    while ((entry = dir.openNextFile())) {
      if (!entry.isDirectory()) {
        String name = entry.name();
        if (name.endsWith(".json")) {
          String fwHash = name.substring(0, name.length() - 5);
          if (!first) response += ',';
          first = false;
          char item[96];
          snprintf(item, sizeof(item), "{\"fwHash\":\"%s\",\"size\":%d}",
                   fwHash.c_str(), (int)entry.size());
          response += item;
        }
      }
      entry.close();
    }
    dir.close();
  }
  response += "]}";
  httpd_resp_send(req, response.c_str(), response.length());
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

// GET /api/espnow/status — one reply string per command, in this order (the
// page's espnowApplyStatusSlots reads them by slot). Slots 0-7 are json and
// are what the page decides from; slot 8 is the human status text, shown in the
// status box and never matched on. Runs as internal system (not the caller),
// so guests can load the page without /api/cli. Each command is submitted to
// the executor (executeInternalStatusCommand), never run on httpd.
static esp_err_t handleEspNowStatus(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  httpd_resp_set_type(req, "application/json");

  static const char* CMDS[] = {
    "espnowstatus json",
    "espnowmode json",
    "bondstatus json",
    "espnowlist",
    "espnowmeshes listjson",
    "espnowdeviceinfo json",
    "espnowmeshrole json",
    "espnowmeshstatus",
    "espnowstatus",
  };

  PSRAM_JSON_DOC(doc);
  doc["success"] = true;
  JsonArray results = doc["results"].to<JsonArray>();
  EXT_RAM_BSS_ATTR static char cmdBuf[CMD_RESULT_MAX];
  for (size_t i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
    (void)executeInternalStatusCommand(CMDS[i], cmdBuf, sizeof(cmdBuf));
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
