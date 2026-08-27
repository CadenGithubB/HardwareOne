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
