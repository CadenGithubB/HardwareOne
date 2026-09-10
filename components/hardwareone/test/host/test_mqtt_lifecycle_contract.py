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

# Acquire admission immediately after identifying the command topic, before
# parse/auth/queue publication. The function-scope RAII object survives through
# every ordinary, mesh, error, and success response publication.
handle = function_body(mqtt, "static void handleMQTTCommand(")
topic_match = handle.index("strncmp(topic, commandTopic.c_str(), topicLen)")
admission = handle.index("MqttClientUse commandUse;")
payload_parse = handle.index("deserializeJson(")
queue_submit = handle.index("submitAndExecuteSync(")
final_publish = handle.rindex("esp_mqtt_client_publish(")
assert topic_match < admission < payload_parse < queue_submit < final_publish
assert brace_depth_at(handle, admission) == 1
assert brace_depth_at(handle, final_publish) == 1
assert "MQTT client is stopping" in handle[admission:payload_parse]
assert "releaseUse" not in handle
admission_destructor = function_body(
    mqtt, "~MqttClientUse()"
)
assert "sMqttLifecycle.releaseUse()" in admission_destructor

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
