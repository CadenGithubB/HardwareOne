/**
 * System_MQTT - Home Assistant MQTT Integration
 */

#ifndef SYSTEM_MQTT_H
#define SYSTEM_MQTT_H

#include "System_BuildConfig.h"

#if ENABLE_WIFI && ENABLE_MQTT

#include <Arduino.h>

// MQTT lifecycle functions
bool startMQTT();
void stopMQTT();
void mqttTick();
bool isMqttConnected();

// MQTT publishing
void publishMQTTSensorData();

// External sensor access (for web page)
int getExternalSensorCount();
bool getExternalSensor(int index, String& topic, String& name, String& value, unsigned long& lastUpdate);

// MQTT CLI commands
const char* cmd_openmqtt(const String& args);
const char* cmd_closemqtt(const String& args);
const char* cmd_mqttstatus(const String& args);

// Command table for registration
extern const struct CommandEntry mqttCommands[];
extern const size_t mqttCommandsCount;

// Settings module for registration
extern const struct SettingsModule mqttSettingsModule;

#else

#include <Arduino.h>

// Stubs when MQTT is disabled
inline bool startMQTT() { return false; }
inline void stopMQTT() {}
inline void mqttTick() {}
inline void publishMQTTSensorData() {}
inline bool isMqttConnected() { return false; }
inline int getExternalSensorCount() { return 0; }
inline bool getExternalSensor(int, String&, String&, String&, unsigned long&) { return false; }

#endif // ENABLE_WIFI && ENABLE_MQTT

#endif // SYSTEM_MQTT_H
