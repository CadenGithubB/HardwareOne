# OpenClaw integration working status

> Historical development and device-test record transferred on 2026-10-13.
> Build, installation and flash statements below describe the originating
> workstation and its devices; they do not confirm the current checkout has
> been built or flashed. Device and network identifiers are redacted.

Current work (2026-10-13): phone-free native G2 Terminal with the
computer-side Even Terminal 0.10.5 server and a local OpenClaw provider. The
user selected browsing/resuming existing Mingus sessions and local bridge
behavior without telemetry or Even networking. The implementation, host setup
kit, focused checks, build/flash state and cleanup ownership are recorded in
[G2_TERMINAL_WORK.md](G2_TERMINAL_WORK.md). This is separate from the already
qualified Hey Even and custom LLM reader below. Terminal candidate
`468a54c2d95c8592a13279ef4b9ab87b7d73fe4a177a18f0972ba473016b79e0`
is now flashed and boot-checked; both lenses/Wi-Fi reconnected and C6 remains
healthy. Studio installation/Gateway connection succeeded. Normal side-menu
Terminal opening and stable Listening through transcript confirmation are
wearer-confirmed. The host update removed an admin-only send option while
retaining the existing narrow scopes and sandbox. A subsequent startup port
conflict was identified from the user's lsof output; after instructions to
restart the old listener, the user reports "Alright I think it works". The
basic Terminal voice flow is now working by wearer report. This does not
qualify every approval/question control. Both Studio processes must stay
running; no background service is installed.
Sanitized native regressions and both saved-profile builds passed. Serial is
released, compact recovery is retained, and the four owned generated
build/dependency directories have been removed again (about 2.60 GiB).

Latest wearer qualification (2026-10-13): the earlier combined `p4-openclaw` image
`5c7e6c96b875a89a5200d0fa45cf157628f99babbed6ae41aefc141006127a41`
was installed for this test. The wearer confirmed native Hey Even → local STT → Mingus →
visible short and long replies with scrolling, plus the taller six-line
Apps/LLM reader with paging and hold follow-up. Native admission now checks
the native connection independently of the custom renderer's silence flag;
the local reply adapter sends final text together with completion and keeps
each native write within 241 bytes. Shared CM5 protocol behavior is unchanged.
Both affected and baseline builds and targeted host tests passed. Final serial
checks show HTTP READY, both lenses connected, and zero C6 transport/recovery
errors. One earlier HTTP 408 was not retried and remains unexplained.
The four task-owned generated build/dependency directories were removed after
verification, reclaiming 2.584 GiB; compact recovery artifacts and canonical
Arduino source remain. Details are indexed in
[G2_LLM_READER_WORK.md](G2_LLM_READER_WORK.md) and
[G2_EVENAI_LOCAL_WORK.md](G2_EVENAI_LOCAL_WORK.md).

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

On 2026-10-13, the missing G2 LLM menu was traced to the old P4 speech profile's
`ENABLE_LLM_BACKEND=0`. The menu is supplied by P4 firmware; onboard inference
alone does not control its visibility. The saved `p4-openclaw` image enables
BACKEND and HTTP while keeping ONBOARD off and local STT on. Its artifact hash,
partition fit and equivalence to current runtime source were reverified. The
existing G2 Ask (Mic / Keys) input already routes local dictation into the
selected chat backend; no new adapter is required.

After the old login failed, the user explicitly requested a full flash erase
and fresh account setup on 2026-10-13. The P4 (`02:48:57:31:01:01`,
`<P4_SERIAL_PORT>`) was backed up in full (16 MiB), erased, and flashed with
the verified `p4-openclaw` bootloader, partition table, app and empty LittleFS
images. All four images passed esptool verification. Normal first-time setup
created the requested administrator; authenticated access was verified again
after a software reboot. The device reports firmware 0.99.98 and zero crashes.
Bluetooth auto-start is saved and running, G2 support is compiled/enabled,
and `llmEnabled` is true. HTTP chat remains unconfigured. The SD card was left
intact and the selected `quartznet15x5-gen2-25397.p4.stt` model is present.
The C6 companion remains online with matching Hosted 2.12.13 firmware.

Private recovery and test evidence is in the ignored directory
`work/openclaw-p4-port/fresh-setup-20261013/`. Retain its 16 MiB flash backup
as the deliberate rollback copy; no new build or project copy was created.
Do not commit the backup or credentials.

The user subsequently saved <WIFI_SSID> and paired both G2 temples. After
closing their serial monitor the P4 reported a USB reset, boot count 3, zero
crashes and no crash record. Bluetooth startup was already enabled; per-peer
`bleautoreconnect g2-glasses on` was enabled and saved through the authenticated
console. Both temples then reconnected automatically (`L=up R=up`), owned by
the fresh administrator. Closing/reopening our serial connection without
control-line toggles left boot count unchanged and both temples connected.
Wi-Fi was reconnected using the saved network and received `<DEVICE_LAN_IP>`.
At the user's follow-up request, Wi-Fi boot startup was enabled and saved;
the setting readback is true and both glasses temples remained connected.
Its reconnect-after-drop setting is also enabled.

Serial tooling caveats: the legacy bare `wifiautoreconnect` command sets false
rather than querying; this was corrected back to the fresh default of true and
saved during verification. Do not use it as a read. `openwifi` can consume a
queued `whoami` in its serial cancellation loop, so a completion-barrier timeout
is not proof of a failed connection. A fresh status query confirmed connection.

On 2026-10-13, the user restored the relay configuration and supplied serial
evidence of the first successful P4 text turn. `llmgenerate json` returned an
accepted asynchronous session; `llmresult json 0` returned
`P4 connected to Mingus.` with `done:true`. The earlier plain `llmgenerate`
instruction was wrong for this HTTP-only build; setup documentation now uses
the required `json` form.

The first spoken test also reached the Studio: local gen2 STT decoded the G2
microphone and the dashboard displayed `hello mingus how are you today`.
The P4 received `No response from OpenClaw.` with `done:true`, HTTP backend
READY and no error; the user confirmed that exact fallback appeared on the
glasses. Thus input, transport and lens display worked for this turn, while
Mingus produced no usable visible answer. The version-matched gateway source
emits that fallback when final assistant text is absent. The session trace
reported the Ollama model completed successfully at 12:00:31 UTC, with no tool
events shown. The user then exported the raw session branch on the Studio:
the smoke test's assistant text was `P4 connected to Mingus.`, and the greeting's
assistant text was exactly `NO_REPLY`. Both stopped normally. Completion
metadata reported success, no abort, no timeout, and no message sent through a
messaging tool. This confirms silent-reply suppression rather than missing
device transport or lens rendering; it does not identify which instruction
made the model choose silence.

Closing the serial monitor again caused a USB reset (boot 4, zero crashes),
clearing local chat history. C6 transport recovery then left Bluetooth blocked
on incomplete teardown. A normal software reboot recovered C6, Wi-Fi, the
valid clock, the HTTP source and automatic connection to both G2 temples
(boot 5). Temporary G2 debugging was cleared by that reboot. No agent policy,
sandbox or model settings were changed.

The relay now supplies a fixed request-scoped system note identifying direct
interactive device chat and asking for concise visible replies, including to
greetings. Incoming device requests still contain exactly one user message;
the relay rejects device-supplied system/developer messages and policy fields.
The original user text and conversation namespace are unchanged. OpenClaw
2026.9.4 supports this through its `extraSystemPrompt` conversation context.
This changes neither firmware nor global agent/sandbox settings.
All 29 relay and 11 setup tests passed, including the real loopback TLS path's
system-context forwarding, unchanged user text, one upstream request, and
rejection of device-provided system/developer messages. The existing small
relay ZIP in `work/openclaw-relay-kit/` was refreshed and its four files verified
against source. No new build or large copy was created. Other concurrent edits
to firmware STT/chat modules are outside this relay change and were not tested
or packaged here.

The user replaced the Studio's installed relay and restarted it after a
successful configuration/TLS preflight. A live P4 serial test then submitted
the same plain greeting, `hello mingus how are you today`, without exact-reply
instructions. Mingus replied: `I'm doing well, Caden! Ready to help with whatever
you've got. What's on your mind today?` The P4 received all 89 bytes with
`done:true`, returned to HTTP READY without error, and both G2 temples remained
connected. No reboot, flash or credential change was required. Private serial
evidence is saved under `relay-greeting-*` in the existing recovery directory.
This verifies the updated relay's live conversational response; the latest test
used serial input and does not yet verify a fresh microphone turn or what the
user sees on the lenses. The CLI reply is available in Apps → LLM → Open chat.

Next work: a fresh spoken greeting/follow-up from G2 Ask (Mic / Keys), verifying
the transcription and a substantive visible answer. The existing device
session key is recorded in private diagnostic notes, not needed in firmware.
Relay credentials are not stored on the workstation. Keep both VLANs separate
and never use a gateway operator token on the device.

2026-10-13 follow-up: Apps/LLM now has a full-width paged reader with plain ring
hold into the existing microphone/review/Done flow. The wearer confirmed paging
and a visible answer to a spoken plant question. Serial records confirm the
342-byte answer in the same conversation, HTTP READY without error, and both
lenses connected. The earlier microphone-check phrase `speaking test speaking`
produced the gateway's completed empty-answer fallback; its raw model completion
has not been inspected, so this is not proof of another `NO_REPLY` or a first-use
failure. No automatic retries or global agent/sandbox changes were added.
The relay model-wait fix and P4 radio DMA-alignment correction are qualified in
[G2_LLM_READER_WORK.md](G2_LLM_READER_WORK.md). Both task-owned managed worktrees
have been archived after compact firmware/provenance preservation; main retains
one canonical patched Arduino dependency tree. This does not resolve the separate
legacy storage backlog below.

Storage cleanup remains outstanding after the source consolidation. The user
removed regenerable compiler files from the old `stt_p4/private` build trees.
Legacy `stt_p4`, `speech_portable`, `camera_portable` and `jpeg_portable` private
directories still contain large app/dependency copies and historical builds.
Their unique experimental edits, private inputs and selected recovery artifacts
need a compact preservation check before retiring those copies. Current P4
builds, the selected model/checkpoint and rollback firmware have been verified
independent of the old `stt_p4/private` folder. Follow the workspace lifecycle
rules in [CONTRIBUTING.md](../CONTRIBUTING.md); do not treat this backlog as done.
