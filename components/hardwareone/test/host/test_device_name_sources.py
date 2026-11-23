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
  - with no name set the fallback is "HardwareOne-B2C3" (the last two address
    bytes only); every screen and text reply that shows the name marks it
    "(default)" through SelfDevice::deviceNameDisplay, while the Bluetooth (GAP
    and advertised) name, the hostname and the ESP-NOW fields carry the bare
    name; the JSON replies with the name carry "deviceNameSet" beside it;
  - the settings load reads only the device name: no old ESP-NOW or Bluetooth
    name key is read or moved (1.0 boards start erased), and ESP-NOW and
    Bluetooth start after the load.
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
    name_fn = selfdev[selfdev.find('static bool nameInUse('):]
    name_fn = name_fn[:name_fn.find('\n}\n')]
    need('devicename::check(stored)' in name_fn and 'devicename::kFallbackPrefix' in name_fn,
         'SelfDevice::nameInUse does not check the stored name or fall back to HardwareOne-B2C3')
    public_fn = selfdev[selfdev.find('String deviceName() {'):]
    public_fn = public_fn[:public_fn.find('\n}\n')]
    need('nameInUse(name)' in public_fn and 'devicename::valid(gSettings.espnowDeviceName.c_str())' in public_fn,
         'SelfDevice::deviceName does not return the stored name, else the fallback from nameInUse')
    need('bool deviceNameSet() { return devicename::valid(gSettings.espnowDeviceName.c_str()); }' in selfdev,
         'SelfDevice::deviceNameSet is not the rule check of the stored name')
    shown_fn = selfdev[selfdev.find('bool deviceNameDisplay('):]
    shown_fn = shown_fn[:shown_fn.find('\n}\n')]
    need('devicename::displayText(name, set, out, cap)' in shown_fn,
         'SelfDevice::deviceNameDisplay does not use the one wording (devicename::displayText)')
    header = text('System_DeviceName.h')
    need('"%s-%02X%02X", prefix ? prefix : kFallbackPrefix, m[4], m[5]' in header,
         'the fallback and peer label do not use the last two address bytes')
    need('constexpr const char* kDefaultMark = " (default)";' in header, 'the "(default)" wording is not in one place')

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

    # "(default)" is for people: never in the GAP or advertised name, the
    # hostname or a frame field. Those paths read the bare deviceName().
    for name, body in (('System_SelfDevice.cpp applyHostname', apply),
                       ('BLE_CentralStack.cpp', central)):
        need('deviceNameDisplay' not in body and 'kDefaultMark' not in body and '(default)' not in body,
             f'{name} puts the "(default)" mark into a Bluetooth or Wi-Fi name')
    init = ble[ble.find('const String deviceName = SelfDevice::deviceName();'):]
    init = init[:init.find('setScanResponseData(scanResponse)')]
    need(init and 'deviceNameDisplay' not in init and 'kDefaultMark' not in init,
         'the Bluetooth GAP or advertised name carries the "(default)" mark')
    need('deviceNameDisplay' not in text('System_ESPNow_Wire.h') and
         not re.search(r'putField\([^;]*(deviceNameDisplay|kDefaultMark|shown)', text('System_ESPNow.cpp')),
         'an ESP-NOW frame field carries the "(default)" mark')
    need('out["deviceNameSet"]  = SelfDevice::deviceNameSet();' in ble, 'bleinfo json has no deviceNameSet')
    need("device visible as 'HardwareOne'" not in ble, 'the advertising line still names a fixed HardwareOne')

    # The screens and text replies that show the name: one wording, no old ones.
    shown_in = {
        'System_DeviceName.cpp': ('SelfDevice::deviceNameDisplay(shown', 'Device name: (not set)'),
        'BLE_Server.cpp': ('SelfDevice::deviceNameDisplay(shown', None),
        'System_ESPNow.cpp': ('SelfDevice::deviceNameDisplay(shown', None),
        'System_Utils.cpp': ('SelfDevice::deviceNameDisplay(shown', None),
        'OLED_ESPNow.cpp': ('SelfDevice::deviceNameDisplay(name', 'gSettings.espnowDeviceName : "(none)"'),
        'G2_Page_Network.cpp': ('SelfDevice::deviceNameDisplay(shown', '"(unset)"'),
        'G2_Glasses.cpp': ('SelfDevice::deviceNameDisplay(name', ': "HardwareOne";'),
        'OLED_Mode_Bluetooth.cpp': ('SelfDevice::deviceNameSet()', None),
    }
    for name, (must, gone) in shown_in.items():
        body = text(name)
        need(must in body, f'{name} does not show the name through the one wording ({must})')
        if gone:
            need(gone not in body, f'{name} still has the old unnamed wording {gone}')
    need('wizardDeviceNameIsDefault()' in wizard and
         'wizardDeviceNameIsDefault()' in text('System_SetupWizardMode.cpp'),
         'the setup wizard does not mark the fallback name (default)')
    # The OLED wizard's value column holds twelve glyphs: the unnamed row shows
    # the mark alone, or "HardwareOne-B2C3 (default)" would be cut to "HardwareO...".
    oled_wizard = text('OLED_SetupWizard.cpp')
    rows = oled_wizard[oled_wizard.find('void renderWizardPage() {'):]
    rows = rows[:rows.find('\n}\n')]
    need('rowItem->type == WIZARD_ITEM_DEVICE_NAME && wizardDeviceNameIsDefault()' in rows and
         'devicename::kDefaultMark + 1' in rows and
         rows.find('devicename::kDefaultMark + 1') < rows.find('oledTextFieldDraw('),
         'the OLED setup wizard row cuts "(default)" off the fallback name')

    # JSON replies with the name carry deviceNameSet beside it.
    utils = text('System_Utils.cpp')
    info = utils[utils.find('void buildSystemInfoJson('):]
    info = info[:info.find('\n}\n')]
    need('doc["deviceName"]    = SelfDevice::deviceName();' in info and
         'doc["deviceNameSet"] = SelfDevice::deviceNameSet();' in info and
         'espnow["deviceNameSet"] = SelfDevice::deviceNameSet();' in info,
         'status json / /api/system do not carry deviceName and deviceNameSet')
    devinfo = espnow[espnow.find('const char* cmd_espnow_deviceinfo('):]
    devinfo = devinfo[:devinfo.find('\n}\n')]
    need('doc["deviceNameSet"] = SelfDevice::deviceNameSet();' in devinfo and 'doc["nameValid"]' in devinfo,
         'espnowdeviceinfo json does not carry deviceNameSet beside nameValid')

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
    need('return espnowPeerAliasRefusal(deviceName);' in espnow,
         'espnowpairsecure does not refuse a peer name that breaks the device-name rule')
    need('strcmp(wireName, "Unknown")' not in espnow and '"Unknown";\n      MeshPeerHealth' not in espnow,
         'TOPO still puts the "Unknown" placeholder on the air or reads it as no name')
    need('devicename::putField(f.partnerName, name);' in text('System_ESPNow_BondForward.cpp'),
         'FORWARD_LINK partnerName is not written with putField')
    need('devicename::getField(f.partnerName, sizeof(f.partnerName), shown)' in text('System_ESPNow_ForwardRx.cpp'),
         'FORWARD_LINK partnerName is not read with getField')

    # No reader for the names the device name replaced (no migration code for 1.0).
    for name, src in (('System_Settings.cpp', settings), ('System_DeviceName.cpp', setter)):
        need('bluetoothDeviceName' not in src and 'Legacy' not in src,
             f'{name} still reads or moves the old device-name keys')
    # setup()'s own calls (the earlier bootAutostartEspNow call is the deferred
    # hand-off, which runs after setup).
    loaded = boot.find('if (readSettingsJson()) {')
    need(0 <= loaded < boot.rfind('bootAutostartEspNow(sBootNetServices.espnow);') and
         loaded < boot.rfind('if (initBluetooth())'),
         'ESP-NOW or Bluetooth can start before the settings load')
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
