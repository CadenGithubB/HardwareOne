#ifndef SYSTEM_NOTIFICATIONS_H
#define SYSTEM_NOTIFICATIONS_H

/**
 * System Notifications - Centralized notification dispatch
 * 
 * All visual/audible notifications go through this module.
 * Internally dispatches to OLED toast/ribbon, G2 glasses, LED, etc.
 * based on what's enabled in BuildConfig. Callers never need to
 * check ENABLE_OLED_DISPLAY or include OLED headers directly.
 * 
 * Usage: #include "System_Notifications.h" and call notify*() functions.
 * All functions are safe to call regardless of build config (no-ops when disabled).
 */

#include <stdint.h>

// Forward declare notification source enum from OLED_Utils.h
enum NotificationSource : uint8_t;

// ============================================================================
// Notification Source Context
// ============================================================================

// Set the source context for notifications (call before executing commands)
// source: NotificationSource enum value
// subsource: IP address, device name, or MAC address (optional)
void setNotificationContext(uint8_t source, const char* subsource = nullptr);

// Clear the source context (call after command completes)
void clearNotificationContext();

// ============================================================================
// Pairing / Connection Events
// ============================================================================

void notifyPairConnected(const char* peerName);
void notifyPairDisconnected(const char* peerName);
void notifyPairHandshakeComplete(const char* peerName);

// ============================================================================
// Remote Command Events
// ============================================================================

// We sent a command to peer and got a result back
// commandText can be nullptr (will show generic OK/FAIL) or the actual command/result text
void notifyRemoteCommandResult(const char* deviceName, bool success, const char* commandText = nullptr);

// Peer sent a command to us (about to execute)
// commandText is the actual command being executed (e.g., "sensor status")
void notifyRemoteCommandReceived(const char* deviceName, const char* commandText = nullptr);

// ============================================================================
// WiFi Events
// ============================================================================

void notifyWiFiConnected(const char* ipAddress);
void notifyWiFiDisconnected();

// ============================================================================
// Audio / Volume Events
// ============================================================================

// Volume changed (any source - FM radio, speaker, etc.)
void notifyVolumeChanged(int volume, int maxVolume);

// ============================================================================
// BLE / G2 Glasses Events
// ============================================================================

void notifyBleDeviceConnected(const char* deviceName);
void notifyBleDeviceDisconnected(const char* deviceName);
void notifyGestureNavToggled(bool enabled);

// ============================================================================
// Battery / Power Events
// ============================================================================

// USB power plugged in or unplugged
void notifyPowerUSBConnected();
void notifyPowerUSBDisconnected();

// Battery dropped below low threshold
void notifyBatteryLow(int percent);

// Battery dropped below critical threshold
void notifyBatteryCritical(int percent);

// ============================================================================
// Login Events
// ============================================================================

// Login succeeded - always notifies (no rate limit)
void notifyLoginSuccess(const char* username, const char* transport);

// Login failed - rate-limited to avoid spamming on brute force attempts
void notifyLoginFailed(const char* username, const char* transport);

// ============================================================================
// Settings Change Events
// ============================================================================

// A setting was changed via CLI or settings editor
// key: setting name (e.g., "oledBrightness"), value: new value as string
void notifySettingChanged(const char* key, const char* value);

// ============================================================================
// Sensor Start/Stop Events
// ============================================================================

// A sensor was started or stopped (name: "IMU", "GPS", "Thermal", etc.)
void notifySensorStarted(const char* sensorName, bool success);
void notifySensorStopped(const char* sensorName);

// ============================================================================
// Feature Toggle Events
// ============================================================================

// ESP-NOW initialized or deinitialized
void notifyEspNowStarted(bool success);
void notifyEspNowStopped();

// ============================================================================
// File Operation Events
// ============================================================================

// A file was deleted (destructive action worth notifying)
void notifyFileDeleted(const char* path);

// ============================================================================
// WiFi Network Management Events
// ============================================================================

// A WiFi network was added or removed from saved list
void notifyWiFiNetworkAdded(const char* ssid);
void notifyWiFiNetworkRemoved(const char* ssid);

// ============================================================================
// Voice / ESP-SR Events
// ============================================================================

// Wake word detected - device is listening for a command
void notifyVoiceListening();

// Voice command executed with result
void notifyVoiceCommandResult(const char* command, bool success);

#endif // SYSTEM_NOTIFICATIONS_H
