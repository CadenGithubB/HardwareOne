# Releasing

How to cut a versioned release of this firmware. Every release gets a commit, a tag
and a `CHANGELOG.md` entry. A GitHub Release is OPTIONAL and occasional - cut one when
a version is worth announcing, using the same notes as its CHANGELOG section. The
CHANGELOG is the complete record; the Releases page is a highlights reel.

This repo is trunk-based: the release commit + tag land directly on `main`.
Commit style is plain-English and version-prefixed, like a changelog line:
`vX.Y.Z: what the change gives you` (e.g. `v0.97.0: unified sensor reading format -
one shared shape for every sensor`), optionally with a `- ` bullet body. Pure-docs
commits use `docs: <plain summary>`. Describe the user-facing outcome, not the
mechanism; do NOT use Conventional Commit type()/scope prefixes.

## Version is declared in (keep all four in sync)
- `CMakeLists.txt` -> `set(PROJECT_VER "X.Y.Z")` - the runtime source of truth; flows via
  `esp_app_get_description()->version` to the boot banner, `status` CLI, OLED, settings JSON,
  `/api/ping`, and backup metadata.
- `README.md` -> `# Hardware One vX.Y.Z`
- `docs/USERGUIDE.md` -> title line
- `docs/QUICKSTART.md` -> title line

## SemVer (pre-1.0)
PATCH = fixes/docs. MINOR = backward-compatible features. Breaking changes bump MINOR (pre-1.0).
An optional fourth number marks a point release on top of X.Y.Z (e.g. `0.99.94.1`); the OTA
protocol, recovery updater and `tools/ota/semver.py` accept it and order it between X.Y.Z and the next patch.

## Gate: before any tag

Every step below must pass on the exact commit you intend to tag, from a clean tree
(`git status --porcelain` empty). They are the "done when" lines of the 1.0 readiness plan
(`docs/RELEASE_READINESS_1_0.md`).

1. Arduino component vendored and patched: `tools/vendor_arduino.sh --check` prints OK
   (a fresh clone: `tools/vendor_arduino.sh`, then ESP32-P4 rows also `tools/p4/prepare_sdk.sh`).
2. Host suite green, serially and in parallel, with Node.js installed so the browser suites run:
   `cmake -S components/hardwareone/test/host -B /tmp/hw1-host && cmake --build /tmp/hw1-host -j6 &&
   ctest --test-dir /tmp/hw1-host && ctest --test-dir /tmp/hw1-host -j6` — 0 failures; "Not Run" only for
   the documented optional suites (`stt_conv_scratch`, the camera managed-SCCB/video suites). The
   OTA protocol suite is part of the same tree.
3. Every stable row of the supported matrix builds from that commit with `tools/build_board.sh <board>`
   (or `tools/build_deployment.sh` for a deployment row) and its BUILD_INFO.md says "clean tree".
4. Tracked tree hygiene: `git grep -nIE '/Users/[a-z]|192\.168\.' -- . ':!components/hardwareone_libs'` is empty.
5. CHANGELOG `[Unreleased]` lists every user-visible change since the last tag; COMPATIBILITY.md and the
   matrix in README match what you built.
6. Private vulnerability reporting is on (SECURITY.md and the issue chooser link to it):
   `gh api repos/CadenGithubB/HardwareOne/private-vulnerability-reporting` returns `{"enabled":true}`.
   If not, the maintainer turns it on under Settings > Code security > Private vulnerability reporting.

## Cut a release
1. Draft notes from `git log <last-tag>..HEAD --oneline`, grouped into Added / Changed / Fixed / Security / Docs.
2. Bump the version in ALL four files above so they agree.
3. Add `## [X.Y.Z] - <date>` to the top of `CHANGELOG.md`. Get the date from `date +%F` - do not guess it.
4. Commit the change as ONE `vX.Y.Z: <plain summary>` commit that carries the code + the version bump (all four files) + the CHANGELOG entry. The version-prefixed commit IS the release - there is no separate content vs `chore(release)` split.
5. Tag and push - push the tag EXPLICITLY, in two commands:
   ```
   git tag vX.Y.Z
   git push origin main
   git push origin vX.Y.Z
   ```
   Do NOT rely on `--follow-tags`: it pushes annotated tags only, and every tag in this repo is
   lightweight (`git tag vX.Y.Z` with no `-a`/`-m`). Using it silently pushes the commit and
   leaves the release untagged on the remote - that is how v0.99.6 and v0.99.91.1 ended up with
   commits but no tags. Verify with `git ls-remote --tags origin | grep vX.Y.Z` before step 6,
   because `gh release create` needs the tag to exist on the remote.
6. OPTIONAL - create a GitHub Release (notes mirror the CHANGELOG section). Write notes to a file and use
   `--notes-file` (NOT `--notes`) so backticks/quotes/apostrophes are not mangled by the shell:
   ```
   gh release create vX.Y.Z --title "vX.Y.Z - <theme>" --notes-file <file>
   ```
7. Verify: `gh release view vX.Y.Z --json tagName,url`; confirm `git status` is clean and local == origin; report the URL.

## This repo's specifics
- Multi-board firmware: do NOT attach board binaries to releases. A single `.bin` is board-specific
  and a footgun; keep releases source-only (GitHub attaches the source archive automatically). Build
  per board with `idf.py set-target <chip> && idf.py fullclean && idf.py build`
  (default boards: esp32s3 -> feathers3, esp32 -> qtpy_esp32).
- Never `git add` build artifacts (`build/`, `*.bin`, `sdkconfig.<board>`) - they are gitignored; do not attach them.
- Release notes are world-readable if this repo ever goes public: keep them user-facing - no secrets,
  credentials, internal paths, or private hostnames.
- Most tags have no GitHub Release and that is fine. As of 2026-08 there are 50 tags and
  24 Releases. A missing Release is NOT a backlog item and needs no backfill - check the
  CHANGELOG entry instead, which always exists. Step 6 is optional by design.
- ASCII only in docs (this repo strips em-dashes): use `-` and `->`, not unicode dashes.
