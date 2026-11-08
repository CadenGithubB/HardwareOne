#!/usr/bin/env python3
"""No MQTT log line prints a password or a raw inbound payload.

An inbound command payload is the JSON envelope {"user","pass","cmd"}, so any
log call in System_MQTT.cpp that takes the payload text (event->data, the
handler's data/payload/dataStr) prints the device password. Every log call
is checked, debug-flag-gated ones included: an admin turns debugmqttpubsub
on exactly while troubleshooting, and the debug sinks reach serial, the web
log and log files. Lengths (data_len, payload.length()) are allowed.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
LOG_CALL = re.compile(r'\b(BROADCAST_PRINTF|broadcastOutput|logSystemEvent|ESP_LOG[EWIDV]|'
                      r'(?:DEBUG|INFO|WARN|ERROR)_\w+F|Serial\.printf?)\s*\(')
# Secret-bearing values: raw payload text, the parsed password, the broker
# password setting and the routed line that carries the target credentials.
SECRET = re.compile(r'event->data\b|\bdata\b|\bdataStr\b|\bpayload\b(?!\.length\(\))|'
                    r'\bpassword\b|\bmqttPassword\b|"pass"|\brouted(Line)?\b')


def calls(text: str):
    for match in LOG_CALL.finditer(text):
        i, depth = match.end(), 1
        while depth and i < len(text):
            ch = text[i]
            if ch == '"':
                i += 1
                while text[i] != '"':
                    i += 2 if text[i] == '\\' else 1
            elif ch == '(':
                depth += 1
            elif ch == ')':
                depth -= 1
            i += 1
        yield match.start(), text[match.start():i]


def without_strings(code: str) -> str:
    return re.sub(r'"(?:\\.|[^"\\])*"', '""', code)


def main() -> None:
    name = 'System_MQTT.cpp'
    text = (ROOT / name).read_text()
    leaks, seen = [], 0
    for pos, call in calls(text):
        seen += 1
        # Format text is checked separately; arguments are what get printed.
        args = without_strings(call[call.index('(') + 1:])
        if SECRET.search(args):
            leaks.append(f'{name}:{text.count(chr(10), 0, pos) + 1}: {" ".join(call.split())[:120]}')
    assert seen > 30, f'log-call scan matched only {seen} calls; the pattern is stale'
    assert not leaks, 'MQTT log lines print a payload or password:\n' + '\n'.join(leaks)

    # The receive trace names the topic and the size, never the payload.
    assert 'DEBUG_MQTT_PUBSUBF("Received: topic=%.*s data=%d bytes"' in text
    # The command path logs the redacted command, not the envelope.
    assert 'redactCmdForAudit(String(command))' in text
    print(f'MQTT log privacy: {seen} log calls in {name} print no payload or password')


if __name__ == '__main__':
    main()
