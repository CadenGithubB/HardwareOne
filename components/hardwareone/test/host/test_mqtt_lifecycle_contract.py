#!/usr/bin/env python3
"""Source-contract checks for MQTT command/lifecycle integration."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : pos + 1]
    raise AssertionError(f"unterminated function: {signature}")


def brace_depth_at(source: str, end: int) -> int:
    """Return C++ brace depth, ignoring comments and ordinary literals."""
    depth = 0
    pos = 0
    while pos < end:
        if source.startswith("//", pos):
            newline = source.find("\n", pos + 2)
            pos = end if newline < 0 else newline + 1
            continue
        if source.startswith("/*", pos):
            close = source.find("*/", pos + 2)
            assert close >= 0
            pos = close + 2
            continue
        if source[pos] in ('"', "'"):
            quote = source[pos]
            pos += 1
            while pos < end:
                if source[pos] == "\\":
                    pos += 2
                elif source[pos] == quote:
                    pos += 1
                    break
                else:
                    pos += 1
            continue
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
        pos += 1
    return depth


mqtt = (ROOT / "System_MQTT.cpp").read_text()
mqtt_header = (ROOT / "System_MQTT.h").read_text()
gate = (ROOT / "System_MQTTLifecycleGate.h").read_text()

assert '#include "System_MQTTLifecycleGate.h"' in mqtt
assert "static MqttLifecycleGate sMqttLifecycle;" in mqtt
assert "static std::atomic<bool> mqttClientRunning{false};" in mqtt
assert "static std::atomic<bool> mqttTofConnected{false};" in mqtt
assert "static std::atomic<bool> sMqttBeforeConnectSeen{false};" in mqtt
assert "bool stopMQTT();" in mqtt_header
assert "inline bool stopMQTT() { return false; }" in mqtt_header

# The production gate is one packed atomic word: admission increments it only
# while OPEN, while stop closes admission and requests teardown in the same CAS.
assert "std::atomic<uint32_t> word_{kClosed};" in gate
admit = function_body(gate, "bool tryAdmitUse()")
request_stop = function_body(gate, "bool requestStop()")
begin_stop = function_body(gate, "bool tryBeginStop()")
assert "current & kClosed" in admit
assert "compare_exchange_weak" in admit
assert "current + 1u" in admit
assert "current | kClosed | kStopRequested" in request_stop
assert "kClosed | kStopRequested" in begin_stop

handle = function_body(mqtt, "static void handleMQTTCommand(")
event_handler = function_body(mqtt, "static void mqtt_event_handler(")
assert "handleMQTTCommand(event->topic" in event_handler

# The esp-mqtt task holds the client's API lock while it dispatches an event,
# and esp_mqtt_client_publish() on the main loop takes the same lock. Nothing
# on the MQTT event path may wait for cmd_exec: commands are queued with
# submitCommandAsync and answered from the completion with the outbox enqueue.
assert "submitAndExecuteSync" not in mqtt, \
    "the MQTT event path must not wait for cmd_exec (submitAndExecuteSync)"
for body in (event_handler, handle):
    assert "xSemaphoreTake" not in body and "vTaskDelay" not in body

# Acquire admission immediately after identifying the command topic, before
# parse/auth/queue publication. The function-scope RAII object survives through
# every response published on the event path; a queued command moves it into
# the cmd_exec completion instead.
topic_match = handle.index("strncmp(topic, commandTopic.c_str(), topicLen)")
admission = handle.index("MqttClientUse commandUse;")
payload_parse = handle.index("deserializeJson(")
queue_submit = handle.rindex("submitMqttCommand(std::move(commandUse)")
assert topic_match < admission < payload_parse < queue_submit
assert brace_depth_at(handle, admission) == 1
assert brace_depth_at(handle, queue_submit) == 1
assert "MQTT client is stopping" in handle[admission:payload_parse]
assert "releaseUse" not in handle
assert "class MqttClientUse final : public MqttAdmission" in mqtt
assert "MqttClientUse() : MqttAdmission(sMqttLifecycle) {}" in mqtt
admission_header = (ROOT / "System_MQTTAdmission.h").read_text()
assert "gate.tryAdmitUse() ? &gate : nullptr" in admission_header
assert "if (gate) gate->releaseUse();" in function_body(
    admission_header, "void release()")
assert "release();" in function_body(admission_header, "~MqttAdmission()")

submit = function_body(mqtt, "static void submitMqttCommand(")
completion = function_body(mqtt, "static void mqttCommandCompleted(")
reply_writer = function_body(mqtt, "static void enqueueMqttCommandReply(")
assert "esp_mqtt_client_enqueue(mqttClient" in reply_writer
# esp-mqtt deletes its outbox when the client stops, and mqttTick tears the
# client down as soon as the reply releases its admission. A reply written
# while a stop is pending (closemqtt sent over MQTT) is published directly
# instead of queued; every other reply goes through the outbox.
stop_branch = function_body(reply_writer, "if (sMqttLifecycle.stopPending())")
assert "esp_mqtt_client_publish(mqttClient, reply.responseTopic" in stop_branch
assert "return;" in stop_branch
assert reply_writer.count("esp_mqtt_client_publish") == 1
assert (reply_writer.index("if (sMqttLifecycle.stopPending())")
        < reply_writer.index("esp_mqtt_client_enqueue(mqttClient"))
assert "esp_mqtt_client_publish" not in completion
assert "MqttAdmission use;" in function_body(mqtt, "struct MqttCommandReply")
# The admission moves into the reply only after allocation, and a refused
# submission answers before the reply (and the admission) is destroyed.
alloc = submit.index("ps_alloc(sizeof(MqttCommandReply)")
moved = submit.index("reply->use = std::move(use);")
queued = submit.index("submitCommandAsync(queued, mqttCommandCompleted, reply)")
assert alloc < moved < queued
refused = function_body(submit, "if (!submitCommandAsync(")
assert refused.index("enqueueMqttCommandReply(*reply, false") < refused.index(
    "reply->~MqttCommandReply();")
# The completion answers first and only then releases the admission.
assert (completion.index("enqueueMqttCommandReply(*reply, ok, result)")
        < completion.index("reply->~MqttCommandReply();")
        < completion.index("free(reply);"))

# Public stop is request-only. The only driver stop lives in the private helper
# that mqttTick claims after responses reach zero, before normal publication.
public_stop = function_body(mqtt, "bool stopMQTT()")
stop_now = function_body(mqtt, "static bool stopMQTTNow()")
tick = function_body(mqtt, "void mqttTick()")
assert "requestStop()" in public_stop
assert "esp_mqtt_client_stop" not in public_stop
assert "esp_mqtt_client_destroy" not in public_stop
assert "esp_mqtt_client_stop(mqttClient)" in stop_now
assert "if (stopErr != ESP_OK)" in stop_now
stop_error = function_body(stop_now, "if (stopErr != ESP_OK)")
assert "sMqttBeforeConnectSeen.load(std::memory_order_acquire)" in stop_error
assert "failuresBeforeConnect > 0" in stop_error
assert "++failuresBeforeConnect" in stop_error
assert "esp_mqtt_client_disconnect(mqttClient)" in stop_error
assert "return false;" in stop_error
assert mqtt.count("esp_mqtt_client_stop(mqttClient)") == 1
assert mqtt.count("stopMQTTNow()") == 2  # definition plus main-loop caller
pending = tick.index("sMqttLifecycle.stopPending()")
claim = tick.index("sMqttLifecycle.tryBeginStop()", pending)
teardown = tick.index("stopMQTTNow()", claim)
normal_publish = tick.index("publishMQTTSensorData()")
assert pending < claim < teardown < normal_publish
pending_block = function_body(tick, "if (sMqttLifecycle.stopPending())")
pending_lines = [line.strip() for line in pending_block.splitlines() if line.strip()]
assert pending_lines[-2:] == ["return;", "}"]
finish = tick.index("sMqttLifecycle.finishStop()", teardown)
retry = tick.index("sMqttLifecycle.retryStop()", finish)
assert teardown < finish < retry < normal_publish
assert "stopRetryScheduled &&" in tick
assert "(int32_t)(now - stopRetryAfterMs) < 0" in tick

# The historically public sensor publisher also owns a gate use, so a future
# non-main caller cannot race main-loop teardown through the exposed API.
sensor_publish = function_body(mqtt, "void publishMQTTSensorData()")
assert sensor_publish.index("MqttClientUse publishUse;") < sensor_publish.index(
    "mqttTofConnected.load(std::memory_order_acquire)"
)
assert "if (!publishUse.admitted()) return;" in sensor_publish

# Startup owns the CLOSED/STARTING transition, checks IDF errors, and opens the
# gate only after the client task and running-state publication succeed.
start = function_body(mqtt, "bool startMQTT()")
start_claim = start.index("sMqttLifecycle.beginStart()")
start_attempt = start.index("MqttStartAttempt startAttempt;")
driver_start = start.index("esp_mqtt_client_start(mqttClient)")
worker_marker_reset = start.index("sMqttBeforeConnectSeen.store(false", start_claim)
start_error = start.index("if (startErr != ESP_OK)", driver_start)
running_publish = start.index("mqttClientRunning.store(true", start_error)
gate_open = start.index("startAttempt.finishStarted()", running_publish)
first_fallible_check = start.index("if (!gSettings.mqttEnabled)")
assert start_claim < start_attempt < first_fallible_check
assert first_fallible_check < worker_marker_reset < driver_start
assert driver_start < start_error < running_publish < gate_open
start_attempt_destructor = function_body(mqtt, "~MqttStartAttempt()")
assert "sMqttLifecycle.finishStart(false)" in start_attempt_destructor
assert "case MQTT_EVENT_BEFORE_CONNECT:" in mqtt
assert "sMqttBeforeConnectSeen.store(true" in mqtt

# Both current shutdown routes go through the central request-only API.
close_command = function_body(mqtt, "const char* cmd_closemqtt(")
enabled_command = function_body(mqtt, "const char* cmd_mqttclientenabled(")
not_running_branch = function_body(close_command, "if (!stopMQTT())")
assert 'return "[MQTT] Not running";' in not_running_branch
assert close_command.index(not_running_branch) < close_command.index(
    'return "[MQTT] Client stop scheduled";'
)
disable_branch = function_body(enabled_command, "if (!enable)")
assert "stopMQTT()" in disable_branch
assert "client stop scheduled" in disable_branch
assert mqtt.count(
    '{ "closemqtt", "Stop MQTT client", true, cmd_closemqtt }'
) == 1
assert mqtt.count(
    '{ "mqttclientenabled", "Enable/disable MQTT [0|1]", true, '
    'cmd_mqttclientenabled, "Usage: mqttclientenabled [0|1]" }'
) == 1

print("MQTT lifecycle source guards passed")
