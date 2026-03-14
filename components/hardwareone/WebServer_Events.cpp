#include "System_BuildConfig.h"

#if ENABLE_HTTP_SERVER

#include "WebServer_Server.h"
#include "System_User.h"
#include "System_Debug.h"
#include "System_MemUtil.h"
#include <ArduinoJson.h>

// External helpers
extern const char* buildSensorStatusJson();

// Debug helper for SSE
void sseDebug(const String& msg) {
  if (isDebugFlagSet(DEBUG_SSE)) { BROADCAST_PRINTF("[SSE] %s", msg.c_str()); }
}

bool sseWrite(httpd_req_t* req, const char* chunk) {
  if (!req) {
    DEBUG_SSEF("sseWrite called with null req");
    return false;
  }
  if (chunk == NULL) {
    // terminate chunked response
    httpd_resp_send_chunk(req, NULL, 0);
    DEBUG_SSEF("sseWrite: terminated chunked response");
    return true;
  }
  size_t n = strlen(chunk);
  esp_err_t r = httpd_resp_send_chunk(req, chunk, n);
  DEBUG_SSEF("sseWrite: sent chunk bytes=%u %s", (unsigned)n, r == ESP_OK ? "OK" : "FAIL");
  return r == ESP_OK;
}

int sseBindSession(httpd_req_t* req, String& outSid) {
  outSid = getCookieSID(req);
  int idx = findSessionIndexBySID(outSid);
  String ip;
  getClientIP(req, ip);

  DEBUG_SSEF("sseBindSession: IP=%s SID=%.8s%s idx=%d", ip.c_str(), outSid.length() ? outSid.c_str() : "<none>", outSid.length() > 8 ? "..." : "", idx);

  // Validate session still exists and hasn't been cleared
  if (idx >= 0) {
    if (gSessions[idx].sid.length() == 0) {
      DEBUG_SSEF("Session was cleared! Rejecting SSE bind for IP: %s", ip.c_str());
      return -1;  // Session was cleared, reject binding
    }
    // Update socket descriptor for this SSE connection
    int newSockfd = httpd_req_to_sockfd(req);
    if (newSockfd >= 0 && newSockfd != gSessions[idx].sockfd) {
      DEBUG_SSEF("Updating session sockfd from %d to %d", gSessions[idx].sockfd, newSockfd);
      gSessions[idx].sockfd = newSockfd;  // Update to SSE socket
    }
  }

  return idx;
}

bool sseHeartbeat(httpd_req_t* req) {
  DEBUG_SSEF("heartbeat");
  return sseWrite(req, ":hb\n\n");
}

bool sseSendNotice(httpd_req_t* req, const String& note) {
  // Wrap message as JSON string payload
  String safe = note;  // minimal escaping
  safe.replace("\n", "\\n");
  // Build SSE frame in stack buffer instead of String concat
  char sseBuf[512];
  snprintf(sseBuf, sizeof(sseBuf), "event: notice\ndata: {\"msg\":\"%s\"}\n\n", safe.c_str());
  bool ok = sseWrite(req, sseBuf);
  DEBUG_SSEF("sendNotice: len=%u %s", (unsigned)note.length(), ok ? "OK" : "FAIL");
  return ok;
}

// Enqueue a typed SSE event (event name + JSON data) into the session's event queue
void sseEnqueueEvent(SessionEntry& s, const char* eventName, const char* data) {
  if (!eventName || !*eventName || !data) return;
  // Queue-only policy: if full, drop oldest then enqueue new
  const int cap = SessionEntry::EVENT_QUEUE_SIZE;
  // Copy name (truncate if necessary)
  auto copyName = [&](int idx){
    size_t nlen = strnlen(eventName, SessionEntry::EVENT_NAME_MAX - 1);
    memcpy(s.eventNameQ[idx], eventName, nlen);
    s.eventNameQ[idx][nlen] = '\0';
  };
  auto copyData = [&](int idx){
    // Ensure JSON payload fits; truncate if necessary
    size_t dlen = strnlen(data, SessionEntry::EVENT_DATA_MAX - 1);
    memcpy(s.eventDataQ[idx], data, dlen);
    s.eventDataQ[idx][dlen] = '\0';
  };
  if (s.eqCount < cap) {
    copyName(s.eqTail);
    copyData(s.eqTail);
    s.eqTail = (s.eqTail + 1) % cap;
    s.eqCount++;
  } else {
    // Drop oldest
    s.eqHead = (s.eqHead + 1) % cap;
    // Enqueue new at tail
    copyName(s.eqTail);
    copyData(s.eqTail);
    s.eqTail = (s.eqTail + 1) % cap;
    // eqCount remains at capacity
  }
  // Enter burst mode to accelerate delivery
  s.noticeBurstUntil = millis() + 15000UL;
  s.needsNotificationTick = true;
}

// Dequeue next typed SSE event from session queue
bool sseDequeueEvent(SessionEntry& s, String& outEventName, String& outData) {
  if (s.eqCount == 0) return false;
  const int cap = SessionEntry::EVENT_QUEUE_SIZE;
  outEventName = String(s.eventNameQ[s.eqHead]);
  outData = String(s.eventDataQ[s.eqHead]);
  s.eqHead = (s.eqHead + 1) % cap;
  s.eqCount--;
  return true;
}

static bool sseSendFetch(httpd_req_t* req, const String& jsonPayload) {
  // Build SSE frame with snprintf instead of String concat
  size_t needed = 14 + jsonPayload.length() + 3; // "event: fetch\n" + "data: " + payload + "\n\n"
  char stackBuf[512];
  char* buf = stackBuf;
  bool heapAlloc = false;
  if (needed >= sizeof(stackBuf)) {
    buf = (char*)malloc(needed + 1);
    if (!buf) return false;
    heapAlloc = true;
  }
  snprintf(buf, heapAlloc ? (needed + 1) : sizeof(stackBuf), "event: fetch\ndata: %s\n\n", jsonPayload.c_str());
  bool ok = sseWrite(req, buf);
  if (heapAlloc) free(buf);
  DEBUG_SSEF("sendFetch: %s json_len=%u", ok ? "OK" : "FAIL", (unsigned)jsonPayload.length());
  return ok;
}

// SSE endpoint: push per-session notices without polling
esp_err_t handleEvents(httpd_req_t* req) {
  if (!req) {
    DEBUG_SSEF("handleEvents: null req");
    return ESP_OK;
  }
  String u;
  String ip;
  getClientIP(req, ip);
  // httpd_req_t::uri is a fixed-size char array; it is never NULL
  DEBUG_SSEF("handleEvents: incoming from %s, uri=%s", ip.length() ? ip.c_str() : "<no-ip>", req->uri);

  AuthContext ctx = makeWebAuthCtx(req);
  if (!tgRequireAuth(ctx)) {
    DEBUG_AUTHF("/api/events (SSE) DENIED - no valid session for IP: %s", ip.c_str());
    DEBUG_SSEF("handleEvents: auth failed; sending 401");
    return ESP_OK;
  }
  u = ctx.user;
  DEBUG_AUTHF("/api/events (SSE) ALLOWED for user: %s from IP: %s", u.c_str(), ip.c_str());

  // Prepare SSE headers
  httpd_resp_set_type(req, "text/event-stream");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_set_hdr(req, "Connection", "keep-alive");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Credentials", "true");
  // Disable proxy buffering if any
  httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");
  DEBUG_SSEF("handleEvents: SSE headers set");

  // Bind session BEFORE sending any body chunks
  String sid;
  int sessIdx = sseBindSession(req, sid);
  if (sessIdx < 0) {
    DEBUG_SSEF("handleEvents: no session bound; closing");
    sseWrite(req, NULL);
    return ESP_OK;
  }
  DEBUG_SSEF("handleEvents: bound session idx=%d, sid=%.8s%s", sessIdx, sid.length() ? sid.c_str() : "<none>", sid.length() > 8 ? "..." : "");
  DEBUG_SSEF("handleEvents: bound session details | idx=%d needsStatusUpdate=%d lastSensorSeqSent=%lu",
             sessIdx,
             gSessions[sessIdx].needsStatusUpdate ? 1 : 0,
             (unsigned long)gSessions[sessIdx].lastSensorSeqSent);

  // Advise browser to backoff reconnects and send initial comment to open stream
  unsigned long nowRetry = millis();
  unsigned long retryMs = (gSessions[sessIdx].needsNotificationTick || (nowRetry < gSessions[sessIdx].noticeBurstUntil)) ? 1000UL : 5000UL;
  char retryLine[32];
  snprintf(retryLine, sizeof(retryLine), "retry: %lu\n\n", (unsigned long)retryMs);
  if (!sseWrite(req, retryLine)) {
    DEBUG_SSEF("handleEvents: failed to send retry hint");
    return ESP_OK;
  }
  if (!sseWrite(req, ":ok\n\n")) {
    DEBUG_SSEF("handleEvents: failed to send initial :ok");
    return ESP_OK;
  }
  DEBUG_SSEF("handleEvents: initial :ok sent");

  DEBUG_SSEF("SSE connection established");

  // Immediately push a 'sensor-status' (if needed) and a 'system' snapshot, then keep streaming
  auto sendStatus = [&](const char* reason) {
    const char* statusJson = buildSensorStatusJson();
    char eventData[1200];
    snprintf(eventData, sizeof(eventData), "event: sensor-status\ndata: %s\n\n", statusJson);
    if (isDebugFlagSet(DEBUG_SSE)) {
      DEBUG_SSEF("Sending 'sensor-status' (%zu bytes) reason=%s", strlen(eventData), reason);
    }
    if (sseWrite(req, eventData)) {
      gSessions[sessIdx].needsStatusUpdate = false;
      gSessions[sessIdx].lastSensorSeqSent = gSensorStatusSeq;
    }
  };

  auto sendSystem = [&]() {
    PSRAM_JSON_DOC(doc);
    buildSystemInfoJson(doc);

    static char sysJsonBuf[1024];
    size_t len = serializeJson(doc, sysJsonBuf, sizeof(sysJsonBuf));

    DEBUG_SSEF("Sending system event snapshot (%d bytes json)", len);
    if (len > 0 && len < 80) {
      DEBUG_SSEF("SSE->system json: %s", sysJsonBuf);
    } else if (len >= 80) {
      DEBUG_SSEF("SSE->system json: %.80s...", sysJsonBuf);
    }

    static char sseEventBuf[1100];
    snprintf(sseEventBuf, sizeof(sseEventBuf), "event: system\ndata: %s\n\n", sysJsonBuf);
    sseWrite(req, sseEventBuf);
  };

  if (gSessions[sessIdx].needsStatusUpdate) {
    sendStatus("refresh");
    sendSystem();
  }

  bool wantHold = gSessions[sessIdx].needsNotificationTick || (gSessions[sessIdx].nqCount > 0) || (gSessions[sessIdx].eqCount > 0);
  if (wantHold) {
    unsigned long holdStart = millis();
    const unsigned long holdMs = 600UL;
    while ((long)(millis() - holdStart) < (long)holdMs) {
      String n;
      int sent = 0;
      while (sseDequeueNotice(gSessions[sessIdx], n)) {
        DEBUG_SSEF("SSE notice tick send: %s", n.c_str());
        if (!sseSendNotice(req, n)) {
          DEBUG_SSEF("SSE write failed while sending notice; closing");
          holdStart = 0;
          break;
        }
        sent++;
        if (sent >= 8) break;
      }
      // Also flush typed events (e.g., espnow-rx)
      int evSent = 0;
      while (true) {
        String evName, evData;
        if (!sseDequeueEvent(gSessions[sessIdx], evName, evData)) break;
        char lineBuf[256];
        // Compose: event: <name>\ndata: <json>\n\n
        // Truncate if necessary
        size_t needed = 8 + evName.length() + 7 + evData.length() + 2; // rough
        if (needed >= sizeof(lineBuf)) {
          // Best effort: trim data
          evData = evData.substring(0, 160);
        }
        int nlen = snprintf(lineBuf, sizeof(lineBuf), "event: %s\ndata: %s\n\n", evName.c_str(), evData.c_str());
        (void)nlen;
        if (!sseWrite(req, lineBuf)) {
          DEBUG_SSEF("SSE write failed while sending event '%s'; closing", evName.c_str());
          holdStart = 0;
          break;
        }
        evSent++;
        if (evSent >= 8) break;
      }
      delay(60);
    }
  }

  if (gSessions[sessIdx].nqCount == 0 && gSessions[sessIdx].eqCount == 0) {
    gSessions[sessIdx].needsNotificationTick = false;
  }

  sseWrite(req, NULL);
  return ESP_OK;
}

#endif // ENABLE_HTTP_SERVER
