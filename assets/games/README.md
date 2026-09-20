# Game development

The editable game page lives here. `components/hardwareone/WebPage_Games.h` is
the generated firmware artifact; edit the sources here and regenerate it instead
of editing that header directly. The firmware continues to consume the checked-in
header, so source changes and their regenerated header belong in the same commit.

This first split preserves the original header byte for byte. It changes where
we edit the game, without changing gameplay or the firmware build configuration.
The verified migration SHA-256 is
`3398802a20f09ac0f2064800d9b51999e3f72fb71b672cbfc8dd18007f0ecd8b`.
That records this migration only; future game edits should change the header, so
the hash is not a permanent test expectation.

## Source layout

- `manifest.json` defines the source assembly and JavaScript fragment order.
- `header.h.in` supplies the C++ wrapper.
- `page.html` supplies the page markup.
- `page.css` supplies the stylesheet.
- `src/*.js` contains the JavaScript in named, ordered sections.

Useful starting points within `src/`:

| Area | Files |
| --- | --- |
| Configuration and shared state | `01-config-state.js` |
| World generation and floor queries | `03-level-generation.js`, `05-endless-world.js`, `06-floor-queries.js` |
| Movement and combat | `09-physics.js`, `10-combat.js`, `11-enemies.js` |
| Rendering | `12-scene-depth.js`, `12-render-core-walls.js`, `13-render-floors-ceilings.js`, `14-render-entities.js`, `15-render-hud-menu.js` |
| Loot and companions | `19-companions-loot.js` |
| Frame loop and lifecycle | `21-frame-loop.js`, `22-lifecycle.js`, `23-events-init.js` |

Use `manifest.json` for the complete ordered list, including the separate
`00-errors.js` bootstrap script.

The JavaScript sections are navigation boundaries, not independent modules. Both
original script regions are preserved. The main script's fragments are joined
without added bytes into one classic script with its existing shared scope.
Reordering them, introducing module syntax or strict mode, or broadly formatting
them can change behavior. Establish real subsystem interfaces in separate,
focused changes.

## Edit and verify

Run these commands from the repository root:

```sh
python3 tools/game/dev.py build
python3 tools/game/dev.py check
python3 tools/game/dev.py test
```

`build` assembles the sources, validates JavaScript syntax, and updates only the
generated header after validation succeeds. `check` checks that the header is
current and that the assembled JavaScript parses; it does not update the header.
`test` performs those checks and runs the tooling's unit tests plus the small
shipping cave geometry, support/headroom, lighting, and rendering regressions in
`tools/game/tests/`.
Those spatial tests are not a substitute for full-runtime movement and browser
checks when changing gameplay.

The tooling uses Python 3's standard library, without npm packages or ESP-IDF.
All commands shown here also validate syntax and need a supported JavaScript
engine. The existing portable runner finds Node, Deno, Bun, QuickJS, or macOS
JavaScriptCore/JXA automatically. A missing engine is an error, not a successful
check. This Mac can use JavaScriptCore without installing Node.

Keep a change small: edit the relevant source, run `build`, run `test`, and review
both the source diff and generated-header diff. For behavior changes, add or
update focused gameplay tests and update any known-bug expectations whose
behavior has intentionally changed.

## Browser preview

```sh
python3 tools/game/dev.py build
python3 tools/game/dev.py serve --port 8001
```

Open `http://127.0.0.1:8001/`. The server binds to loopback only. It assembles the
preview in memory for each request and requires the generated header to be
current. After another edit, run `build` and refresh the browser; there is no hot
reload. Stop the server with Ctrl-C.

For entrance comparisons, select **Cave Test** in the terrain menu, then choose
**Descending** or **Hillside**. Its named viewpoint buttons place the player at
the approach, mouth, inside, looking back, chamber, or surface above the chamber.
Side left/right, Oblique, and Roof edge exercise terrain occlusion. Fixed noon
keeps lighting constant for comparisons and restores the cycle when disabled
or leaving Cave Test. **Visibility fixtures** pauses simulation and temporarily
renders an enemy, chest, and fire orb inside the entrance. Compare Mouth/Inside
with Above cave/Oblique to check both visible content and occlusion. Fixture
objects are render-only: gameplay arrays are restored after each draw. Disabling
the checkbox restarts normal Cave Test; leaving Cave Test clears it.
Normal Endless play still
chooses cave sites and styles from its seed. Use Keyboard + Mouse to test walking
between viewpoints rather than relying on teleport views alone.

### Browser performance checks

In Cave Test, choose a viewpoint and use **Profile preview** or **Profile
fullscreen**. Each run pauses the three visibility fixtures, warms up 12 frames,
then samples 60 browser animation frames. Native fullscreen exits automatically;
the separately labelled viewport-size fallback is not native fullscreen.
**Cancel profile** and changes to the scene controls cancel the run safely.
Disable Visibility fixtures afterward to restart normal Cave Test.

The report separates render-command time from actual animation-frame intervals.
It includes inclusive subsystem times: do not add `draw()` to its child stages.
This is a frozen render workload, not a complete simulation or device benchmark.
The Perf HUD also uses animation-frame cadence for FPS, rather than the
reciprocal of CPU work time. Hidden Cave Debug maps are no longer redrawn.

Particle, exploration, and lighting inputs are made repeatable temporarily and
the real world's references are restored afterward. **Verify pixels after
profiling** is off by default. Enabling it adds an untimed full-image/tile hash,
but browser pixel readback can affect later Canvas performance. Use fresh page
loads with pixel verification off for performance comparisons; keep pixel
verification as a separate correctness check.

## Shared cave contracts

`05-endless-world.js` owns seeded networks and their entrance portals. Portal
coordinates are world-space; `deepCaveEntrances` contains window-local copies
with the same dimensions, floor, ceiling, cover, kind, and style. Use
`sampleCavePortal()` for the footprint and signed depth (positive outside),
rather than inventing another entrance-distance threshold.

`queryCaveGeometry()` describes the stamped floor, cover surface, and ceiling.
`covered`/`ceilZ` determine the roof; `isEntrance` is only a mouth-vicinity
diagnostic and does **not** mean “remove the cap.” A descending approach is open
sky outside the mouth. A hillside site must fit actual terrain cover; failed
fits use a descending entrance. Candidate searches run once per cached network.

`06-floor-queries.js` owns support and clearance. `getWalkableLayerTopAt()`
returns `walk`, `fall`, `blocked`, or `missing`; missing geometry has `topH:null`,
not height zero. Do not use a blocked/missing answer as movement support.
`getCaveSpaceAt()` determines underground state from overhead geometry, including
positive-height caves. The lighting cycle may be off without disabling topology.

Heights in those contracts are mesh units. Player and camera stored Z retain
the legacy `60 + 40 * height` encoding; `getCam3D()` converts to rendering's
`60 + 25 * height` eye position. Zero is a valid stored Z, never an absent value.

The first recipe is one connected passage and chamber, with a constant interior
floor and one entrance. Variable elevation, larger branching/dual-exit recipes,
save/load, and richer world content are separate future work.

### Shared rendering contracts

`12-scene-depth.js` owns per-pixel opaque-scene depth. Begin once per 3D frame
with `beginSceneDepthFrame(canvas.width, canvas.height)` before drawing any
geometry; isolated render tests must do the same. Floors, cap tops, ceilings,
wall sides/tops, and entrance solids use this field. Enemies, pickups, companions,
chests, spawners, ore, and spell effects now read the same field. Some structure,
shop, and other legacy rendering paths remain; this is not a complete replacement
of every renderer or a general entity-to-entity depth system.

`projectSceneWorldPolygon()` takes local XY and render-world Z (mesh height ×25),
clips the near plane, and returns screen XY plus camera-forward depth. Split
nonplanar terrain cells into triangles before projecting. Use
`fillSceneDepthPolygon()` for an opaque face after setting its material;
`withSceneDepthClip()` clips transparent decorations/glow without writing depth.
Its callback must only draw that face/effect, not nest another depth clip.
The legacy one-distance-per-column buffer is not terrain visibility authority.

For a billboard, use `withSceneDepthBillboard(bounds, forwardDepth, draw)` with
screen-space `{x, y, width, height}` covering its body, glow, labels, and strokes.
It never writes transparent bounding rectangles into opaque scene depth.
`renderEntities3D()` opts into this path with a `bounds(entity, vis, camera, now)`
callback and uses `groundAnchor:true` for floor-supported objects. Custom chest
faces and extended effects instead use projected world polygons with per-corner
depth. Beam endpoint glow can be unioned through `extraPolygons` in one depth
clip; do not nest clips. Ground effects follow actual terrain triangles rather
than placing one flat screen ellipse over varying terrain.

`sampleEntitySupportRenderZ(x, y, referenceZ, underground, requireTriangle)`
samples the same semantic layer and diagonal as the floor renderer. A finite
reference height selects the nearest support; otherwise underground selects the
lowest support and surface selects the highest. Missing support is `NaN`.
`requireTriangle:true` rejects incomplete triangles for ground decals.
`getEntityRenderFloorZ(entity)` honors finite absolute `renderFloorZ`, including
zero, then uses `underground`/`caveSpawnId` provenance. Do not use the player's
current stratum to position another entity or drop.

Projectiles and impacts store absolute render-world `z`; convert the player's
stored floor once with `(pos.floorZ - 60) * 0.625` before adding hand height.
Companion bounce is local to its sampled floor. Lob target/ground-effect support
must retain the caster's stratum rather than choosing an unrelated floor below
the surface. These are visual/height contracts, not a completed combat
line-of-sight or navigation system.

`getCaveMaterialColorAt(localX, localY)` reads preauthored packed RGB from the
current mesh. Cave floor, ceiling, walls, and rock cut share this world-position
palette; apply lighting separately. Do not use the camera's underground flag
to choose material or suppress an entire buried object. The mouth blends from
its surrounding terrain to deeper stone by signed portal depth. Material
generation and grain are world-seeded and stable across window rebuilding.

Shipping regressions include real seeded oblique/roof-edge renders, partial
entrance burial, an open doorway sightline, and a mutation check that fails if
terrain depth is omitted. Unit tests cover depth interpolation, clipping,
near-plane crossings, buffer reset/resize, and camera-independent material.
Actor, loot, and spell regressions exercise partial burial, zero height,
support strata, extended effects, and depth-bypass mutations.
The depth field preserves full canvas resolution; benchmark larger canvases
separately because cost scales with covered pixels.

Opaque mask painting merges identical adjacent pixel spans into taller
rectangles. Transparent clipping retains the original one-pixel scanlines:
native Canvas antialiasing can differ slightly when those clip paths are merged.
The optimization changes neither depth sampling nor coverage. Shipping tests
compare randomized mask pixels and depth, including an explicit unmerged-clip
guard.

Floor/ceiling corner stitching is cached lazily in `13-render-floors-ceilings.js`.
Only static semantic corner heights are cached, never camera projections,
lighting, material, or draw order. Float64 values preserve the original
calculation precision. Numeric tile storage is capped at 4 MiB plus 6 KiB scratch
and object bookkeeping; a full cache falls back to uncached calculation without
reducing detail. Mesh/dimension changes and `buildWalkCandZ()` invalidate it.
Future in-place geometry edits must rebuild that derived data or explicitly
delete `mesh._floorRenderStitches` before rendering again.

This is a manual game/UI preview with local substitutes for device services. It
has no hardware backend, sensors, or device network connection, and cannot verify
ESP32 performance or hardware integration. Its JavaScript line numbers match the
generated header, which helps trace an error back to its editable source:

```sh
python3 tools/game/dev.py where 7831
```

Use the header line number from the error or investigation. `where` also requires
a current generated header, validates syntax, and reports the corresponding
source file and line.

## Optional local game audit

The existing local audit lab under `tools/webui` adds behavioral checks when it
is present:

```sh
python3 tools/game/dev.py test --suite smoke
python3 tools/game/dev.py test --suite physics
python3 tools/game/dev.py test --suite cave_milestone
python3 tools/game/dev.py test --suite physics --probes
```

Choose the suite for the behavior being changed. `--probes` requires a specific
suite. The full 1,134-cell investigation matrix is not an everyday edit check;
broaden testing when a change crosses subsystem boundaries or reveals a
regression. Known-bug results describe current behavior and need interpretation
when implementing a repair.

The core build/check/test workflow does not depend on the local audit lab or
handoff documents. Those files remain local and uncommitted under the current
project agreement.

## Working alongside G2 and audio development

Keep game work on `codex/html-game` and the other computer's G2/audio work on its
own branch. This workflow requires no shared CMake, `sdkconfig`, or other
firmware-build configuration changes. Coordinate before editing those shared
files or crossing into G2/audio implementation.

Before a future commit, inspect `git status --short` and stage explicit source,
tooling, and generated-header paths. Avoid `git add -A`: this checkout also holds
local audit and handoff material that must stay out of commits. Each machine
should push its own branch; integrate completed work through deliberate merges.
