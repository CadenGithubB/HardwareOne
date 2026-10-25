"""hw1peer: a Mac "test phone" for the HardwareOne phone link v2.

The tool plays the Android app's role on the link: the P4 is the BLE central and
GATT client, this Mac is the peripheral and GATT server. Everything above the
radio is pure Python so it can be tested offline against the firmware-generated
fixtures in ``tools/ble_secure/fixtures``.

Modules (each one mirrors a firmware source byte for byte where the wire is
concerned):

``paircode``
    The pairing code (``BLE_PhonePairCode.h``), the app's passphrase entry rule
    and the pipe PSK (PBKDF2-HMAC-SHA256, "HW1-PIPE-v1", 100000 rounds).
``channel``
    The outer HW1-PIPE-v1 secure channel, phone (initiator) side
    (``BLE_PhoneSecureChannel.h`` is the P4 responder).
``inner``, ``records``, ``relay``, ``gatt_macos``, ``app``
    The inner protocol v2, console records, the optional TCP relay, the
    CoreBluetooth peripheral and the interactive tool.

Nothing is imported eagerly here: ``gatt_macos`` needs CoreBluetooth, which the
offline tests must never load.

Run the offline tests with ``.venv/bin/python -m hw1peer.tests``.
"""

__all__: list[str] = []
