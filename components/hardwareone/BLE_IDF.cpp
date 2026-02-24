#include "BLE_IDF.h"

#if ENABLE_BLUETOOTH

#include <Arduino.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "System_Command.h"
#include "System_Debug.h"

static const char* TAG = "BLE_IDF";

// =============================================================================
// INTERNAL STATE
// =============================================================================

static struct {
  bool initialized;
  BLEMode mode;
  BLEConnectionState state;
  
  // GATTS (Server) state
  esp_gatt_if_t gatts_if;
  uint16_t app_id;
  
  // Service handles
  uint16_t device_info_service_handle;
  uint16_t command_service_handle;
  uint16_t data_service_handle;
  
  // Device Info Service characteristic handles
  uint16_t manufacturer_handle;
  uint16_t model_handle;
  uint16_t firmware_handle;
  
  // Command Service characteristic handles
  uint16_t cmd_request_handle;
  uint16_t cmd_response_handle;
  uint16_t cmd_status_handle;
  
  // Data Service characteristic handles
  uint16_t sensor_data_handle;
  uint16_t system_status_handle;
  uint16_t event_notify_handle;
  uint16_t stream_control_handle;
  
  // Extended advertising
  uint8_t adv_instance;
  esp_ble_gap_ext_adv_params_t ext_adv_params;
  
  // Service creation state machine
  uint8_t service_creation_step;
  
  // GATTC (Client) state
  esp_gatt_if_t gattc_if;
  uint16_t gattc_conn_id;
  esp_bd_addr_t remote_bda;
  
  // Connection tracking
  BLEConnection connections[BLE_MAX_CONNECTIONS];
  int connectionCount;
  
  // Stream control
  uint8_t streamFlags;
  uint32_t sensorStreamInterval;
  uint32_t systemStreamInterval;
  uint32_t lastSensorStream;
  uint32_t lastSystemStream;
  
  // Callbacks
  void (*onCommandReceived)(uint16_t connId, const uint8_t* data, size_t len);
  
} gBleState = {};

// =============================================================================
// UTILITY FUNCTIONS
// =============================================================================

static void bleLogAddr(const char* prefix, const esp_bd_addr_t addr) {
  ESP_LOGI(TAG, "%s %02x:%02x:%02x:%02x:%02x:%02x",
    prefix, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

static BLEConnection* bleFindConnection(uint16_t connId) {
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    if (gBleState.connections[i].active && gBleState.connections[i].connId == connId) {
      return &gBleState.connections[i];
    }
  }
  return NULL;
}

static BLEConnection* bleAllocConnection(uint16_t connId, uint16_t gatts_if, const esp_bd_addr_t bda) {
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    if (!gBleState.connections[i].active) {
      BLEConnection* conn = &gBleState.connections[i];
      memset(conn, 0, sizeof(BLEConnection));
      conn->active = true;
      conn->connId = connId;
      conn->gatts_if = gatts_if;
      memcpy(conn->remote_bda, bda, sizeof(esp_bd_addr_t));
      conn->connectedSince = millis();
      conn->lastActivityMs = millis();
      gBleState.connectionCount++;
      return conn;
    }
  }
  return NULL;
}

static void bleFreeConnection(uint16_t connId) {
  BLEConnection* conn = bleFindConnection(connId);
  if (conn) {
    conn->active = false;
    gBleState.connectionCount--;
  }
}

// Parse 128-bit UUID string "12345678-1234-5678-1234-56789abcdef0" to esp_bt_uuid_t
static bool bleParseUuid128(const char* uuidStr, esp_bt_uuid_t* uuid) {
  if (!uuidStr || !uuid) return false;
  
  // Remove dashes and parse hex
  uint8_t uuid128[16];
  int idx = 0;
  
  for (int i = 0; uuidStr[i] && idx < 16; i++) {
    if (uuidStr[i] == '-') continue;
    
    char hex[3] = {uuidStr[i], uuidStr[i+1], 0};
    uuid128[15 - idx] = (uint8_t)strtol(hex, NULL, 16);
    idx++;
    i++;
  }
  
  if (idx != 16) return false;
  
  uuid->len = ESP_UUID_LEN_128;
  memcpy(uuid->uuid.uuid128, uuid128, 16);
  return true;
}

// =============================================================================
// GAP EVENT HANDLER
// =============================================================================

static void bleGapEventHandler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
  switch (event) {
    // Extended advertising events
    case ESP_GAP_BLE_EXT_ADV_SET_PARAMS_COMPLETE_EVT:
      ESP_LOGI(TAG, "GAP: Extended adv params set, status=%d instance=%d",
        param->ext_adv_set_params.status, param->ext_adv_set_params.instance);
      
      if (param->ext_adv_set_params.status == ESP_BT_STATUS_SUCCESS) {
        // Build advertising data
        uint8_t adv_data[31];
        uint8_t adv_len = 0;
        
        // Flags
        adv_data[adv_len++] = 2;
        adv_data[adv_len++] = ESP_BLE_AD_TYPE_FLAG;
        adv_data[adv_len++] = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT;
        
        // Complete local name
        const char* name = "HardwareOne";
        uint8_t name_len = strlen(name);
        adv_data[adv_len++] = name_len + 1;
        adv_data[adv_len++] = ESP_BLE_AD_TYPE_NAME_CMPL;
        memcpy(&adv_data[adv_len], name, name_len);
        adv_len += name_len;
        
        esp_ble_gap_config_ext_adv_data_raw(gBleState.adv_instance, adv_len, adv_data);
      }
      break;
      
    case ESP_GAP_BLE_EXT_ADV_DATA_SET_COMPLETE_EVT:
      ESP_LOGI(TAG, "GAP: Extended adv data set, status=%d",
        param->ext_adv_data_set.status);
      
      if (param->ext_adv_data_set.status == ESP_BT_STATUS_SUCCESS) {
        // Start extended advertising
        esp_ble_gap_ext_adv_t ext_adv[1];
        ext_adv[0].instance = gBleState.adv_instance;
        ext_adv[0].duration = 0;  // Continuous
        ext_adv[0].max_events = 0;  // No limit
        
        esp_ble_gap_ext_adv_start(1, ext_adv);
      }
      break;
      
    case ESP_GAP_BLE_EXT_ADV_START_COMPLETE_EVT:
      ESP_LOGI(TAG, "GAP: Extended adv start, status=%d",
        param->ext_adv_start.status);
      
      if (param->ext_adv_start.status == ESP_BT_STATUS_SUCCESS) {
        gBleState.state = BLE_STATE_ADVERTISING;
        ESP_LOGI(TAG, "Extended advertising started successfully");
      } else {
        ESP_LOGE(TAG, "Extended advertising start failed");
      }
      break;
      
    case ESP_GAP_BLE_EXT_ADV_STOP_COMPLETE_EVT:
      ESP_LOGI(TAG, "GAP: Extended adv stopped, status=%d",
        param->ext_adv_stop.status);
      gBleState.state = BLE_STATE_IDLE;
      break;
      
    case ESP_GAP_BLE_ADV_TERMINATED_EVT:
      ESP_LOGI(TAG, "GAP: Advertising terminated, status=0x%02x instance=%d conn_idx=%d",
        param->adv_terminate.status, param->adv_terminate.adv_instance,
        param->adv_terminate.conn_idx);
      
      if (param->adv_terminate.status == 0x00) {
        // Connected
        gBleState.state = BLE_STATE_CONNECTED;
      }
      break;
      
    // Scan events (for G2 client)
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
      if (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "GAP: Scan started");
        gBleState.state = BLE_STATE_SCANNING;
      } else {
        ESP_LOGE(TAG, "GAP: Scan start failed: %d", param->scan_start_cmpl.status);
      }
      break;
      
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
      esp_ble_gap_cb_param_t* scan_result = param;
      switch (scan_result->scan_rst.search_evt) {
        case ESP_GAP_SEARCH_INQ_RES_EVT:
          // Scan result - will be handled by G2 client code
          break;
        case ESP_GAP_SEARCH_INQ_CMPL_EVT:
          ESP_LOGI(TAG, "GAP: Scan complete");
          gBleState.state = BLE_STATE_IDLE;
          break;
        default:
          break;
      }
      break;
    }
    
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
      ESP_LOGI(TAG, "GAP: Connection params updated - status=%d latency=%d timeout=%d",
        param->update_conn_params.status,
        param->update_conn_params.latency,
        param->update_conn_params.timeout);
      break;
      
    default:
      ESP_LOGD(TAG, "GAP: Unhandled event %d", event);
      break;
  }
}

// =============================================================================
// GATTS (SERVER) EVENT HANDLER
// =============================================================================

static void bleGattsEventHandler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t* param) {
  switch (event) {
    case ESP_GATTS_REG_EVT:
      ESP_LOGI(TAG, "GATTS: App registered, if=%d status=%d app_id=%d",
        gatts_if, param->reg.status, param->reg.app_id);
      if (param->reg.status == ESP_GATT_OK) {
        gBleState.gatts_if = gatts_if;
        gBleState.app_id = param->reg.app_id;
        gBleState.service_creation_step = 0;
        
        // Create Device Info Service (0x180A) - standard 16-bit UUID
        esp_gatt_srvc_id_t service_id;
        service_id.is_primary = true;
        service_id.id.inst_id = 0;
        service_id.id.uuid.len = ESP_UUID_LEN_16;
        service_id.id.uuid.uuid.uuid16 = BLE_DEVICE_INFO_SERVICE_UUID;
        
        esp_err_t ret = esp_ble_gatts_create_service(gatts_if, &service_id, 10);
        if (ret != ESP_OK) {
          ESP_LOGE(TAG, "Create Device Info service failed: %s", esp_err_to_name(ret));
        }
      }
      break;
      
    case ESP_GATTS_CREATE_EVT: {
      ESP_LOGI(TAG, "GATTS: Service created, status=%d handle=%d step=%d",
        param->create.status, param->create.service_handle, gBleState.service_creation_step);
      
      if (param->create.status != ESP_GATT_OK) {
        ESP_LOGE(TAG, "Service creation failed");
        break;
      }
      
      esp_bt_uuid_t char_uuid;
      esp_attr_value_t char_val = {};
      esp_attr_control_t control = {.auto_rsp = ESP_GATT_AUTO_RSP};
      
      switch (gBleState.service_creation_step) {
        case 0: // Device Info Service created
          gBleState.device_info_service_handle = param->create.service_handle;
          
          // Add Manufacturer characteristic (Read)
          char_uuid.len = ESP_UUID_LEN_16;
          char_uuid.uuid.uuid16 = BLE_MANUFACTURER_CHAR_UUID;
          esp_ble_gatts_add_char(gBleState.device_info_service_handle, &char_uuid,
            ESP_GATT_PERM_READ, ESP_GATT_CHAR_PROP_BIT_READ, &char_val, &control);
          break;
          
        case 1: // Command Service created
          gBleState.command_service_handle = param->create.service_handle;
          
          // Add CMD_REQUEST characteristic (Write)
          if (!bleParseUuid128(BLE_CMD_REQUEST_CHAR_UUID, &char_uuid)) {
            ESP_LOGE(TAG, "Failed to parse CMD_REQUEST UUID");
            break;
          }
          control.auto_rsp = ESP_GATT_RSP_BY_APP;
          esp_ble_gatts_add_char(gBleState.command_service_handle, &char_uuid,
            ESP_GATT_PERM_WRITE, 
            ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR,
            &char_val, &control);
          break;
          
        case 2: // Data Service created
          gBleState.data_service_handle = param->create.service_handle;
          
          // Add SENSOR_DATA characteristic (Notify)
          if (!bleParseUuid128(BLE_SENSOR_DATA_CHAR_UUID, &char_uuid)) {
            ESP_LOGE(TAG, "Failed to parse SENSOR_DATA UUID");
            break;
          }
          control.auto_rsp = ESP_GATT_AUTO_RSP;
          esp_ble_gatts_add_char(gBleState.data_service_handle, &char_uuid,
            ESP_GATT_PERM_READ, ESP_GATT_CHAR_PROP_BIT_NOTIFY, &char_val, &control);
          break;
      }
    } break;
      
    case ESP_GATTS_ADD_CHAR_EVT: {
      ESP_LOGI(TAG, "GATTS: Characteristic added, status=%d handle=%d",
        param->add_char.status, param->add_char.attr_handle);
      
      if (param->add_char.status != ESP_GATT_OK) break;
      
      uint16_t handle = param->add_char.attr_handle;
      esp_bt_uuid_t descr_uuid;
      esp_attr_value_t descr_val = {};
      esp_attr_control_t control = {.auto_rsp = ESP_GATT_AUTO_RSP};
      
      // Track which characteristic was just added based on service_creation_step
      // Device Info Service (step 0): manufacturer, model, firmware
      if (gBleState.service_creation_step == 0) {
        if (gBleState.manufacturer_handle == 0) {
          gBleState.manufacturer_handle = handle;
          // Add Model characteristic
          esp_bt_uuid_t char_uuid;
          char_uuid.len = ESP_UUID_LEN_16;
          char_uuid.uuid.uuid16 = BLE_MODEL_CHAR_UUID;
          esp_attr_value_t cval1 = {};
          esp_ble_gatts_add_char(gBleState.device_info_service_handle, &char_uuid,
            ESP_GATT_PERM_READ, ESP_GATT_CHAR_PROP_BIT_READ, &cval1, &control);
        } else if (gBleState.model_handle == 0) {
          gBleState.model_handle = handle;
          // Add Firmware characteristic
          esp_bt_uuid_t char_uuid;
          char_uuid.len = ESP_UUID_LEN_16;
          char_uuid.uuid.uuid16 = BLE_FIRMWARE_CHAR_UUID;
          esp_attr_value_t cval2 = {};
          esp_ble_gatts_add_char(gBleState.device_info_service_handle, &char_uuid,
            ESP_GATT_PERM_READ, ESP_GATT_CHAR_PROP_BIT_READ, &cval2, &control);
        } else {
          gBleState.firmware_handle = handle;
          // All Device Info chars added, start service
          esp_ble_gatts_start_service(gBleState.device_info_service_handle);
        }
      }
      // Command Service (step 1): cmd_request, cmd_response, cmd_status
      else if (gBleState.service_creation_step == 1) {
        if (gBleState.cmd_request_handle == 0) {
          gBleState.cmd_request_handle = handle;
          // Add CMD_RESPONSE characteristic (Notify)
          esp_bt_uuid_t char_uuid;
          if (bleParseUuid128(BLE_CMD_RESPONSE_CHAR_UUID, &char_uuid)) {
            esp_attr_value_t cval3 = {};
            esp_ble_gatts_add_char(gBleState.command_service_handle, &char_uuid,
              ESP_GATT_PERM_READ, ESP_GATT_CHAR_PROP_BIT_NOTIFY, &cval3, &control);
          }
        } else if (gBleState.cmd_response_handle == 0) {
          gBleState.cmd_response_handle = handle;
          // Add CCCD descriptor for notifications
          descr_uuid.len = ESP_UUID_LEN_16;
          descr_uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
          esp_ble_gatts_add_char_descr(gBleState.command_service_handle, &descr_uuid,
            ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, &descr_val, &control);
        } else if (gBleState.cmd_status_handle == 0) {
          gBleState.cmd_status_handle = handle;
          // All Command chars added, start service
          esp_ble_gatts_start_service(gBleState.command_service_handle);
        }
      }
      // Data Service (step 2): sensor_data, system_status, event_notify, stream_control
      else if (gBleState.service_creation_step == 2) {
        if (gBleState.sensor_data_handle == 0) {
          gBleState.sensor_data_handle = handle;
          // Add CCCD descriptor
          descr_uuid.len = ESP_UUID_LEN_16;
          descr_uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
          esp_ble_gatts_add_char_descr(gBleState.data_service_handle, &descr_uuid,
            ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, &descr_val, &control);
        } else if (gBleState.system_status_handle == 0) {
          gBleState.system_status_handle = handle;
          // Add EVENT_NOTIFY characteristic (Notify)
          esp_bt_uuid_t char_uuid;
          if (bleParseUuid128(BLE_EVENT_NOTIFY_CHAR_UUID, &char_uuid)) {
            esp_attr_value_t cval4 = {};
            esp_ble_gatts_add_char(gBleState.data_service_handle, &char_uuid,
              ESP_GATT_PERM_READ, ESP_GATT_CHAR_PROP_BIT_NOTIFY, &cval4, &control);
          }
        } else if (gBleState.event_notify_handle == 0) {
          gBleState.event_notify_handle = handle;
          // Add CCCD descriptor
          descr_uuid.len = ESP_UUID_LEN_16;
          descr_uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
          esp_ble_gatts_add_char_descr(gBleState.data_service_handle, &descr_uuid,
            ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, &descr_val, &control);
        } else {
          gBleState.stream_control_handle = handle;
          // All Data chars added, start service
          esp_ble_gatts_start_service(gBleState.data_service_handle);
        }
      }
      break;
    }
    
    case ESP_GATTS_ADD_CHAR_DESCR_EVT: {
      ESP_LOGI(TAG, "GATTS: Descriptor added, status=%d handle=%d",
        param->add_char_descr.status, param->add_char_descr.attr_handle);
      
      // After CCCD added, add next characteristic or status char
      if (gBleState.service_creation_step == 1) {
        // After cmd_response CCCD, add cmd_status
        if (gBleState.cmd_status_handle == 0) {
          esp_bt_uuid_t char_uuid;
          if (bleParseUuid128(BLE_CMD_STATUS_CHAR_UUID, &char_uuid)) {
            esp_attr_value_t char_val = {};
            esp_attr_control_t ctrl = {.auto_rsp = ESP_GATT_AUTO_RSP};
            esp_ble_gatts_add_char(gBleState.command_service_handle, &char_uuid,
              ESP_GATT_PERM_READ, ESP_GATT_CHAR_PROP_BIT_READ, &char_val, &ctrl);
          }
        }
      } else if (gBleState.service_creation_step == 2) {
        // After sensor_data CCCD, add system_status
        if (gBleState.system_status_handle == 0) {
          esp_bt_uuid_t char_uuid;
          if (bleParseUuid128(BLE_SYSTEM_STATUS_CHAR_UUID, &char_uuid)) {
            esp_attr_value_t char_val = {};
            esp_attr_control_t ctrl = {.auto_rsp = ESP_GATT_AUTO_RSP};
            esp_ble_gatts_add_char(gBleState.data_service_handle, &char_uuid,
              ESP_GATT_PERM_READ, ESP_GATT_CHAR_PROP_BIT_NOTIFY, &char_val, &ctrl);
          }
        }
        // After event_notify CCCD, add stream_control
        else if (gBleState.stream_control_handle == 0) {
          esp_bt_uuid_t char_uuid;
          if (bleParseUuid128(BLE_STREAM_CONTROL_CHAR_UUID, &char_uuid)) {
            esp_attr_value_t char_val = {};
            esp_attr_control_t ctrl = {.auto_rsp = ESP_GATT_AUTO_RSP};
            esp_ble_gatts_add_char(gBleState.data_service_handle, &char_uuid,
              ESP_GATT_PERM_WRITE, ESP_GATT_CHAR_PROP_BIT_WRITE, &char_val, &ctrl);
          }
        }
      }
    } break;
      
    case ESP_GATTS_START_EVT:
      ESP_LOGI(TAG, "GATTS: Service started, status=%d handle=%d step=%d",
        param->start.status, param->start.service_handle, gBleState.service_creation_step);
      
      if (param->start.status != ESP_GATT_OK) break;
      
      // Move to next service
      gBleState.service_creation_step++;
      
      if (gBleState.service_creation_step == 1) {
        // Create Command Service
        esp_gatt_srvc_id_t service_id;
        service_id.is_primary = true;
        service_id.id.inst_id = 0;
        if (bleParseUuid128(BLE_COMMAND_SERVICE_UUID, &service_id.id.uuid)) {
          esp_ble_gatts_create_service(gatts_if, &service_id, 12);
        }
      } else if (gBleState.service_creation_step == 2) {
        // Create Data Service
        esp_gatt_srvc_id_t service_id;
        service_id.is_primary = true;
        service_id.id.inst_id = 0;
        if (bleParseUuid128(BLE_DATA_SERVICE_UUID, &service_id.id.uuid)) {
          esp_ble_gatts_create_service(gatts_if, &service_id, 16);
        }
      } else {
        // All services created, setup extended advertising
        ESP_LOGI(TAG, "All GATT services created, setting up extended advertising");
        
        gBleState.ext_adv_params.type = ESP_BLE_GAP_SET_EXT_ADV_PROP_LEGACY_IND;
        gBleState.ext_adv_params.interval_min = 0x20;
        gBleState.ext_adv_params.interval_max = 0x40;
        gBleState.ext_adv_params.channel_map = ADV_CHNL_ALL;
        gBleState.ext_adv_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
        gBleState.ext_adv_params.peer_addr_type = BLE_ADDR_TYPE_PUBLIC;
        memset(gBleState.ext_adv_params.peer_addr, 0, 6);
        gBleState.ext_adv_params.filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
        gBleState.ext_adv_params.tx_power = 127; // No preference
        gBleState.ext_adv_params.primary_phy = ESP_BLE_GAP_PRI_PHY_1M;
        gBleState.ext_adv_params.max_skip = 0;
        gBleState.ext_adv_params.secondary_phy = ESP_BLE_GAP_PHY_1M;
        gBleState.ext_adv_params.sid = 0;
        gBleState.ext_adv_params.scan_req_notif = false;
        
        gBleState.adv_instance = 0;
        esp_ble_gap_ext_adv_set_params(gBleState.adv_instance, &gBleState.ext_adv_params);
      }
      break;
      
    case ESP_GATTS_CONNECT_EVT: {
      ESP_LOGI(TAG, "GATTS: Client connected, conn_id=%d", param->connect.conn_id);
      bleLogAddr("  Remote BDA:", param->connect.remote_bda);
      
      BLEConnection* conn = bleAllocConnection(param->connect.conn_id, gatts_if, param->connect.remote_bda);
      if (conn) {
        gBleState.state = BLE_STATE_CONNECTED;
        ESP_LOGI(TAG, "  Connection allocated, total=%d", gBleState.connectionCount);
      } else {
        ESP_LOGW(TAG, "  Failed to allocate connection (max=%d)", BLE_MAX_CONNECTIONS);
      }
    } break;
      
    case ESP_GATTS_DISCONNECT_EVT:
      ESP_LOGI(TAG, "GATTS: Client disconnected, conn_id=%d reason=%d",
        param->disconnect.conn_id, param->disconnect.reason);
      bleFreeConnection(param->disconnect.conn_id);
      
      if (gBleState.connectionCount == 0) {
        ESP_LOGI(TAG, "  No connections remaining, resuming extended advertising");
        esp_ble_gap_ext_adv_t ext_adv[1];
        ext_adv[0].instance = gBleState.adv_instance;
        ext_adv[0].duration = 0;
        ext_adv[0].max_events = 0;
        esp_ble_gap_ext_adv_start(1, ext_adv);
      }
      break;
      
    case ESP_GATTS_WRITE_EVT: {
      ESP_LOGI(TAG, "GATTS: Write received, conn_id=%d handle=%d len=%d",
        param->write.conn_id, param->write.handle, param->write.len);
      
      // Handle command write
      if (param->write.handle == gBleState.cmd_request_handle) {
        BLEConnection* wconn = bleFindConnection(param->write.conn_id);
        if (wconn) {
          wconn->commandsReceived++;
          wconn->lastActivityMs = millis();
        }
        
        // Process command through existing command system
        if (gBleState.onCommandReceived) {
          gBleState.onCommandReceived(param->write.conn_id, param->write.value, param->write.len);
        }
      }
      
      // Send write response if needed
      if (param->write.need_rsp) {
        esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id,
          ESP_GATT_OK, NULL);
      }
    } break;
      
    case ESP_GATTS_MTU_EVT:
      ESP_LOGI(TAG, "GATTS: MTU changed to %d", param->mtu.mtu);
      break;
      
    default:
      ESP_LOGD(TAG, "GATTS: Unhandled event %d", event);
      break;
  }
}

// =============================================================================
// GATTC (CLIENT) EVENT HANDLER
// =============================================================================

static void bleGattcEventHandler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t* param) {
  switch (event) {
    case ESP_GATTC_REG_EVT:
      ESP_LOGI(TAG, "GATTC: App registered, if=%d status=%d", gattc_if, param->reg.status);
      if (param->reg.status == ESP_GATT_OK) {
        gBleState.gattc_if = gattc_if;
      }
      break;
      
    case ESP_GATTC_OPEN_EVT:
      if (param->open.status == ESP_GATT_OK) {
        ESP_LOGI(TAG, "GATTC: Connected, conn_id=%d mtu=%d",
          param->open.conn_id, param->open.mtu);
        gBleState.gattc_conn_id = param->open.conn_id;
        gBleState.state = BLE_STATE_CONNECTED;
        
        // Request MTU increase
        esp_ble_gattc_send_mtu_req(gattc_if, param->open.conn_id);
      } else {
        ESP_LOGE(TAG, "GATTC: Connection failed, status=%d", param->open.status);
        gBleState.state = BLE_STATE_IDLE;
      }
      break;
      
    case ESP_GATTC_CLOSE_EVT:
      ESP_LOGI(TAG, "GATTC: Disconnected, reason=%d", param->close.reason);
      gBleState.state = BLE_STATE_IDLE;
      gBleState.gattc_conn_id = 0;
      break;
      
    case ESP_GATTC_CFG_MTU_EVT:
      if (param->cfg_mtu.status == ESP_GATT_OK) {
        ESP_LOGI(TAG, "GATTC: MTU configured to %d", param->cfg_mtu.mtu);
        // Start service discovery
        esp_ble_gattc_search_service(gattc_if, param->cfg_mtu.conn_id, NULL);
      }
      break;
      
    case ESP_GATTC_SEARCH_RES_EVT:
      ESP_LOGI(TAG, "GATTC: Service found");
      // TODO: Store service handles for G2 client
      break;
      
    case ESP_GATTC_SEARCH_CMPL_EVT:
      ESP_LOGI(TAG, "GATTC: Service discovery complete");
      // TODO: Get characteristics for G2 client
      break;
      
    case ESP_GATTC_NOTIFY_EVT:
      ESP_LOGI(TAG, "GATTC: Notification received, len=%d", param->notify.value_len);
      // TODO: Forward to G2 client handler
      break;
      
    default:
      ESP_LOGD(TAG, "GATTC: Unhandled event %d", event);
      break;
  }
}

// =============================================================================
// PUBLIC API IMPLEMENTATION
// =============================================================================

bool bleIdfInit() {
  if (gBleState.initialized) {
    ESP_LOGW(TAG, "Already initialized");
    return true;
  }
  
  ESP_LOGI(TAG, "Initializing ESP-IDF Bluedroid BLE stack");
  
  // Release BT controller memory if not needed
  esp_err_t ret;
  
  // Release Classic BT memory (ESP32-S3 doesn't support it anyway)
  ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
  
  // Initialize BT controller
  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  ret = esp_bt_controller_init(&bt_cfg);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(ret));
    return false;
  }
  
  // Enable BT controller in BLE mode
  ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(ret));
    return false;
  }
  
  // Initialize Bluedroid stack
  ret = esp_bluedroid_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
    return false;
  }
  
  ret = esp_bluedroid_enable();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
    return false;
  }
  
  // Register GAP callback
  ret = esp_ble_gap_register_callback(bleGapEventHandler);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "GAP callback register failed: %s", esp_err_to_name(ret));
    return false;
  }
  
  // Register GATTS callback
  ret = esp_ble_gatts_register_callback(bleGattsEventHandler);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "GATTS callback register failed: %s", esp_err_to_name(ret));
    return false;
  }
  
  // Register GATTC callback
  ret = esp_ble_gattc_register_callback(bleGattcEventHandler);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "GATTC callback register failed: %s", esp_err_to_name(ret));
    return false;
  }
  
  // Set device name
  ret = esp_ble_gap_set_device_name("HardwareOne");
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Set device name failed: %s", esp_err_to_name(ret));
  }
  
  gBleState.initialized = true;
  gBleState.mode = BLE_MODE_OFF;
  gBleState.state = BLE_STATE_IDLE;
  
  ESP_LOGI(TAG, "BLE stack initialized successfully");
  return true;
}

void bleIdfDeinit() {
  if (!gBleState.initialized) {
    return;
  }
  
  ESP_LOGI(TAG, "Deinitializing BLE stack");
  
  // Stop any active mode
  if (gBleState.mode == BLE_MODE_SERVER) {
    bleIdfStopServer();
  } else if (gBleState.mode == BLE_MODE_CLIENT) {
    bleIdfStopClient();
  }
  
  // Disable and deinit Bluedroid
  esp_bluedroid_disable();
  esp_bluedroid_deinit();
  
  // Disable and deinit BT controller
  esp_bt_controller_disable();
  esp_bt_controller_deinit();
  
  memset(&gBleState, 0, sizeof(gBleState));
  ESP_LOGI(TAG, "BLE stack deinitialized");
}

bool bleIdfIsRunning() {
  return gBleState.initialized;
}

bool bleIdfStartServer() {
  if (!gBleState.initialized) {
    ESP_LOGE(TAG, "Not initialized");
    return false;
  }
  
  if (gBleState.mode == BLE_MODE_CLIENT) {
    ESP_LOGE(TAG, "Cannot start server while in client mode");
    return false;
  }
  
  ESP_LOGI(TAG, "Starting GATT Server (phone peripheral mode)");
  
  // Register GATTS app
  esp_err_t ret = esp_ble_gatts_app_register(0);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "GATTS app register failed: %s", esp_err_to_name(ret));
    return false;
  }
  
  gBleState.mode = BLE_MODE_SERVER;
  
  // Advertising will be started after service is created
  ESP_LOGI(TAG, "GATT Server starting...");
  return true;
}

bool bleIdfStopServer() {
  if (gBleState.mode != BLE_MODE_SERVER) {
    return false;
  }
  
  ESP_LOGI(TAG, "Stopping GATT Server");
  
  // Stop extended advertising
  uint8_t instances[1] = {gBleState.adv_instance};
  esp_ble_gap_ext_adv_stop(1, instances);
  
  // Disconnect all clients
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    if (gBleState.connections[i].active) {
      esp_ble_gatts_close(gBleState.gatts_if, gBleState.connections[i].connId);
    }
  }
  
  gBleState.mode = BLE_MODE_OFF;
  gBleState.state = BLE_STATE_IDLE;
  
  return true;
}

bool bleIdfStartClient() {
  if (!gBleState.initialized) {
    ESP_LOGE(TAG, "Not initialized");
    return false;
  }
  
  if (gBleState.mode == BLE_MODE_SERVER) {
    ESP_LOGE(TAG, "Cannot start client while in server mode");
    return false;
  }
  
  ESP_LOGI(TAG, "Starting GATT Client (G2 glasses central mode)");
  
  // Register GATTC app
  esp_err_t ret = esp_ble_gattc_app_register(0);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "GATTC app register failed: %s", esp_err_to_name(ret));
    return false;
  }
  
  gBleState.mode = BLE_MODE_CLIENT;
  
  ESP_LOGI(TAG, "GATT Client ready");
  return true;
}

bool bleIdfStopClient() {
  if (gBleState.mode != BLE_MODE_CLIENT) {
    return false;
  }
  
  ESP_LOGI(TAG, "Stopping GATT Client");
  
  // Disconnect if connected
  if (gBleState.gattc_conn_id != 0) {
    esp_ble_gattc_close(gBleState.gattc_if, gBleState.gattc_conn_id);
  }
  
  // Stop scanning if active
  if (gBleState.state == BLE_STATE_SCANNING) {
    esp_ble_gap_stop_scanning();
  }
  
  gBleState.mode = BLE_MODE_OFF;
  gBleState.state = BLE_STATE_IDLE;
  
  return true;
}

BLEMode bleIdfGetMode() {
  return gBleState.mode;
}

BLEConnectionState bleIdfGetState() {
  return gBleState.state;
}

bool bleIdfServerSendResponse(uint16_t connId, const uint8_t* data, size_t len) {
  if (gBleState.mode != BLE_MODE_SERVER || !data || len == 0) {
    return false;
  }
  
  BLEConnection* conn = bleFindConnection(connId);
  if (!conn) {
    return false;
  }
  
  esp_err_t ret = esp_ble_gatts_send_indicate(
    conn->gatts_if,
    connId,
    gBleState.cmd_response_handle,
    len,
    (uint8_t*)data,
    false  // Don't need confirmation
  );
  
  if (ret == ESP_OK) {
    conn->responsesSent++;
    conn->lastActivityMs = millis();
    return true;
  }
  
  return false;
}

bool bleIdfServerBroadcastResponse(const uint8_t* data, size_t len) {
  if (gBleState.mode != BLE_MODE_SERVER || !data || len == 0) {
    return false;
  }
  
  bool success = false;
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    if (gBleState.connections[i].active) {
      if (bleIdfServerSendResponse(gBleState.connections[i].connId, data, len)) {
        success = true;
      }
    }
  }
  
  return success;
}

bool bleIdfServerSendSensorData(const uint8_t* data, size_t len) {
  if (gBleState.mode != BLE_MODE_SERVER || !data || len == 0) {
    return false;
  }
  
  bool success = false;
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    if (gBleState.connections[i].active) {
      esp_err_t ret = esp_ble_gatts_send_indicate(
        gBleState.connections[i].gatts_if,
        gBleState.connections[i].connId,
        gBleState.sensor_data_handle,
        len,
        (uint8_t*)data,
        false
      );
      if (ret == ESP_OK) {
        success = true;
      }
    }
  }
  return success;
}

bool bleIdfServerSendEvent(BLEEventType eventType, const char* message) {
  if (gBleState.mode != BLE_MODE_SERVER) {
    return false;
  }
  
  // Format: 1 byte event type + message string
  uint8_t buf[256];
  buf[0] = (uint8_t)eventType;
  size_t msg_len = message ? strlen(message) : 0;
  if (msg_len > 254) msg_len = 254;
  if (msg_len > 0) {
    memcpy(&buf[1], message, msg_len);
  }
  
  bool success = false;
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    if (gBleState.connections[i].active) {
      esp_err_t ret = esp_ble_gatts_send_indicate(
        gBleState.connections[i].gatts_if,
        gBleState.connections[i].connId,
        gBleState.event_notify_handle,
        msg_len + 1,
        buf,
        false
      );
      if (ret == ESP_OK) {
        success = true;
      }
    }
  }
  return success;
}

bool bleIdfServerSendSystemStatus(const uint8_t* data, size_t len) {
  if (gBleState.mode != BLE_MODE_SERVER || gBleState.connectionCount == 0) {
    return false;
  }
  if (!data || len == 0 || gBleState.system_status_handle == 0) {
    return false;
  }
  
  bool success = false;
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    if (gBleState.connections[i].active) {
      esp_err_t ret = esp_ble_gatts_send_indicate(
        gBleState.connections[i].gatts_if,
        gBleState.connections[i].connId,
        gBleState.system_status_handle,
        len,
        (uint8_t*)data,
        false
      );
      if (ret == ESP_OK) {
        success = true;
      }
    }
  }
  return success;
}

// =============================================================================
// STREAM CONTROL
// =============================================================================

void bleIdfEnableStream(uint8_t streamFlags) {
  gBleState.streamFlags |= streamFlags;
}

void bleIdfDisableStream(uint8_t streamFlags) {
  gBleState.streamFlags &= ~streamFlags;
}

void bleIdfSetStreamInterval(uint32_t sensorMs, uint32_t systemMs) {
  gBleState.sensorStreamInterval = sensorMs;
  gBleState.systemStreamInterval = systemMs;
}

bool bleIdfIsStreamEnabled(uint8_t streamFlag) {
  return (gBleState.streamFlags & streamFlag) != 0;
}

bool bleIdfIsConnected() {
  return gBleState.mode == BLE_MODE_SERVER && gBleState.connectionCount > 0;
}

void bleIdfUpdateStreams() {
  if (gBleState.mode != BLE_MODE_SERVER || gBleState.connectionCount == 0) {
    return;
  }
  
  uint32_t now = millis();
  
  // Sensor stream
  if (gBleState.streamFlags & BLE_STREAM_SENSORS) {
    if (gBleState.sensorStreamInterval > 0 && 
        (now - gBleState.lastSensorStream >= gBleState.sensorStreamInterval)) {
      // Build and send sensor data - caller should handle this
      gBleState.lastSensorStream = now;
    }
  }
  
  // System stream
  if (gBleState.streamFlags & BLE_STREAM_SYSTEM) {
    if (gBleState.systemStreamInterval > 0 &&
        (now - gBleState.lastSystemStream >= gBleState.systemStreamInterval)) {
      // Build and send system status - caller should handle this
      gBleState.lastSystemStream = now;
    }
  }
}

// =============================================================================
// CLIENT (GATTC) API
// =============================================================================

bool bleIdfClientWrite(const uint8_t* data, size_t len) {
  if (gBleState.mode != BLE_MODE_CLIENT || gBleState.gattc_conn_id == 0) {
    return false;
  }
  
  // TODO: Implement write to G2 characteristic
  ESP_LOGW(TAG, "Client write not yet implemented");
  return false;
}

bool bleIdfClientScan(uint32_t durationMs) {
  if (gBleState.mode != BLE_MODE_CLIENT) {
    ESP_LOGE(TAG, "Not in client mode");
    return false;
  }
  
  // TODO: Implement BLE scan for G2 glasses
  ESP_LOGW(TAG, "Client scan not yet implemented");
  return false;
}

bool bleIdfClientConnect(const esp_bd_addr_t remote_bda) {
  if (gBleState.mode != BLE_MODE_CLIENT) {
    ESP_LOGE(TAG, "Not in client mode");
    return false;
  }
  
  // TODO: Implement connection to G2 glasses
  ESP_LOGW(TAG, "Client connect not yet implemented");
  return false;
}

bool bleIdfClientDisconnect() {
  if (gBleState.mode != BLE_MODE_CLIENT || gBleState.gattc_conn_id == 0) {
    return false;
  }
  
  esp_ble_gattc_close(gBleState.gattc_if, gBleState.gattc_conn_id);
  return true;
}

bool bleIdfClientIsConnected() {
  return gBleState.mode == BLE_MODE_CLIENT && 
         gBleState.state == BLE_STATE_CONNECTED &&
         gBleState.gattc_conn_id != 0;
}

void bleIdfGetStatus(char* buffer, size_t bufferSize) {
  if (!buffer || bufferSize == 0) return;
  
  const char* modeStr = "OFF";
  if (gBleState.mode == BLE_MODE_SERVER) modeStr = "SERVER";
  else if (gBleState.mode == BLE_MODE_CLIENT) modeStr = "CLIENT";
  
  snprintf(buffer, bufferSize,
    "BLE: %s | State: %s | Connections: %d",
    modeStr,
    bleIdfGetStateString(),
    gBleState.connectionCount);
}

const char* bleIdfGetStateString() {
  switch (gBleState.state) {
    case BLE_STATE_IDLE: return "idle";
    case BLE_STATE_ADVERTISING: return "advertising";
    case BLE_STATE_SCANNING: return "scanning";
    case BLE_STATE_CONNECTING: return "connecting";
    case BLE_STATE_CONNECTED: return "connected";
    case BLE_STATE_DISCONNECTING: return "disconnecting";
    default: return "unknown";
  }
}

int bleIdfGetConnectionCount() {
  return gBleState.connectionCount;
}

bool bleIdfGetConnectionInfo(int index, BLEConnection* outInfo) {
  if (!outInfo || index < 0 || index >= BLE_MAX_CONNECTIONS) {
    return false;
  }
  
  if (gBleState.connections[index].active) {
    memcpy(outInfo, &gBleState.connections[index], sizeof(BLEConnection));
    return true;
  }
  
  return false;
}

void bleIdfSessionTick() {
  // Periodic maintenance tasks
  // TODO: Timeout inactive connections, etc.
}

#endif // ENABLE_BLUETOOTH
