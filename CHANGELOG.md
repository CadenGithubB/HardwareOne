# Changelog

All notable changes to this project are documented here. The format follows
Keep a Changelog (https://keepachangelog.com) and this project uses
Semantic Versioning (https://semver.org).

Entries for 0.96.1 and earlier were backfilled from git history (this repo had
no tags or releases before 0.96.2); they are terse, commit-grounded summaries,
dated from each version's commit. Dates are YYYY-MM-DD.

## [0.99.92] - 2026-08-26

The largest release since the OTA work, and most of it is things that were quietly broken. The glasses get a real QWERTY keyboard and stop inventing taps out of app-lifecycle traffic, `closewifi` stops crashing the board outright, recovery OTA grows past the FeatherS3 to four boards, `memreport` measures what it used to estimate, dictation works from the glasses instead of only the on-device screen, a corrupt config file is quarantined instead of deleted, and credential redaction in the audit log fails closed instead of falling back to the raw line.

### Added
- Text entry on the glasses is a real QWERTY keyboard. A 288x144 key grid drawn as four bands beside a seven-row navigation list (Cancel, Done, the four arrows, Mic and Keys) covers the whole character set across three pages with no list rebuilds, and a double-tap on the ring selects the highlighted key. It replaces the old six-group cycling keyboard, and a measured canonical action now takes 147 ms against 246 ms for the one-tile surface it replaces. If the four-band push does not go through, the pad falls back to a single image and then to the legacy grouped character list on its own.
- Dictation is no longer tied to the on-device screen. The keyboard's Mic row opens a speech page on the lens (GET READY, SPEAK NOW, TRANSCRIBING, auto-stopping on silence, tap Mic to cancel) and the transcript lands in the field being edited. Each surface drains only its own transcript, so an on-screen session and a glasses session can no longer take each other's text, and a secret field refuses dictation outright and renders the row as "Mic disabled". The host transcript timeout went from 15 to 90 seconds to cover a voice fetch plus a cold model load on the co-processor, and arming now fails with `host not present`, `host stale` or `host not ready` rather than recording audio nobody will ever transcribe.
- Recovery OTA works on four boards instead of one. `feather_esp32_v2` and `qtpy_esp32` join `feathers3` and `feathers3_fe` in the board registry, each carrying its own board id, layout id and version suffix together in the manifest, so a 16 MB image cannot be staged onto an 8 MB board even though the two 8 MB layouts are byte-identical. The classic ESP32 boards also get a UART0 recovery console, since they have no USB-Serial/JTAG peripheral to fall back on, and the recovery power interlock is now selectable per board rather than assuming a fuel gauge exists.
- `memreport` reports measurement rather than arithmetic. One section is cumulative allocation traffic — attempts, successes, failures, fallbacks and per-tag DRAM and PSRAM bytes from a live tracker — and the other measures every task stack and control block off the allocator in a single heap walk. It used to fold hardcoded "FreeRTOS ~8192" and "WiFi Driver ~32768" credits into a supposedly measured total while summing no stacks or control blocks at all, hiding a 24 KB shortfall and dropping 20,992 bytes of live heap on a minimal build. TOTALS now prints actual DRAM used, allocator metadata, what was attributed, and a signed unattributed residual that is never padded to zero; when the task snapshot cannot be taken it says "NOT MEASURED this run" and asks you to run it again instead of printing zero as though it were a reading.
- R1 firmware 2.2.9.0003 is recognised as its own profile with a per-primitive capability list, so an operation the connected ring cannot serve settles immediately as unsupported instead of arming a transaction whose only possible outcome is a timeout. The web health page follows: `Poll Now` renders disabled with the reason and the logging line reads `passive/on-demand` rather than a fake poll interval, and `/api/health/action` names the missing capability and states that no command was sent, replacing a flat "ring is not connected" that was never true.

### Changed
- A critical config file that fails the boot JSON check is renamed to `<path>.corrupt` and left on flash instead of being deleted. Deleting it turned "corrupt" into "absent", which every load gate treats as the legitimate first-boot case, so the next save wrote defaults straight over a file that was still recoverable — for `users.json` that meant losing every account with nothing to seed them back. Anything that assumed a clean absent-file state after a bad boot now has to cope with the quarantined copy, and with the flash it occupies.
- `dictate` is no longer a command anyone can type. It moved off the command registry onto the authenticated UART control plane, is claimed before the command dispatcher runs, and the host daemon must advertise `dictate hostready v1` against its exact login epoch after every login or the firmware refuses to arm. Nothing outside an authorized co-processor session can invoke it at all, and a build without dictation answers plainly instead of falling through to the CLI.
- The Pet app is gone with no replacement: `g2pet`, its Apps launcher row and `/system/pet.json` are all removed. The on-lens transport test bench and its `g2probe` and `g2imgprobe` commands now exist only in builds made with `ENABLE_G2_TESTSUITE=1`, so without the flag the System launcher has no Tests row at all; scripts calling either command must check for it or build with the flag.
- The NTP server and timezone offset moved out of the WiFi settings module into their own `system.time` clock module. The timezone reverted on every reboot on a build with the radio compiled out, because both settings lived inside the WiFi block and had no row to persist through — even though a timezone is perfectly meaningful with an RTC and a manually set clock. The `ntpserver` and `tz` commands are unchanged.

### Fixed
- `closewifi` could crash the board outright, jumping to a garbage address assembled from ASCII bytes of a JSON buffer. Disconnecting the station is not a power-down: it clears only the station bit, so from the combined access-point mode a previous ESP-NOW session leaves behind, the radio stayed powered with a hidden channel-parking access point still beaconing while the command cheerfully reported "WiFi off." Switching straight to off was worse, destroying both network interfaces while an access-point-stop event was still queued, so its deferred cleanup ran against memory already reused. The access point is now dropped as a real mode change, the stop event is given 150 ms to drain, and only then does the historically safe station power-down run. `radiopower off` had the same shape and the same fix, and teardown no longer deinitialises the driver underneath a live ESP-NOW send, nor changes mode with sensor polling still in flight.
- The glasses stop inventing taps. Every lens gesture used to be guessed from a heuristic read of one byte channel that 2.2.9 captures prove is app lifecycle, not input — wake-word beacons and app-switch snapshots were read as user taps and display-off teardowns, dismissing pages, exiting text views and tearing down containers with nobody touching the device. Gestures and terminal events now come only from the typed event channel, and the grace windows and 150-370 ms beacon workarounds built on top of the old misreading are gone in favour of one reducer fed by real exit events, a debounced ownership loss, BLE disconnect or a 60-second safety fallback.
- Several WiFi controls had never worked at all. The web Settings "Save and connect" button posted a verb that does not exist; both Connect buttons and every scan-result Select button did nothing when clicked, because their inline handlers were double-quoted and contained double quotes; on-device "Forget network" failed every single time, because it quoted the name and the handler matched the quotes as part of it; a 32-character SSID could never join, ever, because the copy into the driver's fixed-width field reserved a terminator it does not use and handed over 31 characters; and `wifiadd "My"Net" pass123` silently stored the wrong credentials outright, while an over-long SSID or password was refused with "saved-network list is full", the wrong reason entirely.
- The recovery access point had been refusing to start on the 16 MB FeatherS3 since the partitions grew on 16 August: three of the eight layout constants in the recovery preflight described a partition table that no longer existed. A device that needs recovery stays dark, which makes it invisible at the worst possible moment. The updater and the application now compile one tracked partition table chosen by board and flash size, and every candidate-image size check reads the running partition table instead of a hard-coded slot offset that matched no current layout — one that under-reported the real slot on 16 MB boards and over-reported it by roughly 650 KiB on the 8 MB Feather V2, letting an oversized image pass validation and then die partway through flashing.
- `otaupdate confirm` refused for want of power on every routine update on the voltage-only battery boards, because the battery percentage it tested is never populated on that path — the check was always `0 >= 30` — so `force-power` on every single update had become muscle memory. On a board with no battery subsystem at all it refused permanently after the first thirty seconds of uptime, demanding fresh power telemetry the hardware cannot produce. Power is now judged from voltage where a cell exists, treated as external power where none is fitted, and skipped entirely on boards with no monitor.
- One corrupt `debug.json` plus one flag change silently flattened the operator's entire debug-flag set, because the writer rebuilds the file from RAM and RAM after a failed read holds nothing but compiled defaults. The notification policy file had the same hole, committing all-zero masks that were byte-identical to "no policy file", so the next per-kind write erased every override. A password change on a user whose settings file would not parse wrote a file containing only that password, destroying every other per-user setting and, on the gamepad path, the account's own login hash. A corrupt `boot_anchors.json` collapsed sixteen anchors to one. All of these now refuse the write, preserve the file and raise a corrupt-config event.

### Security
- Credential redaction in the command audit log now fails closed. `wifiadd "My Net pass123` with an unterminated quote swallowed the rest of the line as one token and `wifiadd "My"Net" pass123` shifted the masked position onto an SSID fragment — both returned the raw line, password included. Whenever tokenization up to the masked position is ambiguous, the log now records the verb plus `***` and nothing else. `usersync` left passwords readable by the same class of bug and was caught by nothing downstream; dictation results and failures used to write the transcript and the host's reason into the audit trail verbatim and are now reduced to the operation, a hex id and `***`.
- The plaintext WiFi password was written into the debug queue on every connect attempt, and that queue lives in PSRAM — a probeable external chip on boards where flash encryption is off. The password now prints as a length only, the SSID prints bounded so it cannot run off a full-width field into the adjacent key, and the deferred Bluetooth login job that also carries a plaintext password is now a hard internal-DRAM requirement instead of a preference that could spill outward under memory pressure.
- Basic Auth decoding sized a heap allocation straight from the attacker-supplied header length; the header is now bounded to the protocol's real ceiling, parsed in a stack buffer, wiped on every exit path, and rejected outright if the decoded payload contains an embedded NUL rather than being silently truncated. Dictation results and failures were previously accepted from any logged-in UART session, and an over-long Bluetooth write ran as a silently truncated command — the copy stopped at 511 bytes and executed whatever prefix fit. The whole record is discarded now, and the session is told exactly why.

## [0.99.91.1] - 2026-08-19
A memory release for the classic ESP32 boards, which have no second heap to fall back
on when internal RAM runs out. About nineteen and a half kilobytes of internal DRAM
were handed back on the Adafruit Feather ESP32 V2: free memory at boot went from
137,599 to 157,151 bytes and the resident internal footprint from 64,520 to 45,760.
Nothing was moved on faith - every buffer was checked against interrupt, DMA,
early-boot, spinlock, cache-off and secrets rules before it left internal RAM, and the
result was measured on hardware rather than estimated.

### Changed
- Large buffers that are only ever touched by ordinary tasks now live in external PSRAM: the system event ring, the UART link's frame, reply and status buffers, the ESP-NOW address scratch used at thirty-seven call sites and its topology streams, the notification preference cache and app sinks, the CLI confirmation state, the on-screen file browser's render data, and the R1 transaction history. The one UART buffer that stays internal is the wire buffer, because the driver copies it out from inside a spinlock where PSRAM cannot be read.
- Reading back the event history no longer holds the ring's lock for the whole fetch. It copies four entries at a time instead, which is what makes the ring safe to keep in PSRAM, and the count of entries lost to overflow stays exact across the chunks.
- The R1 ring's protocol debug scratches, worth 4.7 KB, are allocated only while the protocol debug gate is actually open and released when it closes, instead of sitting resident for a session that may never turn debugging on. Decoded health records still never touch PSRAM, and the boot self-test borrows its working memory from PSRAM and gives it straight back.
- ESP-NOW peer buffering and the mesh retry queue were deleted. Neither had a caller left; `espnowmeshstatus` reports the same JSON it always did.
- Constant tables that were being copied into RAM at startup now stay in flash, including the audio codec's pointer tables, the log file-path strings and the on-screen Bluetooth text seed.

### Fixed
- ESP-NOW is switched back on. A test build for the language-model work turned it off and the resulting configuration was never linked, so the feature was absent from a build that reported it as present.
- The two file-transfer buffer sizes are declared once and shared, ending a mismatch where one translation unit and the web server disagreed about their type.

### Security
- The on-screen ESP-NOW remote-login form wipes the username, password and command on send, on cancel, when it opens and when it is initialised, and the send path clears its own command buffer as well. Cancelling out of that form used to leave the typed password sitting in memory indefinitely.
- The deferred Bluetooth login job now demands internal memory for its working copy, so a plaintext password can no longer land in PSRAM, which is readable off the chip.

## [0.99.9] - 2026-08-19
The largest release since the OTA work, and mostly about identity: who issued a
command, over which interface, and whether that session was still alive by the
time the command ran. Every transport now carries a session epoch and refuses
work admitted by a session that has since died. Around that, the Raspberry Pi
co-processor becomes a first-class part of the device - it answers language-model
prompts, transcribes dictation, reports its own fan and temperature and can
correct a dark-boot clock - the Android app becomes a notification sink over
Bluetooth, and a rewritten redaction pass stops credentials leaking into logs,
audits, exports and mesh frames.

This release also grows the application partition, so it has to be installed over
a cable rather than over the air.

### Added
- Sessions can now be addressed by name. `login <user> <pass> [serial|uart|display]` and `logout [serial|uart|display]` act on the interface you name, the bare form acts on the interface you typed it on rather than always on the serial console, targeted forms are accepted only from a named serial, UART or display session, and five failures in a row cost sixty seconds. `whoami` answers with your name, your role and the transport you are on, an unauthenticated serial console says so plainly, and guest is now a real named view-only role that can do nothing but log in, log out and ask who it is.
- The Raspberry Pi co-processor is controlled from the device. `cm5 power` replaces `hostpower`, `cm5 fan` reports temperature, PWM, RPM and health and takes quiet, auto or max, `cm5 heartbeat` gives the Pi a presence lease so the device knows whether the host is starting, ready, busy or degraded, `cm5 time set` lets a Pi with a synced clock finish a dark boot and nudge an already-set clock only when it is more than two minutes out and was not set by hand or by NTP, and `cm5 status`, `cm5 capabilities` and `help cm5` describe the link.
- The language model can now run on the Pi instead of on the device. Models are named `<source>:<name>` and come from one registry, the default build ships the feature with the Pi source on and the on-board PSRAM engine compiled out, and the web `/llm` page with its dashboard card and nav link, plus the OLED menu of Ask, Guided, Switch and Unload, all follow whichever backend is selected. Streamed answers are idempotent by sequence number and are given up on by a sixty-second stall watchdog and a two-minute selection watchdog rather than hanging.
- Dictation from the on-device keyboard. A MIC page records what you say, shows the elapsed seconds and input level while it runs, hands the audio to the Pi and types the transcript back into the field, reporting `Transcribing...` and then a named reason if it fails. The recording is deleted once it has been delivered, a result is only accepted for the display session that asked for it, and the keyboard's maximum entry grew from 32 to 128 characters.
- The Android app is a notification sink. With `notifydeviceapp` on, which is the default, notifications are delivered as `#NOTIF` lines to authenticated Bluetooth sessions, capped just under two hundred bytes, honouring each user's per-kind level floor and mutes, and `notifstats` counts what was sent to the app and what was dropped.

### Changed
- Serial, UART, Bluetooth, web, the on-device screen and the glasses each carry a session epoch, and a command is refused with an error ending "before command execution." if the session that admitted it died first, if a `*requireauth` policy flipped underneath it, or if the glasses' voice authority changed. A command could previously be admitted under one identity and execute under another, or execute after the session that authorised it was gone. Any whitespace is now a command separator too, so a tab no longer slips past authorisation.
- A confirm prompt belongs to the session that opened it and is answered as an addressed reply, with the caller's rank re-checked when the answer arrives; someone else's "yes" is now an unknown command rather than a confirmation of your file deletion, user deletion or factory reset. Machine transports - UART, MQTT, automations, the glasses, Bluetooth and `/api/cli` without the interactive flag - cannot open or answer prompts at all, help renders for them without holding a slot, and log suppression while help or a prompt is on screen is now serial-only, with security, auth and error lines still passing.
- A command that failed says so on every interface it can reach: `/api/cli` answers HTTP 400 on an error result, MQTT reports its ok field honestly, the UART link says "Error: command failed" and voice says the command was rejected. Those surfaces previously could not tell a failure from a success, because the real result was discarded before it got to them.
- Operations that take time now queue and tell you where to watch them. `ringscan` answers with a queued scan and points at `ringstatus`, `ringconnect` says queued and stamps pairing authority on every call, and `g2recover` answers "repair queued - use g2status"; all three used to report inline and hold the console while they worked. `ringconnect` also waits up to ninety seconds for a real glasses recording or speech session instead of declining outright.
- WiFi has a single radio owner. `closewifi`, `wifidisconnect`, `radiopower off` and `openespnow` now fail with an explicit busy error instead of fighting a scan that is already running, scans release the driver's result list before returning, a failed scan says why on every surface rather than showing an empty list, each connection attempt gets 25 seconds instead of 12 with power save off during the handshake, and `closewifi` was reordered so a busy radio leaves the web interface usable.
- The application partition is half a megabyte larger on five of the six layouts, which buys back the room the year's features have eaten. This release must therefore be installed over a cable, speech-recognition boards must re-flash their model blob as well, and only a provably blank data region is auto-formatted so your files survive. NVS is untouched, but the OTA tooling and the recovery updater still assume the old slot size and were not updated in this release.

### Fixed
- Bluetooth sessions are properly separated from each other and from the rest of the device. A Bluetooth `login` was re-serialized into a command whose last word was read as an interface name, so `login alice hunter2 display` could mint a session on the on-device screen; replies were broadcast, so every connected peer saw every other peer's output; a reply could land on a connection id that a new peer had since been given; and decrypted frames or queued commands from a peer that had already disconnected still executed. Logins now take a native quote-aware path with the password zeroed after use, replies are point-to-point, and every connection carries its own epoch.
- Recordings no longer lose the ends of words - "potato" came back as "potat" - because the discard threshold was an absolute level rather than one measured against the noise floor. Auto-stop behaviour is unchanged. Separately, `micsource` could latch a preference of the glasses while the on-board microphone was actually the active source for a whole session, which is why some recordings came back around −25 dBFS.
- Two ways to end up with a device that will not come back are closed. `esptool erase_flash` left a board that could not boot, because a completely blank filesystem partition was read as corrupt instead of being formatted once; and a power cut during first-time setup locked the device out permanently, because the user database was written before the credential that opens it. Setup now writes the credential first and withholds the user database with an explicit error if it cannot, so setup simply re-runs on the next boot.
- The language-model surfaces stop misreporting themselves. All seventeen `llm*` setting commands wrote the wrong field, because their table was indexed by position after two rows were prepended to it. The result endpoint split multi-byte characters and emitted invalid JSON on control bytes, which left the web client polling forever, and truncated a fast answer to a single 511-byte window. The glasses viewer cut every turn at 127 bytes and wrote past the end of its buffer. Prompt casing flipped ordinary words, so "How do" was sent as "how Do".

### Security
- Redaction was rewritten to work on tokens with whitespace awareness rather than by matching text, and is now applied everywhere a command or its result is recorded or echoed: exec traces, unknown-command echoes, automation logs and exports, glasses hijack logs, Bluetooth, serial and UART logins, `/api/cli` bodies, feeds and batches, the on-device echo and result, and every ESP-NOW frame, log, event and stored record. Previously a tab character bypassed every rule, a command spelled `loginXYZ` dodged the login rule, MQTT logged the broker password, and `automation list` printed stored credentials verbatim. Automation exports now fail closed on a malformed file instead of dumping it, MQTT logs a byte count rather than the payload, and `testencryption` and `testpassword` print lengths and a verdict only.
- ESP-NOW command, response and stream frames are accepted only when they are session-encrypted and come from a paired peer, so plaintext frames from older peers are dropped rather than executed. Mesh routing driven over MQTT is admin-only and goes through the authorized command registry with escaped JSON, on its own task rather than the MQTT one.
- Role lookup fails closed: an account that cannot be resolved is treated as anonymous rather than as an ordinary user, which is exactly how the web guest gate could previously be escaped, and the same lookup now backs command authorization, the nav bar and file access. Session lists show only an eight-character prefix of a session id, and revoke accepts that short form, so full identifiers are no longer written where another authenticated user can read them.

## [0.99.89] - 2026-08-11
The glasses answer "Hey Even" with no phone anywhere in the loop, a Linux
co-processor can drive the device over a wire, and the microphone pipeline stops
losing audio it used to lose silently. The firmware now plays the phone's part in
the glasses' own voice flow, a new UART machine channel gives a co-processor its
own logged-in session for commands, audio and host power, and recording moved off
a single boolean onto a state machine that still finalizes a WAV when the source
disappears mid-capture. Settings also lose the ability to erase themselves after
one bad read.

### Added
- The glasses answer "Hey Even" without a phone. The firmware now plays the phone's part in the native voice flow: a wake opens a session, a heartbeat holds the listening card open, replies stream back as they are produced, and an idle exchange exits itself after a minute. `g2evenai` drives all of it, wearer dismissals ride a small priority queue ahead of general traffic so a burst cannot drop one, and a wake that arrives with no host to answer it is declined outright so the glasses fall back to their own no-phone timeout instead of sitting on a listening card nobody is listening to.
- A wake with a logged-in host attached starts recording on its own and tells the host everything without a round trip: the saved file path when the recording auto-stops, the wake, claim, first-audio and endpoint timestamps with the sample count, a cancellation notice delivered three times with retry, and a stream-complete marker. The capture is endpointed by voice activity and bound to the wearer who woke it.
- A Linux co-processor can now drive the device over a wire. `uartlink` brings up a machine command channel on dedicated UART pins: newline text commands run through the normal dispatcher under their own transport and audit source, and a framed binary lane shares the same wire for audio and events without ever interleaving with a text reply. The channel has its own session - in-band `login`, `logout` and `whoami`, its own lockout and idle window - and `uartrequireauth` and `uartlinkbaud` tune it. The IDF console moved to USB-Serial-JTAG to free the port.
- `voicefetch` sends a finished recording to the host as framed binary with byte and frame totals and a whole-file checksum, and an opt-in live stream tees an in-progress capture to the host in real time. The live tee is a shadow: it can abort itself if it falls behind, but it can never delay or damage the recording it is copying.
- `hostpower` changes the Linux host's power profile and asks it to reboot, halt, suspend or sleep for a set time. Requests carry their own ids through an acknowledged state machine and are reconciled afterwards against the host's boot identity, so a lost final acknowledgement is completed rather than executed a second time, and an outcome that stays ambiguous fails closed until an operator runs `hostpower recover confirm`. The destructive verbs need super-admin and a confirmation on the same line.
- `micrecord start` gained `vad <ms>`, which stops the recording after that much silence, and `trim`, which tracks the ambient floor from both ends and drops the room tone before you started speaking. A capture owned by a live voice exchange also keeps up to 1.2 seconds of audio from before it was told to start, recovering the words spoken in the gap between the wake and the microphone being claimed, and a capture that never latches onto speech records straight through instead of producing a stub.
- Recordings can carry a caller's own 64-bit token. `micrecord startid`, `statusid` and `stopid` plus `micdeleteid` let a host start, watch, stop and delete exactly the capture it owns, filenames are derived from the token, a completion history ring remembers results after the fact, and a delete matches an exact path only.

### Changed
- Recording is a five-state machine instead of a boolean. Starting, capturing, stopping and finalizing are separate states behind one finalizer and one serialized open/close lock, with a non-blocking hook for a source that vanishes - so glasses that drop mid-recording now leave a properly finalized WAV where they used to leave a corrupt one. A reboot drains an in-flight recording before restarting, a factory reset refuses to run over one, a failed capture says exactly why and removes the unusable partial file, and the wake-word engine waits for a busy recorder instead of seizing the microphone or reviving one that a lost source had already shut down.
- Microphone-over-glasses statistics mean something now. `gap_events`, `lost` and `stalls` are read from the real per-packet counter in the stream trailer instead of being guessed at, a delivered-rate watchdog marks a recording degraded when delivery sinks below seventeen packets a second and seals that flag into the result, and captures hold the Bluetooth link at its fast interval for their duration because the glasses' idle power state halves mic delivery. Ring reconnection is held off while a glasses capture is running, and interval readouts finally show real milliseconds (15.00, not 15.15).
- Boards build in isolation. `tools/build_board.sh <board>` builds into its own directory with its own configuration, so switching boards no longer means a full clean first, and every successful build drops a manifest of the resolved flags, sizes and source commit it was made from. The blank-filesystem restore target now exists on every board, with the multi-device port guard extended to cover it.
- Flash and memory headroom for the smaller boards: NeoPixel is a real feature flag whose commands, icon, settings and screen rows compile out together while the shared power rail stays asserted so I2C and STEMMA still work, the XIAO S3 carrier profile drops the screen, sensor layer, maps and web extras, voice phrases moved out of every command table into one speech-owned route table to reclaim about eleven kilobytes in builds without speech recognition, and SD cards mount fastest-first at 16 MHz with a full bus reset between attempts.

### Fixed
- Settings can no longer erase themselves after one bad read. Saving now refuses to rebuild settings.json from memory when this boot never successfully loaded the file, and refuses to overwrite an on-disk file it could not parse - previously a single failed read was flattened into compiled defaults, silently destroying Bluetooth pairings and the ESP-NOW mesh configuration. A refusal is logged, raises a system event, and leaves the file intact for recovery. The firmware-version stamp inside the file is now rewritten one key at a time with the rest byte-preserved, so the settings-carried-over notice fires once per update instead of on every boot.
- The raw glasses recorder stops losing its first packets. It wrote the SD card directly from the Bluetooth callback and lost two packets every session, deterministically; packets now land in a PSRAM ring drained in the background, and stopping flushes the staged tail and reports the ring counters.
- `openble` no longer crashes the device with a double free. The server and client roles were fighting over the radio because neither `openble` nor `closeble` recorded which role was actually wanted, so a client reconnect intent survived into server ownership; the desired role is persisted now, `closeble` hands the radio back to the glasses client, and a vendored Bluetooth patch clears the library's static client alias on destruction, which was the other half of the same crash.
- The ring's clock is set to your time zone rather than UTC. Time sync sent an offset of zero, so the ring keyed its daily pages to the wrong midnight; the configured offset is sent now and an out-of-range one is refused. Daily parsing also labels day and latest-reading timestamps by what they actually are instead of inventing 1970-era times when it guessed wrong.
- The glasses' voice protocol is parsed the way the stock app actually speaks it. The response command id is two bytes and is 161, not the 12 it was being read as; the heartbeat, the real configuration message shape and the shared command prefix were corrected against captures, and self-test vectors lock every one of them byte-for-byte.
- Login auditing works on a device with no web server. It lived inside the HTTP code, so headless builds lost the audit trail on every other transport, and the stub that stood in for it passed its arguments in the wrong order. Every transport now goes through one canonical front door.

### Security
- Every voice exchange is bound to the connection that started it. A wake mints a 64-bit exchange id tied to the temple that sent it, the Bluetooth connection generation and the host login epoch, and every later message is checked against that binding: an exit is honoured only from the exact connection generation that delivered the wake, an in-flight reply re-checks the session between fragments and cancels if the exchange has gone terminal, and the older untagged commands fail closed and terminate any live exchange rather than acting on someone else's.
- A UART host's session is fenced to its login. In-flight transfers stop at the next frame boundary when the host logs in again, serial-console login and logout are rejected over the link, UART sessions appear in the session list and are revoked along with their user, and a UART session can never become a lens's persistent paired-by owner.

## [0.99.82] - 2026-08-06
Observability and safety work on top of the OTA release. Signed updates now have
their own event group instead of a single catch-all, changing the recovery
credential needs someone physically at the device, the recovery updater narrates
the multi-minute parts of a transfer instead of going silent, and cable-flash
targets say which board they are about to write.

### Added
- Firmware updates are now their own event group. `Firmware & OTA` joins the eleven existing groups and carries ten kinds: credential changed, upload started and finished, staged, staging rejected, trial started, accepted, rolled back, recovery entered, and the general transaction result. Each one shows up in notifications and can be muted or forced per user like any other event, and automations can trigger on them individually.
- Setting or clearing the recovery credential now raises an alert naming which of the two happened and who did it. The command log could not tell them apart, because it redacts everything after the verb and renders both as `otapin ***`.
- The recovery updater says how long the slow parts will take before starting them, then prints progress every ten percent and a completion line. Writing a five megabyte image takes one to three minutes, during which it previously printed nothing at all and looked hung.
- Cable-flash targets print the port they are about to write, and refuse outright when more than one USB serial device is attached and none was named. `idf.py -p` never reached these targets - it was accepted and discarded - so with two boards connected they would silently write whichever one answered first.

### Changed
- `otapin` is restricted to the physical serial console. It decides whether the recovery updater will bring up its network at all, so a hijacked super-admin session could previously take recovery offline from anywhere. Everything else in the update lifecycle still works remotely; only changing the credential now needs physical presence.
- The recovery web page keeps the result of an install where you can read it. The status poller used to overwrite it a second later, so the one line that said whether the update worked was the line guaranteed to disappear.
- The recovery web page warns before the image transfer that it takes minutes and reports nothing until it finishes, and shows elapsed seconds while it runs.
- A command context with no explicitly assigned origin is treated as a web request rather than a physical serial one, so the two commands that accept only serial fail closed if a future transport forgets to identify itself.

### Fixed
- A rolled-back image now really does appear in `otastatus`. The 0.99.81 release notes said it did; it did not, because the boot path consumed the breadcrumb before an operator could ever read it. The record is now kept until the next rollback replaces it, and is still logged only once.
- `otastage` no longer tells operators of an already-provisioned device to go and set a credential they set months ago, and the two commands that genuinely need one now name the serial console in their error.
- The recovery updater accepts a request while another operation is running by refusing it immediately rather than waiting a second first. It runs on a single task, so that wait stalled every other connection before failing anyway.

### Security
- Both setting and clearing the recovery credential are refused from the web UI, Bluetooth, MQTT, ESP-NOW, voice, the on-device screen and the glasses. This is a deterrent rather than a boundary: physical access still allows a full reflash, and only flash encryption with secure boot makes that a real barrier.

## [0.99.81] - 2026-08-05
A follow-up to the OTA release that closes the gaps found by actually provisioning
devices with it. The biggest one: first-time setup was being rolled back. A freshly
cable-flashed image serves a probation that it can only clear by finishing startup,
but the setup wizard blocks waiting for a human, so an operator who read the menu
slowly blew the five-minute deadline, the image was marked aborted, and the device
came back to the recovery updater needing a full re-provision. Reaching the wizard
now accepts the image outright, which proves more than the probation would.

The recovery updater also stops being an open console. Once a recovery credential is
set, the serial console requires an `unlock` before it will accept anything
destructive, and both the console and the network interface share one per-peer
throttle that turns repeated failures into escalating lockouts. Provisioning got
simpler too: a blank filesystem image is now built on the host and flashed like any
other partition, so the special `formatfs` build that existed only to wipe storage is
gone entirely.

### Added
- The recovery serial console requires authentication once a recovery credential is set. `unlock <credential>` opens the session; `setpin`, `apply`, `allowdowngrade confirm` and `resetjournal confirm` are refused until it succeeds. `status`, `help`, `reboot` and `cancel` stay open so a locked-out operator can still see state and back out of a bad transfer.
- Failed credential attempts feed a per-peer leaky bucket shared by the network interface and the serial console: five failures inside ten minutes lock that peer out for thirty seconds, doubling to thirty minutes, with a global backstop and a separate lockout per station so one attacker cannot deny service to everyone. Blocked-peer and failure counts are reported in the recovery status JSON.
- `help` (or `?`) in the recovery console lists every command with its arguments, and an unrecognised command now says what was typed and points at `help` instead of failing silently.
- A trial image that gets rolled back now leaves a breadcrumb naming which check rejected it - setup timeout, loop stall, health timeout, supervisor allocation failure, or a refused OTA journal - along with how long it had been running. All five previously arrived as one indistinguishable rollback, and the image itself is destroyed by the rollback, so there was nothing left to inspect. It appears in `otastatus` and is written to the durable event log.
- A `factory-flash` build target programs just the recovery updater and touches nothing else, with the same refusal on flash-encrypted builds that the other partition targets already had.
- Host-side unit tests for the authentication throttle compile the real device source and cover paced guessing, lockout escalation, slot eviction, attempts with no credential set, and the constant-time comparison boundaries.

### Changed
- Provisioning now flashes a blank LittleFS image built on the host, so the separate `formatfs` firmware variant that existed only to erase the data partition has been removed along with its command. The migration tool warns that the data partition is replaced rather than formatted on a later boot.
- On flash-encrypted builds, `littlefs-flash` refuses to run rather than quietly writing a plaintext filesystem image to a partition the device will only read as ciphertext.
- The recovery HTTP server accepts request headers up to 2 KB, so browsers that send large cookie or header sets no longer get a 431 on the upload page.
- An upload or apply request that arrives while another OTA operation is already running is refused straight away instead of waiting a second first. The recovery server runs on a single task, so that wait stalled every other connection - including status and cancel - before failing anyway.

### Fixed
- First-time setup no longer trips the OTA probation deadline. Reaching the interactive setup wizard, with the filesystem confirmed mounted, accepts the running image immediately - the wizard can only be reached on a cable-provisioned device, because an OTA-delivered image arrives on a filesystem that still holds its user database.
- Recovery URL matching stops at `?` or `#`, so a request with a query string reaches the handler it names instead of falling through to a 404.
- A recovery peer whose address could not be read no longer shares a throttle slot with the peer at address zero.

### Security
- The recovery console's destructive commands are no longer reachable by anyone with a USB cable once a credential is provisioned. This is a deterrent, not a boundary: physical access still allows a full re-flash, and only flash encryption plus secure boot make that a real barrier.
- A failed unlock attempt does not refresh the recovery updater's activity timer, so a guesser cannot hold the recovery session open indefinitely by typing wrong credentials.

## [0.99.8] - 2026-08-05
This release adds signed over-the-air updates with a recovery path that cannot be bricked. An opt-in partition layout puts an immutable, pure-ESP-IDF recovery updater in the `factory` slot and moves the application into `ota_0`. The recovery updater is the only thing that ever writes the application slot, and the bootloader falls back to it automatically whenever the application fails validation, so a bad image, an interrupted write, or a power cut mid-flash always lands on a device that can still be talked to. Firmware is authenticated by an RSA-3072 signature over a manifest that commits to the image digest, size, board, layout and version; the signature is checked when the image is staged, again before it is applied, and a third time inside the recovery updater before a byte of flash is touched, after which the written slot is read back and re-hashed. A new image then serves a 60-second probation and is rolled back automatically if it cannot prove it is healthy.

The R1 ring link was rebuilt around a single serialized owner that turns every ring write into a tracked transaction with a real acknowledged outcome, and the daily health history it collects is now merged, validated and stored encrypted on flash so it survives a reboot. ESP-NOW stops being a single-hop star: nodes now carry each other's traffic, so a peer that is out of radio range is reachable through a neighbour, and two devices that have never heard each other can pair and hold an encrypted conversation through a relay that cannot read a word of it. The glasses gained explicit, verifiable control over device-side settings, with the policy you asked for always reported separately from what the device actually confirmed.

The release also makes the device far less willing to destroy your data. A filesystem or NVS problem at boot is now reported and preserved rather than silently wiped, and three long-standing holes are closed: an unauthenticated remote crash reachable through the HTTP Basic auth header, a filesystem permission check that could be sidestepped by respelling a path, and session identifiers being written in full to a log any authenticated user could read.

### Added
- Signed recovery OTA, enabled with the opt-in `HW_OTA_LAYOUT=1` build on 16 MB FeatherS3 boards. The `factory` partition holds a recovery updater that owns the application slot; migration to the layout is a one-time cable operation that preserves NVS.
- `otawrite` streams a firmware image to the device over the encrypted Bluetooth channel. A dropped link no longer discards the transfer - the partial image is kept, and a later `otawrite begin` under the same size and digest contract re-hashes what is already on flash and resumes from that byte.
- Nine `ota*` commands cover the whole lifecycle: `otastatus` reports journal and image state, `otapin` provisions the recovery credential, `otawrite` uploads, `otastage` validates and journals a candidate, `otaupdate` applies a staged pair, `otarecovery` boots to recovery for a direct upload, `otacancel` backs out, `otaack` acknowledges a durable result, and `otaresetjournal` repairs the journal from serial.
- The recovery updater brings up a WPA2 SoftAP and an authenticated HTTP interface for uploading and applying firmware, and exposes the same operations on a serial console so bench work needs no network join. It refuses to start the radio at all until a recovery credential has been provisioned.
- `tools/ota` provides host-side manifest and bundle signing, reproducible bundle output, device backup and restore with integrity verification, and a build-time audit that refuses to flash unless the application and recovery updater were built from the same signing key and partition layout.
- ESP-NOW is now a real multi-hop mesh. Broadcast text and time sync flood the whole mesh and each node acts on a message exactly once, `espnowmeshroutes` shows what this node can reach and through which neighbour with hop count and path signal strength, and `espnowmeshrelay <0|1>` opts a node out of carrying other nodes' traffic while still using multi-hop for its own.
- Ring health-collection and low-power policy can be set from the `/r1-health` web page and the OLED R1 Health screen as Preserve, On or Off. The choice is saved and re-applied on every reconnect, defaults to Preserve so a first boot never changes your ring, and every surface shows the policy you asked for separately from what the ring reported back.
- R1 daily history is kept on flash under `/logging_captures/r1-health-history`, so trends and activity survive a reboot. Files are named with a keyed pseudonym rather than the ring's MAC address, are sealed at rest according to the capture-encryption setting, and are pruned to the newest 30 days.
- The G2 Health menu gained an Activity screen showing the day's steps and active, resting and total calories, along with how many of the day's 144 ten-minute slots the ring has actually delivered. The same figures appear on the web page.
- `g2control` sets and reports a persisted policy for the glasses' head-up display and native notifications, with a background worker that re-applies and verifies your choice across reconnects and reports the phase and reason in plain English.
- `g2nativeconfig` replays the official app's captured dashboard, notification and menu configuration messages, and `g2nativeconfig selftest` checks offline that the firmware still builds every captured message byte-for-byte.
- `healthstatus` now reports ring setup state and protocol profile, desired versus observed collection and low-power state, local logging status, typed daily history, and the state of the on-disk store, with `history`, `force-history` and `refresh-controls` subcommands.

### Changed
- Ordinary non-OTA builds are unaffected by the OTA work: the subsystem compiles down to about 800 bytes of stub commands that explain why OTA is unavailable, and costs no static RAM at all.
- Installing an older signed image is refused unless you ask for it explicitly. The device compares the candidate's version against what is installed and requires `allow-downgrade` to go backwards.
- Every write to the ring goes through one serialized owner task. Commands such as `ringquery` and the Poll Now buttons now answer with a transaction handle and the word queued rather than claiming the frame was sent, and each transaction ends in a real outcome: acked, verified, refused, timeout or disconnected.
- Connecting to the ring walks pairAuth, deviceInfo, time sync and advStart one acknowledged step at a time and fails outright if any step is refused, reporting which stage failed and why. Firmware-specific payloads are only used when the ring reports a profile that was actually validated; any other firmware is treated as read-only.
- Ring setup now requires a real host clock rather than adopting the ring's own time, so a dark boot can no longer push a 1970 epoch that would corrupt the ring's daily history buckets.
- History refresh is one coordinated sweep over heart rate, HRV, SpO2, sleep and activity instead of separate per-metric queries, skipped when a sweep already ran recently unless an admin forces it.
- Health Track is now Health Logging everywhere, including the `healthlogging` command, so it is clear this is HardwareOne's own local capture of ring vitals and is independent of whether the ring itself is collecting.
- `espnowmeshttl` now controls something real: it is the hop budget stamped on relay-eligible frames. The `adaptive` sub-verb and the `espnowmeshadaptivettl` setting are gone, because they toggled a mode no code ever read.
- Traffic returns to a direct radio link the moment a relayed peer becomes audible again, and fails over to a relay path when it goes quiet, with no manual step.
- Connecting to the glasses no longer silently rewrites their notification settings. Enabling native notification cards is now the explicit `g2notifenable` action, and `g2glasses` writes are refused unless the connected temple reports a firmware version the settings profile was validated against.
- A LittleFS mount failure no longer reformats the data partition, and an NVS partition reporting no free pages or a newer layout is no longer bulk-erased at boot. Retained files, WiFi credentials, BLE bonds and capture-sealing material are preserved and repair is left to an explicit tool.
- `oledbrightness` applies to the panel immediately rather than waiting for the next reboot or wake.

### Fixed
- The OLED no longer sits on a stale frame after a change made over CLI, web, Bluetooth or ESP-NOW, and a change that lands while a frame is already being pushed over I2C survives into the next frame instead of being swallowed. `oledflip` no longer leaves the screen black, and `oledtext` updates the message when you are already on the custom-text page.
- Ring notify frames are copied out of the Bluetooth callback and parsed on the owner task, keeping heavy work off the radio stack's small stack. The same change was made for glasses traffic, which is now reassembled across protocol frames and checked against the checksum of the whole message.
- Repeated or retransmitted ring frames are recognised and skipped, so a duplicate daily page can no longer be counted twice into the day's history.
- Trends and graphs can no longer show a mix of two days or two different rings, and the health sample series is protected against concurrent access so a graph render cannot read a series while it is being rewritten.
- A day is only reported as complete when the activity page is present and verified for the whole day; anything less is honestly labelled partial.
- Fragmented ESP-NOW messages report their true reassembled length instead of guessing from whichever fragment completed the set, and a broadcast that loops back to its sender is discarded instead of being shown as an incoming message from yourself.
- A peer restored from saved settings is no longer treated as something this node can currently hear, so sends to a paired but out-of-range peer take a relay path instead of a link that is not there.
- Cached glasses settings are wiped when a temple reconnects, and firmware version is tracked per temple, so one arm's values can no longer masquerade as the other's.
- `closeg2 full`, `g2deinit` and the automatic Bluetooth stack recycle no longer free the glasses' buffers and GATT client while a worker might still be using them; they report that teardown was deferred instead.
- Deriving the Bluetooth secure-channel key from a passphrase yields periodically so a handshake cannot starve other tasks and trip the task watchdog, and a failed derivation aborts the handshake rather than continuing with a zeroed key.
- The file APIs use the shared URL decoder, so filenames containing `+`, `#`, `%`, spaces or UTF-8 characters round-trip correctly.

### Security
- Fixed an unauthenticated remote reboot. An oversized HTTP Basic auth header decoded to exactly the size of the receiving buffer and wrote its terminator onto the stack canary, aborting the device - on a code path that runs before the login lockout and before any user validation, so it needed no credentials and was not rate-limited.
- Fixed a filesystem permission bypass. Paths were checked in whatever spelling the caller sent, so a path with no leading slash, a `.` segment, or surrounding whitespace matched no rule row and fell through to a permissive default while the storage layer still resolved it to the real file. Paths are now rebuilt from their segments before the permission lookup.
- Session identifiers are no longer written in full. The web CLI mirror, readable by any authenticated non-guest, carried the complete session cookie for every login, and an SSE diagnostic dumped the live session table under a debug flag that is on by default. The mirror carries no part of the session ID and the debug lanes keep only a short prefix.
- The OTA signing key never enters the repository. Build tooling passes only a key path and receives the signature through a temporary file, the generated OTA sdkconfig is ignored by git, and a missing key fails the build loudly rather than silently producing an unsigned image.
- No unverified byte can become bootable. The recovery updater verifies the signature before erasing anything, re-reads and re-hashes the written slot against the signed manifest, and only then sets the boot partition; a failed or abandoned upload leaves the boot selection untouched.
- Sensitive ring payloads are no longer written to logs. The device serial number, algorithm key material, NV-recovery blob and stored user demographics are reported as a redacted byte count instead of a hex dump.
- Ring and glasses writes that change device state now require authorization. `ringquery raw` needs admin plus an interactive confirmation for every SET and refuses a raw userInfo SET outright, and the generic `g2probe` escape hatch can no longer reach the services that mutate dashboard, menu, notification and device settings.
- A mesh time sync that would shift the clock by more than an hour is rejected once the device already has a time, so a stale or replayed frame cannot silently corrupt timestamps on logs and health data.
- Relayed ESP-NOW text from a device this node has never paired with and has no route to is dropped, multi-frame messages that are not end-to-end encrypted are refused, and pairing, heartbeats, route advertisements and the bonded remote-execution channel are single hop by design so joining a mesh stays proximity-scoped.

## [0.99.7] - 2026-08-03
This release turns the G2 lens into a more complete control surface and hardens the storage and Bluetooth paths underneath it. Maps now render as an interactive three-pane page with pan, zoom, recenter, GPS/fix status, and name search. The camera viewfinder can take snapshots, start or stop AVI recording, open settings, select FPS, and choose one of four tone maps. Image pushes automatically use bare-block LZ4 when it saves bytes, reducing BLE traffic for maps, camera frames, health graphs, and other lens visuals while retaining raw transfer for incompressible frames.

BLE recovery now treats repeated instant connection refusals as evidence of a wedged host stack rather than retrying forever. When no peripheral is linked and the worker is idle, the firmware records the failure and safely recycles Bluedroid. The same release closes a broad set of filesystem races: the global lock exists before the first boot-time file access, SD lifecycle transitions and long-lived handles are serialized, and optional radio-callback diagnostics drop work instead of blocking the host task.

### Added
- `g2glasses` reads and writes supported glasses-device settings, including brightness, auto-brightness, wear detection, display position, silent mode, and unit preferences, and reports the glasses' own acknowledgement.
- G2 Maps gains native move controls, live GPS/fix status, feature-name search, recentering, and highlighting alongside the rendered map.
- The G2 camera viewfinder gains Snapshot, Record start/stop, and Settings controls. Lens camera settings now expose FPS plus Linear, Balanced, Shadows, and Legacy tone mapping.
- G2 System Events is now a paged, wrapped snapshot of the recent event ring instead of an eight-row clipped live panel.
- G2 image tests gain bare-block LZ4 mixed-container canaries and a raw-versus-compressed benchmark.

### Changed
- G2 image transfer selects LZ4 `CompressMode=2` only when the compressed BMP is smaller; current verified glasses firmware paints the bare-block format, and raw remains the fallback. External protocol decoders must understand mode 2 when inspecting these pushes.
- Camera power work is allocated lazily, avoiding the permanent worker stack on builds that never turn the camera on.
- Health Track always resumes in per-day CSV mode and keeps its first sample due while waiting for the ring to reconnect.
- The checked-in build profile now targets the XIAO ESP32-S3 Sense: 8 MB flash, octal PSRAM, camera and microphone enabled, with I2C, OLED, and gamepad disabled. Bonded mode is enabled in that profile.

### Fixed
- Repeated G2/R1 connection failures now produce durable diagnostics, and a wedged BLE host can recover without rebooting the device.
- AVI start/stop is serialized across lens and web callers; short SD writes stop cleanly, final header patches are checked, camera shutdown no longer waits while holding the camera lock, and the web player can recover complete frames from an unfinalized clip.
- Filesystem locking now covers SD mount/unmount, camera and microphone captures, sensor logs, model loads, manifests, backups, and other long-lived file operations. Cross-core glasses settings snapshots and acknowledgements are read coherently.
- Health graph axes no longer turn synthetic backfill times into false wall-clock labels, jitter at minute boundaries, or show indistinguishable endpoints for a 24-hour window.
- Boot-folder promotion uses collision-proof names and never overwrites a prior capture; failed or unrecognized entries no longer make a folder look finished.
- Notification mute/force policy now covers all event kinds above index 127 and fails the build if the event list ever outgrows its mask again.
- The timezone is applied immediately after settings load, so early crash-history and boot-time file timestamps no longer appear in UTC while later logs use local time.
- The LZ4 encoder handles short inputs without forming out-of-range pointers, and its production image path remains bounded to the supported input size.

### Security
- Normal S3 board defaults now pin NVS encryption off as well as flash encryption off, preventing stale flash-encryption trial state from selecting HMAC key protection and burning an eFuse on an ordinary board.

## [0.99.6] - 2026-07-30
This release ends a heap mystery and lays the groundwork for flash encryption. The mystery: the web output mirror's trim loop had an off-by-one that let its string terminator land one byte past the buffer, zeroing the low byte of whatever heap block header sat next door - usually an lwIP connection block the web server allocates moments later - so the eventual free of that neighbour asserted deep inside the allocator, minutes or hours after the actual overwrite and nowhere near it. That is the crash the TLSF investigation had been chasing with the new crashlog. The glasses protocol's per-stream stats table had a smaller cousin: two tasks appending concurrently could both pass the bounds check, and the loser wrote a whole entry past the array.

The flash-encryption groundwork is config-only and changes nothing about a normal build: a dedicated `feathers3_fe` board variant carries the encryption trial config (AES-128, development mode, encrypted NVS, partition table at 0x9000) while the regular boards now pin encryption explicitly off, guarding against a stale setting from an encrypted build silently producing a bootloader that permanently encrypts whatever board it boots on. Every partition layout moves NVS to 0xA000 and gains the small key partition the encrypted build requires - inert without encryption, though the NVS move reformats it once (saved WiFi credentials re-enter on next connect).

### Added
- G2 Health graphs grew a time axis. Trends label hours into the ring's day ("0:00 / 12:00 / 24:00"), live views label local wall-clock time when the clock is synced and elapsed-from-start when it is not, and a graph with no honest time domain shows no axis rather than a misleading one. Labels are anchored to the data window, not the render instant, so a repaint never moves them.
- `feathers3_fe` board variant for the flash-encryption trial (config only - nothing burns fuses until it is deliberately flashed to the sacrificial board).
- `tools/command_registry.py`: generates docs/COMMAND_REFERENCE.md from the command registry source, and audits for drift - duplicate names, command tables never registered, commands invoked in code that resolve to nothing.

### Fixed
- Web mirror buffer one-byte heap overflow (see above) - the trim bound now keeps the terminator inside the allocation, in both append paths.
- Stopping the HTTP server from a command (`httpstop`, `closewifi`, radio power-off) while a web request was mid-flight deadlocked both sides for 60 seconds - the handler was waiting on the command pipeline and the command was waiting on the handler's task to exit - freezing every command surface (G2, OLED, MQTT, automations) with them. The stop now defers to the main loop when a request is in flight and completes a moment later.
- SSE fetch frames undercounted their fixed framing by 4 bytes, so payloads large enough to spill to the heap were truncated - dropping the blank line that terminates an SSE event and stalling the client's parse of everything after it.
- G2 protocol per-stream stats: lookup-or-append is now locked; concurrently recording streams could corrupt neighbouring memory (see above).
- G2 WiFi menu: the Auto Start toggle submitted `wifiautoreconnect` instead of `wifiautostart` - it appeared to do nothing while silently clearing the unrelated reconnect preference.

### Changed
- Board-defaults `=n` guard lines are now satisfied by sdkconfig's "# not set" form, so a guard no longer strips and rewrites sdkconfig on every configure.
- The in-tree sdkconfig now reflects the `feathers3_fe` trial build; plain-board builds recover automatically via the new board guards.

## [0.99.5] - 2026-07-30
This release is mostly about time. The firmware only ever applied the configured timezone as a side effect of NTP setup, so any boot that never joined WiFi silently rendered every "local" timestamp as UTC while claiming otherwise - dated capture folders, sensor-log day rollover, log-line prefixes, the date shown on the glasses, and the hour a TIME automation fired were all shifted. The timezone is now applied on every boot path. Underneath that, the five ways the clock can be set - NTP, the DS3231 RTC, manual `timeset`, the R1 ring, and background SNTP corrections - each hand-rolled a different subset of the chores that should follow a clock change (sync event, boot anchor, RTC write-back, scheduler wake, pending-timestamp resolution); those now funnel through one chokepoint so every source gets all of them. The ring earns a promotion out of this: it has a battery-backed clock of its own, so a boot with no other time source now adopts the ring's time, the connect ritual echoes the ring's clock back instead of stomping it with a 1970 epoch (which had been corrupting the day boundaries of the ring's own stored history), and once real time arrives a corrective push repairs the ring - and the glasses - automatically.

Two new subsystems ship alongside. First, a crash now leaves a note: a panic-time hook copies the assert text, faulting core and program counter, a backtrace, and a boot-phase marker into memory that survives the reboot, so the next boot can print what happened at the very top of its output (even when the device is boot-looping before the console comes up), append it to a persistent crash-history log, and serve it anywhere via the new `crashlog` command. Second, health captures are encrypted at rest by default: rows are sealed as they are written using a key minted on and never leaving the device, viewers you are authorized to use (CLI, web, display, glasses) decrypt transparently, and raw byte surfaces - file reads, downloads, ESP-NOW transfers - deliberately ship ciphertext, with `capturecrypt export` as the explicit plaintext escape hatch. Closing that loop also closed a real hole: the health-log merge command carried unscoped system authority and could truncate the command audit log.

The third thread is the glasses. Typing used to rebuild the whole keyboard list on every keystroke, resetting the highlight and scroll each time; the keyboard is now a character list on the left with a live buffer panel on the right, keystrokes patch only the panel, and switching character groups is a fast in-place refresh instead of a full page swap. Password keyboards leave no trace in the logs, and every other tap now logs which row you tapped and where it went. Below the surface, the Bluetooth plumbing shared by the glasses and the ring got a hardening pass that includes a genuine memory-corruption fix (a fragment-count error that wrote past a stack buffer for particular payload sizes), bounded connect timeouts where an infinite wait could wedge the shared connect worker for hours, per-link MTU negotiation that stops one device's preference from breaking another's, and several object leaks - one of which made a manual ring disconnect/reconnect come back mute every time. And as usual, a crop of things that looked fine turned out to be quietly broken: an idle ring was fabricating a fake health sample every five seconds and wiping a day of Trends history in eight minutes, the web Games gamepad panel had been polling an API renamed two releases ago, and two sensor drivers responded to a failing sensor by crashing the entire device.

### Added
- **Crash forensics.** A panic hook records the last crash (panic/assert text, core, PC, up to 12 backtrace frames, build id, boot phase) in RTC memory that survives the reboot. On the next boot it prints a `=== PREVIOUS CRASH ===` banner over the raw ROM console - visible even in a boot loop before anything else comes up - lands in the boot crash event, and is appended to `/system/sys_logs/crash-history.log` (64 KB, oldest-trimmed). New `crashlog [json]` command on every surface shows it, including a consecutive-crash counter and a same-crash repeat streak that resets when you flash a fix. Instrumentation only: it never gates boot behavior.
- **Capture at-rest encryption.** New setting `captureEncryptMode` (Off / Health / All, default Health): capture sessions that include R1 ring data are sealed at rest. Sealed files keep their names and plaintext header, but every data row is encrypted with a device-local key held in NVS; a file is always entirely sealed or entirely plain, and a session refuses to start (with a clear error) if sealing is required but the key is unavailable. New admin command `capturecrypt [status|off|health|all|export "<in>" "<out>"]`; export writes a decrypted copy inside the capture tree, since the key never leaves the device. The file viewers on the CLI (`fileview`), web Logging page and file viewer, display file browser, and glasses Files page decrypt transparently for authorized users; `fileread`, raw downloads, and ESP-NOW transfers deliberately ship ciphertext. `healthstatus` reports the mode and whether the live session is sealed. Existing plaintext files are left exactly as they are.
- **The ring as a backup clock.** On a boot with no time source, connecting the R1 ring adopts the ring's battery-backed time (broadcast as `[RING] Adopted ring clock`), which un-breaks automations, dated capture folders, and date display downstream. Once NTP or a manual set arrives, a corrective push repairs the ring; the same drift rule (2 minutes) keeps the glasses' clock and timezone correct mid-session instead of only at connect.
- Ring connect failures are now loud: broadcast messages on every failure path (link timeout, missing service or characteristic, submit refused, attempt stuck) and a throttled `ring_reconnect_failed` entry in the event history. Previously all of it was debug-only silence.
- Glasses tap logs name what you tapped: `Hijack tap: item N (label) -> target` instead of a bare index, resolved against the rows actually on the lens. During a password keyboard the log says only `keyboard (secret)`.
- `ntpstatus` gains a `Time source` line (none/rtc/ntp/manual/ring), and `ntpsync` now reports what actually happened: server replied, clock kept while NTP retries in background, or time taken from the RTC instead.
- Reset reasons USB, JTAG, eFuse, power glitch, and CPU lockup are now labeled instead of rendering as "Unknown" - USB being what an ordinary reflash produces.
- The glasses Files page shows sealed captures with an `(encrypted)` mark in file info, and the path readout is a taller wrapped box (up to 95 characters) instead of one truncated 36-character line.

### Changed
- **Breaking for anything parsing sensor JSON:** the derivable `age` key is removed from every sensor reading (staleness comes from `ts`, which is now always present); `anoencoder json` moved to the standard envelope (`schema` and `position` gone, `position` -> `pos`); IMU `temp` is now an integer and accel has 2 decimals (the hardware never had more - and the narrower payload stops motion readings being dropped by the mesh size gate); `thermalread json` lost its stray `enabled` key; and `/api/sensors/status` renames its non-I2C map from `sensors` (which collided with an array of the same name elsewhere) to `nonI2c`. Failure replies now keep the envelope shape too, instead of a bare `{"valid":false}`.
- **Breaking for external readers of health captures:** with the new default, capture files containing ring data are ciphertext off-device. Set `captureEncryptMode` to off, or use `capturecrypt export`, if you post-process them elsewhere.
- **Breaking:** `healthlogmerge` and `gpstrackmerge` now refuse output paths outside the capture tree - and refuse before truncating anything. `healthlogmerge` also refuses to stitch sealed and plaintext inputs together.
- **Breaking for automations:** an automation more than 15 minutes overdue (device was off, or the clock jumped) is advanced to its next occurrence with a logged event instead of firing late; `automation add` rejects interval triggers under 1000 ms (they rewrote the automations file every loop pass - real flash wear); and `IF SATS` conditions fail closed while GPS is deliberately off, instead of firing continuously against a zeroed reading.
- **Breaking:** the display's default orientation is no longer flipped 180 degrees (`oledFlipped` default true -> false). A device without a saved value for this setting will boot rotated relative to before.
- `automation recompute` actually recomputes now. The old implementation was a no-op for single-trigger automations and actively corrupted multi-trigger ones; the new one recomputes each clock trigger with the scheduler's own arithmetic and leaves manual/boot/event triggers and armed timers alone.
- `time` reports the real sync ledger (`source: rtc|ntp|manual|ring` or "carried over soft reboot") instead of always implying NTP, and RTC-sourced time displays as local time - it was raw UTC on RTC boards but local on NTP-only boards. `timeset` rejects values outside 2020-2099 and malformed input (`timeset 100` used to silently un-sync a good clock), and runs the full post-sync chore chain like every other source.
- First NTP sync after WiFi is faster and more reliable: the random 0-5 s SNTP startup delay is cut to 100 ms, and the two backup servers actually register (the sdkconfig allowed exactly one server, silently dropping the rest). Background hourly SNTP corrections are now visible to the rest of the system instead of stepping the clock behind its back. The DS3231 is written back whenever any non-RTC source sets the clock.
- The glasses keyboard is a compound page: characters on the left, a live buffer panel on the right showing the prompt, wrapped text with a cursor, and an (n/max) counter. Rows reordered - Cancel/Space/Backspace/Done, then prev/next group rows replacing the single forward-only cycle; the buffer is no longer a tappable row. Group switching is an in-place ~80 ms refresh with a full-swap fallback.
- The three glasses worker stacks grew by ~10.5 KB total, each sized from a measured worst-case call chain rather than a guess - one of them was 112 bytes short of its real worst case, a latent overflow. Each worker now reports its exact stack peak when it sets a new one.
- The web Bluetooth page polls status 4x slower in the background (6 s active / 60 s idle): every poll is an audited command, and an overnight review found it writing ~800 audit-log lines in four minutes. Action-triggered refreshes still give immediate feedback.
- Temple and ring BLE connects use a hard 35-second timeout instead of waiting forever, so one lost connect event can no longer wedge the shared connect worker - the suspected cause of a two-hour no-reconnect window in field testing. A 240-second watchdog also clears a stuck in-flight connect flag rather than refusing all future attempts.
- Note for builders: the BLE lifecycle work depends on a locally patched Arduino BLE library (a `connectTimeout` overload and a GATT-interface accessor); building against a stock copy of the library will fail. The patch file, a verify script, and instructions are now tracked in `docs/arduino-local-patches/`.

### Fixed
- **Local time was UTC on every boot that never joined WiFi.** Dated capture folders, day-file rollover, log prefixes, the glasses' date, and TIME automation firing hours all silently used UTC while labeled local. The timezone now applies on every boot path, immediately after a timezone change (no WiFi needed), and on leaving the setup wizard's timezone page - which previously left the running system on the old offset until reboot.
- **An idle ring destroyed its own Trends history.** Every live-cache resample was stamped with the current time, defeating the deduplication gate, so an unchanging reading fabricated one sample per metric per 5 seconds and refilled all 96 history slots in about 8 minutes - evicting a full day of real data. Samples are now stamped with their actual receive time and an unchanged reading dedupes forever.
- Trends drawing and labeling: daily backfill now spans its real 24-hour window instead of compressing into half the plot; sparklines draw in time order (the polyline used to jump from the newest live point back to the start of history); and screens claim a date only when one exists - "Trends (Jul 29)" with a real clock, "boot N" without, never a false "today".
- **A specific payload size corrupted memory on the glasses link.** The fragment counter folded the trailing CRC into its arithmetic, so for payload lengths just under a fragment boundary it over-counted, underflowed a size, and memcpy'd wildly over a stack frame. Fragments are now counted from the payload alone with a CRC-only tail fragment when needed, matching the reference implementation.
- **Manual `ringdisconnect` then `ringconnect` came back mute.** Disconnect dropped the client object without freeing it (leaking ~10-14 KB per cycle) and stranded a stale flag that gated all transmission off forever. Teardown now frees clients behind proper gates - or deliberately leaks one it cannot prove safe to free - and clears the flag on successful connect.
- A late MTU negotiation no longer strands a link at 20-byte chunks: the send path re-reads the live per-link MTU instead of trusting a snapshot taken before negotiation finished. The ring also stopped setting the process-global MTU preference downward, which had been breaking glasses discovery after a ring connect.
- Disabling and re-enabling the glasses client now cleanly closes the ring link too, so the next `ringconnect` is not refused with "already connected".
- The gesture and presence sensor tasks crashed the whole device when auto-disabling after repeated I2C failures - the shutdown path returned from the FreeRTOS task entry point, a guaranteed panic. Both now shut down cleanly.
- The RTC driver no longer mutates the process-global timezone to do UTC conversions (a window where every other task got wrong local time, and a failure mode that could pin the device to UTC all boot), and it refuses to adopt implausible RTC time - a dead coin cell reporting year 2000 used to step a good clock back decades.
- After `closegps` / `closertc`, consumers no longer see stale valid readings: GPS-gated automations see the fix drop immediately, and the RTC envelope stops reporting valid data from a stopped sensor.
- Sensors hot-plugged and opened after boot now appear in `sensors json`; their readings were silently dropped until reboot because the aggregate gated on the boot-scan flag. A gamepad on the alternate I2C address auto-starts correctly, and reboot-and-resume marks the input device's autostart failures again ("gamepad" -> "input" rename fallout in both cases).
- The web Games page gamepad panel worked again after being silently dead since the input rename two releases ago - it polled `sensor=gamepad` and read a key the server no longer sends.
- The dashboard's sensor-status stream could truncate mid-event and swallow the following system event, freezing that panel; status events are no longer copied through a fixed buffer smaller than their producer allows. `/api/sensors` tof and imu return an explicit unavailable error instead of an empty body that made browser JSON parsing throw.
- Large BLE debug flushes silently duplicated or vanished: the flush buffer exceeded what the BLE attribute accepts, and an oversize write keeps the previous payload - so every size-triggered flush re-notified stale data. The buffer now fits the transport.
- Log timestamp prefixes are correct immediately after any clock change; they previously latched an offset computed from the first pre-sync log line and only some sources ever refreshed it.
- ESP-NOW peers no longer receive every boot notification twice, and a boot notification can no longer be permanently suppressed by the heartbeat task failing to start.
- Boot anchors are upserts now: a boot that anchors twice (ring adoption, then NTP minutes later) updates its anchor in place instead of appending a second row that always lost to the first, and the user-timestamp resolver holds the filesystem lock across its whole read-modify-write so a concurrent user add is not clobbered.

### Security
- **`bleevent` could smash the stack and inject JSON.** The message from this non-admin command was interpolated unescaped into a fixed buffer with unchecked appends: a long message wrote past the stack frame, and a quote in the message could inject arbitrary keys into the event JSON delivered over BLE. Messages are now escaped with a bounded escaper and every append is bounds-checked.
- **`healthlogmerge` could truncate the command audit log.** It ran under an unscoped system identity that bypassed path scoping entirely, so an admin could target `/system/sys_logs` files that admins are deliberately not allowed to write. It and `gpstrackmerge` now run under identities scoped to the capture tree and refuse out-of-tree outputs before opening (and truncating) anything.
- FM radio RDS text - station name and radio text, controlled entirely by whoever broadcasts - is scrubbed to printable ASCII at ingest. An unescaped quote in radio text could previously inject keys into the sensor JSON envelope, poison the mesh remote-sensor response, and freeze the FM web panel. One scrub point fixes every consumer; the cost is that non-ASCII station names render as spaces.
- Credentials stop leaking into logs: the echo of a bonded remote command (`@login bob hunter2`) is redacted before it fans out to every output sink including the log file, and a mistyped credential command (`logni user pass`) has its arguments masked in the audit log instead of landing verbatim.
- Password entry on the glasses is fully mute in the logs - no row index, no label, no buffer content - via an explicit secret-session contract on the keyboard.

## [0.99.4] - 2026-07-27
Capture files can now tell you when they were written. A board without an RTC does not know the time at boot, so a session started before the clock syncs was named by boot counter and uptime - ordered, but undated. This release records one time anchor per boot the moment real time is learned, which makes the files written earlier in that boot datable after the fact, and a background sweep moves them into dated folders. CSV capture also targets one file per calendar day and stamps its rows with wall-clock time, so a day of health data graphs as one continuous series instead of a scatter of per-session files.

The other substantial thread is working out how the G2 glasses actually behave on Bluetooth, so that pairing and talking to an R1 ring at the same time works properly. Wearing both means three links sharing one controller, and nearly everything that went wrong there came from assuming what the controller would accept instead of measuring it. Two new diagnostics do the measuring - one probes the connection-interval range the controller will actually admit, the other lets image-transfer pacing be tuned by hand - and what they turned up accounts for most of the trouble people hit with a ring connected: the interval the firmware had been compiling in was refused outright once several links were up, a refused request was cached as though it had succeeded so every later attempt was skipped, and pushing an image to the lens dropped both temples to a slower interval for the whole transfer. Link speed is now arbitrated between the things that need it rather than set directly by whoever asked last, and transfer pacing follows the interval actually in force rather than whether a ring happens to be connected.

Alongside that, a run of things that looked like they worked turn out not to have been running at all. Choosing a saved WiFi network on the display did nothing, because the menu invoked a command name that was only ever the C function's name and was never registered. The display's input toggle called `opengamepad` / `closegamepad`, which stopped existing when the gamepad and the rotary encoder moved behind a shared input layer. ESP-NOW pinned every peer to the channel it happened to be added on, so any later channel change silently killed key exchange to it. And the I2C subsystem classified every failure as the same kind of error, so a timeout, an absent device and a stuck bus were indistinguishable and two of the three responses to them could never fire.

### Added
- **Locked power mode** (`power mode locked`, `lock`, `max`, or `power mode 4`): holds 240 MHz through idle power-save, so the screen still blanks but the core never downclocks. It is also a row on the display's Power -> CPU menu and on the glasses' Power -> CPU Power picker. The existing modes are unchanged; the display rows now show each mode's idle clock alongside its active one (Performance 240/80MHz, Balanced 160/80MHz).
- **Dated capture folders.** A single anchor line per boot (`.anchors.csv`) makes files written before the clock synced datable after the fact, and a low-duty background sweep promotes them from `boot-N/` folders into `YYYY-MM-DD/` ones - including on a later boot, for boots that ended before they ever learned the time. Three deliberate limits: the file an active session still holds open is promoted only after the session stops, a file whose computed date lands more than 40 days from its anchor is left boot-named rather than mis-dated, and the anchor registry keeps 32 boots. A boot that never learned the time keeps its `boot-N/` folder.
- **One CSV per day.** A capture session appends to that day's file instead of minting a new one, and rolls over quietly at midnight. A session that started before the clock synced rolls onto the day file as soon as it syncs. If the day's file has incompatible columns - because the sensor selection changed - the session mints a numbered variant beside it (`health-2026-07-27-2.csv` through `-9`), falling back to a per-session timestamped name rather than refusing to start. Rotation size is seeded from the file already on disk, so an appended file rotates on time instead of one full size late.
- **Paginated Files browsing on the glasses.** Directories past about twenty entries used to fail to render at all, silently, because the compound payload that builds the list ran out of buffer. They now page through twelve at a time with Prev / Next. The file-browser directory cache also grew from 64 to 256 entries.
- **R1 ring events**: `ring_connected`, `ring_disconnected`, `ring_worn` and `ring_not_worn`, available as automation triggers in the web builder and as notification-center entries, so an automation can react to putting the ring on or taking it off.
- **Measured ESP-NOW link quality.** Each peer now tracks the signal strength our own radio sees for that peer's frames, kept separately from the strength the peer reports about itself. The two answer different questions and only the first describes the link between the two devices.
- **`espnowstats` reports RX-ring drops.** The counter was already being incremented but had no display anywhere; it now appears as `rxRingDrops` in JSON and as a text line, and `espnowresetstats` clears it with the rest.
- **Two instruments for the glasses-and-ring work above**: `g2connpri` reports each temple's measured connection interval, latency and timeout, and sets the range to request - so you can find where the controller stops admitting requests rather than guessing; `g2envgap` overrides the gap between image envelopes, for tuning transfer pacing against a live link. Also `espnowrelayblock`, which drops every inbound frame from one MAC so a peer can be made to look out-of-range on the bench.
- New glasses Test Suite -> Image Tests -> Motion Tests submenu, with a safe container-reposition probe and a riskier one that is documented as able to wedge the lens firmware until Bluetooth is reconnected.

### Changed
- **Breaking for anything parsing capture CSVs:** the `timestamp_ms` column now carries wall-clock milliseconds since the epoch (13+ digits) whenever the clock is synced, and falls back to milliseconds since boot only when it is not. A parser that assumes small uptime numbers will misread synced files. This is what makes a day file a genuinely continuous series rather than several sessions concatenated.
- **Breaking for automations on builds without bonded mode:** the `BOND_MODE`, `BOND_ROLE`, `BOND_ONLINE` and `BOND_SYNCED` conditions now fail outright instead of returning placeholder values. An automation written against a placeholder - `BOND_ROLE == WORKER`, for instance - stops firing. Bonded mode is off in the committed build configuration.
- `i2cmetrics` reports every initialized bus separately instead of only the first, with its warnings scoped per bus so "high contention" says which bus to fix, and it now exposes the wait times, transaction durations and duration histogram it had been collecting but never showing. `i2chealth` gained a bus column - the device registry is keyed on address plus bus, so the same address can legitimately appear twice and was previously indistinguishable. Both report `schema: 2` in JSON.
- The capture selection is now protected while a CSV session is running: `sensorlog sensors <list>` refuses to change it mid-session, and `healthtrack on` during a running non-R1 CSV session restarts the session rather than appending seven ring columns under a header that does not describe them. Text sessions still allow live changes.
- CSV capture no longer writes a bare-timestamp row every few seconds when no selected sensor has anything to report. Text and track formats keep their heartbeat lines.
- The saved capture path now stores the un-shaped base path rather than the last session's dated one, so restarts stop compounding date-in-date folders and variant digits. A literal path you typed yourself is still stored exactly as typed.
- **Link speed to the glasses is now arbitrated rather than set directly.** Anything that genuinely needs throughput asks for it and releases it when done; overlapping requesters compose, and whoever finishes first cannot drag the link back out from under someone still using it. When nobody is asking, the glasses run their own idle power state machine, which is easier on their battery than being pinned fast. The compiled fast-interval floor also moved to the boundary the controller was measured to admit - the previous value was refused outright once several links were up, which is exactly the condition a ring creates.
- Boot-time command-registry diagnostics now reach you. The overflow warning was emitted before the debug system existed and was therefore discarded every time; it is now a system-event line that echoes immediately and persists. The module table also grew from 32 to 64 entries so the summary lists all of them on a full-feature build instead of quietly truncating.
- Committed FeatherS3 build configuration: GPS, thermal camera, RTC, on-device LLM and bonded mode are off, web speech and web bond pages are off, and the web MQTT page is on. Turn any of them back on in `System_BuildConfig.h` for your own hardware.

### Fixed
- **Choosing a saved WiFi network on the display did nothing.** The menu row ran `wificonnect`, which is the C handler's function name and was never registered as a command, so the selection silently went nowhere. It now runs `openwifi`.
- **The display's input open/close toggle did nothing**, for the same class of reason: it called `opengamepad` / `closegamepad`, removed when the Seesaw gamepad and the ANO encoder moved behind the shared input layer. It now calls `openinput` / `closeinput`.
- **ESP-NOW key exchange died silently after any channel change.** Peers were added pinned to the channel in force at the time, so an access-point roam or an `espnowchannel` change broke key exchange to that peer with nothing to indicate why. Peers now follow the radio.
- **Pushing an image to the glasses with a ring connected could wedge the link** - the most visible symptom of the coexistence problem above. Every push dropped both temples to a slower connection interval for its whole duration, backing the host queue up into aborts that stranded the write lock. Pacing, retry and backoff now key off the connection interval actually in force rather than off whether a ring happens to be connected.
- Three further connection-parameter faults found by the same investigation: a refused request stayed cached as though it had been applied, so every later attempt was skipped and the link sat at the wrong speed for the rest of the session; a reconnect inherited that stale cache and ran at the peer's default interval indefinitely; and an unconditional request fired on every temple connect always failed on one side, with an alarming log line to match.
- **I2C bus recovery recovered the wrong bus.** It counted degraded devices across the whole registry and then always reset the primary bus, so a cascade on the secondary bus tore down the healthy one and left the sick one untouched. Each bus is now judged and recovered on its own, with a quorum floor so a sparsely populated bus does not tear itself down over a single sick device, and a five-second cooldown so a persistently broken bus cannot recover in a tight loop.
- **I2C error classification never ran.** Every failure was recorded as a device NACK regardless of cause, so of the five error types three could never occur: timeouts never adapted a timeout, bus faults never triggered recovery, and the per-device TIMEOUT and BUS_ERR counters sat permanently at zero. Failures are now classified by asking the bus itself, and the status table was rewritten against the codes this build actually returns - the real timeout code was previously unhandled and would have been escalated to a full bus reset. The adaptive timeout, reachable for the first time as a result, now decays back toward its configured value instead of only ever ratcheting up.
- **I2C device health was blind to the second bus.** The address-keyed health helpers all resolved to the primary bus, so for a sensor on the secondary one every health check quietly did nothing: auto-disable could never fire, `i2crecover` reported the device as unregistered, and the thermal camera's diagnostics omitted their health section entirely.
- `i2chealth json` truncated into unparseable output past roughly eight devices, having been sized against a buffer a quarter of the real one, and dropped devices silently when it ran out of room; it now says how many it omitted. `espnowmeshstatus` had the same bug and failed with "Response too large" on larger meshes.
- Capture files could be written without a header: only the session-start create wrote one, so files created by a rotation, by the SD overflow mirror, or by the append path's rollover began mid-stream. A short or failed header write is now detected too, and reported rather than leaving a truncated file behind.
- Health Track restarts now shape their path like every other entry point, instead of writing to the bare configured filename.
- The map and track pickers on both the web interface and the display now descend into session subfolders, without which none of the dated or boot-numbered captures this release introduces would have been selectable. The display's picker additionally never looked in the capture directory at all.
- The glasses' Files page re-scans when you return to it from another page or after a timeout, so sizes and files written while you were away appear without a folder hop.
- Renaming the ESP-NOW identity from the glasses now quotes the submitted name, so a name containing a space is cleanly rejected instead of being silently truncated. The lens keyboard has no quote key, so this could not be worked around.
- Two non-default build configurations compile again: Bluetooth disabled, and bonded mode disabled.

### Security
- On builds without bonded mode, an inbound bonded remote command over ESP-NOW is now explicitly rejected instead of falling through to a path whose token validation does not exist in that configuration. Bonded mode is off in the build this release ships.

### Docs
- The README, Quick Start and User Guide gained sections for the glasses interface, user roles and access, notifications, and backup and restore, plus a per-board configuration table, the rotary-encoder input commands, and a fuller description of the setup wizard - including the identity question that disables the mesh entirely if you decline it.
- A batch of corrections to commands the docs described but that never existed under those names: `webstart` / `webauto` are `openhttp` / `httpAutoStart`, `battery status` is `batterystatus`, `espnow setname` is `espnowsetname`, `useradd`'s trailing flag forces a password change rather than granting admin, `usersync` takes six arguments, there is no web Pair page, and `opengamepad` / `closegamepad` / `gamepadautostart` are gone.

## [0.99.3] - 2026-07-26
Every feature now has two independent settings instead of one: whether it is allowed to run at all, and whether it starts itself at boot. Before this each feature had a single saved setting and that setting was the boot flag, so there was no way to say "keep this available, just do not start it on its own" - the feature switch and the start-at-boot switch were the same switch. Starting and stopping something for the session was always separate from that, and is unchanged. Alongside this, a long-standing fault in the settings-to-command binding is fixed: an audit of all 407 controls found 243 that silently discarded writes and 6 that ran a different command entirely - most memorably, toggling the camera's "Capture" debug flag took a real photo and saved it to storage.

### Added
- **Apps → Health** on the G2 lens (replaces the old Apps → Ring live-text vitals page): Pet-style left metric list + right 288×144 sparkline image for R1 HR / HRV / SpO2 / temperature / battery, with Poll Now, in-RAM history, and optional daily-history backfill when a metric is focused. CLI: `g2health`.
- **Apps → Health → Trends**: submenu for ring daily history (HR / HRV / SpO2 today + Refresh), graphed separately from live sparklines. Weekly aggregation deferred.
- **R1 wear detection** and **sample age** on Health Overview, OLED R1 Health, Web `/r1-health`, and `healthstatus` / `healthstatus json` (schema 2). Wear comes from `deviceStatus` byte[1]; ages prefer ring epoch timestamps when the wall clock is synced. Overview / OLED show one shared recentness (freshest vital, 5s buckets under 1 min) instead of a per-line age tick.
- **Health Track** — `healthtrack on` / Apps → Health → Toggle Track enables R1 logging and starts capture (persists for boot). While on, mines R1 vitals on a configurable interval (default 15 min; setting `healthTrackPollIntervalSec` / `healthtrack interval <sec>`). Apps → Health entry and Poll Now also write a sensorlog sample after the refresh when R1 logging is active. Setting `healthTrackingEnabled`. Still uses the shared sensor logger (`sensorlog sensors r1` remains available for manual control).
- **`ENABLE_R1_HEALTH`** compile gate (default on with Bluetooth + G2): R1 Health UI and Track can be stripped while keeping ring connect under `ENABLE_G2_GLASSES`. Web page gate: `CUSTOM_ENABLE_WEB_R1_HEALTH` / `ENABLE_WEB_R1_HEALTH`.
- **OLED R1 Health** mode (vitals + Poll Now + Track) and **Web `/r1-health`** page; Bluetooth OLED/Web keep ring connect only. CLI/BLE: `healthstatus` / `healthstatus json` / `healthstatus poll` (+ existing `healthtrack`, `ringquery`).
- **BLE mid-session auto-reseek:** with `bleautoreconnect <peer> on`, unexpected drops of R1/G2 now retry with backoff (boot reconnect already existed). Intentional `ringdisconnect` / `closeg2` does not reseek. Per-peer flag: `bluetooth.peers.<name>.autoReconnect` in `settings.json`.
- **`healthlogmerge`** — stitch health/sensor TEXT logs like `gpstrackmerge` (`"<out>" "<in1>" …`).
- Two switches for every feature: `<thing>enabled` decides whether anything may start it, `<thing>autostart` decides whether it starts at boot. Boot now requires both. Applies to the thermal camera, ToF, IMU, GPS, FM radio, gesture sensor, clock, presence sensor, input device, camera, microphone, speech recognition, on-device AI, web server, Bluetooth, ESP-NOW, display, camera AI, automations, and both logs. Every new switch defaults to on, so nothing changes until you deliberately separate them.
- WiFi gained a third switch, because "connect at boot" and "keep retrying after a drop" are genuinely different things: `wifienabled` (may WiFi run at all), `wifiautostart` (connect at boot), and `wifiautoreconnect` (keep hunting for the access point after an unexpected disconnect). The last one is what you want on a device you expect to stay online.
- The 2.4 GHz radio has its own section in the web interface's Network Services, instead of appearing as a badge inside the WiFi card. It is shared by WiFi and ESP-NOW, so it was never a WiFi property. The section explains why the radio is on ("held up by ESP-NOW"), says whether it will be on after the next reboot and what to change if you want that different, and has On/Off buttons. Powering it off warns first, since it drops WiFi and ESP-NOW together.
- `espnowcapturetosd` and `espnowcaptureskipheartbeats`. The ESP-NOW frame capture these control was already implemented and read these settings, but no command ever wrote them, so the feature could not be switched on from anywhere.
- `sensorlogenabled` and `systemlogenabled` as master switches for the two logs. Sensor logging in particular can be started from several places that do not announce themselves as "start logging" - opening the map screen begins a GPS track log - so there was previously no way to say "never write logs on this device".
- A `cmd` field in the machine-readable control list the companion app reads, naming the exact command for each setting. Clients should send that and must not guess a command from the setting's key - see Fixed.

### Changed
- BLE central TX no longer holds the controller gate for a whole multi-envelope image burst. Each envelope takes `bleCentralTx` then releases it before the inter-fragment delay so G2 heartbeats can land; R1 writes that miss the gate are queued (coalesced polls, small FIFO) and drained in those gaps instead of being dropped.
- G2 image push aligned with g2-kit-unofficial known-good path (`ble/docs/images.md` / `ui/image-streamer.ts`): Cmd=3 window=4, tolerate ≤3 ack misses, bumping `MapSessionId` on abort. ESP32-only: slower envelope gaps with R1 up + out-of-lock `rc=-1` fragment retry (in-lock retry can hang). Health skips g2-kit’s sacrificial warmup on FW 2.2.4.34 (first stream already paints).
- Health no longer stays in `ImageProbing` for the whole session (that swallowed firmware `SYSTEM_EXIT reason=0` / connection-lost as a page-swap echo). Probe state covers CREATE/graph bursts only; settle after image push; stop `UPDATE_TEXT` on `TextFailed`; never echo-suppress `SYSTEM_EXIT reason=0`.
- Health graph push is a **single** Cmd=3 stream (no sacrificial warmup). On FW 2.2.4.34 the first stream already paints; the old warmup+real double push (~40 KB) was what looked like “sent again” and then tripped `rc=-1` / connection-lost with the ring up.
- `deinitBluetooth` / `openg2` now call `g2RingInvalidateLink()` before `BLEDevice::deinit`, so a prior `ringconnect` cannot leave dangling GATT pointers that panic on Health poll (`LoadProhibited` in `BLERemoteCharacteristic::writeValue`).
- G2 long-lived hijack UI sessions (Health, Map, LLM, Pet, BMP/JPG/camera viewers, live pages) now run on one persistent `g2_session_w` (8 KB INTERNAL stack, job queue, single-flight abort) instead of a fresh `xTaskCreate` per Apps tap. Lens page-swap stays on `g2_page_swap_w`. Shared PSRAM scratch stages Health/Pet/Map/full-viewer tiles. Expected: less DRAM fragmentation after long hijack use; `memreport` should show `g2_session_w` rather than leftover `g2_health_page` / `g2_bmp_view` task names after exit.
- BLE peer auto-reconnect setting/CLI is now `autoReconnect` / `bleautoreconnect`.
- Settings screens now list Enabled and Auto-start first in every module, on the web interface, the display, and the glasses.
- Turning WiFi off with `wifienabled 0` does not take ESP-NOW down with it. They share the radio but are separate features.
- Commands and settings that control whether a feature runs are admin-only. Starting or stopping something for the session stays open to any signed-in user.
- Restoring a backup onto a different device now forces all three WiFi switches on, not one, so a restored device reliably gets back on the network.
- `tools/settings_registry.py` no longer regenerates the settings matrix when run with no arguments. That default meant an exploratory run silently rewrote a tracked file.
- Several always-reserved buffers are now claimed only when the feature that needs them starts, and released when it stops: the web server's 64 KB JSON buffer and console mirror, the debug message pool (which took ~28 KB at boot and now grows on demand), the map waypoint table, the display's console history on devices with no display, MQTT's external-sensor table, and the ESP-NOW remote-sensor cache. The memory-diagnostics table moved to PSRAM.
- **Breaking for anything reading the API:** live-state fields in sensor and status JSON are renamed from `*Enabled` to `*Running`, because they report what is currently running, not what is permitted. `/api/sensors/status` is the one most likely to be consumed externally. The `*Enabled` names now belong to the persistent master switches.
- **Breaking for the companion app:** the machine-readable control list no longer implies that a setting's key is its command. Each control carries an explicit `cmd`, and a control without one must be shown read-only. An app built against the old contract writes nothing until it is updated. See `docs/BLE_SENSOR_CONTROLS_CONTRACT.md`.
- The stored setting `bluetoothRequireAuth` is now `bleRequireAuth`, alongside `bluetoothAutoStart` becoming `bleAutoStart`.

### Fixed
- Apps → Health no longer dies ~60 s into a passive graph watch: the page worker feeds the hijack safety timer while it is alive (same idea as camera streaming). Force-exit (safety-timeout / DISPLAY_OFF) also skips the second Shutdown + Apps rebuild that raced CREATE, re-armed a phantom hijack, and left BLE central TX stuck (`Heartbeat skipped — central TX busy`). Failed page-swap CREATE now leaves Idle instead of Hijacked with no widget.
- Apps → Health metric graphs are one-shot (~20 KB BLE push once after daily settle, or again on Poll Now). Live vitals still UPDATE_TEXT; the sparkline no longer re-pushes every few seconds as samples accumulate.
- BLE central TX gate serializes G2 L/R and R1 ring GATT writes on the shared BT controller. Large image bursts no longer collide with heartbeats or ring polls; heartbeats skip while the gate is busy. On `writeValue` rc=-1 we abort the burst and release the gate (no in-lock retry — that path can hang forever on Bluedroid's write semaphore). Health graph pushes retry once out-of-lock after a drain. When the ring is up, image pushes also drop glasses to BALANCED and use wider inter-envelope gaps (Health/Q30 under 3-link load).
- Master `*enabled` toggles now stop a live subsystem immediately (same idea as `mqttclientenabled`), not only on the next boot. Clearing `httpenabled` stops the web server; `bleenabled` tears down G2 client + BLE server; sensors/camera/mic/SR/LLM/logs call their matching `close*`/`stop` commands. `espnowenabled 0` and `oledenabled 0` do the same. Turning a switch off works everywhere; refusing to *start* while off is implemented for the thermal camera, camera, microphone, speech recognition, on-device AI, Bluetooth, WiFi, the web server, ESP-NOW and both logs — see Known gaps for the eight sensors where it is still boot-only.
- Turning Edge Impulse off actually releases it now: `eienable 0` runs `eicontinuous 0` and `eimodelunload` instead of only flipping the flag, so live inference stops and the model leaves memory. `srstart` and `llmload` refuse with a pointer to `srenabled 1` / `llmenabled 1` when their master switch is off.
- `wifiautoreconnect` now reaches the WiFi driver on boot (`setupWiFi`) and when toggled while STA is up or the reconnect hunt is already armed. Before, boot never called `WiFi.setAutoReconnect`, and the setting command only persisted — so `wifiautoreconnect 0` could be ignored until a later `openwifi`/`closewifi`.
- Health Track R1-only sessions no longer write 5 s empty timestamp “heartbeats” while the ring is down; they log only on the Track mine interval (default 15 min) and Poll Now / page refresh.
- Apps → Health opened to a black screen then bounced back to Apps: CREATE/push magics were 258/259 (over the lens uint8 ≤255 limit), so CREATE was silently dropped. Health now uses the same 242/243 magic band as Pet/Maps.
- Apps → Health Overview vitals are native lens text (list+text) instead of unreadably small BMP glyphs; HR/HRV/SpO2/Battery keep list+image line graphs with auto-scaled Y so trails aren’t flat. Tests → Image → Streaming → Compound adds Q30/Q30b/Q30c probes for unproven list+text+image 3-pane CREATEs.
- Apps → Health: tapping a metric row after Overview failed to open the list+image graph (and bounced to Apps) because Overview CREATE never marked `containerReady`, so the shape-switch skipped SHUTDOWN and tried to CREATE on top of the live text compound.
- Apps → Health metric rows use the Q30-proven 3-pane layout: native text (title/value/n/range) above a graph-only 288×144 image so the sparkline can use the full tile.
- Settings written from the companion app now reach the right command. The device published only a setting's key and claimed it doubled as the command name; for most settings it did not. 243 of 407 controls silently discarded writes, and 6 ran an unrelated command that happened to share the key - toggling the camera's "Capture" debug flag took a real photo and wrote it to storage. Settings now carry their command explicitly, and anything without one is shown as read-only rather than quietly doing nothing or something else.
- The same fault made about a hundred settings look editable on the web Settings page while silently throwing the change away. Those now save.
- The "Auto-Reconnect" switch on the web WiFi card was setting whether WiFi connects at boot, not whether it reconnects after a drop. Reconnect was not adjustable at all - it was fixed on in the firmware - so a device could not be told to stop hunting for a network that was gone.
- Sensor status indicators, the words used in the firmware for "this sensor is currently running", and the settings that mean "this sensor is allowed to run" were all spelled the same. They are now distinct, so a running sensor and a permitted one can no longer be confused.
- The Automations on/off switch never worked. It was pressing a command that was never registered.
- Long command results are no longer truncated. A command may return up to 4 KB, but the output queue carries one 255-byte line per message, so anything longer arrived cut short with a `[CUT]` marker in the system log, the display console, the web console mirror and over Bluetooth - `files`, long help output and JSON status blobs were all affected. Results are now split across as many frames as they need, paced so the tail is not dropped when the queue is busy.
- On the serial console the result of a command is now written straight out, whole and undecorated, instead of being pushed back through the line-oriented log queue in chunks. JSON from `memreport json`, `status json` and friends can be selected and pasted somewhere useful; the `$ ` prompt now appears after the result rather than racing it. Every other destination - the log file, the display console, the web console - receives the result exactly as it did before. Lines a command prints while it is still running are unchanged.
- ESP-NOW file transfers are verified end to end with a CRC of the whole file, a receiver that cancels now stops the sender mid-flight instead of logging a complaint afterwards, and `espnowsendfile` reports failure when the transfer actually failed instead of always claiming success. The receiver also explains rejections and dead chunks rather than going quiet. **This changes the wire format: both devices must be on this release. A device running 0.99.1 or earlier sends zero where the checksum belongs, and this build rejects that as a mismatch, so every non-empty transfer between old and new firmware fails.**
- Several ESP-NOW cases where a good transfer was reported as failed at the very end, and same-filename conflicts that rejected legitimate sends.
- The web interface's file-fetch progress never reported success even when the fetch worked, and a failed send showed green.
- Six LED settings (brightness, and the five startup-effect settings) and three input-device settings could not be edited from any settings surface, because they were never bound to a command. They save now.
- A failed 64 KB allocation at boot used to hang the device forever in a retry loop with no message. That buffer is only claimed when the web server starts, and a failure is now reported instead of hanging.
- The R1 Health screen on the display now has a menu entry, under Apps, after Automations - the same place Health sits in the glasses launcher. It was previously reachable only by typing `oledmode r1health`.
- The R1 columns in a new sensor CSV are labelled correctly. The header named five fields while each row wrote seven, so temperature and wear landed in unnamed columns and, worse, reading positionally mislabelled them. Note this applies to newly created files - a log that already exists keeps the header it was created with.
- `oledautostart` and `automationautostart` now actually do something. Both saved, read back, and were ignored at boot. When the display declines to start it says so on the console and names both cures, because a dark screen is otherwise indistinguishable from a dead device.
- Automations triggered by BOOT no longer run when automations are switched off. `runAutomationsOnBoot()` was gated by nothing at all - it checked only a once-per-boot latch and the filesystem - so a device with automations disabled still fired every boot-triggered automation on the way up.

### Removed
- The old behaviour where a setting's storage key doubled as its command name. It is the reason most of the faults above existed.

### Known gaps
These shipped and are known; they are written down so they are not rediscovered as mysteries.
- Eight sensor master switches are honoured at boot but not by their start commands: with `tofenabled`, `imuenabled`, `gpsenabled`, `apdsenabled`, `rtcenabled`, `presenceenabled`, `fmradioenabled` or `inputenabled` set to 0, the device will not start that sensor on its own, but `opentof`, `openimu`, `opengps`, `openapds`, `openrtc`, `openpresence`, `openfmradio` and `openinput` still will. Turning a switch off does stop a running sensor.
- `eiautostart` saves and reads back but nothing acts on it. Edge Impulse is compiled out of the default build and has no boot-start path at all, so the setting is waiting on the feature rather than the other way round. It also answers "setting not found for this command" on builds where Edge Impulse is off, because the command is registered even though the setting row is compiled out.
- Twenty-three companion-app controls send `on` where the firmware expects `1`. The fix is in the app, not here.

## [0.99.1] - 2026-07-24
The two screens you can actually reach without a browser - the on-device display and the G2 glasses - stopped being read-only status panels and became places you can administer the device from: user accounts, LED control, an editable settings screen, file rename and delete, automation authoring, live performance stats, and an I2C scan. Both menus were rearranged into the same six categories so the two surfaces finally match. Alongside that: a view-only Guest role enforced end-to-end, ESP-NOW pairing that asks before it pairs, a real airplane-mode switch, one debug-category vocabulary shared by every surface, and two separate crash loops closed.

### Added
- A view-only Guest role below regular user. A guest can sign in and look around, but the only commands they may run are `login` and `logout`; anything else answers "Guest accounts are view-only." The web interface hides every control that would change something and drops the Command Line link, the display hides the screens a guest cannot use and answers "View-only" to the rest, and file access is read-only everywhere. Grant it with `useradd <user> <pass> guest` or `userdemote <user> guest`.
- Config > Users, on both the display and the glasses: an admin-only account manager. List every account with its role, add one through a guided username/password/role sequence, change a role, or delete an account behind a confirm step. Everything runs the real `useradd`, `userpromote`, `userdemote` and `userdelete` commands, so the usual rank rules apply, a regular admin is only offered roles up to admin, the founder account is shown locked, and a refusal appears on screen instead of the action silently doing nothing.
- LED control on both screens. Pick a colour from the named palette, pick an effect (fade, pulse, blink, rainbow, strobe), step brightness through a preset ladder, or switch the LED off. The row only appears on boards that have a NeoPixel pin.
- System > Perf on the display: a live performance screen refreshing twice a second, showing main-loop laps per second, average lap time and recent stalls, then every running task with a bar and its CPU percentage, busiest first. A second page lists each task's lowest-ever free stack in bytes with a "!" beside anything under 512.
- Hardware > I2C Scan on the display: sweep both I2C buses and get a scrollable list of every address that answered, with its bus number and the identified device name.
- Connect > Bluetooth > R1 Ring on the display: live heart rate, heart-rate variability, blood oxygen and battery from a paired ring, with Connect, Disconnect and an AutoRecon toggle. Opening the screen asks the ring for readings straight away instead of waiting for it to volunteer them.
- Apps > Automations on the display gained a "+ New" row that walks you through creating one: name, trigger type (Interval, Daily @time, On event, or Delay), the matching detail, then the command, then a summary screen. Event triggers are picked by family first, so the 134 event kinds never arrive as one endless list. New automations are created switched on.
- Notification settings on the device itself: press Y on the notification list for "Notif config". "My mutes" ticks kinds off for your own account; "Device kinds", for admins, cycles each kind between all, admin and off device-wide. Both write through the same commands as the web.
- A per-file menu in the display's file browser: View, Rename, Delete. View shows text wrapped and scrolled a line at a time for the usual text formats; Rename opens the keyboard pre-filled; Delete asks first, defaulting to No.
- On the glasses: System > System Events (the eight most recent events, refreshed every second), System > Logging (start and stop sensor logging and tick which sensors it captures), Config > OLED Login (sign in at the device's display by typing on the lens), and file rename and delete with the same confirm steps.
- Apps > Pet on the glasses: a Tamagotchi-style creature with Feed, Play, Clean, Medicine and Sleep, drawn as an animated sprite. It hatches from an egg and grows through baby and child into one of two adult forms depending on how well you looked after it, can get dirty or sick, and can die from neglect or old age. Its state lives in `/system/pet.json` and time only passes while the page is open. Also openable with `g2pet`.
- A guided question menu for the on-device AI, on all three surfaces. A model can now ship its own curated questions; the web page gets a "Guided:" row of group/question/subject dropdowns that fills the ask box, the display's LLM Chat opens the same picker with X, and the command line gets `llmmenu` to list what a model offers and `llmask <group> <template> [entity]` to ask one (`llmask repeat` re-asks the last). The web interface gained `GET /api/llm/menu`.
- `radiopower [on|off|toggle]`: an airplane-mode switch for the whole 2.4 GHz radio, powering WiFi and ESP-NOW down together rather than just dropping the network connection. Turning it back on restores exactly what was running. It appears as a "Radio" row in the display's Quick Settings and on the glasses' WiFi page, and is runtime-only, so the radio returns to its configured state on the next boot.
- `deepsleep`: powers the device off in the closest way an ESP32 can, with no wake source, so only the physical reset button brings it back. This is what Power Off on the glasses now runs.
- `espnowchannel` sets the radio channel two devices meet on when there is no WiFi to follow: pin it to 1-13, `auto` to follow the access point you are joined to, or `resync` to force the radio back onto the right channel now. With no argument it prints the preference, the channel the radio is actually on, and an OK/MISMATCH verdict. Both devices need the same value to meet away from a shared access point. Also on the display, the glasses, and a Radio Channel card on the web ESP-NOW page.
- `espnowdiscovered` lists what the discovery window has heard - name, MAC, signal strength, and how many seconds ago - holding up to 16 devices and dropping anything unheard for 15 seconds.
- Secure sensor fetching over the mesh: a master can ask a paired worker to stream a chosen set of sensors for a fixed lease, and the worker stops on its own when the lease runs out. The worker must opt in with `espnowacceptsensorcontrol on` (off by default) and name who may ask, via `espnowmasterfingerprint` and `espnowbackupfingerprint`; a request from an unmatched device identity is dropped.
- `micsource [auto|pdm|g2]` makes the microphone a device-wide choice, so a board with no microphone hardware of its own gets a working one as soon as the glasses connect. `auto` uses the onboard mic when the board has one and the glasses otherwise. Selecting `g2` before the glasses connect is allowed and takes effect when they do.
- `debugflags` prints the live debug mask as four hex words followed by the name of every category currently switched on, and counts any bits it cannot name.
- Settings > System Logging gained a "Debug message categories" checkbox grid covering every debug category the firmware has, grouped under family headings. The same value can be set with `systemlogflags <0x...>`.
- `sensorlogmask` chooses which sensors get logged and `sensorlogformat` picks text, CSV or track output, both without starting a session. Sensor logs gained size and rotation settings (256 KB and 3 old logs by default).
- The log viewer can now read the device's always-on logs - errors.log, system-events.log, events.log and i2c_errors.log - with real dates instead of raw numbers, event lines filed under the event kind they carry, and a new EVENT level in the filter.
- Automations can act on how far you are from a saved map waypoint: `WP_DIST:<name>` gives metres from the current GPS fix, for example `IF WP_DIST:Home > 200 THEN closewifi`. The name is matched case-insensitively, and the condition is simply false when maps are off, there is no fix, or no such waypoint exists, so an automation cannot fire on a missing reading.
- The device's 134 event kinds are now grouped into eleven named families. `events kinds` prints one section per family with a count, and the same grouping drives the event pickers on the display and the glasses and the notification list on the web Settings page.
- Notifications can be pushed to the glasses as native cards on the lens, titled with the event's family. The card is only offered while the glasses are connected, and the person they are paired to is treated as the viewer, so admin-only kinds and that person's preferences decide what appears.
- The glasses can show true firmware-native notification cards, drawn by the glasses themselves rather than the full-screen placeholder `g2notify` uses. Send one with `g2nativenotify <title>|<body>`; `g2nativenotify selftest` checks the checksum path first.
- `taskstats json` and `perftop json` return machine-readable output, which is what the phone app's performance view and the display's Perf screen both read, so all three show the same numbers from the same source.
- Join a WiFi network from the glasses: tapping a network in Scan Networks opens the lens keyboard for the password and saves it with `wifiadd`. Both on-screen keyboards also gained a symbols page, so real passwords and HH:MM times can be typed.
- Folder rows in the glasses file browser show how many items are inside, as "[12]" after the name ("[99+]" past ninety-nine), the same count the web and phone app show.
- Creating a user from the web Settings page now includes a Role picker. Roles above your own are shown greyed out and marked "(requires higher role)" rather than hidden.

### Changed
- Both menus were rearranged into the same six categories: System, Config, Connect, Hardware, Apps, and Power. On the display "Tools" is now "Apps" and holds ESP-NOW, Files, Maps, LLM Chat and Automations; on the glasses "Sensors" is now "Hardware" and "Network" is now "Connect", and Status, Settings and Tests are no longer top-level rows.
- Config > Settings on the glasses is now an editor, not just a viewer. Tap an entry to change it: a switch flips, a choice opens a tick-list, and anything else opens the lens keyboard pre-filled with the current value. Every change is saved by running that setting's own command, so it is checked and logged like any other. Settings that cannot be edited safely say so ("Read-only", "Secret - edit on web", "Bitmask - edit on web").
- The display's settings editor can now edit text and multiple-choice settings, not just numbers and switches. A setting with fixed choices opens a scrollable list; a free-text setting opens the keyboard pre-filled.
- ESP-NOW pairing is now discover-then-accept instead of instant auto-pair. `espnowpairmode` opens a discovery window, you pick a device and send a request with `espnowpairrequest`, and the other side has to answer `espnowaccept` or `espnowreject`. On the display an incoming request pops a prompt wherever you happen to be, defaulting to No, so an unattended device never pairs itself.
- Actions taken from the glasses now run the real commands. Restart, RAM Flush and Power Off run `reboot`, `ramflush` and `deepsleep`; the connect entries run `openg2`, `closeg2`, `ringconnect` and `ringdisconnect`; Bluetooth actions on the display dispatch `openble`, `closeble`, `bleadv` and `bledisconnect`. They obey the same permission checks as everywhere else and each gets a normal audit-log line.
- Commands arriving over MQTT and from on-device voice recognition now run in the same single queue as every other command source instead of on their own background task, so two sources can no longer collide on shared state mid-command. Their output is labelled `[mqtt]` and `[voice]` and the audit log records them as such.
- Routine happenings no longer pop a banner or a web toast by default; they go straight to the notification center. Affected: USB plugged and unplugged, a successful sign-in, a setting change, a sensor starting or stopping, a file deleted, wake word and voice commands, a WiFi network added or removed, ESP-NOW switched on or off, a mesh peer pairing, and battery reaching full. Security and safety events always interrupt.
- Turning the network on or off is now admin-only: `openwifi`, `closewifi`, `wifidisconnect`, `radiopower`, `openhttp` and `closehttp`. So are the Bluetooth controls (`openble`, `closeble`, `bledisconnect`, `bleadv`, `bleautostart`, `blemode`, `bleautoreconnect`), the wearable connect verbs (`openg2`, `closeg2`, `ringconnect`, `ringdisconnect`), and `openmqtt`/`closemqtt`. Read-only status commands stay open to any signed-in user. An automation owned by a non-admin that calls one of these will now fail, since automations run as their owner.
- Bluetooth no longer starts itself at boot on a newly set-up device; "Auto-start at boot" now defaults to off. Choose the "G2 Companion" profile during setup, or run `bleautostart on`. A peer already set to auto-reconnect still brings the radio up in client mode.
- `useradd` can create an account at a role directly: `useradd <username> <password> [0|1] [guest|user|admin|superadmin]`, with the two optional pieces in either order. You cannot create an account ranked above your own, and the check runs before the account exists so a refusal leaves nothing half-made. `userpromote` now also accepts `user` and `userdemote` now also accepts `guest`.
- Usernames are restricted to letters, digits, dot, underscore and hyphen, 1 to 64 characters, and passwords to 6 to 64 characters - the same rules whether an admin runs `useradd` or someone submits the sign-up form.
- `fileview` now pages through a file instead of cutting it off at 8 KB. It takes a page number, each page is about 3.5 KB broken at line ends with a `page 2/7` header, long lines wrap at 200 columns, and stray control bytes are dropped so opening a binary by mistake no longer sprays escape codes at your terminal.
- UltraSaver no longer makes the device feel broken to use. Its headline 40 MHz is now reserved for when idle power-saving has already blanked the screen; while you are using the device it holds 80 MHz, because 40 MHz made the screen and controls roughly six times slower to respond.
- `closewifi` now actually stops the radio hunting for the network it just left, and powers the radio down completely when nothing else needs it. Previously the driver kept scanning channels for the lost network, which could knock ESP-NOW off its channel.
- The "Debug Message Categories" list on the Logging page is now generated by the device rather than hand-written into the page, so it covers every category instead of the roughly forty it listed. The hand-maintained list had fallen about 71 categories behind.
- Settings entries that are really a set of on/off choices now render as a labelled checkbox grid with Select All and Select None instead of a raw number or hex box.
- Notification lists on the web Settings page and the Dashboard are grouped by event family and collapsed by default, with a count and a "set all" control per family. One flat list of about 134 dropdowns is what made this unusable.
- The web interface stopped scraping the command line for status. The Bluetooth, ESP-NOW, Speech, MQTT, Automations, Logging and notification screens now read purpose-built read-only addresses, which is also what lets a view-only guest load those pages at all.
- Promote and Demote on the web move one tier at a time and say which tier. Accounts you cannot act on show "Higher-privileged account - no actions available at your role" instead of a row of buttons guaranteed to fail.
- Log files moved into one capture tree: system logs to `/logging_captures/system/` and sensor logs to `/logging_captures/sensors/`, created at boot.
- System logging remembers which categories you chose. `log start flags=0x...` saves the mask, a plain `log start` re-applies the last one, and auto-start replays it at boot instead of logging whatever happened to be enabled. The tags choice sticks too.
- Model files carry their metadata in a new sectioned layout, which is what makes room for a question menu inside the model. Models need converting again with the current converter. An older model still loads and answers, but this firmware ignores its description, icon and topic list - so a model that used to refuse off-topic questions will answer them.
- Voice control listens through whichever microphone `micsource` selects, and starting speech recognition now tells the glasses to actually stream audio instead of arming the local decoder and hearing silence. Speech-recognition logging follows the `debugsr` family rather than the microphone switch.
- The microphone is no longer labelled PDM-only across the interface, and "Connected" now means a microphone is reachable at all - onboard or via glasses - rather than whether it happens to be capturing.
- Work that talks to the sensor bus is kept off the processor core that handles WiFi and Bluetooth: starting sensors, powering the camera, camera-AI detection and the microphone visualizer each run on a fixed core now.
- More scarce internal memory stays free: the thermal camera's driver object (about 4.7 KB of calibration data), the I2C device table, several command response buffers, and the ring's checksum table moved to external memory or program flash.
- A backup's list of protected fields is now built from the file that was actually created, naming every encrypted value it contains plus each user's stored password. It was a two-line hardcoded guess that had drifted. A cross-device restore now blanks values encrypted with the source device's key instead of writing dead blobs.
- A peer that has gone away stops eating airtime: heartbeats to a peer unheard from for 30 seconds slow to one every 10 seconds, then 20 after two minutes, then 30 after five. Any heartbeat back puts it straight to full rate.
- Every `debug*` switch now rejects anything that is not 0 or 1 and prints its usage line, instead of quietly treating any other number as "on".

### Fixed
- The device no longer panics and restarts whenever it loses or drops a WiFi connection. The handler that logged the lost connection ran on a system task whose stack had been cut to a quarter of its intended size, and the logging overflowed it. The stack is restored and the logging now happens on the main loop.
- The device no longer crash-loops when you take it out of range of your home network with GPS running. Every sensor on the I2C bus now runs on the second processor core, away from the WiFi work; out of range the WiFi stack hunts hard for the missing network and starved a GPS read mid-transaction, wedging the bus and resetting the device within seconds of boot.
- Connecting to or disconnecting from a WiFi network no longer knocks the ESP-NOW mesh off the air, and the mesh keeps working when you join, leave, or roam between networks. Peers now follow whatever channel the radio is actually using, and the device re-pins its channel whenever the connection changes.
- ESP-NOW messages are no longer silently dropped on a device not connected to WiFi - out and about, with your home network nowhere in range, which is exactly when the mesh matters most. The radio's power-saving mode slept the receiver between beacons and was only switched off after a successful WiFi join; it is now switched off whenever ESP-NOW starts.
- Later pages of the glasses settings module list are reachable again. The list tried to fit ten modules plus the view switch, back row and both page arrows onto one screen; the last row was silently dropped, and it was the "Next page" arrow.
- Setting rows on the glasses show the setting's name instead of its storage key. Every debug entry shares the storage key "enabled", so the whole debug module rendered as identical "enabled=..." rows.
- Errors and warnings written to the system log were all filed under the AUTH category, whatever raised them. They now carry the category of the subsystem that produced them, so filtering by category or colour works for error lines.
- `debugespnow` and `debugespnowcore` control the same category but were saved as two separate settings, so switching it off with one and on with the other left the device disagreeing with itself after a restart. They are now one setting.
- Recordings are no longer written with a wrong WAV header. Every recording is 16-bit, so a device left on `micbitdepth 32` no longer produces a file that plays as noise, and a recording made through the glasses is stamped 16 kHz instead of the onboard mic's rate.
- Microphone sample rate, gain and bit depth now survive a reboot. All three replied "(saved)" but were never written to the settings file.
- On the display's Microphone screen, Y really starts and stops a recording. It used to only flip the on-screen "REC" label - no file was opened and nothing was ever saved.
- Seeking a station on the FM radio no longer freezes the device. `fmradioseek` starts the seek and returns immediately; previously it sat on the shared sensor bus for up to five seconds, stalling the gamepad and display with it.
- LED effects no longer lock up the device while they run. `ledeffect` starts the effect and returns straight away, and `ledeffect off`, `ledcolor` and `ledclear` genuinely cancel a running one. Previously the command ran the whole effect inline.
- Running an automation from the display now tells you what actually happened. It used to flash "Running..." set after the work had already finished, and nothing kept the screen refreshing, so the message stayed frozen indefinitely. You now get "Done" or "Run failed", and action messages fade on their own.
- Back-to-back notification banners no longer cut each other off. A banner arriving while another is showing waits its turn (up to twelve queued) and appears as soon as the ribbon frees up.
- Connecting the R1 ring from the glasses no longer looks like it failed. The command returns as soon as the attempt starts, but the scan and handshake take several seconds, so the menu repainted "disconnected" and never updated. It now shows "Ring: connecting..." and flips when it really is.
- Paired glasses and rings no longer end up with no owner, and signing out at the glasses or banning the person who paired them no longer strands the lens. Ownership is re-assigned immediately, falling back to the device's founding account. An unowned pairing was the cause of glasses-menu commands running as nobody and being refused by admin checks.
- Deleting a user whose name was passed in quotes now works. The Users screens quote the name so names with spaces survive, but `userdelete` compared the quote characters as part of the name, so nothing ever matched.
- The user list in web Settings shows the right role. A Super Admin matched neither the admin nor the user test and was drawn as a plain "User" with a Promote button. Demoting a Super Admin also no longer drops them two tiers in one click.
- The Migration Tool's browser request to `/api/backup` no longer fails with a CORS error. Full-featured builds register about 110 web addresses but the server only had room for 100, so the last handlers registered were silently dropped; the budget is now 160 and a failure to register is reported instead of passing silently.
- The Reboot button on the Settings page no longer blanks the page when the reboot is refused, and the ESP-NOW user-sync switch no longer claims to be Enabled when the device refused. On the Bluetooth page, "Auto-Start on Boot" and "Require Authentication" now flip only after the device confirms.
- Refused actions in Settings report the device's actual reason instead of "Error: Error: ...".
- On the Speech page, an error from the model-file listing is no longer turned into a fake model. A refusal like "Error: Permission denied" was split on spaces and its last word offered as a selectable model file called "denied".
- A command whose answer is too big for the connection it arrived on now says so, with the size and the limit, instead of handing back a torn-off fragment that looks like a complete answer. That silent cut is how `events kinds json` reached MQTT as a fragment and how the web notification editor got a bare "OK".
- `events kinds json` returns the list of event kinds instead of a bare "OK". The list was printed to the console while the actual reply was just the word OK.
- The Notifications section on the web Settings page works again. It said "Not available (admin login required)" even to an administrator, because the machine-readable form of `notifydevicekind list json` grew past what any connection can carry back.
- Removing your last saved WiFi network now sticks; `wifirm` on the final network appeared to work but the empty list was never written to disk. A settings save issued after a failed settings load no longer rewrites the network list at all, so a bad load cannot wipe stored networks.
- Saving, removing or re-prioritising a WiFi network now reports a failed write instead of claiming success and quietly reverting on the next reboot. A full list returns a proper "saved-network list is full" error rather than a misleading blank-SSID message.
- `wifiscan json` no longer breaks when a nearby network has a quotation mark or backslash in its name. Results are now properly escaped, and a crowded area is trimmed with a `dropped` count rather than running past the end of the reply.
- `wifiinfo` showed a blank "Saved SSID" even with networks saved - it read a dead single-network field frozen empty for the life of the device. It now shows your top-priority saved network, or "(none)".
- A WiFi network saved during first-time setup on a freshly erased device was kept in memory but not written to storage, so it was gone after the first reboot.
- Viewing a file over serial, the event stream or Bluetooth showed only the first 255 bytes, because the whole file was sent as a single message and the output path clamps one message at 255 bytes.
- `ramflush` never brought the camera or the on-device model back after the reboot; it checked for them using feature switches that do not exist in this firmware, so both always read as "was not running".
- Waking from idle power-saving now always restores the clock speed it lowered. The wake path only ever un-did a downclock from above 80 MHz.
- Requesting an account no longer lets the same name pile up in the approval queue, the queue is capped at 32 outstanding requests, and the pending list is written as real JSON so a name containing a quote can no longer corrupt the file.
- Lines sent to the web console appeared twice when WiFi was down - added once by the normal delivery path and once by a backfill that was no longer needed.
- The Power menus now scroll; all three drew every entry unconditionally, so adding a row would have pushed one off the bottom with no way to reach it. Yes/No confirmation boxes are no longer clipped by the header bar.
- The status and network screens now separate the radio being powered from being joined to a network, so "(off)" no longer reads as the radio being powered down when it is merely disconnected.
- The settings list on the display no longer shows rows it cannot edit. Secrets, read-only entries and settings hidden because their sensor is absent are filtered out of the list, the navigation and the first-item jump using one shared rule; previously drawing and the cursor used different rules, so scrolling could land on a row that was not on screen.
- Backing out of a screen with the Home button no longer leaves it half-finished for next time.
- Starting or stopping the FM radio from the glasses sensor list used to send `openfm`/`closefm`, which are not real commands. It never showed up because no board with the FM radio had shipped.
- Turning the microphone on or off from the glasses no longer saves the auto-start preference when the microphone failed to start.
- A glasses microphone recording no longer hangs forever when the temple carrying the audio drops mid-session; the disconnect stops the capture, closes the file and releases the microphone.
- If the glasses disconnect while they are the microphone source, the mic stops reporting itself as live within one refresh instead of showing an active mic with a dead level meter.
- `g2status` no longer reports "idle" when the glasses client was never started - it now says "off" - and says "connecting" while only one temple is linked instead of claiming "connected". The web Bluetooth page reports the same states honestly.
- Unpairing a device now cleans up after itself completely: the peer's stored identity, any half-written temp file, and the now-empty folder are all removed. In direct mode it also stops being counted as an online peer.
- A mesh message too large for the receiver to reassemble now fails immediately with a clear reason instead of appearing to send successfully.
- Sensor streaming on a worker no longer risks crashing or wedging the device; stopping the last sensor could have two parts of the firmware tear down the same task at once, and the task's shutdown left a stale reference that meant streaming could never restart until reboot.
- Retry on a guided AI question re-asks the same question instead of banning the words of the previous answer. That behaviour is right for a question you typed but guaranteed a worse answer for a fixed fact question.
- Loading a model from the web page capped the context window at 2048 tokens even when `llmmaxcontext` was set higher. The web page is the only place that setting is read, so a raised value never took effect.
- When memory is short as a model loads, the device shrinks its context window to fit - which used to happen silently and left the model answering in one or two words. It now says so on every surface and records a system event.
- `outserial` no longer accepts nonsense; `outserial banana` was read as 0 and silently switched serial output off.
- `devices json`, `sensors json`, `features json` and `devicefile` now say the list outgrew the response buffer instead of returning a document silently cut in half.
- Commands run in the first moments of boot get the same output allowance as the same command run a moment later.
- Sensor-log auto-start no longer tries to create the old `/logs/sensors` folder before writing, which would have aborted the start.
- `log start` with a nested path now creates every missing folder along the way instead of only the last one.
- The battery log table keeps its column headings visible while you scroll and fits the width of the panel.

### Removed
- The `g2mic` and `setmicsource` commands. There is one device-wide microphone choice now, `micsource [auto|pdm|g2]`, which both recording and voice control follow. Low-level probing is still available via `g2micon`, `g2micoff` and `g2micstats`.
- The `outweb` and `outdisplay` commands and the "Web Output", "Display Output" and "G2 Glasses Output" switches. None of them routed anything - output to the web page follows the web server being up, output to the glasses is turned on per session with `outg2`, and the display console was never gated by that switch. "Serial Output" (`outserial`) is the one output switch that is saved.
- The unused output-routing web addresses `/api/output` and `/api/output/temp`. No page ever called them.
- The unused single-network `wifiSSID` and `wifiPassword` settings. Saved credentials have lived in the saved-networks list for a while and nothing read these.
- The "All ESP-NOW" master checkbox in the ESP-NOW debug settings group - a duplicate that switched the same logging as "Core". The `debugespnow` command still works as an alias of `debugespnowcore`.
- The "Auto-Start" row from the display's Logging menu. Those switches now live in Config > Settings, and the old row only ever showed and toggled the sensor-log setting despite sitting above both.

### Security
- Actions taken from the glasses no longer quietly bypass the permission check when the device is busy. Every toggle on the WiFi, Bluetooth, HTTP, ESP-NOW and sensor screens used to fall back to changing the setting directly if the command queue was full, which skipped the permission check entirely. They now do nothing and log it.
- If the glasses have no recorded pairing owner, taps are refused outright instead of running as an anonymous user.
- A command arriving over an outside connection with no signed-in account is refused with "Authentication required" and logged as a failed attempt. An empty identity had been slipping past the guest and admin checks.
- An empty identity is never treated as an administrator. If the account file could not be parsed, the "first account is the owner" fallback compared an empty name against an empty name and matched, so an unnamed caller could pass admin checks.
- Demoting the first account in the user list now takes effect. The device treats the first-listed account as an administrator when no role is recorded - a fallback for user files predating roles - but that fired even when the account had an explicit role of user or guest, so a demoted first account kept admin rights.
- The audit log no longer records WiFi passwords in the clear. Redaction counted plain spaces to find the password, so a quoted network name containing a space shifted the count and logged the password unmasked.
- The audit log now shows which role a new account was given. `useradd` masked everything after the username, so creating a plain user and creating a super admin left identical entries.
- The sign-in page no longer trusts the username it echoes back. A failed login redisplayed whatever was typed straight into the form's value attribute, so a crafted username could inject markup into the page shown to the next person at that address.
- Requesting an account is now rate-limited and validated like signing in, and the failure page shows one of a fixed set of messages instead of splicing the device's raw command output into a public, unauthenticated page. Sign-in and account-request submissions are capped at 2 KB; a huge body previously got a matching allocation before anything checked it.
- Rejecting a reserved username answers a plain "Invalid username" instead of naming which reserved word was hit, so the sign-up page no longer confirms that internal identities exist.
- Restoring a backup over the network no longer writes to storage on its own. During first-time setup the device accepts the file, holds it in memory and prints "BACKUP RECEIVED - CONFIRM ON DEVICE"; nothing touches flash until someone answers the prompt on the display or types yes/no at the serial console. Previously anyone who could reach the device on the network during setup could overwrite it outright.
- Sensor readings no longer go out over the mesh as a plaintext broadcast any listening device could pick up. A worker now sends each reading as an encrypted message addressed only to the master holding the lease, with no fall back to plaintext. Note the consequence: on a mesh, `espnowsensorstream` on its own no longer delivers anything - the master has to take out a lease with the fingerprint-checked sensor request described above, and until it does, the web ESP-NOW page's sensor-streaming toggle will report success while no readings arrive. Bonded-pair sensor streaming is unaffected.
- File transfers between paired devices require an established encrypted session in both directions. Knowing a paired device's address is no longer enough to push a file, cancel someone else's transfer, or claim a transfer failed.
- The special bonded-pair files a partner sends (its settings, manifest and schema) are accepted only from the configured bond partner, while bond mode is on and that partner is online. Another paired mesh device can no longer overwrite what your device believes its partner's settings are.
- `espnowsensorstream` can only be run at the device itself now, so a signed-in remote account can no longer switch on another device's sensors through general remote command execution.
- File access now recognises guests and super admins. A guest can read what its role could already read and nothing else; a super admin has unrestricted access, including the certificate, key and firmware files hidden from everyone else. Denied-access log lines name the role that was refused.
- Commands sent over Bluetooth are recorded with the connected phone's address instead of a bare "ble", so the audit log says which device ran what. When `blerequireauth` is off, those commands run as the reserved name AuthBypass, matching serial and the display.
- File content shown inside the web interface now escapes apostrophes as well as quotes and angle brackets, closing the remaining way a file's contents could break out of a single-quoted attribute.
- A guest signed in at the display can no longer change settings; both Config > Settings and the Quick Settings panel refuse guest input.

## [0.99.0] - 2026-07-18
Adds a Super Admin role above admin so the most destructive commands are no longer in reach of every admin, a `ramflush` command that reboots to reclaim memory and brings your running features back with it, and a round of ESP-NOW reliability work. Several commands that could reboot, wipe, or reach across the mesh were admin-gated for the first time.
### Added
- Super Admin: a new role above admin, granted with `userpromote <user> superadmin` and removed with `userdemote <user> admin`. Only a super admin can run the commands that can destroy or take over the device - factory reset, formatting storage, regenerating the device identity, changing the mesh passphrase, and the switches that control whether serial, display, and Bluetooth require a login.
- `ramflush`: reboots the device to reclaim fragmented memory, then restores the features that were actually running when you ran it. It never changes your configured autostart settings, and it deliberately forgets the session if anything else caused the restart, so a feature that wedges the device cannot bring itself back on the next boot.
### Changed
- A bonded ESP-NOW peer now acts with super admin rights. Bonding joins two devices as one unit and the bond token is the proof, so the far side no longer needs a separate login. Ordinary mesh pairing is unaffected and still gets only the role the account was given.
- About two dozen commands that take real action are now admin-only: `power` and `powersave`, the remote ESP-NOW verbs (`espnowremote`, `espnowfetch`, `espnowbrowse`, `espnowsendfile`), the bond control verbs, `imagesend`, `features`, `log`, and `automation run`/`trigger`. Read-only status commands stay open. An automation owned by a non-admin that calls one of these will now fail, since automations run as their owner.
- A regular admin can no longer demote, ban, delete, or reset the password of a super admin. A super admin can do all of those to a regular admin, and the first account stays protected from everyone.
- ESP-NOW reliability: the receive ring and broadcast trackers moved off fixed arrays onto checked allocations, and several large tables moved to PSRAM to free scarce internal memory. The mesh fingerprint is now verified on both send and receive, and regenerating the device identity with `--confirm-wipe-all-bonds` now clears every stored peer.
### Removed
- The `espnowbuffers` command and the `txQueueSize`, `rxBufferSize`, `chunkSize`, and `fileChunkSize` settings. These tuned buffers that are now sized automatically.
- The "Reassembly Timeouts" line from `routerstats`, along with the counter behind it.
### Fixed
- The FM radio driver failed to compile on any board with the radio enabled, because of a half-finished rename. Boards with FM radio switched off were unaffected, which is why it went unnoticed.
- `ramflush` checked whether the gesture sensor was running using a setting name that does not exist, so it always read "off". Restoring a session could therefore switch off a gesture sensor you had configured to start automatically.
- Stack usage reported for kernel tasks is now labelled as an estimate. Only the free-space column was ever measured; the rest assumed a fixed margin and was printed as though measured.
### Security
- Resetting another user's password now obeys the same rank rules as promoting or banning them. Previously any admin could reset a super admin's password and then sign in as them.

## [0.98.9] - 2026-07-18
Brings a batch of features to the G2 glasses display: automations and ring health readouts on the Apps menu, a live view of on-device AI replies, plain-text and CSV file previews, and map panning.
### Added
- Apps > Automations on the glasses: browse your saved automations, run one, and switch it on or off. The on/off badge only changes when the device confirms it, so a denied action will not look like it worked.
- Apps > Ring: a live readout of heart rate, heart-rate variability, blood oxygen, and battery from a paired R1 ring. Values that are not yet known show as "--" rather than zero.
- A read-only view of on-device AI conversations on the glasses, showing prompts and replies as they stream in. This is a first pass: it shows the most recent part of the answer, with scroll-back still to come.
- Text and CSV file previews in the glasses file browser, alongside the existing JSON view.
- Map panning from the glasses: step the map north, south, east, or west.
### Changed
- The three separate paged-text viewers on the glasses (files, settings, and ESP-NOW chat) now share one engine. Long lines wrap instead of being cut off, which is what makes CSV and plain text readable on the lens.
- Several glasses screens keep their row buffers in PSRAM, freeing scarce internal memory.

## [0.98.8] - 2026-07-18
A small round of fixes.
### Fixed
- Viewing a file in the web interface now follows the theme you chose in the app. Previously it followed the browser or operating system setting, so a light-mode app could open a file as white text on black. Plain text, CSV, and empty files were unstyled entirely and now match the rest of the interface.
- Merged GPS tracks no longer draw a long straight line across the map. Where a stitched log jumps more than five miles between fixes, that leg is left undrawn instead of being joined up.
- The device no longer restarts itself after sitting idle. Entering and leaving power saving changes the processor clock, which could collide with a sensor read already in progress; the switch now waits for the sensor bus to be clear.

## [0.98.7] - 2026-07-14
A device-wide event system is the centerpiece: a single event register now records what happens on the device, drives notifications, and lets automations react to live events. Notifications were rebuilt on top of it with per-kind and per-user controls. Also splits debug settings into their own file and fixes a batch of bugs.
### Added
- System event register: the new `events` command shows the most recent system events (last 48), each stamped with who or what caused it (e.g. "by web:hub"); `events kinds` lists every event type — the same vocabulary automation event triggers use. Events are also written to a durable, machine-parseable log at `/system/sys_logs/events.log` (one line per event, capped at 256 KB, on by default via the `eventlog` setting), with a gap marker if events ever outrun the writer.
- Event-triggered automations: automations can now fire the moment something happens on the device, not just on a clock or interval. Choose "On Event" (`type=event on=<kind>`) with an optional case-insensitive `match=` filter on the event's subject/detail. Event triggers fire even when the clock isn't synced (when time-scheduled automations can't run), are rate-limited to about once every 2 seconds per automation to prevent runaway loops, and round-trip through automation import/export.
- Events across the device: dozens of subsystems now raise named events that feed automations and the notification/event log.
  - ESP-NOW & mesh: peer online/offline, auto-pair complete, text/file received, bonded peer online/offline, an unpaired device probing the bond channel, ESP-NOW on/off, pairing window open/close/expire, backup-master promote/demote, an authenticated remote command running here (and, on the sending device, the command being sent and its result coming back), a failed remote-command login, and metadata changes.
  - Sensors & inputs: IMU shake/tap/freefall and orientation flips, APDS gesture swipes, presence detected/cleared, GPS fix gained/lost, FM-radio RDS station name, and gamepad/rotary-encoder buttons — plus a "sensor fault" event when a sensor auto-disables after repeated I2C errors (IMU, ToF, thermal, GPS, presence, gesture, gamepad, encoder).
  - Camera, audio & on-device AI: camera on/off, photo saved and video recording finished (with filename, plus frame count for video), a microphone recording saved, and the camera-AI recognizing or losing an object (Edge Impulse object detection).
  - System: battery and charger changes; WiFi, MQTT, BLE, and glasses connect/disconnect, plus WiFi networks added or removed and connection failures; the first successful clock sync; storage mounts, file deletes, low-storage and SD write-failure warnings, and a failed settings save; logins (success and failure, now including MQTT and web), user account and password changes, and access requests; per-sensor start, stop, and start-failure; voice wake and commands; on-device model load and generation; power-save enter/exit; and an intentional reboot (posted on the next boot, tagged with its reason — reboot command, first-time setup, factory reset, or the G2 power menu). (`events kinds` lists the full set.)
- A codebase-wide event-coverage pass added the security, fault, and capability happenings that were still missing: privilege and access actions (a user promoted, demoted, or banned; an IP banned; a brute-force lockout; an ESP-NOW peer unpaired or the device's identity rotated; voice control armed; a storage volume formatted; glasses silent mode), fault conditions (a device crash, an MQTT or on-device-model start failure, an unrecoverable LLM engine fault, an RTC that lost power), and more (a microphone recording started, thermal-sensor motion, an automation firing). Security alerts and faults also raise a notification; the rest are automation-triggerable and in the event log.
- A second pass rounded out the vocabulary with service and session lifecycle (web server up/down, Bluetooth on/off, TLS certificate generated, a session logout, power mode changed, battery full, the on-device model unloaded, voice control disarmed, glasses interactive session start/end), configuration and audit actions (factory reset, a feature toggled, firmware updated, a backup created or restored, a config file failing its integrity check, a stored secret failing to decrypt, a denied privileged command, a mesh passphrase change, an ESP-NOW output tap or failed file transfer, an automation created/deleted or an action dropped mid-run), and sensor thresholds (a thermal hotspot, a ToF object approaching or leaving, the FM radio tuned, walking started/stopped, continuous camera-AI detection on, an MQTT sensor discovered). The device now raises about 130 distinct event kinds.
- Per-kind notification policy (admin): the new `notifydevicekind` command sets each notification kind's device-wide visibility — `all`, `admin` (admin viewers only), or `off` — saved to `/system/notifications.json` and applied across the OLED banner, web toast, and notification center. The web Settings page gains a matching admin-only "Notifications" section.
- Per-user notification mutes: the new `notifyusermute` command, and a "Customize notifications" / "My notifications" panel on the web dashboard, let any signed-in user silence specific notification kinds just for themselves. Muted kinds disappear from that user's banners, toasts, and notification center only; the underlying events and any automations are unaffected.
- Notification channel switches: independent device-wide on/off switches for the three notification sinks — OLED banners, web toasts, and the notification center — on by default.
- Notification diagnostics: the new `notifstats` command reports pipeline counters (events that became banners/toasts, how many were filtered by device policy or per-user mutes, suppressed by a per-kind cooldown, or dropped; `notifstats reset` zeroes them), and the new `debugnotifications` flag streams live pipeline diagnostics.
- More notifications: Bluetooth connect/disconnect, G2 glasses connect/disconnect and worn/removed, and web sign-in success/failure (web logins previously raised no notification, unlike the CLI and OLED).
### Changed
- Notifications were rebuilt on the event bus: every subsystem posts an event and a single rules table decides what becomes a banner, toast, or notification-center entry, with per-viewer routing (admin-only kinds and per-user mutes) and per-kind cooldowns. Most existing notifications are unchanged, but a few were retuned in the cutover: the failed-login cooldown is now 30 seconds (was 10), USB connect and disconnect are cooled independently, and the "remote command running" notification on the executing device is now recorded to the event log instead of popping on screen.
- The on-device notification center now draws from the unified event log and filters by whoever is logged in at the display, so per-user mutes and admin-only kinds are honored on the local screen too.
- Debug flags — plus the log level, the web-console toggle, and the memory-sample interval — moved out of `/system/settings.json` into a new `/system/debug.json`. They were roughly half of the ~5 KB settings document and every toggle rewrote the whole file; splitting them out frees that space for other settings and limits a debug change to a small, isolated write. The web, bonded-peer, and G2 settings views are unchanged, and device backups now include the new file.
- `log start flags=` now accepts a full 256-bit debug mask (one hex string up to 64 digits, or colon-separated 64-bit words highest-first) instead of the old 128-bit form. Debug flags were regrouped by family into per-byte banks, so raw hex masks written for earlier firmware no longer select the same categories; the named per-feature toggles (`debuggps`, `debugwifi`, …) are unaffected.
- The OLED status ribbon uses larger, cleaner icons — a checkmark when linked, an X when down, a refresh glyph while syncing, and a magnifier while searching for peers.
- Changing the FM radio volume now shows a brief "Vol: N/15" banner on the OLED instead of posting a notification, so routine volume tweaks no longer pile up in the notification center.
- The internal ESP-NOW message-type numbering was reorganized; there is intentionally no cross-version compatibility, so every meshed or bonded device must be reflashed to this version to talk to the others.
### Fixed
- Multi-trigger automations built in the web UI are no longer silently dropped: the command parser now understands backslash-escaped quotes, so secondary triggers, quoted command text, and event match patterns are parsed in full instead of being cut off at the first inner quote.
- ESP-NOW traffic capture to SD now records frames. With capture enabled it had been silently writing nothing because it filtered on an outdated protocol version that no live frame matched.
- The thermal camera and distance (ToF) sensor tasks no longer peg a CPU core at 100% while their polling is paused or the sensor isn't ready; they now idle like the other sensor tasks, saving power and freeing the core.
### Removed
- The `debugespnowencryption` debug toggle was removed — it never controlled any log output, so enabling it did nothing.

## [0.98.6] - 2026-07-13
First-time setup now works fully over a serial-only connection, and a bug that silently disabled ESP-NOW during setup is fixed.
### Fixed
- First-time setup over serial (no OLED) can now choose HTTPS and Server/G2-Bluetooth modes, matching the serial/OLED options already offered for the WiFi, MQTT, and ESP-NOW pages.
- Typing a device name at the ESP-NOW setup prompt no longer fell through to the skip branch that silently turned ESP-NOW off; a typed name now configures ESP-NOW so it auto-starts at boot. Fixed identically in the serial, OLED, and CLI setup engines.

## [0.98.5] - 2026-07-12
A small cleanup pass: a latent MQTT link error, dead scheduler code, and an unimplemented automation trigger mode.
### Fixed
- Corrected a misspelled `isMqttConnected()` definition that would have failed to link if MQTT were compiled in.
### Removed
- Dead automation-scheduler code, and the non-functional "once when it becomes true" trigger mode, whose web control was never backed by any logic.

## [0.98.4] - 2026-07-12
BLE file uploads are now limited by free storage instead of a fixed 256 KB cap.
### Changed
- BLE file upload drops the 256 KB ceiling. Uploads stream one append at a time and never buffer the whole file, so the only limit is free storage, matching the web upload. A volume that fills mid-upload returns cleanly ("storage full?") instead of corrupting. Pairs with the companion app's storage-based cap.

## [0.98.3] - 2026-07-12
The on-device LLM runs about 50% faster, with a new per-section forward profiler.
### Changed
- On-device LLM matmul quantizes activations to INT16 once per matmul and accumulates int8xint16 in int32, replacing a per-weight int-to-float convert in the hot loop. HW-validated ~1.6 -> ~2.5 tok/s, output quality-neutral. Weights stay INT8, so PSRAM bandwidth is unchanged.
### Added
- `llmprofile` command/setting: per-section forward timing (matmul vs attention vs the rest) dumped after each generation. Default off, near-zero overhead when off.

## [0.98.2] - 2026-07-12
Automations can now check far more of the device's own state, the trigger form is simpler, and a multi-trigger scheduling bug is fixed.
### Added
- Automation conditions can check about 30 new things, usable in both the "fire when" condition and IF/THEN command logic: battery percent, free memory (heap, PSRAM, storage), uptime, chip temperature, hour of day, day of week, and whether the clock is synced; WiFi state and signal strength, Bluetooth state, and GPS fix/speed/satellites; the on-device model's state; and a range of ESP-NOW/bond signals - bond online/synced/paired/role, bond link signal strength, the bonded peer's free memory and uptime, whether pairing mode is open, how many mesh peers are known, and the oldest peer's heartbeat age.
### Changed
- The automation form now shows one unified "Triggers" list - the main trigger plus a quiet "+ Add trigger" button for extra ones (up to 4, auto-numbered) - replacing the separate always-visible "Additional Triggers" panel.
### Fixed
- Automations with more than one trigger now fire on each trigger's own schedule. Previously only the first trigger's schedule was honored, so an added trigger set to fire sooner than the main one never ran on its own cadence, and the scheduler repeatedly re-read the automations file until the main trigger came due.

## [0.98.1] - 2026-07-12
Fixes to the on-device LLM's domain refusal gate: it stops refusing basic help questions, and the web chat now shows the refusal message for off-topic prompts instead of going blank.
### Fixed
- The domain refusal gate no longer refuses universal help and identity questions like "what are you", "what do you do", or "help" - any model can answer those even though they contain no topic word. Works on the web, the phone app, and the OLED.
- Web chat now shows the model's refusal message for an off-topic prompt instead of a blank reply. The phone app and OLED already showed it.

## [0.98.0] - 2026-07-11
The on-device LLM comes online with guardrails that keep it on-topic and controls to steer its answers, plus a round of auth-store hardening and crash fixes.
### Added
- On-device LLM is now built into the firmware: load a tiny model from LittleFS or SD and chat with it from the web, OLED, CLI, or over BLE.
- Domain refusal gate: a model can carry a list of its own topic words and now declines a prompt that mentions none of them (with a custom refusal line) instead of inventing an answer.
- Answer controls you can tune per model or per message - a confidence gate that prefixes "I'm not sure, but" when the model is unsure, an n-gram blocker that breaks phrase-repeat loops, and a content boost that keeps replies on topic; the phone app can override sampling per message.
- Model info card: a loaded model can carry an icon and a short description, shown on the OLED ready screen.
- New `bootcount` command reports boot count, crash count, and last reset reason.
- Files can transfer to the phone as raw binary over the secure BLE channel instead of base64 (about a third smaller), speeding up offline map downloads.
### Changed
- Web chat and CLI now fall back to your saved LLM settings for any field left blank, instead of forcing fixed defaults.
- Casually typed names (like "bulbasaur") are matched to the model's trained form, so casing no longer hurts answers.
- KV cache defaults to FP16, giving about twice the context in the same memory.
- Re-saving a known Wi-Fi network with a blank password now keeps the stored password instead of clearing it.
### Fixed
- GPS could corrupt the I2C bus when it collided with other sensors; reads are now serialized through the shared bus lock and served from a cached snapshot, and a southern/western-hemisphere sign error in the cached position is fixed.
- Sensor tasks low on stack could hard-fault; they now shut down cleanly instead.
- Large file reads no longer truncate the reply during map downloads.
- The offline map tile cache is now shared safely between the OLED and the glasses.
### Security
- Auth-database hardening against secret loss: the boot counter moved to its own NVS partition so it no longer rewrites the whole user database every boot (a power cut in that window could wipe all logins). Every user-database write is now atomic. Device-key handling self-heals across key derivations, validates decrypted padding strictly, and refuses to overwrite a still-recoverable secret with an empty value after a failed load.
### Docs
- New specs and plans: per-message LLM generation overrides (app integration), an exact-answer retrieval-hybrid plan, and an LLM settings/control-surface audit; updated the BLE secure-channel framing doc for the new binary frame.

## [0.97.5] - 2026-07-07
A much clearer GPS-track experience on the web: readable start/end/direction and stitched multi-log day-tracks.
### Added
- GPS Tracks (web): a loaded track now shows a green START circle and a red END square, each labelled, plus direction-of-travel arrows along the path - so where you began, where you finished, and which way you went are obvious at a glance.
- GPS track stitching: pick several logs in the order you want and merge them into one continuous day-track (`gpstrackmerge` CLI, or the Stitch panel on the web Maps page) - for an outing a power interruption split across several files.
### Changed
- GPS tracks can hold up to 10000 points (was 500); the buffer lives in PSRAM (~117 KB) - about 14 hours of continuous logging at the default 5-second interval.
### Fixed
- Web Maps: the GPS Tracks Load and Stitch buttons (and camera video delete) now quote the file path, so names that need quoting work instead of failing with "cannot load file".
- Stitching or saving a track now writes under the captures folder with the right permissions (was failing with "cannot create output").
- Loading a track over the web now runs with the caller's identity, fixing a "file not found" when the file was really there.
- The track reader now skips a GPS line that has no fix even when other sensors are co-logged on the same line, so a stray comma can't inject a bogus point.

## [0.97.4] - 2026-07-07
Clearer, more legible offline maps on the glasses and OLED, and no more missing chunks.
### Added
- Map feature differentiation on the glasses and OLED: roads render with a thick-to-thin weight hierarchy (highway / major / minor) and each class has a distinct line style, so the map is far easier to read. The G2 lens additionally uses 16-level green shading and surfaces water bodies and coastlines (drawn cleanly, without tracing the tile grid).
### Changed
- G2 glasses maps render at the lens's native 288x144 in 16-level grayscale, instead of a 128x64 1-bit page upscaled 2.25x - crisp uniform lines and feature classes that stand out. The OLED stays 128x64 but now shares the road weight hierarchy and line styles (it can't show brightness at 1-bit, but thickness and pattern read fine).
- Larger default map tile cache: 1.25 MB, up from 1 MB (PSRAM).
### Fixed
- Chunks of the map no longer go missing. Tiles larger than the tile cache's biggest size class were silently dropped every frame, so dense and coastal areas showed rectangular holes; the cache now always sizes its top tier to the largest tile in the map (the generator caps tiles at 20 KB).
### Docs
- Updated the G2 map multi-shade rendering plan for the shipped native-resolution renderer and the OLED / water-coast follow-ups.

## [0.97.3] - 2026-07-06
Maps on the G2 glasses, WPS-style ESP-NOW pairing, and a batch of OLED, web, and build fixes.
### Added
- G2 glasses: a "Maps" page under a new "Apps" hijack submenu renders the offline map to the lens as a list+image compound (Zoom In/Out, Reset View, Recenter); the `g2map` CLI opens it too. GPS and RTC re-enabled.
- ESP-NOW WPS-style pairing mode: open a timed window (`espnowpairmode`, or the OLED ESP-NOW > Pairing screen) on two same-mesh devices and they broadcast a discovery beacon and auto secure-pair. Admin-only; refuses without a mesh passphrase.
- OLED CLI Input page shows an inline OK/FAILED result screen after each command.
- OLED Power menu: "Restart Device" moved to the top menu behind a Yes/No confirmation.
### Changed
- Setup wizard timezone list shows a representative city (e.g. "US Eastern - New York") instead of only an abbreviation.
- Web logging page: the debug-flag checkbox pane is now wired to its select-all/none and flag-collection JS.
### Fixed
- G2 hijack map and camera pages refresh the 60s safety-timeout on every control tap, so active use no longer drops back to the menu mid-interaction.
- OLED Change Password: the on-screen keyboard now draws when a field is selected (was active but never rendered).
- OLED ESP-NOW settings/device-config keyboards exit on a single B press (was leaving a title-less ghost keyboard).
- Build: prefer `-DIDF_TARGET` over a stale sdkconfig so `idf.py set-target` can switch boards without tripping the target-mismatch guard.
### Docs
- Planning docs: sensor reading-envelope / envelope-cleanup / rendering-unification, ESP-NOW pairing-mode brief, a settings.json lifecycle audit, and a G2 map multi-shade rendering plan.

## [0.97.2] - 2026-07-03
Durable system event log: the firmware now keeps a persistent, always-on record of what it does at the system level, so an unexpected event (like an odd startup message) can be read back from the device instead of reconstructed after the fact.
### Added
- logSystemEvent() writes always-on [EVENT][CAT] lines to /system/sys_logs/system-events.log (256 KB capped ring), independent of debug flags and viewable from the web logging page. Early-boot events are held in a small buffer and flushed once the log system is up.
- Lifecycle coverage across boot, filesystem (mount/format, file deletions, orphan cleanup), settings load/save/failures, WiFi, MQTT, HTTP/HTTPS server, ESP-NOW init and mesh/bond peer online-offline, users, I2C buses and sensors, camera, OLED, voice (SR), LLM, automations, and every reboot path - logged in both directions (came up / went down) where it applies.
- Per-boot orientation divider ("Device Powered On | boot #N | reset=...") written to the login, i2c, and error logs so each log file reads correctly on its own.
### Docs
- New docs/AUTH_LOG_FORMAT.md documents the boot divider and time-sync anchor lines.
- Sensor JSON comments aligned with the unified reading envelope (valid/connected/ts fields).

## [0.97.1] - 2026-07-01
Sensors web page: a remote (ESP-NOW peer) sensor now renders as a readable card instead of a raw JSON dump.
### Added
- Shared `hwRenderGenericSensor` labeled-field card for remote sensors (ToF, thermal, GPS, RTC, presence, IMU), replacing the raw `JSON.stringify` fallback.
### Changed
- Remote gamepad / rotary-encoder now reuse the local card renderer (parameterized `hwRenderAnoState` + a shared `hwBuildAnoInner`); removed the duplicate `hwRenderRemoteInput`.

## [0.97.0] - 2026-07-01
Unified sensor reading format: every sensor now shares one envelope shape, and the bodies were trimmed to just the measurement.
### Added
- Shared reading envelope (`valid`, `connected`, `ts`, `age`) at the head of every sensor's data, via `sensorEnvelopeBegin()`.
- Thermal min/avg/max summary reading (`thermalread json`), also embedded in `sensors json` like the other sensors.
- FM radio readings now carry a real `ts`/`age` (added a `lastUpdate` to its cache).
### Changed
- Sensor bodies trimmed to measurement-only: dropped bookkeeping (`seq`, `total_objects`) and device-state already in the discovery layer (`enabled`, IMU init flags, APDS mode flags).
- ToF emits only detected objects (was 4 fixed slots padded with nulls); dropped the redundant `distance_cm`.
- Not-ready / error readings now report `valid:false` instead of an `{"error":...}` shape; renamed `val` -> `valid` and `timestamp`/`ageMs` -> `ts`/`age` where they differed.
### Fixed
- ToF JSON builder could underflow its remaining-length; APDS read its cache without holding the mutex.
- Disabled the thermal ESP-NOW broadcast: the 768-pixel frame never fit the 200-byte packet limit, so it was built and silently dropped every second.

## [0.96.3] - 2026-07-01
Small CLI fixes: imagesend needs an explicit path, and espnowbroadcast flags no-peers as an error.
### Changed
- `imagesend <device> "<path>"` now requires an explicit path (removed the implicit "send the most recent image").
### Fixed
- `espnowbroadcast` with no paired devices now returns an `Error:` message instead of a plain string.

## [0.96.2] - 2026-06-28
CLI self-documentation: the built-in `help` now describes every module and command accurately.
### Added
- Per-module subsystem overviews printed atop `help <module>` (all 43 modules).
### Changed
- ESP-NOW and bond commands now state when they are asynchronous and name where the result lands (`espnowmessages json`, `espnowtoporesults`, `bondshowremotemanifest`, and so on); fire-and-forget sends say "no reply".
- Fixed stale usage strings: `log` (added `autostart`), `sensorlog` (added `interval`/`autostart`), `power` (real per-subcommand syntax).
### Fixed
- Corrected range bounds: `ledBrightness` (0-100), `tzoffsetminutes` (now reaches UTC+13/+14), `oledBootDuration` (500-10000 ms), and board-derived i2c SDA/SCL pin limits (39 on ESP32-classic, 48 on ESP32-S3).

## [0.96.1] - 2026-06-24
G2 on-device image decode and display-mode robustness.
### Added
- G2 glasses: decode and display JPG/BMP files on-device without a camera sensor; app partition grown to fit.
### Fixed
- OLED Bluetooth and Remote-Settings mode files anchored so `--gc-sections` no longer strips them.

## [0.96.0] - 2026-06-23
Guided recovery after a partial restore.
### Added
- Guided post-restore login flow and cross-device WiFi preservation.
### Docs
- Expanded CLI usage and settings documentation.

## [0.95.13] - 2026-06-21
### Changed
- User-sync now authenticates against the receiving device's admin; synced users land as standard role; `createdBy`/`createdAtSource` split.

## [0.95.12] - 2026-06-20
### Added
- ESP-NOW deferred-writer big-file streaming (chunk-87 fix); espnow-core cleanup.

## [0.95.11] - 2026-06-19
### Added
- JSON command surface; unified auth logging.
### Fixed
- espnow/bond audit; bonded-CLI socket fix.

## [0.95.10] - 2026-06-19
### Added
- Peer metadata over BLE/CLI with a shared serializer; chunked CMD results.

## [0.95.9] - 2026-06-18
### Changed
- G2 ESP-NOW messages UI brought in line with the chunked message store.

## [0.95.8] - 2026-06-18
### Fixed
- Reliable device-to-app secure-channel delivery and framing (BLE).

## [0.95.7] - 2026-06-14
### Changed
- JSON schema-version field renamed `v` to `schema`; unified sensor error shape.

## [0.95.6] - 2026-06-14
### Added
- On-device LLM registered as a first-class feature with auto-start; JSON list-command envelope; HW_BOARD board files extended to ESP32 boards.

## [0.95.5] - 2026-06-09
### Changed
- Migrated to ESP-IDF 5.5.1 (from 5.3.1), including the new `i2c_master` driver.

## [0.95] - 2026-06-01
### Added
- Per-transport session idle-logout, plus accumulated fixes.

## [0.94] - 2026-05-25
### Added
- ANO rotary-encoder support; OLED input unification.

## [0.93] - 2026-05-10
### Added
- Per-sensor sub-flag debug system.
### Changed
- Settings JSON restructure.

## [0.92] - 2026-04-24
### Added
- Video recording.
### Changed
- Logging hardening; SD overflow routing.

## [0.91] - 2026-04-03
### Fixed
- NTP timestamp resolution.
### Changed
- Debug-flag cleanup; UI color variables.

## [0.9] - 2026-03-14
### Added
- Version single-sourced from CMakeLists.txt (`PROJECT_VER`) and surfaced at runtime: boot banner, `status` CLI, OLED, settings JSON, `/api/ping`, and backup metadata.
