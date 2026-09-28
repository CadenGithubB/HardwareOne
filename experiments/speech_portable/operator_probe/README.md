# P4 QuartzNet pointwise operator probe

Standalone ESP-IDF experiment, built and measured on the P4 on 2026-09-28.
The root hardware task owns all builds and device access. This application is
not HardwareOne and does not recognize speech. See the measured results below.

It tests the dominant QuartzNet-5x5 convolution signature: 512 input channels,
512 output channels, kernel 1, stride 1, no padding, T800. One invocation performs
209,715,200 MACs. This signature occurs 17 times per 16 seconds of feature input
in the inspected checkpoint. Multiplying isolated operator timings is only a
screening estimate: other layers, frontend, residuals, data movement and real
quantized model accuracy remain unmeasured.

## Three paths and correctness

- Conv1D, runtime layout `[1,800,512]`, single-core.
- Equivalent Conv2D, layout `[1,800,1,512]`, single-core.
- The same Conv2D, explicit multi-core.

ESP-DL 3.3.12 maps 1D time to width; its height-split condition cannot split that
case. With 2D time as height, this pointwise case is eligible. AUTO mode does
not split a width-one tensor. No performance advantage is assumed.

All paths use identical deterministic synthetic input and correctly packed
weights in PSRAM. The counter-based integer mixer and both seeds are in the
source and printed in the log. Values lie in [-8,7]. Logical weights are
`W[output,input]`; the PIE int8 byte order is
`(output/16,input,output%16)`. The runtime's filter tensor shape is
`[1,512,512]` or `[1,1,512,512]`; the shape does not imply ordinary unpacked
filter storage. Both versions use the same packed bytes.

Input, weight and output exponents are all -4: represented real values are
`integer * 2^-4`; output requantization shifts the accumulator right 4 with
round-half-to-even and saturation to [-128,127]. This first probe has no bias,
activation or per-channel weight exponents; those are later model-export gates.

Each path gets one warm-up and three measured repetitions. Checksums run
outside timing and must agree across repetitions. Distinct pre-run output
sentinels help catch unexecuted kernels. A scalar int64 reference independently
regenerates canonical weights for 128 selected output elements, including the
core split and 16-channel boundaries. Final outputs are compared byte-for-byte
across all three paths. Timings are printed only after every correctness check
passes. A checksum alone is not treated as proof of equality.

The public API is `dl::module::Conv::run()` with borrowed `dl::TensorBase`
buffers. Its per-call dispatch/context overhead is included in timing; buffer
allocation, generation, delay, reference computation and checksums are outside.
The largest five data allocations total 1,900,544 bytes in PSRAM. The dual-core runtime also includes two 2048-byte worker stacks; these static
workers intentionally survive until reset. Heap free/minimum/largest-block
figures are printed before allocation, after each path and after data cleanup.
Three samples establish neither a p95 nor a sustained full-model benchmark.

## Reused toolchain and dependencies

Use the existing inspected ESP-IDF 5.5.5 installation. Supply the absolute
ESP-DL managed-component directory through `ESPDL_COMPONENT_DIR`. Its sibling
components must also already exist:

| Component | Version |
|---|---|
| `espressif__esp-dl` |3.3.12|
| `espressif__dl_fft` |0.7.0|
| `espressif__esp_new_jpeg` |1.0.2|

These versions match `../dependencies-p4.lock`. ESP-DL's manifest records
upstream commit `4f4efd7ff021a2c9dcafad4f1dab3da349b8bf4a`. CMake checks the
local manifest versions and disables the component manager: it does not fetch
or replace dependencies. Build-local symlinks supply the plain component names
required when the component manager is disabled. It compiles the existing runtime, not a
copied kernel. Review its source/checksums with the speech build if that local
component directory has changed.

The defaults mirror the current qualified P4 build: silicon minimum 3.1
(the attached board is 3.2), CPU 400 MHz, HEX PSRAM 200 MHz, flash 16 MiB/DIO 80 MHz,
and USB Serial/JTAG console. Dynamic frequency scaling is disabled. This
isolated probe disables the task watchdog so a slow single kernel can finish
and report; it is not a production configuration.

## Build and partition gate — hardware owner only

The source author did not build or access the device; the hardware owner built
and ran the recorded experiment below. A fresh, ignored build directory is
required for reproduction. After activating the existing IDF 5.5.5 environment, the build owner
can run this from the repository root, substituting inspected absolute paths:

```sh
idf.py -C experiments/speech_portable/operator_probe \
  -B experiments/speech_portable/private/operator-probe-build \
  -DESPDL_COMPONENT_DIR=/absolute/path/to/managed_components/espressif__esp-dl \
  -DP4_PARTITION_CSV=/absolute/path/to/the/current/P4/partition-table.csv \
  build
```

`P4_PARTITION_CSV` is mandatory. CMake copies its bytes unchanged into the build
directory, prints its SHA256, and rejects a stale sdkconfig selecting another
layout. The table offset is the existing 0x8000. Obtain the exact source CSV
from the qualified P4 build's project/configuration; do not substitute a newly
designed layout. Before any flash, compare the generated partition binary
with the qualified firmware's binary:

```sh
cmp experiments/speech_portable/private/build-p4/partition_table/partition-table.bin \
    experiments/speech_portable/private/operator-probe-build/partition_table/partition-table.bin
```

The hardware owner must also check the linked image fits the currently active
application partition and retain the qualified application for restoration.
Only the probe application binary may be flashed to that verified application
offset. Do not use a full-project flash command or flash the generated
bootloader/partition table. This source bundle provides no flashing script.

The application does not initialize or mount NVS/LittleFS, open user files,
write partitions, change OTA selection, start radios, or acquire audio. The
existing bootloader and user-data partitions stay in place. Output is USB
console only; after one run it prints `OPERATOR_PROBE_DONE` and returns. Reset
repeats the experiment.

## Reading the result

Require all three `CHECK passed=1`, identical checksums, zero scalar-reference
and path differences, and `OPERATOR_PROBE_DONE passed=1`. A correctness failure
suppresses all TIMING rows. Capture startup/configuration and heap rows too.

A median slower than approximately 941 ms per pointwise call means its 17 copies
alone exceed 16 seconds, before the other model work. A faster result only
justifies extending the experiment: first exercise per-channel scales, folded
bias, the longest depthwise kernel 87/dilation 2, residual Add, and the 29-channel
output tail; then export and calibrate real weights and measure whole-model
accuracy and memory. No local-transcription performance claim follows from
this synthetic probe alone.


## Measured P4 run — 2026-09-28

The standalone application compiled with ESP-IDF 5.5.5 and the inspected local
ESP-DL 3.3.12. The hardware owner verified the generated partition binary was
byte-identical to the qualified P4 build, flashed only the application at
`0x10000`, and verified its flash digest. The existing bootloader reported
silicon revision 3.2, CPU 400 MHz, 32 MiB PSRAM at 200 MHz, and 16 MiB DIO flash
at 80 MHz. It loaded the expected probe project successfully.

Every path passed all 128 independent scalar-reference samples. Outputs were
byte-identical across the three paths, with maximum difference zero; all
repetition checksums were `e8be081a`. The final record was
`OPERATOR_PROBE_DONE passed=1`, and no panic was captured.

| Path | Three measured runs (ms) | Median (ms) |
|---|---|---:|
| Native Conv1D, single-core | 88.245, 88.239, 88.247 | 88.245 |
| Equivalent Conv2D, single-core | 88.853, 88.840, 88.843 | 88.843 |
| Equivalent Conv2D, explicit multi-core | 58.784, 58.758, 58.767 | 58.767 |

Each path had one untimed warm-up. These are elapsed wall-clock times for one
209,715,200-MAC operator, including dispatch/context overhead. The measured
multi-core path was approximately 1.50 times faster than native Conv1D and
1.51 times faster than single-core Conv2D. This validates the layout comparison
for this shape, precision, input and weight pattern; it does not establish
that every QuartzNet operator will benefit from the same transformation.

Multiplying the medians by this signature's 17 occurrences gives 1.500 seconds
for native Conv1D or 0.999 seconds for multi-core Conv2D per 16 seconds of input
features. The multi-core projection divided by 16 seconds is an operator-only
screening ratio of 0.0624. This is an **arithmetic projection for the tested
operator only**, not measured whole-model inference or real-time factor. It passes the initial
throughput screening gate and justifies the next operator/export experiment.

Memory observations, in bytes:

| Measurement | Before buffers | After multi-core path | After buffer cleanup |
|---|---:|---:|---:|
| Free PSRAM | 33,551,868 | 31,651,304 | 33,551,868 |
| Largest free PSRAM block | 33,030,144 | 31,457,280 | 33,030,144 |
| Free internal RAM | 586,919 | 586,495 | 586,743 |
| Largest free internal block | 491,520 | 491,520 | 491,520 |

The observed PSRAM allocation delta was 1,900,564 bytes, matching the five
1,900,544-byte data buffers plus allocator overhead. Free PSRAM and its largest
block returned exactly to their initial values. Internal free RAM ended 176
bytes below its initial reading. Its reported low-watermark moved from
554,888 to 553,628 bytes, but these global watermarks include startup; they do
not isolate benchmark peak allocation. One run does not establish leak-free
repeated lifecycle behavior, and the persistent dual-core workers remain alive.

The next gate is per-channel scales/folded bias and the longest depthwise
kernel (512 channels, kernel 87, dilation 2), followed by the small real-weight
export and full-model quantization/accuracy checks. This run used no actual
speech-model weights, frontend, microphone, CTC decoder, BLE, camera or web
workload. It neither measures transcription accuracy nor removes the source
model's long right context and whole-utterance normalization requirement.

### Run provenance

All paths below are relative to `experiments/speech_portable/`; private logs
and binaries remain ignored and are not embedded in this document.

- Result: `private/run-20260928/p4-operator-v1-result.json`, 2,120 bytes,
  SHA256 `9ca7dde3ef3d07ee9f90fc1ae3f261d3b886a07216df787deb67aa09d155eddc`.
- Serial log: `private/run-20260928/p4-operator-v1-serial.log`, 5,918 bytes,
  SHA256 `3d7114d68c179615cfe90e1de08c65c986d2fbce37bf2e47e38d97d3f779a70e`.
- Flash/verification log: `private/run-20260928/p4-operator-v1-flash.log`.
- Application: `private/operator-probe-build/hw1_p4_operator_probe.bin`,
  1,343,040 bytes (`0x147e40`), SHA256
  `9d563a7cc4a7bdb37abfafc56463bc1015376c1774b371004ea8bee467076dac`.
- Partition binary: `private/operator-probe-build/partition_table/partition-table.bin`,
  3,072 bytes, byte-identical to
  `private/build-p4/partition_table/partition-table.bin`, SHA256
  `d6e3ea6ea3e74e8baf99c8fd5b70b9c96adbdf248ffca778474e70e17b64d7df`.

The root hardware task subsequently restored and flash-verified the v8
HardwareOne application, then passed the P4-only saved-settings check. See
[../RESULTS.md](../RESULTS.md); those separate records establish restoration,
not the probe capture itself.
