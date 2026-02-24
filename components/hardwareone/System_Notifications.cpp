/**
 * System Notifications - Centralized notification dispatch
 * 
 * Routes notifications to OLED toast/ribbon, web UI toast, and
 * potentially other outputs (G2 glasses, LED, buzzer) in the future.
 * All functions are no-ops when the relevant output is disabled.
 * SSE is an optional transport - notifications work without the web server.
 */

#include "System_Notifications.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY
#include "OLED_UI.h"
#include "OLED_Utils.h"
#endif

#include <Arduino.h>
#include <cstdio>

// Real impl in WebServer_Server.cpp; inline no-op stub when HTTP server disabled
#if ENABLE_HTTP_SERVER
#include "WebServer_Server.h"
#else
#include "System_SensorStubs.h"
#endif

// ============================================================================
// Notification Source Context Tracking
// ============================================================================

// Global context for tracking the source of the current command/action
static struct {
  uint8_t source;
  char subsource[32];
} gNotificationContext = { NOTIF_SOURCE_UNKNOWN, "" };

// Set notification context (called before executing commands)
void setNotificationContext(uint8_t source, const char* subsource) {
  gNotificationContext.source = source;
  if (subsource && subsource[0]) {
    strncpy(gNotificationContext.subsource, subsource, sizeof(gNotificationContext.subsource) - 1);
    gNotificationContext.subsource[sizeof(gNotificationContext.subsource) - 1] = '\0';
  } else {
    gNotificationContext.subsource[0] = '\0';
  }
}

// Clear notification context (called after command completes)
void clearNotificationContext() {
  gNotificationContext.source = NOTIF_SOURCE_UNKNOWN;
  gNotificationContext.subsource[0] = '\0';
}

// Convert level string to numeric level for notification queue
static uint8_t levelToNum(const char* level) {
  if (!level) return 0;
  if (strcmp(level, "success") == 0) return 1;
  if (strcmp(level, "warning") == 0) return 2;
  if (strcmp(level, "error") == 0) return 3;
  return 0;  // info
}

// Build a small JSON payload and push to all web clients via SSE
// Also adds to persistent OLED notification queue with source tracking
static void notifyWeb(const char* level, const char* msg, uint32_t ms = 4000) {
  char json[128];
  snprintf(json, sizeof(json), "{\"level\":\"%s\",\"msg\":\"%s\",\"ms\":%u}",
           level, msg, (unsigned)ms);
  broadcastEventToAllSessions("notification", json);
  
  // Add to persistent OLED notification queue with source context
  #if ENABLE_OLED_DISPLAY
  oledNotificationAdd(msg, levelToNum(level), gNotificationContext.source, 
                      gNotificationContext.subsource[0] ? gNotificationContext.subsource : nullptr);
  #endif
}

// ============================================================================
// Cooldown tracking - prevents churn from rapid-fire identical notifications
// ============================================================================

static uint32_t sLastBatteryLowMs = 0;
static uint32_t sLastBatteryCritMs = 0;
static uint32_t sLastUSBMs = 0;
static const uint32_t NOTIFY_COOLDOWN_MS = 30000;  // 30s between same battery/power toasts

static bool notifyCooldownOk(uint32_t& lastMs) {
  uint32_t now = millis();
  if (now - lastMs < NOTIFY_COOLDOWN_MS) return false;
  lastMs = now;
  return true;
}

// ============================================================================
// Pairing / Connection Events
// ============================================================================

void notifyPairConnected(const char* peerName) {
  #if ENABLE_OLED_DISPLAY
  oledPairingRibbonShow(peerName, PairingRibbonIcon::LINK, 3000, true);
  #endif
  char msg[48];
  snprintf(msg, sizeof(msg), "Paired: %s", peerName ? peerName : "device");
  notifyWeb("success", msg, 3000);
}

void notifyPairDisconnected(const char* peerName) {
  #if ENABLE_OLED_DISPLAY
  oledPairingRibbonShow(peerName, PairingRibbonIcon::LINK_OFF, 4000, true);
  #endif
  char msg[48];
  snprintf(msg, sizeof(msg), "Disconnected: %s", peerName ? peerName : "device");
  notifyWeb("warning", msg, 4000);
}

void notifyPairHandshakeComplete(const char* peerName) {
  #if ENABLE_OLED_DISPLAY
  oledPairingRibbonShow(peerName, PairingRibbonIcon::LINK, 3000, true);
  #endif
  char msg[48];
  snprintf(msg, sizeof(msg), "Handshake: %s", peerName ? peerName : "device");
  notifyWeb("success", msg, 3000);
}

// ============================================================================
// Remote Command Events
// ============================================================================

void notifyRemoteCommandResult(const char* deviceName, bool success, const char* commandText) {
  char msg[64];
  if (commandText && commandText[0]) {
    // Extract just the command name (first word) to keep it concise
    char cmdName[32];
    const char* space = strchr(commandText, ' ');
    if (space) {
      size_t len = space - commandText;
      if (len > sizeof(cmdName) - 1) len = sizeof(cmdName) - 1;
      strncpy(cmdName, commandText, len);
      cmdName[len] = '\0';
    } else {
      strncpy(cmdName, commandText, sizeof(cmdName) - 1);
      cmdName[sizeof(cmdName) - 1] = '\0';
    }
    snprintf(msg, sizeof(msg), "%s", cmdName);
  } else {
    // Fallback to generic message
    snprintf(msg, sizeof(msg), "Remote");
  }
  #if ENABLE_OLED_DISPLAY
  // Update in place if banner is already showing "Running: ..." - swaps icon without re-animating
  oledNotificationBannerUpdate(msg, success ? PairingRibbonIcon::SUCCESS : PairingRibbonIcon::ERROR_ICON, 1500);
  #endif
  notifyWeb(success ? "success" : "error", msg, 3000);
}

void notifyRemoteCommandReceived(const char* deviceName, const char* commandText) {
  char msg[48];
  if (commandText && commandText[0]) {
    // Extract just the command name (first word) to keep it concise
    char cmdName[32];
    const char* space = strchr(commandText, ' ');
    if (space) {
      size_t len = space - commandText;
      if (len > sizeof(cmdName) - 1) len = sizeof(cmdName) - 1;
      strncpy(cmdName, commandText, len);
      cmdName[len] = '\0';
    } else {
      strncpy(cmdName, commandText, sizeof(cmdName) - 1);
      cmdName[sizeof(cmdName) - 1] = '\0';
    }
    snprintf(msg, sizeof(msg), "Running: %s", cmdName);
  } else {
    // Fallback to device name if no command text
    snprintf(msg, sizeof(msg), "From: %s", deviceName ? deviceName : "peer");
  }
  #if ENABLE_OLED_DISPLAY
  // Show SYNC icon to indicate command is in progress
  oledNotificationBannerShow(msg, PairingRibbonIcon::SYNC, 2000);
  #endif
  notifyWeb("info", msg, 2000);
}

// ============================================================================
// WiFi Events
// ============================================================================

void notifyWiFiConnected(const char* ipAddress) {
  char msg[32];
  snprintf(msg, sizeof(msg), "WiFi: %s", ipAddress ? ipAddress : "connected");
  #if ENABLE_OLED_DISPLAY
  oledNotificationBannerShow(msg, PairingRibbonIcon::SUCCESS, 3000);
  #endif
  notifyWeb("success", msg, 3000);
}

void notifyWiFiDisconnected() {
  #if ENABLE_OLED_DISPLAY
  oledNotificationBannerShow("WiFi off", PairingRibbonIcon::INFO_ICON, 2000);
  #endif
  notifyWeb("info", "WiFi off", 2000);
}

// ============================================================================
// Audio / Volume Events
// ============================================================================

void notifyVolumeChanged(int volume, int maxVolume) {
  char msg[24];
  snprintf(msg, sizeof(msg), "Vol: %d/%d", volume, maxVolume);
  #if ENABLE_OLED_DISPLAY
  oledNotificationBannerShow(msg, PairingRibbonIcon::INFO_ICON, 1500);
  #endif
  notifyWeb("info", msg, 1500);
}

// ============================================================================
// BLE / G2 Glasses Events
// ============================================================================

void notifyBleDeviceConnected(const char* deviceName) {
  char msg[48];
  snprintf(msg, sizeof(msg), "BLE: %s", deviceName ? deviceName : "connected");
  #if ENABLE_OLED_DISPLAY
  oledNotificationBannerShow(msg, PairingRibbonIcon::SUCCESS, 2500);
  #endif
  notifyWeb("success", msg, 2500);
}

void notifyBleDeviceDisconnected(const char* deviceName) {
  char msg[48];
  snprintf(msg, sizeof(msg), "BLE: %s off", deviceName ? deviceName : "device");
  #if ENABLE_OLED_DISPLAY
  oledNotificationBannerShow(msg, PairingRibbonIcon::INFO_ICON, 2000);
  #endif
  notifyWeb("info", msg, 2000);
}

void notifyGestureNavToggled(bool enabled) {
  const char* msg = enabled ? "Gesture nav ON" : "Gesture nav OFF";
  #if ENABLE_OLED_DISPLAY
  oledNotificationBannerShow(msg, PairingRibbonIcon::INFO_ICON, 1500);
  #endif
  notifyWeb("info", msg, 1500);
}

// ============================================================================
// Battery / Power Events
// ============================================================================

void notifyPowerUSBConnected() {
  if (!notifyCooldownOk(sLastUSBMs)) return;
  #if ENABLE_OLED_DISPLAY
  oledNotificationBannerShow("USB connected", PairingRibbonIcon::SUCCESS, 2000);
  #endif
  notifyWeb("success", "USB connected", 2000);
}

void notifyPowerUSBDisconnected() {
  if (!notifyCooldownOk(sLastUSBMs)) return;
  #if ENABLE_OLED_DISPLAY
  oledNotificationBannerShow("USB disconnected", PairingRibbonIcon::WARNING_ICON, 2000);
  #endif
  notifyWeb("warning", "USB disconnected", 2000);
}

void notifyBatteryLow(int percent) {
  if (!notifyCooldownOk(sLastBatteryLowMs)) return;
  char msg[24];
  snprintf(msg, sizeof(msg), "Batt low: %d%%", percent);
  #if ENABLE_OLED_DISPLAY
  oledNotificationBannerShow(msg, PairingRibbonIcon::WARNING_ICON, 3000);
  #endif
  notifyWeb("warning", msg, 3000);
}

void notifyBatteryCritical(int percent) {
  if (!notifyCooldownOk(sLastBatteryCritMs)) return;
  char msg[24];
  snprintf(msg, sizeof(msg), "Battery: %d%%!", percent);
  #if ENABLE_OLED_DISPLAY
  oledNotificationBannerShow(msg, PairingRibbonIcon::ERROR_ICON, 4000, true);
  #endif
  notifyWeb("error", msg, 4000);
}

// ============================================================================
// Login Events
// ============================================================================

static uint32_t sLastLoginFailMs = 0;
static const uint32_t LOGIN_FAIL_COOLDOWN_MS = 10000;  // 10s between fail notifications

void notifyLoginSuccess(const char* username, const char* transport) {
  char msg[48];
  snprintf(msg, sizeof(msg), "Login: %s", username ? username : "user");
  #if ENABLE_OLED_DISPLAY
  oledNotificationBannerShow(msg, PairingRibbonIcon::SUCCESS, 2000);
  #endif
  notifyWeb("success", msg, 2000);
}

void notifyLoginFailed(const char* username, const char* transport) {
  if (!notifyCooldownOk(sLastLoginFailMs)) return;
  sLastLoginFailMs = millis();  // Reset after cooldown check
  char msg[48];
  snprintf(msg, sizeof(msg), "Login failed: %s", username ? username : "user");
  #if ENABLE_OLED_DISPLAY
  oledNotificationBannerShow(msg, PairingRibbonIcon::ERROR_ICON, 2000);
  #endif
  notifyWeb("error", msg, 2000);
}

// ============================================================================
// Settings Change Events
// ============================================================================

void notifySettingChanged(const char* key, const char* value) {
  char msg[48];
  snprintf(msg, sizeof(msg), "Set: %s=%s", key ? key : "?", value ? value : "?");
  #if ENABLE_OLED_DISPLAY
  oledNotificationBannerShow(msg, PairingRibbonIcon::INFO_ICON, 1500);
  #endif
  notifyWeb("info", msg, 1500);
}

// ============================================================================
// Sensor Start/Stop Events
// ============================================================================

void notifySensorStarted(const char* sensorName, bool success) {
  char msg[32];
  snprintf(msg, sizeof(msg), "%s: %s", sensorName ? sensorName : "Sensor",
           success ? "started" : "failed");
  #if ENABLE_OLED_DISPLAY
  oledNotificationBannerShow(msg,
    success ? PairingRibbonIcon::SUCCESS : PairingRibbonIcon::ERROR_ICON, 1500);
  #endif
  notifyWeb(success ? "success" : "error", msg, 1500);
}

void notifySensorStopped(const char* sensorName) {
  char msg[32];
  snprintf(msg, sizeof(msg), "%s: stopped", sensorName ? sensorName : "Sensor");
  #if ENABLE_OLED_DISPLAY
  oledNotificationBannerShow(msg, PairingRibbonIcon::INFO_ICON, 1500);
  #endif
  notifyWeb("info", msg, 1500);
}

// ============================================================================
// Feature Toggle Events
// ============================================================================

void notifyEspNowStarted(bool success) {
  const char* msg = success ? "ESP-NOW: on" : "ESP-NOW: failed";
  #if ENABLE_OLED_DISPLAY
  oledNotificationBannerShow(msg,
    success ? PairingRibbonIcon::SUCCESS : PairingRibbonIcon::ERROR_ICON, 2000);
  #endif
  notifyWeb(success ? "success" : "error", msg, 2000);
}

void notifyEspNowStopped() {
  #if ENABLE_OLED_DISPLAY
  oledNotificationBannerShow("ESP-NOW: off", PairingRibbonIcon::INFO_ICON, 2000);
  #endif
  notifyWeb("info", "ESP-NOW: off", 2000);
}

// ============================================================================
// File Operation Events
// ============================================================================

void notifyFileDeleted(const char* path) {
  char msg[48];
  // Show just the filename, not the full path
  const char* name = path;
  if (path) {
    const char* slash = strrchr(path, '/');
    if (slash) name = slash + 1;
  }
  snprintf(msg, sizeof(msg), "Deleted: %s", name ? name : "file");
  #if ENABLE_OLED_DISPLAY
  oledNotificationBannerShow(msg, PairingRibbonIcon::WARNING_ICON, 2000);
  #endif
  notifyWeb("warning", msg, 2000);
}

// ============================================================================
// WiFi Network Management Events
// ============================================================================

void notifyWiFiNetworkAdded(const char* ssid) {
  char msg[40];
  snprintf(msg, sizeof(msg), "WiFi saved: %s", ssid ? ssid : "network");
  #if ENABLE_OLED_DISPLAY
  oledNotificationBannerShow(msg, PairingRibbonIcon::SUCCESS, 2000);
  #endif
  notifyWeb("success", msg, 2000);
}

void notifyWiFiNetworkRemoved(const char* ssid) {
  char msg[40];
  snprintf(msg, sizeof(msg), "WiFi removed: %s", ssid ? ssid : "network");
  #if ENABLE_OLED_DISPLAY
  oledNotificationBannerShow(msg, PairingRibbonIcon::WARNING_ICON, 2000);
  #endif
  notifyWeb("warning", msg, 2000);
}

// ============================================================================
// Voice / ESP-SR Events
// ============================================================================

void notifyVoiceListening() {
  #if ENABLE_OLED_DISPLAY
  oledNotificationBannerShow("Listening...", PairingRibbonIcon::INFO_ICON, 2000);
  #endif
  notifyWeb("info", "Listening...", 2000);
}

void notifyVoiceCommandResult(const char* command, bool success) {
  char msg[32];
  snprintf(msg, sizeof(msg), "Voice: %s", command ? command : "cmd");
  #if ENABLE_OLED_DISPLAY
  oledNotificationBannerShow(msg, success ? PairingRibbonIcon::SUCCESS : PairingRibbonIcon::ERROR_ICON, 2000);
  #endif
  notifyWeb(success ? "success" : "error", msg, 2000);
}
