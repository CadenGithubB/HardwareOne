"""gatt and gatt_macos: the GATT boundary's pure rules, and the pyobjc layer's wiring.

CoreBluetooth itself cannot run here (macOS refuses Bluetooth to a process without the
permission; the real run is in Terminal.app), so ``gatt_macos`` is checked in a subprocess
that only imports it: pyobjc then builds the delegate class against the formal
``CBPeripheralManagerDelegate`` protocol, which catches a misspelt selector or a wrong
argument count, and every CoreBluetooth call the module makes is checked to exist. No
peripheral manager is created.
"""

import subprocess
import sys
import textwrap
from pathlib import Path

from hw1peer.gatt import (DEFAULT_LOCAL_NAME, MAX_PRIMARY_NAME, RX_UUID, SERVICE_UUID, TX_UUID, AttError,
                          WriteRequest, primary_name, triage_writes)
from hw1peer.inner import VALUE_CAP
from hw1peer.tests import Skip


def test_uuids_match_the_firmware():
    assert SERVICE_UUID == "b7e10000-6f7d-4b2e-a8c4-3d9f5021c6a0"
    assert RX_UUID == SERVICE_UUID.replace("b7e10000", "b7e10001")
    assert TX_UUID == SERVICE_UUID.replace("b7e10000", "b7e10002")
    # BLE_PhoneGatt.cpp's little-endian form: a0 c6 21 50 9f 3d c4 a8 2e 4b 7d 6f <c> 00 e1 b7
    little = bytes.fromhex(SERVICE_UUID.replace("-", ""))[::-1]
    assert little == bytes.fromhex("a0c621509f3dc4a82e4b7d6f0000e1b7")


def test_advertised_name_fits_beside_the_uuid():
    # 31 bytes of legacy advertising: flags 3 + UUID128 AD 18 + name AD (2 + n) <= 31.
    assert 3 + 18 + 2 + MAX_PRIMARY_NAME == 31
    assert len(DEFAULT_LOCAL_NAME.encode()) <= MAX_PRIMARY_NAME
    assert primary_name("") is None and primary_name(None) is None and primary_name("X") == "X"
    assert primary_name("HW1Peer!") == "HW1Peer!" and primary_name("HW1Peer!!") is None
    assert primary_name("ééééé") is None, "bytes, not characters"


def test_write_batches_are_all_or_nothing():
    rx = WriteRequest("c1", RX_UUID, 0, b"\x01")
    assert triage_writes([rx, rx]) == (AttError.SUCCESS, [rx, rx])
    assert triage_writes([]) == (AttError.SUCCESS, [])
    assert triage_writes([rx, WriteRequest("c1", TX_UUID, 0, b"")]) == (AttError.WRITE_NOT_PERMITTED, [])
    assert triage_writes([WriteRequest("c1", RX_UUID, 3, b"")]) == (AttError.INVALID_OFFSET, [])
    assert "value" not in repr(rx).split("(", 1)[1], "a write's bytes never reach a repr"


def test_att_codes_match_corebluetooth():
    assert (AttError.SUCCESS, AttError.WRITE_NOT_PERMITTED, AttError.REQUEST_NOT_SUPPORTED,
            AttError.INVALID_OFFSET) == (0, 3, 6, 7)
    assert VALUE_CAP == 512


IMPORT_CHECK = textwrap.dedent("""
    import CoreBluetooth as CB
    import hw1peer.gatt_macos as g
    from hw1peer.gatt import AttError
    d = g.HW1PeerPeripheralDelegate
    assert d.__name__ == "HW1PeerPeripheralDelegate" and not hasattr(g, "_Delegate")
    selectors = [b"peripheralManagerDidUpdateState:", b"peripheralManager:didAddService:error:",
                 b"peripheralManagerDidStartAdvertising:error:",
                 b"peripheralManager:central:didSubscribeToCharacteristic:",
                 b"peripheralManager:central:didUnsubscribeFromCharacteristic:",
                 b"peripheralManager:didReceiveWriteRequests:", b"peripheralManager:didReceiveReadRequest:",
                 b"peripheralManagerIsReadyToUpdateSubscribers:"]
    missing = [s for s in selectors if not d.instancesRespondToSelector_(s)]
    assert not missing, missing
    for cls, sel in [(CB.CBPeripheralManager, b"initWithDelegate:queue:options:"),
                     (CB.CBPeripheralManager, b"addService:"), (CB.CBPeripheralManager, b"removeAllServices"),
                     (CB.CBPeripheralManager, b"startAdvertising:"), (CB.CBPeripheralManager, b"stopAdvertising"),
                     (CB.CBPeripheralManager, b"updateValue:forCharacteristic:onSubscribedCentrals:"),
                     (CB.CBPeripheralManager, b"respondToRequest:withResult:"),
                     (CB.CBMutableCharacteristic, b"initWithType:properties:value:permissions:"),
                     (CB.CBMutableService, b"initWithType:primary:"), (CB.CBMutableService, b"setCharacteristics:"),
                     (CB.CBCentral, b"maximumUpdateValueLength"), (CB.CBATTRequest, b"value"),
                     (CB.CBATTRequest, b"offset"), (CB.CBATTRequest, b"central"),
                     (g.NSProcessInfo, b"beginActivityWithOptions:reason:"), (g.NSProcessInfo, b"endActivity:")]:
        assert cls.instancesRespondToSelector_(sel), sel
    assert d.peripheralManager_didReceiveWriteRequests_.signature == b"v@:@@"
    assert {AttError.SUCCESS: CB.CBATTErrorSuccess, AttError.REQUEST_NOT_SUPPORTED: CB.CBATTErrorRequestNotSupported,
            AttError.INVALID_OFFSET: CB.CBATTErrorInvalidOffset,
            AttError.WRITE_NOT_PERMITTED: CB.CBATTErrorWriteNotPermitted} == {a: int(a) for a in
            (AttError.SUCCESS, AttError.REQUEST_NOT_SUPPORTED, AttError.INVALID_OFFSET, AttError.WRITE_NOT_PERMITTED)}
    assert len(g._STATES) == 6
    print("gatt_macos ok")
""")


def test_gatt_macos_imports_and_its_selectors_exist():
    result = subprocess.run([sys.executable, "-c", IMPORT_CHECK], capture_output=True, text=True, timeout=120)
    assert result.returncode == 0 and "gatt_macos ok" in result.stdout, result.stderr[-2000:]


NAME_CLASH = textwrap.dedent("""
    import objc
    from Foundation import NSObject

    class _Delegate(NSObject):          # another module's generic delegate name
        pass
    import hw1peer.gatt_macos as g      # must not clash with it
    print("no clash", g.HW1PeerPeripheralDelegate.__name__)
""")


def test_the_delegate_class_name_cannot_clash():
    """Objective-C class names are global: a generic ``_Delegate`` elsewhere made the import fail."""
    result = subprocess.run([sys.executable, "-c", NAME_CLASH], capture_output=True, text=True, timeout=120)
    assert result.returncode == 0 and "no clash" in result.stdout, result.stderr[-2000:]


KEEP_AWAKE = textwrap.dedent("""
    import os, subprocess, time
    import hw1peer.gatt_macos as g

    def mine():
        out = subprocess.run(["/usr/bin/pmset", "-g", "assertions"], capture_output=True, text=True).stdout
        return [line for line in out.splitlines() if f"pid {os.getpid()}(" in line]
    end = g.hold_awake("hw1peer keep-awake test")
    held = mine()
    end()
    end()                              # a second end is harmless
    time.sleep(0.3)
    print("held:", held)
    print("after:", mine())
""")


def test_hold_awake_prevents_idle_sleep_for_the_run():
    """The NSProcessInfo activity shows in pmset as PreventUserIdleSystemSleep and goes with end()."""
    if not Path("/usr/bin/pmset").exists():
        raise Skip("no pmset")
    result = subprocess.run([sys.executable, "-c", KEEP_AWAKE], capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr[-2000:]
    held, after = result.stdout.splitlines()[-2:]
    assert "PreventUserIdleSystemSleep" in held and "hw1peer keep-awake test" in held, held
    assert after == "after: []", after


MAC_RESET = textwrap.dedent("""
    import CoreBluetooth as CB
    import hw1peer.gatt_macos as g

    class Manager:                      # a fake CBPeripheralManager: no Bluetooth needed
        def __init__(self):
            self.st, self.calls = CB.CBManagerStatePoweredOn, []
        def state(self): return self.st
        def removeAllServices(self): self.calls.append("removeAll")
        def addService_(self, service): self.calls.append("add")
        def startAdvertising_(self, data): self.calls.append("startAdv")
        def stopAdvertising(self): self.calls.append("stopAdv")

    class Events:
        def __init__(self): self.log = []
        def __getattr__(self, name):
            return lambda *args: self.log.append((name,) + args)

    p = g.MacPeripheral(name="HW1Peer", on_error=print)
    events, manager = Events(), Manager()
    p._events, p._manager = events, manager
    p.set_advertising(True)
    p.radio_changed()                                  # powered on: the service is added
    assert manager.calls == ["removeAll", "add"], manager.calls
    p.reset_service()                                  # an add in flight: ignored
    assert manager.calls == ["removeAll", "add"], manager.calls
    p.service_added("refused")                         # didAddService with an error
    assert ("on_service_added", "unknown error") in events.log, events.log
    p.reset_service()                                  # the retry
    assert manager.calls[-2:] == ["removeAll", "add"], manager.calls
    p.service_added(None)
    assert manager.calls[-1] == "startAdv"
    p.advertising_started(None)
    del manager.calls[:]
    p.reset_service()                                  # the kick: advertising stops, then remove + add
    assert manager.calls == ["stopAdv", "removeAll", "add"], manager.calls
    assert events.log[-1] == ("on_advertising", False, None), events.log[-1]
    p.service_added(None)
    manager.st = CB.CBManagerStatePoweredOff
    del manager.calls[:]
    p.reset_service()                                  # radio off: ignored
    assert manager.calls == [], manager.calls
    print("reset ok")
""")


def test_mac_peripheral_reset_service():
    """reset_service removes and re-adds the service (stopping advertising first) and is ignored
    while an add is in flight or the radio is off. Driven with a fake manager."""
    result = subprocess.run([sys.executable, "-c", MAC_RESET], capture_output=True, text=True, timeout=120)
    assert result.returncode == 0 and "reset ok" in result.stdout, (result.stdout + result.stderr)[-2000:]
