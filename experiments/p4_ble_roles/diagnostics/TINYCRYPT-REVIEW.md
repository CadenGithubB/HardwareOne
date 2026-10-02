# Independent TinyCrypt backport review

Scope: the five-file `idf-tinycrypt-9fd7cb7.patch`, upstream commit
`9fd7cb7e606a06111e1b14be7f4e00d77d9cf3dd`, for the isolated IDF 5.5.5 build.
No application, SDK or diagnostic-component source was changed by this review.

**Recommendation: proceed with isolated hardware validation of the exact
upstream backport on both P4 and S3.** This review does not establish completed
accessory pairing or exhaustive cryptographic correctness.

## Provenance and intended change

Independently recomputed the filtered patch SHA-256 and all five patched-file
SHA-256 and Git blob IDs. Every value matches `tinycrypt-fix-provenance.json`
and the recorded official commit file identities. The omitted sixth change is
the NimBLE submodule pointer; these builds use Bluedroid and shared TinyCrypt.

P4 enables `SOC_ECC_SUPPORTED`; S3 does not. The patch routes ECDH and signing
through the established regularized software ladder, retaining optional random
initial-Z blinding for ECDH and temporary-secret cleanup. Supported canonical
public-key multiplication and point validation may still use the accelerator,
with explicit native-word/little-endian conversion and the existing hardware
lock and constant-time setting. Software point validation remains available
when hardware validation cannot establish validity.

The patch does not change SMP negotiation, authentication requirements, key
size, pairing acceptance, application authorization or the public-key checks
at the SMP call sites. No security-requirement bypass was found. The S3 normal
software path remains algorithmically equivalent; both targets can use one
patched SDK/component source.

## Host checks

Compiled actual baseline and patched `ecc.c` and `ecc_dh.c` with host Clang.
Used the RFC 5903 P-256 known-answer vector already supplied in the SDK's
mbedTLS `tests/suites/test_suite_ecdh.data`, rather than reproducing ECC math.
Both software builds passed both expected public keys, both directions of the
expected ECDH secret, valid-point acceptance and invalid-zero-Y rejection.

A third build selected the hardware-capable source branches while replacing
only the hardware adapter with a function that always reports failure. Its
ECDH tests also passed and made zero calls to that adapter. This verifies the
new ECDH software routing; it does not simulate or qualify the real accelerator
or its byte-conversion code. Results are in `tinycrypt-host-vector-review.json`.

## Confirmed upstream fallback limitation

When public-key hardware multiplication reports failure, `EccPoint_mult()`
falls through to the software ladder with the original canonical scalar and
fixed curve bit count. That ladder assumes a regularized leading bit. For the
small scalar 2, the fault-injected build returned success with the wrong public
point; both baseline and patched software builds returned the correct 2G.
The expected 2G was independently checked with OpenSSL SEC1 public-key
derivation. The RFC vector uses scalars with their high bit set and therefore
does not expose this failure path.

This is a real error-path robustness limitation, not evidence that the normal
P4 accelerator operation fails or that invalid SMP authentication is accepted.
ECDH no longer enters this fallback. Keep the upstream patch unchanged for the
current controlled test, record the limitation, and investigate upstream if
hardware multiplication errors occur. A separate scalar-1 check fails closed
in both existing and patched software; it is not a new S3 regression.

Remaining qualification belongs on the actual boards: P4 pairing, native S3
regression, and the relevant application tests. No serial or hardware access
was performed here.
