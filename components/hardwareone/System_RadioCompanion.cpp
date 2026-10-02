#include "System_RadioCompanion.h"

#if HW1_RADIO_COMPANION

#include <Arduino.h>
#include <atomic>
#include <new>
#include <inttypes.h>
#include <string.h>

#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "BLE_Peers.h"
#include "Bluetooth.h"
#include "G2_Glasses.h"
#include "Radio_CompanionCore.h"
#include "System_AuthIdentity.h"
#include "System_BuildConfig.h"
#include "System_Command.h"
#include "System_CommandTypes.h"
#include "System_Debug.h"
#include "System_ESPNow.h"
#include "System_Filesystem.h"
#include "System_EventCatalog.h"
#include "System_Events.h"
#include "System_MemUtil.h"
#include "System_OTASafety.h"
#include "System_Settings.h"
#include "System_Utils.h"
#include "System_VFS.h"
#include "esp_now_hosted_companion.h"
#include "esp_now_hosted_rpc.h"
#if ENABLE_WIFI
#include "System_WiFi.h"
#endif

extern bool submitDeferredToCmdExec(ExecReq::DeferredFn fn, void* arg);
#if ENABLE_ESPNOW
extern bool isEspNowInitialized();
#endif

namespace {

using hw1::radio::CompanionAction;
using hw1::radio::CompanionMonitor;
using hw1::radio::CompanionOutage;
using hw1::radio::CompanionPolicy;
using hw1::radio::CompanionState;
using hw1::radio::CompanionStats;
using hw1::radio::companionOutageName;
using hw1::radio::companionStateName;

constexpr const char* kTag = "C6";
constexpr uint32_t kLowMemoryBytes = 20 * 1024;      // companion internal heap alarm
constexpr uint32_t kLowMemoryEventCooldownMs = 60000;
constexpr size_t kUpdateChunk = 4096;
constexpr size_t kMaxImageBytes = 1920 * 1024;       // the C6's OTA slot size
constexpr size_t kEventRing = 16;

enum class Op : uint8_t { None = 0, Start, Recover, Hold, Release, Update };

portMUX_TYPE sLock = portMUX_INITIALIZER_UNLOCKED;
CompanionMonitor sMonitor;                   // guarded by sLock
RadioHalEventData sEvents[kEventRing];       // guarded by sLock
uint8_t sEvHead = 0, sEvTail = 0;
uint32_t sEventsDropped = 0;
std::atomic<bool> sOpInFlight{false};
std::atomic<uint8_t> sOp{static_cast<uint8_t>(Op::None)};
bool sBootInitDone = false;
bool sEverOnline = false;
bool sMismatchReported = false;
bool sBridgeAbsentReported = false;
uint32_t sLastLowMemoryEventMs = 0;
char sLastResult[160] = "none";

// What was running before a stop, so recovery, hold and update put it back.
struct RadioUsers {
  bool espnow = false;
  bool wifi = false;              // radio on for Wi-Fi (also true while ESP-NOW runs)
  bool bleServer = false;
  bool g2Client = false;
  bool radioPoweredOff = false;   // stopUsers ran `radiopower off`; `radiopower on` replays its snapshot
};
RadioUsers sHeldUsers;  // restored by `c6hold off`

template <typename F>
void withMonitor(F fn) {
  portENTER_CRITICAL(&sLock);
  fn(sMonitor);
  portEXIT_CRITICAL(&sLock);
}

CompanionState monitorState() {
  portENTER_CRITICAL(&sLock);
  const CompanionState s = sMonitor.state();
  portEXIT_CRITICAL(&sLock);
  return s;
}

CompanionStats monitorStats() {
  portENTER_CRITICAL(&sLock);
  const CompanionStats s = sMonitor.stats();
  portEXIT_CRITICAL(&sLock);
  return s;
}

void setLastResult(const char* text) {
  portENTER_CRITICAL(&sLock);
  strncpy(sLastResult, text, sizeof(sLastResult) - 1);
  sLastResult[sizeof(sLastResult) - 1] = '\0';
  portEXIT_CRITICAL(&sLock);
}

void applyPolicyFromSettings() {
  CompanionPolicy policy;
  policy.heartbeatMs = gSettings.c6HeartbeatSec > 0 ? static_cast<uint32_t>(gSettings.c6HeartbeatSec) * 1000u : 0;
  policy.autoRecover = gSettings.c6AutoRecover;
  policy.autoHold = gSettings.c6AutoHold;
  withMonitor([&](CompanionMonitor& m) { m.setPolicy(policy); });
  radioHalConfigureMonitoring(static_cast<uint16_t>(gSettings.c6HeartbeatSec < 0 ? 0 : gSettings.c6HeartbeatSec),
                              kLowMemoryBytes);
}

// ---- events from the HAL (esp_event task): copy only ------------------------
void onHalEvent(const RadioHalEventData& event) {
  portENTER_CRITICAL(&sLock);
  const uint8_t next = static_cast<uint8_t>((sEvHead + 1) % kEventRing);
  if (next == sEvTail) {
    sEventsDropped++;
  } else {
    sEvents[sEvHead] = event;
    sEvHead = next;
  }
  portEXIT_CRITICAL(&sLock);
}

bool popEvent(RadioHalEventData& out) {
  portENTER_CRITICAL(&sLock);
  const bool have = sEvHead != sEvTail;
  if (have) {
    out = sEvents[sEvTail];
    sEvTail = static_cast<uint8_t>((sEvTail + 1) % kEventRing);
  }
  portEXIT_CRITICAL(&sLock);
  return have;
}

// ---- radio users -----------------------------------------------------------
RadioUsers snapshotUsers() {
  RadioUsers u;
#if ENABLE_ESPNOW
  u.espnow = isEspNowInitialized();
#endif
#if ENABLE_WIFI
  u.wifi = wifiRadioOn();
#endif
#if ENABLE_BLUETOOTH
  u.bleServer = isBleServerInitialized();
  u.g2Client = isG2ClientInitialized();
#endif
  return u;
}

bool anyRadioUser() {
  const RadioUsers u = snapshotUsers();
  return u.espnow || u.wifi || u.bleServer || u.g2Client;
}

// Stop everything that uses the transport, in the order the radio stack needs.
void stopUsers(RadioUsers& users) {
#if ENABLE_BLUETOOTH
  if (users.g2Client) deinitG2Client();
  if (users.bleServer) deinitBluetooth();
#endif
#if ENABLE_WIFI
  // `radiopower off` stops ESP-NOW, the HTTP server and Wi-Fi in order and
  // keeps a snapshot of exactly what ran, which `radiopower on` replays.
  if (users.wifi) {
    cmd_radiopower("off");
    users.radioPoweredOff = true;
  }
#endif
#if ENABLE_ESPNOW
  if (users.espnow && isEspNowInitialized()) cmd_espnow_deinit("");
#endif
}

void restoreUsers(const RadioUsers& users) {
#if ENABLE_WIFI
  if (users.radioPoweredOff) cmd_radiopower("on");  // ESP-NOW first, then Wi-Fi, from its snapshot
#endif
#if ENABLE_ESPNOW
  if (users.espnow && !isEspNowInitialized()) cmd_espnow_init("");
#endif
#if ENABLE_BLUETOOTH
  if (users.bleServer) {
    if (initBluetooth()) startBLEAdvertising();
  } else if (users.g2Client) {
    if (initG2Client()) {
      vTaskDelay(pdMS_TO_TICKS(2000));
      bleBootReconnect();
    }
  }
#endif
}

// The boot would have started these; the companion came up too late for it.
void bringUpConfiguredUsers() {
  RadioUsers wanted;
#if ENABLE_WIFI
  wanted.wifi = gSettings.wifiEnabled && gSettings.wifiAutoStart;
#endif
#if ENABLE_ESPNOW
  wanted.espnow = gSettings.espnowEnabled && gSettings.espnowAutoStart;
#endif
#if ENABLE_BLUETOOTH
  if (gSettings.bleEnabled && gSettings.bleAutoStart) {
    wanted.bleServer = gSettings.bleMode == BLE_MODE_SERVER;
    wanted.g2Client = gSettings.bleMode == BLE_MODE_G2_CLIENT;
  }
#endif
  const RadioUsers running = snapshotUsers();
#if ENABLE_WIFI
  if (wanted.wifi && !running.wifi) setupWiFi();
#endif
#if ENABLE_ESPNOW
  if (wanted.espnow && !running.espnow) cmd_espnow_init("");
#endif
#if ENABLE_BLUETOOTH
  if (wanted.bleServer && !running.bleServer) {
    if (initBluetooth()) startBLEAdvertising();
  } else if (wanted.g2Client && !running.g2Client) {
    if (initG2Client()) {
      vTaskDelay(pdMS_TO_TICKS(2000));
      bleBootReconnect();
    }
  }
#endif
}

// ---- after the transport is up --------------------------------------------
void confirmPendingImage() {
  const RadioCompanionIdentity& id = radioHalIdentity();
  if (!id.infoKnown || id.imageState != ESP_NOW_HOSTED_IMAGE_PENDING_VERIFY) return;
  const esp_err_t err = radioHalBridgeConfirmImage();
  if (err == ESP_OK) {
    logSystemEvent("C6", "companion image confirmed (slot %u, %s)", id.runningSlot, id.build);
    systemEventPost(SYSEVT_COMPANION_UPDATED, id.build, "confirmed");
    radioHalRefreshIdentity();
  } else {
    logSystemEvent("C6", "companion image confirmation failed: %s", esp_err_to_name(err));
  }
}

void reportIdentity() {
  const RadioCompanionIdentity& id = radioHalIdentity();
  char subject[48];
  snprintf(subject, sizeof(subject), "%" PRIu32 ".%" PRIu32 ".%" PRIu32, id.major, id.minor, id.patch);
  systemEventPost(SYSEVT_COMPANION_ONLINE, subject, radioHalBridgeStateName(id.bridge));
  if (id.versionKnown && !id.versionMatches && !sMismatchReported) {
    sMismatchReported = true;
    systemEventPost(SYSEVT_COMPANION_MISMATCH, subject, HW1_C6_HOSTED_TAG);
    logSystemEvent("C6", "companion runs ESP-Hosted %s, qualified %s", subject, HW1_C6_HOSTED_TAG);
  }
  if (id.bridge == RadioBridgeState::Absent && !sBridgeAbsentReported) {
    sBridgeAbsentReported = true;
    logSystemEvent("C6", "companion firmware has no ESP-NOW bridge; Wi-Fi/BLE only (tools/p4/companion)");
  }
  confirmPendingImage();
}

// The transport came up on its own (a feature started it after an idle
// close): the companion rebooted, so its monitoring configuration is gone.
void onTransportResumed() {
  radioHalRefreshIdentity();
  applyPolicyFromSettings();
  reportIdentity();
  sEverOnline = true;
}

// ---- procedures (run on the command executor task) -------------------------
void finishOp() {
  sOp.store(static_cast<uint8_t>(Op::None));
  sOpInFlight.store(false);
}

void startProc(void*) {
  const bool ok = radioHalStartTransport() == ESP_OK;
  withMonitor([&](CompanionMonitor& m) { m.onStartResult(millis(), ok); });
  if (ok) {
    applyPolicyFromSettings();
    reportIdentity();
    setLastResult("start: companion online");
    if (!sEverOnline) bringUpConfiguredUsers();  // the boot skipped them
    sEverOnline = true;
  } else {
    setLastResult("start: companion did not answer");
    systemEventPost(SYSEVT_COMPANION_OFFLINE, "start_failed", "no answer on SDIO");
  }
  finishOp();
}

void recoverProc(void*) {
  RadioUsers users = snapshotUsers();
  // The restart resets the companion either way, so no clean stop request is
  // worth waiting for: mark the host side lost so every remote call fails
  // fast instead of timing out one by one on a companion that may be dead.
  radioHalMarkTransportLost();
  logSystemEvent("C6", "recovery: stopping radio users (espnow=%d wifi=%d ble=%d g2=%d)", users.espnow,
                 users.wifi, users.bleServer, users.g2Client);
  stopUsers(users);
  radioHalStopTransport();
  const bool ok = radioHalStartTransport() == ESP_OK;
  if (ok) {
    applyPolicyFromSettings();
    reportIdentity();
    restoreUsers(users);
    setLastResult("recovery: companion back, radio restored");
    logSystemEvent("C6", "recovery complete");
  } else {
    setLastResult("recovery: companion did not come back");
    systemEventPost(SYSEVT_COMPANION_OFFLINE, "recovery_failed", "no answer on SDIO");
    logSystemEvent("C6", "recovery failed: companion did not answer");
  }
  withMonitor([&](CompanionMonitor& m) { m.onRecoveryResult(millis(), ok); });
  finishOp();
}

void holdProc(void*) {
  sHeldUsers = snapshotUsers();
  stopUsers(sHeldUsers);
  radioHalStopTransport();
  const esp_err_t err = radioHalHold(true);
  if (err == ESP_OK) {
    withMonitor([&](CompanionMonitor& m) { m.onHold(millis()); });
    setLastResult("hold: companion held in reset");
    systemEventPost(SYSEVT_COMPANION_OFFLINE, "held", "radio off by request");
  } else {
    setLastResult("hold: failed");
  }
  finishOp();
}

void releaseProc(void*) {
  radioHalHold(false);
  withMonitor([&](CompanionMonitor& m) {
    m.onRelease(millis());
    m.onStartAttempt(millis());
  });
  const bool ok = radioHalStartTransport() == ESP_OK;
  withMonitor([&](CompanionMonitor& m) { m.onStartResult(millis(), ok); });
  if (ok) {
    applyPolicyFromSettings();
    reportIdentity();
    restoreUsers(sHeldUsers);
    sHeldUsers = RadioUsers();
    setLastResult("release: companion online");
  } else {
    setLastResult("release: companion did not answer");
  }
  finishOp();
}

// ---- firmware update ---------------------------------------------------------
struct UpdateJob {
  String path;
  AuthContext ctx;  // the caller's identity, for the guarded file open
};

// A candidate file must be an ESP32-C6 application image that carries this
// bridge; anything else would leave the board without ESP-NOW or without a
// radio at all until the next cable flash.
bool validateImage(File& file, size_t size, char* why, size_t whyCap) {
  if (size < 4096 || size > kMaxImageBytes) {
    snprintf(why, whyCap, "size %u bytes is not a C6 application image", static_cast<unsigned>(size));
    return false;
  }
  esp_image_header_t header;
  if (file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
    snprintf(why, whyCap, "could not read the image header");
    return false;
  }
  if (header.magic != ESP_IMAGE_HEADER_MAGIC) {
    snprintf(why, whyCap, "not an ESP firmware image (magic 0x%02X)", header.magic);
    return false;
  }
  if (header.chip_id != ESP_CHIP_ID_ESP32C6) {
    snprintf(why, whyCap, "image is for chip id 0x%04X, not the ESP32-C6", static_cast<unsigned>(header.chip_id));
    return false;
  }
  const char* marker = ESP_NOW_HOSTED_BRIDGE_MARKER;
  const size_t markerLen = strlen(marker);
  uint8_t* buf = static_cast<uint8_t*>(ps_alloc(kUpdateChunk + markerLen, AllocPref::PreferPSRAM, "c6.scan"));
  if (!buf) {
    snprintf(why, whyCap, "out of memory for the image scan");
    return false;
  }
  bool found = false;
  size_t carry = 0;
  file.seek(0);
  while (!found) {
    const size_t got = file.read(buf + carry, kUpdateChunk);
    if (got == 0) break;
    const size_t total = carry + got;
    for (size_t i = 0; i + markerLen <= total; i++) {
      if (memcmp(buf + i, marker, markerLen) == 0) {
        found = true;
        break;
      }
    }
    carry = total >= markerLen - 1 ? markerLen - 1 : total;
    memmove(buf, buf + total - carry, carry);
  }
  free(buf);
  file.seek(0);
  if (!found) {
    snprintf(why, whyCap, "image carries no HardwareOne ESP-NOW bridge (%s)", marker);
    return false;
  }
  return true;
}

void updateProc(void* arg) {
  UpdateJob* job = static_cast<UpdateJob*>(arg);
  char why[128] = {};
  bool ok = false;
  File file = VFS::openGuarded(job->path, "r", job->ctx);
  if (!file) {
    snprintf(why, sizeof(why), "cannot open %s", job->path.c_str());
  } else if (!validateImage(file, file.size(), why, sizeof(why))) {
    // why is set
  } else {
    const size_t size = file.size();
    withMonitor([&](CompanionMonitor& m) { m.onUpdateStarted(); });
    // ESP-NOW and BLE would compete with the transfer for the link and are
    // interrupted by the companion's reboot anyway. Wi-Fi may stay up: the
    // transport carries the update whether or not Wi-Fi uses it.
    RadioUsers users = snapshotUsers();
#if ENABLE_BLUETOOTH
    if (users.g2Client) deinitG2Client();
    if (users.bleServer) deinitBluetooth();
#endif
#if ENABLE_ESPNOW
    if (users.espnow) cmd_espnow_deinit("");
#endif
    // Closing BLE while Wi-Fi is off closes the transport too (Arduino deinits
    // Hosted when nothing uses it); the transfer needs it up.
    esp_err_t err = radioHalTransportUp() ? ESP_OK : radioHalStartTransport();
    BROADCAST_PRINTF("[C6] Updating companion firmware from %s (%u bytes)", job->path.c_str(),
                     static_cast<unsigned>(size));
    uint8_t* buf = static_cast<uint8_t*>(ps_alloc(kUpdateChunk, AllocPref::PreferPSRAM, "c6.update"));
    if (err == ESP_OK) err = buf ? radioHalUpdateBegin() : ESP_ERR_NO_MEM;
    size_t sent = 0;
    uint8_t lastPercent = 0;
    while (err == ESP_OK && sent < size) {
      const size_t got = file.read(buf, kUpdateChunk);
      if (got == 0) {
        err = ESP_ERR_INVALID_SIZE;
        break;
      }
      err = radioHalUpdateWrite(buf, got);
      sent += got;
      const uint8_t percent = static_cast<uint8_t>((sent * 100) / size);
      if (percent / 10 != lastPercent / 10) {
        BROADCAST_PRINTF("[C6] %u%%", percent);
        lastPercent = percent;
      }
    }
    if (buf) free(buf);
    if (err == ESP_OK) err = radioHalUpdateEnd();
    if (err == ESP_OK) err = radioHalUpdateActivate();
    if (err != ESP_OK) {
      snprintf(why, sizeof(why), "transfer failed after %u bytes: %s", static_cast<unsigned>(sent),
               esp_err_to_name(err));
      withMonitor([&](CompanionMonitor& m) { m.onUpdateFinished(millis(), radioHalTransportUp()); });
      restoreUsers(users);
    } else {
      // The companion reboots into the new image two seconds after activate.
      // Tear the host side down as lost, then rebuild the link, which resets
      // the companion once more and brings it up on the new image.
      BROADCAST_PRINTF("[C6] Image accepted; restarting the companion");
      vTaskDelay(pdMS_TO_TICKS(2500));
      radioHalMarkTransportLost();
      RadioUsers rest = users;  // ESP-NOW and BLE are already down
      rest.espnow = false;
      rest.bleServer = false;
      rest.g2Client = false;
      stopUsers(rest);
      users.radioPoweredOff = rest.radioPoweredOff;
      radioHalStopTransport();
      ok = radioHalStartTransport() == ESP_OK;
      if (ok) {
        applyPolicyFromSettings();
        const RadioCompanionIdentity& id = radioHalIdentity();
        if (id.bridge == RadioBridgeState::Present || id.bridge == RadioBridgeState::PresentNoInfo) {
          confirmPendingImage();
          systemEventPost(SYSEVT_COMPANION_UPDATED, id.build, "running");
          logSystemEvent("C6", "companion updated to %s (Hosted %" PRIu32 ".%" PRIu32 ".%" PRIu32 ")", id.build,
                         id.major, id.minor, id.patch);
        } else {
          // Never confirmed: the bootloader rolls back on the next reset.
          snprintf(why, sizeof(why),
                   "new image answers Hosted but not the bridge; it rolls back on the next companion reset");
          ok = false;
        }
        reportIdentity();
        restoreUsers(users);
      } else {
        snprintf(why, sizeof(why), "companion did not come back after the update");
      }
      withMonitor([&](CompanionMonitor& m) { m.onUpdateFinished(millis(), radioHalTransportUp()); });
    }
  }
  if (file) file.close();
  if (ok) {
    setLastResult("update: companion updated and confirmed");
    BROADCAST_PRINTF("[C6] Update complete");
  } else {
    char text[160];
    snprintf(text, sizeof(text), "update: %s", why);
    setLastResult(text);
    BROADCAST_PRINTF("[C6] Update failed: %s", why);
    logSystemEvent("C6", "companion update failed: %s", why);
  }
  delete job;
  finishOp();
}

bool submitOp(Op op, ExecReq::DeferredFn fn, void* arg) {
  bool expected = false;
  if (!sOpInFlight.compare_exchange_strong(expected, true)) return false;
  sOp.store(static_cast<uint8_t>(op));
  if (!submitDeferredToCmdExec(fn, arg)) {
    finishOp();
    return false;
  }
  return true;
}

const char* opName(Op op) {
  switch (op) {
    case Op::Start: return "start";
    case Op::Recover: return "recovery";
    case Op::Hold: return "hold";
    case Op::Release: return "release";
    case Op::Update: return "update";
    default: return "none";
  }
}

// ---- event processing on the main loop ------------------------------------
void handleEvent(const RadioHalEventData& event, uint32_t now) {
  switch (event.type) {
    case RadioHalEvent::TransportUp: {
      const CompanionState before = monitorState();
      withMonitor([&](CompanionMonitor& m) { m.onTransportUp(now); });
      // A feature re-opened the transport after an idle close: the companion
      // rebooted for it, so its identity and monitoring must be re-read.
      if (before == CompanionState::Idle && !sOpInFlight.load()) onTransportResumed();
      break;
    }
    case RadioHalEvent::TransportDown:
      withMonitor([&](CompanionMonitor& m) { m.onTransportDown(now); });
      break;
    case RadioHalEvent::TransportFailure:
      withMonitor([&](CompanionMonitor& m) { m.onTransportFailure(now); });
      if (!sOpInFlight.load()) {
        systemEventPost(SYSEVT_COMPANION_OFFLINE, "transport_failure", "SDIO link failed");
        logSystemEvent("C6", "transport failure");
      }
      break;
    case RadioHalEvent::Heartbeat:
      withMonitor([&](CompanionMonitor& m) { m.onHeartbeat(now, event.value); });
      break;
    case RadioHalEvent::CompanionBoot: {
      const CompanionState before = monitorState();
      withMonitor([&](CompanionMonitor& m) { m.onCompanionBoot(now, static_cast<uint8_t>(event.value)); });
      const bool expected = sOpInFlight.load() || before != CompanionState::Online;
      systemEventPost(SYSEVT_COMPANION_RESTARTED, radioHalResetReasonLabel(static_cast<uint8_t>(event.value)),
                      expected ? "expected" : "unexpected");
      if (!expected) {
        logSystemEvent("C6", "companion reset unexpectedly (%s)",
                       radioHalResetReasonLabel(static_cast<uint8_t>(event.value)));
      }
      break;
    }
    case RadioHalEvent::Memory:
      if (event.value < kLowMemoryBytes && now - sLastLowMemoryEventMs > kLowMemoryEventCooldownMs) {
        sLastLowMemoryEventMs = now;
        char free[16], minimum[16];
        snprintf(free, sizeof(free), "%" PRIu32, event.value);
        snprintf(minimum, sizeof(minimum), "%" PRIu32, event.value2);
        systemEventPost(SYSEVT_COMPANION_LOW_MEMORY, free, minimum);
      }
      break;
  }
}

void dispatch(CompanionAction action, uint32_t now) {
  switch (action) {
    case CompanionAction::None:
      break;
    case CompanionAction::StartTransport:
      withMonitor([&](CompanionMonitor& m) { m.onStartAttempt(now); });
      logSystemEvent("C6", "retrying the companion transport");
      if (!submitOp(Op::Start, startProc, nullptr)) {
        withMonitor([&](CompanionMonitor& m) { m.onStartResult(now, false); });
      }
      break;
    case CompanionAction::Recover: {
      const CompanionStats stats = monitorStats();
      withMonitor([&](CompanionMonitor& m) { m.onRecoveryStarted(now); });
      systemEventPost(SYSEVT_COMPANION_OFFLINE, companionOutageName(stats.lastOutage), "recovering");
      logSystemEvent("C6", "companion outage (%s): soft recovery", companionOutageName(stats.lastOutage));
      if (!submitOp(Op::Recover, recoverProc, nullptr)) {
        withMonitor([&](CompanionMonitor& m) { m.onRecoveryResult(now, false); });
      }
      break;
    }
    case CompanionAction::HardReboot: {
      const CompanionStats stats = monitorStats();
      systemEventPost(SYSEVT_COMPANION_REBOOT, companionOutageName(stats.lastOutage), "soft recovery kept failing");
      logSystemEvent("C6", "rebooting: companion outage (%s) after %" PRIu32 " soft recoveries",
                     companionOutageName(stats.lastOutage), stats.softRecoveries);
      broadcastOutput("[C6] Companion keeps failing; rebooting to recover the radio");
      vTaskDelay(pdMS_TO_TICKS(750));  // let the notification and log leave the device
      esp_restart();
      break;
    }
    case CompanionAction::Hold:
      if (anyRadioUser()) break;  // a feature came back while idle; keep the transport
      if (radioHalHold(true) == ESP_OK) {
        withMonitor([&](CompanionMonitor& m) { m.onHold(now); });
        logSystemEvent("C6", "idle: companion held in reset");
      }
      break;
  }
}

// ---- status ------------------------------------------------------------------
void fillStatus(JsonObject out, bool full) {
  const uint32_t now = millis();
  const CompanionState state = monitorState();
  const CompanionStats stats = monitorStats();
  const RadioCompanionIdentity& id = radioHalIdentity();
  out["backend"] = "hosted";
  out["state"] = companionStateName(state);
  out["transport_up"] = radioHalTransportUp();
  out["held"] = radioHalHeld();
  out["operation"] = opName(static_cast<Op>(sOp.load()));
  out["outage"] = companionOutageName(stats.lastOutage);
  out["auto_recover"] = (bool)gSettings.c6AutoRecover;
  out["auto_hold"] = (bool)gSettings.c6AutoHold;
  JsonObject fw = out["firmware"].to<JsonObject>();
  if (id.versionKnown) {
    char v[24];
    snprintf(v, sizeof(v), "%" PRIu32 ".%" PRIu32 ".%" PRIu32, id.major, id.minor, id.patch);
    fw["hosted"] = v;
    fw["match"] = id.versionMatches;
  }
  fw["expected"] = HW1_C6_HOSTED_TAG;
  if (id.descKnown) {
    fw["project"] = id.project;
    fw["version"] = id.version;
    fw["idf"] = id.idf;
    fw["built"] = String(id.date) + " " + id.time;
  }
  JsonObject bridge = out["bridge"].to<JsonObject>();
  bridge["state"] = radioHalBridgeStateName(id.bridge);
  if (id.infoKnown) {
    bridge["wire"] = id.bridgeWire;
    bridge["image_state"] = id.imageState == ESP_NOW_HOSTED_IMAGE_PENDING_VERIFY ? "pending"
                            : id.imageState == ESP_NOW_HOSTED_IMAGE_INVALID         ? "invalid"
                            : id.imageState == ESP_NOW_HOSTED_IMAGE_VALID           ? "valid"
                                                                                    : "unknown";
    bridge["slot"] = id.runningSlot;
    bridge["build"] = id.build;
  }
  JsonObject hb = out["heartbeat"].to<JsonObject>();
  hb["interval_sec"] = gSettings.c6HeartbeatSec;
  hb["seen"] = stats.heartbeatsSeen;
  hb["age_ms"] = stats.heartbeatsSeen ? (unsigned long)(now - stats.lastHeartbeatAt) : 0;
  {
    portENTER_CRITICAL(&sLock);
    const uint32_t missed = sMonitor.missedHeartbeats(now);
    portEXIT_CRITICAL(&sLock);
    hb["missed"] = missed;
  }
  out["boot_count"] = stats.bootCount;
  out["last_reset_reason"] = radioHalResetReasonLabel(stats.lastResetReason);
  if (!full) return;
  if (id.chipKnown) {
    JsonObject chip = out["chip"].to<JsonObject>();
    chip["id"] = id.chipId;
    chip["name"] = id.chip;
  }
  if (id.infoKnown) {
    JsonObject live = out["companion"].to<JsonObject>();
    live["uptime_ms"] = id.uptimeMs;
    live["free_heap"] = id.freeHeap;
    live["min_free_heap"] = id.minFreeHeap;
    live["cpu_mhz"] = id.cpuMhz;
    live["reset_reason"] = radioHalResetReasonLabel(id.resetReason);
    live["native_espnow"] = id.nativeEspNowVersion;
  }
  const RadioCompanionMemory& mem = radioHalLastMemory();
  if (mem.known) {
    JsonObject m = out["memory"].to<JsonObject>();
    m["total"] = mem.totalHeap;
    m["free"] = mem.freeHeap;
    m["min_free"] = mem.minFreeHeap;
    m["internal_free"] = mem.internalFree;
    m["internal_largest"] = mem.internalLargest;
    m["age_ms"] = (unsigned long)(now - mem.atMs);
  }
  RadioBridgeStats bs;
  radioHalBridgeStats(bs);
  JsonObject counters = out["bridge_counters"].to<JsonObject>();
  counters["timeouts"] = bs.timeouts;
  counters["transport_errors"] = bs.transportErrors;
  counters["events_dropped"] = bs.eventsDropped;
  counters["malformed"] = bs.malformed;
  counters["stale"] = bs.stale;
  JsonObject rec = out["recovery"].to<JsonObject>();
  rec["start_attempts"] = stats.startAttempts;
  rec["start_failures"] = stats.startFailures;
  rec["soft"] = stats.softRecoveries;
  rec["failed"] = stats.recoveriesFailed;
  rec["hard_requested"] = stats.hardRebootsRequested;
  rec["outage_age_ms"] = stats.lastOutage == CompanionOutage::None ? 0 : (unsigned long)(now - stats.lastOutageAt);
  rec["next_retry_ms"] = stats.retryPending ? (long)(stats.nextRetryAt - now) : 0;
  rec["online_age_ms"] = state == CompanionState::Online ? (unsigned long)(now - stats.onlineSince) : 0;
  rec["events_dropped"] = sEventsDropped;
  char last[sizeof(sLastResult)];
  portENTER_CRITICAL(&sLock);
  memcpy(last, sLastResult, sizeof(last));
  portEXIT_CRITICAL(&sLock);
  out["last_result"] = last;  // char[]: ArduinoJson copies it
}

// ---- commands ---------------------------------------------------------------
const char* cmd_c6status(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  // A live view costs a few round trips; refresh when the companion answers.
  if (radioHalTransportUp() && !sOpInFlight.load()) {
    radioHalRefreshIdentity();
    RadioCompanionMemory mem;
    radioHalQueryMemory(mem);
  }
  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    JsonObject root = doc.to<JsonObject>();
    root["schema"] = 1;
    fillStatus(root, true);
    static char* buf = nullptr;
    if (!buf) buf = static_cast<char*>(ps_alloc(2048, AllocPref::PreferPSRAM, "c6status.json"));
    if (!buf) return "{\"error\":\"oom\"}";
    serializeJson(doc, buf, 2048);
    return buf;
  }
  const uint32_t now = millis();
  const CompanionState state = monitorState();
  const CompanionStats stats = monitorStats();
  const RadioCompanionIdentity& id = radioHalIdentity();
  broadcastOutput("ESP32-C6 radio companion:");
  BROADCAST_PRINTF("  State: %s%s%s", companionStateName(state), radioHalHeld() ? " (held in reset)" : "",
                   sOpInFlight.load() ? " (busy)" : "");
  if (id.versionKnown) {
    BROADCAST_PRINTF("  Hosted firmware: %" PRIu32 ".%" PRIu32 ".%" PRIu32 " (%s %s)", id.major, id.minor, id.patch,
                     id.versionMatches ? "matches" : "differs from", HW1_C6_HOSTED_TAG);
  } else {
    broadcastOutput("  Hosted firmware: unknown (companion never answered)");
  }
  if (id.descKnown) BROADCAST_PRINTF("  Image: %s %s, IDF %s, built %s %s", id.project, id.version, id.idf, id.date, id.time);
  if (id.chipKnown) BROADCAST_PRINTF("  Chip: %s (0x%04" PRIX32 ")", id.chip, id.chipId);
  BROADCAST_PRINTF("  ESP-NOW bridge: %s", radioHalBridgeStateName(id.bridge));
  if (id.infoKnown) {
    BROADCAST_PRINTF("  Running slot: ota_%u, image %s, build %s", id.runningSlot,
                     id.imageState == ESP_NOW_HOSTED_IMAGE_PENDING_VERIFY ? "pending confirmation"
                     : id.imageState == ESP_NOW_HOSTED_IMAGE_INVALID         ? "invalid"
                                                                             : "valid",
                     id.build);
    BROADCAST_PRINTF("  Companion uptime: %" PRIu32 " s, CPU %" PRIu32 " MHz, last reset: %s", id.uptimeMs / 1000, id.cpuMhz,
                     radioHalResetReasonLabel(id.resetReason));
    BROADCAST_PRINTF("  Companion heap: %" PRIu32 " free, %" PRIu32 " minimum", id.freeHeap, id.minFreeHeap);
  }
  const RadioCompanionMemory& mem = radioHalLastMemory();
  if (mem.known) {
    BROADCAST_PRINTF("  Memory monitor: %" PRIu32 " internal free (largest %" PRIu32 "), total heap %" PRIu32,
                     mem.internalFree, mem.internalLargest, mem.totalHeap);
  }
  if (gSettings.c6HeartbeatSec > 0) {
    portENTER_CRITICAL(&sLock);
    const uint32_t missed = sMonitor.missedHeartbeats(now);
    portEXIT_CRITICAL(&sLock);
    BROADCAST_PRINTF("  Heartbeat: every %d s, %" PRIu32 " seen, last %lu ms ago, %" PRIu32 " missed",
                     gSettings.c6HeartbeatSec, stats.heartbeatsSeen,
                     stats.heartbeatsSeen ? (unsigned long)(now - stats.lastHeartbeatAt) : 0UL, missed);
  } else {
    broadcastOutput("  Heartbeat: disabled (c6heartbeat <seconds> to enable)");
  }
  BROADCAST_PRINTF("  Companion boots seen: %" PRIu32 " (last reason: %s)", stats.bootCount,
                   radioHalResetReasonLabel(stats.lastResetReason));
  RadioBridgeStats bs;
  radioHalBridgeStats(bs);
  BROADCAST_PRINTF("  Bridge counters: %" PRIu32 " timeouts, %" PRIu32 " transport errors, %" PRIu32 " dropped events",
                   bs.timeouts, bs.transportErrors, bs.eventsDropped);
  BROADCAST_PRINTF("  Recovery: %" PRIu32 " starts (%" PRIu32 " failed), %" PRIu32 " soft recoveries (%" PRIu32
                   " failed), %" PRIu32 " reboots requested; last outage: %s",
                   stats.startAttempts, stats.startFailures, stats.softRecoveries, stats.recoveriesFailed,
                   stats.hardRebootsRequested, companionOutageName(stats.lastOutage));
  if (stats.retryPending) BROADCAST_PRINTF("  Next start attempt in %ld ms", (long)(stats.nextRetryAt - now));
  BROADCAST_PRINTF("  Auto-recover: %s, auto-hold when idle: %s", gSettings.c6AutoRecover ? "on" : "off",
                   gSettings.c6AutoHold ? "on" : "off");
  char last[sizeof(sLastResult)];
  portENTER_CRITICAL(&sLock);
  memcpy(last, sLastResult, sizeof(last));
  portEXIT_CRITICAL(&sLock);
  BROADCAST_PRINTF("  Last operation: %s", last);
  return "OK";
}

const char* cmd_c6restart(const String&) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (otaSafetyIsPendingVerification()) return "Error: refused while the new firmware is in OTA health probation";
  const CompanionState state = monitorState();
  if (state == CompanionState::Updating) return "Error: a companion update is in progress";
  withMonitor([&](CompanionMonitor& m) {
    m.onManualStartRequested();
    m.onRecoveryStarted(millis());
  });
  logSystemEvent("C6", "manual restart requested");
  if (!submitOp(Op::Recover, recoverProc, nullptr)) {
    withMonitor([&](CompanionMonitor& m) { m.onRecoveryResult(millis(), false); });
    return "Error: another companion operation is running";
  }
  return "Restarting the C6 companion; radio features come back when it answers (watch c6status)";
}

const char* cmd_c6hold(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);
  String arg = a.arg(0);
  arg.toLowerCase();
  const bool held = radioHalHeld();
  bool wantHold;
  if (arg == "on") wantHold = true;
  else if (arg == "off") wantHold = false;
  else if (arg.length() == 0) {
    return held ? "C6 is held in reset (c6hold off to release)" : "C6 is running (c6hold on to hold it in reset)";
  } else return "Usage: c6hold [on|off]";
  if (wantHold == held) return wantHold ? "C6 already held" : "C6 already released";
  if (monitorState() == CompanionState::Updating) return "Error: a companion update is in progress";
  if (wantHold) {
    if (!submitOp(Op::Hold, holdProc, nullptr)) return "Error: another companion operation is running";
    return "Holding the C6 in reset: Wi-Fi, Bluetooth and ESP-NOW stop (c6hold off restores them)";
  }
  withMonitor([&](CompanionMonitor& m) { m.onManualStartRequested(); });
  if (!submitOp(Op::Release, releaseProc, nullptr)) return "Error: another companion operation is running";
  return "Releasing the C6 and restoring the radio features that were running";
}

const char* cmd_c6update(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);
  String path;
  const char* quoted = requireQuotedPath(a, 0, path);
  if (quoted) return quoted;
  if (otaSafetyIsPendingVerification()) return "Error: refused while the new firmware is in OTA health probation";
  if (!radioHalTransportUp()) return "Error: the C6 is not answering; c6restart first";
  if (sOpInFlight.load()) return "Error: another companion operation is running";
  if (!VFS::existsGuarded(path, currentAuthContext())) return "Error: file not found or not permitted";
  UpdateJob* job = new (std::nothrow) UpdateJob();
  if (!job) return "Error: out of memory";
  job->path = path;
  job->ctx = currentAuthContext();
  if (!submitOp(Op::Update, updateProc, job)) {
    delete job;
    return "Error: another companion operation is running";
  }
  return "C6 update started: the image is checked, streamed, activated and confirmed; ESP-NOW and Bluetooth "
         "pause until the companion is back (watch c6status)";
}

const char* cmd_c6confirm(const String&) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!radioHalEspNowAvailable()) return "Error: the companion bridge is not answering";
  const esp_err_t err = radioHalBridgeConfirmImage();
  if (err == ESP_ERR_NOT_SUPPORTED) return "Error: the companion firmware predates image confirmation";
  if (err != ESP_OK) {
    if (!ensureDebugBuffer()) return "Error: confirmation failed";
    snprintf(getDebugBuffer(), 1024, "Error: confirmation failed: %s", esp_err_to_name(err));
    return getDebugBuffer();
  }
  radioHalRefreshIdentity();
  return "Companion image confirmed; it will not roll back";
}

const char* setBoolSetting(const String& argsInput, bool& field, const char* label) {
  CommandArgs a(argsInput);
  String arg = a.arg(0);
  arg.toLowerCase();
  if (arg.length() == 0) {
    if (!ensureDebugBuffer()) return field ? "on" : "off";
    snprintf(getDebugBuffer(), 1024, "%s: %s", label, field ? "on" : "off");
    return getDebugBuffer();
  }
  bool value;
  if (arg == "on" || arg == "1" || arg == "true") value = true;
  else if (arg == "off" || arg == "0" || arg == "false") value = false;
  else return "Usage: [on|off]";
  setSetting(field, value);
  applyPolicyFromSettings();
  if (!ensureDebugBuffer()) return "OK";
  snprintf(getDebugBuffer(), 1024, "%s: %s", label, value ? "on" : "off");
  return getDebugBuffer();
}

const char* cmd_c6autorecover(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return setBoolSetting(argsInput, gSettings.c6AutoRecover, "Companion auto-recover");
}

const char* cmd_c6autohold(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return setBoolSetting(argsInput, gSettings.c6AutoHold, "Companion auto-hold when idle");
}

const char* cmd_c6heartbeat(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);
  String arg = a.arg(0);
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  if (arg.length() == 0) {
    snprintf(getDebugBuffer(), 1024, "Companion heartbeat: %d s (0 = off)", gSettings.c6HeartbeatSec);
    return getDebugBuffer();
  }
  const int seconds = arg.toInt();
  if (seconds < 0 || seconds > 60 || (seconds == 0 && arg != "0")) return "Usage: c6heartbeat <0..60>";
  setSetting(gSettings.c6HeartbeatSec, seconds);
  applyPolicyFromSettings();
  snprintf(getDebugBuffer(), 1024, "Companion heartbeat: %d s%s", seconds,
           seconds ? "" : " (watchdog off; unresponsive companions are only noticed by a failed request)");
  return getDebugBuffer();
}

}  // namespace

// ---- public API --------------------------------------------------------------
void radioCompanionBootInit() {
  if (sBootInitDone) return;
  radioHalSetEventHandler(onHalEvent);
  applyPolicyFromSettings();
  const bool up = radioHalPrepared() && radioHalTransportUp();
  withMonitor([&](CompanionMonitor& m) { m.onBootResult(millis(), up); });
  sBootInitDone = true;
  if (up) {
    sEverOnline = true;
    reportIdentity();
    setLastResult("boot: companion online");
  } else {
    setLastResult("boot: companion did not answer");
    systemEventPost(SYSEVT_COMPANION_OFFLINE, "boot_failed", "no answer on SDIO");
    logSystemEvent("C6", "companion did not answer at boot; radio features skipped, retrying in the background");
  }
}

void radioCompanionLogBootSummary() {
  const RadioCompanionIdentity& id = radioHalIdentity();
  RadioBridgeStats bs;
  radioHalBridgeStats(bs);
  ESP_LOGI(kTag, "RADIO_STATS backend=hosted state=%s hosted=%" PRIu32 ".%" PRIu32 ".%" PRIu32 " match=%d bridge=%s",
           companionStateName(monitorState()), id.major, id.minor, id.patch, id.versionMatches,
           radioHalBridgeStateName(id.bridge));
  ESP_LOGI(kTag, "BRIDGE_STATS malformed=%" PRIu32 " stale=%" PRIu32 " event_drops=%" PRIu32 " timeouts=%" PRIu32
                 " transport_errors=%" PRIu32,
           bs.malformed, bs.stale, bs.eventsDropped, bs.timeouts, bs.transportErrors);
}

void radioCompanionTick() {
  if (!sBootInitDone) return;
  const uint32_t now = millis();
  RadioHalEventData event;
  while (popEvent(event)) handleEvent(event, now);
  if (sOpInFlight.load()) return;  // a procedure owns the transport right now
  CompanionAction action;
  portENTER_CRITICAL(&sLock);
  action = sMonitor.tick(now);
  portEXIT_CRITICAL(&sLock);
  dispatch(action, now);
}

bool radioCompanionOnline() { return monitorState() == CompanionState::Online && radioHalTransportUp(); }

bool radioCompanionBlocksRadio(const char** reason) {
  if (reason) *reason = nullptr;
  if (!sBootInitDone) return false;  // before boot init, the HAL state alone decides
  const CompanionState state = monitorState();
  switch (state) {
    case CompanionState::Online:
    case CompanionState::Idle:
      return false;
    case CompanionState::Held:
      if (sOpInFlight.load()) {
        if (reason) *reason = "the C6 companion is busy";
        return true;
      }
      // A feature start is a request to wake the companion: do it now.
      radioHalHold(false);
      withMonitor([&](CompanionMonitor& m) {
        m.onRelease(millis());
        m.onStartAttempt(millis());
      });
      if (radioHalStartTransport() == ESP_OK) {
        withMonitor([&](CompanionMonitor& m) { m.onStartResult(millis(), true); });
        applyPolicyFromSettings();
        reportIdentity();
        return false;
      }
      withMonitor([&](CompanionMonitor& m) { m.onStartResult(millis(), false); });
      if (reason) *reason = "the C6 companion did not answer after release";
      return true;
    case CompanionState::Starting:
      if (reason) *reason = "the C6 companion is starting";
      return true;
    case CompanionState::Recovering:
      if (reason) *reason = "the C6 companion is recovering";
      return true;
    case CompanionState::Updating:
      if (reason) *reason = "the C6 companion is being updated";
      return true;
    case CompanionState::Offline:
    default:
      if (reason) *reason = "the C6 radio companion is offline (c6status; c6restart to retry now)";
      return true;
  }
}

void radioCompanionBeforeLightSleep() {
  withMonitor([&](CompanionMonitor& m) { m.onWatchdogPaused(millis()); });
}

void radioCompanionAfterLightSleep() {
  withMonitor([&](CompanionMonitor& m) { m.onWatchdogResumed(millis()); });
}

void radioCompanionBeforeDeepSleep() {
  // Deep sleep is "off": nothing on the P4 will use the radio until the next
  // boot, which resets the companion anyway. Stop its users so the companion
  // is not left serving a vanished host, then hold it in reset through sleep.
  if (sOpInFlight.load()) return;
  RadioUsers users = snapshotUsers();
  stopUsers(users);
  radioHalStopTransport();
  radioHalPrepareForDeepSleep();
}

void radioCompanionJson(JsonObject out) { fillStatus(out, false); }

const CommandEntry companionCommands[] = {
  { "c6status",      "ESP32-C6 companion status: firmware, bridge, heartbeat, memory, recovery (add 'json').", false, cmd_c6status },
  { "c6restart",     "Restart the C6 companion in place (radio features stop and come back).", true, cmd_c6restart },
  { "c6hold",        "Hold the C6 in reset (radio fully off) or release it: c6hold [on|off].", true, cmd_c6hold,
    "Usage: c6hold [on|off]\n  on  - stop Wi-Fi, Bluetooth and ESP-NOW and hold the companion in reset\n  off - release it and restore what was running" },
  { "c6update",      "Flash companion firmware from a file over SDIO: c6update \"<path>\".", true, cmd_c6update,
    "Usage: c6update \"/sd/firmware/network_adapter.bin\"\nThe file must be an ESP32-C6 image built by tools/p4/companion (it is checked for the ESP-NOW bridge marker before anything is sent).", true },
  { "c6confirm",     "Confirm the companion's running image so it cannot roll back.", true, cmd_c6confirm },
  { "c6autorecover", "Auto-recover the companion after an outage: c6autorecover [on|off] (persists).", true, cmd_c6autorecover },
  { "c6autohold",    "Hold the companion in reset while no radio feature runs: c6autohold [on|off] (persists).", true, cmd_c6autohold },
  { "c6heartbeat",   "Companion heartbeat interval in seconds, 0 disables the watchdog: c6heartbeat <0..60> (persists).", true, cmd_c6heartbeat },
};
const size_t companionCommandsCount = sizeof(companionCommands) / sizeof(companionCommands[0]);

static const SettingEntry companionSettingsEntries[] = {
  { "c6AutoRecover",  SETTING_BOOL, &gSettings.c6AutoRecover,  1, 0, nullptr, 0, 1,  "Auto-recover after an outage", nullptr, false, nullptr, "c6autorecover" },
  { "c6AutoHold",     SETTING_BOOL, &gSettings.c6AutoHold,     0, 0, nullptr, 0, 1,  "Hold in reset while idle",      nullptr, false, nullptr, "c6autohold" },
  { "c6HeartbeatSec", SETTING_INT,  &gSettings.c6HeartbeatSec, 5, 0, nullptr, 0, 60, "Heartbeat interval (s, 0=off)", nullptr, false, nullptr, "c6heartbeat" },
};

extern const SettingsModule companionSettingsModule = {
  "companion",
  "network.companion",
  companionSettingsEntries,
  sizeof(companionSettingsEntries) / sizeof(companionSettingsEntries[0]),
  radioCompanionOnline,
  "ESP32-C6 radio companion"
};

#endif  // HW1_RADIO_COMPANION
