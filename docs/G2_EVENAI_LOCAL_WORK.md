# Native Hey Even with local speech and chat

> Historical development and device-test record transferred on 2026-10-13.
> Build, installation and flash statements below describe the originating
> workstation and its devices; they do not confirm the current checkout has
> been built or flashed. Device and network identifiers are redacted.

2026-10-13, implemented and build/host verified on main (`40261a39` starting
point), uncommitted. The first combined-image wearer test failed before chat
submission; native hardware qualification is in progress in the main chat.
Scope: native G2 wake/ASK/REPLY using the shared local STT broker and selected
chat source. Retain the CM5 UART route. No device, relay or agent settings are
changed; the main conversation owns its active hardware/relay investigation.

Code owners: `G2_Glasses.cpp` for native session/display, `System_STT` for finite
speech capture using its existing segment worker, `System_LLMChat` for exact
turn reads/cancellation. Native requests retain paired-user and temple epochs.

Verification covers endpointing, cancellation, session replacement, reply
framing and slow-model heartbeat scheduling. Hardware wake/audio/display
qualification remains separate from compilation.

Coordination sent to `Plan OpenClaw device integration` on 2026-10-13 at the
user's request. Its `docs/G2_LLM_READER_WORK.md` owns the Apps/LLM reader, hold
shortcut, TextEntry microphone startup and reply retention. This task owns
only the native adapter portion of the shared G2 file and the STT/chat APIs.
The chat API now exposes `ChatRunToken`, `chatGetRunToken`, `chatReadRun` and
`chatStopRun`; do not revive numeric-session exact-read/cancel APIs, because
backend session numbers can be reused. Reader integration needs the getter
plus `chatReadRun` and must handle -1 if the turn changed between calls.

Passed ASan/UBSan suites: native adapter (enabled and disabled variants), chat
lifecycle, Conversate, continuous STT, speaker-enabled continuous STT and
one-shot STT. The continuous suite's first run hit its host timeout during
heavy compilation; an idle rerun passed. CTest discovers `g2_evenai_local`.
Both saved profiles compiled and linked after the shared reader/API handoff:

| Profile | Image bytes | App headroom |
|---|---:|---:|
| `p4-openclaw` | 5,822,464 | 1,603,584 bytes (21.6%) |
| `p4-glasses-test` | 5,470,000 | 1,956,048 bytes (26.3%) |

The OpenClaw candidate includes both this native adapter and the coordinated
reader implementation. Its SHA256 is
`9984483074995a22a0a6da66f839e6c78e20818c201920114b3aac2f2b3afa66`.
The coordinated production/configuration file hashes stayed unchanged during
both builds; only host tests and documentation changed afterward.
No hardware, credentials, relay configuration, branches, index or commits
have been changed in this side conversation.

The slow-model host test executes the production session tick with mock BLE:
20 seconds of speech preparation followed by 120 seconds awaiting reply bytes
keeps the session active, schedules 46 heartbeats, and resumes 60-second idle
expiry after completion. Physical heartbeat acceptance still needs testing.

Recovery artifacts are in `work/hey-even-local/candidates/<profile>/`: app,
compressed ELF/map, bootloader, partition table, saved profile, SDK/dependency
lock and BUILD_INFO. `manifest.json` records exact artifact hashes. The task
directory also retains a combined source patch, source hashes and test/build
logs (61.4 MiB total). This candidate has not been flashed.

Cleanup is complete. Task-created APFS dependency clones and both build trees
were removed after artifact verification and ownership coordination:
`components/arduino`, `.sdk-overrides`, `managed_components`,
`build-config-p4-openclaw`, `build-config-p4-glasses-test`, and the temporary
host CMake configure directory. They totalled 2.67 GiB by `du`; observed free
space increased by 2.00 GiB (APFS clones share storage). Details are recorded
in `work/hey-even-local/cleanup.json`. The donor worktree and the other
session's builds were not modified. No large temporary directory remains
owned by this side conversation.

Hardware qualification, owned by the main device session:

1. Use the combined `p4-openclaw` image with the existing STT model, paired G2
   user and selected ready OpenClaw source. Keep the current relay settings.
2. Say "Hey Even", speak one short question, then pause for 1.5 seconds.
   Confirm the native popup shows the recognized question and the streamed
   answer, with one submitted question in the agent conversation.
3. Ask a follow-up and confirm conversation continuity. Try a multi-part
   answer and confirm its final text, including non-ASCII characters.
   Explicitly test a cold local model and a slow first response: the existing
   native heartbeat runs every 3 seconds while STT/chat refreshes activity.
   The 180-second whole-request deadline remains; completed replies use the
   existing 60-second idle exit. Confirm the physical popup survives the wait.
4. Dismiss once during capture and once while waiting for an answer; neither
   old exchange may write into a later popup. Reconnect and repeat a question.
   Cancellation after submission stops local delivery; it cannot undo agent
   actions already executed by the remote backend.
5. `g2evenai status` reports `provider=local-stt/chat` for a native local turn;
   selecting a CM5 model retains `provider=cm5` and its existing UART flow.

This is a qualification checklist, not a claim that these device checks passed.

Integration follow-up (2026-10-13, main device chat): the reader-only hardware
image omitted this adapter, which explains why the user's native wake path
was not yet available. The native section and shared STT/chat modules were
verified unchanged from this task's candidate before the combined build.
Root rebuilt main with native voice, the taller six-line reader and the
qualified Hosted DMA-alignment fix. `p4-openclaw` passed at 5,823,856 bytes,
with 1,602,192 bytes app headroom; SHA256
`f196c0bd685337320f0cc3e78980d18772130736073033cbf350692dfcf6bc83`.
Compact firmware/debug/configuration and the resolved P4 lock are in
`work/g2-llm-reader/combined-candidate/`, with a frozen source manifest/patch
beside it. The app-only flash verified its data hash; startup and wearer checks
are in progress. The current root task owns main's two profile build directories
and generated dependencies until qualification and cleanup.

The combined image booted with both lenses connected, <WIFI_SSID> joined,
HTTP READY and zero C6 transport errors/recoveries. The wearer reported the
native stock "trouble understanding" message and no transcript. The native
provider changed to local-stt/chat, proving WAKE admission reached source
selection, but no chat turn was submitted and no native heartbeat accumulated.
Read-only checks show pairedBy=a and both base heartbeat services responding;
both custom renderer pluginDead flags are set. Existing BoundArm and packet
guards couple native EvenAI to renderer liveness, unlike native Conversate and
DevCfg. The focused runtime-debug capture at 14:13:04 UTC recorded WAKE_UP,
then `terminal ... reason=local_owner_missing` about 1 ms later; the glasses
issued EXIT 7.7 seconds afterward. The user confirmed the same stock error.
Debug settings were restored after the capture. No question reached OpenClaw.

The narrow fix removes renderer-silence checks only from native BoundArm and
per-packet GuardMatches. Both require a nonzero exact Ready connection
generation; exchange ID, paired-owner/UART epoch and physical-write fences
remain. The custom renderer's watchdog and reader admission are unchanged.
Native host tests now extract the actual BoundArm and readiness functions,
replacing mocks that incorrectly coupled readiness to pluginDead. The new
regression failed before the fix and passes with it under ASan/UBSan, including
enabled/disabled variants, zero/stale generation, missing readiness, and owner
revocation. A second reviewer independently reran the suite. The original
combined source/image manifest is preserved as `combined-initial-*`; the new
554-file production freeze is `combined-source-manifest.json` with SHA256
`82e3d20bca9d4b1b675122b7bd813b67f0e44b09f160036bd1dc564a654c004d`.
Incremental affected-profile and baseline builds reuse the same directories.

Both final builds passed against that freeze. `p4-openclaw` is 5,823,840 bytes,
with 1,602,208 bytes free; SHA256
`14302094c15623cab6bcb78c99b1f3e7c9250a6bb809699daf18ca4bbbacbaf9`.
The app-only flash verified its hash. At 14:17 UTC the P4 boot had zero crashes,
both lenses connected, valid Wi-Fi/time/HTTP state and zero C6 transport errors
or recoveries. The final baseline is 5,470,144 bytes (1,955,904 bytes free),
SHA256 `c31a0bb6b33d43720bd2e8b355030ec800de440c25b3ce1d11ba381afb88b037`.
Its actual compiler choices retain G2, local 15x5 speech and SDMMC while disabling
all chat backends; both profiles compile the corrected Hosted override.
Compact final baseline evidence is in `work/g2-llm-reader/combined-baseline/`;
no baseline binary/debug image copy was made. Corrected native wearer testing
is in progress; these build/boot checks do not establish visible voice answers.

Wearer result for that guard-fixed image: native ENTER now succeeds, the cold
local model loads while audio is retained, and speech reaches Mingus. The P4
records `show me a potato` and the complete 22-byte reply `Here's a potato:`
followed by a potato emoji. The user saw the Studio reply but no lens answer.
A second ASCII-only question also showed the recognized question but no answer,
ruling out emoji as the cause of that failure. The P4 stayed healthy and the
C6 still had zero transport errors/recoveries. Native receipt ACKs appeared,
but no STREAM_COMPLETE event. This is not native rendering qualification.

The shared legacy sender put nonfinal text and an empty final packet on wire
about 2 ms apart, unlike the earlier qualified nonempty-final one-shot path.
The local adapter is being changed to hold one bounded chunk so the last text
and final flag travel together. Actual-codec checks also exposed that the
adapter's 240-byte text chunks exceed the encoder's single-envelope capacity;
212 text bytes fit the qualified 241-byte write limit. The pending change stays
inside the local adapter, preserves UTF-8/authority/cancellation and does not
change shared CM5 stream flags. Baseline/current build trees remain retained
for that incremental fix. A separate CLI long-answer preparation request
returned HTTP 408 before the taller-reader wearer test; its cause is not yet
established, and it was not automatically retried.

The local finalization fix is now implemented and host verified. A single
213-byte pending buffer plus its length live in the existing PSRAM session
state; read offsets include the retained tail and never count a flush twice.
Later text flushes the previous chunk, while EOF sends the final nonempty tail
with its end flag. Local ASK and REPLY text use 212-byte UTF-8-safe windows.
The test runner now compiles the actual protocol codec and verifies native
fields, exact text, CRC and ATT bounds using `g2_evenai_wire_fixture.h`.
ASan/UBSan passes all three feature combinations, covering short ASCII/emoji,
exact-boundary UTF-8, long/precompleted and progressive replies, completion
without new bytes, pending owner/run revocation, invalid tails and TX failure.
An independent reviewer reran the suite. Shared streamEnable=0/CM5 behavior
remains unchanged, so actual multi-chunk lens rendering still needs observation.
Final production freeze SHA256:
`1c1ecfa3b3fad49a34201ca0d1534390b47cf8d7e18deafdc0fb644a0ee9e4fc`.
The preceding guard-only source and image manifest are preserved as
`combined-guardfix-*`; both profile directories are reused for this rebuild.

Both finalization builds passed. The affected image is 5,823,952 bytes,
1,602,096 bytes free, SHA256
`5c7e6c96b875a89a5200d0fa45cf157628f99babbed6ae41aefc141006127a41`.
It was app-only flashed and boot verified at 14:32 UTC, again with zero crashes,
both lenses/Wi-Fi/HTTP ready and no C6 errors or recoveries. Baseline remains
5,470,144 bytes with 1,955,904 free, image SHA256
`e13539f78587ae3746d760feb13267a18913d9c5b1950d8691d8f7d503fb6556`.
Its final compact evidence was refreshed with the previous image hash retained
in `prior-build-history.jsonl`. New wearer reply/long-page checks are pending.

At 14:34 UTC the wearer confirmed the native short answer appears on the final
image. The diagnostic capture `native-debug-20261013T143210Z.log` records
STREAM_COMPLETE for both native turns at 14:34:02 and 14:34:35 UTC, followed
by native scroll events. This qualifies short-answer display, beyond receipt
ACKs. Long multi-chunk native rendering and the taller custom reader remain
pending separate wearer checks.

The subsequent six-houseplant-tips request also passed: the wearer confirmed
the full native answer and scrolling. The same capture records four full
241-byte REPLY envelopes plus a final 41-byte envelope at 14:36:09 UTC,
STREAM_COMPLETE at 14:36:40, and SCROLL events. This qualifies multi-chunk
native rendering with the existing shared stream flags, without a CM5 change.

The wearer also confirmed the taller Apps/LLM reader, paging and hold follow-up.
Final read-only snapshot `combined-watch-20261013T143750Z.json` retains the
complete 862-byte six-tip answer, HTTP READY with no error, both lenses up,
and zero C6 timeouts, transport errors or recoveries. Temporary G2 debug was
restored and serial released without resetting the P4. No global Mingus policy,
sandbox or credentials changed. Earlier single HTTP 408 remains an isolated,
unexplained test failure; it did not recur in these final three native turns.
The four task-owned generated build/dependency trees were audited and removed,
reclaiming 2.584 GiB; compact recovery/provenance and canonical Arduino remain.
See `work/g2-llm-reader/combined-cleanup-20261013.json`.
