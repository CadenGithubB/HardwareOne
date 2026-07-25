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
