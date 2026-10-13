// Host harness for the MQTT command reply path (cmd_exec completion).
// The runner inserts MqttCommandReply, enqueueMqttCommandReply and
// mqttCommandCompleted from System_MQTT.cpp. The lifecycle gate and the
// movable admission are the production headers. The esp-mqtt client is a
// small model of the behaviour the reply depends on: a direct publish leaves
// at once, an enqueued item waits for the client task's next loop pass, and
// stopping the client deletes every item still in the outbox.
#include <Arduino.h>
#include <ArduinoJson.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "System_MQTTLifecycleGate.h"
#include "System_MQTTAdmission.h"

void webBatchTestStringAppend() {}

#define PSRAM_JSON_DOC(name) JsonDocument name
static int warnings = 0;
#define WARN_MQTTF(...) (++warnings)
#define DEBUG_MQTT_COMMANDSF(...) ((void)0)

static MqttLifecycleGate sMqttLifecycle;

typedef void* esp_mqtt_client_handle_t;
static int clientToken;
static esp_mqtt_client_handle_t mqttClient = &clientToken;

struct Message {
  std::string topic;
  std::string payload;
};
static std::vector<Message> delivered;  // left the device
static std::vector<Message> outbox;     // queued, not yet sent
static int publishCalls = 0;
static int enqueueCalls = 0;
static uint32_t usesAtLastPublish = 0;

// Unused when a reply path never publishes directly (the --baseline replay).
[[maybe_unused]] static int esp_mqtt_client_publish(
    esp_mqtt_client_handle_t client, const char* topic, const char* data,
    int len, int qos, int retain) {
  if (client != mqttClient || qos != 0 || retain != 0) return -1;
  ++publishCalls;
  usesAtLastPublish = sMqttLifecycle.usesInFlight();
  delivered.push_back({topic, std::string(data, (size_t)len)});
  return 0;
}

static int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t client,
                                   const char* topic, const char* data,
                                   int len, int qos, int retain, bool store) {
  if (client != mqttClient || qos != 0 || retain != 0) return -1;
  ++enqueueCalls;
  if (!store) return -1;  // esp-mqtt does not keep a QoS 0 item without store
  outbox.push_back({topic, std::string(data, (size_t)len)});
  return 0;
}

// One esp-mqtt task loop pass: queued items go out.
static void clientLoopPass() {
  for (const Message& m : outbox) delivered.push_back(m);
  outbox.clear();
}

// mqttTick's pending-stop branch; the client task deletes its outbox on exit.
static bool mainLoopTeardown() {
  if (!sMqttLifecycle.stopPending() || !sMqttLifecycle.tryBeginStop()) {
    return false;
  }
  outbox.clear();
  sMqttLifecycle.finishStop();
  return true;
}

static String redactMQTTCommandResult(const String& output) { return output; }

// INSERT_PRODUCTION_DEFINITIONS

static int failures = 0;
#define CHECK(condition, message)                                       \
  do {                                                                  \
    if (!(condition)) {                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,      \
                   message);                                            \
      ++failures;                                                       \
    }                                                                   \
  } while (0)

static void reset() {
  delivered.clear();
  outbox.clear();
  publishCalls = enqueueCalls = warnings = 0;
  usesAtLastPublish = 0;
}

static void openGate() {
  CHECK(sMqttLifecycle.beginStart(), "stopped gate admits one starter");
  CHECK(sMqttLifecycle.finishStart(true), "start opens admission");
}

// Mirrors submitMqttCommand: the admission moves into a heap reply.
static MqttCommandReply* admittedReply(const char* user, const char* cmd,
                                       const char* routedKind) {
  MqttAdmission use(sMqttLifecycle);
  CHECK(use.admitted(), "open gate admits the command");
  void* storage = std::malloc(sizeof(MqttCommandReply));
  CHECK(storage != nullptr, "reply allocation");
  MqttCommandReply* reply = new (storage) MqttCommandReply();
  reply->use = std::move(use);
  reply->responseTopic = "hw1/response";
  reply->user = user;
  reply->safeCommand = cmd;
  reply->routedKind = routedKind;
  return reply;
}

// A command that stops MQTT (closemqtt, "mqttclientenabled 0") requests the
// stop on cmd_exec before its completion runs. Its reply must leave before
// the completion releases the admission, because the main loop tears the
// client down on its next pass and the outbox goes with it.
static void test_reply_survives_stop_requested_by_the_command() {
  reset();
  openGate();
  MqttCommandReply* reply = admittedReply("admin", "closemqtt", nullptr);
  CHECK(sMqttLifecycle.requestStop(), "the command schedules the stop");
  CHECK(!mainLoopTeardown(), "teardown waits for the held admission");
  mqttCommandCompleted(true, "[MQTT] Client stop scheduled", reply);
  CHECK(sMqttLifecycle.usesInFlight() == 0, "completion releases admission");
  CHECK(mainLoopTeardown(), "teardown follows the release");
  CHECK(delivered.size() == 1,
        "the stop command's reply is delivered despite the teardown");
  CHECK(publishCalls == 1 && outbox.empty(),
        "the reply is published directly while the stop is pending");
  CHECK(usesAtLastPublish == 1,
        "the reply leaves while the admission still blocks teardown");
  if (!delivered.empty()) {
    CHECK(delivered[0].topic == "hw1/response", "reply topic");
    CHECK(delivered[0].payload ==
              "{\"ok\":true,\"user\":\"admin\",\"cmd\":\"closemqtt\","
              "\"result\":\"[MQTT] Client stop scheduled\"}",
          "reply document");
  }
  CHECK(warnings == 0, "no warning for a sent reply");
}

// Without a pending stop the reply goes through the outbox: the completion
// takes the client lock only briefly and does no socket write.
static void test_reply_queued_when_no_stop_pending() {
  reset();
  openGate();
  MqttCommandReply* reply = admittedReply("admin", "status", nullptr);
  mqttCommandCompleted(true, "fine", reply);
  CHECK(enqueueCalls == 1 && publishCalls == 0,
        "a normal reply is enqueued, not written on cmd_exec");
  CHECK(outbox.size() == 1 && delivered.empty(),
        "the reply waits for the client task");
  CHECK(sMqttLifecycle.usesInFlight() == 0, "completion releases admission");
  clientLoopPass();
  CHECK(delivered.size() == 1, "the next loop pass sends the reply");
  if (!delivered.empty()) {
    CHECK(delivered[0].payload ==
              "{\"ok\":true,\"user\":\"admin\",\"cmd\":\"status\","
              "\"result\":\"fine\"}",
          "local reply document");
  }
  CHECK(sMqttLifecycle.requestStop(), "stop the open gate");
  CHECK(mainLoopTeardown(), "teardown after the test");
}

// The routed (mesh) reply keeps its own keys and the failure default.
static void test_routed_failure_document() {
  reset();
  openGate();
  MqttCommandReply* reply = admittedReply("admin", "x", "room");
  mqttCommandCompleted(false, "", reply);
  clientLoopPass();
  CHECK(delivered.size() == 1, "routed reply delivered");
  if (!delivered.empty()) {
    CHECK(delivered[0].payload ==
              "{\"ok\":false,\"routed\":\"room\","
              "\"error\":\"Command execution failed\"}",
          "routed failure document");
  }
  CHECK(sMqttLifecycle.usesInFlight() == 0, "completion releases admission");
  CHECK(sMqttLifecycle.requestStop(), "stop the open gate");
  CHECK(mainLoopTeardown(), "teardown after the test");
}

int main() {
  test_reply_survives_stop_requested_by_the_command();
  test_reply_queued_when_no_stop_pending();
  test_routed_failure_document();
  if (failures != 0) {
    std::fprintf(stderr, "MQTT command reply: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("MQTT command reply tests passed\n");
  return 0;
}
