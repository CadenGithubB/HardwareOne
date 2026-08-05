#include "System_BuildConfig.h"

#if ENABLE_WEB_BLUETOOTH

#include <Arduino.h>

#include "WebPage_Bluetooth.h"
#include "WebServer_Server.h"
#include "WebServer_Utils.h"
#include "System_User.h"
#include "System_Settings.h"
#include <ArduinoJson.h>
#include "System_Utils.h"
#include "System_CommandTypes.h"
#include "System_MemUtil.h"
#if ENABLE_G2_GLASSES
#include "G2_Glasses.h"
#endif

static void streamBluetoothContent(httpd_req_t* req, const String& username) {
  streamBeginHtml(req, "Bluetooth", false, username, "bluetooth");
  httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
  streamBluetoothInner(req);
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
  streamEndHtml(req);
}

esp_err_t handleBluetoothPage(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  streamPageWithContent(req, "bluetooth", ctx.user, streamBluetoothContent);
  return ESP_OK;
}

static bool bleRunInternal(const char* cmd, char* out, size_t outSize) {
  AuthContext sys;
  sys.transport = SOURCE_INTERNAL;
  sys.user = "system";
  sys.ip = "local";
  sys.path = cmd;
  out[0] = '\0';
  return executeCommand(sys, cmd, out, outSize);
}

#if ENABLE_G2_GLASSES
static const char* g2DesiredJsonName(G2ControlDesired value) {
  switch (value) {
    case G2_CONTROL_PRESERVE: return "preserve";
    case G2_CONTROL_OFF: return "off";
    case G2_CONTROL_ON: return "on";
    default: return "unknown";
  }
}

static const char* g2ObservedJsonName(G2ControlObserved value) {
  switch (value) {
    case G2_CONTROL_OBS_OFF: return "off";
    case G2_CONTROL_OBS_ON: return "on";
    case G2_CONTROL_UNKNOWN:
    default: return "unknown";
  }
}

static const char* g2PhaseJsonName(G2ControlPhase value) {
  switch (value) {
    case G2_CONTROL_IDLE: return "idle";
    case G2_CONTROL_WAITING_LINK: return "waiting-link";
    case G2_CONTROL_WAITING_FIRMWARE: return "waiting-firmware";
    case G2_CONTROL_QUEUED: return "queued";
    case G2_CONTROL_SENT: return "sent";
    case G2_CONTROL_ACKED: return "acked";
    case G2_CONTROL_VERIFIED: return "verified";
    case G2_CONTROL_UNSUPPORTED: return "unsupported";
    case G2_CONTROL_FAILED: return "failed";
    default: return "unknown";
  }
}

static void addG2FeatureJson(JsonObject out,
                             const G2ControlFeatureStatus& feature) {
  out["desired"] = g2DesiredJsonName(feature.desired);
  out["observed"] = g2ObservedJsonName(feature.observed);
  out["phase"] = g2PhaseJsonName(feature.phase);
  out["detail"] = feature.detail;
  out["changedAtMs"] = feature.changedAtMs;
  out["appliedGeneration"] = feature.appliedGeneration;
}
#endif

// GET /api/ble/status — mode + bleinfo + optional g2/ring text for the BT page.
static esp_err_t handleBleStatus(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  httpd_resp_set_type(req, "application/json");

  const char* mode = (gSettings.bleMode == 1) ? "client" : "server";
  EXT_RAM_BSS_ATTR static char bleJson[CMD_RESULT_MAX];
  bleRunInternal("bleinfo json", bleJson, sizeof(bleJson));

  PSRAM_JSON_DOC(doc);
  doc["schema"] = 1;
  doc["mode"] = mode;
  {
    PSRAM_JSON_DOC(ble);
    if (!deserializeJson(ble, bleJson)) {
      doc["ble"] = ble;
    } else {
      doc["bleRaw"] = bleJson;
    }
  }

#if ENABLE_G2_GLASSES
  {
    char g2Buf[256];
    getG2Status(g2Buf, sizeof(g2Buf));
    doc["g2Text"] = g2Buf;

    G2ControlStatus status{};
    if (g2ControlStatusSnapshot(&status)) {
      JsonObject g2 = doc["g2"].to<JsonObject>();
      g2["revision"] = status.revision;
      g2["rightGeneration"] = status.rightGeneration;
      JsonObject firmware = g2["firmware"].to<JsonObject>();
      firmware["left"] = status.firmwareLeft;
      firmware["right"] = status.firmwareRight;
      JsonObject rx = g2["rx"].to<JsonObject>();
      rx["queued"] = status.rxQueued;
      rx["drops"] = status.rxDrops;
      JsonObject controls = g2["controls"].to<JsonObject>();
      addG2FeatureJson(controls["headUp"].to<JsonObject>(), status.headUp);
      addG2FeatureJson(controls["notifications"].to<JsonObject>(),
                       status.notifications);
    }
  }
  {
    EXT_RAM_BSS_ATTR static char ringBuf[512];
    bleRunInternal("ringstatus json", ringBuf, sizeof(ringBuf));
    PSRAM_JSON_DOC(ring);
    if (!deserializeJson(ring, ringBuf)) {
      doc["ring"] = ring;
    } else {
      // Fallback text form for older parsers
      bleRunInternal("ringstatus", ringBuf, sizeof(ringBuf));
      doc["ringText"] = ringBuf;
    }
  }
#endif

  String out;
  serializeJson(doc, out);
  httpd_resp_send(req, out.c_str(), out.length());
  return ESP_OK;
}

void registerBluetoothHandlers(httpd_handle_t server) {
  static const httpd_uri_t bluetoothPage = { 
    .uri = "/bluetooth", 
    .method = HTTP_GET, 
    .handler = handleBluetoothPage, 
    .user_ctx = NULL 
  };
  httpd_register_uri_handler(server, &bluetoothPage);
  static const httpd_uri_t bleStatus = {
    .uri = "/api/ble/status",
    .method = HTTP_GET,
    .handler = handleBleStatus,
    .user_ctx = NULL
  };
  httpd_register_uri_handler(server, &bleStatus);
}

#endif // ENABLE_HTTP_SERVER
