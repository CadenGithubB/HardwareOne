#include "System_BuildConfig.h"

#if ENABLE_WEB_BLUETOOTH

#include <Arduino.h>

#include "WebPage_Bluetooth.h"
#include "WebServer_Server.h"
#include "WebServer_Utils.h"
#include "System_User.h"
#include "System_Settings.h"
#include <ArduinoJson.h>
#include "Bluetooth.h"
#include "System_MemUtil.h"
#if ENABLE_G2_GLASSES
#include "BLE_Peers.h"
#include "G2_Glasses.h"
#include "G2_Ring.h"
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
  // feature belongs to a temporary status snapshot; JSON must own this text.
  out["detail"] = JsonString(feature.detail, false);
  out["changedAtMs"] = feature.changedAtMs;
  out["appliedGeneration"] = feature.appliedGeneration;
}

static void addG2TempleConnectionJson(JsonObject out,
                                     const G2TempleConnectionStatus& status) {
  out["connected"] = status.connected;
  out["mtu"] = status.mtu;
  out["battery"] = status.battery;
  out["tx"] = status.tx;
  out["rx"] = status.rx;
}
#endif

// GET /api/ble/status — owner-built BLE info + optional G2/ring status.
static esp_err_t handleBleStatus(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  httpd_resp_set_type(req, "application/json");

  const char* mode = getBleModeString();
  PSRAM_JSON_DOC(doc);
  doc["schema"] = 1;
  doc["mode"] = mode;
  const bool available = buildBLEInfoJson(doc["ble"].to<JsonObject>());
  if (doc.overflowed()) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, "{\"error\":\"Out of memory\"}", HTTPD_RESP_USE_STRLEN);
  }
  if (!available) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "{\"error\":\"BLE status unavailable\"}", HTTPD_RESP_USE_STRLEN);
  }

#if ENABLE_G2_GLASSES
  {
    G2ConnectionStatus connection{};
    if (!g2ConnectionStatusSnapshot(&connection)) {
      httpd_resp_set_status(req, "503 Service Unavailable");
      return httpd_resp_send(req, "{\"error\":\"G2 status unavailable\"}", HTTPD_RESP_USE_STRLEN);
    }
    JsonObject g2 = doc["g2"].to<JsonObject>();
    g2["autoReconnect"] = blePeerAutoReconnectEnabled(BLE_PEER_G2_GLASSES);
    JsonObject link = g2["connection"].to<JsonObject>();
    link["running"] = connection.running;
    link["state"] = connection.state;
    addG2TempleConnectionJson(link["left"].to<JsonObject>(), connection.left);
    addG2TempleConnectionJson(link["right"].to<JsonObject>(), connection.right);
    char g2Buf[256];
    formatG2Status(connection, g2Buf, sizeof(g2Buf));
    doc["g2Text"] = g2Buf;

    G2ControlStatus status{};
    if (g2ControlStatusSnapshot(&status)) {
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
  const bool ringAvailable = g2RingBuildStatusJson(doc["ring"].to<JsonObject>());
  if (!ringAvailable && !doc.overflowed()) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "{\"error\":\"Ring status unavailable\"}", HTTPD_RESP_USE_STRLEN);
  }
  doc["ring"]["autoReconnect"] = blePeerAutoReconnectEnabled(BLE_PEER_R1_RING);
#endif

  String out;
  if (!doc.overflowed()) serializeJson(doc, out);
  // ArduinoJson's String writer can report accepted bytes before its final
  // flush. Check the resulting length so allocation failure cannot publish a
  // truncated status document as a successful response.
  if (doc.overflowed() || out.length() != measureJson(doc)) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, "{\"error\":\"Out of memory\"}", HTTPD_RESP_USE_STRLEN);
  }
  return httpd_resp_send(req, out.c_str(), out.length());
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

#endif // ENABLE_WEB_BLUETOOTH
