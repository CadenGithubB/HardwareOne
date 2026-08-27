# HardwareOne OTA production-readiness implementation plan

Status: prospective engineering design with the host-only immediate work package
implemented. No device-execution, fault-injection, or hardware result should be
inferred from that tooling milestone.

Last reviewed against source: 2026-08-04.

## Executive decision

The recovery OTA architecture is functional enough to justify hardening rather
than replacement. It already uses ESP-IDF's native signed-image, OTA partition,
boot-selection, and rollback facilities; it does not depend on Arduino OTA.
The direct recovery path has meaningful hardware evidence. The remaining work
is principally qualification, production isolation of test facilities, and
closing several deliberately visible policy and operational gaps.

The recommended next work is:

1. Promote the temporary hardware runner into a maintained, restartable
   qualification tool with USB-serial evidence collection.
2. Qualify the existing staged-file path without changing production firmware.
3. Add deterministic interruption points to lab-only builds and enforce that
   release artifacts cannot contain them.
4. Use those facilities to close the reset, power-loss, watchdog, probation,
   and cleanup rows on the plain FeatherS3.
5. Resolve the credential-storage decision and hardware-test backup/restore.
6. Add the smaller release-engineering safeguards, then repeat the complete
   matrix on the flash-encrypted board.

This order obtains new evidence early, minimizes simultaneous changes, and
keeps fault-injection code out of the production path until the ordinary staged
workflow has been shown to work as written.

## Document map

- [Current baseline](#current-baseline)
- [Implementation checkpoint](#implementation-checkpoint-2026-08-04)
- [Scope](#scope)
- [Non-negotiable safety invariants](#non-negotiable-safety-invariants)
- [Proposed end-state architecture](#proposed-end-state-architecture)
- [Change inventory and downstream impacts](#change-inventory-and-downstream-impacts)
- [Q1: Maintained OTA qualification runner](#q1-maintained-ota-qualification-runner)
- [Q2: USB-serial observer and structured lab events](#q2-usb-serial-observer-and-structured-lab-events)
- [Q3: Staged-file qualification suite](#q3-staged-file-qualification-suite)
- [Q4: Negative artifact fixture generator](#q4-negative-artifact-fixture-generator)
- [Q5: Deterministic interruption framework](#q5-deterministic-interruption-framework)
- [Q6: External reset and power control](#q6-external-reset-and-power-control)
- [Q7: Evidence and release ledger](#q7-evidence-and-release-ledger)
- [S1: Recovery credential hardening](#s1-recovery-credential-hardening)
- [S2: Direct HTTP-handler watchdog decision](#s2-direct-http-handler-watchdog-decision)
- [O1: Hardware backup/restore qualification](#o1-hardware-backuprestore-qualification)
- [O2: Dedicated production-updater cable refresh target](#o2-dedicated-production-updater-cable-refresh-target)
- [O3: Signing-key rotation and artifact archival](#o3-signing-key-rotation-and-artifact-archival)
- [O4: Durable multi-result history](#o4-durable-multi-result-history)
- [O5: ESP-SR model-partition migration](#o5-esp-sr-model-partition-migration)
- [O6: Flash-encrypted-board parity](#o6-flash-encrypted-board-parity)
- [Deferred architectural changes](#deferred-architectural-changes)
- [Detailed file impact map](#detailed-file-impact-map)
- [Cross-cutting downstream impacts](#cross-cutting-downstream-impacts)
- [Implementation sequence](#implementation-sequence)
- [Verification strategy](#verification-strategy)
- [Definition of production-ready](#definition-of-production-ready-for-the-current-architecture)
- [Rollout and rollback](#rollout-and-rollback-of-these-changes)
- [Open decisions](#open-decisions)
- [Recommended immediate work package](#recommended-immediate-work-package)

## Relationship to the current documentation

`docs/OTA_RECOVERY_UPDATER.md` is the operator guide and current qualification
record. It describes behavior that exists today and is the place where completed
hardware evidence belongs.

`docs/OTA_RECOVERY_UPDATER_PLAN.md` records earlier design thinking. It is not
the source of truth for the implementation.

This document is the prospective change plan. It intentionally lives under
`docs/`, not `docs2/`. The private `docs2/` knowledge base describes the
as-built system and must not be made to claim that planned behavior already
exists. When an implementation phase lands, every affected current-behavior
document must be updated and accepted through the `docs2` review workflow.

Current firmware source remains authoritative if this plan and the source ever
disagree.

## Implementation checkpoint: 2026-08-04

The first host-only package is implemented under `tools/ota/`:

- The authenticated main-device HTTP client is shared by backup/restore and
  qualification tooling without changing the backup CLI.
- `hardware_qualification.py` lists and renders STG-001 through STG-018,
  verifies manifest/image identity, optionally runs the paired-build audit,
  probes explicitly selected serial/ADB interfaces, and creates/resumes
  sanitized evidence checkpoints.
- Host-socket and selected-Pixel/ADB raw recovery transports, recovery HTTP
  parsing, and exclusive timestamped serial observation are reusable modules.
- `make_test_fixtures.py` creates 16 deterministic lab-signed pairings using the
  canonical manifest serializer, including the detached-valid/native-invalid
  case.
- The host suite contains 26 passing tests: the original 14 backup/restore
  contracts and 12 qualification/fixture/transport/checkpoint tests.

The runner is deliberately non-destructive at this checkpoint. It has no path
that uploads OTA files, executes `otastage`/`otaupdate`, acknowledges a result,
reboots, or flashes a device. `init-run` records `dryRunOnly=true` and
`destructiveExecutorPresent=false`. This preserves the review boundary promised
by the immediate work package; staged hardware execution is the next change.

Exact usage and security constraints are in `tools/ota/README.md`.

## Current baseline

The present design has these important properties:

- A factory ESP-IDF updater and a single large `ota_0` main application slot.
- Detached RSA-3072/PSS/SHA-256 manifests plus ESP-IDF signed-app validation.
- Board, layout, project, size, digest, version, minimum-updater, and data-schema
  binding.
- A duplicated, CRC-protected OTA journal in NVS and a one-slot durable result.
- Staged LittleFS updates and direct recovery uploads.
- Main-image probation, ESP-IDF rollback state, early-crash escape, and recovery
  holding behavior.
- Guarded paired builds and guarded cable migration targets.
- Recovery networking over a WPA2 SoftAP with authenticated HTTP.
- No updater self-update, no second main slot, no general data migration engine,
  no key rotation protocol, and no anti-rollback fuse policy.

Hardware testing on a disposable plain FeatherS3 has already covered direct
upload, authentication, several invalid artifact classes, interrupted direct
transfer, probation, rollback after an unexpected reset, repair, result
acknowledgement, and journal reset. The staged path and deterministic
millisecond-scale interruption boundaries are not yet qualified.

## Scope

### In scope

- A maintained device qualification runner.
- Staged-path happy, negative, interruption, resume, probation, acknowledgement,
  and cleanup coverage.
- Structured evidence from the main application, recovery updater, USB serial,
  HTTP clients, and an optional external power controller.
- Lab-only deterministic pause/reset/fault points.
- Release gates that reject test instrumentation.
- Watchdog qualification and any narrowly justified watchdog correction.
- Recovery-credential storage policy and implementation options.
- Hardware backup/restore qualification.
- A narrowly guarded production-updater refresh target.
- Artifact retention, downgrade, signing-key rotation, and release-record policy.
- ESP-SR model-partition migration design.
- Durable result-history policy.
- Flash-encrypted-board parity.

### Not in the first implementation wave

- Replacing ESP-IDF OTA with Arduino OTA.
- Adding cloud pull, GitHub pull, BLE, ESP-NOW, or infrastructure-Wi-Fi recovery.
- Adding TLS to the isolated recovery AP.
- Updating the factory updater, bootloader, or partition table over the air.
- Adding a second full main application slot.
- Enabling Secure Boot or irreversible anti-rollback eFuses.
- Building a fleet-management service.

Those are legitimate later architecture choices, but they increase the risk
surface and do not block qualification of the current recovery design.

Implementation update (2026-08-04): the offline Android companion now includes
an encrypted BLE **main-application staging transport**. This does not add BLE
to factory recovery and does not change the signed manifest, journal, recovery
writer, or probation design described by this plan. The transport accepts only
candidate/manifest members from a live encrypted superadmin session, binds
exact length and SHA-256 at begin, carries raw chunks with explicit offsets,
checkpoints accepted offsets in bounded bursts, and deletes an abandoned
active partial after disconnect/timeout. Production qualification must add
throughput, frame-loss resynchronization, disconnect cleanup, foreground/
background Android behavior, multi-client exclusion, and full staged-update
evidence before treating the phone path as release-qualified.

## Non-negotiable safety invariants

Every change in this plan must preserve these rules:

1. No unverified application image may execute.
2. Board, partition layout, project, artifact digest, and native app signature
   must be checked before a candidate becomes bootable.
3. Failure before a destructive write must leave the installed main selectable.
4. Failure after destructive write begins must retain authenticated recovery,
   never guess that a partial main is safe.
5. A trial image must not be marked valid until its full probation contract has
   passed.
6. NVS and LittleFS init failures must not trigger an implicit erase or format.
7. Production builds must contain no remotely armable fault-injection surface.
8. Secrets, credentials, private signing keys, user data, and raw device captures
   must not enter the repository or `docs2/`.
9. Test actions must bind to an explicitly selected disposable device, serial
   port, board ID, layout ID, and artifact pair.
10. Test resumption must first rediscover durable device state; it must not trust
    the runner's previous in-memory assumption.
11. No test may silently acknowledge or overwrite a pending terminal result.
12. Flash-encrypted and non-encrypted devices must use separate, explicitly
    selected build and flash workflows.

## Proposed end-state architecture

```text
 signed main + manifest     qualification scenario
          |                          |
          v                          v
  paired-artifact audit ---> evidence/run controller
                                  /    |      \
                                 /     |       \
                       main HTTP/CLI   |    recovery HTTP
                               |       |           |
                               v       v           v
                            main app  USB serial  factory updater
                               |       |           |
                               +-------+-----------+
                                       |
                              optional reset/power rig
                                       |
                               sanitized run evidence
```

The runner controls the scenario but never becomes the authority on device
state. The journal, bootloader state, authenticated status responses, artifact
hashes, and observed boot identity are the authorities. Serial messages provide
timing and diagnosis; they do not replace durable status checks.

## Change inventory and downstream impacts

The following table is the high-level backlog. Detailed designs follow it.

| ID | Potential change | Priority | Shipped firmware changes? | Primary downstream impacts |
|---|---|---:|---|---|
| Q1 | Maintained OTA qualification runner | P0 | No, initially | New tool contract, test fixtures, operator workflow, evidence format, CI unit tests |
| Q2 | USB-serial observer and structured lab events | P0 | Optional; lab build only for structured events | Serial ownership, log schema, timing evidence, runner dependencies |
| Q3 | Staged-file qualification suite | P0 | No for first pass | LittleFS capacity, upload API load, journal/result cleanup, much longer hardware matrix |
| Q4 | Negative artifact fixture generator | P0 | No | Test-key custody, deterministic fixture metadata, manifest tooling tests |
| Q5 | Deterministic interruption framework | P0 after Q3 | Lab builds only | Main/updater control flow, NVS test state, WDT behavior, build gates, release audits |
| Q6 | External reset/power-controller adapter | P1 | No | Lab hardware, USB topology, recovery timing, safety interlocks |
| Q7 | Release artifact and evidence ledger | P1 | No | Release procedure, storage/retention, downgrade availability, reproducibility |
| S1 | Recovery credential hardening | P1 decision | Yes if changed | NVS schema, serial provisioning, HTTP auth, migration, backup expectations, FE parity |
| S2 | Direct HTTP-handler watchdog decision | P1 | Possibly | Recovery networking/task model, timeout behavior, forced-stall tests, RAM/task resources |
| O1 | Hardware backup/restore qualification | P1 | Usually no | Files API, filesystem quiescence, user database ordering, migration runbook |
| O2 | Dedicated production-updater cable refresh target | P1 | Build system only | Flash safety, operator workflow, pair audit, recovery repair procedure |
| O3 | Signing-key rotation and artifact archival policy | P1 policy | Later, if rotation protocol added | Key custody, updater/main compatibility, recovery inventory, downgrade process |
| O4 | Multi-result history decision | P2 | Yes if implemented | NVS use, journal protocol, status JSON, CLI/UI, acknowledgement semantics |
| O5 | ESP-SR model-partition migration | P2 before SR support | Build/migration tooling and possibly firmware | Partition data, backup size, cable duration, validation, rollback limits |
| O6 | Flash-encrypted-board parity | Release gate | No new feature expected | Encrypted flashing, NVS state, test duration, artifact separation |
| A1 | Factory updater OTA/self-update | Deferred | Major | New trust chain, power-loss recovery, second recovery slot or ROM-assisted repair |
| A2 | Second main application slot | Deferred | Major | Partition layout, LittleFS capacity, migration, rollback semantics, artifact sizing |
| A3 | Secure Boot and hardware anti-rollback | Deferred policy | Major and irreversible | Manufacturing, signing, recovery, downgrade, RMA, development workflow |
| A4 | Additional recovery transports/TLS | Deferred | Major | Attack surface, memory, certificate/key lifecycle, network UX, testing matrix |

## Q1: Maintained OTA qualification runner

### Purpose

Replace the temporary one-off phone and shell scripts with a repository-owned,
bounded, restartable runner that can exercise both staged and direct paths and
produce a reviewable result without relying on the Codex session remaining
connected.

### Proposed layout

```text
tools/ota/
  hardware_qualification.py       command entry point
  qualification/
    artifacts.py                  pair audit and fixture identity
    device_http.py                authenticated main HTTP/CLI client
    recovery_http.py              recovery protocol client
    serial_observer.py            USB serial capture and event matching
    transports.py                 host and Android/ADB transports
    power_control.py              explicit supported lab controllers
    scenarios.py                  scenario state machines
    evidence.py                   redaction and result bundle writer
    model.py                      typed statuses, checkpoints, and errors
  tests/
    test_qualification_*.py       host-only contract tests
```

Exact names can change, but responsibilities should remain separated. The
scenario engine must not contain raw ADB, serial, or HTTP string manipulation.

### Command model

Suggested commands:

```text
hardware_qualification.py preflight ...
hardware_qualification.py staged-negative ...
hardware_qualification.py staged-happy ...
hardware_qualification.py direct-negative ...
hardware_qualification.py interrupt ...
hardware_qualification.py probation ...
hardware_qualification.py cleanup ...
hardware_qualification.py resume --run <run-id>
hardware_qualification.py run --profile plain-staged-core ...
```

Each destructive command must require all of the following:

- Explicit serial port or unambiguous USB device identity.
- Expected chip and board ID.
- Expected layout ID.
- Main binary and manifest paths.
- Successful current paired-artifact audit.
- A `--disposable-device` acknowledgement.
- Confirmation that a pending result was reviewed, not silently discarded.

The runner should accept credentials through an interactive prompt, a protected
file descriptor, or an environment variable. It must never echo them, include
them in command lines recorded as evidence, or write them to disk.

### State and resume model

Every scenario is a state machine. A checkpoint contains only non-secret
information such as:

- Run ID and test-case ID.
- Expected board/layout and artifact hashes.
- Last completed runner action.
- Last observed journal phase and sequence.
- Last observed result sequence.
- Whether a reboot, AP transition, or operator network action is expected.
- Evidence file offsets.

On resume, the runner must:

1. Reopen the explicitly selected serial device.
2. Rediscover whether main or recovery is running.
3. Query authenticated status where a transport is available.
4. Compare durable candidate identity and sequences with the checkpoint.
5. Stop as `INCONCLUSIVE` on a mismatch rather than trying to repair
   automatically.

This prevents a stale host checkpoint from sending a destructive action to the
wrong board or transaction.

### Outcome vocabulary

Every case must end in exactly one of:

- `PASS`: the required durable end state and all required intermediate evidence
  were observed.
- `FAIL`: the device produced an end state forbidden by the test contract.
- `INCONCLUSIVE`: transport or evidence was lost before the contract could be
  established.
- `SKIP`: a declared prerequisite was unavailable before mutation began.
- `PROBATION_PENDING`: a successful install is plausible, but main probation
  and its durable terminal result have not yet completed.

`PROBATION_PENDING` is resumable, never a pass.

### Existing code reuse

`tools/ota/device_backup.py` already contains an authenticated `DeviceClient`
for login, file listing, file read, and file upload. The recommended change is
to extract the generic client into a shared module and leave a compatibility
import in the backup tool. The existing 14 backup contract tests must remain
green throughout the extraction.

The runner should invoke or import `tools/ota/check_ota_builds.py` rather than
reimplementing paired-artifact rules. Manifest construction should continue to
use the canonical manifest serializer and signer.

### Downstream impacts

- `tools/ota/device_backup.py`: import movement; its CLI and output must remain
  compatible.
- `tools/ota/tests/`: new HTTP, transport, redaction, resume, and scenario tests.
- Python environment: serial observation requires `pyserial`; ADB is optional.
  The tool must fail with an actionable prerequisite message and must not
  auto-install packages.
- Operator workflow: tests become explicit commands with run IDs instead of
  ad hoc shell sessions.
- CI: host-only tests can run without hardware; hardware profiles remain manual
  or lab-triggered.
- Security: a centralized redactor becomes responsible for keeping credentials,
  cookies, authorization headers, and signing-key paths out of evidence.

### Acceptance criteria

- Killing and restarting the runner during every wait state resumes safely.
- An ambiguous serial selection refuses to run.
- Wrong board/layout/artifact identity refuses before any device mutation.
- Authentication secrets are absent from generated files and process output.
- Existing backup tests and pair audits remain green.
- A complete no-fault staged run can be executed without editing the tool.

## Q2: USB-serial observer and structured lab events

### Why serial is required

When the test client joins the recovery SoftAP, the controlling Mac may lose
its internet connection. HTTP responses can also disappear during an immediate
reboot. USB serial provides an independent local observation channel and lets
the runner continue without depending on the interactive Codex connection.

Serial is evidence, not the only authority. The runner still checks durable
journal state and boot identity after reconnection.

### Phase 1: consume current output

Initially, record timestamped raw serial lines and recognize only a small set of
stable existing events:

- Main boot identity and version.
- Factory updater boot identity and version.
- Recovery AP ready.
- Apply start and completion/failure.
- Trial boot and probation completion.
- Reboot reason where available.

Unrecognized output is retained in the local run bundle but does not alter the
scenario state.

### Phase 2: lab-only structured events

If text parsing proves ambiguous, test builds may emit one-line records with a
stable prefix:

```text
HW1TEST {"v":1,"component":"updater","event":"image_verified","seq":42}
```

Rules for this event stream:

- It is compiled only when the lab fault-injection option is enabled.
- It contains no credentials, tokens, cookies, file contents, or keys.
- It reports phase and sequence identifiers already safe to expose on a
  physically attached serial console.
- Events are versioned and have host parser tests.
- Absence of an event cannot make a test pass.

### Serial ownership

Only one process may own the port. The qualification runner should multiplex
capture, commands, reset-line control, and evidence rather than launching a
second monitor. If an external terminal already holds the port, preflight must
fail before mutation.

### Downstream impacts

- Main/updater logging: no production change in phase 1; lab-only code in phase
  2.
- USB reset behavior: opening a serial port must not inadvertently toggle
  DTR/RTS and reboot the board unless that action is explicitly requested.
- Evidence size: raw serial needs a retention cap and rotation.
- Timing claims: host monotonic timestamps must be distinguished from device
  uptime values.

## Q3: Staged-file qualification suite

### Existing staged transaction

The main application currently expects:

```text
/system/ota/candidate.part
/system/ota/manifest.part
```

`otastage confirm` validates the pair, removes old promoted files, and performs
two separate renames to:

```text
/system/ota/candidate.bin
/system/ota/manifest.json
```

It then revalidates the promoted pair and begins a staged journal transaction.
`otaupdate` revalidates again, checks power policy, arms factory recovery, and
reboots. Recovery reopens the staged pair read-only, checks it against the
journal, writes or verifies `ota_0`, and completes boot selection. Successful
probation removes promoted staged files.

The two renames are intentionally not described as an atomic filesystem
transaction. A reset between them must produce a rejected/incomplete pair, not
a guessed pairing.

### Upload behavior

The first runner should reuse the authenticated file API. This produces URL and
base64 expansion and therefore requires more transient host and server work
than the raw image size suggests. Preflight must check:

- Candidate and manifest sizes.
- Current LittleFS free space.
- Whether older `.part` or promoted files exist.
- Whether a pending OTA result or active transaction blocks staging.
- The server's upload/free-space policy.

No new generic file-delete privilege should be added merely for testing. An
exact re-upload may replace incomplete `.part` files. Existing `otacancel`
should handle an eligible requested transaction. If field experience shows
that abandoned internal files cannot be recovered through existing operations,
a narrow, confirmation-gated `otacleanup` command may be designed later; it is
not part of the initial suite.

### Required P0 staged cases

| Test ID | Scenario | Required result |
|---|---|---|
| STG-001 | Candidate `.part` only | `otastage` refuses; installed main and journal remain safe |
| STG-002 | Manifest `.part` only | Refusal before recovery/write |
| STG-003 | Truncated candidate | Size/digest refusal before recovery/write |
| STG-004 | Truncated manifest | Parse/length/signature refusal before recovery/write |
| STG-005 | Valid manifest paired with another same-size image | Digest refusal before promotion/journal mutation |
| STG-006 | Invalid manifest signature | Refusal before promotion/journal mutation |
| STG-007 | Wrong board ID | Refusal in main and, if independently presented, updater |
| STG-008 | Wrong layout ID | Refusal in main and updater |
| STG-009 | Unsupported data schema | Refusal before write |
| STG-010 | Minimum updater too new | Refusal before write |
| STG-011 | Pending result not acknowledged | New normal staging refused without replacing the result |
| STG-012 | Valid stage | Exact pair promoted; journal records staged candidate identity |
| STG-013 | Valid apply | Factory revalidates and writes; exact candidate becomes trial |
| STG-014 | Probation success | Durable success/result identity matches candidate |
| STG-015 | Exact acknowledgement | Only exact current result sequence clears pending state |
| STG-016 | Successful cleanup | Promoted files are removed after success; unrelated files remain |
| STG-017 | Re-upload after incomplete upload | Fresh exact pair stages without a format or broad delete |
| STG-018 | Mutation after stage, before apply | Launch-time revalidation fails and recovery/main remains safe |

### P1 interruption cases

| Test ID | Boundary | Required recovery |
|---|---|---|
| INT-001 | Upload interrupted during candidate `.part` | Stage refuses; re-upload works |
| INT-002 | Upload interrupted during manifest `.part` | Stage refuses; re-upload works |
| INT-003 | Reset after first promotion rename | Incomplete promoted pair is rejected; no unrelated image executes |
| INT-004 | Reset after staged journal commit | Recovery or main resumes exact recorded transaction safely |
| INT-005 | Reset after recovery-arm journal commit | Next boot enters/resumes recovery |
| INT-006 | Reset after applying commit, before first write | Recovery revalidates and restarts exact candidate |
| INT-007 | Reset during staged write | Partial image never executes; recovery restarts/reconciles |
| INT-008 | Reset immediately after `esp_ota_end` | Full image is reverified; no blind rewrite/boot decision |
| INT-009 | Reset after `IMAGE_VERIFIED` commit | Reconciliation continues exact finalization |
| INT-010 | Reset during factory-to-`ota_0` selection | Boot resolves to recovery or exact candidate trial only |
| INT-011 | Reset after `TRIAL_BOOT_ARMED` commit | Exact candidate enters trial or recovery reports failure |
| INT-012 | Reset during probation | Trial is not silently accepted; rollback/recovery is durable |
| INT-013 | Reset after mark-valid, before success journal commit | Reconciliation records a truthful terminal result |
| INT-014 | Reset after success commit, before staged cleanup | Success remains visible; cleanup is safely repeatable |

### Assertions for every case

Every test must verify more than the final HTTP status:

- Booted partition and component identity.
- Journal phase, source, candidate identity, and sequence movement.
- OTA partition state where observable.
- Result code/detail/sequence and `resultPending` behavior.
- Candidate and manifest file presence/absence.
- Exact image version and digest after a successful install.
- Ability to repair with a known-good exact pair after destructive failures.
- Absence of automatic NVS/LittleFS erase or format.

### Downstream impacts

- LittleFS receives repeated multi-megabyte writes; the disposable board may
  experience significant flash wear. Count destructive iterations.
- Main HTTP upload code is stressed by large encoded bodies and disconnects.
- OTA journal sequences and the one-slot result policy become part of test
  setup/cleanup.
- Each happy-path case includes at least the full probation interval, so the
  suite must expose realistic duration estimates rather than promising a few
  minutes for the whole matrix.
- Network transitions must be operator-assisted or Android-mediated without
  requiring the Mac to abandon its normal Wi-Fi.

## Q4: Negative artifact fixture generator

### Purpose

Negative cases must be reproducible. Manually hex-editing binaries or JSON
creates uncertainty about which layer should reject the artifact.

### Proposed interface

Add `tools/ota/make_test_fixtures.py`, or a clearly test-only subcommand to the
manifest tool. Given one known-good lab pair, it creates a local output
directory containing selected fixtures and a metadata index.

Fixture classes:

- Truncated image.
- Same-size image with one payload bit changed and the original manifest.
- Image corrupted so a newly signed detached manifest is valid but ESP-IDF's
  native app signature is invalid.
- Detached manifest with an invalid signature.
- Correctly test-signed wrong-board manifest.
- Correctly test-signed wrong-layout manifest.
- Unsupported schema and too-new minimum-updater manifests.
- Wrong declared size and wrong digest.
- Malformed, oversized, duplicate-field, and non-canonical manifests as allowed
  by the parser threat model.

The metadata index must record expected rejection layer:

```text
host-audit | main-stage | recovery-manifest | streamed-digest | esp_ota_end
```

### Signing-key rules

- Use a dedicated lab key whose public key is embedded only in lab artifacts.
- Never copy a private key into the repository or evidence bundle.
- Never derive fixtures from a production private key when a lab pair can prove
  the same behavior.
- Record only the public-key fingerprint.
- The generator must refuse to overwrite an existing output directory unless
  explicitly directed.

### Downstream impacts

- Manifest serializer behavior becomes directly testable.
- Lab updater and main builds need the same lab public key.
- Fixture storage can be large; generated binaries should remain outside Git.
- A fixture intended to reach `esp_ota_end` is destructive and must be marked
  as sacrificial-hardware-only.

## Q5: Deterministic interruption framework

### Why it is needed

Hand-timed resets can test long writes and probation. They cannot repeatedly
hit the narrow boundaries around `esp_ota_end`, a journal commit, OTA-data
selection, or reboot. Those windows need deterministic test instrumentation or
they must remain explicit residual risk.

### Build isolation

Add a default-off build option with a name such as:

```text
CONFIG_HW1_OTA_TEST_FAULT_INJECTION=n
```

Requirements:

- The production defaults for both main and updater set it off explicitly.
- Enabling it adds an unmistakable test suffix to image identity.
- Paired-artifact auditing rejects it for every release or migration profile.
- Production release CMake targets fail if it is on.
- Test-enabled artifacts use the lab signing key and a separate build directory.
- The code is compiled out, not merely runtime-disabled, in production.
- A lab artifact must print a conspicuous serial banner on every boot.

### Control channel

Faults should be armed through physical USB serial only. Do not add an HTTP,
web, MQTT, automation, BLE, or ordinary remote CLI endpoint.

Some automatic staged boundaries occur after main reboots into factory. The
armed instruction therefore needs to survive that reboot. A lab-only NVS
namespace is the simplest practical mechanism. A record should bind:

- Schema version.
- Hook ID.
- Action.
- Remaining hit count, normally one.
- Random run nonce.
- Candidate digest prefix or full digest.
- CRC/integrity field.

Both the test main and test updater may consume the namespace only when the
compile-time option is on. Production firmware ignores it. The runner clears
the record during preflight and cleanup, and an instruction expires after one
matching hit.

### Actions

Start with only two actions:

- `PAUSE`: announce the hook and wait in a watchdog-safe test loop until the
  runner requests continuation or external power is cut.
- `RESET`: announce the hook, flush the serial event if possible, and perform a
  software restart.

Add `STALL` only for explicit watchdog testing. It must be distinct from
`PAUSE`: a safe pause services required watchdog responsibilities; a stall
deliberately stops servicing the selected responsibility and has a bounded
expected reset.

Do not implement arbitrary delay values, addresses, shell commands, partition
writes, or remotely supplied callbacks.

### Hook inventory

The exact source placement must be reviewed against journal and boot-selection
semantics. Proposed IDs are:

| Hook ID | Component | Placement | Priority | What it proves |
|---|---|---|---:|---|
| `MAIN_AFTER_REQUEST_COMMIT` | Main | After staged request is durably committed | P1 | Requested transaction replay |
| `MAIN_AFTER_RECOVERY_ARM_COMMIT` | Main | After recovery-arm state is durable, before reboot | P1 | Recovery entry survives reset |
| `UPDATER_AFTER_RECOVERY_STARTED` | Updater | After recovery-running reconciliation | P2 | Recovery restart idempotence |
| `UPDATER_AFTER_APPLYING_COMMIT` | Updater | Before destructive staged write | P0 | Applying replay before first write |
| `UPDATER_AFTER_FIRST_WRITE` | Updater | After confirmed nonzero progress | P0 | Partial-write recovery |
| `UPDATER_AFTER_ESP_OTA_END` | Updater | Immediately after successful `esp_ota_end` | P0 | Native verification/reset boundary |
| `UPDATER_AFTER_IMAGE_VERIFIED_COMMIT` | Updater | After `IMAGE_VERIFIED` is durable | P0 | Verified-image finalization replay |
| `UPDATER_AFTER_FACTORY_SELECTION` | Updater | After factory is selected/canonicalized | P0 | OTA-data two-step selection recovery |
| `UPDATER_AFTER_OTA0_SELECTION` | Updater | After `ota_0` is selected as trial | P0 | Trial selection before reboot |
| `UPDATER_AFTER_TRIAL_ARMED_COMMIT` | Updater | After `TRIAL_BOOT_ARMED` is durable | P0 | Journal/boot-state reconciliation |
| `MAIN_AFTER_TRIAL_RUNNING_COMMIT` | Main | After main records trial running | P1 | Probation restart behavior |
| `MAIN_BEFORE_MARK_VALID` | Main | After health interval, before ESP-IDF validation | P1 | Trial remains rollbackable |
| `MAIN_AFTER_MARK_VALID` | Main | After ESP-IDF accepts image, before success commit | P0 | Truthful reconciliation after validation |
| `MAIN_AFTER_SUCCESS_COMMIT` | Main | Before result/staged-file cleanup finishes | P1 | Idempotent terminal cleanup |

Hook placement must never split an operation in a way that production code does
not naturally tolerate. If placing a hook exposes a non-idempotent step, that is
a design finding to fix, not a reason to weaken the expected result.

### Watchdog-safe pause implementation

A `PAUSE` hook must not accidentally become a watchdog test. Depending on the
calling task, the lab hook should either:

- Execute a bounded loop that continues to service the task watchdog and emits
  a low-rate heartbeat; or
- Signal a dedicated lab task and wait only where the calling task is allowed
  to block.

It must not globally disable idle-task watchdog coverage. Any temporary
unsubscribe/resubscribe operation must fail closed and be exercised in host and
hardware tests. `STALL` uses separate code and an explicit expected reset
deadline.

### Release audit extensions

`tools/ota/check_ota_builds.py` should reject release artifacts if any of these
are true:

- Test fault-injection config is enabled in either main or updater.
- The version or project identity has a lab/test marker.
- Main and updater test-mode settings differ.
- A release profile is paired with the lab public-key fingerprint.
- A required production default does not explicitly disable the option.

Where practical, also scan build metadata or symbol tables for the lab event
prefix and hook table. Config inspection is the primary gate; symbol scanning
is defense in depth.

### Downstream impacts

- `components/hardwareone/System_OTA.cpp` and possibly
  `System_OTASafety.cpp`: main transition hooks and validation-boundary hooks.
- `components/hw1_ota_protocol/`: only if a shared lab record definition is
  placed there. The production journal wire format should not change merely for
  test control.
- `updater/main/updater_main.c`: staged write, verification, and boot-selection
  hooks.
- Main and updater Kconfig/CMake/defaults: default-off option and lab identity.
- NVS: a lab-only namespace and cleanup procedure. No production migration is
  required because production ignores it.
- Timing/flash/RAM: production cost must be zero after compilation; lab builds
  accept small code and NVS overhead.
- Security: physical-only control plus compilation and release gates prevents a
  remote production fault surface.

### Acceptance criteria

- Every P0 hook is hit repeatably in at least five consecutive runs.
- `PAUSE`, `RESET`, and later `STALL` produce distinguishable evidence.
- A wrong candidate digest/run nonce cannot trigger an armed hook.
- A consumed hook cannot fire again after reboot.
- A production artifact pair fails to build or audit if test mode is enabled.
- A normal production pair has no callable test command or test NVS consumer.

## Q6: External reset and power control

### Reset is not power loss

Toggling USB serial control lines or calling `esp_restart()` tests reset
reconciliation, not loss of board power. Literal power-loss qualification needs
a controller that removes power from the board while preserving or separately
capturing serial where possible.

### Adapter design

The runner may support fixed adapters for known devices such as:

- A USB-controlled relay on the board's power lead.
- A programmable bench supply with a documented command set.
- A dedicated USB hub port-power controller that truly removes VBUS.

Do not support an arbitrary shell command from a JSON/YAML profile. That would
turn test configuration into code execution. Each adapter must enumerate and
verify a specific controller identity, expose `power_off`, `power_on`, and
`status`, and apply hard minimum/maximum timing bounds.

### Safety interlocks

- Require separate identities for the Feather and power controller.
- Refuse if multiple matching boards/controllers are present.
- Confirm the board is disposable before the first cut.
- Never power-cycle the host, storage device, or an unresolved USB hub target.
- Record whether a case used software reset, USB reset lines, or literal power
  removal.
- Restore power in a `finally`/cleanup path even if the runner is interrupted.

### Downstream impacts

- The lab needs dedicated hardware and known USB topology.
- Serial may disappear during power-off; the runner must expect port
  re-enumeration.
- Android-mediated HTTP and the host runner need bounded reconnect behavior.
- Evidence can claim literal power loss only when the controller confirms the
  commanded state.

## Q7: Evidence and release ledger

### Per-run evidence bundle

Default to a user-selected directory outside the repository. A run contains:

```text
summary.json
events.jsonl
serial.log
artifacts.json
environment.json
cases/<test-id>.json
```

Record:

- Test tool version or source revision.
- Board/layout/chip identity and masked device identifier.
- Main/updater versions, sizes, SHA-256 values, and public-key fingerprint.
- Partition-table digest and relevant production/test config flags.
- Monotonic and wall-clock timestamps with timezone.
- Commands as semantic actions, not raw strings containing credentials.
- HTTP status and sanitized response fields.
- Journal/result sequences and phase transitions.
- Reset reason and reset mechanism.
- Final disposition and unmet evidence requirements.

Do not record:

- Passwords, HTTP authorization, cookies, private keys, full usernames, user
  files, backup archives, or unredacted device captures.

### Release ledger

For a releasable pair, retain outside the source repository:

- Exact main, updater, bootloader, partition table, and manifest artifacts.
- SHA-256 values and public-key fingerprint.
- Board/layout/version/schema/minimum-updater identity.
- Toolchain/ESP-IDF version and reproducible build inputs.
- Paired-artifact audit output.
- Qualification summary and explicit skipped/residual-risk rows.
- Signing approval and key identifier, never the private key.
- Known-good recovery and downgrade instructions.

### Downstream impacts

- Release storage needs access control, backup, retention, and deletion policy.
- A source tag alone is no longer treated as a recovery artifact.
- Qualification claims become traceable to exact binaries instead of a mutable
  build directory.
- The one-slot device result remains an operational signal, while long-term
  audit history lives in the external ledger unless O4 is implemented.

## S1: Recovery credential hardening

### Current decision gap

The updater stores the same recovery credential as plaintext NVS strings for AP
access and HTTP authentication. Flash encryption protects this storage on the
encrypted board but not on the plain board. WPA2 needs credential material from
which the AP passphrase can be supplied, so replacing only the HTTP value with
a password verifier does not remove the need to protect or derive the AP secret.

### Options

| Option | Benefit | Cost/limitation | Recommendation |
|---|---|---|---|
| Accept plaintext on plain board | No compatibility change | Physical flash read reveals recovery secret | Accept only with an explicit physical-threat decision |
| Require flash encryption for production | Protects NVS and code/data at rest | Manufacturing and recovery become more complex; plain board becomes development-only | Strongest near-term production policy |
| Store AP secret plus separate HTTP verifier/token | HTTP credential need not be stored reversibly | AP secret still reversible; provisioning/migration changes | Useful only if separate credentials are desired |
| Derive both values from a device secret | Reduces directly stored reusable secret | Device secret must be protected; WPA2 passphrase still derivable on device | Consider with hardware-backed/encrypted storage |
| Open physical rescue AP after credential loss | Improves reachability | Creates a major unauthorized-access risk | Do not implement |

### Recommended decision path

For production, prefer flash-encrypted devices and document plain FeatherS3 as
a development/test profile. If unencrypted devices must ship, perform a formal
physical-threat decision before adding a complex verifier scheme that cannot
protect the WPA2 secret from a full physical flash reader.

If credentials change:

- Version the NVS credential record.
- Keep migration one-way only after a known-good recovery image is installed.
- Provide physical serial reprovisioning.
- Never auto-create a default or open AP.
- Rate-limit HTTP failures exactly as today or more strictly.
- Add old-record, partial-record, corrupt-record, NVS-full, reset-during-migrate,
  and FE/plain parity tests.

### Downstream impacts

- `updater/main/recovery_network.c`: AP and HTTP auth loading.
- Serial provisioning commands and operator guide.
- NVS keys/schema and migration/recovery behavior.
- Backup scope: recovery secrets remain excluded.
- Manufacturing and RMA workflows.
- Any phone profile must be updated if the AP password changes.

## S2: Direct HTTP-handler watchdog decision

### Question

The updater control task is subscribed to the task watchdog, but the direct
HTTP handler relies on absolute receive/apply deadlines and platform idle-task
coverage. Hardware forced-stall testing must determine whether that is
sufficient.

### Required experiments

- Stall before receiving the manifest body.
- Stall during manifest body receive.
- Stall before firmware erase.
- Stall during socket receive with no data.
- Stall during sequential OTA write.
- Stall during final native verification.
- Block the control task independently from the HTTP server task.
- Repeat with network disconnect and with a client that never closes.

### Possible implementation

If the experiments show the HTTP task can wedge without a bounded recovery,
subscribe the actual request-handling task for the destructive firmware path,
feed it only after meaningful bounded progress, and guarantee unsubscribe on
every exit. Do not feed the watchdog merely because a socket is still open.

An alternative is a separate supervisor timer/task that aborts the request and
reboots recovery when the absolute progress deadline expires. The choice should
be based on observed task ownership and ESP-IDF HTTP server behavior, not on a
generic preference.

### Downstream impacts

- `updater/main/recovery_network.c`: request lifecycle and cleanup paths.
- WDT configuration and control-task coordination.
- One small task/timer or subscription resource if a supervisor is added.
- Recovery availability after malicious or broken clients.
- More hardware cases and precise timeout documentation.

## O1: Hardware backup/restore qualification

### Purpose

The host backup tool has contract tests, but first-layout migration depends on
its behavior against the real web server and a changing filesystem.

### Required cases

- Normal complete backup with manifest inspection and representative hashes.
- HTTP 200 response containing an application-level read error.
- File size changing between list and read.
- Malformed, duplicate, missing, or path-traversal roster entries.
- Wrong bootstrap username.
- Exclusion of `/sd` and `/system/ota`.
- `users.json` restored last.
- Interrupted backup and interrupted restore.
- Insufficient local or device free space.
- Existing destination files and partial overwrite behavior.
- Filesystem mutation while backup runs.
- Restore verification by list, size, and content hash/readback.
- Plain and flash-encrypted device parity.

### Implementation approach

First run these tests without firmware changes. If live mutation causes
unavoidable inconsistency, add a narrowly scoped filesystem quiescence lease to
the authenticated superadmin maintenance path. A lease would need a timeout,
visible status, reboot cleanup, exclusion from automation, and explicit tests
that normal logging resumes. Do not add it preemptively.

### Downstream impacts

- Potential short interruption of file-producing subsystems during a future
  quiescence lease.
- Files API error contracts become release-critical.
- Migration duration and storage requirements become measurable.
- Restore ordering becomes a maintained compatibility contract.

## O2: Dedicated production-updater cable refresh target

### Problem

The first migration target can install a migration-only updater capable of an
explicit filesystem format. Re-running an entire migration merely to replace it
with a production updater is unnecessarily broad.

### Proposed target

Add a guarded cable target that writes only the factory updater partition and
only when:

- Board/chip/layout identity is explicit and matches.
- A current main/updater artifact pair passes the paired audit.
- The updater was built with migration-only formatting disabled.
- Flash-encryption profile matches the device.
- The factory offset and size match the audited partition table.
- The operator supplies an explicit confirmation token.

It must not write NVS, OTA data, LittleFS, model data, `ota_0`, bootloader, or
partition table. It is a recovery-component refresh, not a general flash target.

### Downstream impacts

- Root `CMakeLists.txt`: one new narrowly scoped target and guard messages.
- `tools/ota/check_ota_builds.py`: production-updater config assertion.
- Operator guide: repair and post-migration sequence becomes shorter.
- Security: physical cable access remains trusted; this does not create OTA
  updater self-update.

## O3: Signing-key rotation and artifact archival

### Near-term policy

The current updater embeds one public key. Losing the corresponding private key
or deciding to rotate it requires a guarded cable update of the updater and a
main signed under the new trust set. Therefore:

- Maintain an offline, backed-up signing-key custody procedure.
- Assign non-secret key IDs/fingerprints to releases.
- Retain at least one exact known-good artifact pair per supported board/layout.
- Test intentional downgrade using retained artifacts before depending on it.
- Document who can authorize signing and cable trust-root replacement.

### Later dual-key rotation option

A future updater could trust `current` and `next` public keys during a staged
rotation:

1. Cable-install updater trusting K1+K2.
2. Release main artifacts signed by K2.
3. Qualify recovery and downgrade policy.
4. Cable-install updater trusting K2 only when K1 retirement is approved.

Do not add a remotely writable key store. Trust-root updates remain cable-only
unless a separately reviewed root-of-trust protocol is designed.

### Downstream impacts

- Updater image size and manifest verifier if dual-key trust is implemented.
- Build tooling needs explicit key ID/fingerprint selection.
- Downgrade rules become key-epoch-aware.
- RMA and offline recovery inventory must span the rotation window.

## O4: Durable multi-result history

### Current policy

The device stores one terminal result. Normal updates require exact
sequence-bound acknowledgement. Marked emergencies may supersede it so an old
unacknowledged result cannot prevent rescue.

### Decision options

| Option | Benefits | Costs | Recommendation |
|---|---|---|---|
| Keep one slot plus external ledger | Small, already implemented, fail-closed | Device-local history is shallow | Preferred until fleet requirements demand more |
| Small NVS ring, e.g. 4-8 results | Better local diagnosis | New CRC/rotation/ack semantics, NVS wear and migration | Reasonable P2 enhancement |
| Append-only LittleFS log | More capacity | Filesystem may be the reason recovery is needed; locking/cleanup complexity | Not authoritative for recovery |
| Remote fleet audit service | Strong centralized history | Connectivity, identity, privacy, service operations | Separate fleet project |

### If an NVS ring is implemented

- Version it independently of the active transaction journal.
- Keep the current active result and acknowledgement semantics explicit.
- Define whether acknowledgement applies to one entry or a high-water mark.
- Make ring wrap visible; never pretend history is complete after wrap.
- Test corrupt slots, partial writes, NVS full, sequence wrap, emergency
  supersession, downgrade, and old-firmware compatibility.
- Extend status JSON and CLI without exposing secrets.

### Downstream impacts

- `components/hw1_ota_protocol/`: durable format and tests.
- Main/updater status serialization and reconciliation.
- NVS footprint and wear.
- CLI/web UI and operator acknowledgement procedures.
- Compatibility with old updater/main pairs.

## O5: ESP-SR model-partition migration

### Problem

The checked-in ESP-SR OTA partition layout relocates or introduces model data,
but the guarded migration workflow does not provision and verify that partition.
Therefore the layout must remain unsupported operationally.

### Required design

1. Define the source and exact version/hash of the model partition image.
2. Bind the model artifact to board/layout and main/updater release metadata.
3. Back up user LittleFS before repartitioning.
4. Guardedly write bootloader, table, updater, `ota_0`, and model partition to
   audited offsets without bulk-erasing NVS.
5. Verify model partition readback/hash before booting an ESP-SR main.
6. Restore user files into the relocated filesystem.
7. Verify main model discovery and representative ESP-SR operation.
8. Define rollback: returning to the no-SR layout is another cable migration,
   not an OTA downgrade.

### Downstream impacts

- Migration duration and failure surface increase materially.
- Backup free-space requirements change because the filesystem size changes.
- Release artifacts gain a model image and digest.
- Paired-artifact audit becomes a multi-artifact layout audit.
- A failed model write can leave main bootable but ESP-SR unusable; validation
  needs to distinguish those states.
- Documentation and support must treat layout change separately from a normal
  OTA application update.

## O6: Flash-encrypted-board parity

### Rule

The non-encrypted board is the development path, not proof for the encrypted
profile. After the plain matrix is stable, rebuild current source and run the
complete matrix on a dedicated `feathers3_fe` board.

### Required parity areas

- Guarded migration and production-updater refresh.
- NVS credential and journal retention.
- Staged and direct update.
- Negative manifest/native-signature cases.
- Reset and literal power interruption at P0 hooks.
- Probation, rollback, repair, acknowledgement, and cleanup.
- Hardware backup/restore.
- Correct rejection of every plaintext or wrong-profile flash target.
- Recovery after encrypted factory updater corruption using the documented
  development-mode encryption workflow.

### Downstream impacts

- Test time approximately doubles; destructive cycles may take longer.
- Build directories and evidence must never mix plain/encrypted artifacts.
- Flash commands and readback expectations differ under encryption.
- Credential-storage conclusions differ because flash encryption changes the
  physical-at-rest threat.

## Deferred architectural changes

### A1: Factory updater OTA/self-update

Do not add this to the present design. The factory updater is the last software
recovery anchor. Updating it in place creates a power-loss window with no
authenticated recovery application. A safe design would need another recovery
slot, a ROM/download-mode recovery procedure accepted as the product guarantee,
or an atomic bootable recovery chain with its own rollback. It also needs a
separate trust/version policy and a much larger test matrix.

Use the guarded cable refresh target instead.

### A2: Second main application slot

Two main slots would permit conventional A/B rollback to the previous main, but
the 16 MB layout would have to sacrifice a large part of LittleFS or model/data
capacity. It would require a new partition layout, destructive migration,
different staged-space calculations, and new rollback/data-compatibility rules.
The present factory-recovery-plus-one-main design is defensible when recovery
availability is more important than retaining the previous full application.

Revisit only if product requirements prioritize automatic previous-version
rollback over local storage.

### A3: Secure Boot and hardware anti-rollback

ESP-IDF signed apps without Secure Boot protect the OTA protocol but not a
physically attached attacker. Secure Boot, flash encryption, and anti-rollback
eFuses can strengthen that boundary, but eFuse decisions are partly
irreversible and affect development, manufacturing, RMA, signing, and recovery.
They require a separate threat model and manufacturing qualification project.

Do not enable irreversible fuses as a side effect of OTA hardening.

### A4: Additional transports and TLS

The recovery AP is deliberately small. Adding infrastructure Wi-Fi, BLE,
ESP-NOW, cloud pull, or TLS expands credentials, parsers, memory use, failure
states, and authentication policy. WPA2 plus application authentication and
signed artifacts is adequate for present local recovery, subject to the
credential-storage decision.

Add another transport only for a concrete product requirement and keep the
artifact verification/journal engine transport-independent.

## Detailed file impact map

| Path | Planned role | Risk and required verification |
|---|---|---|
| `tools/ota/hardware_qualification.py` | New qualification CLI | Destructive-command guards, resume behavior, redaction |
| `tools/ota/qualification/` | Clients, transports, scenarios, evidence | Host unit tests and stable schemas |
| `tools/ota/device_backup.py` | Consume extracted shared HTTP client | All existing CLI behavior and 14 tests remain green |
| `tools/ota/make_manifest.py` | Reuse canonical manifest logic | No release serializer drift |
| `tools/ota/make_test_fixtures.py` | Deterministic negative artifacts | Lab-key isolation and expected rejection layers |
| `tools/ota/check_ota_builds.py` | Reject test mode in release pairs | Both board profiles and mismatched pairs tested |
| `components/hardwareone/System_OTA.cpp` | Main staged hooks and possible test command plumbing | Journal ordering, FS lock scope, two-rename recovery, cleanup |
| `components/hardwareone/System_OTASafety.cpp` | Probation/mark-valid hooks if required | No production timing change; rollback still works |
| `components/hw1_ota_protocol/` | Optional shared lab record; possible result ring later | Do not change v1 journal casually; exhaustive host protocol tests |
| `components/hardwareone/CMakeLists.txt` | Main default-off test option integration | Release build has zero test code |
| `updater/main/updater_main.c` | Apply/verify/selection hooks | Reset reconciliation at every hook |
| `updater/main/recovery_network.c` | Direct-handler watchdog only if evidence requires | Auth/deadline/rate-limit regression suite |
| `updater/main/Kconfig.projbuild` | Default-off test configuration | Production defaults explicit |
| `updater/CMakeLists.txt` | Lab identity/key and release guards | Paired build consistency |
| `updater/boards/*.defaults` | Explicit production disable and board profile | Plain/FE comparison |
| Root `CMakeLists.txt` | Qualification targets and updater-only cable refresh | Stock destructive targets remain refused |
| `docs/OTA_RECOVERY_UPDATER.md` | Completed operator behavior and evidence | Update only after tests actually pass |
| `docs2/systems/recovery-ota.md` and affected file docs | As-built private knowledge | `docsctl` review/acceptance after source changes |

## Cross-cutting downstream impacts

### Security

- The qualification runner handles powerful authenticated operations and must
  redact aggressively.
- Fault injection is acceptable only with compile-time exclusion, physical-only
  control, lab keys, conspicuous identity, and release audit rejection.
- Credential changes alter the recovery threat model and require migration
  tests, not just stronger hashing.
- Key rotation remains a root-of-trust operation and should stay cable-bound.

### Reliability

- Deterministic hooks may reveal non-idempotent transitions. Fix transition
  ordering rather than teaching tests to accept ambiguous recovery.
- External power testing provides stronger evidence but increases hardware wear
  and the chance of intentionally corrupting `ota_0`.
- One main slot means every destructive failure needs a direct-recovery repair
  case.

### Flash and storage

- Staged tests repeatedly write multi-megabyte LittleFS files and `ota_0`.
- A result ring consumes NVS and increases writes.
- ESP-SR migration changes filesystem/model allocation.
- Evidence and generated fixtures should remain on the host outside Git.

### Memory and tasking

- Phase-1 tooling adds no firmware memory.
- Lab hooks and event strings must compile out of production.
- A watchdog supervisor or HTTP-task subscription may add a task/timer and
  cleanup paths.
- No test endpoint should cause the updater to buffer a full image or manifest
  beyond existing limits.

### Protocol and compatibility

- The production OTA journal should remain v1 during qualification work.
- A test-control record belongs in a separate lab namespace, not unused journal
  fields.
- Credential schema, a result ring, dual signing keys, or an ESP-SR layout each
  require explicit compatibility and downgrade analysis.
- Older main/updater pair combinations must either remain supported or fail
  clearly through the existing minimum-updater contract.

### Operator experience

- Tests will have honest, scenario-specific duration estimates.
- AP transitions can be handled by Android/ADB or manual association while USB
  serial maintains evidence.
- Cleanup and result acknowledgement remain explicit.
- Release and recovery instructions refer to exact archived hashes, not “latest
  build.”

### Documentation

- Prospective work stays here until implemented.
- Each landed phase updates the operator guide, command reference if commands
  change, build instructions, and current `docs2` entries.
- Hardware results must name exact artifacts and clearly distinguish reset,
  brownout proxy, and literal power removal.

## Implementation sequence

### Phase 0: Freeze test contracts

Deliverables:

- Stable test IDs and expected durable outcomes.
- Evidence JSON schema v1.
- Runner redaction rules.
- Board/layout/artifact identity rules.
- Decision on Python package layout and invocation environment.

Verification:

- Schema/parser host tests.
- Review against the current journal state machine.
- No firmware changes.

### Phase 1: Build the host runner

Deliverables:

- Shared authenticated device HTTP client.
- Main, recovery, host, ADB, and serial transports.
- Artifact preflight and evidence bundle.
- Checkpoint/resume framework.
- Negative fixture generator.
- Dry-run and non-destructive preflight commands.

Verification:

- Existing backup tests.
- New mocked HTTP/serial/ADB tests.
- Secret redaction tests with sentinel values.
- Crash/restart tests at every host checkpoint.
- No production firmware changes.

### Phase 2: Qualify current staged behavior

Deliverables:

- STG-001 through STG-018 on the disposable plain board where practicable
  without hooks.
- Exact duration and wear/destructive-cycle record.
- Any real defects fixed one at a time with regression tests.

Verification:

- Pair audit and host suites before flashing.
- Serial plus durable status evidence.
- Known-good direct recovery repair after destructive cases.
- Operator guide updated only with completed results.

Exit criterion: the ordinary staged happy path, all pre-write negative cases,
probation, acknowledgement, and cleanup are green before fault injection is
introduced.

### Phase 3: Add lab-only deterministic hooks

Deliverables:

- Default-off Kconfig/build option.
- Separate lab NVS test record and physical serial arm/clear protocol.
- P0 hook points, `PAUSE` and `RESET`, structured serial events.
- Release audit rejection.

Verification:

- Production pair byte/config/symbol checks show no test surface.
- Test and production artifacts cannot be confused by version, key, or build
  directory.
- Host tests cover record integrity, one-shot consumption, nonce/digest binding,
  and release rejection.

### Phase 4: Reset and literal power interruption

Deliverables:

- INT-003 through INT-014 using deterministic hooks.
- External controller adapter and literal-power evidence for P0 boundaries.
- Reconciliation fixes with one regression case per discovered defect.

Verification:

- At least five repeat hits per P0 hook.
- Software reset and literal power-loss results reported separately.
- Every destructive failure has a demonstrated known-good repair.

### Phase 5: Watchdog and operational hardening

Deliverables:

- Forced-stall matrix.
- Direct HTTP watchdog decision and implementation if needed.
- Hardware backup/restore report.
- Dedicated production-updater cable refresh target.
- Release/evidence ledger procedure.
- Recovery credential decision.

Verification:

- Auth/rate-limit/deadline regression tests.
- Migration backup/restore on hardware.
- Cable target dry-run and offset/size readback.
- Current public and private documentation accepted.

### Phase 6: Encrypted-board parity

Deliverables:

- Current plain and FE pairs rebuilt from the same reviewed source.
- Complete relevant matrix on the disposable FE board.
- Explicit encrypted recovery and wrong-profile refusal evidence.

Exit criterion: no production claim is broader than the rows actually passed on
both supported profiles.

### Phase 7: Optional policy-driven features

Only after the core qualification is green:

- Multi-result NVS ring if fleet audit requires it.
- Dual-key cable-mediated rotation window.
- ESP-SR migration support.
- Separate projects for Secure Boot/eFuses, A/B main slots, updater OTA, or new
  recovery transports.

## Verification strategy

### Host-only checks

- Protocol/journal serialization and corruption cases.
- Manifest canonicalization/signing/verification fixtures.
- Runner state-machine transitions and invalid transitions.
- HTTP status/body disagreement.
- Partial reads/writes and disconnects.
- Secret redaction.
- Checkpoint corruption and stale resume.
- Pair-audit release rejection for every lab-mode mismatch.
- Power-controller adapter bounds using fakes.

### Build checks

- Both plain and FE main/updater pairs.
- Exact partition-table comparison.
- App sizes and signed-image verification.
- Same shared public-key fingerprint per pair.
- Production test mode explicitly off.
- Lab artifacts conspicuously marked and rejected by release targets.
- Migration-only format option absent from production updater.

### Hardware checks

- Run the documented matrix, not a smaller happy-path smoke test.
- Capture USB serial independently of Wi-Fi.
- Query durable state after every reset.
- Verify repair after every destructive failure class.
- Distinguish software reset, USB reset, brownout proxy, and literal power cut.
- Count flash-destructive iterations per disposable board.

### Documentation checks

After first-party changes:

```text
python3 docs2/docsctl.py update --changed
python3 docs2/docsctl.py check
```

Queueing changed documents does not make them fresh. Review and explicitly
accept every affected as-built document according to `docs2/README.md`. Do not
accept unrelated stale documents merely to make the status green.

## Definition of production-ready for the current architecture

The current architecture may be described as production-ready only when all of
the following are true:

- The exact release pair passes guarded paired-artifact checks.
- Ordinary staged and direct happy paths pass on each supported board profile.
- All pre-write authenticity/identity negative cases fail without destructive
  mutation.
- Interrupted upload/write/verification/boot-selection/probation cases reconcile
  to a documented safe state.
- Literal power-loss evidence exists for the agreed P0 boundaries.
- Every destructive failure has a demonstrated authenticated repair path.
- Watchdog behavior is measured for control, staged apply, and direct HTTP.
- Backup/restore has real-device evidence for the migration contract.
- Recovery credential storage has an explicit accepted threat decision.
- Release artifacts, key fingerprints, audit output, and qualification evidence
  are archived.
- Test instrumentation is absent from and rejected by production artifacts.
- Public operator documentation matches source and `docs2` current-behavior
  documentation is reviewed and fresh for affected files.
- All skipped cases and residual risks are explicitly listed in the release
  record.

“Production-ready” does not mean the device can recover from physical damage,
a corrupted factory updater without a cable, loss of all retained signing keys,
or an incompatible irreversible application data migration. Those remain
separate operational guarantees.

## Rollout and rollback of these changes

### Host tooling

The runner and fixtures are additive. If they fail, stop using them; they do
not change device behavior. Keep older manual recovery instructions until the
runner has completed multiple known-good runs.

### Lab fault injection

The feature is removed from production by compilation. If hook integration
creates uncertainty, revert the hooks and retain the host runner plus external
power rig. Never relax the release rejection to unblock a build.

### Production firmware corrections found by tests

Land one reconciliation or watchdog correction at a time. Rebuild the pair,
run host tests, rerun the failing hardware case, then rerun the complete happy
path and adjacent transitions. Preserve a known-good cable recovery pair before
each experiment.

### Credential, journal, or partition-format changes

These need separately versioned migrations, old/new compatibility tests, and a
cable rollback plan. Do not bundle them into the qualification-runner change.

## Open decisions

These decisions should be recorded before their corresponding phase, but none
blocks starting Q1-Q4:

1. Whether the qualification modules live directly under `tools/ota/` or in a
   package subdirectory.
2. Evidence retention location and duration.
3. Which Android device/ADB flow is supported as the primary recovery HTTP
   transport.
4. Which literal-power controller the lab will standardize on.
5. Whether phase-2 evidence needs structured lab serial events or current logs
   are sufficient.
6. Exact lab NVS record name, expiry, and candidate-binding format.
7. Whether plain unencrypted hardware is a supported product or development
   profile only.
8. Whether the direct HTTP handler needs task-specific watchdog supervision
   after forced-stall evidence.
9. Whether one device-local terminal result plus an external ledger meets fleet
   audit requirements.
10. Artifact retention period and dual-control signing policy.
11. Whether ESP-SR is a committed supported layout and therefore merits its
    migration project.

## Recommended immediate work package

Implemented on 2026-08-04:

- Shared authenticated HTTP client extraction with all prior compatibility
  tests retained.
- Maintained qualification CLI, redaction, checkpoints, serial capture,
  and host/ADB recovery transports.
- Deterministic negative fixture generation.
- STG-001 through STG-018 scenario definitions.
- Twenty-six passing host-only tests and dry-run/preflight evidence.
- No production firmware behavior change.

The next implementation change is the reviewed staged executor for the
pre-write cases and STG-012 happy staging, still against current production
firmware. Only after those cases are green should the executor arm the
destructive STG-013 through STG-016 flow. The observations from those runs
should decide the smallest necessary firmware changes. Deterministic hooks
remain a later package.
