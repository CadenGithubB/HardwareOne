# P4X-EYE display and local input — 2026-09-27

Built and flashed the combined SPI LCD, rotary wheel and three-button profile
on the P4X-EYE. The user confirmed the screen, wheel scrolling in both
directions, tap-to-select and hold-to-back work on the corrected image.
Display and input both started automatically after reboot.

| Check | Result |
| --- | --- |
| SPI LCD | ST7789 connected and enabled; 240×240 panel with the existing 128×64 UI scaled to 192×96 and centered. |
| Wheel | Rotation reaches menu navigation. Tap selects and a 700 ms hold goes back. User confirmed all three behaviors. |
| Three user buttons | GPIO 3/4/5 enabled as X/Y/START. Each input was observed independently during physical presses (masks 4/8/16), followed by released state; 5 ms sampling with debounce. |
| G2 and R1 | Both glasses arms and ring reconnected with the local display/input active, all at ATT MTU 244. Ring setup verified. |
| Encrypted mesh | Two verified 401-byte messages, one each direction between P4 and S3, each in three encrypted fragments with all accessory links active. |
| CPU policy | Final runtime reports Performance at 400 MHz, with 100/200/400 MHz options and a 100 MHz Performance idle floor. |
| Startup | Saved display/input enables and automatic startup survived the reflash/reset. |

The final app is **4,379,504 bytes**, SHA-256
`d51a00ee8395440bf9ecc0d203b8a99e896367fb041c4f6267961c41819bdc61`.
The app partition has 31% free. The SDK configuration and partition table match
the qualified BLE-role P4 build byte for byte. Only the application at 0x10000
was flashed, with esptool verification; bootloader, partitions, filesystem and
saved data were preserved. C6 and S3 firmware were not replaced.
The P4 was left on the main menu at 400 MHz with both G2 links and R1 connected;
the S3 control was returned to BLE Server advertising with no clients. The
coordinator closed both USB serial ports without resetting either board.

ESP-IDF remains 5.5.5 with the same pinned official Bluetooth backport,
Arduino 3.3.5 and Hosted 2.12.13. The separate BLE Server mode, HTTP and G2/R1
implementations remain inherited from the qualified BLE-role snapshot.

## Rotation failure and correction

The first image initialized the LCD and detected wheel turns, but the user
reported that turning did not scroll. The position counter advanced while
the shared login form discarded wheel-only navigation: its early exit looked
only for button presses and joystick deflection. The normal authentication
gate had redirected the requested menu to Login, while the old status path
reported “Unknown” and the mode command echoed the requested mode.

The shared login handler now accepts its existing rotary up/down events, for
either GPIO or ANO input. Status falls back to the existing mode-name table,
and the mode command reports the resulting mode. Authentication remains
enabled. The display was signed into the existing test account with the normal
authenticated `login … display` command before the final physical menu check.
No CPU-specific navigation fork or authentication bypass was added.

The actual login handler is compiled in the focused host regression with
stubbed platform, keyboard and auth boundaries. All three tests pass, including
neutral input, both wheel directions, field wrapping, keyboard behavior,
credential submission and the back-button auth gate. Restoring only the old
guard makes both wheel-direction tests fail. Six other scoped host suites
passed under ASan/UBSan for display mapping/lifecycle, rotary/button cores,
GPIO driver integration and latched input snapshots; their nine implementation
files were verified identical to the frozen experiment sources.

## Evidence and limits

Final serial run: `20260927T195108Z-d422d5e4`. Initial failure/clock run:
`20260927T193936Z-c0455bfb`. Raw logs, test transcripts, firmware and credentials
remain in ignored `private/`. [RESULTS.json](RESULTS.json) contains only selected
non-sensitive facts. Preparation verified all 7,929 source files, and an
independent reconstruction reproduced their hashes.
No panic, assertion, abort or heap-corruption signature was found in either
final-run serial log. Existing disconnected-Wi-Fi lookup warnings and transient
loop-stall warnings were present; this is not a claim of warning-free endurance.

Direct 100 and 200 MHz selections passed with display/input active on the
initial image, then Performance restored 400 MHz. This is not an idle/sleep
qualification. Idle 40 MHz, deep/light sleep, long-duration endurance, SD,
camera and microphone remain untested. SD and both media features are disabled
in this profile. Existing monochrome layouts remain; this is not a native
240×240 color UI.

The known Even-app preparation requirement after accessory reboot remains.
The earlier HTTP and encrypted BLE Server qualification applies to the
[BLE-role image](../p4_ble_roles/results/2026-09-27/FINAL.md); those full suites
were not repeated here. No Mac Bluetooth or Wi-Fi was used.

All source changes are confined to the isolated experiment and its reproducible
overlay. Unrelated main-tree changes and the earlier peripheral experiment
were preserved.
