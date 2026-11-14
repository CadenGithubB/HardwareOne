// System_MQTTControlTopic.h - the two topics this client uses for commands
//
// <base>/command carries the {user, pass, cmd} envelope this client executes
// and <base>/response carries the reply it publishes. Neither is sensor data.
// The broker delivers the command topic to the same event handler as the
// external-sensor subscriptions, and a wildcard subscription can deliver the
// response topic too, so the sensor table asks here before it keeps a message.
//
// Kept dependency-free beside System_MQTTLifecycleGate.h so the host tests
// compile the same code the firmware uses.
#pragma once

#include <stddef.h>
#include <string.h>

constexpr char kMqttCommandTopicSuffix[] = "/command";
constexpr char kMqttResponseTopicSuffix[] = "/response";

// True when `topic` (length-delimited, as esp-mqtt hands it over) is exactly
// `base` followed by `suffix`.
inline bool mqttTopicIsBasePlus(const char* topic, size_t topicLen,
                                const char* base, size_t baseLen,
                                const char* suffix) {
  if (!topic || !base || !suffix) return false;
  const size_t suffixLen = strlen(suffix);
  if (topicLen != baseLen + suffixLen) return false;
  return memcmp(topic, base, baseLen) == 0 &&
         memcmp(topic + baseLen, suffix, suffixLen) == 0;
}

// True when `topic` is the command topic or the response topic of a client
// whose base topic is `base`.
inline bool mqttIsControlTopic(const char* topic, size_t topicLen,
                               const char* base, size_t baseLen) {
  return mqttTopicIsBasePlus(topic, topicLen, base, baseLen, kMqttCommandTopicSuffix) ||
         mqttTopicIsBasePlus(topic, topicLen, base, baseLen, kMqttResponseTopicSuffix);
}
