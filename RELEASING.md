# Releasing

How to cut a versioned release of this firmware. Every release has BOTH a
`CHANGELOG.md` entry and a matching GitHub Release with the same notes.

This repo is trunk-based: content commits and the release commit + tag land
directly on `main`. Use Conventional Commits (feat/fix/docs/chore + scope).

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
4. Commit content as conventional commits, then a final `chore(release): X.Y.Z` carrying the version bump + CHANGELOG entry.
5. Tag and push: `git tag vX.Y.Z && git push origin main --follow-tags`
6. Create the GitHub Release (notes mirror the CHANGELOG section). Write notes to a file and use
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
- ASCII only in docs (this repo strips em-dashes): use `-` and `->`, not unicode dashes.
