# ADC SDK compatibility

`prepare_sdk_adc.py` extracts the actual private ADC functions from
`System_Battery.cpp` and copies the platform-independent policy header into a
small IDF application. This isolates driver API compatibility from Arduino and
the full application's feature configuration. It performs no flash operation.

Pinned IDF 5.5.5 successfully compiled and linked:

- Classic ESP32: GPIO35, ADC1 channel7, 2:1 divider, line-fitting calibration.
- ESP32-S3: GPIO1, ADC1 channel0, hypothetical external 2:1 divider,
  curve-fitting calibration. This is not an onboard XIAO divider.

The P4 ADC path is compiled in the full HardwareOne integration build.
`SDK_ADC.md` describes compile coverage only; neither standalone build was
flashed and these results do not establish analog accuracy.

After activating the pinned SDK, reproduce with:

```sh
python3 experiments/battery_portable/prepare_sdk_adc.py \
  --target esp32 --output experiments/battery_portable/private/sdk-adc-esp32
cd experiments/battery_portable/private/sdk-adc-esp32
idf.py -D IDF_TARGET=esp32 build
```

Repeat with `esp32s3` and a separate output directory. The generator writes an
input manifest containing source, extracted-backend and policy hashes. Public
build results and artifact hashes are in `sdk-adc-results.json`; build trees
and logs remain under ignored `private/`.
