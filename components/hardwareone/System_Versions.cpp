// System_Versions.cpp — the `versions` command and the "versions" object of
// `status json`. See System_Versions.h; formatting lives in System_VersionsCore.h.
#include "System_Versions.h"

#include <Arduino.h>
#include <new>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_arduino_version.h"
#include "esp_idf_version.h"

#include "HAL_Radio.h"  // HW1_RADIO_COMPANION, radioHalIdentity
#include "System_BuildConfig.h"
#include "System_MemUtil.h"
#include "System_OTA.h"
#include "System_Utils.h"  // RETURN_VALID_IF_VALIDATE_CSTR
#include "hw1_ota_protocol.h"  // HW1_OTA_MANIFEST_FORMAT_VERSION

#if ENABLE_ESPNOW
#include "System_ESPNow.h"
#include "System_ESPNow_Identity.h"
#include "System_ESPNow_Wire.h"
#endif
#if ENABLE_AUTOMATION
#include "System_Automation.h"  // kAutomationsFileVersion
#endif
#if ENABLE_MIGRATION_TOOL
#include "WebServer_MigrationTool.h"  // kBackupFormatVersion
#endif
#if ENABLE_BLUETOOTH
#include "System_BleSecureChannel.h"
#endif
#if ENABLE_BLUETOOTH && ENABLE_BLE_PHONE_PIPE
#include "BLE_PhonePipeConsoleCore.h"
#include "BLE_PhonePipeCore.h"
#include "BLE_PhoneSecureChannel.h"
#endif
#if ENABLE_RASPBERRY_PI_HOST_POWER || ENABLE_RASPBERRY_PI_HOST_FAN
#include "System_Cm5HostControl.h"
#endif
#if HW1_RADIO_COMPANION
#include "esp_now_hosted_companion.h"
#include "esp_now_hosted_rpc.h"
#endif
#if ENABLE_LOCAL_STT
#include "stt/stt_model_selection.h"
#endif

// Keys are stable (System_VersionsCore.h). A value not readable yet is
// "unknown"; an item whose boundary is not compiled is absent.
void versionsCollect(hw1versions::List& out) {
  out.clear();
  const esp_app_desc_t* app = esp_app_get_description();
  out.add("firmware", "Firmware", "%s", (app && app->version[0]) ? app->version : "unknown");

#if HW1_OTA_LAYOUT
  // The recovery updater in the factory slot and what a signed update must
  // agree with: the layout id, the manifest envelope and the data schema. A
  // manifest's minUpdaterVersion floor is per release and not stored here.
  out.add("ota_layout", "OTA layout", "%s", HW1_OTA_LAYOUT_ID);
  {
    char updater[33] = "";
    const bool known = otaFactoryUpdaterVersion(updater, sizeof(updater));
    out.add("updater", "Recovery updater", "%s", known ? updater : "unavailable");
  }
  out.add("ota_manifest_format", "OTA manifest format", "%u",
          (unsigned)HW1_OTA_MANIFEST_FORMAT_VERSION);
  out.add("ota_data_schema", "OTA data schema", "%u", (unsigned)otaDataSchemaVersion());
#endif

#if ENABLE_ESPNOW
  // The mesh format version (the byte peers compare) and the Espressif
  // ESP-NOW frame class it fits: v1 (250-byte frames) or v2 (1470).
  out.add("espnow", "ESP-NOW", "mesh v%u on ESP-NOW v%d frames", (unsigned)ESPNOW_V4_VERSION,
          (ESPNOW_V4_HEADER_LEN + ESPNOW_V4_MAX_PAYLOAD) <= ESP_NOW_MAX_IE_DATA_LEN ? 1 : 2);
  out.add("espnow_wire", nullptr, "%u", (unsigned)ESPNOW_V4_VERSION);
  {
    // Read at ESP-NOW init on every board (through the bridge on a P4): the
    // companion identity cache predates ESP-NOW and does not carry it.
    const uint32_t stack = espnowStackVersion();
    if (stack) out.add("espnow_stack", "ESP-NOW radio stack", "v%u", (unsigned)stack);
    else out.add("espnow_stack", "ESP-NOW radio stack", "unknown");
  }
  out.add("espnow_identity_file", "ESP-NOW identity file", "%u",
          (unsigned)espnowIdentityFileVersion());
  out.add("espnow_peer_file", "ESP-NOW peer files", "%u", (unsigned)peerIdentityFileVersion());
#endif

#if ENABLE_AUTOMATION
  // The automations.json format this firmware writes and the newest it loads.
  out.add("automations_file", "Automations file", "%u", (unsigned)kAutomationsFileVersion);
#endif
#if ENABLE_MIGRATION_TOOL
  // The HWBACKUP bundle format: what the export writes, the newest restore takes.
  out.add("backup_format", "Backup bundle format", "%u", (unsigned)kBackupFormatVersion);
#endif

#if ENABLE_BLUETOOTH
  out.add("ble_console_channel", "BLE console channel", "%s", bleScProtocolLabel());
#endif
#if ENABLE_BLUETOOTH && ENABLE_BLE_PHONE_PIPE
  out.add("phone_link", "Phone link", "v%u", (unsigned)hw1pipe::kVersion);
  out.add("phone_pipe_channel", "Phone pipe channel", "%s", hw1phone::kPipeLabel);
  out.add("phone_console_records", "Phone console records", "v%u", (unsigned)hw1rec::kInfoVersion);
#endif

#if ENABLE_RASPBERRY_PI_HOST_POWER || ENABLE_RASPBERRY_PI_HOST_FAN
  out.add("cm5_control", "CM5 control protocol", "v%s", cm5HostControlProtocolVersion());
#endif

#if HW1_RADIO_COMPANION
  {
    const RadioCompanionIdentity& id = radioHalIdentity();
    if (id.versionKnown) {
      out.add("c6_hosted", "C6 ESP-Hosted", "v%u.%u.%u", (unsigned)id.major,
              (unsigned)id.minor, (unsigned)id.patch);
    } else {
      out.add("c6_hosted", "C6 ESP-Hosted", "unknown");
    }
    out.add("c6_hosted_qualified", "C6 ESP-Hosted expected", "%s", HW1_C6_HOSTED_TAG);
    if (id.infoKnown) out.add("c6_bridge_wire", "C6 bridge wire", "%u", (unsigned)id.bridgeWire);
    else out.add("c6_bridge_wire", "C6 bridge wire", "unknown");
    out.add("c6_bridge_wire_expected", "C6 bridge wire expected", "%u",
            (unsigned)ESP_NOW_HOSTED_WIRE_VERSION);
  }
#endif

#if ENABLE_LOCAL_STT
  {
    // The model file this firmware loads and the first bytes of the SHA-256
    // of the weights it was generated for (stt_model_identity*.h).
    const char* path = HW1_STT_MODEL_PATH;
    const char* slash = strrchr(path, '/');
    const uint8_t* sha = hw1::stt::identity::kRawSha;
    out.add("stt_model", "Local STT model", "%s sha %02x%02x%02x%02x",
            slash ? slash + 1 : path, sha[0], sha[1], sha[2], sha[3]);
  }
#endif

  out.add("idf", "ESP-IDF", "%s", esp_get_idf_version());
  out.add("arduino", "Arduino core", "%s", ESP_ARDUINO_VERSION_STR);
}

namespace {

// The list is about 2.5 KB: on the heap (PSRAM first), never on a command
// handler's stack.
hw1versions::List* newList() {
  void* memory = ps_alloc(sizeof(hw1versions::List), AllocPolicy::PreferPSRAM, "versions.list");
  return memory ? new (memory) hw1versions::List() : nullptr;
}

void freeList(hw1versions::List* list) {
  if (!list) return;
  list->~List();
  ps_free(list);
}

const char* cmd_versions(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const bool json = argWantsJson(argsInput);
  PSRAM_STATIC_BUF(reply, 3072);
  hw1versions::List* list = newList();
  if (!list) return json ? "{\"error\":\"oom\"}" : "Error: out of memory. Try again.";
  versionsCollect(*list);
  const size_t written = json ? hw1versions::formatJson(*list, reply, reply_SIZE)
                              : hw1versions::formatText(*list, reply, reply_SIZE);
  freeList(list);
  if (!written) return json ? "{\"error\":\"versions too large\"}" : "Error: versions too large.";
  return reply;
}

}  // namespace

void versionsAddJson(JsonObject out) {
  hw1versions::List* list = newList();
  if (!list) return;
  versionsCollect(*list);
  for (size_t i = 0; i < list->count; ++i) {
    // const char*, not the char array: ArduinoJson keeps a pointer to arrays
    // (it takes them for literals) and this list is freed below.
    out[list->items[i].key] = static_cast<const char*>(list->items[i].value);
  }
  freeList(list);
}

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
extern const CommandEntry versionsCommands[] = {
  { "versions", "Show the firmware version and every version a separately built part must match. (add 'json')", false,
    cmd_versions, "Usage: versions [json]" },
};
extern const size_t versionsCommandsCount = sizeof(versionsCommands) / sizeof(versionsCommands[0]);
