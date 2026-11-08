# Security policy

Hardware One is maintained by one person in spare time. This page says how to report a
vulnerability privately, what to expect afterwards, and which security limits are already known and
documented, so you can decide where to deploy it.

## Supported versions

Only the most recent release on `main` gets security fixes. Fixes are not backported to older
releases. If you are on an older release, update first and check that the problem still happens.

| Version | Security fixes |
|---|---|
| Latest release on `main` | Yes |
| Older releases | No; update to the latest release |

## Reporting a vulnerability

**Do not open a public issue for a security problem.**

Open a new issue with the **Request a private security contact** form. It has no required
fields: leave out every detail of the problem, and the maintainer will arrange a private channel.

Please include:

- the firmware version (first line of `status`) and the board or deployment profile;
- which interface is involved (web UI/HTTP, BLE console, phone pipe, ESP-NOW mesh, MQTT, OTA,
  serial console, the recovery updater);
- the steps to reproduce, and what an attacker needs (same network, BLE range, physical access);
- any proof of concept, with passwords, Wi-Fi passphrases, tokens and keys removed.

### What to expect

This is a best-effort, single-maintainer project, so the following are aims, not guarantees:

- an acknowledgement within about two weeks;
- an assessment, and a fix or an agreed next step, after that, depending on severity and on
  whether the problem can be reproduced on hardware the maintainer has;
- credit in the release notes if you want it.

Please give a reasonable time for a fix before you publish details.

## Scope

In scope:

- the firmware in this repository (`main/`, `components/hardwareone/` and the other first-party
  components), including the web UI, the command system and its permission checks, BLE, ESP-NOW,
  MQTT and OTA;
- the recovery updater (`updater/`) and the OTA tooling (`tools/ota/`);
- the build and configuration tools under `tools/`, and the integrations under `integrations/`.

Out of scope:

- the limits listed under [Security posture](#security-posture) below, unless you find a way past
  them that is not described there;
- vulnerabilities in upstream ESP-IDF, arduino-esp32 or other third-party components, unless they
  are caused by HardwareOne's local patches or configuration (report upstream problems to the
  upstream project);
- attacks that need a modified firmware image to be flashed onto the device.

## Security posture

Hardware One is built for a **trusted local network**: your own Wi-Fi, your own phone and your own
paired devices. The limits below are known, and are part of the current design. They are listed
here so you can choose a deployment with them in mind.

### Network and web UI

- The web UI is served over plain **HTTP by default**. Over HTTP, logins and session cookies can be
  read by anyone who can see the traffic on your network.
- HTTPS exists only in builds compiled with `ENABLE_HTTPS=1`, where it is a setting
  (`httpsEnabled`, off by default, needs certificates and a reboot). It is **compiled out** of the
  `p4x_eye` board profile and of every deployment (`headless`, `pocket_assistant`, `handheld`).
  Those builds serve the web UI over HTTP only and cannot be switched to HTTPS.

### Credentials at rest and physical access

- **Flash encryption is enabled only on the `feathers3_fe` profile**, and only in a limited form.
  Every other board profile (FeatherS3, XIAO ESP32-S3, Feather ESP32 V2, QT Py ESP32, ESP32-P4X-EYE,
  Waveshare ESP32-P4) and every deployment builds with flash and NVS encryption off.
- `feathers3_fe` is **experimental**, and its first boot on a chip **burns eFuses and encrypts the
  flash in place, irreversibly**. It uses Development-mode flash encryption without secure boot.
  That stops a plain read-out of the flash, but the USB download mode stays open, so someone holding
  the board can flash their own code and use it to read the decrypted credentials. It does not
  protect a lost or stolen device.
- On the other boards, **anyone holding the device can read its stored credentials** by reading the
  flash over USB. `settings.json` is encrypted with a key derived from the chip's eFuse MAC and the
  flash chip's unique ID, and both can be read from the board itself. That ties the file to one
  device; it does not protect it against someone holding the board.
- On the native-Wi-Fi boards (ESP32-S3 and classic ESP32: FeatherS3, XIAO ESP32-S3, Feather ESP32
  V2, QT Py ESP32), the **Wi-Fi driver also stores the network passphrase in plaintext** in the
  `nvs` partition. Arduino's Wi-Fi layer runs with persistent storage on, and the firmware does not
  turn it off. On the ESP32-P4 boards Wi-Fi runs on the ESP32-C6 through ESP-Hosted, and the host
  side keeps Wi-Fi configuration in RAM only; the C6 side has not yet been checked with a flash
  dump.
- On every profile, `feathers3_fe` included, treat a lost or stolen device as a disclosed Wi-Fi
  passphrase and disclosed account passwords: change them.

### Bluetooth

- **There is no BLE link-layer security.** The firmware sets up no BLE pairing, bonding or link
  encryption, and the phone pipe declines link-layer pairing requests. Protection comes from the
  application layer instead:
  - The BLE console requires a login by default (`bluetoothRequireAuth`).
  - The BLE secure channel (`bleRequireSecureChannel`, on by default) encrypts traffic only once a
    passphrase has been set with `blesecret`. **Until then the BLE console, including its login,
    is plaintext.** The device prints a "Bluetooth is UNENCRYPTED" notice at boot in that state,
    and `bleinfo json` reports it as `secureChannelRequired`.
  - The BLE server does not start at boot unless you turn it on (`bluetoothAutoStart`, off by
    default).
- **Even Realities G2 glasses and the R1 ring are trusted by Bluetooth address, or by advert name
  when no address is saved.** After pairing, reconnection accepts only the saved address of each
  temple. When a side has no saved address, it is found by its advertised name pattern. A nearby
  device that copies a saved address, or that advertises a matching name before the address is
  saved, could be accepted as the glasses or ring. This has not been demonstrated on hardware.

If you need more than this (an untrusted network, untrusted people within Bluetooth range), use a
build that includes HTTPS and turn it on (where HTTPS is compiled out, the web UI stays plain HTTP),
set `blesecret` before you turn on Bluetooth, and keep the device on a network you control. No
profile protects the stored credentials of a device that is lost: change the passwords.
