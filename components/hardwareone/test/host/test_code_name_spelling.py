#!/usr/bin/env python3
"""An acronym inside a code name is written as a word.

The rule (CONTRIBUTING.md, "Names"): in the name of a function, variable,
type or constant, an acronym is spelled like any other word, with one capital
at most: `gBleState`, `isImuConnected()`, `OledMode`, `sendHttpRequest()`,
`gI2cBusRunning`, `psramSizeMb`. The same holds for the three names that have
their own mixed spelling: `Wifi` (not `WiFi`), `EspNow` (not `ESPNow` or
`Espnow`) and `Tof` (not `ToF`).

What the rule does not touch:

- names in capitals only (`ENABLE_BLE`, `DEBUG_I2C`): macros and build flags;
- file names (`BLE_Client.cpp`, `OLED_Display.h`);
- text: command names, setting keys, JSON keys, log and screen text;
- part numbers (`gBNO055`, `VL53L4CX_StopMeasurement`);
- names a library defines (`BLEDevice`, `WiFi`, `cJSON_Delete`). They are
  listed in LIBRARY_NAMES below. A new library name with a listed acronym is
  added there; the test asks for it. A listed name nobody uses any more is
  removed, so the list stays exact;
- KEPT_NAMES: own names that a macro turns into text the user sees.

ACRONYMS is the list of acronyms the test knows. A new acronym is added there
when it first appears in a name.

Reads the working tree. --rewrite renames every name that breaks the rule, in
code and in comments, and in the tests and tools that mention those names. It
is how a branch that was cut before the rule existed is brought to it.
--component points the test at another component directory.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
SOURCE_SUFFIXES = (".cpp", ".h", ".c", ".hpp", ".inc")
SKIPPED_DIRECTORIES = ("test/", "third_party/", "tools/", "managed_components/", "build/")
# Tests and tools mention code names; --rewrite keeps them in step.
FOLLOWER_DIRECTORIES = ("test", "tools")
FOLLOWER_TEXT_SUFFIXES = (".py", ".txt", ".cmake", ".sh", ".js", ".mjs", ".json", ".md")

ACRONYMS = {
    "ADC": "Adc", "AE": "Ae", "AEAD": "Aead", "AFE": "Afe", "AI": "Ai", "API": "Api", "APDS": "Apds",
    "BLE": "Ble", "BSSID": "Bssid", "CA": "Ca", "CID": "Cid", "CLI": "Cli", "CPU": "Cpu", "CRC16": "Crc16",
    "CSS": "Css", "CSV": "Csv", "EI": "Ei", "ELSE": "Else", "ELSEIF": "ElseIf", "ESPNOW": "EspNow",
    "ESPSR": "EspSr", "EWMA": "Ewma", "FM": "Fm", "FPS": "Fps", "FS": "Fs", "FTS": "Fts", "GPIO": "Gpio",
    "GPS": "Gps", "GPT2": "Gpt2", "HTTP": "Http", "HW": "Hw", "I2C": "I2c", "I2S": "I2s", "ID": "Id",
    "IF": "If", "IMU": "Imu", "IO": "Io", "IP": "Ip", "ISO8601": "Iso8601", "JS": "Js", "JSON": "Json",
    "KB": "Kb", "LED": "Led", "LLM": "Llm", "MAC": "Mac", "MB": "Mb", "MM": "Mm", "MN": "Mn",
    "MQTT": "Mqtt", "MTU": "Mtu", "NACK": "Nack", "NL": "Nl", "NMEA": "Nmea", "NTP": "Ntp", "OLED": "Oled",
    "PCM": "Pcm", "PS": "Ps", "PSRAM": "Psram", "PWM": "Pwm", "RGB": "Rgb", "RSSI": "Rssi", "RTC": "Rtc",
    "SD": "Sd", "SID": "Sid", "SPI": "Spi", "SR": "Sr", "SSID": "Ssid", "STT": "Stt", "SW": "Sw",
    "TCB": "Tcb", "TE": "Te", "TL": "Tl", "TLS": "Tls", "TOF": "Tof", "TTL": "Ttl", "UDP": "Udp",
    "UI": "Ui", "USB": "Usb", "UUID": "Uuid", "VFS": "Vfs", "WB": "Wb", "WIFI": "Wifi",
}
MIXED_SPELLINGS = [
    (re.compile(r"ESPNOW|ESPNow|Espnow"), "EspNow"),
    (re.compile(r"WIFI|WiFi"), "Wifi"),
    (re.compile(r"TOF|ToF"), "Tof"),
]

# Own names that a macro turns into text (the debug flag table prints its
# settings field in the command reply).
KEPT_NAMES = {"debugI2C", "debugI2CAutoStart", "debugI2CBus", "debugI2CDiscovery"}

# Names that libraries define. One per line, sorted.
LIBRARY_NAMES = set("""
Adafruit_GPS
Adafruit_PWMServoDriver
BLEAddress
BLEAdvertisedDevice
BLEAdvertisedDeviceCallbacks
BLEAdvertisementData
BLEAdvertising
BLECharacteristic
BLECharacteristicCallbacks
BLEClient
BLEClientCallbacks
BLEClientConnectOutcome
BLEClientConnectStage
BLEClientUnregisterState
BLEDevice
BLEDeviceDeinitResult
BLERemoteCharacteristic
BLERemoteDescriptor
BLERemoteNotifyResult
BLERemoteService
BLEScan
BLEScanResults
BLEServer
BLEServerCallbacks
BLEService
FSImplPtr
IPAddress
LittleFS
SPISettings
SWReset
VFSImpl
WiFi
WiFiClass
WiFiGenericClass
WiFiUDP
addServiceUUID
cJSON
cJSON_AddBoolToObject
cJSON_AddNumberToObject
cJSON_AddStringToObject
cJSON_CreateObject
cJSON_Delete
cJSON_GetArraySize
cJSON_GetObjectItemCaseSensitive
cJSON_IsNumber
cJSON_IsObject
cJSON_IsString
cJSON_ParseWithOpts
cJSON_PrintPreallocated
dnsIP
gatewayIP
getMTU
getRSSI
hostedDeinitWiFi
hostedInitWiFi
hostedIsBLEActive
hostedReleaseBLE
lastNMEA
localIP
setGPIOInterrupts
setMTU
setPWM
setPWMFreq
xPortGetCoreID
xTaskGetCoreID
""".split())

TOKEN = re.compile(r'''
    (?P<include>^[ \t]*\#[ \t]*include[^\n]*)
  | (?P<line_comment>//[^\n]*)
  | (?P<block_comment>/\*.*?\*/)
  | (?P<raw>(?<![A-Za-z0-9_])(?:u8|u|U|L)?R"(?P<delimiter>[^()\\\s]{0,16})\(.*?\)(?P=delimiter)")
  | (?P<string>"(?:\\.|[^"\\\n])*")
  | (?P<character>'(?:\\.|[^'\\\n])*')
  | (?P<number>(?<![A-Za-z0-9_])[0-9][A-Za-z0-9_.]*)
  | (?P<name>[A-Za-z_][A-Za-z0-9_]*)
''', re.S | re.X | re.M)
NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
CAPITAL_RUN = re.compile(r"[A-Z][A-Z0-9]+")


def capital_runs(name: str):
    """Runs of capitals in a name, as (start, end, text)."""
    for match in CAPITAL_RUN.finditer(name):
        start, end, text = match.start(), match.end(), match.group(0)
        after = name[end:end + 1]
        if after.islower() and text[-1].isalpha():
            plural = after == "s" and not name[end + 1:end + 2].islower() and text in ACRONYMS
            if not plural:
                end -= 1                     # the last capital starts the next word
                text = text[:-1]
        if sum(c.isalpha() for c in text) >= 2:
            yield start, end, text


def split_run(text: str):
    """A capital run as a list of known acronyms, or None (a part number, for example)."""
    if text in ACRONYMS:
        return [text]
    for cut in range(len(text) - 1, 1, -1):
        if text[:cut] in ACRONYMS:
            rest = split_run(text[cut:])
            if rest:
                return [text[:cut]] + rest
    return None


def as_words(name: str) -> str:
    """The name with every known acronym written as a word."""
    if not re.search(r"[a-z]", name):
        return name                          # capitals only: a macro or a build flag
    for pattern, word in MIXED_SPELLINGS:
        name = pattern.sub(
            lambda m: m.group(0) if m.string[m.end():m.end() + 1].islower() else word, name)
    pieces, last = [], 0
    for start, end, text in capital_runs(name):
        parts = split_run(text)
        if parts:
            pieces.append(name[last:start] + "".join(ACRONYMS[part] for part in parts))
            last = end
    return "".join(pieces) + name[last:]


def wanted(name: str) -> str:
    return name if name in LIBRARY_NAMES or name in KEPT_NAMES else as_words(name)


def own_roots(component: Path):
    repo = component.parents[1]
    extra = [repo / "main", repo / "components" / "hw1_ota_protocol", repo / "updater"]
    return [component] + [root for root in extra if root.is_dir()]


def sources(component: Path):
    for root in own_roots(component):
        for path in sorted(root.rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            relative = path.relative_to(root).as_posix()
            if any(("/" + relative).find("/" + skipped) >= 0 for skipped in SKIPPED_DIRECTORIES):
                continue
            yield path


def followers(component: Path):
    repo = component.parents[1]
    for root in [component / d for d in FOLLOWER_DIRECTORIES] + [repo / "tools"]:
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if path.is_file() and "__pycache__" not in path.parts and path != Path(__file__).resolve():
                if path.suffix in SOURCE_SUFFIXES or path.suffix in FOLLOWER_TEXT_SUFFIXES:
                    yield path


def read(path: Path) -> str:
    return path.read_bytes().decode("utf-8", errors="surrogateescape")


def write(path: Path, text: str) -> None:
    path.write_bytes(text.encode("utf-8", errors="surrogateescape"))


def code_names(text: str):
    """Names used as code: (offset, name). Comments, strings and include lines are skipped."""
    for match in TOKEN.finditer(text):
        if match.lastgroup == "name":
            yield match.start(), match.group(0)


def rewrite_source(text: str, renames: dict) -> str:
    out, last = [], 0
    for match in TOKEN.finditer(text):
        kind = match.lastgroup
        if kind == "name":
            new = renames.get(match.group(0))
        elif kind in ("line_comment", "block_comment"):
            new = NAME.sub(lambda m: renames.get(m.group(0), m.group(0)), match.group(0))
        else:
            continue
        if new and new != match.group(0):
            out.append(text[last:match.start()] + new)
            last = match.end()
    return "".join(out) + text[last:]


def measure(component: Path):
    """(wrong uses as {name: [(file, line)]}, set of all code names in use)."""
    wrong, used = {}, set()
    for path in sources(component):
        text = read(path)
        for offset, name in code_names(text):
            used.add(name)
            if wanted(name) != name:
                line = text.count("\n", 0, offset) + 1
                wrong.setdefault(name, []).append((path, line))
    return wrong, used


def rewrite(component: Path) -> int:
    wrong, _ = measure(component)
    renames = {name: as_words(name) for name in wrong}
    if not renames:
        print("nothing to rename")
        return 0
    changed = 0
    for path in list(sources(component)) + [p for p in followers(component) if p.suffix in SOURCE_SUFFIXES]:
        text = read(path)
        new = rewrite_source(text, renames)
        if new != text:
            write(path, new)
            changed += 1
    whole_name = re.compile(
        r"(?<![A-Za-z0-9_])(" + "|".join(sorted(map(re.escape, renames), key=len, reverse=True))
        + r")(?![A-Za-z0-9_])")
    for path in followers(component):
        if path.suffix in SOURCE_SUFFIXES:
            continue
        text = read(path)
        new = whole_name.sub(lambda m: renames[m.group(1)], text)
        if new != text:
            write(path, new)
            changed += 1
    print(f"renamed {len(renames)} names in {changed} files")
    for old in sorted(renames):
        print(f"  {old} -> {renames[old]}")
    return 0


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", type=Path, default=COMPONENT)
    parser.add_argument("--rewrite", action="store_true")
    parser.add_argument("--print-library-names", action="store_true",
                        help="print the listed names that are in use, for the LIBRARY_NAMES block")
    args = parser.parse_args()
    component = args.component.resolve()

    if args.rewrite:
        sys.exit(rewrite(component))

    wrong, used = measure(component)
    if args.print_library_names:
        print("\n".join(sorted(name for name in LIBRARY_NAMES if name in used)))
        return

    failures = []
    repo = component.parents[1]
    for name in sorted(wrong):
        path, line = wrong[name][0]
        more = f" (+{len(wrong[name]) - 1} more)" if len(wrong[name]) > 1 else ""
        failures.append(
            f"{path.relative_to(repo).as_posix()}:{line}: `{name}` should be `{as_words(name)}`{more}")
    stale = sorted(name for name in LIBRARY_NAMES | KEPT_NAMES if name not in used)
    for name in stale:
        failures.append(f"`{name}` is listed in this test but no code uses it: remove it from the list")
    conflicts = sorted(name for name in LIBRARY_NAMES | KEPT_NAMES if as_words(name) == name)
    for name in conflicts:
        failures.append(f"`{name}` is listed in this test but already follows the rule: remove it")

    if failures:
        print("\n".join(failures))
        print(
            "\nAn acronym inside a code name is written as a word (CONTRIBUTING.md, \"Names\").\n"
            "If the name comes from a library, add it to LIBRARY_NAMES in this test.\n"
            "To rename everything at once: python3 " + Path(__file__).name + " --rewrite")
        sys.exit(1)
    print(f"code name spelling: {len(used)} names, {len(LIBRARY_NAMES)} library names, "
          f"{len(KEPT_NAMES)} kept names")


if __name__ == "__main__":
    main()
