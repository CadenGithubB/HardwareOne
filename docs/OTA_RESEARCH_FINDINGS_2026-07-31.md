# OTA research findings — 2026-07-31 (updated)

Community repos, firmware archives, FlutterApp-main, shipping `libapp.so`
(Even app 2.2.7), **and live HCI captures** of successful G2 + R1 updates.

Passive tooling: [`OTA_PASSIVE_CAPTURE.md`](OTA_PASSIVE_CAPTURE.md),
[`tools/btsnoop/`](../tools/btsnoop/).

---

## Bottom line

| Goal | Best source |
|---|---|
| Study **G2 firmware bytes** | Offline archive (`EVENOTA` bins) or HCI-rebuilt components |
| Study **how the phone flashes G2** | HCI snoop + `jimrandomh/g2flash` (validated vs capture) |
| Study **R1 firmware bytes** | HCI Secure DFU reconstruct (`r1_dfu_extract.py`) — archive only has BL/SD |
| Study **how the phone flashes R1** | HCI: `otaStart` → disconnect → Nordic Secure DFU |

**R1 uses Secure DFU on the wire (HCI-proven).** G2 uses `AA21` SID `0xC0`/`0xC1`.
They share nothing. Early notes that named SMP/`mcumgr_flutter` as the R1 upload
path were **not** what the 2026-07-31 capture showed.

---

## G2 — two flash transports

### A. BLE (official app / `g2flash`) — HCI validated

Documented in [`jimrandomh/g2flash`](https://github.com/jimrandomh/g2flash) and
confirmed on a live Even-app flash:

| Channel | UUID family | Role |
|---|---|---|
| CTRL / EvenHub | `…e5450` write `…e5401` | Heartbeat (`sid 0x80`) during transfer |
| DATA / firmware | `…e1001` write `…e0001` notify `…e0002` | OTA stream |

| SID | Body |
|---|---|
| `0xC0` | `0x00` BEGIN, `0x01` FILE_CHECK (+128 B EVENOTA subheader), `0x02` DATA_MARKER, `0x03` END |
| `0xC1` | **4096-byte** payload blocks (multi-frag; CRC-16 on last frag) |

Arms flashed **one at a time** (second BEGIN after first arm’s components).
Per-component CRC32C (MSB-first, init 0, xorout 0) checked on END.

Also: case **pogo / UART** path in `am-guru/evenRealities-webflasher` — not BLE.

### B. Image format — `EVENOTA`

CDN: `https://cdn.evenreal.co/firmware/<md5>.bin` (content-addressed, not enumerable).

Local archives:

- `/Users/morgan/even-realities-firmware-archive`
- `/Users/morgan/g2-firmware-archive` (2.0.1.14 → 2.2.6.10)

```
EVENOTA\0
u32LE component_count          @ 0x08
… date / time / version strings …
TOC[i] @ 0x40 + i*16:  eid, file_off, size, crc32c
At file_off: 128 B subheader + payload
```

Typical components: `ota/s200_firmware_ota.bin`, `ota/s200_bootloader.bin`,
`firmware/{touch,codec,ble_em9305,box}.bin`.

Tools: `evenota_split.py` (archive), `ota_extract.py` (HCI rebuild).

### Shipping app / other trees

`libapp.so` 2.2.7: `EvenOTAUpgrade*`, `EVENOTA`, `FirmwareUpdateManager`, CDN host,
`/v2/g/check_latest_firmware`. FlutterApp-main only has `ota_transmit` enums.

---

## R1 — Nordic Secure DFU (HCI-proven)

- **Not** `EVENOTA`. Package = Nordic DFU zip (`manifest` + `.bin` + signed `.dat`).
- Session: `otaStart` (subCmd `0x09`) on normal R1 chars → **ACL disconnect** →
  reconnect → Secure DFU control/packet ATT (Create / Select / Set PRN / Calc CRC /
  Execute). Init object (type=1) then data pages (type=2, typically 4096 B).
- Device `CALC_CRC`: zlib/IEEE CRC32; **data CRCs are cumulative** over the image.
- Init `hash`: `SHA256(bin)[::-1]` (nrfutil little-endian digest) — verified on
  recovered app and on archive BL/SD packages.
- `otaStart` / DFU writes: **observe-only** from our stack (brick hazard).
- Older APKs bundled `B210_*.zip` (BL/SD); app 2.2.6+ zeroed assets (runtime download).
  Application image recovered from HCI via `r1_dfu_extract.py` + `r1_fw_analyze.py`.

### R1 application image (HCI-recovered)

| Field | Value |
|---|---|
| Version | `2.2.7.0005` (+ `603MV1.9.3`) |
| Product path | `product/B210/app/_build/B210_Application` |
| Size | 649 376 B |
| Vector | SP `0x2003bed0`, Reset `0x00027489` (nRF52-class) |
| Init | `SignedCommand` / APPLICATION; `fw_version=3`, `hw_version=52`, `sd_req=[0x0100]`, ECDSA-P256, `boot_validation=VALIDATE_GENERATED_CRC` |
| Page CRCs | 160/160 vs device |
| Init SHA-256 | matches `SHA256(image)[::-1]` |

**BOM / stacks (strings):** IQS7211E, YHM2710, BMA456, Goodix Gh3x2x, GoMore,
`sd_ble_*`, `THREAD_BLE_EVT_DFU_START`, glasses bind (`ADV_START`, `glasses_mac_addr`,
`RING_CTRL_ADV_SUBCMD`).

### Path toward a custom R1 image

Understood offline: recover bytes, package format, hash rule, transport shape,
init field constraints (`hw_version=52`, `sd_req`, LE-SHA256, ECDSA over init command).

**Hard gate to flash your own bin on a stock ring:** bootloader public key must
accept the init signature. Without Even’s private key (or a replaced bootloader
you key yourself), stock DFU rejects self-signed packages. Near-term: RE the
recovered image; keep packaging tools honest; never auto-send `otaStart`/DFU.

---

## Implications for tooling

1. G2: reassemble `AA21` + `0xC0`/`0xC1` on DATA handles; rebuild FILE_CHECK components.
2. Always pull **both** snoop files and combine (`.last` then current) — long flashes rotate.
3. R1: `r1_dfu_extract.py` after `otaStart` window; verify CRC + LE-SHA256.
4. No MitM / dual-connect during live updates.
5. Archive G2 bins: `evenota_split.py` without updating a device.

---

## G2 vs R1 (quick)

| | R1 | G2 |
|---|---|---|
| Transport | Nordic **Secure DFU** after `otaStart` | `AA21` SID `0xC0`/`0xC1` on `…e1001` |
| Package | DFU zip + ECDSA init | `EVENOTA` multi-component |
| Hash / CRC | zlib page CRC; SHA256 LE in init | CRC32C per component on END |
| In G2 HCI? | Only as side traffic | Primary |
| In R1 HCI? | Primary | Glasses may still chatter |

---

## References

- `/Users/morgan/even-realities-firmware-archive/docs/{FINDINGS,G2_FIRMWARE,R1_FIRMWARE,FLASHING}.md`
- https://github.com/jimrandomh/g2flash
- https://github.com/NordicSemiconductor/pc-nrfutil (`package.py` LE hash)
- Shipping `libapp.so` (com.even.sg 2.2.7)
- Live captures (working): `.scratch/btsnoop/g2-ota-*`, `.scratch/btsnoop/r1-ota-*`
- Archived copy: `/Users/morgan/even-realities-firmware-archive/captures/hci-2026-07-31/`
- R1 app package: `…/firmware/r1/hci-recovered-2.2.7.0005/`
