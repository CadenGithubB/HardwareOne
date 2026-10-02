# Bluetooth SMP pairing diagnostics

Logging-only instrumentation of the Bluedroid Security Manager (SMP), used on
2026-09-27 to find why LE Secure Connections pairing failed its DHKey check on
the P4. It led to the TinyCrypt ECC fix described in `../SDK-FIX.md`.

- `SMP_DIAGNOSTICS.md` - what was instrumented and what the logs showed.
- `TINYCRYPT-REVIEW.md`, `tinycrypt-fix-provenance.json`,
  `tinycrypt-host-vector-review.json` - review of the upstream fix.
- `smp-diagnostic.patch`, `smp-diagnostic-manifest.json` - the instrumentation,
  applied to a copy of the IDF `bt` component, never to the shared SDK.
- `configure-p4-smpdiag.sh`, `build-p4-smpdiag.sh`, `build-s3-smpdiag.sh` and
  the `*-summary.json` / `firmware-*.json` files - how the diagnostic images
  were built. Their raw build and flash logs are archived outside the repository.
  The scripts expect the copied `bt` component under
  `experiments/p4_ble_roles/private/diagnostic-components/bt`, which is also
  archived; `SMP_DIAGNOSTICS.md` refers to their original `private/` paths.
