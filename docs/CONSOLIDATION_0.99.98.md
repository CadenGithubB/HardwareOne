# 0.99.98 consolidation

This is a development checkpoint on `main`, with ordinary changelog notes.
It does not create a release or tag. The canonical local checkout is the
Hardware One project under `Documents/Codex/Projects/hardwareone`.

## Source inventory

| Work | Preserved input | Result |
| --- | --- | --- |
| Committed 0.99.97.4 fixes | `f4cc542c` | Included once, with all earlier main history |
| OpenClaw and tested XIAO path | `9d2913e8` | Included with relay, skill, settings, UI and tests |
| Current P4 STT/G2 work | `2e56063d` + working snapshot `0b0e91ef` | Included, including 34 modified and 21 previously untracked files |
| Translation investigation | Already tracked in the newer baseline | Preserved; local copies added no new behavior |
| Enclosure references | Existing tracked references + `assembly-r21.png` | Preserved, including the unique local image |
| JPEG / P4X tool branches | Ancestor or patch-equivalent commits | Already represented; no duplicate implementation |
| Old main trainer/deploy changes | Already synchronized or superseded by portable STT scripts | Newer portable versions retained |

Both HardwareOne Git repositories had no stashes. The merge audit compared
surviving STT and OpenClaw paths and the overlapping G2, build configuration,
root CMake and host-test documentation changes. No runtime file or source
change was omitted. Component CMake retains the newer checked flag parser and
adds speaker-label selection to that parser, alongside HTTP source selection.

## Deliberately separate work

Claude's unfinished changes in the separate firmware-review worktree are
excluded at the user's request. Its original files and branch are untouched.
A recoverable local snapshot was taken before that decision; it is not part of
this checkpoint or its pushed ancestry. Claude can merge the updated main
when ready and land those fixes separately.

The standalone speech application's wrappers, reports and device test tools
remain in its own repository. Its 158 shared tool files were already synced;
its older engine files do not overwrite newer P4 work.

Private build copies, flash backups, recordings, training/calibration data,
model exports and checkpoints are retained outside Git. The selected 15x5 gen2
raw model identity remains
`47e58faa01470d32dad88f1faa46e3f61dff0fd605aa542103aefd11502dae72`.
No model retraining or automatic experiment resumption is part of this merge.

## Verification

- Preserved P4 baseline: full build passed; 5,460,320-byte app. All 2,849 SDK
  values and 92 selected feature macros match the qualified donor.
- Nine baseline STT/dictation/G2 production suites passed with ASan/UBSan.
- Combined profile/configuration checks: 46 tests passed, retaining the 15x5
  model, local STT, speaker labels, language model, SDMMC and QuickNote.
- Consolidated HTTP/G2 host suites and combined firmware build: in progress.

Earlier XIAO hardware tests passed HTTPS chat, conversation continuity and New
chat. Earlier P4 hardware tests qualified the current local STT/G2 baseline.
The combined P4 voice-to-OpenClaw path is not yet hardware-qualified. No P4
firmware was changed during consolidation.

The P4 preflight read the live partition table: app at `0x10000`, size
`0x715000` (7,426,048 bytes), LittleFS at `0x725000`. This supersedes the older
`0x615000` test note. The current app was backed up and its ELF identity matches
the qualified `d4e15f5bc` build. Preserve this device's bootloader, table,
NVS, LittleFS and SD assets during the later app-only test.

The OpenClaw integration does not change sandbox, elevation or exec policy.
Strict host-execution isolation remains unverified; see the limits in
[OpenClaw setup](OPENCLAW_SETUP.md).
