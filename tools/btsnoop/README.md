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
