# Game source editing

- Treat this directory as the editable source for
  `components/hardwareone/WebPage_Games.h`. Regenerate the header with
  `python3 tools/game/dev.py build`; do not hand-edit the generated header.
- Preserve the manifest's fragment order, both original script regions, and
  classic-script scope. Fragments are not independent modules. Do not casually
  reorder or reformat them, enable strict mode, or convert them to ES modules.
- Keep behavior changes focused. After editing, run `build` and then
  `python3 tools/game/dev.py test` from the repository root. A successful tooling
  test validates generation and syntax; use the relevant behavioral suite for
  gameplay changes when the local audit lab is available.
- Update focused tests and known-bug expectations for intentional repairs. Do
  not run the full cross-suite investigation matrix for every small edit.
- Review the source and generated-header diffs together. Include both in any
  authorized commit, staging explicit paths only; never use `git add -A` here.
- Keep local handoff documents and the optional audit lab uncommitted under the
  current project agreement. Core tooling must work without those files.
- Keep this work within the game branch and source/tooling paths. Coordinate
  before touching G2/audio code or shared CMake, `sdkconfig`, and firmware build
  configuration. Do not inspect `WebPage_DarkRoom.h` for this task.
- See `README.md` for the local preview, syntax-engine requirements, optional
  suite commands, and generated-header line-to-source lookup.
