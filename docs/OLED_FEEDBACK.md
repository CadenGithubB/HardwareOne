# Shared OLED feedback

`OLED_UI` owns transient OLED feedback: one severity enum (`OledUiIcon`),
wrapped renderer, bounded queue and visible lifetime. Screens retain their
forms, navigation and persistent states; they do not draw temporary error boxes
or terminal-result footer messages.

## Producers

- Background activity posts existing system events. `System_Notifications`
  applies device policy, viewer preferences, importance and cooldowns before
  calling `oledNotificationBannerShow`. The persisted banner setting and
  `NSINK_BANNER` name remain supported; the OLED now presents these events as
  wrapped feedback rather than an animated scrolling ribbon. `NSINK_TOAST`
  continues to mean web SSE toast.
- Local command actions use `executeOledCommand`, `oledRunCommandAsync` or
  `oledRunCommand`. Their shared reply mapping preserves the service refusal,
  removes the error prefix and shortens administrator denials. Success feedback
  is emitted only after acceptance. Async acceptance is distinct from operation
  completion.
- An action that ignores the verdict calls `executeOledCommand`, which runs in
  the background (2026-11-22): the line goes to cmd_exec through a
  `PendingCommand` and the frame loop keeps running. One OLED command is in
  flight at a time (shared with `oledRunCommandAsync`); a press while it runs
  shows "Busy" and runs nothing. A small hourglass shows at the top right of
  the content area while it runs. A failure is the toast when it ends: the
  refusal's reason, a full queue after 2 s or the 60 s timeout. A success
  shows no toast, because the screen shows the new state (owner, 2026-11-22),
  except where it does not: those call sites use
  `executeOledCommandShowResult`, which toasts the reply without its
  `OK:`/`SUCCESS:` lead ("Done" when it says nothing). A reply that outlives
  its display session shows nothing.
- Value-scroll rows (FM tune, the LED brightness nudge and preset) never wait
  and never say "Busy" (step 2, 2026-11-22): the row keeps its own wanted
  value, which moves at once; only the newest value is sent, when none of the
  row's commands runs and the OLED command slot is free; a step while one
  runs replaces the waiting value. When the last command ends and the device
  shows the value (or 1.5 s later, or at once after a refusal), the row
  follows the device again (`oledValueScrollStep`, `OLED_ValueScroll.h`).
- `oledRunCommand` waits (up to 2 s + 60 s) and is left only where the screen
  uses the verdict, plus a few chained or completion-dependent actions tagged
  `// waits: <why>`.
- Local form validation uses `oledToastShow` with explicit `ERROR` severity.
  Local feedback is independent of notification preferences and creates no
  system event or notification-history entry merely because it was displayed.
- The remaining direct background producers (voice, RTC and FM status) enter
  through the same notification presentation function. This does not turn
  those existing direct calls into policy-filtered system events.

## Presentation and lifetime

All feedback uses the same box, severity icons, word wrapping and explicit
ellipsis when available lines cannot hold the message. Messages are copied into
192-byte owned buffers. `NONE` requests automatic classification: recognized
error prefixes become errors; other text becomes informational. New failure
callers must specify `ERROR`, since English wording is not a reliable severity.

The default local duration is three seconds; callers can request a duration.
Every error gets at least eight seconds. Feedback automatically dismisses and
never captures input. A confirmation owns the content area and input while
open; feedback is hidden and its remaining lifetime paused until it closes.
A display-session replacement clears active and pending feedback.

There is one active message and twelve pending messages, in arrival order.
Identical active/pending messages with the same severity coalesce without
extending their lifetime. A new message never replaces the active message.
Pending lifetime starts when promoted. At capacity, the oldest pending non-error
is discarded first; if every pending message is an error, routine feedback is
discarded and a new error replaces the oldest pending error. This transient
queue is not a history store; event history remains owned by System_Events.

The message of a system event carries that event's seq (`oledEventBannerShow`,
called by `System_Notifications`). Clearing the notification center calls
`oledEventBannersDismissThrough` with its new watermark: pending messages of the
cleared events are dropped and a visible one ends at once, so the next kept
message takes its place. Local feedback and messages that are not from an event
stay. A repeat that is coalesced into an existing message hands that message the
newer event's seq, so a clear made between the two leaves it.

## Migrated screens

Login, password changes, User Manager and Settings Editor use shared errors.
Automations and Quick Settings use shared terminal results; Quick Settings
retains only ongoing `Working...` screen state. ESP-NOW and notification-settings
commands preserve checked refusals. File Browser failures and relevant camera /
Bluetooth refusals have explicit error severity. Existing map, transcription
and network errors already use the shared path. The unused standalone ribbon
state machine, marquee, banner queue and duplicate timed error renderers were
removed.

## Verification

Host tests execute the shipping queue/renderer for 64-, 88- and 128-pixel
heights, error lifetime, duplicate suppression, overflow, confirmation pauses
(including arrival during a pause), rollover and clearing. Caller tests cover
command replies, async/session fencing, settings, automation drafts, ESP-NOW
ownership, pairing prompts and camera actions. Firmware build results and physical
qualification limits follow.

### Completed software milestone — 2026-11-02

Base revision: `9674eb49`; implementation and tests are the reviewable working-tree
changes in this checkout. Scope ends at shared transient presentation and its
existing callers; persistent screen states, event policy/history and command
permissions retain their owners.

Nineteen focused host suites passed: `oled_toast`, `oled_command_feedback`,
`oled_automation_wizard`, `oled_remote_settings`, `oled_session_role`,
`oled_owner_requests`, `oled_dead_paths`, `oled_power_cpu_rows`,
`oled_pair_prompt`, `oled_espnow_ownership`, `oled_camera`,
`oled_commit_window`, `display_auth_policy`, `notification_integration_guards`,
`credential_attempts`, `dictation_ui`, `command_task_isolation`,
`password_change_gate` and `fm_radio_snapshot`. Queue/rendering and migrated
command, settings, wizard, pairing, camera and session-role suites ran with
ASan/UBSan; the remaining suites used their normal runner checks.

Saved-profile builds passed through `tools/build_config.py`:

| Profile | Image bytes | App-partition headroom bytes |
|---|---:|---:|
| `qtpy-oled` | 2,893,696 | 3,090,560 |
| `xiao-default` (display disabled) | 4,390,400 | 1,593,856 |

Matching manifests and binaries remain in the existing
`build-config-qtpy-oled` (~1.0 GiB) and `build-config-xiao-default` (~1.3 GiB)
directories for reuse and recovery. No extra build directory or source copy was
created. Task-only temporary scripts and redirected build logs were removed
after recording these results.

The initial software milestone did not flash firmware. Physical qualification
remains: check long errors,
bursts, Yes/No prompts, navigation while a toast is active, and login/logout on
the attached display. Host geometry coverage does not establish actual panel
legibility or touch/gamepad behavior.

### Connected P4 deployment — 2026-11-02

At the user's request, built and flashed `p4-phone-pipe` to the connected
P4X-EYE, USB serial `02:48:57:31:04:73`, port `/dev/cu.usbmodem112101`.
Readback of the device partition table matched the saved profile exactly.
Only the application at `0x10000` was written using the generated
`flash_app_args`; bootloader, partition table, NVS and LittleFS were preserved.
Esptool verified the written data hash and reset the P4.

- Image: 6,136,240 bytes; app-partition headroom: 1,289,808 bytes.
- Binary SHA-256: `f3a3328447d63460632b79927234aba2efb48ba2bd88cfd8e8990b3ace1ad090`.
- Boot-reported ELF SHA prefix `b11d536ce` matches the built ELF.
- Boot #45 reported crash count zero, successful LittleFS mount and
  `P4-EYE ST7789 online (spi)`. The 40-second boot capture contained no
  panic/assert markers. User-facing popup appearance still awaits observation.

The existing ~1.1 GiB `build-config-p4-phone-pipe` directory retains the matching
firmware, ELF and build manifest for recovery/reuse. No extra build directory
was created; task-only serial captures, partition readback and temporary logs
were removed after recording the deployment evidence.
