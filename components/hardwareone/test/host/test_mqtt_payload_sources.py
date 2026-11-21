#!/usr/bin/env python3
"""Source contract for the MQTT state and Home Assistant discovery documents.

test_mqtt_payload_core.cpp builds the documents from fixture texts of the
sensor builders and resolves every Home Assistant template in them. This test
ties those fixtures to the firmware: each fixture ("// fixture: <file>
<definition>") may only have keys the named builder's format strings write
(with HW1_SK_* expanded from System_SensorJsonKeys.h, plus the shared
envelope valid/connected/ts when the builder opens with sensorEnvelopeBegin),
and misses at most one key a builder writes on another branch.
A builder that renames a key then fails here instead of leaving an HA entity
that never gets a value (U6, surface review 2026-11-20).

It also pins System_MQTT.cpp to the core:
  - no "ESP32-S3"; the HA model is BOARD_NAME and sw_version is
    SelfDevice::firmwareVersion();
  - thermal goes through thermalBuildSummaryJson (the frame never fit);
  - the state document is built by the core (ArduinoJson), with no snprintf
    and no hand-written JSON in publishMqttSensorData, checked with
    serializeBounded before it is published, and MqttClientUse stays first;
  - the discovery documents are built by the core, and MqttJsonWriter is gone;
  - every sensor builder the publish calls is one ESP-NOW and `sensors json`
    use, behind the same #if as before.
And System_MQTTPayloadCore.h stays pure (no Arduino, no ESP-IDF, no #if on
build features).
"""
from __future__ import annotations

import json
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
sys.path.insert(0, str(HERE))
from test_web_batch_handlers import extract_block  # noqa: E402
from test_sensor_json_keys import strip_comments  # noqa: E402

ENVELOPE = {'valid', 'connected', 'ts'}

failures: list[str] = []


def require(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


def owner_keys() -> dict[str, str]:
    text = (COMPONENT / 'System_SensorJsonKeys.h').read_text()
    return dict(re.findall(r'^#define (HW1_SK_\w+)\s+"([^"]*)"\s*$', text, re.M))


def builder_keys(file: str, marker: str, keys: dict[str, str]) -> set[str]:
    block = strip_comments(extract_block((COMPONENT / file).read_text(), marker))
    expanded = re.sub(r'HW1_SK_Q\((HW1_SK_\w+)\)',
                      lambda m: '\\"' + keys[m.group(1)] + '\\":', block)
    found = set(re.findall(r'\\"(\w+)\\":', expanded))
    if 'sensorEnvelopeBegin(' in block:
        found |= ENVELOPE
    return found


def json_keys(value) -> set[str]:
    out: set[str] = set()
    if isinstance(value, dict):
        for key, item in value.items():
            out.add(key)
            out |= json_keys(item)
    elif isinstance(value, list):
        for item in value:
            out |= json_keys(item)
    return out


def check_fixtures(keys: dict[str, str]) -> None:
    test = (HERE / 'test_mqtt_payload_core.cpp').read_text()
    fixtures = re.findall(
        r'^// fixture: (\S+) (.+?)\nstatic const char (\w+)\[\] = R"json\((.*?)\)json";',
        test, re.M)
    require(len(fixtures) >= 11, f'expected the 11 builder fixtures, found {len(fixtures)}')
    for file, marker, name, text in fixtures:
        try:
            fixture = json_keys(json.loads(text))
        except json.JSONDecodeError as error:
            failures.append(f'{name}: not JSON ({error})')
            continue
        sent = builder_keys(file, marker, keys)
        # Every key of the fixture is one the builder writes. (A builder may
        # write more on another branch, e.g. ToF's lock-timeout
        # "total_objects".)
        require(fixture <= sent,
                f'{name}: fixture has keys {file} {marker.strip("(")} never writes: '
                f'{sorted(fixture - sent)}')
        # And the fixture is the builder's ordinary reading, not a subset.
        require(len(sent - fixture) <= 1,
                f'{name}: fixture misses builder keys {sorted(sent - fixture)}')


def check_mqtt_source() -> None:
    mqtt = (COMPONENT / 'System_MQTT.cpp').read_text()
    code = strip_comments(mqtt)
    require('ESP32-S3' not in code, 'System_MQTT.cpp still names the model "ESP32-S3"')
    require('device.model = BOARD_NAME;' in code, 'the HA model is BOARD_NAME')
    require('device.swVersion = SelfDevice::firmwareVersion();' in code,
            'the HA sw_version is SelfDevice::firmwareVersion()')
    require('class MqttJsonWriter' not in code, 'MqttJsonWriter is gone (discovery is ArduinoJson)')
    require('#include "System_MQTTPayloadCore.h"' in mqtt, 'System_MQTT.cpp uses the payload core')

    state = strip_comments(extract_block(mqtt, 'void publishMqttSensorData('))
    require(state[state.index('{') + 1:].lstrip().startswith('MqttClientUse publishUse;'),
            'publishMqttSensorData takes its MqttClientUse first')
    require('snprintf' not in state and '\\"' not in state,
            'publishMqttSensorData writes no JSON by hand')
    require('mqttpayload::beginState(in, doc)' in state, 'the state document starts in the core')
    publish = state.find('esp_mqtt_client_publish(mqttClient, stateTopic')
    bounded = state.find('mqttpayload::serializeBounded(doc, jsonBuf, cap)')
    require(0 <= bounded < publish,
            'the state document is checked against its buffer before it is published')
    require('thermalBuildSummaryJson' in state and 'thermalBuildDataJson' not in code,
            'MQTT thermal uses the summary builder, never the 768-pixel frame')
    for builder, flag in [('thermalBuildSummaryJson', 'ENABLE_THERMAL_SENSOR'),
                          ('tofBuildDataJson', 'ENABLE_TOF_SENSOR'),
                          ('imuBuildDataJson', 'ENABLE_IMU_SENSOR'),
                          ('presenceBuildDataJson', 'ENABLE_PRESENCE_SENSOR'),
                          ('gpsBuildDataJson', 'ENABLE_GPS_SENSOR'),
                          ('apdsBuildDataJson', 'ENABLE_APDS_SENSOR'),
                          ('rtcBuildDataJson', 'ENABLE_RTC_SENSOR'),
                          ('gamepadBuildDataJson', 'ENABLE_GAMEPAD_SENSOR'),
                          ('anoEncoderBuildDataJson', 'ENABLE_ANO_ENCODER'),
                          ('gpioEncoderBuildDataJson', 'ENABLE_GPIO_ENCODER')]:
        at = state.find(builder)
        require(at >= 0, f'publishMqttSensorData does not call {builder}')
        if at >= 0:
            guards = re.findall(r'^\s*#\s*(?:if|elif)\b.*$', state[:at], re.M)
            require(bool(guards) and flag in guards[-1], f'{builder} is not behind #if/#elif {flag}')
    for getter in ('inputGetX', 'inputGetButtonsLogical', 'rtcGetYear', 'apdsGetColorR',
                   'presenceCacheSnapshot', 'gpsCacheSnapshot'):
        require(getter not in state, f'publishMqttSensorData reads {getter}; use the builder')

    for name in ('static void publishDiscoveryConfig(', 'static void publishPeerDiscoveryConfig('):
        body = strip_comments(extract_block(mqtt, name))
        require('mqttpayload::buildDiscovery(' in body and 'mqttpayload::serializeBounded(' in body,
                f'{name.split("(")[0].split()[-1]} builds through the core')
        require('snprintf' not in body and '\\"' not in body,
                f'{name.split("(")[0].split()[-1]} writes no JSON by hand')
    peer = strip_comments(extract_block(mqtt, 'static void publishPeerDiscoveryConfig('))
    require('mqttpayload::peerDisplayName(' in peer,
            'an unnamed peer gets the unknown-peer label, not its full address')


def check_core() -> None:
    core = (COMPONENT / 'System_MQTTPayloadCore.h').read_text()
    includes = re.findall(r'^#include\s+(\S+)', core, re.M)
    require(includes == ['<ArduinoJson.h>', '<stddef.h>', '<stdint.h>', '<stdio.h>', '<string.h>',
                         '"System_DeviceName.h"', '"System_SensorJsonKeys.h"'],
            f'System_MQTTPayloadCore.h includes only ArduinoJson, C headers and the two pure '
            f'owners; got {includes}')
    require(not re.search(r'^\s*#\s*if', core, re.M), 'System_MQTTPayloadCore.h has no #if')
    code = strip_comments(core)
    require(not re.search(r'^\s*static\s+(?!constexpr|inline|const)', code, re.M),
            'System_MQTTPayloadCore.h keeps no mutable statics')


def main() -> int:
    keys = owner_keys()
    check_fixtures(keys)
    check_mqtt_source()
    check_core()
    if failures:
        for failure in failures:
            print('FAIL:', failure)
        return 1
    print('MQTT payload sources: fixtures match the builders; System_MQTT.cpp uses the core')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
