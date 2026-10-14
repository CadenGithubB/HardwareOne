# Passive OTA / HCI tooling (G2 + R1)

Read-only tools for Android `btsnoop_hci.log` captures and offline firmware
images. **Never MitM. Never send `otaStart` / DFU from Hardware One.**

Full workflow: [`docs/OTA_PASSIVE_CAPTURE.md`](../../docs/OTA_PASSIVE_CAPTURE.md)  
Research summary: [`docs/OTA_RESEARCH_FINDINGS_2026-07-31.md`](../../docs/OTA_RESEARCH_FINDINGS_2026-07-31.md)

## Tools

| Script | Purpose |
|---|---|
| `pull_hci.sh` | `adb bugreport` → `.scratch/btsnoop/<label>-<stamp>/` including `btsnoop_hci.log`, `.last`, and **combined** log |
| `ota_extract.py` | G2 `AA21` OTA/EFS reassembly + component rebuild; R1 frame **observe** |
| `evenota_split.py` | Split archived G2 `EVENOTA` CDN bins (no device) |
| `r1_dfu_extract.py` | Nordic Secure DFU reconstruct from HCI (init `.dat` + app `.bin`) |
| `r1_fw_analyze.py` | Decode init packet + mine app image (versions, BOM, symbols) |
| `terminal_inspect.py` | Strict offline SID `0x30` framing inspection; redacted by default, no Terminal enum/schema assumptions |

## Terminal protocol capture inspection

This inspects an existing log; it does not connect to glasses, run an agent or
implement Terminal Mode. See the current [Terminal plan](../../docs/G2_TERMINAL_WORK.md).
Unlike the OTA capture workflow below, this does **not** require a firmware update.
Python 3.9+ and its standard library are sufficient; no packages are installed.

```sh
python3 -B tools/btsnoop/terminal_inspect.py /path/to/btsnoop_hci.log

# List only Bluetooth log member names/sizes, without extracting a bugreport:
python3 -B tools/btsnoop/terminal_inspect.py /path/to/bugreport.zip --list-logs

# Select the exact member name reported above; only that log is read in memory:
python3 -B tools/btsnoop/terminal_inspect.py /path/to/bugreport.zip \
  --zip-member 'FS/data/misc/bluetooth/logs/btsnoop_hci.log'
```

Output includes connection/ATT handles, direction (`0` sent / `1` received),
fragment indexes, CRC verification, digests and top-level protobuf wire shapes.
Length-delimited values remain opaque; the tool does not guess submessages or
enum names. `btsnoop_timestamp_us` is the raw btsnoop timestamp, not Unix time.
Text and payload hex are omitted unless explicitly requested with
`--include-payload`. That option can expose the captured conversation; keep its
output private, for example in an owner-only directory with `umask 077`.

Supported framing is H4 btsnoop v1, fixed ATT channel `0x0004`, and G2
`AA21`/`AA12`. ACL assembly is separated by connection and direction; G2
assembly also separates ATT handle and sequence. All records must be complete;
drops, malformed lengths, missing starts/tails, fragment-order changes and bad
Terminal CRCs fail with no partial JSON. A rotated or abruptly ended log may
therefore need recapturing. Enhanced ATT and btsnooz text are not decoded.

Bounds are 64 MiB per log, 512 MiB per ZIP, 4,096 ZIP entries, a 2 MiB ZIP
central directory (checked before allocation), one million
records, 64 pending assemblies per layer, 65,535 bytes per G2 message and
10,000 Terminal messages. No archive is expanded onto disk. The tool writes
only to stdout/stderr; it never saves or changes the input. ZIP64, multidisk
archives and nonstandard concatenated layouts are unsupported.

Focused validation:

```sh
python3 -B tools/btsnoop/test_terminal_inspect.py
```

The four-case suite covers both fragmentation layers, independent connections,
bad/truncated input, redaction and bounded ZIP selection. A separate read-only
check accepted the archived Even app 2.2.7 settings capture: 724,110 bytes,
11,002 records, three SID `0x30` messages. This validates framing, not the newer
Terminal command meanings.

## Quick paths

```bash
# After Even app update succeeds (ESP32 disconnected):
./tools/btsnoop/pull_hci.sh g2-ota   # or r1-ota

# G2 — prefer combined log if the flash spanned a snoop rotation:
python3 tools/btsnoop/ota_extract.py \
  .scratch/btsnoop/g2-ota-*/btsnoop_hci.combined.log \
  -o .scratch/btsnoop/g2-ota-*/out

# R1 DFU — current log usually holds the DFU window; combined is fine too:
python3 tools/btsnoop/r1_dfu_extract.py \
  .scratch/btsnoop/r1-ota-*/btsnoop_hci.log \
  -o .scratch/btsnoop/r1-ota-*/dfu_out

python3 tools/btsnoop/r1_fw_analyze.py \
  --bin .scratch/btsnoop/r1-ota-*/dfu_out/r1_dfu_application.bin \
  --dat .scratch/btsnoop/r1-ota-*/dfu_out/r1_dfu_init.dat \
  -o .scratch/btsnoop/r1-ota-*/dfu_out
```

## Proven facts (2026-07-31 HCI)

- **G2:** SID `0xC0`/`0xC1` on DATA `…e0001` / notify `…e0002`; FILE_CHECK + 4 KB blocks; dual-arm pass.
- **R1:** `otaStart` → disconnect → Nordic Secure DFU (not SMP on the wire). Init hash = `SHA256(bin)[::-1]` (nrfutil LE). Page CRCs = zlib/IEEE cumulative.
- Android keeps two rotating files (`btsnoop_hci.log` + `.last`, default 65535 packets each). Always prefer the combined log for long flashes.
