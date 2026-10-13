# OpenClaw integration working status

The 0.99.98 checkpoint combines the committed 0.99.97.4 baseline, OpenClaw and
current P4 STT/G2 work on main. See [the consolidation inventory and checks](CONSOLIDATION_0.99.98.md).
Claude's unfinished firmware-review changes remain in its separate worktree.

XIAO live tests passed verified HTTPS chat to Mingus, a follow-up recalling
conversation context, and New chat clearing that context. The user has found
the device conversation in the OpenClaw dashboard. Those tests used the
0.99.97.4-based image; they are not a hardware test of the new checkpoint.

The Studio gateway retains its loopback listener and uses the restricted HTTPS
relay. Preserve its existing sandbox, exec and elevation settings. An unknown
channel makes sandbox explain report `allowedByConfig: false`; that alone does
not prove host-execution denial. Strict host-execution isolation is unverified.

The P4 `p4-openclaw` profile preserves the working local STT/model/speaker/G2
features and adds HTTP chat. Its original working source and current firmware
have been preserved. No P4 app was flashed during consolidation. The attached
P4's actual app slot is `0x715000`, not the older size in the qualification
notes. A preflight backup matches the known working ELF `d4e15f5bc`.

Next hardware work: confirm normal device login, configure the same test
network and relay with private credential entry, set the clock and public CA,
then test text chat before local STT → Stop → wait → review → Send. The first
post-restart login attempt failed; do not reset accounts or bypass normal
authentication. Relay credentials are not stored on the workstation. Keep both
VLANs separate and never use a gateway operator token on the device.
