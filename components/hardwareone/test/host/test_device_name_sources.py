#!/usr/bin/env python3
"""Source contract for the one device name (System_DeviceName.h).

One stored name is the ESP-NOW name, the Bluetooth (GAP) name and the Wi-Fi
hostname, set by one command ('devicename', compiled in every build) and checked
in one place. What cannot run on the host is pinned in the source:
  - no Bluetooth-only name is left (setting, 'blename', its settings row) and no
    'espnowsetname'; Bluetooth starts with SelfDevice::deviceName() and builds its
    scan response with devicename::advertisedName (29 of up to 31 bytes; Arduino's
    default one breaks above 29);
  - the hostname is handed to the Wi-Fi stack after the settings load and before
    the boot starts anything else (SelfDevice::applyHostname calls
    WiFi.setHostname, which the stack applies when the station starts, and
    gives a running station the name too);
  - a new name is handed on in one place (deviceNameApply: ESP-NOW, the
    hostname, the "same name" warning), by the setter on set and clear and by
    the setup wizards' save after it applies the settings; no other file does
    it itself;
  - the Bluetooth client roles use the device name as their GAP name, and a
    cut name is advertised as shortened;
  - every frame field that carries this device's name is written with
    devicename::putField and every received one is read with devicename::getField;
  - the setter is registered with its settings row in an always-compiled module;
  - a settings file from before the one device name keeps its name: the boot
    load moves the old ESP-NOW (else a chosen Bluetooth) name into the setting
    after the registered read and before ESP-NOW's start gate, Bluetooth and the
    hostname read it; the boot's one-key rewrite patches the file and every
    full save removes the old keys (the pick: device_name_migration_tests; the
    move's RAM side with the boot load and the file rewrites: settings_file_writes).
"""
import argparse
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    a = p.parse_args()
    c = a.component
    failures = []

    def need(ok, what):
        if not ok:
            failures.append(what)

    def text(name):
        return (c / name).read_text()

    sources = sorted(list(c.glob('*.cpp')) + list(c.glob('*.h')))
    for path in sources:
        body = path.read_text()
        need('bleDeviceName' not in body, f'{path.name} still has the Bluetooth-only name setting')
        need('cmd_blename' not in body and '"blename"' not in body, f'{path.name} still has blename')
        need('espnowsetname' not in body, f'{path.name} still names espnowsetname')

    boot = text('HardwareOne.cpp')
    load = boot.find('readSettingsJson()')
    host = boot.find('SelfDevice::applyHostname();')
    first_cmd = boot.find('initializeCommandSystem();')
    need(0 <= load < host < first_cmd,
         'the boot does not hand the hostname to Wi-Fi after the settings load and before the rest starts')

    selfdev = text('System_SelfDevice.cpp')
    apply = selfdev[selfdev.find('void applyHostname() {'):]
    apply = apply[:apply.find('\n}\n')]
    need('WiFi.setHostname(name.c_str());' in apply and 'const String name = deviceName();' in apply,
         'applyHostname does not give the Wi-Fi stack the device name')
    need('if (WiFi.STA.started()) (void)WiFi.STA.setHostname(name.c_str());' in apply,
         'applyHostname does not give a running station the new name')
    name_fn = selfdev[selfdev.find('String deviceName() {'):]
    name_fn = name_fn[:name_fn.find('\n}\n')]
    need('devicename::check(stored)' in name_fn and 'devicename::kFallbackPrefix' in name_fn,
         'SelfDevice::deviceName does not check the stored name or fall back to HardwareOne-XXXXXX')

    # One hand-on of a new name (deviceNameApply): the setter on set and clear,
    # and the setup wizards' save, which writes the setting directly.
    setter = text('System_DeviceName.cpp')
    hand_on = setter[setter.find('void deviceNameApply('):]
    hand_on = hand_on[:hand_on.find('\n}\n')]
    need('SelfDevice::applyHostname();' in hand_on and 'espnowDeviceNameApplied(name);' in hand_on and
         'espnowNameInUseBy(name.c_str()' in hand_on,
         'deviceNameApply does not hand the name to ESP-NOW and Wi-Fi and look for another device with it')
    cmd = setter[setter.find('const char* cmd_devicename('):]
    need(cmd.count('deviceNameApply(') == 2, 'the setter does not hand the name on (deviceNameApply) on set and clear')
    wizard = text('System_SetupWizard.cpp')
    save = wizard[wizard.find('bool wizardSaveAndApplySettings('):]
    need('if (draft.deviceName != initial.deviceName) {' in save and
         0 <= save.find('applySettings();') < save.find('deviceNameApply(gSettings.espnowDeviceName'),
         'the setup wizard does not hand a new device name on after it applies the settings')
    for path in sources:
        if path.name in ('System_DeviceName.cpp', 'HardwareOne.cpp', 'System_SelfDevice.cpp', 'System_ESPNow.cpp',
                         'System_SelfDevice.h', 'System_ESPNow.h'):
            continue
        body = path.read_text()
        need('SelfDevice::applyHostname()' not in body and 'espnowDeviceNameApplied(' not in body,
             f'{path.name} hands the name on itself instead of through deviceNameApply')
    need('devicename::check(name.c_str())' in setter, 'the setter does not check the rule')
    need('"deviceName", SETTING_STRING, &gSettings.espnowDeviceName' in setter and '"devicename" }' in setter,
         'the device name settings row is not in the always-compiled module, through the setter')
    settings = text('System_Settings.cpp')
    need('registerSettingsModule(&deviceSettingsModule);' in settings, 'deviceSettingsModule is not registered')
    need('{ "devicename",' in settings and 'cmd_devicename' in settings, 'devicename is not a command')
    espnow = text('System_ESPNow.cpp')
    need('"deviceName", SETTING_STRING' not in espnow, 'the ESP-NOW module still owns the device name row')

    ble = text('BLE_Server.cpp')
    need('const String deviceName = SelfDevice::deviceName();' in ble and
         'bluetoothHalInit(deviceName.c_str())' in ble, 'Bluetooth does not start with the device name')
    need('devicename::advertisedName(deviceName.c_str()' in ble and 'setScanResponseData(scanResponse)' in ble and
         'setShortName' in ble, 'the scan response does not carry the advertised name')
    need('out["deviceName"]     = SelfDevice::deviceName();' in ble, 'bleinfo does not report the device name')
    need(re.search(r'if \(complete\) \{\s*scanResponse\.setName\(String\(advName\)\);\s*\} else \{\s*'
                   r'scanResponse\.setShortName\(String\(advName\)\);', ble) is not None,
         'the scan response does not mark a cut name as shortened')
    central = text('BLE_CentralStack.cpp')
    need('const String hostName = SelfDevice::deviceName();' in central and
         'bluetoothHalInit(hostName.c_str())' in central,
         'the Bluetooth client roles do not use the device name as their GAP name')

    # Wire writers and readers of the device name.
    raw_copies = re.findall(r'strncpy\([^;]*deviceName[^;]*gSettings\.espnowDeviceName', espnow)
    need(not raw_copies, f'System_ESPNow.cpp copies the device name into a frame without putField: {raw_copies}')
    need(espnow.count('devicename::putField(hb.deviceName, gSettings.espnowDeviceName.c_str());') >= 1,
         'the heartbeat name is not written with putField')
    for needle in ('devicename::putField(cap.deviceName, gSettings.espnowDeviceName.c_str());',
                   'devicename::putField(beacon.deviceName, gSettings.espnowDeviceName.c_str());',
                   'devicename::putField(p.deviceName, gSettings.espnowDeviceName.c_str());',
                   'devicename::putField(payload->deviceName, gSettings.espnowDeviceName.c_str());',
                   'devicename::putField(payload.name, peerName);',
                   'devicename::getField(tp.name, sizeof(tp.name), wireName)',
                   'devicename::getField(cap.deviceName, sizeof(cap.deviceName), validName)',
                   'devicename::getField(meta.deviceName, sizeof(meta.deviceName), name)'):
        need(needle in espnow, f'System_ESPNow.cpp does not have: {needle}')
    # A peer's name given to the pair commands follows the same rule (the rule
    # itself runs in test_espnow_mesh_slots): both commands return its refusal.
    need('if (const char* refused = espnowPeerAliasRefusal(name, mac)) return refused;' in espnow and
         'return espnowPeerAliasRefusal(deviceName);' in espnow,
         'espnow pair / espnowpairsecure do not refuse a peer name that breaks the device-name rule')
    need('strcmp(wireName, "Unknown")' not in espnow and '"Unknown";\n      MeshPeerHealth' not in espnow,
         'TOPO still puts the "Unknown" placeholder on the air or reads it as no name')
    need('devicename::putField(f.partnerName, name);' in text('System_ESPNow_BondForward.cpp'),
         'FORWARD_LINK partnerName is not written with putField')
    need('devicename::getField(f.partnerName, sizeof(f.partnerName), shown)' in text('System_ESPNow_ForwardRx.cpp'),
         'FORWARD_LINK partnerName is not read with getField')

    # The move from the names the device name replaced (System_DeviceNameLegacy.h).
    load = settings[settings.find('bool readSettingsJson() {'):]
    load = load[:load.find('\n}\n')]
    need(0 <= load.find('readRegisteredSettings(doc, nullptr') < load.find('deviceNameMigrateLegacy(doc);'),
         'readSettingsJson does not move the old name after the registered read')
    restamp = settings[settings.find('bool settingsRestampFirmwareVersion() {'):]
    restamp = restamp[:restamp.find('\n}\n')]
    need('deviceNameLegacyPatchPending()' in restamp and 'deviceNameLegacyPatch(doc)' in restamp and
         0 <= restamp.find('writeJsonSnapshot(') < restamp.find('deviceNameLegacyPatchSaved();'),
         'the boot rewrite does not patch the file or marks the patch saved before writing it')
    full = settings[settings.find('static bool writeSettingsJsonImpl('):]
    full = full[:full.find('\n}\n')]
    need(0 <= full.find('buildSettingsJsonDoc(doc') < full.find('devicename::removeLegacyKeys(doc);') <
         full.find('writeSettingsSnapshot(doc'),
         'a full save does not remove the old name keys after the merge and build')
    move = setter[setter.find('void deviceNameMigrateLegacy('):]
    move = move[:move.find('\n}\n')]
    need('devicename::pickLegacy(gSettings.espnowDeviceName.c_str()' in move and
         'gSettings.espnowDeviceName = pick.name;' in move and 'logSystemEvent(' in move,
         'deviceNameMigrateLegacy does not take the pick into the setting with a log line')
    # setup()'s own calls (the earlier bootAutostartEspNow call is the deferred
    # hand-off, which runs after setup).
    loaded = boot.find('if (readSettingsJson()) {')
    need(0 <= loaded < boot.rfind('bootAutostartEspNow(sBootNetServices.espnow);') and
         loaded < boot.rfind('if (initBluetooth())'),
         'ESP-NOW or Bluetooth can start before the settings load (and the move)')
    gate = espnow[espnow.find('const char* checkEspNowFirstTimeSetup() {'):]
    gate = gate[:gate.find('\n}\n')]
    need('devicename::valid(gSettings.espnowDeviceName.c_str())' in gate,
         "ESP-NOW's start gate does not read the device name setting")

    for f in failures:
        print(f'FAIL {f}')
    if failures:
        sys.exit(1)
    print('PASS device_name_sources')


if __name__ == '__main__':
    main()
