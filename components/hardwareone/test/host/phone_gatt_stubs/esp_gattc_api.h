#pragma once
#include <cstdint>
using esp_err_t=int; constexpr int ESP_OK=0, ESP_FAIL=-1;
using esp_gatt_if_t=uint8_t; constexpr esp_gatt_if_t ESP_GATT_IF_NONE=0xff;
using esp_gatt_status_t=int; constexpr int ESP_GATT_OK=0, ESP_GATT_CONGESTED=0x8f, ESP_GATT_BUSY=0x84;
using esp_ble_addr_type_t=uint8_t;
using esp_bd_addr_t=uint8_t[6];
constexpr int ESP_UUID_LEN_128=16, ESP_UUID_LEN_16=2;
constexpr int ESP_GATT_CHAR_PROP_BIT_WRITE=8, ESP_GATT_CHAR_PROP_BIT_NOTIFY=16;
constexpr int ESP_GATT_CHAR_PROP_BIT_WRITE_NR=4;
constexpr int ESP_GATT_UUID_CHAR_CLIENT_CONFIG=0x2902;
constexpr int ESP_GATT_WRITE_TYPE_RSP=1, ESP_GATT_AUTH_REQ_MITM=2;
constexpr int ESP_GATT_WRITE_TYPE_NO_RSP=2, ESP_GATT_AUTH_REQ_NONE=0;
struct esp_bt_uuid_t {uint16_t len=0;union {uint16_t uuid16;uint8_t uuid128[16];} uuid{};};
struct esp_gatt_id_t {esp_bt_uuid_t uuid{};};
struct esp_gattc_char_elem_t {uint16_t char_handle=0;uint8_t properties=0;};
struct esp_gattc_descr_elem_t {uint16_t handle=0;};
enum esp_gattc_cb_event_t {ESP_GATTC_REG_EVT,ESP_GATTC_UNREG_EVT,ESP_GATTC_OPEN_EVT,
 ESP_GATTC_CONNECT_EVT,ESP_GATTC_DISCONNECT_EVT,ESP_GATTC_CLOSE_EVT,ESP_GATTC_CFG_MTU_EVT,
 ESP_GATTC_SEARCH_RES_EVT,ESP_GATTC_SEARCH_CMPL_EVT,ESP_GATTC_REG_FOR_NOTIFY_EVT,
 ESP_GATTC_WRITE_DESCR_EVT,ESP_GATTC_WRITE_CHAR_EVT,ESP_GATTC_NOTIFY_EVT,
 ESP_GATTC_CANCEL_OPEN_EVT,ESP_GATTC_SRVC_CHG_EVT,ESP_GATTC_CONGEST_EVT,ESP_GATTC_QUEUE_FULL_EVT,
 ESP_GATTC_DIS_SRVC_CMPL_EVT};
// Only the public fields used by the production adapter; firmware builds check
// these declarations against the actual SDK as well as running this harness.
struct esp_ble_gattc_cb_param_t {
 struct {int status=0;uint16_t app_id=0;} reg;
 struct {int status=0;uint16_t conn_id=0;uint8_t remote_bda[6]{};uint16_t mtu=23;} open;
 struct {uint8_t remote_bda[6]{};} connect;
 struct {int status=0;uint16_t conn_id=0;} close;
 struct {uint16_t conn_id=0;uint8_t remote_bda[6]{};int reason=0;} disconnect;
 struct {int status=0;uint16_t conn_id=0;uint16_t mtu=23;} cfg_mtu;
 struct {uint16_t conn_id=0,start_handle=0,end_handle=0;esp_gatt_id_t srvc_id{};bool is_primary=false;} search_res;
 struct {int status=0;uint16_t conn_id=0;} search_cmpl;
 struct {int status=0;uint16_t handle=0;} reg_for_notify;
 struct {int status=0;uint16_t conn_id=0,handle=0;} write;
 struct {uint16_t conn_id=0,handle=0,value_len=0;uint8_t* value=nullptr;bool is_notify=true;} notify;
 struct {int status=0;uint8_t remote_bda[6]{};} cancel_open;
 struct {uint8_t remote_bda[6]{};} srvc_chg;
 struct {uint16_t conn_id=0;bool congested=false;} congest;
 struct {uint16_t conn_id=0;bool is_full=false;} queue_full;
 struct {int status=0;uint16_t conn_id=0;} dis_srvc_cmpl;
};
struct esp_ble_gattc_cancel_open_params_t {esp_gatt_if_t gattc_if;uint8_t remote_bda[6];};
int esp_ble_gattc_app_register(uint16_t);
int esp_ble_gattc_app_unregister(esp_gatt_if_t);
int esp_ble_gattc_open(esp_gatt_if_t,uint8_t*,esp_ble_addr_type_t,bool);
int esp_ble_gattc_close(esp_gatt_if_t,uint16_t);
int esp_ble_gattc_cancel_open(const esp_ble_gattc_cancel_open_params_t*);
int esp_ble_gattc_cache_refresh(uint8_t*);
int esp_ble_gattc_send_mtu_req(esp_gatt_if_t,uint16_t);
int esp_ble_gattc_search_service(esp_gatt_if_t,uint16_t,esp_bt_uuid_t*);
int esp_ble_gattc_get_char_by_uuid(esp_gatt_if_t,uint16_t,uint16_t,uint16_t,esp_bt_uuid_t,esp_gattc_char_elem_t*,uint16_t*);
int esp_ble_gattc_get_descr_by_char_handle(esp_gatt_if_t,uint16_t,uint16_t,esp_bt_uuid_t,esp_gattc_descr_elem_t*,uint16_t*);
int esp_ble_gattc_register_for_notify(esp_gatt_if_t,uint8_t*,uint16_t);
int esp_ble_gattc_write_char_descr(esp_gatt_if_t,uint16_t,uint16_t,uint16_t,uint8_t*,int,int);
int esp_ble_gattc_write_char(esp_gatt_if_t,uint16_t,uint16_t,uint16_t,uint8_t*,int,int);
