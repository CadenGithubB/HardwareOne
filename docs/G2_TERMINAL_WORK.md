# G2 Terminal through the P4

> Historical development and device-test record transferred on 2026-10-13.
> Build, installation and flash statements below describe the originating
> workstation and its devices; they do not confirm the current checkout has
> been built or flashed. Device and network identifiers are redacted.

Status (2026-10-13): native side-menu Terminal launch and stable Listening /
transcript review are wearer-confirmed. After the host Send correction and
instructions to stop the old port3457 listener and start the updated host,
the user reports: "Alright I think it works". Treat the basic Terminal voice
flow as working by wearer report, not as independently captured final reply
telemetry or qualification of every approval/question control. The installed P4 New-session repair is:
`74857a7d749f4fa890a7f0cdd919bff03b3f18c80f1feed489d90dc5f2e4adb9`.
It has passed both builds and startup checks; its wearer test is pending.
The host update preserves the narrow Gateway scopes and Mingus sandbox. Both
Studio processes (relay and Terminal host) must remain running; no background
service was installed. Compact recovery is retained. Generated build directories are cleaned and
serial is released. No Terminal test action was captured in the bounded
observation; the new-session wearer check remains pending. The earlier wearer-qualified taller-reader /
Hey Even image remains the rollback.
The user wants P4 → Mingus with no phone in
normal use, and authorizes research, a challenged plan, then implementation.
Work stays in the shared main checkout; preserve other ongoing changes.

The user clarified that the computer-side Even Terminal component is required,
that existing Mingus sessions must be browsable/resumable, and that the target
is the existing bridge's behavior without telemetry or Even-related networking.
The implementation includes the host session browser and provider; a
one-conversation native UI alone would not meet this scope.

## New-session repair: installed, wearer check pending

The wearer confirms switching existing conversations and loading their history.
They also confirm interrupting a running reply and sending the next message.
Structured Gateway question/approval cards remain unqualified on physical
glasses; the observed Send/Cancel screen only confirms a voice transcript.
The earlier supplied Mingus sandbox allowlist omits `ask_user`, although the
pinned Gateway includes it. Bridge question support is distinct from agent
tool availability. A deliberate non-secret `question.request` aimed at the
currently viewed session can test the UI separately; no such request or
sandbox configuration change has been made.
The user is assigning `ask_user` enablement to another agent; this task keeps
ownership of the native Terminal fix and does not change Mingus's tool policy.
Selecting New session instead closes Terminal; the P4 reports
`unknown-native-agent` in `observe-20261013T172930Z.json`. USB is now
`<P4_SERIAL_PORT>`, verified against the same P4 serial/MAC. Both lenses,
HTTP and C6 remain healthy. Exact stock code sends agent0 for the single-agent
New-session flow, while our adapter required the advertised Mingus ID1.
The focused fix accepts only implicit0 or explicit1; host configuration is
unchanged. Read-only inspection found no confirmed host defect. Stock success
NEW_RESULT also resets its session/content binding; send and acknowledge it
before LIST, SessionChange and sync. The existing sanitized production-adapter
harness passed default/explicit zero, explicit one, unknown-ID rejection and
ordering checks, including feature-disabled variants. Independent review passed.
Evidence: `work/g2-terminal-research/NATIVE_NEW_SESSION_2.3.2.14.md`.

The affected profile passed: candidate `74857a7d749f4fa890a7f0cdd919bff03b3f18c80f1feed489d90dc5f2e4adb9`,
5,858,336 bytes, 1,567,712 bytes of app-partition headroom. Compact packaging
verified all 560 frozen source inputs and the Hosted override. The sequential
disabled-chat baseline also passed (5,470,848 bytes; SHA
`7c2f00586dfa0ed47e3608fbc6ea1ad9f7d8acaa55171f78e8167d0d2f11c2ff`),
and was not flashed. App-only flashing passed hash verification. Startup
shows <WIFI_SSID> / <DEVICE_LAN_IP>, both lenses connected at MTU244, HTTP
READY, zero crashes and zero C6 recovery/transport errors. Evidence:
`candidate-boot-20261013T175042Z.json`; live New-session qualification is
pending. The bounded `live-20261013T175226Z.log` observed no Terminal test
action; final status remains both lenses connected, C6 online with no errors.
Serial was released afterward.

Root regenerated and then removed `build-config-p4-openclaw`,
`build-config-p4-glasses-test`, `managed_components` and `.sdk-overrides` for
this fix. Sequential three-job builds passed. After verifying compact image,
debug and source recovery and no active users of those trees, cleanup removed
2,762,203,136 allocated bytes (2.57 GiB); filesystem free space increased by
2,789,888,000 bytes. No full checkout or SDK snapshot was created. Cleanup was
completed before handoff while retaining the unqualified candidate and rollback.
The previously installed compact468a54 candidate and matching source provenance moved to
`work/g2-terminal/previous-candidate` (about31MiB) for rollback during this
repair; remove that superseded copy after the replacement is wearer-qualified.
The older combined reader/Hey Even recovery image is retained separately.
No extra checkout or dependency snapshot is needed.

## Current target after that clarification

```text
G2 native Terminal ↔ P4 (BLE, local STT, bounded async Terminal client)
                      ↕ existing verified HTTPS/device credential
                  restricted Studio relay /v1/terminal
                      ↕ loopback, separate host credential
                  Even Terminal 0.10.5 + OpenClaw provider
                      ↕ local Gateway WebSocket, paired host identity
                  existing Mingus sessions / sandbox
```

Implement a reproducible adapter for the official host's current provider and
event interfaces. Retain session browsing/history, live text and actual tool
activity, explicit voice follow-ups, interruption, and offered approvals or
questions supported by the installed Gateway. Disable Even account/cloud
connections, telemetry, update checks, tunnel/exposure providers and secret/text
logging in this local host mode. This does not change Mingus's own configured
model, tools, sandbox or network policy. Do not silently advertise unavailable
provider-specific controls as working.

The exact OpenClaw release tag resolves to
`3a9d69db306cd7f081e06254cb89c4bcc14a7107`, matching the Studio's reported
`3a9d69d`. Use that source's protocol 4 schemas, not guessed generic RPCs.
The host holds a persistent private Ed25519 identity and requests only required
operator read/write/approvals/questions scopes. Approval review requires normal Gateway
device pairing in that release; do not replace it with operator.admin or a
weakened Gateway setting. Installation/pairing on the Studio is a final setup
step once the concrete kit is ready.

The relay accepts a fixed, validated Terminal operation set and pins agent main.
The device receives host-issued opaque handles, never Gateway session keys or
its operator token. Bound JSON exchanges to 4 KiB, native text blocks to 512 UTF-8
bytes, and each visible list to the glasses' ten-session capacity with explicit
paging. Keep session selection, run identity, server-instance/cursor continuity,
and pending approval IDs distinct. A stale view or a reconnect must not submit
again or approve a replacement request. Mutation request IDs are idempotency
keys; unknown network outcomes are reported without inventing a new retry.

Root owns builds, hardware and cleanup. Native protocol/UI, host provider and
relay/device API source are frozen after focused checks and cross-review.
`integrations/openclaw/terminal/DEVICE_API.md` records their shared contract.
The 49,655-byte source-only Studio setup kit installed successfully. The user
verified Gateway 2026.9.4 connection with operator read/write/approvals/questions
scopes, enabled the existing relay's Terminal route, and started the local
Terminal host on loopback port 3457. Native opening is wearer-confirmed.

Follow-up source fixes: unchanged host state no longer repeatedly resets the
stock speech UI. Recording through transcript confirmation retains native
presentation ownership; only heartbeat, ASR and close may write during that
draft. Host snapshots and pending-card revocations still govern admission,
and deferred history keeps its last acknowledged cursor. Queued read-only
polls no longer reject voice start or confirmation. Diagnostic closure retains
the initial reason rather than replacing it with a generic successful close.

The host Send correction omits the admin-only field and locally rejects
Gateway command forms using that pinned release's input normalization and
cancellation aliases. Ordinary user text remains unchanged. Failures now log
fixed operation/error categories and recognized Gateway codes, never message
text or credentials. The existing provider/device API suites (15 tests) passed;
an independent review verified the admission fix, input guard and updater.
`integrations/openclaw/terminal/update.py` verifies installed module provenance,
backs up changed modules and preserves private setup/dependencies. The focused
update check passed, including dry run, backups, repeat execution and refusing
locally modified installed modules. No P4 flash or relay restart is required.
The 21,781-byte `work/g2-terminal/hardwareone-terminal-send-fix.zip` contains only
that updater, three host modules, instructions and hashes. The full setup kit
was also refreshed (53,075 bytes). The user applied the host update;
its backup is `host-update-20261013T170226Z-_uimhbzi` on the Studio. The next
launch exited with the generic local-host error. The user's read-only lsof
output showed Node PID77419 already listening on loopback port3457. We gave
commands to stop that exact old listener and start the updated host; the user
then reported that it appears to work. No reinstall, relay restart, credential
change or further P4 flash was required. Final reply visibility is supported
by that wearer report, without a new serial capture.

The stock side-menu START now enters the shared opener using a single-use
ticket bound to the original paired owner and connection, with a 15-second
pre-OPEN deadline. It reads sessions/history and does not send a prompt. Stock
firmware has no EXIT notification before OPEN; the deadline bounds that
unobservable early-cancellation case. Exact evidence is in
`work/g2-terminal-research/NATIVE_MENU_START_2.3.2.14.md`.

The two saved profiles were rebuilt sequentially with `IDF_PY_BUILD_JOBS=3`
on this 8 GiB Mac. After checking compact recovery/source hashes and confirming
no compiler used them, root removed the four regenerated build/dependency
directories. No extra source checkout or SDK snapshot was created.

The previous candidate `e83a8f8bbe6719a6e4ee60b5f82b5d12c1e10e727a67f0362e1716e01e453274`
was 5,856,544 bytes, with 1,569,504 bytes of app-partition headroom. App-only
flashing verified its hash and preserved settings/storage. Startup reports
<WIFI_SSID> / `<DEVICE_LAN_IP>`, both lenses connected, HTTP READY, zero
crashes, and the matching C6 2.12.13 companion with zero transport/recovery
errors. Physical first-open exposed missing firmware metadata after reconnect.
The adapter now sends one generation/owner-guarded BASIC query and waits
asynchronously for at most 1.5 seconds before any host/native Terminal operation;
known unsupported firmware still rejects immediately. The corrected candidate
was built in both profiles and flashed. A bounded read-only open correctly
reported `firmware-metadata-unavailable-wake-glasses-and-retry` and released
ownership. The user was asked to wake the glasses before retrying. No successful
Studio end-to-end voice turn is yet verified; native opening was subsequently
confirmed by the wearer after Studio setup.

The corrected candidate is 5,858,336 bytes with 1,567,712 bytes of app-partition
headroom. The final disabled-chat baseline passed at 5,470,848 bytes; its
hash is `0aaa78309da77042790e75c6601ac70656a75e364a340d0a6fed9be3d58259af`
and it was not flashed. Both builds match the frozen 560-input source manifest.
`work/g2-terminal/voice-fix-checks.json` records the focused checks, and the
private `observe-20261013T162842Z` capture records the original listening/view
mismatch. Build logs end in `voice-fix-final.log`.
App-only flash hash verification passed; startup at 16:51 UTC shows
<WIFI_SSID> / `<DEVICE_LAN_IP>`, both lenses connected, HTTP READY, zero
crashes and zero C6 transport/recovery errors. Credentials and storage were
preserved. `candidate-boot-20261013T164956Z.json` records health, and the bounded
`live-20261013T165143Z.log` records native-menu opening, recording,
transcription, confirmation and then the host-error closure. The wearer
confirmed both physical UI fixes. Final status shows both lenses connected
and zero C6 transport/recovery errors. The capture ended and serial ownership
was released. Generated build/dependency trees have been removed again.

Compact candidate, compressed ELF/map, configurations, locks and source
provenance are under `work/g2-terminal/`. The final disabled-chat baseline is
5,470,848 bytes with 1,955,200 bytes of headroom and was not flashed. Focused
checks passed: relay/setup (48), host provider/device API (15), sanitized
HTTP runtime, native lifecycle with disabled-feature coverage, and codec.
The actual-server HTTP smoke preceded the final activity/usage mapping;
those final mappings were checked through the production provider/device API
fixtures. No real Gateway approval was exercised.

Cleanup after this firmware qualification removed the four owned generated
trees: `build-config-p4-openclaw`, `build-config-p4-glasses-test`,
`managed_components`, and `.sdk-overrides`. Removed allocated size was
2,787,270,656 bytes (about 2.60 GiB); free space rose by 2,813,550,592 bytes during
the operation. Retained deliberately: about 31 MiB of candidate firmware,
compressed debug and configuration recovery, about 6 MiB of focused research,
the earlier qualified rollback and canonical Arduino source. No source
checkout, shared download cache, user recordings, SD models or other agent's
files were removed. `work/g2-terminal/cleanup-result.json` records this cleanup;
`cleanup-before-voice-fix.json` records the prior round.

## What Terminal is

Even's Terminal is a glasses interface to computer-hosted AI agents: sessions,
their output and activity, voice input, and agent questions/approvals. It is not
the ESP serial console or OpenClaw's text UI. The normal product uses the phone
app between the computer and glasses. The computer package does not speak G2
Bluetooth directly. Replacing that phone role is the work needed here.

Current source checks:

| Component | Verified version / source | Relevance |
| --- | --- | --- |
| Official computer host | `@evenrealities/even-terminal` 0.10.5, published September 24 | Inspected the published source, then smoke-tested the patched local launcher. Stock providers are Claude, Claude Sync and Codex; no built-in OpenClaw provider. |
| Current iOS app | 2.3.2, October 2 | App Store history identifies Terminal changes in 2.3.0, September 15. |
| Android archive available here | 2.3.1, build 241 | Local research records the post-2.3.0 Terminal changes. The live Play page does not expose a version number, so this is not claimed to be the latest Android release. |
| Paired glasses | 2.3.2.14 | Previously read from the device and qualified for Hey Even. No glasses firmware change is planned. |

Primary sources: [Even Terminal](https://evenrealities.org/terminal),
[published package](https://www.npmjs.com/package/@evenrealities/even-terminal),
[registry version metadata](https://registry.npmjs.org/@evenrealities%2feven-terminal),
[App Store history](https://apps.apple.com/us/app/even-realities/id6747017725),
[Google Play listing](https://play.google.com/store/apps/details?id=com.even.sg&hl=en_US).
Even's separate [OpenClaw bridge tutorial](https://support.evenrealities.com/hc/en-us/articles/16280926743695-Tutorial-Bridging-G2-to-OpenClaw-Bring-Your-Own-Agent)
configures Even AI's agent connection; it does not document the native Terminal
Bluetooth protocol.

The package uses HTTP `/api/sessions`, `/api/prompt`, `/api/messages`,
`/api/status`, `/api/events`, `/api/interrupt`, `/api/permission-response`, and
`/api/question-response`. Events carry session state and structured messages;
the phone converts those to the native glasses UI. The small source inspection
copy and registry integrity are in `work/g2-terminal-research/` (about 314 KB
initially). Initial research required no installation or APK expansion. The
later disposable host smoke and firmware builds have explicit cleanup owners.

## Plan and completion gates

1. **Establish the current wire and host contracts.** Compare recovered native
   descriptors against installed glasses firmware 2.3.2.14. Pin the official
   computer package 0.10.5 and the Studio's exact OpenClaw 2026.9.4 source. Record
   framing, event identity, history, approvals and questions before integration.
2. **Implement the computer provider and local relay projection.** Retain the
   official host's session/event interfaces, add an OpenClaw provider, and use a
   dedicated loopback-only launcher. Device operations use opaque session/view
   IDs and bounded JSON; Gateway tokens and paired identity stay on the Studio.
3. **Implement the native P4 adapter.** Reuse existing BLE framing, physical-write
   guards, worker scheduling and local STT ownership. Browse real Mingus sessions,
   open recent history, stream content/tool activity, confirm voice prompts, and
   bind interrupts/approval choices to the exact selected view and pending run.
   Preserve the existing custom LLM reader and native Hey Even paths.
4. **Challenge failure boundaries.** Reconcile history with live events; invalidate
   stale selections on disconnect/reconnect; never replay an uncertain mutation.
   Preserve all approval details or refer that request to the host. Do not grant
   permissions merely because a different bridge/provider offers that control.
5. **Qualify proportionately.** Focused production-code codec/lifecycle and host
   contract tests, the affected saved profile and disabled-chat baseline, then
   short physical session/history/voice/scrolling/reopen tests. Build success is
   not hardware qualification. Install the prepared Studio kit and pair its
   identity through normal Gateway controls before end-to-end approval testing.
6. **Finish cleanup.** Keep the known-good app as rollback, flash only the app,
   preserve settings and compact source/debug/configuration provenance, then
   remove owned compiler/dependency output. No extra checkout or APK expansion.

## Holes found in review

| Risk / incorrect assumption | Resolution |
| --- | --- |
| Installing the official host removes the phone | It does not; P4 implements native SID 0x30 and the phone's input/display role. |
| Old Terminal commands still work | Current 2.3.2.14 descriptors and dispatch were checked; the adapter rejects unsupported firmware. |
| Chat completions can implement existing sessions/approvals | Use the exact Gateway WebSocket contract in a host provider; the existing chat endpoint remains independently usable. |
| Reopening a screen resends a prompt | Opening projects history; submission is a separate explicit operation with an idempotency key. |
| Closing a view stops agent work | It only closes presentation. Interrupt targets an exact host run and reports the Gateway verdict. |
| A run token is a persistent conversation | Host-issued session handles, view handles, server instance, event cursor, run ID and pending question ID have separate lifetimes. |
| History reads cannot race a completion | Subscribe first and reconcile live content/pending requests against the history snapshot before advancing the baseline. |
| An approval can be shortened like ordinary text | Critical details and offered choices must fit completely; otherwise require host review without an approval shortcut. |
| Reconnection makes retry safe | Unknown mutation outcomes remain unknown; never replace the request ID or auto-replay a voice prompt. |
| Terminal input can overlap other microphone users | Reuse STT/native audio ownership, connection epochs and worker boundaries, including transcript confirmation. |
| Full bridge behavior means unlimited device history | The host owns durable history. Native lists page, text uses 512-byte UTF-8 blocks, and the glasses retain a bounded 64-item view. |
| Image tools imply pictures in Terminal | Official 0.10.5 reports image-view/generation tools as text activity. Current native Terminal has no image payload field; actual pictures require a separate display path. |
| A local host entry point cannot contact Even | Audit reachable runtime imports and disable account/update/tunnel/telemetry paths explicitly; package download occurs only during installation. |

## Protocol evidence and remaining hardware qualification

The available APK research preserves command names but not the new numeric
field definitions. Community notes targeted older firmware and were insufficient
for the current implementation. Offline recovery linked the archived 2.3.0.24
nanopb wrapper to SID `0x30`, then the current image below confirmed the actual
contract. List-focus notification uses tag 30 instead of the older tag 24.

The exact stock 2.3.2.14 image was found in a public catalog and downloaded
directly from [Even's CDN](https://cdn.evenreal.co/firmware/7892790e40cffbae6db8a635b335e17f.bin).
All six component CRCs, bundle MD5/SHA256, main SHA256 and the embedded version
were checked. It is 4,701,884 bytes; no extracted binary copy was created.
Comparison confirms all 32 descriptors / 81 fields match, including capacities
and struct offsets. Native EXIT is command `0xAB`, tag 21, in the current image.
Current code also confirms that OPEN directly enters service `0x30`, voice
start acquires audio owner 6 before notifying the host, and exit releases it
before clearing the context. Notifications and ACKs originate only from the
native primary role, so the adapter must not wait for a separate ACK from both
lenses. This removes the offline protocol blocker; the one-time phone capture
is now a fallback if bounded device qualification disagrees. No glasses update
is needed. Actual primary-side routing, native audio and display behavior still
need a controlled P4/glasses test.

Implementation ownership: the native adapter owner edits the codec,
`G2_Glasses.h/.cpp` and its focused lifecycle harness. The other user chat was
notified and remains read-only. Terminal uses its own host session projection;
it does not reuse the custom reader's shared `ChatRunToken` as a session key.

Build ownership: root will reuse the normal `build-config-p4-openclaw` and
`build-config-p4-glasses-test` directories in this checkout, plus the existing
SDK override mechanism and shared component cache. About 2.6 GiB of disposable
output is expected; free space before reconstruction was 22 GiB. Keep the
qualified image under `work/g2-llm-reader/combined-candidate/` as rollback; no
duplicate source/dependency checkout. Remove these new build outputs after
preserving the selected compact image/debug/configuration set and verification.

## Contract used by this implementation

This is the stock **2.3.2.14** contract; unsupported glasses versions are refused
before opening. SID is `0x30`, wrapper field 1 is the command and field 2 is an
8-bit correlation magic. The right temple is the native primary: current role
getter value 1 both permits Terminal replies and maps to the firmware's literal
`RIGHT`/`R`. Its ACK is sufficient; the left supplies microphone audio.

| Host command | Body tag | Nested fields |
| --- | --- | --- |
| OPEN 1 | 3 | agents in field 1; session-list message in field 2 |
| CLOSE 2 | 4 | error in field 1 |
| ASR 3 | 5 | text, sentence-final, all-final in fields 1–3 |
| State 4 | 6 | session ID, state in fields 1–2 |
| Content 5 | 7 | session ID, content ID, kind, text in fields 1–4 |
| Query 6 | 8 | query ID, question, up to eight offered choices, session ID |
| Sync 12 | 14 | session ID, phase (begin 1/end 2) in fields 1–2 |
| Heartbeat 255 | 41 | empty |

OPEN supports up to ten agents and ten sessions; this bridge exposes Mingus
as agent 1, with explicit page controls for larger session lists. Each session
has a nonempty title (up to 128 bytes on the wire). The nested session list contains the current ID in
field 1 and repeated session records in field 2. Each session contains ID,
title, agent ID and state in fields 1–4. These IDs identify this native view;
they are not OpenClaw session keys.

ASR and content text are at most **512 UTF-8 bytes per update**. Same content ID
replaces a block; a new ID appends to the native 64-item ring. Sync begin clears
the ring. Streamed partial blocks must retain their ID until full or final,
otherwise small streaming deltas would quickly evict the beginning of a reply.
This ring is not unlimited history. The host projects recent messages and live
updates; durable conversation history remains in OpenClaw. The separate custom
LLM reader still retains at most 16,383 bytes.

Native events: START `0xA1`/tag 20 and EXIT `0xAB`/tag 21 have empty bodies;
VOICE `0xA2`/tag 22 has field 1 values start 1, stop 2, timeout 3, confirm 4,
cancel 5. ACK `0xF0`/tag 40 has error in field 1; omitted zero means success.
VOICE notifications can share magic values, so magic alone is not an event ID.
The glasses suppress duplicate host magic for three seconds; the host retires
magic values across close/reopen and serializes outstanding requests. Successful
commands refresh the native 20-second heartbeat deadline; idle keepalive is five
seconds. Unknown events do not submit chat.

The compact evidence is retained in `work/g2-terminal-research/`:
`STATIC_TERMINAL_2.3.2.14.md`, versioned contract JSON, descriptor comparison and
named disassembly slices. The sole downloaded firmware image has SHA256
`26b6067665a441b779c47f6b95427b8b4bb5f502aaf527466dadce628aeb00b2`;
its main component is
`d6d29ec6a6d7d3f7e0abb260c54493e2e5dd0ed978346dc41061ee6522d40fc6`.
The research directory is about 6 MiB, with no APK expansion. A separate 4.5 MiB
disposable host smoke directory exercised the patched server with a fake Gateway;
it has been removed after preserving the source hashes and check results.

## Implemented preparation and validation

`tools/btsnoop/terminal_inspect.py` reads an existing H4 log or one explicitly
selected bugreport ZIP member. It validates both ACL and G2 fragmentation,
connection/handle/direction separation, ordering, length and CRC before emitting
SID `0x30` metadata. Text is hidden by default; explicit `--include-payload`
is available for private protocol analysis. It does not assume old command
enums, install anything, extract archives or send Bluetooth traffic.
Usage and limits are in [the tooling README](../tools/btsnoop/README.md).

Four focused tests passed. The tool also accepted the existing 2.2.7 settings
capture: 724,110 bytes, 11,002 records, three Terminal messages; SHA256
`696a7652605e7b549c7fa2bb2cbbc293d6dea1650f78a7a30c593d2f45fa76a9`.
That is a framing check against real traffic, not a newer-protocol fixture.
No P4 build is needed for this offline Python-only change.

For an Android reference capture, [AOSP's documented method](https://source.android.com/docs/core/connect/bluetooth/verifying_debugging)
is to enable Bluetooth HCI snoop logging in Developer options and restart
Bluetooth before reproduction. Capture only a short synthetic Terminal
conversation; record the app/glasses versions and action times. Export a bug
report, turn snoop logging back off and restore normal P4 connectivity. Keep
the bugreport private; this tool can select just its Bluetooth log without
expanding unrelated system diagnostics. Exact phone/host setup is pending
availability; no phone settings or pairing have been changed here.
