#include "HAL_Radio.h"

#if HW1_RADIO_COMPANION

#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_hosted_event.h"
#include "esp_hosted_misc.h"
#include "esp_hosted_misc_types.h"
#include "esp_hosted_ota.h"
#include "esp_log.h"
#include "esp_now_hosted_companion.h"
#include "esp_now_hosted_host.h"
#include "esp_now_hosted_rpc.h"
#include "esp_timer.h"
#include "esp32-hal-hosted.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char* kTag = "HW1_RADIO";

// ESP32-P4X-EYE / EYE-MB v2.3 board profile, the only Hosted board so far.
// Keep wiring here rather than in the shared mesh protocol; a second Hosted
// board needs its own pin set selected here.
namespace eye {
constexpr gpio_num_t kC6Boot = GPIO_NUM_33;
constexpr gpio_num_t kC6UartRx = GPIO_NUM_35;  // Also the P4 BOOT strap.
constexpr gpio_num_t kC6UartTx = GPIO_NUM_36;
constexpr gpio_num_t kC6Enable = GPIO_NUM_9;   // Low holds the C6 in reset.
constexpr int8_t kSdioClk = 28;
constexpr int8_t kSdioCmd = 27;
constexpr int8_t kSdioD0 = 29;
constexpr int8_t kSdioD1 = 30;
constexpr int8_t kSdioD2 = 31;
constexpr int8_t kSdioD3 = 32;
}  // namespace eye

// ---- companion console mirror ------------------------------------------------
// The C6's UART0 console reaches the P4 on GPIO36. Mirroring it into the P4 log
// is the only way to see the companion's own boot messages, crashes and
// backtraces while HardwareOne drives it (the programmer service is the
// alternative, and it replaces the application).
constexpr uart_port_t kConsoleUart = UART_NUM_1;
TaskHandle_t sConsoleTask = nullptr;
volatile bool sConsoleStop = false;

void consoleTask(void*) {
  uint8_t buf[256];
  char line[200];
  size_t n = 0;
  while (!sConsoleStop) {
    const int got = uart_read_bytes(kConsoleUart, buf, sizeof(buf), pdMS_TO_TICKS(200));
    for (int i = 0; i < got; i++) {
      const char c = static_cast<char>(buf[i]);
      if (c == '\n' || n >= sizeof(line) - 1) {
        line[n] = '\0';
        if (n) ESP_LOGI("C6>", "%s", line);
        n = 0;
        if (c != '\n' && c != '\r') line[n++] = c;
      } else if (c != '\r') {
        line[n++] = c;
      }
    }
  }
  sConsoleTask = nullptr;
  vTaskDelete(nullptr);
}

constexpr int kBootAttempts = 2;  // each attempt resets the C6 and waits up to ~20 s

bool sBoardPrepared = false;
bool sPrepared = false;        // radioHalPrepare() ran and the transport came up at least once
bool sTransportUp = false;     // last TRANSPORT_UP without a DOWN/FAILURE since
bool sTransportLost = false;
bool sHeld = false;
bool sEventsRegistered = false;
RadioHalEventFn sEventFn = nullptr;
RadioCompanionIdentity sIdentity = {};
RadioCompanionMemory sMemory = {};
uint16_t sHeartbeatSec = 0;
uint32_t sLowMemoryBytes = 0;
portMUX_TYPE sLock = portMUX_INITIALIZER_UNLOCKED;

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void copyBounded(char* dst, size_t cap, const char* src, size_t srcCap) {
  if (!cap) return;
  size_t n = 0;
  while (n < srcCap && n + 1 < cap && src[n]) {
    dst[n] = src[n];
    n++;
  }
  dst[n] = '\0';
}

esp_err_t prepareEyeBoard() {
  if (sBoardPrepared) return ESP_OK;
  // After a deep-sleep wake the enable line may still be held low by
  // radioHalPrepareForDeepSleep(); release every hold before Hosted drives it.
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
  gpio_deep_sleep_hold_dis();
#endif
  gpio_hold_dis(eye::kC6Enable);
  // Set the output latch before enabling it: C6 BOOT must be high when Hosted
  // pulses EN. Leave the ROM UART idle; never pull P4 BOOT low.
  ESP_RETURN_ON_ERROR(gpio_set_level(eye::kC6Boot, 1), kTag, "C6 BOOT level");
  ESP_RETURN_ON_ERROR(gpio_set_direction(eye::kC6Boot, GPIO_MODE_OUTPUT), kTag, "C6 BOOT direction");
  ESP_RETURN_ON_ERROR(gpio_set_direction(eye::kC6UartRx, GPIO_MODE_INPUT), kTag, "C6 UART RX direction");
  ESP_RETURN_ON_ERROR(gpio_set_pull_mode(eye::kC6UartRx, GPIO_PULLUP_ONLY), kTag, "C6 UART RX pull-up");
  ESP_RETURN_ON_ERROR(gpio_set_direction(eye::kC6UartTx, GPIO_MODE_INPUT), kTag, "C6 UART TX direction");
  // Arduino's generic esp32p4 variant overrides the Kconfig SDIO pins with a
  // different board's wiring. Set its pin map before the first hostedInitWiFi().
  if (!hostedSetPins(eye::kSdioClk, eye::kSdioCmd, eye::kSdioD0, eye::kSdioD1, eye::kSdioD2,
                     eye::kSdioD3, static_cast<int8_t>(eye::kC6Enable))) {
    ESP_LOGE(kTag, "Could not set EYE SDIO pins before Hosted startup");
    return ESP_ERR_INVALID_STATE;
  }
  sBoardPrepared = true;
  return ESP_OK;
}

void quietRpcLogs() {
  // initArduino() resets the global log level after Hosted's constructor
  // muted per-RPC INFO traffic. Restore only those tags.
  esp_log_level_set("rpc_core", ESP_LOG_WARN);
  esp_log_level_set("rpc_rsp", ESP_LOG_WARN);
  esp_log_level_set("rpc_evt", ESP_LOG_WARN);
}

void deliver(RadioHalEvent type, uint32_t value, uint32_t value2 = 0) {
  RadioHalEventFn fn;
  portENTER_CRITICAL(&sLock);
  fn = sEventFn;
  portEXIT_CRITICAL(&sLock);
  if (!fn) return;
  const RadioHalEventData event = {type, value, value2};
  fn(event);
}

void hostedEventHandler(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base != ESP_HOSTED_EVENT) return;
  switch (id) {
    case ESP_HOSTED_EVENT_TRANSPORT_UP:
      portENTER_CRITICAL(&sLock);
      sTransportUp = true;
      portEXIT_CRITICAL(&sLock);
      deliver(RadioHalEvent::TransportUp, 0);
      break;
    case ESP_HOSTED_EVENT_TRANSPORT_DOWN:
      portENTER_CRITICAL(&sLock);
      sTransportUp = false;
      portEXIT_CRITICAL(&sLock);
      deliver(RadioHalEvent::TransportDown, 0);
      break;
    case ESP_HOSTED_EVENT_TRANSPORT_FAILURE:
      portENTER_CRITICAL(&sLock);
      sTransportUp = false;
      portEXIT_CRITICAL(&sLock);
      deliver(RadioHalEvent::TransportFailure, 0);
      break;
    case ESP_HOSTED_EVENT_CP_HEARTBEAT: {
      const auto* hb = static_cast<const esp_hosted_event_heartbeat_t*>(data);
      deliver(RadioHalEvent::Heartbeat, hb ? hb->heartbeat : 0);
      break;
    }
    case ESP_HOSTED_EVENT_CP_INIT: {
      const auto* init = static_cast<const esp_hosted_event_init_t*>(data);
      deliver(RadioHalEvent::CompanionBoot, init ? static_cast<uint32_t>(init->reason) : 0);
      break;
    }
    case ESP_HOSTED_EVENT_MEM_MONITOR: {
      const auto* mem = static_cast<const esp_hosted_event_mem_info_t*>(data);
      if (!mem) break;
      portENTER_CRITICAL(&sLock);
      sMemory.known = true;
      sMemory.freeHeap = mem->curr_total_free_heap_size;
      sMemory.minFreeHeap = mem->curr_min_free_heap_size;
      sMemory.internalFree = mem->curr_internal.cap_8bit.free_size;
      sMemory.internalLargest = mem->curr_internal.cap_8bit.largest_free_block;
      sMemory.atMs = nowMs();
      portEXIT_CRITICAL(&sLock);
      deliver(RadioHalEvent::Memory, mem->curr_total_free_heap_size, mem->curr_min_free_heap_size);
      break;
    }
    default:
      break;
  }
}

esp_err_t registerEvents() {
  if (sEventsRegistered) return ESP_OK;
  const esp_err_t loop = esp_event_loop_create_default();
  if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) return loop;
  ESP_RETURN_ON_ERROR(esp_event_handler_register(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, hostedEventHandler, nullptr),
                      kTag, "Hosted event handler");
  sEventsRegistered = true;
  return ESP_OK;
}

esp_err_t applyMonitoring() {
  if (!radioHalTransportUp()) return ESP_ERR_INVALID_STATE;
  esp_err_t first = ESP_OK;
  esp_err_t err = esp_hosted_configure_heartbeat(sHeartbeatSec > 0, sHeartbeatSec > 0 ? sHeartbeatSec : 1);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "Heartbeat configuration failed: %s", esp_err_to_name(err));
    first = err;
  }
  esp_hosted_config_mem_monitor_t config = {};
  esp_hosted_curr_mem_info_t current = {};
  config.config = sLowMemoryBytes > 0 ? ESP_HOSTED_MEMMONITOR_ENABLE : ESP_HOSTED_MEMMONITOR_DISABLE;
  config.report_always = false;  // a report only when a threshold is crossed
  config.interval_sec = 30;
  config.internal_mem.threshold_mem_8bit = sLowMemoryBytes;
  config.internal_mem.threshold_mem_dma = sLowMemoryBytes;
  err = esp_hosted_set_mem_monitor(&config, &current);
  if (err == ESP_OK) {
    portENTER_CRITICAL(&sLock);
    sMemory.known = true;
    sMemory.totalHeap = current.curr_total_heap_size;
    sMemory.internalFree = current.curr_internal.cap_8bit.free_size;
    sMemory.internalLargest = current.curr_internal.cap_8bit.largest_free_block;
    sMemory.freeHeap = current.curr_internal.cap_8bit.free_size + current.curr_external.cap_8bit.free_size;
    sMemory.atMs = nowMs();
    portEXIT_CRITICAL(&sLock);
  } else {
    ESP_LOGW(kTag, "Memory monitor configuration failed: %s", esp_err_to_name(err));
    if (first == ESP_OK) first = err;
  }
  return first;
}

// One Hosted bring-up attempt: Arduino's helper resets the companion, waits
// for the SDIO link and records its lifecycle state. The version query is the
// first RPC and proves the link end to end.
esp_err_t startOnce() {
  if (!hostedInitWiFi()) {
    ESP_LOGE(kTag, "Hosted transport initialization failed");
    return ESP_FAIL;
  }
  esp_hosted_coprocessor_fwver_t version = {};
  const esp_err_t err = esp_hosted_get_coprocessor_fwversion(&version);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "C6 firmware version query failed: %s", esp_err_to_name(err));
    return err;
  }
  portENTER_CRITICAL(&sLock);
  sTransportUp = true;
  sTransportLost = false;
  sHeld = false;
  sIdentity.versionKnown = true;
  sIdentity.major = version.major1;
  sIdentity.minor = version.minor1;
  sIdentity.patch = version.patch1;
  sIdentity.versionMatches = version.major1 == HW1_C6_HOSTED_VERSION_MAJOR &&
                             version.minor1 == HW1_C6_HOSTED_VERSION_MINOR &&
                             version.patch1 == HW1_C6_HOSTED_VERSION_PATCH;
  portEXIT_CRITICAL(&sLock);
  return ESP_OK;
}

}  // namespace

esp_err_t radioHalConsoleMirror(bool on) {
  if (on == (sConsoleTask != nullptr)) return ESP_OK;
  if (on) {
    uart_config_t cfg = {};
    cfg.baud_rate = 115200;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    ESP_RETURN_ON_ERROR(uart_driver_install(kConsoleUart, 2048, 0, 0, nullptr, 0), kTag, "C6 console UART");
    esp_err_t err = uart_param_config(kConsoleUart, &cfg);
    if (err == ESP_OK) {
      err = uart_set_pin(kConsoleUart, UART_PIN_NO_CHANGE, eye::kC6UartTx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
      sConsoleStop = false;
      if (xTaskCreate(consoleTask, "c6console", 3072, nullptr, 3, &sConsoleTask) != pdPASS) err = ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
      sConsoleTask = nullptr;
      uart_driver_delete(kConsoleUart);
      ESP_LOGW(kTag, "C6 console mirror not started: %s", esp_err_to_name(err));
    }
    return err;
  }
  sConsoleStop = true;
  while (sConsoleTask) vTaskDelay(pdMS_TO_TICKS(20));  // the task leaves uart_read_bytes and exits
  return uart_driver_delete(kConsoleUart);
}

bool radioHalConsoleMirrorOn() { return sConsoleTask != nullptr; }

esp_err_t radioHalPrepare() {
  quietRpcLogs();
  ESP_RETURN_ON_ERROR(prepareEyeBoard(), kTag, "EYE board preparation failed");
  radioHalConsoleMirror(true);  // not fatal; c6console off silences it
  ESP_RETURN_ON_ERROR(registerEvents(), kTag, "Hosted events");
  esp_err_t err = ESP_FAIL;
  for (int attempt = 1; attempt <= kBootAttempts; attempt++) {
    err = radioHalStartTransport();
    if (err == ESP_OK) break;
    ESP_LOGW(kTag, "C6 did not come up (attempt %d of %d): %s", attempt, kBootAttempts, esp_err_to_name(err));
  }
  sPrepared = err == ESP_OK;
  return err;
}

bool radioHalPrepared() { return sPrepared; }

bool radioHalTransportUp() {
  portENTER_CRITICAL(&sLock);
  const bool up = sTransportUp && !sTransportLost;
  portEXIT_CRITICAL(&sLock);
  return up && hostedIsInitialized();
}

bool radioHalEspNowAvailable() {
  if (!radioHalTransportUp()) return false;
  const esp_now_hosted_bridge_state_t state = esp_now_hosted_bridge_state();
  return state == ESP_NOW_HOSTED_BRIDGE_PRESENT || state == ESP_NOW_HOSTED_BRIDGE_PRESENT_NO_INFO;
}

const RadioCompanionIdentity& radioHalIdentity() { return sIdentity; }

esp_err_t radioHalRefreshIdentity() {
  if (!radioHalTransportUp()) return ESP_ERR_INVALID_STATE;
  RadioCompanionIdentity id = sIdentity;
  esp_hosted_coprocessor_fwver_t version = {};
  if (esp_hosted_get_coprocessor_fwversion(&version) == ESP_OK) {
    id.versionKnown = true;
    id.major = version.major1;
    id.minor = version.minor1;
    id.patch = version.patch1;
    id.versionMatches = version.major1 == HW1_C6_HOSTED_VERSION_MAJOR &&
                        version.minor1 == HW1_C6_HOSTED_VERSION_MINOR &&
                        version.patch1 == HW1_C6_HOSTED_VERSION_PATCH;
  }
  esp_hosted_app_desc_t desc = {};
  if (esp_hosted_get_coprocessor_app_desc(&desc) == ESP_OK) {
    id.descKnown = true;
    copyBounded(id.project, sizeof(id.project), desc.project_name, sizeof(desc.project_name));
    copyBounded(id.version, sizeof(id.version), desc.version, sizeof(desc.version));
    copyBounded(id.idf, sizeof(id.idf), desc.idf_ver, sizeof(desc.idf_ver));
    copyBounded(id.date, sizeof(id.date), desc.date, sizeof(desc.date));
    copyBounded(id.time, sizeof(id.time), desc.time, sizeof(desc.time));
  }
  uint32_t chipId = 0;
  char chip[16] = {};
  if (esp_hosted_get_cp_info(&chipId, chip, sizeof(chip)) == ESP_OK) {
    id.chipKnown = true;
    id.chipId = chipId;
    copyBounded(id.chip, sizeof(id.chip), chip, sizeof(chip));
  }
  // The bridge: a fresh start left the companion's ESP-NOW instance empty, so
  // the host's mirror of it must be empty too before the first request.
  esp_now_hosted_info_t info = {};
  const esp_err_t probe = esp_now_hosted_host_probe(&info);
  switch (esp_now_hosted_bridge_state()) {
    case ESP_NOW_HOSTED_BRIDGE_PRESENT: id.bridge = RadioBridgeState::Present; break;
    case ESP_NOW_HOSTED_BRIDGE_PRESENT_NO_INFO: id.bridge = RadioBridgeState::PresentNoInfo; break;
    case ESP_NOW_HOSTED_BRIDGE_ABSENT: id.bridge = RadioBridgeState::Absent; break;
    case ESP_NOW_HOSTED_BRIDGE_TRANSPORT_ERROR: id.bridge = RadioBridgeState::TransportError; break;
    default: id.bridge = RadioBridgeState::Unknown; break;
  }
  if (probe == ESP_OK && id.bridge == RadioBridgeState::Present) {
    id.infoKnown = true;
    id.bridgeWire = info.wire_version;
    id.imageState = info.image_state;
    id.runningSlot = info.running_slot;
    id.resetReason = info.reset_reason;
    id.uptimeMs = info.uptime_ms;
    id.freeHeap = info.free_heap;
    id.minFreeHeap = info.min_free_heap;
    id.cpuMhz = info.cpu_mhz;
    id.nativeEspNowVersion = info.native_espnow_version;
    copyBounded(id.build, sizeof(id.build), info.build, sizeof(info.build));
  } else {
    id.infoKnown = false;
  }
  portENTER_CRITICAL(&sLock);
  sIdentity = id;
  portEXIT_CRITICAL(&sLock);
  return ESP_OK;
}

void radioHalBridgeStats(RadioBridgeStats& out) {
  esp_now_hosted_host_stats_t stats = {};
  esp_now_hosted_host_get_stats(&stats);
  out.malformed = stats.malformed_messages;
  out.stale = stats.stale_responses;
  out.eventsDropped = stats.events_dropped;
  out.timeouts = stats.request_timeouts;
  out.transportErrors = stats.transport_errors;
}

esp_err_t radioHalQueryMemory(RadioCompanionMemory& out) {
  if (!radioHalTransportUp()) return ESP_ERR_INVALID_STATE;
  esp_hosted_config_mem_monitor_t config = {};
  esp_hosted_curr_mem_info_t current = {};
  config.config = ESP_HOSTED_MEMMONITOR_NO_CHANGE;
  const esp_err_t err = esp_hosted_set_mem_monitor(&config, &current);
  if (err != ESP_OK) return err;
  portENTER_CRITICAL(&sLock);
  sMemory.known = true;
  sMemory.totalHeap = current.curr_total_heap_size;
  sMemory.internalFree = current.curr_internal.cap_8bit.free_size;
  sMemory.internalLargest = current.curr_internal.cap_8bit.largest_free_block;
  sMemory.freeHeap = current.curr_internal.cap_8bit.free_size + current.curr_external.cap_8bit.free_size;
  sMemory.atMs = nowMs();
  out = sMemory;
  portEXIT_CRITICAL(&sLock);
  return ESP_OK;
}

const RadioCompanionMemory& radioHalLastMemory() { return sMemory; }

void radioHalSetEventHandler(RadioHalEventFn fn) {
  portENTER_CRITICAL(&sLock);
  sEventFn = fn;
  portEXIT_CRITICAL(&sLock);
}

esp_err_t radioHalConfigureMonitoring(uint16_t heartbeatSec, uint32_t lowMemoryBytes) {
  sHeartbeatSec = heartbeatSec;
  sLowMemoryBytes = lowMemoryBytes;
  if (!radioHalTransportUp()) return ESP_OK;  // applied by the next start
  return applyMonitoring();
}

esp_err_t radioHalStartTransport() {
  ESP_RETURN_ON_ERROR(prepareEyeBoard(), kTag, "EYE board preparation failed");
  ESP_RETURN_ON_ERROR(registerEvents(), kTag, "Hosted events");
  // A new link starts from a clean slate on both sides.
  hostedSetTransportLost(false);
  esp_now_hosted_host_set_offline(false);
  esp_now_hosted_host_reset_local();
  portENTER_CRITICAL(&sLock);
  sTransportLost = false;
  portEXIT_CRITICAL(&sLock);
  // Hosted's own reset pulse releases a held companion; the pin reconfigures.
  const esp_err_t err = startOnce();
  if (err != ESP_OK) {
    portENTER_CRITICAL(&sLock);
    sTransportUp = false;
    portEXIT_CRITICAL(&sLock);
    return err;
  }
  radioHalRefreshIdentity();
  applyMonitoring();
  if (!radioHalTransportUp()) {
    // A transport failure arrived while the identity was being read: the
    // companion answered the version query and then went away.
    ESP_LOGW(kTag, "C6 answered, then the transport failed while reading its identity");
    return ESP_FAIL;
  }
  const RadioCompanionIdentity& id = sIdentity;
  ESP_LOGI(kTag, "C6 up: Hosted %" PRIu32 ".%" PRIu32 ".%" PRIu32 " (%s), bridge %s%s", id.major, id.minor, id.patch,
           id.versionMatches ? "qualified" : "differs from " HW1_C6_HOSTED_TAG, radioHalBridgeStateName(id.bridge),
           id.infoKnown && id.imageState == ESP_NOW_HOSTED_IMAGE_PENDING_VERIFY ? ", image pending confirmation" : "");
  return ESP_OK;
}

void radioHalStopTransport() {
  if (hostedIsInitialized()) {
    // Arduino closes the transport once neither Wi-Fi nor BLE claims it. The
    // callers have stopped both; clear a stale BLE claim so the close happens.
    if (hostedIsBLEActive()) hostedReleaseBLE();
    hostedDeinitWiFi();
  }
  portENTER_CRITICAL(&sLock);
  sTransportUp = false;
  portEXIT_CRITICAL(&sLock);
}

void radioHalMarkTransportLost() {
  hostedSetTransportLost(true);
  esp_now_hosted_host_set_offline(true);
  portENTER_CRITICAL(&sLock);
  sTransportLost = true;
  sTransportUp = false;
  portEXIT_CRITICAL(&sLock);
}

bool radioHalTransportLost() {
  portENTER_CRITICAL(&sLock);
  const bool lost = sTransportLost;
  portEXIT_CRITICAL(&sLock);
  return lost;
}

esp_err_t radioHalHold(bool hold) {
  if (hold && hostedIsInitialized()) return ESP_ERR_INVALID_STATE;  // stop the transport first
  ESP_RETURN_ON_ERROR(prepareEyeBoard(), kTag, "EYE board preparation failed");
  gpio_hold_dis(eye::kC6Enable);
  ESP_RETURN_ON_ERROR(gpio_set_level(eye::kC6Enable, hold ? 0 : 1), kTag, "C6 EN level");
  ESP_RETURN_ON_ERROR(gpio_set_direction(eye::kC6Enable, GPIO_MODE_OUTPUT), kTag, "C6 EN direction");
  portENTER_CRITICAL(&sLock);
  sHeld = hold;
  if (hold) sTransportUp = false;
  portEXIT_CRITICAL(&sLock);
  ESP_LOGI(kTag, "C6 %s", hold ? "held in reset" : "released from reset");
  return ESP_OK;
}

bool radioHalHeld() {
  portENTER_CRITICAL(&sLock);
  const bool held = sHeld;
  portEXIT_CRITICAL(&sLock);
  return held;
}

void radioHalPrepareForDeepSleep() {
  // The caller has stopped the radio users and the transport. Keep the
  // companion in reset through sleep; prepareEyeBoard() releases the hold at
  // the next boot before Hosted drives the pin.
  if (radioHalHold(true) != ESP_OK) return;
  // On the ESP32-P4 (rev 3.0 and later) a held pad keeps its level through
  // deep sleep by itself; chips without per-IO deep-sleep hold also need the
  // global switch. An older P4 revision releases the line during sleep, which
  // only lets the companion boot idle without a host until the next reset.
  gpio_hold_en(eye::kC6Enable);
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
  gpio_deep_sleep_hold_en();
#endif
}

esp_err_t radioHalUpdateBegin() {
  if (!radioHalTransportUp()) return ESP_ERR_INVALID_STATE;
  return esp_hosted_slave_ota_begin();
}

esp_err_t radioHalUpdateWrite(const uint8_t* data, size_t len) {
  if (!data || !len) return ESP_ERR_INVALID_ARG;
  if (!radioHalTransportUp()) return ESP_ERR_INVALID_STATE;
  return esp_hosted_slave_ota_write(const_cast<uint8_t*>(data), static_cast<uint32_t>(len));
}

esp_err_t radioHalUpdateEnd() {
  if (!radioHalTransportUp()) return ESP_ERR_INVALID_STATE;
  return esp_hosted_slave_ota_end();
}

esp_err_t radioHalUpdateActivate() {
  if (!radioHalTransportUp()) return ESP_ERR_INVALID_STATE;
  return esp_hosted_slave_ota_activate();
}

esp_err_t radioHalBridgeConfirmImage() {
  if (!radioHalTransportUp()) return ESP_ERR_INVALID_STATE;
  return esp_now_hosted_confirm_image();
}

const char* radioHalResetReasonLabel(uint8_t reason) {
  // esp_reset_reason_t values are shared by every ESP32 family member.
  switch (reason) {
    case 1: return "power-on";
    case 2: return "external";
    case 3: return "software";
    case 4: return "panic";
    case 5: return "interrupt watchdog";
    case 6: return "task watchdog";
    case 7: return "other watchdog";
    case 8: return "deep sleep";
    case 9: return "brownout";
    case 10: return "SDIO";
    case 11: return "USB";
    case 12: return "JTAG";
    case 13: return "eFuse";
    case 14: return "power glitch";
    case 15: return "CPU lockup";
    default: return "unknown";
  }
}

const char* radioHalBridgeStateName(RadioBridgeState state) {
  switch (state) {
    case RadioBridgeState::Unknown: return "unknown";
    case RadioBridgeState::Present: return "present";
    case RadioBridgeState::PresentNoInfo: return "present (older)";
    case RadioBridgeState::Absent: return "absent";
    case RadioBridgeState::TransportError: return "transport error";
  }
  return "?";
}

#endif  // HW1_RADIO_COMPANION
