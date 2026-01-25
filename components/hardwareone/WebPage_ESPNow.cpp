#include "System_BuildConfig.h"

#if ENABLE_HTTP_SERVER

#include <Arduino.h>

#include "System_User.h"
#include "WebPage_ESPNow.h"
#include "WebServer_Server.h"
#include "WebServer_Utils.h"

// Forward declarations
extern bool isAuthed(httpd_req_t* req, String& outUser);
extern void logAuthAttempt(bool success, const char* path, const String& userTried, const String& ip, const String& reason);
extern void streamPageWithContent(httpd_req_t* req, const String& activePage, const String& username, void (*contentStreamer)(httpd_req_t*));
extern void streamBeginHtml(httpd_req_t* req, const char* title, bool isPublic, const String& username, const String& activePage);
extern void streamEndHtml(httpd_req_t* req);

static void streamEspNowContent(httpd_req_t* req) {
  String u;
  isAuthed(req, u);
  streamBeginHtml(req, "ESP-NOW", false, u, "espnow");
  httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
  streamEspNowInner(req);
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
  streamEndHtml(req);
}

static esp_err_t handleEspNowPage(httpd_req_t* req) {
  AuthContext ctx;
  ctx.transport = SOURCE_WEB;
  ctx.opaque = req;
  ctx.path = req ? req->uri : "/espnow";
  getClientIP(req, ctx.ip);
  if (!tgRequireAuth(ctx)) return ESP_OK;
  logAuthAttempt(true, req->uri, ctx.user, ctx.ip, "");

  streamPageWithContent(req, "espnow", ctx.user, streamEspNowContent);
  return ESP_OK;
}

// =============================================================================
// ESP-NOW API Endpoints
// =============================================================================

#if ENABLE_ESPNOW

#include "System_ESPNow.h"
#include "System_MemUtil.h"

extern EspNowState* gEspNow;
extern void* ps_alloc(size_t size, AllocPref pref, const char* tag);

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

/**
 * @brief Fetch received ESP-NOW text messages since lastSeq
 * @param req HTTP request (query param: ?since=<seqNum>)
 * @return ESP_OK
 * 
 * Returns JSON array of messages:
 * {
 *   "messages": [
 *     {"seq":123,"mac":"XX:XX:XX:XX:XX:XX","name":"device","msg":"text","enc":true,"ts":12345},
 *     ...
 *   ]
 * }
 */
static esp_err_t handleEspNowMessages(httpd_req_t* req) {
  AuthContext ctx;
  ctx.transport = SOURCE_WEB;
  ctx.opaque = req;
  ctx.path = "/api/espnow/messages";
  getClientIP(req, ctx.ip);
  
  if (!tgRequireAuth(ctx)) {
    return ESP_OK;
  }
  
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
      // Parse MAC address (format: AA:BB:CC:DD:EE:FF or AABBCCDDEEFF)
      if (strlen(paramBuf) >= 12) {
        hasMacFilter = true;
        sscanf(paramBuf, "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
               &filterMac[0], &filterMac[1], &filterMac[2],
               &filterMac[3], &filterMac[4], &filterMac[5]);
      }
    }
  }
  
  // Allocate temporary buffer for messages (max 100 messages)
  ReceivedTextMessage* messages = (ReceivedTextMessage*)ps_alloc(sizeof(ReceivedTextMessage) * 100,
                                                                AllocPref::PreferPSRAM,
                                                                "web.esnow.msgs");
  if (!messages) {
    httpd_resp_send(req, "{\"messages\":[]}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  
  // Get messages from per-device buffers
  int msgCount = 0;
  if (hasMacFilter) {
    // Get messages from specific peer
    msgCount = getPeerMessages(filterMac, messages, 100, sinceSeq);
  } else {
    // Get all messages from all peers
    msgCount = getAllMessages(messages, 100, sinceSeq);
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

    err = webEspnowSendChunk(req, "}");
  }

  free(messages);

  if (err == ESP_OK) {
    err = webEspnowSendChunk(req, "]}");
  }
  httpd_resp_send_chunk(req, NULL, 0);
  return err;
}

#endif // ENABLE_ESPNOW

// =============================================================================
// Register ESP-NOW Handlers
// =============================================================================

void registerEspNowHandlers(httpd_handle_t server) {
  static httpd_uri_t espnowPage = { .uri = "/espnow", .method = HTTP_GET, .handler = handleEspNowPage, .user_ctx = NULL };
  httpd_register_uri_handler(server, &espnowPage);
  
#if ENABLE_ESPNOW
  static httpd_uri_t espnowMessages = { .uri = "/api/espnow/messages", .method = HTTP_GET, .handler = handleEspNowMessages, .user_ctx = NULL };
  httpd_register_uri_handler(server, &espnowMessages);
#endif
}

#endif // ENABLE_HTTP_SERVER
