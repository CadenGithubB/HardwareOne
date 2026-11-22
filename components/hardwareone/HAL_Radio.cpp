#include "HAL_Radio.h"

#if HW1_RADIO_COMPANION

#include "System_BuildConfig.h"

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
#include "esp_system.h"
#include "esp_now_hosted_companion.h"
#include "esp_now_hosted_host.h"
#include "esp_now_hosted_rpc.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp32-hal-hosted.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "Radio_CompanionCore.h"
#include "System_RadioLogCore.h"

// ESP-Hosted 2.12.13 host/drivers/transport/transport_drv.{h,c} (external
// linkage, but not in the component's public include dirs).
// esp_hosted_hw1_fence_tx is HardwareOne's one-way restart TX fence
// (tools/p4/esp-hosted-2.12.13-hw1.patch): every producer goes through
// esp_hosted_tx or the STA/AP/serial wrappers in front of it (lwIP, RPC, BLE
// HCI), and each refuses a new packet once it is set. radioHalSilenceForRestart
// sets it before it holds the C6. Hosted's transport_state is left alone on
// purpose (see there). A refusal at the fence is silent and only counted
// (esp_hosted_hw1_tx_fence_drops): the log ran on the producer's task, and
// Bluedroid's hciT overflowed its stack printing it (bench 2026-11-08). An
// HCI packet is also dropped without a log whenever TX is not ready (a C6
// crash or recovery, a companion reset or hold with the Bluedroid host still
// up), counted in esp_hosted_hw1_hci_down_drops and shown by c6status.
extern "C" uint8_t is_transport_tx_ready(void);
extern "C" void esp_hosted_hw1_fence_tx(void);
extern "C" uint32_t esp_hosted_hw1_tx_fence_drops(void);
extern "C" uint32_t esp_hosted_hw1_hci_down_drops(void);

namespace {

constexpr const char* kTag = "HW1_RADIO";

// Transport wiring has one owner: the selected board's SDK defaults.
namespace board {
constexpr gpio_num_t kC6Enable = static_cast<gpio_num_t>(CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE);
constexpr int8_t kSdioClk = CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CLK_SLOT_1;
constexpr int8_t kSdioCmd = CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CMD_SLOT_1;
constexpr int8_t kSdioD0 = CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D0_SLOT_1;
constexpr int8_t kSdioD1 = CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D1_4BIT_BUS_SLOT_1;
constexpr int8_t kSdioD2 = CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D2_4BIT_BUS_SLOT_1;
constexpr int8_t kSdioD3 = CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D3_4BIT_BUS_SLOT_1;
#if defined(RADIO_C6_UART_TX_PIN)
constexpr gpio_num_t kC6Boot = static_cast<gpio_num_t>(RADIO_C6_BOOT_PIN);
constexpr gpio_num_t kC6UartRx = static_cast<gpio_num_t>(RADIO_C6_UART_RX_PIN);
constexpr gpio_num_t kC6UartTx = static_cast<gpio_num_t>(RADIO_C6_UART_TX_PIN);
#endif
}  // namespace board

// ---- companion console mirror ------------------------------------------------
// The C6's UART0 console reaches the P4 on GPIO36. Mirroring it into the P4 log
// is the only way to see the companion's own boot messages, crashes and
// backtraces while HardwareOne drives it (the programmer service is the
// alternative, and it replaces the application). It costs internal SRAM (the
// 2048 B UART buffer, the 4096 B task stack and the driver's own objects), so
// it is not started at every boot: radioHalPrepare starts it once a boot
// attempt has failed, and System_RadioCompanion starts it after a companion
// fault or when `c6console on` is saved, and stops it again.
#if defined(RADIO_C6_UART_TX_PIN)
constexpr uart_port_t kConsoleUart = UART_NUM_1;
TaskHandle_t sConsoleTask = nullptr;
volatile bool sConsoleStop = false;

// Each line goes out at the level the C6 printed it with (its crash reports
// as errors), so the quiet default of the C6> tag still shows the companion's
// errors; debugwifidriver or debugbluetoothcore shows the rest
// (System_RadioLogCore.h).
void logC6Line(radiolog::C6ConsoleLevels& levels, const char* line, bool continued) {
  switch (levels.classify(line, continued)) {
    case radiolog::LineLevel::Error: ESP_LOGE(radiolog::kC6ConsoleTag, "%s", line); break;
    case radiolog::LineLevel::Warn: ESP_LOGW(radiolog::kC6ConsoleTag, "%s", line); break;
    default: ESP_LOGI(radiolog::kC6ConsoleTag, "%s", line); break;
  }
}

void consoleTask(void*) {
  uint8_t buf[256];
  char line[200];
  size_t n = 0;
  bool split = false;  // the next chunk continues a line longer than `line`
  radiolog::C6ConsoleLevels levels;
  while (!sConsoleStop) {
    const int got = uart_read_bytes(kConsoleUart, buf, sizeof(buf), pdMS_TO_TICKS(200));
    for (int i = 0; i < got; i++) {
      const char c = static_cast<char>(buf[i]);
      if (c == '\n' || n >= sizeof(line) - 1) {
        line[n] = '\0';
        if (n) logC6Line(levels, line, split);
        split = c != '\n';
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

#endif

constexpr int kBootAttempts = 2;  // each attempt resets the C6 and waits up to ~20 s
// Hosted's own slave-ready wait (MAX_RETRY_TRANSPORT_ACTIVE 100 x 200 ms). A
// start waits for TRANSPORT_UP inside this budget even when Hosted's spent
// retry counter made it give up at once (hw1::radio::waitForTransportUp).
constexpr uint32_t kSlaveReadyBudgetMs = 20000;
constexpr uint32_t kSlaveReadyPollMs = 50;
// After the TX fence closes, Hosted's write task still empties its queue (the
// packets producers had already queued) while the C6 runs and acknowledges
// them. A full queue is 3 priority queues x 20 packets (SDIO TX queue size) of
// at most 1536 bytes, about 0.15 ms each at 20 MHz 4-bit: about 10 ms, and
// 60 ms even at 1 ms a packet. The margin covers a slave briefly out of RX
// buffers; one that stays out makes the write task drop the packet after its
// buffer polls (up to about 0.5 s), which a fixed wait cannot cover.
constexpr uint32_t kHostedTxFlushMs = 100;

bool sBoardPrepared = false;
bool sPrepared = false;        // radioHalPrepare() ran and the transport came up at least once
bool sBootRetried = false;     // radioHalPrepare()'s first attempt failed
bool sUpdateOnly = false;     // Explicit legacy OTA session; unavailable to radio users.
bool sTransportUp = false;     // last TRANSPORT_UP without a DOWN/FAILURE since
bool sHostedLinkUp = false;    // Hosted posted TRANSPORT_UP in this session (cleared before each start)
bool sTransportLost = false;
bool sHeld = false;
bool sRestartLatched = false;  // radioHalSilenceForRestart ran: EN stays low until the CPU reset
bool sEventsRegistered = false;
RadioHalEventFn sEventFn = nullptr;
RadioCompanionIdentity sIdentity = {};
// The link limit the companion reported in this transport session. A refresh
// whose probe fails (a busy bridge, a status read) keeps it, so link admission
// does not fall back to three links mid-session; a new session clears it.
uint8_t sSessionBleLinks = 0;
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

esp_err_t prepareBoard() {
  if (sBoardPrepared) return ESP_OK;
  // After a deep-sleep wake the enable line may still be held low by
  // radioHalPrepareForDeepSleep(); release every hold before Hosted drives it.
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
  gpio_deep_sleep_hold_dis();
#endif
  gpio_hold_dis(board::kC6Enable);
#if defined(RADIO_C6_UART_TX_PIN)
  // Set the output latch before enabling it: C6 BOOT must be high when Hosted
  // pulses EN. Leave the ROM UART idle; never pull P4 BOOT low.
  ESP_RETURN_ON_ERROR(gpio_set_level(board::kC6Boot, 1), kTag, "C6 BOOT level");
  ESP_RETURN_ON_ERROR(gpio_set_direction(board::kC6Boot, GPIO_MODE_OUTPUT), kTag, "C6 BOOT direction");
  ESP_RETURN_ON_ERROR(gpio_set_direction(board::kC6UartRx, GPIO_MODE_INPUT), kTag, "C6 UART RX direction");
  ESP_RETURN_ON_ERROR(gpio_set_pull_mode(board::kC6UartRx, GPIO_PULLUP_ONLY), kTag, "C6 UART RX pull-up");
  ESP_RETURN_ON_ERROR(gpio_set_direction(board::kC6UartTx, GPIO_MODE_INPUT), kTag, "C6 UART TX direction");
#endif
  // Arduino's generic esp32p4 variant overrides the Kconfig SDIO pins with a
  // different board's wiring. Set its pin map before the first hostedInitWiFi().
  if (!hostedSetPins(board::kSdioClk, board::kSdioCmd, board::kSdioD0, board::kSdioD1, board::kSdioD2,
                     board::kSdioD3, static_cast<int8_t>(board::kC6Enable))) {
    ESP_LOGE(kTag, "Could not set board SDIO pins before Hosted startup");
    return ESP_ERR_INVALID_STATE;
  }
  sBoardPrepared = true;
  return ESP_OK;
}

void quietHostedLogs() {
  // initArduino() resets the global log level after Hosted's constructor
  // muted per-RPC INFO traffic. Every Hosted tag and the C6 console start
  // quiet (ERROR): the settings, debugwifidriver among them, load only after
  // this first session, and applySettings() then sets each tag from its debug
  // flag and the saved esplog levels (applyRadioStackLogLevels). rpc_utils'
  // one line is a WARN "Event: SSID <ssid>" printed for every station-config
  // answer, not an event (the ESP-NOW channel check reads it every 15 s).
  for (const radiolog::TagRow& row : radiolog::kTags) {
    if (row.stack == radiolog::Stack::Hosted) {
      esp_log_level_set(row.tag, static_cast<esp_log_level_t>(radiolog::kQuiet));
    }
  }
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
      sHostedLinkUp = true;
      portEXIT_CRITICAL(&sLock);
      deliver(RadioHalEvent::TransportUp, 0);
      break;
    case ESP_HOSTED_EVENT_TRANSPORT_DOWN:
      portENTER_CRITICAL(&sLock);
      sTransportUp = false;
      sSessionBleLinks = 0;
      sHostedLinkUp = false;
      portEXIT_CRITICAL(&sLock);
      deliver(RadioHalEvent::TransportDown, 0);
      break;
    case ESP_HOSTED_EVENT_TRANSPORT_FAILURE:
      portENTER_CRITICAL(&sLock);
      sTransportUp = false;
      sSessionBleLinks = 0;
      sHostedLinkUp = false;
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

bool hostedLinkUp() {
  portENTER_CRITICAL(&sLock);
  const bool up = sHostedLinkUp;
  portEXIT_CRITICAL(&sLock);
  return up;
}

// One Hosted bring-up attempt: Arduino's helper resets the companion, waits
// for the SDIO link and records its lifecycle state. The version query is the
// first RPC and proves the link end to end.
esp_err_t startOnce(bool allowLegacyUpdate) {
  const uint32_t startedAt = nowMs();
  // A fresh Hosted session announces itself with TRANSPORT_UP. An already
  // initialized one (another claim kept it) has nothing new to announce.
  const bool freshSession = !hostedIsInitialized();
  if (freshSession) {
    portENTER_CRITICAL(&sLock);
    sHostedLinkUp = false;
    portEXIT_CRITICAL(&sLock);
  }
  if (!hostedInitWiFi()) {
    ESP_LOGE(kTag, "Hosted transport initialization failed");
    return ESP_FAIL;
  }
  // hostedInitWiFi() returns as soon as Hosted's reconfigure gives up, which
  // after one start that spent Hosted's whole wait is at once (its retry
  // counter is never reset on failure). Wait for the slave ourselves, in
  // Hosted's own budget.
  // This helps when the SDMMC host is healthy; it does not recover a wedged
  // one (the 2026-11-08 reboot outage), and a retry on a dead companion now
  // takes the full budget instead of returning at once.
  if (freshSession &&
      !hw1::radio::waitForTransportUp(
          startedAt, kSlaveReadyBudgetMs, kSlaveReadyPollMs, [] { return hostedLinkUp(); },
          [] { return nowMs(); }, [](uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1); })) {
    ESP_LOGW(kTag, "C6 transport not up %" PRIu32 " ms after the start", nowMs() - startedAt);
  }
  esp_hosted_coprocessor_fwver_t version = {};
  const esp_err_t err = esp_hosted_get_coprocessor_fwversion(&version);
  if (err != ESP_OK) {
    if (!allowLegacyUpdate) {
      ESP_LOGE(kTag, "C6 firmware version query failed: %s", esp_err_to_name(err));
      return err;
    }
    // Older factory companions predate the version RPC but implement the
    // standard Wi-Fi/OTA RPCs. Require a successful round trip before making
    // their update path available; a transport INIT event alone is insufficient.
    // WIFI_NOT_INIT is a valid remote reply before Wi-Fi initialization.
    uint8_t mac[6] = {};
    const esp_err_t probe = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (probe != ESP_OK && probe != ESP_ERR_WIFI_NOT_INIT) {
      ESP_LOGE(kTag, "C6 version and legacy MAC probes failed: %s / %s",
               esp_err_to_name(err), esp_err_to_name(probe));
      return probe;
    }
    ESP_LOGW(kTag, "C6 responds to legacy MAC RPC; version unavailable, update recommended");
  }
  portENTER_CRITICAL(&sLock);
  sTransportUp = true;
  sTransportLost = false;
  sHeld = false;
  sUpdateOnly = err != ESP_OK;
  sIdentity.versionKnown = err == ESP_OK;
  sIdentity.major = version.major1;
  sIdentity.minor = version.minor1;
  sIdentity.patch = version.patch1;
  sIdentity.versionMatches = err == ESP_OK && version.major1 == HW1_C6_HOSTED_VERSION_MAJOR &&
                             version.minor1 == HW1_C6_HOSTED_VERSION_MINOR &&
                             version.patch1 == HW1_C6_HOSTED_VERSION_PATCH;
  portEXIT_CRITICAL(&sLock);
  return ESP_OK;
}

}  // namespace

#if defined(RADIO_C6_UART_TX_PIN)
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
      err = uart_set_pin(kConsoleUart, UART_PIN_NO_CHANGE, board::kC6UartTx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
      sConsoleStop = false;
      // 4096 B: 3072 left 740 B free on the P4X-EYE (2026-10-29), the tightest stack of ours.
      if (xTaskCreate(consoleTask, "c6_console", 4096, nullptr, 3, &sConsoleTask) != pdPASS) err = ESP_ERR_NO_MEM;
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
bool radioHalConsoleMirrorSupported() { return true; }
#else
// This assembly has no P4-connected companion UART console.
esp_err_t radioHalConsoleMirror(bool on) { return on ? ESP_ERR_NOT_SUPPORTED : ESP_OK; }
bool radioHalConsoleMirrorOn() { return false; }
bool radioHalConsoleMirrorSupported() { return false; }
#endif

esp_err_t radioHalPrepare() {
  quietHostedLogs();
  ESP_RETURN_ON_ERROR(prepareBoard(), kTag, "Board preparation failed");
  ESP_RETURN_ON_ERROR(registerEvents(), kTag, "Hosted events");
  // The mirror runs for the boot (owner decision B13): the companion's own
  // boot messages are seen on every boot, and System_RadioCompanion stops it
  // when setup() ends unless `c6console on` or a fault wants it. Not fatal.
  radioHalConsoleMirror(true);
  esp_err_t err = ESP_FAIL;
  for (int attempt = 1; attempt <= kBootAttempts; attempt++) {
    // A failed start leaves Hosted initialised (Arduino's hostedInit keeps its
    // flag: esp_hosted_connect_to_slave returns ESP_OK even when the slave
    // never answered), so a second start would reuse the dead session: no C6
    // reset, no wait, and the version query fails in the same millisecond
    // (bench log 2026-11-08: attempts 1 and 2 both failed at 26756 ms). Close
    // it first, as the companion's Recover does before every retry.
    if (attempt > 1) {
      // The first start failed, which is a companion fault: radioCompanionBootInit
      // reads it (radioHalBootRetried) and keeps the mirror for the time a
      // fault earns (System_RadioCompanion.cpp).
      sBootRetried = true;
      radioHalStopTransport();
    }
    err = radioHalStartTransport();
    if (err == ESP_OK) break;
    ESP_LOGW(kTag, "C6 did not come up (attempt %d of %d): %s", attempt, kBootAttempts, esp_err_to_name(err));
  }
  sPrepared = err == ESP_OK;
  return err;
}

bool radioHalPrepared() { return sPrepared; }
bool radioHalBootRetried() { return sBootRetried; }

static bool updateTransportUp() {
  portENTER_CRITICAL(&sLock);
  const bool up = sTransportUp && !sTransportLost;
  portEXIT_CRITICAL(&sLock);
  return up && hostedIsInitialized();
}

bool radioHalTransportUp() {
  portENTER_CRITICAL(&sLock);
  const bool normal = !sUpdateOnly;
  portEXIT_CRITICAL(&sLock);
  return normal && updateTransportUp();
}

bool radioHalEspNowAvailable() {
  if (!radioHalTransportUp()) return false;
  const esp_now_hosted_bridge_state_t state = esp_now_hosted_bridge_state();
  return state == ESP_NOW_HOSTED_BRIDGE_PRESENT || state == ESP_NOW_HOSTED_BRIDGE_PRESENT_NO_INFO;
}

const RadioCompanionIdentity& radioHalIdentity() { return sIdentity; }

uint8_t radioHalCompanionBleLinks() {
  // CONFIG_BT_LE_MAX_CONNECTIONS defaults to 3 on the C6 (IDF 5.5).
  constexpr uint8_t kUnreportedLinks = 3;
  portENTER_CRITICAL(&sLock);
  const uint8_t reported = sSessionBleLinks;
  portEXIT_CRITICAL(&sLock);
  const uint8_t links = reported ? reported : kUnreportedLinks;
  return links < HW1_C6_BLE_MAX_CONNECTIONS ? links : HW1_C6_BLE_MAX_CONNECTIONS;
}

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
    id.bleMaxConnections = info.ble_max_connections;
    copyBounded(id.build, sizeof(id.build), info.build, sizeof(info.build));
  } else {
    id.infoKnown = false;
  }
  portENTER_CRITICAL(&sLock);
  sIdentity = id;
  if (id.infoKnown) sSessionBleLinks = id.bleMaxConnections;
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
  out.hciTxDownDrops = esp_hosted_hw1_hci_down_drops();
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

static bool restartLatched() {
  portENTER_CRITICAL(&sLock);
  const bool latched = sRestartLatched;
  portEXIT_CRITICAL(&sLock);
  return latched;
}

static esp_err_t startTransport(bool allowLegacyUpdate) {
  if (restartLatched()) return ESP_ERR_INVALID_STATE;  // the P4 restarts with the C6 held
  ESP_RETURN_ON_ERROR(prepareBoard(), kTag, "Board preparation failed");
  ESP_RETURN_ON_ERROR(registerEvents(), kTag, "Hosted events");
  // A new link starts from a clean slate on both sides.
  hostedSetTransportLost(false);
  esp_now_hosted_host_set_offline(false);
  esp_now_hosted_host_reset_local();
  portENTER_CRITICAL(&sLock);
  sTransportLost = false;
  sUpdateOnly = allowLegacyUpdate;
  sIdentity = {};
  sSessionBleLinks = 0;
  portEXIT_CRITICAL(&sLock);
  // Hosted's own reset pulse releases a held companion; the pin reconfigures.
  const esp_err_t err = startOnce(allowLegacyUpdate);
  if (err != ESP_OK) {
    portENTER_CRITICAL(&sLock);
    sTransportUp = false;
    sSessionBleLinks = 0;
    portEXIT_CRITICAL(&sLock);
    return err;
  }
  if (sUpdateOnly) {
    ESP_LOGW(kTag, "Legacy C6 transport available for firmware update only");
    return updateTransportUp() ? ESP_OK : ESP_FAIL;
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
  const uint8_t bleLinks = radioHalCompanionBleLinks();
  if (bleLinks < HW1_C6_BLE_MAX_CONNECTIONS) {
    ESP_LOGW(kTag, "C6 allows %u BLE links (%s), this image is built for %d; reflash the C6 (tools/p4/companion/README.md)",
             bleLinks, id.infoKnown && id.bleMaxConnections ? "reported" : "limit not reported, assumed",
             HW1_C6_BLE_MAX_CONNECTIONS);
  }
  return ESP_OK;
}

esp_err_t radioHalStartTransport() { return startTransport(false); }
esp_err_t radioHalStartUpdateTransport() { return startTransport(true); }

void radioHalStopTransport() {
  // After radioHalSilenceForRestart, Hosted stays up on purpose: its teardown
  // deletes tasks that may be inside an SDMMC transaction (an op that was
  // already running when the restart began).
  if (restartLatched()) return;
  if (hostedIsInitialized()) {
    // Arduino closes the transport once neither Wi-Fi nor BLE claims it. The
    // callers have stopped both; clear a stale BLE claim so the close happens.
    if (hostedIsBLEActive()) hostedReleaseBLE();
    hostedDeinitWiFi();
  }
  portENTER_CRITICAL(&sLock);
  sTransportUp = false;
  sSessionBleLinks = 0;
  portEXIT_CRITICAL(&sLock);
}

void radioHalMarkTransportLost() {
  hostedSetTransportLost(true);
  esp_now_hosted_host_set_offline(true);
  portENTER_CRITICAL(&sLock);
  sTransportLost = true;
  sTransportUp = false;
  sSessionBleLinks = 0;
  portEXIT_CRITICAL(&sLock);
}

bool radioHalTransportLost() {
  portENTER_CRITICAL(&sLock);
  const bool lost = sTransportLost;
  portEXIT_CRITICAL(&sLock);
  return lost;
}

// Drive C6 EN low and latch the pad (gpio_hold_en): while it is held, a
// gpio_set_level/gpio_config on the pin, Hosted's own reset pulse included,
// does not reach the pad. The hold is in the LP domain and survives the CPU
// reset; prepareBoard() releases it at the next boot before Hosted drives EN.
static void latchC6EnableLow() {
  gpio_hold_dis(board::kC6Enable);
  gpio_set_level(board::kC6Enable, 0);
  gpio_set_direction(board::kC6Enable, GPIO_MODE_OUTPUT);
  gpio_hold_en(board::kC6Enable);
}

esp_err_t radioHalHold(bool hold) {
  // A restart is under way and the C6 is latched in reset: it stays there.
  if (restartLatched()) return hold ? ESP_OK : ESP_ERR_INVALID_STATE;
  if (hold && hostedIsInitialized()) return ESP_ERR_INVALID_STATE;  // stop the transport first
  ESP_RETURN_ON_ERROR(prepareBoard(), kTag, "Board preparation failed");
  gpio_hold_dis(board::kC6Enable);
  ESP_RETURN_ON_ERROR(gpio_set_level(board::kC6Enable, hold ? 0 : 1), kTag, "C6 EN level");
  ESP_RETURN_ON_ERROR(gpio_set_direction(board::kC6Enable, GPIO_MODE_OUTPUT), kTag, "C6 EN direction");
  // A release that raced radioHalSilenceForRestart (checked above before it
  // latched, wrote EN after it) puts the line back: whoever writes last with
  // the latch set leaves EN low.
  if (!hold && restartLatched()) {
    latchC6EnableLow();
    return ESP_ERR_INVALID_STATE;
  }
  portENTER_CRITICAL(&sLock);
  sHeld = hold;
  if (hold) {
    sTransportUp = false;
    sSessionBleLinks = 0;
  }
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
  // companion in reset through sleep; prepareBoard() releases the hold at
  // the next boot before Hosted drives the pin.
  if (radioHalHold(true) != ESP_OK) return;
  // On the ESP32-P4 (rev 3.0 and later) a held pad keeps its level through
  // deep sleep by itself; chips without per-IO deep-sleep hold also need the
  // global switch. An older P4 revision releases the line during sleep, which
  // only lets the companion boot idle without a host until the next reset.
  gpio_hold_en(board::kC6Enable);
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
  gpio_deep_sleep_hold_en();
#endif
}

void radioHalSilenceForRestart() {
  // The caller restarts the P4 next (rebootDevice). esp_restart() would run
  // esp_wifi_remote's esp_wifi_stop shutdown handler, an RPC over SDIO taken
  // in the last milliseconds before the SDMMC reset; Hosted's own restart
  // helper (hosted_restart_host) unregisters it for the same reason.
  esp_unregister_shutdown_handler(reinterpret_cast<shutdown_handler_t>(&esp_wifi_stop));
  // The latch flag goes first, so radioHalHold(false), startTransport and
  // radioHalStopTransport refuse from here on (see below).
  portENTER_CRITICAL(&sLock);
  sRestartLatched = true;
  sHeld = true;
  sTransportUp = false;
  sSessionBleLinks = 0;
  sHostedLinkUp = false;
  portEXIT_CRITICAL(&sLock);
  // Then stop Hosted TX at the fence while the C6 still runs. The users keep
  // running (lwIP's reply to `reboot`, TCP ACKs, BLE HCI, RPC polls), and each
  // packet they queued after the C6 went into reset became a CMD53 with no
  // answer: "sdmmc_send_cmd returned 0x107", "Failed to send data",
  // "Unrecoverable host sdio state" during the drain (bench 2026-11-08, 7 of
  // 15 reboots). With the fence set every new packet is refused and freed by
  // its producer, without a log on that task (Bluedroid's hciT overflowed its
  // stack printing one; radioHalRestartTxDrops counts them instead); the
  // short wait lets the write task send what was already
  // queued to a C6 that still acknowledges it. Receive is left alone. The
  // fence is one-way (a slave INIT from a Start op cannot reopen it), and it
  // leaves Hosted's transport_state at TX_ACTIVE: lowering that state would
  // make any transport_drv_reconfigure() in the window (a Wi-Fi init, a BLE
  // open) pulse the C6's reset and run SDIO card init on slot 1. Without a
  // live link there is nothing queued to flush.
  const bool txWasOpen = is_transport_tx_ready();
  esp_hosted_hw1_fence_tx();
  if (txWasOpen) vTaskDelay(pdMS_TO_TICKS(kHostedTxFlushMs));
  // Hold the C6 in reset so it stops sending: no new rx data or interrupts,
  // and a transfer it was in the middle of ends on the SDMMC timeout instead
  // of streaming into the reset. Hosted stays initialised on purpose: its
  // teardown deletes tasks that may be inside an SDMMC transaction, and its
  // users are still running.
  //
  // The pad is latched low with gpio_hold_en: a Start/Recover op that was
  // already inside hostedInitWiFi (Hosted's transport_gpio_reset pulses EN,
  // at the start and every 50 slave-ready polls) cannot raise it again during
  // the drain.
  // Hosted never touches the hold in this build (host power save is off, so
  // H_HOST_PS_ALLOWED is 0). EN stays low through the CPU reset; prepareBoard()
  // releases the hold at the next boot before Hosted's reset pulse, as after
  // deep sleep.
  latchC6EnableLow();
}

uint32_t radioHalRestartTxDrops() { return esp_hosted_hw1_tx_fence_drops(); }

esp_err_t radioHalUpdateBegin() {
  if (!updateTransportUp()) return ESP_ERR_INVALID_STATE;
  return esp_hosted_slave_ota_begin();
}

esp_err_t radioHalUpdateWrite(const uint8_t* data, size_t len) {
  if (!data || !len) return ESP_ERR_INVALID_ARG;
  if (!updateTransportUp()) return ESP_ERR_INVALID_STATE;
  // Old factory companions cannot reassemble the modern 4 KB OTA request.
  // Keep their protobuf request inside a single transport packet. Qualified
  // companions retain the existing larger transfer size.
  const size_t limit = sIdentity.versionKnown ? len : 1024;
  while (len) {
    const size_t chunk = len < limit ? len : limit;
    const esp_err_t err = esp_hosted_slave_ota_write(const_cast<uint8_t*>(data),
                                                   static_cast<uint32_t>(chunk));
    if (err != ESP_OK) return err;
    data += chunk;
    len -= chunk;
  }
  return ESP_OK;
}

esp_err_t radioHalUpdateEnd() {
  if (!updateTransportUp()) return ESP_ERR_INVALID_STATE;
  return esp_hosted_slave_ota_end();
}

esp_err_t radioHalUpdateActivate() {
  if (!updateTransportUp()) return ESP_ERR_INVALID_STATE;
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
