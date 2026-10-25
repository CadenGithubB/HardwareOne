#!/usr/bin/env python3
"""No always-on log line in the Bluetooth code prints a device address.

The always-on outputs (BROADCAST_PRINTF, broadcastOutput, ERROR_/WARN_/INFO_
macros at the default log level, logSystemEvent and ESP_LOGE/W/I) reach
serial, the web log, files and the lens. A saved or live Bluetooth, Wi-Fi or
ESP-NOW address must never be one of their arguments; debug-flag-gated lines
(DEBUG_*F, off by default) are listed in docs, not checked here. The command
audit masks every address (maskMacTokens, run by test_llm_http_settings), and
the specific lines that used to leak are pinned below.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
FILES = ['G2_Glasses.cpp', 'G2_Ring.cpp', 'G2_HijackCmd.cpp', 'G2_Page_ESPNow.cpp', 'G2_Page_Network.cpp',
         'Bluetooth.cpp', 'HAL_Bluetooth.cpp', 'HAL_Radio.cpp', 'System_RadioCompanion.cpp',
         'OLED_Mode_Bluetooth.cpp'] + sorted(p.name for p in ROOT.glob('BLE_*.cpp'))
ALWAYS_ON = re.compile(r'\b(BROADCAST_PRINTF|ERROR_\w+F|WARN_\w+F|INFO_\w+F|logSystemEvent|ESP_LOG[EWI]|broadcastOutput)\s*\(')
# Address-bearing values. A saved target's mac1/mac2 may be tested ([0]) but not printed.
ADDRESS = re.compile(r'savedMac|deviceAddress|gRingDeviceAddress|\.mac[12]\b(?!\[0\])|getAddress\(|remote_bda|'
                     r'macStr|\bbda\b|toString\(\)|\bmac\b(?!\[0\])')


def calls(text: str):
    for match in ALWAYS_ON.finditer(text):
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
    leaks = []
    for name in FILES:
        text = (ROOT / name).read_text()
        for pos, call in calls(text):
            if ADDRESS.search(without_strings(call)):
                leaks.append(f'{name}:{text.count(chr(10), 0, pos) + 1}: {" ".join(call.split())[:120]}')
    assert not leaks, 'always-on log lines print an address:\n' + '\n'.join(leaks)

    glasses = (ROOT / 'G2_Glasses.cpp').read_text()
    assert 'MAC %s, attempt' not in glasses, 'the repair line names the temple address'
    tap = glasses[glasses.index('char lbl[kTapLabelWidth];'):]
    tap = tap[:tap.index('Text-entry overlay')]
    assert 'maskMacTokens(String(lbl))' in tap, 'lens tap labels are masked before they are logged'
    ring = (ROOT / 'G2_Ring.cpp').read_text()
    assert 'useSavedMac ? "saved" : "scan"' in ring and 'ringscan: found %s @' not in ring
    espnow = (ROOT / 'G2_Page_ESPNow.cpp').read_text()
    assert 'forget peer: %s", who' in espnow and 'line + 13' not in espnow
    utils = (ROOT / 'System_Utils.cpp').read_text()
    redactor = utils[utils.index('String redactCmdForAudit(const String& argsInput,\n'):]
    assert 'String c = maskMacTokens(argsInput);' in redactor[:600], 'the audit masks addresses before its rules'
    print(f'BLE log privacy: no always-on line in {len(FILES)} files prints an address')


if __name__ == '__main__':
    main()
