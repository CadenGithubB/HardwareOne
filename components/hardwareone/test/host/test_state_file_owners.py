#!/usr/bin/env python3
"""The state the device stores has one owner per name (2026-11-14 sweep, step 4).

Three sets of names that the code writing a stored file and the code reading,
backing up, restoring, wiping or guarding it used to spell each on its own:

1. The eight /system state paths: settings.json, debug.json, users.json,
   pending_users.json, the user_settings folder, boot_anchors.json,
   automations.json and espnow/devices.json (census-duplicates-05). Owner:
   System_StatePaths.h (HW1_*_PATH, HW1_USER_SETTINGS_DIR). The modules keep
   their own names (SETTINGS_JSON_FILE, USERS_JSON_FILE, DEBUG_JSON_FILE,
   PENDING_USERS_FILE, ...) as aliases of the owner; the path permission
   table, the backup/restore tool and factory reset use the owner directly.
2. The phone-link credential keys in settings.json (census-duplicates-13):
   phonePipeSecret/phonePipeSecretGenerated and the peers.phone enrolment
   keys. Owner: BLE_PhonePipeKeys.h (hw1pipekeys), whose lists factory reset
   removes, so a key the writer adds cannot survive a factory reset.
3. The network.wifi section and keys the backup restore edits by hand
   (census-duplicates-14). Owner: hw1wifijson in System_WiFiNetworks.h, used
   by the wifi settings rows and module section (System_WiFi.cpp), the saved
   network array (System_WiFiNetworks.cpp) and the restore.

Source contract: each value appears as a literal only in its owner (firmware
sources under components/hardwareone outside test/, comments ignored), and
every former site uses the owner's name.

No behaviour change: the owners are compiled on the host and compared with
the values the old code spelled out; the permission table is compiled from
production source and its state-file rows compared with the old rows
(position, path, user/admin/system permissions, exact match, sensitive-name
exemption); the texts that now splice an owner macro (setup replies, the boot
sanitize line, the per-user settings path, the factory reset prompt and help)
are compiled and compared with the old bytes; and the production blocks that
changed (factory reset's key removal, the peers.phone writer and reader, the
restore's network.wifi merge) run on the same JSON as the old code, embedded
here, with byte-identical output.

--root <repository> reads another tree (for example `git archive HEAD`); the
pre-change tree must fail. --print-table prints the compiled permission table
(for an old/new comparison of the whole table).
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_web_batch_handlers import extract_block  # noqa: E402

SOURCE_SUFFIXES = {'.c', '.cpp', '.h', '.hpp'}

# The values the old code spelled at every site.
OLD_PATHS = {
    'HW1_SETTINGS_JSON_PATH': '/system/settings.json',
    'HW1_DEBUG_JSON_PATH': '/system/debug.json',
    'HW1_USERS_JSON_PATH': '/system/users/users.json',
    'HW1_PENDING_USERS_JSON_PATH': '/system/users/pending_users.json',
    'HW1_USER_SETTINGS_DIR': '/system/users/user_settings',
    'HW1_BOOT_ANCHORS_JSON_PATH': '/system/boot_anchors.json',
    'HW1_AUTOMATIONS_JSON_PATH': '/system/automations.json',
    'HW1_ESPNOW_DEVICES_JSON_PATH': '/system/espnow/devices.json',
}
# The state-file rows of the old permission table: index|path|user|admin|system|exact|exempt.
OLD_STATE_ROWS = [
    '0|/system/users/user_settings|0|1|63|0|0',
    '1|/system/users/pending_users.json|0|1|63|1|0',
    '2|/system/settings.json|0|1|63|1|0',
    '3|/system/debug.json|0|1|63|1|0',
    '4|/system/automations.json|0|1|63|1|0',
    '5|/system/espnow/devices.json|0|1|63|1|0',
]
# Texts that now splice an owner macro, with their old bytes.
OLD_TEXTS = {
    'setup.saved': b'Saved /system/users/users.json',
    'setup.created': b'Created /system/automations.json',
    'fs.sanitize': b'[autos] Boot sanitize: /system/automations.json not found, skipping',
    'settings.userPath': b'/system/users/user_settings/%lu.json',
    'espnow.devicesTemp': b'/system/espnow/devices.json.tmp',
}
OLD_PROMPT_LINES = [
    b'  - Delete all user accounts (/system/users/users.json), pending requests and user settings\n',
    b'  - Delete all user accounts (/system/users/users.json)\n',
]
OLD_HELP_LINE = b"Deletes /system/users/users.json, pending account requests, every user's\n"

OLD_PIPE_KEYS = {
    'kPhonePipeSecret': 'phonePipeSecret',
    'kPhonePipeSecretGenerated': 'phonePipeSecretGenerated',
    'kPipeChannelVersion': 'pipeChannelVersion',
    'kPipeEnrolled': 'pipeEnrolled',
    'kPipeBondVersion': 'pipeBondVersion',
    'kPipeIdentity': 'pipeIdentity',
    'kPipeIdentityType': 'pipeIdentityType',
}
# The lists factory reset removed (System_Settings.cpp before the change).
OLD_ENROLLMENT_LIST = 'pipeChannelVersion,pipeEnrolled,pipeBondVersion,pipeIdentity,pipeIdentityType'
OLD_SECRET_LIST = 'phonePipeSecret,phonePipeSecretGenerated'

OLD_WIFI_KEYS = {
    'kParent': 'network',
    'kSection': 'wifi',
    'kModuleSection': 'network.wifi',
    'kNetworks': 'networks',
    'kAutoStart': 'wifiAutoStart',
    'kAutoReconnect': 'wifiAutoReconnect',
}

# The old blocks, verbatim, that the value check runs beside the new ones.
OLD_FACTORY_RESET_BLOCK = r'''
    JsonObject bluetooth=doc["network"]["bluetooth"].as<JsonObject>();
    bluetooth.remove("phonePipeSecret");bluetooth.remove("phonePipeSecretGenerated");
    JsonObject phone=bluetooth["peers"]["phone"].as<JsonObject>();
    for(const char* key:{"pipeChannelVersion","pipeEnrolled","pipeBondVersion","pipeIdentity","pipeIdentityType"})phone.remove(key);
'''
OLD_PEER_WRITE = r'''
    e.remove("pipeBondVersion");e.remove("pipeIdentity");e.remove("pipeIdentityType");
    e.remove("pipeChannelVersion");e.remove("pipeEnrolled");
    if(pipeEnrollment.length()) {e["pipeChannelVersion"]=1;e["pipeEnrolled"]=true;}
'''
OLD_PEER_VALID = r'''e["pipeChannelVersion"].is<int>() && e["pipeChannelVersion"].as<int>()==1 &&
                         e["pipeEnrolled"].is<bool>() && e["pipeEnrolled"].as<bool>()'''
OLD_RESTORE_WIFI = r'''
        mergedDoc["network"]["wifi"]["networks"] = deviceSettingsDoc["network"]["wifi"]["networks"];
        mergedDoc["network"]["wifi"]["wifiAutoStart"]     = true;
        mergedDoc["network"]["wifi"]["wifiAutoReconnect"] = true;
'''
OLD_RESTORE_PROBE = 'deviceSettingsDoc["network"]["wifi"]["networks"].is<JsonArray>()'


def code_only(source: str) -> str:
    """The source with C/C++ comments removed (string literals kept)."""
    out, pos, state = [], 0, 'code'
    while pos < len(source):
        char, following = source[pos], source[pos + 1:pos + 2]
        if state == 'line':
            if char == '\n':
                state = 'code'
                out.append(char)
        elif state == 'comment':
            if char == '\n':
                out.append(char)
            if char == '*' and following == '/':
                state = 'code'
                pos += 1
        elif state in ('string', 'character'):
            out.append(char)
            if char == '\\':
                out.append(following)
                pos += 1
            elif char == ('"' if state == 'string' else "'"):
                state = 'code'
        elif char == '/' and following == '/':
            state, pos = 'line', pos + 1
        elif char == '/' and following == '*':
            state, pos = 'comment', pos + 1
        else:
            if char in ('"', "'"):
                state = 'string' if char == '"' else 'character'
            out.append(char)
        pos += 1
    return ''.join(out)


def body(block: str) -> str:
    return block[block.index('{') + 1:block.rindex('}')]


def sources(component: Path):
    for path in sorted(component.rglob('*')):
        if path.suffix in SOURCE_SUFFIXES and path.is_file() and \
                path.relative_to(component).parts[0] != 'test':
            yield path


def contract(root: Path) -> list[str]:
    failures: list[str] = []
    component = root / 'components' / 'hardwareone'
    paths_owner = component / 'System_StatePaths.h'
    pipe_owner = component / 'BLE_PhonePipeKeys.h'
    wifi_owner = component / 'network' / 'wifi' / 'System_WiFiNetworks.h'

    def rel(path: Path) -> str:
        return str(path.relative_to(component))

    def read(name: str) -> str:
        return (component / name).read_text()

    # 1. Each value is a literal only in its owner.
    wifi_literals = ['"' + v + '"' for k, v in OLD_WIFI_KEYS.items() if k not in ('kParent', 'kSection', 'kNetworks')]
    for path in sources(component):
        code = code_only(path.read_text(errors='replace'))
        if path != paths_owner:
            for value in OLD_PATHS.values():
                if value in code:
                    failures.append(f'{rel(path)} spells {value}; use System_StatePaths.h')
        if path != pipe_owner:
            for value in OLD_PIPE_KEYS.values():
                if '"' + value + '"' in code:
                    failures.append(f'{rel(path)} spells the pipe credential key "{value}"; use BLE_PhonePipeKeys.h')
        if path != wifi_owner:
            for literal in wifi_literals:
                if literal in code:
                    failures.append(f'{rel(path)} spells the Wi-Fi settings key {literal}; use hw1wifijson')
            if re.search(r'\["wifi"\]', code):
                failures.append(f'{rel(path)} spells the network.wifi section; use hw1wifijson')

    # The owners hold the old values.
    if not paths_owner.exists():
        failures.append('System_StatePaths.h (owner of the /system state paths) is missing')
    else:
        owner = paths_owner.read_text()
        for name, value in OLD_PATHS.items():
            if not re.search(r'^#define ' + name + r'\s+"' + re.escape(value) + r'"', owner, re.M):
                failures.append(f'System_StatePaths.h does not own {name} "{value}"')
    if not pipe_owner.exists():
        failures.append('BLE_PhonePipeKeys.h (owner of the pipe credential keys) is missing')
    else:
        owner = code_only(pipe_owner.read_text())
        for name, value in OLD_PIPE_KEYS.items():
            if f'inline constexpr char {name}[] = "{value}";' not in owner:
                failures.append(f'BLE_PhonePipeKeys.h does not own {name} "{value}"')
        if '#include' in owner:
            failures.append('BLE_PhonePipeKeys.h must stay pure (factory reset uses it in every build)')
    wifi_header = wifi_owner.read_text()
    if 'namespace hw1wifijson {' not in wifi_header:
        failures.append('System_WiFiNetworks.h does not own the network.wifi names (hw1wifijson)')
    else:
        owner = extract_block(wifi_header, 'namespace hw1wifijson')
        for name, value in OLD_WIFI_KEYS.items():
            if f'inline constexpr char {name}[] = "{value}";' not in owner:
                failures.append(f'hw1wifijson does not own {name} "{value}"')
        if wifi_header.index('namespace hw1wifijson') > wifi_header.index('#if ENABLE_WIFI'):
            failures.append('hw1wifijson must sit outside #if ENABLE_WIFI (the restore uses it in every build)')

    # 2. Every former site uses the owner's name.
    def expect(text: str, needle: str, where: str, count: int = 1) -> None:
        found = text.count(needle)
        if found != count:
            failures.append(f'{where}: expected {count}x `{needle}`, found {found}')

    main = code_only(read('HardwareOne.cpp'))
    expect(main, 'const char* SETTINGS_JSON_FILE = HW1_SETTINGS_JSON_PATH;', 'HardwareOne.cpp')
    expect(main, 'const char* AUTOMATIONS_JSON_FILE = HW1_AUTOMATIONS_JSON_PATH;', 'HardwareOne.cpp')
    user = code_only(read('System_User.cpp'))
    expect(user, '#define PENDING_USERS_FILE HW1_PENDING_USERS_JSON_PATH', 'System_User.cpp')
    expect(user, 'const char* USERS_JSON_FILE = HW1_USERS_JSON_PATH;', 'System_User.cpp')
    expect(user, 'const char* BOOT_ANCHORS_FILE = HW1_BOOT_ANCHORS_JSON_PATH;', 'System_User.cpp')
    expect(code_only(read('System_Settings.h')), '#define DEBUG_JSON_FILE HW1_DEBUG_JSON_PATH', 'System_Settings.h')
    espnow = code_only(read('System_ESPNow.cpp'))
    expect(espnow, 'static const char* ESPNOW_DEVICES_FILE = HW1_ESPNOW_DEVICES_JSON_PATH;', 'System_ESPNow.cpp')
    expect(espnow, 'static const char* ESPNOW_DEVICES_TEMP_FILE = HW1_ESPNOW_DEVICES_JSON_PATH ".tmp";',
           'System_ESPNow.cpp')

    fs = code_only(read('System_Filesystem.cpp'))
    table = extract_block(fs, 'static const PathRule sPathRules[]')
    for name in ('HW1_USER_SETTINGS_DIR', 'HW1_PENDING_USERS_JSON_PATH', 'HW1_SETTINGS_JSON_PATH',
                 'HW1_DEBUG_JSON_PATH', 'HW1_AUTOMATIONS_JSON_PATH', 'HW1_ESPNOW_DEVICES_JSON_PATH'):
        if not re.search(r'\{\s*' + name + r'\s*,', table):
            failures.append(f'the permission table has no row keyed by {name}')
    expect(fs, 'const char* criticalFiles[] = { HW1_DEBUG_JSON_PATH, HW1_AUTOMATIONS_JSON_PATH, HW1_USERS_JSON_PATH };',
           'System_Filesystem.cpp boot validation')
    expect(fs, 'VFS::mkdirGuarded(HW1_USER_SETTINGS_DIR,', 'System_Filesystem.cpp boot mkdir')
    expect(extract_block(fs, 'static void cleanupBootTemporaryFiles('), 'HW1_USER_SETTINGS_DIR',
           'cleanupBootTemporaryFiles')

    migration = code_only(read('WebServer_MigrationTool.cpp'))
    if re.search(r'^static const char\* [A-Z_]+\s*=\s*"/system/', migration, re.M):
        failures.append('WebServer_MigrationTool.cpp keeps its own /system path constants')
    for name in ('SETTINGS_FILE', 'DEBUG_FILE', 'USERS_FILE', 'AUTOMATIONS_FILE', 'ESPNOW_DEVICES',
                 'USER_SETTINGS_DIR', 'BOOT_ANCHORS_FILE'):
        if re.search(r'(?<![A-Z0-9_])' + name + r'\b', migration):
            failures.append(f'WebServer_MigrationTool.cpp still names {name}')
    # A backup carries every state file but the pending account list.
    allowed = extract_block(migration, 'static bool restorePathAllowed(')
    for name in OLD_PATHS:
        if name != 'HW1_PENDING_USERS_JSON_PATH' and name not in allowed:
            failures.append(f'restorePathAllowed does not take {name} from the owner')
    backup = extract_block(migration, 'static esp_err_t handleBackup(')
    for name in OLD_PATHS:
        if name != 'HW1_PENDING_USERS_JSON_PATH' and name not in backup:
            failures.append(f'handleBackup does not bundle {name}')

    utils = code_only(read('System_Utils.cpp'))
    expect(utils, 'static constexpr const char* kFactoryResetPendingUsers = HW1_PENDING_USERS_JSON_PATH;',
           'System_Utils.cpp factory reset')
    expect(utils, 'static constexpr const char* kFactoryResetUserSettingsDir = HW1_USER_SETTINGS_DIR;',
           'System_Utils.cpp factory reset')
    expect(code_only(read('WebServer_Server.cpp')), 'Guarded(HW1_PENDING_USERS_JSON_PATH,', 'WebServer_Server.cpp', 2)
    for name, header in (('HardwareOne.cpp', 'System_StatePaths.h'), ('System_User.cpp', 'System_StatePaths.h'),
                         ('System_Settings.h', 'System_StatePaths.h'), ('System_ESPNow.cpp', 'System_StatePaths.h'),
                         ('System_Filesystem.cpp', 'System_StatePaths.h'), ('System_Utils.cpp', 'System_StatePaths.h'),
                         ('System_FirstTimeSetup.cpp', 'System_StatePaths.h'),
                         ('WebServer_Server.cpp', 'System_StatePaths.h'),
                         ('WebServer_MigrationTool.cpp', 'System_StatePaths.h'),
                         ('WebServer_MigrationTool.cpp', 'network/wifi/System_WiFiNetworks.h'),
                         ('BLE_Peers.cpp', 'BLE_PhonePipeKeys.h'), ('BLE_Server.cpp', 'BLE_PhonePipeKeys.h'),
                         ('System_Settings.cpp', 'BLE_PhonePipeKeys.h')):
        if f'#include "{header}"' not in read(name):
            failures.append(f'{name} does not include {header}')

    # Pipe keys: the writer owns the list factory reset removes.
    peers = code_only(read('BLE_Peers.cpp'))
    writer = extract_block(peers, 'void blePeersWriteJson(')
    if 'for (const char* key : hw1pipekeys::kPhonePipeEnrollmentKeys) e.remove(key);' not in writer:
        failures.append('blePeersWriteJson does not drop the enrolment keys through hw1pipekeys::kPhonePipeEnrollmentKeys')
    written = re.findall(r'e\[hw1pipekeys::(\w+)\]\s*=', writer)
    listed = re.search(r'kPhonePipeEnrollmentKeys\[\]\s*=\s*\{([^}]*)\}', code_only(pipe_owner.read_text())) \
        if pipe_owner.exists() else None
    listed_names = re.findall(r'\w+', listed.group(1)) if listed else []
    if sorted(written) != ['kPipeChannelVersion', 'kPipeEnrolled']:
        failures.append(f'blePeersWriteJson writes the phone enrolment as {written}, not through hw1pipekeys')
    for name in written:
        if name not in listed_names:
            failures.append(f'blePeersWriteJson writes {name}, which factory reset does not remove')
    reader = extract_block(peers, 'void blePeersReadJson(')
    if reader.count('e[hw1pipekeys::kPipeChannelVersion]') != 2 or reader.count('e[hw1pipekeys::kPipeEnrolled]') != 2:
        failures.append('blePeersReadJson does not check the enrolment marker through hw1pipekeys')
    settings = code_only(read('System_Settings.cpp'))
    reset = extract_block(settings, 'if(clearPipeCredentials)')
    if 'for(const char* key:hw1pipekeys::kPhonePipeSecretKeys)bluetooth.remove(key);' not in reset or \
            'for(const char* key:hw1pipekeys::kPhonePipeEnrollmentKeys)phone.remove(key);' not in reset:
        failures.append('factory reset does not remove the pipe credential through the hw1pipekeys lists')
    server = code_only(read('BLE_Server.cpp'))
    for name in ('kPhonePipeSecret', 'kPhonePipeSecretGenerated'):
        if not re.search(r'\{\s*hw1pipekeys::' + name + r',\s*SETTING_', server):
            failures.append(f'the bluetooth settings row for {OLD_PIPE_KEYS[name]} does not use hw1pipekeys::{name}')

    # Wi-Fi: the rows, the module section, the networks array and the restore share the names.
    wifi = code_only(read('System_WiFi.cpp'))
    rows = extract_block(wifi, 'static const SettingEntry wifiSettingsEntries[]')
    for name in ('kAutoStart', 'kAutoReconnect'):
        if not re.search(r'\{\s*hw1wifijson::' + name + r',\s*SETTING_BOOL', rows):
            failures.append(f'the wifi settings row {OLD_WIFI_KEYS[name]} does not use hw1wifijson::{name}')
    module = extract_block(wifi, 'extern const SettingsModule wifiSettingsModule')
    if not re.search(r'"wifi",\s*hw1wifijson::kModuleSection,', module):
        failures.append('wifiSettingsModule does not take its section from hw1wifijson::kModuleSection')
    nets = code_only((component / 'network' / 'wifi' / 'System_WiFiNetworks.cpp').read_text())
    expect(nets, 'doc[hw1wifijson::kParent][hw1wifijson::kSection][hw1wifijson::kNetworks]',
           'System_WiFiNetworks.cpp', 3)
    restore = extract_block(migration, 'static bool writeRestoreFilesFromDoc(')
    merge = extract_block(restore, 'if (haveDeviceWifi && pathStr == ')
    for name in ('kNetworks', 'kAutoStart', 'kAutoReconnect'):
        if f'mergedDoc[kParent][kSection][{name}]' not in merge:
            failures.append(f'the restore does not write network.wifi.{OLD_WIFI_KEYS[name]} through hw1wifijson')
    if 'using namespace hw1wifijson;' not in merge:
        failures.append('the restore merge does not take its names from hw1wifijson')
    if 'deviceSettingsDoc[hw1wifijson::kParent][hw1wifijson::kSection][hw1wifijson::kNetworks].is<JsonArray>()' \
            not in restore:
        failures.append('the restore does not probe this device\'s networks through hw1wifijson')
    return failures


def compile_and_run(cxx: str, work: Path, name: str, code: str, includes: list[Path]) -> bytes:
    src = work / f'{name}.cpp'
    exe = work / name
    src.write_text(code)
    command = [cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', str(src), '-o', str(exe)]
    for include in includes:
        command[1:1] = ['-I', str(include)]
    subprocess.run(command, check=True)
    return subprocess.run([str(exe)], check=True, stdout=subprocess.PIPE).stdout


def table_program(component: Path) -> str:
    fs = (component / 'System_Filesystem.cpp').read_text()
    fs_h = (component / 'System_Filesystem.h').read_text()
    owner = '#include "System_StatePaths.h"\n' if (component / 'System_StatePaths.h').exists() else ''
    return f'''#include <cstdint>
#include <cstdio>
{owner}{extract_block(fs_h, 'enum FilePermission')};
{extract_block(fs, 'struct PathRule')};
{extract_block(fs, 'static const PathRule sPathRules[]')};
int main() {{
  for (size_t i = 0; i < sizeof(sPathRules) / sizeof(sPathRules[0]); ++i) {{
    const PathRule& r = sPathRules[i];
    std::printf("%zu|%s|%u|%u|%u|%d|%d\\n", i, r.path ? r.path : "(null)", unsigned(r.userPerms),
                unsigned(r.adminPerms), unsigned(r.systemPerms), int(r.exactMatch), int(r.exemptSensitiveExt));
  }}
  return 0;
}}
'''


def values(root: Path, cxx: str) -> list[str]:
    failures: list[str] = []
    component = root / 'components' / 'hardwareone'
    json_include = root / 'components' / 'hardwareone_libs' / 'ArduinoJson' / 'src'

    def read(name: str) -> str:
        return (component / name).read_text()

    with tempfile.TemporaryDirectory(prefix='hw1-state-owners-') as tmp:
        work = Path(tmp)
        # The permission table: the state-file rows are the old rows.
        rows = compile_and_run(cxx, work, 'table', table_program(component), [component]).decode().splitlines()
        state = [row for row in rows if row.split('|')[1] in OLD_PATHS.values()]
        if state != OLD_STATE_ROWS:
            failures.append(f'permission table state rows changed: {state} != {OLD_STATE_ROWS}')

        # The texts that quote a path.
        setup, fs, settings, espnow = (read('System_FirstTimeSetup.cpp'), read('System_Filesystem.cpp'),
                                       read('System_Settings.cpp'), read('System_ESPNow.cpp'))
        found = {
            'setup.saved': re.search(r'broadcastOutput\(("Saved [^;]*?)\);', setup),
            'setup.created': re.search(r'broadcastOutput\(("Created [^;]*?)\);', setup),
            'fs.sanitize': re.search(r'DEBUG_AUTO_SCHEDULERF\(("\[autos\] Boot sanitize: [^;]*?not found, skipping")\);', fs),
            'settings.userPath': re.search(r'snprintf\(pathBuf, sizeof\(pathBuf\), (.*?), \(unsigned long\)userId\);',
                                           extract_block(settings, 'String getUserSettingsPath(')),
            'espnow.devicesTemp': re.search(r'static const char\* ESPNOW_DEVICES_TEMP_FILE = ([^;]+);', espnow),
        }
        missing = [k for k, v in found.items() if not v]
        if missing:
            return failures + [f'could not extract {", ".join(missing)}']
        utils = read('System_Utils.cpp')
        prompt = re.search(r'String prompt = (wipeAll \?.*?This cannot be undone\.";)', utils, re.S)
        help_row = re.search(r'("Usage: factoryreset \[all\] \(confirmation required\)\\n".*?),\s*/\*requiresSuperAdmin=\*/',
                             utils, re.S)
        if not prompt or not help_row:
            return failures + ['could not extract the factory reset prompt or help']
        include = '#include "System_StatePaths.h"\n' if (component / 'System_StatePaths.h').exists() else ''
        puts = ''.join(f'  put({m.group(1)});\n' for m in found.values())
        out = compile_and_run(cxx, work, 'texts', f'''{include}#include <cstdio>
#include <cstring>
#define HW1_RADIO_COMPANION 0
static void put(const char* s) {{ std::fwrite(s, 1, std::strlen(s) + 1, stdout); }}
static const char* prompt(bool wipeAll) {{
  return {prompt.group(1)[:-1]};
}}
int main() {{
{puts}  put(prompt(true));
  put(prompt(false));
  put({help_row.group(1)});
  return 0;
}}
''', [component])
        got = out.split(b'\0')[:-1]
        for (name, old), new in zip(OLD_TEXTS.items(), got):
            if new != old:
                failures.append(f'{name} changed: {new!r} != {old!r}')
        if len(got) != len(OLD_TEXTS) + 3:
            failures.append(f'expected {len(OLD_TEXTS) + 3} texts, got {len(got)}')
        else:
            full, plain, help_text = got[len(OLD_TEXTS):]
            if OLD_PROMPT_LINES[0] not in full or OLD_PROMPT_LINES[1] not in plain:
                failures.append('the factory reset prompt no longer quotes /system/users/users.json as before')
            if OLD_HELP_LINE not in help_text:
                failures.append('the factory reset help no longer quotes /system/users/users.json as before')

        # The owners' values.
        if (component / 'System_StatePaths.h').exists():
            out = compile_and_run(cxx, work, 'paths', '#include "System_StatePaths.h"\n#include <cstdio>\nint main() {\n'
                                  + ''.join(f'  std::puts({name});\n' for name in OLD_PATHS) + '  return 0;\n}\n',
                                  [component])
            if out.decode().splitlines() != list(OLD_PATHS.values()):
                failures.append(f'state paths changed: {out!r}')

        pipe_owner = component / 'BLE_PhonePipeKeys.h'
        wifi_header = (component / 'network' / 'wifi' / 'System_WiFiNetworks.h').read_text()
        have_pipe = pipe_owner.exists()
        have_wifi = 'namespace hw1wifijson' in wifi_header
        if not have_pipe or not have_wifi:
            return failures + ['pipe or Wi-Fi key owner missing; JSON blocks not compared']
        wifi_ns = extract_block(wifi_header, 'namespace hw1wifijson') + '  // namespace hw1wifijson\n'

        # The production blocks that changed, beside the old ones, on the same JSON.
        peers = read('BLE_Peers.cpp')
        writer = extract_block(peers, 'void blePeersWriteJson(')
        write_start = writer.index('e["autoReconnect"] = autoReconnect;') + len('e["autoReconnect"] = autoReconnect;')
        write_new = writer[write_start:writer.index('// Only emit pairedByUser', write_start)]
        valid = re.search(r'const bool valid=(.*?);', extract_block(peers, 'void blePeersReadJson('), re.S)
        reset_new = body(extract_block(settings, 'if(clearPipeCredentials)'))
        migration = read('WebServer_MigrationTool.cpp')
        restore = extract_block(migration, 'static bool writeRestoreFilesFromDoc(')
        merge_new = body(extract_block(restore, 'if (haveDeviceWifi && pathStr == '))
        probe = re.search(r'if \((deviceSettingsDoc\[[^\n]*\]\.is<JsonArray>\(\))\) \{', restore)
        if not valid or not probe:
            return failures + ['could not extract the enrolment check or the restore probe']
        program = f'''#include <ArduinoJson.h>
#include "BLE_PhonePipeKeys.h"
#include <cstdio>
#include <string>
{wifi_ns}
static std::string text(const JsonDocument& doc) {{ std::string s; serializeJson(doc, s); return s; }}
static void resetNew(JsonDocument& doc) {{ {reset_new} }}
static void resetOld(JsonDocument& doc) {{ {OLD_FACTORY_RESET_BLOCK} }}
static void writeNew(JsonObject e, const std::string& pipeEnrollment) {{ {write_new} }}
static void writeOld(JsonObject e, const std::string& pipeEnrollment) {{ {OLD_PEER_WRITE} }}
static bool validNew(JsonObjectConst e) {{ return {valid.group(1)}; }}
static bool validOld(JsonObjectConst e) {{ return {OLD_PEER_VALID}; }}
static void mergeNew(JsonDocument& mergedDoc, JsonDocument& deviceSettingsDoc) {{ {merge_new} }}
static void mergeOld(JsonDocument& mergedDoc, JsonDocument& deviceSettingsDoc) {{ {OLD_RESTORE_WIFI} }}
static bool probeNew(JsonDocument& deviceSettingsDoc) {{ return {probe.group(1)}; }}
static bool probeOld(JsonDocument& deviceSettingsDoc) {{ return {OLD_RESTORE_PROBE}; }}
static int failures = 0;
static void same(const std::string& a, const std::string& b, const char* what) {{
  if (a != b) {{ ++failures; std::fprintf(stderr, "%s differs:\\n new %s\\n old %s\\n", what, a.c_str(), b.c_str()); }}
}}
int main() {{
  const char* settings[] = {{
    "{{}}",
    "{{\\"network\\":{{\\"wifi\\":{{\\"wifiEnabled\\":false}}}}}}",
    "{{\\"network\\":{{\\"bluetooth\\":{{\\"phonePipeSecret\\":\\"AES:x\\",\\"phonePipeSecretGenerated\\":true,"
    "\\"bleSecureChannelSecret\\":\\"AES:y\\",\\"peers\\":{{\\"phone\\":{{\\"mac1\\":\\"a\\",\\"pipeChannelVersion\\":1,"
    "\\"pipeEnrolled\\":true,\\"pipeBondVersion\\":1,\\"pipeIdentity\\":\\"legacy\\",\\"pipeIdentityType\\":1,"
    "\\"pipeFuture\\":2,\\"pairedByUser\\":\\"owner\\"}},\\"r1-ring\\":{{\\"mac1\\":\\"r\\",\\"pipeEnrolled\\":true}}}}}},"
    "\\"wifi\\":{{\\"networks\\":[{{\\"ssid\\":\\"src\\",\\"password\\":\\"AES:src\\"}}],\\"wifiEnabled\\":false,"
    "\\"wifiAutoStart\\":false,\\"autoReconnect\\":false,\\"other\\":1}}}},\\"system\\":{{\\"a\\":1}}}}",
    "{{\\"network\\":{{\\"bluetooth\\":{{\\"peers\\":{{\\"phone\\":{{\\"pipeEnrolled\\":1}}}}}}}}}}",
  }};
  const char* device[] = {{
    "{{\\"network\\":{{\\"wifi\\":{{\\"networks\\":[{{\\"ssid\\":\\"here\\",\\"password\\":\\"AES:here\\"}}]}}}}}}",
    "{{\\"network\\":{{\\"wifi\\":{{\\"networks\\":{{\\"not\\":\\"array\\"}}}}}}}}",
    "{{}}",
  }};
  for (const char* s : settings) {{
    JsonDocument a, b;
    deserializeJson(a, s); deserializeJson(b, s);
    resetNew(a); resetOld(b);
    same(text(a), text(b), "factory reset key removal");
    for (const char* d : device) {{
      JsonDocument ma, mb, da, db;
      deserializeJson(ma, s); deserializeJson(mb, s); deserializeJson(da, d); deserializeJson(db, d);
      if (probeNew(da) != probeOld(db)) {{ ++failures; std::fprintf(stderr, "restore probe differs on %s\\n", d); }}
      mergeNew(ma, da); mergeOld(mb, db);
      same(text(ma), text(mb), "restore network.wifi merge");
    }}
    JsonDocument pa, pb;
    deserializeJson(pa, s); deserializeJson(pb, s);
    JsonObjectConst phoneA = pa["network"]["bluetooth"]["peers"]["phone"].as<JsonObjectConst>();
    JsonObjectConst phoneB = pb["network"]["bluetooth"]["peers"]["phone"].as<JsonObjectConst>();
    if (validNew(phoneA) != validOld(phoneB)) {{ ++failures; std::fprintf(stderr, "enrolment check differs on %s\\n", s); }}
    for (const char* enrollment : {{"", "app-key-v1"}}) {{
      JsonDocument wa, wb;
      deserializeJson(wa, s); deserializeJson(wb, s);
      JsonObject ea = wa["network"]["bluetooth"]["peers"]["phone"].to<JsonObject>();
      JsonObject eb = wb["network"]["bluetooth"]["peers"]["phone"].to<JsonObject>();
      ea["mac1"] = "a"; eb["mac1"] = "a";
      writeNew(ea, enrollment); writeOld(eb, enrollment);
      same(text(wa), text(wb), "peers.phone enrolment write");
      // Over an object that still holds every old key (the writer drops them first).
      JsonDocument xa, xb;
      deserializeJson(xa, settings[2]); deserializeJson(xb, settings[2]);
      writeNew(xa["network"]["bluetooth"]["peers"]["phone"].as<JsonObject>(), enrollment);
      writeOld(xb["network"]["bluetooth"]["peers"]["phone"].as<JsonObject>(), enrollment);
      same(text(xa), text(xb), "peers.phone enrolment rewrite");
    }}
  }}
  std::string list;
  for (const char* key : hw1pipekeys::kPhonePipeEnrollmentKeys) list += (list.empty() ? "" : ",") + std::string(key);
  std::string secrets;
  for (const char* key : hw1pipekeys::kPhonePipeSecretKeys) secrets += (secrets.empty() ? "" : ",") + std::string(key);
  std::printf("%s\\n%s\\n", list.c_str(), secrets.c_str());
'''
        program += ''.join(f'  std::printf("%s\\n", hw1pipekeys::{name});\n' for name in OLD_PIPE_KEYS)
        program += ''.join(f'  std::printf("%s\\n", hw1wifijson::{name});\n' for name in OLD_WIFI_KEYS)
        program += '  return failures ? 1 : 0;\n}\n'
        try:
            out = compile_and_run(cxx, work, 'json', program, [component, json_include]).decode().splitlines()
        except subprocess.CalledProcessError:
            return failures + ['the changed JSON blocks do not behave as the old ones (see above)']
        expected = [OLD_ENROLLMENT_LIST, OLD_SECRET_LIST, *OLD_PIPE_KEYS.values(), *OLD_WIFI_KEYS.values()]
        if out != expected:
            failures.append(f'pipe or Wi-Fi key values changed: {out} != {expected}')
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, default=HERE.parents[3], help='repository root to read')
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('c++'))
    parser.add_argument('--print-table', action='store_true', help='print the compiled permission table and exit')
    args = parser.parse_args()
    root = args.root.resolve()
    if args.print_table:
        with tempfile.TemporaryDirectory(prefix='hw1-state-table-') as tmp:
            component = root / 'components' / 'hardwareone'
            sys.stdout.write(compile_and_run(args.cxx, Path(tmp), 'table', table_program(component),
                                             [component]).decode())
        return 0
    failures = contract(root) + values(root, args.cxx)
    if failures:
        for failure in failures:
            print(f'state_file_owners: {failure}', file=sys.stderr)
        return 1
    print('state_file_owners: /system state paths, phone-link credential keys and network.wifi keys each have '
          'one owner; permission rows, texts and JSON output unchanged')
    return 0


if __name__ == '__main__':
    sys.exit(main())
