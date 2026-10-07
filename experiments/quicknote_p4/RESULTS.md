# Glasses hold input and QuickNote, bench log

This is the bench record for the ring or temple hold as a HardwareOne input and
for QuickNote, its first consumer (user guide: `docs/QUICKNOTE.md`). The
implementation follows the local long-press spec. Its hardware checklist
(§B.10) was written for G2 2.2.9.22. The user's glasses run 2.3.2.14, and the
first probe below changed several expectations. The checklist is restated
further down for the build as it stands now. Where the two differ, follow this
file.

## 2026-10-05: hold probe on the deployed build

**Setup.** P4X-EYE running the deployed v0.99.95, which is the build before this
feature. G2 on 2.3.2.14 on both arms: the version arrived with the glasses'
first settings push, and `g2info` confirmed it after `g2glasses refresh`.
`ringstatus` showed `ring=down`, so HardwareOne had no ring link of its own and
the R1 firmware went unrecorded. The user wore the glasses. Logging was
`loglevel debug`, `debugg2 1 temp` and `debugg2events 1 temp`, as in spec B0.1.
Blocks was opened from the glasses twice, and both times showed the six-item
Hub list.

| # | Session | Held | `SysEvent` 9 → 10 (device Δ) |
| ---: | --- | --- | ---: |
| 1 | 1 | right temple, src 1 | 3,271 ms |
| 2 | 1 | right temple, src 1 | 3,300 ms |
| 3 | 1 | left temple, src 3 | 2,460 ms |
| 4 | 1 | left temple, src 3 | 3,840 ms |
| 5 | 1 | ring, src 2 | 3,510 ms |
| 6 | 1 | ring, src 2 | 2,490 ms |
| 7 | 1 | ring, src 2 | 3,720 ms |
| 8 | 2 | ring, src 2 | 2,129 ms |
| 9 | 2 | ring, src 2 | 899 ms |
| 10 | 2 | ring, src 2 | 2,312 ms |

Findings:

- Every plain hold delivered exactly one `SysEvent type=9` when the hold was
  recognised and one `type=10` when the finger lifted, in that order. Both came
  from the right arm only. There was no left-arm mirror and no List or Text
  copy, and no FG_ENTER/FG_EXIT overlay came between them. `reason` was 0
  throughout.
- The source was filled in: right temple 1, ring 2, left temple 3. Ring holds
  reached HardwareOne through the glasses even though HardwareOne had no ring
  link.
- In session 1, where the user held for about 2 to 3 s, press to release took
  2.4 to 3.8 s. Session 2 included shorter holds, down to 0.9 s.
- Nothing native got in the way inside the Hub page. No QuickNote session, no
  shortcut traffic and no other sid appeared around the holds, only heartbeats
  and SyncInfo. The Even app's Press & hold binding was not recorded.
- **The holds did not count as activity.** Session 1 ended with
  `Firmware ended widget (safety-timeout) after 63.146s`, 63 s after Blocks
  opened. The user had made seven holds and no taps, and the last release came
  15 s before the timeout. Session 2 ended the same way after 61.015 s, with
  three holds. Each time the expected reason-1 `SYSTEM_EXIT` echo followed.
- While Blocks was closed between the sessions, Silent/DND turned on and off
  again (20:43:44 and 20:43:52). No 9 or 10 can reach HardwareOne without one of
  its pages up, and the cause was not recorded.
- Not covered: tap then long-press (H4), a both-temples hold on a HardwareOne
  page (H3), and a text page.

What the build took from this (approved 2026-10-05):

1. Hold profiles (R19). L = R = 2.2.9.22 is trusted with an unreliable source,
   since static analysis shows src is always 0. L = R = 2.3.2.14 is trusted with
   a reliable source, as verified here. Matching stays exact, and any other pair
   is `unprofiled`. R20 still applies on 2.3.2.14: HardwareOne never emulates,
   answers or suppresses native QuickNote or shortcut traffic, and never writes
   the gesture table. It only consumes 9/10 that reach its own foreground page.
2. Setting `g2holdsource` (`0` Ring only, the default; `1` Ring + temples).
   With a reliable source, the hold action and the Notes page take only src 2
   unless `g2holdsource` is 1. With an unreliable source (src 0 on 2.2.9.22),
   any hold is accepted. A press this rejects is recorded as
   `Press→ignored:source`, and its release is absorbed.
3. Any admitted hold on a HardwareOne page counts as user activity and restarts
   the 60 s safety watchdog, whether a consumer takes it or not. A running note
   also holds the page open (R13).

Raw evidence: a serial capture from 20:41 to 20:45 local time, timestamped by the
host and the device. It is kept outside the repository, because serial captures
can carry typed credentials.

## Build under test

Not yet flashed. Built 2026-10-06 from `a172013` plus the uncommitted feature
work:

| Board | App image | Partition | Free |
| --- | ---: | ---: | ---: |
| XIAO-S3 | 5,880,096 B | 5,984,256 B | 104,160 B |
| P4X-EYE | 6,486,064 B | 7,426,048 B (0x715000) | 939,984 B |

## Hardware checklist (spec B.10, restated for G2 2.3.2.14)

Bench: P4X-EYE with G2 2.3.2.14 on both arms and the user's R1 (record its
firmware from the Even app). The CM5 rows use XIAO-S3 with a CM5. A row passes
only with every condition in its Pass column. Fill in Result with the date, the
build and what was seen.

| ID | When | Steps | Pass | Result |
| --- | --- | --- | --- | --- |
| H0 | first | `g2info` for L and R (an admin runs `g2glasses refresh` if it shows `fw=?`), `g2hold status` and `ringstatus`. Write down the R1 firmware and the Press & hold bindings (left, right, ring; screen on and off) shown in the Even app. **Do not accept firmware or app update prompts** | L = R = 2.3.2.14, or L = R = 2.2.9.22. `g2hold status` reads `trusted=yes (2.3.2.14) sourceReliable=yes`; on 2.2.9.22 it reads `sourceReliable=no`. Any other pair reads `trusted=no (unprofiled)` with the hint, and holds then act on nothing | |
| H1 | B1.2 / B1.3 | Blocks open on the Apps list, with `loglevel debug` and `debugg2events 1 temp`. (a) `g2holdaction 0`: hold the ring, the right temple and the left temple, 2 s each, three times each. (b) `g2holdaction 1`, `g2holdsource 0`: one ring hold while speaking a few words, then one hold on each temple. (c) `g2holdsource 1`: one hold on each temple. Read `g2hold status` after each set | Each hold gives one `LONG_PRESS(9)` and one `LONG_RELEASE(10)` on R, `via=sys`, with src 2 for the ring, 1 for the right temple and 3 for the left. No L mirror and no FG overlay. Rows: (a) `Press→ignored:action-off` for all three sources. (b) ring `Press→quicknote`, with Notes opening on `● REC`; temples `Press→ignored:source`, with nothing on the lens. (c) temples `Press→quicknote`, or `Press→page:24` on the Notes page. Every press has its `Release` row, and `OrphanRelease` and `PressAfterLostRelease` stay 0 | |
| H2 | B2.6 | On Apps, hold the ring, speak for 5 s, release | The lens swaps to Notes `● REC` and `speak now` appears. The release arrives, and the header shows `✓ Saved · N words`. `note status` is then idle | |
| H3 | B1.2 | Both-temples hold on a HardwareOne page, once with `g2holdsource 0` and once with `1` | Silent toggles (`Silent/DND ON/OFF`). Record whether a 9 comes first and its src, and whether with `g2holdsource 1` it starts a note | |
| H4 | B1.3 | Tap then long-press, and double tap, on a HardwareOne page | Overlay or exit as before. No hold dispatch: no `Press→quicknote` or `Press→page:` row, at most `ignored:overlay` or `Aborted(…)` | |
| H5 | passive | Watch for `SysEvent: type=ABNORMAL_EXIT(6)` | Note when it happens | |
| H6 | B0.2 | Tap a non-Blocks Hub plugin entry while HardwareOne is host | B0.2 is not built. Record how long the loading page stays (baseline only) | |
| H7 | B1.3 / B2.6 | Watchdog (new). On Blocks, with no taps. (a) `g2holdaction 0`: a 2 to 3 s ring hold about every 10 s for 90 s. (b) `g2holdaction 1`, `g2holdsource 0`: temple holds the same way, which are ignored as `source`. (c) `g2holdaction 1`, `notemaxsec 120`: one ring hold of about 75 s while speaking | No `Firmware ended widget (safety-timeout)` in (a), (b) or (c), and in (c) the note is saved. Known limit: a single hold longer than 60 s that starts nothing still ends Blocks 60 s after its press, because nothing arrives while the finger is down. Record it if seen; it does not fail the row | |
| H-QN | B2.6 | 10 notes each of 3, 10 and 30 s, on the Notes page and from another HardwareOne page. Mid-note: tap then long-press (overlay), let the lens blank, take the ring out of range, wake Even AI, start Conversate from the menu, storage near the reserve, 20 notes in a row | The right Saved, Discarded or Failed reason. After each note, `note status` shows `"available":true` (or the right reason), and `g2micstats` counters stop climbing, so the glasses mic is off. Heap and PSRAM stay steady, and there are no ring resets | |
| H-CLI | B2.5 | Web `note start` / `note stop`; serial `note start glasses`; serial `note start`; `note start glasses` from an account that is not the paired wearer, including an admin | Web and `note start glasses` start a note and save it. Serial `note start` answers `From this console use: note start glasses`. Every account other than the paired wearer is refused | |
| H-PRIV | B2.5 | Two accounts, a guest, an admin and a superadmin: `note list/read/delete`, `note stop/cancel/status <id>`, `events`, SSE, logs | `note list/read/delete` work only for the note's owner or a superadmin; a plain admin is refused like any other account. `note stop/cancel/status <id>` work for the owner or an admin, and `status` never shows text. Guests cannot record. No note text appears anywhere except the owner's own replies: events, SSE, logs and notifications carry none, and the mirror shows `[private STT result]` | |
| H-REG | each step | Transcription app (including `status` with no id during a note), keyboard dictation, ring list navigation, double-tap exit, Conversate local, Even AI | Unchanged | |
| H-PHONE | end | Power HardwareOne off and reconnect the phone with the Even app | The glasses menu is the user's own, and the Gestures page is unchanged. **The Press & hold bindings match what H0 wrote down.** Blocks still opens from the phone, and the app works normally (R1 to R5) | |
| Q9 | each step | Rebuild both boards and read `build-*/BUILD_INFO.md` | P4 below 0x715000 (7,426,048 B). S3 headroom at least 100 KB (102,400 B), or `ENABLE_QUICKNOTE 0` on S3 | 2026-10-06: pass. P4 6,486,064 B (939,984 B free); S3 5,880,096 B (104,160 B free) |
