# Portable JPEG results — 2026-09-28

**Both physical probes passed (`JPEG_RESULT failures=0`).** The tested boards
were ESP32-P4 revision 3.2 at 400 MHz and XIAO ESP32-S3 revision 0.2 at 240 MHz.
Each ran the production HAL against 16 original synthetic JPEG fixtures. The
P4 had no display attached; these tests require only USB.

Both boards were restored to their **original HardwareOne applications** and
completed normal startup. Application backups and restoration were verified
against flash. The first 64 KiB, including bootloader, partition table and NVS,
remained byte-identical throughout the probe tests. Only the application
partition was flashed; the probe never initializes or writes the filesystem,
settings, radio or camera. Authenticated login was not repeated.

## Measured decoding performance

For the committed 320×240 4:2:0 image, after one discarded warmup and 11 samples:

| Board / backend | Median | Minimum–maximum |
|---|---:|---:|
| P4 software | 73.083 ms | 73.052–73.198 ms |
| P4 Auto, hardware selected | 18.639 ms | 18.622–18.657 ms |
| S3 Auto, software selected | 151.056 ms | 151.055–151.057 ms |

P4 acceleration reduced this image's decode latency by **3.92×**, including
header parsing, DMA buffer copies, engine setup/cleanup and the final full-range
colour conversion. This measures the decode call, not total application speed
or CPU utilization. Small/cold images do not uniformly benefit: engine startup
can cost more than a tiny software decode.

## Correctness and compatibility

- P4 decoded all 15 baseline fixtures; its 12 legacy-supported fixtures remained
  byte-identical in software. The three 4:2:0 fixtures also work after fixing the
  old undersized workspace. Progressive JPEG remains unsupported.
- S3's 14 legacy-supported fixtures remained byte-identical. Both the actual
  old converter and the new path reject grayscale and progressive input on this
  ROM decoder, confirming an existing limitation rather than a regression.
- P4 accelerated eight fixtures. Odd-sized, grayscale and too-narrow images
  selected software immediately. S3 always selected software in Auto mode.
- Hardware RGB output preserved channel order, orientation and cropped row
  stride. Against P4 software, every accelerated fixture had maximum channel
  error ≤4/255; patterned images had mean error approximately 1.13/255. Small
  outputs were independently reconstructed from complete pixel dumps, with
  hashes checked and a contact sheet inspected.
- Pixel acceptance allows maximum error 8 and mean error 2 for this corpus, to
  permit decoder rounding. The offline validator accepts the corrected results
  and rejects the first run's colour-range bug. This is a corpus qualification,
  not a guarantee for every possible JPEG.
- Valid repeated-marker padding, malformed/truncated input and output limits
  passed. Auto and HardwareOnly produced identical accelerated pixels.
- Each board completed 60 concurrent decodes on two cores, all matching the
  reference for the backend actually used. P4 selected hardware 50 times and
  software 10 times, exercising busy fallback; S3 used software all 60 times.
- Heap integrity passed. S3 retained no extra heap; P4 ended 20 bytes lower in
  total/internal free heap. One batch does not establish long-term leak freedom.
  Neither corrected run logged a watchdog, timeout, panic or assertion.

## Problems found and fixed by device testing

1. The P4's `FASTDECODE=1` software decoder needs more than esp_jpeg 1.3.1's
   default 3,100-byte pool. A private 6,080-byte workspace fixes standard 4:2:0
   input while preserving independent scratch for concurrent calls.
2. IDF's direct RGB path applies limited-range video colour coefficients to
   full-range JPEG data. The backend now requests YUV444 and converts it to
   full-range RGB in place. Entropy decoding, inverse DCT and chroma expansion
   remain accelerated, without allocating a second image buffer.
3. The pinned driver's fixed horizontal DMA blocks stall on narrow images.
   MCU-padded widths below 40 pixels for 4:4:4 or 32 for 4:2:2/4:2:0 now go
   directly to software.

The external decoder is qualified with RGB888 (`CONFIG_JD_FORMAT=0`) and default
Huffman injection disabled. The S3 retains its ROM configuration. Other decoder
configurations need their own qualification; no global SDK files were modified.

## Final builds and host checks

The corrected probes and both full HardwareOne applications built successfully.
The full P4 app retains its LCD/wheel/buttons, G2/R1, web and mesh profile. The S3
qualification build enables camera and Sense support so both live-camera decode
call sites compile. **These new full-app images were not flashed.** The boards
retain their previous firmware and feature profiles after the temporary probes.

Post-build source checks passed for 7,934 P4 and 7,924 S3 files. The S3 ELF has
both live-camera workers and shared decode, with no hardware JPEG symbol; P4 has
the hardware decoder. The installed SDK and verified private JPEG override are
unchanged. The override's 10 allocation/IRQ cleanup test methods and five
original-SDK negative controls remain qualified.

Host ASan/UBSan checks pass for dispatch, input bounds, normalization, allocator
policy, full-range conversion, padded stride, cleanup and contention. Actual
TJpgDec testing now covers **FASTDECODE 0, 1 and 2**, each with 15 baseline images
and 192 concurrent decodes (576 total). Every image supported by the old
converter remains byte-identical; larger private workspace repairs the fast
modes' former allocation failures. The final host manifest has 47 source hashes.

Firmware/source fingerprints are in `qualification.json`; measured cases,
legacy outcomes, timings, pixel analysis and restoration evidence are summarized
in `device-results.json`. Raw logs, pixel images and backups remain ignored in
`private/device-20260928/`.

## Remaining integration checks

This milestone qualifies shared JPEG **decoding** on both processors. It does
not test a live camera, G2 image delivery, a display, or JPEG encoding. P4 camera
bring-up remains separate and is disabled in its current full-app profile.
Before broader release, deploy the new full application and check stored-image
viewing plus S3 live-camera viewing through G2. Long-running mixed workloads and
any new independent 2D-DMA user (such as PPA) need additional qualification.
