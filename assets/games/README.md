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
| Biome palette and shared object materials | `01-materials.js` |
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

## Shared palette and material authoring

`src/01-materials.js` is the appearance-data entry point. It loads before
`01-config-state.js` and has no DOM, Canvas or gameplay initialization. The
existing `BIOME_PALETTE` and floor-band thresholds moved there without changing
their values or public names. Biome colors still feed terrain, walls and sky;
`makePattern()` now reads `patternBase` instead of duplicating its base color.
Its existing four recipe choices/fallback and inline fine-detail colors remain.

`GAME_MATERIAL_COLORS` contains named base-color families. Approved visual
passes can extend a family with additional authored roles without changing its
consumer API:

| Material | Connected consumers |
| --- | --- |
| `caveStone` | Authored interior albedo, mineral variants and stable legacy fallback; cave floor/wall/ceiling/rock-cut readers consume the shared field |
| `entranceStone` | Constructed entrance pillars and lintels |
| `palisadeWood` | Entrance flanking planks |
| `rubbleStone` | Rubble and rock piles |
| `crateWood` | Crate body, bracing, broken interior and outline |
| `wallPropWood`, `wallPropIron` | Constructed wall mounts, shafts, frames, rims and bosses |
| `wallHeraldry`, `wallFlame` | Banner/shield fields and low-cost torch/sconce flame layers |
| `floorPropWood`, `floorPropIron` | Barrels, fallen timber, debris, chains and other floor clutter |
| `floorFoliage` | Grass, ferns and dry floor vegetation |
| `bone` | Bones, dry bones, skulls, rib cages and femurs |

Author six-digit hex colors in that object. `compileGameMaterials()` validates
them once at startup and derives the read-only `GAME_MATERIALS` catalog. Use
`.hex.role` for Canvas styles, `.rgb.role` for channel arithmetic, or
`.packed.role` for stored RGB integers; do not duplicate a color in each format.
For example, `GAME_MATERIALS.crateWood.hex.base` reads the crate's base color.
Ordered `.swatches` are reused by rubble/rock-pile variation; their order follows
the named colors in the source definition. Keep that order intentional.

After editing colors, run `build` and reload the page. Cave colors are authored
into chunk meshes and some artwork is cached: changing a global halfway through
a session is **not** a supported live-theme operation. Materials remain base
appearance rather than texture maps or geometry. Authored cave variation clamps
RGB channels so near-black/white material colors cannot overflow.

Crystals, many other props, larger structures, creature/hand sprites,
spells and HUD colors still have local definitions. Migrate those incrementally
as their art changes; do not mistake this initial catalog for complete engine
coverage. Materials describe base appearance, not a physically based renderer.

The shipping material regression records production drawing commands and checks
the approved command baseline for eight prop types, seven pattern inputs and
three entrance styles. It also verifies that editing shared definitions reaches actual
consumers, validates compiled forms, and checks black/white grain boundaries.
Its compatibility hashes are intentional visual baselines; update them only
when an approved art change warrants it. These are command-level checks, not a
replacement for native-browser visual review of a new art style.

### Indoor stone V1 (2026-09-20)

The layered cave renderer is currently the only world path with a unified
ground, wall and ceiling interior. Fortresses, arenas and watchtowers still use
surface terrain and grid walls beneath their world-authored exterior shell;
this pass does not pretend they are closed rooms.

The cave interior now uses the low-saturation **Ashen Reliquary** treatment:
the walkable floor is the lightest navigation plane, walls use a soot-dark roof
join/readable middle/contact-dark base, and the ceiling is the darkest enclosing
mass. Existing amber point lights warm all three roles consistently, while
ceiling light response stays restrained. This indoor-stone pass does not alter
the exterior cap, HUD, crosshair or any other UI rendering.

`sampleInteriorStoneAlbedo()` authors fine 12-unit grain plus broader 48/96-unit
base, damp, iron and worn-mineral fields into the existing packed `caveStone`
mesh value. Hashes use absolute world coordinates and no random/camera state,
so the material remains stable across yaw and streaming-window rebases. Mouth
rock still blends continuously from the pristine neighboring terrain palette.
`shadeCaveSurfaceColor()` applies allocation-free role, local-light, fog and AO
arithmetic at draw time and returns packed RGB for the existing `rgbQ()` cache.
That shared cache now rotates on a novel color after 4,096 retained entries;
hot colors remain hits, while long sessions cannot retain every fog/light mix.

This V1 adds no polygons, texture canvases, patterns, blur, dynamic lights,
collision changes or extra depth submissions. Walls retain one cached gradient
per quantized color/screen range; the interior/exterior ramp bit is part of the
cache key. Helper-level material tests cover role ordering, bounded mineral
fields, warm local-light response, deterministic authoring and safe fallbacks.
Debug-off production regressions pin the floor/ceiling colors and both wall
ramps, including an interior/exterior same-color cache-key collision; seeded
render regressions separately preserve geometry and depth.

Informal Indoor V1 checkpoint: the same seed-12345 Inside scene at 360×240 measured
10.1 / 10.7 ms median / p95 render commands before the pass and 10.2 / 10.7 ms
after it. Final wall and ceiling stages were 2.4 / 2.8 ms and 1.6 / 1.7 ms;
animation-frame cadence remained 16.7 ms median. Visual review covered the
descending Inside/Chamber/Look-out views and the hillside Chamber view. This is
one fresh-page native-browser sample, not a device/GPU/thermal benchmark; no
fullscreen check was run, and this indoor-stone pass made no UI changes.

### Ruined Hut V1 (2026-09-20)

Ruined Hut dressing is now authored in structure-local world space. Rubble,
the waypost and the two partial roof beams no longer depend on camera distance;
they follow terrain height, clip at the near plane and use the shared scene
depth field. This removes the old orbiting debris, hillside intersection and
whole-decoration pop as the player approaches. Only the depth-clipped label
fades; solid ruin pieces remain opaque when they write depth.

The hut footprint is a rotated broken 5×4 outline with a three-cell entrance,
several low wall heights and a deliberately missing corner. Generation selects
a nearby dry, reasonably level site, reserves a clear 7×7 pad, removes stale
scatter/decor/actors from that pad and assigns explicit rubble-stone colors.
Authored ruin and structure walls are excluded from the natural forest
bark/canopy path. The HUD and controls are unchanged.

### Wall Props V1 (2026-09-20)

Constructed wall items now read as mounted objects instead of flat symbols.
Torches have iron backplates/brackets, tapered wooden shafts, retaining bands
and three flat flame layers. Sconces have a separate plate, projecting arm,
bowl and flame. Shields use a pointed heater silhouette, rim, field, heraldry
and raised boss; banners use a crossbar, finials, cloth folds, swallowtail hem
and one of three deterministic heraldic treatments. Stable cell/side hashes
choose the variants, so they do not crawl or change as the camera moves.

The sweep also repairs a visibility bug in endless generation. Its former
`stalactite`, `crack`, `moss`, `frost_crack` and `vine` names did not match any
3D recipe. New chunks emit canonical types, and cached chunks normalize those
old names as they enter the active window. Cracks now have a real branched wall
recipe and a matching 2D overview mark. All twelve generated wall-prop types
have command-level coverage.

This remains a bounded Canvas path pass: no gradients, filters, blur, texture
canvases, pixel reads or new lights were added. On the native 360×240
seed-12345 Approach fixture, `sceneEntities` measured 1.0 / 1.1 ms median / p95
before the pass and 1.0–1.1 / 1.1–1.2 ms across two post-change runs. Whole-frame
render commands moved from 11.3 / 12.0 ms to 11.2–11.4 / 11.5–11.9 ms, with
16.7 ms median animation cadence. Treat these as single-host command timings,
not an ESP32/GPU/thermal guarantee. Wall geometry, collision, depth clipping,
controls and HUD are unchanged.

### Structure Shell & Floor Clutter V1 (2026-09-20)

Fortresses, arenas and watchtowers now have distinct world-authored silhouettes.
Fortresses gain gate lintels, capped corner towers, a six-sided central canopy,
gabled inner roofs and four standards. Arenas gain rotated gate lintels,
standards, a pillar entablature and a central pedestal. Watchtowers gain a tall
central cap plus rotated roofs and standards at each generated arm. These pieces
use the same sampled structure-floor height as the grid walls and write through
the scene-depth path; the former absolute-height gazebo and screen-space banner
arrays were removed. Rotation, palette and generated structure counts now
survive chunk-window assembly instead of being discarded.

The flattened courts also receive subtle structure-specific paving in their
generated color mesh: staggered fortress stones, arena rings and watchtower
spokes. Because this is baked while a chunk is generated, it adds no recurring
draw calls. Existing fortress interaction objects and sealed markers now project
from the same floor height instead of world zero.

Floor scatter now uses biome and structure-specific pools, stable four-cell
clusters and deliberately clear circulation cores. Raised clutter receives one
small contact ellipse, while puddles and ground cover do not. Shared wood, iron
and foliage material families cover the new sweep, and every generated floor
type has an explicit perspective tier. Prop recipes multiply rather than replace
the caller's alpha, fixing distant clutter that previously popped through fog,
lighting and spawn fades. The clustering, recipes and shell face counts are all
fixed and bounded; no particles, filters, gradients, pixel reads or additional
lights were added. UI, controls and collision are unchanged.

Native 360×240 seed-12345 Approach checks used 12 warmups and 60 frames. The
standard fixture measured 11.6 / 12.1 ms median / p95 render commands after the
pass versus 11.4 / 11.6 ms at its pre-pass checkpoint; `sceneEntities` remained
1.1 ms median (1.3 ms p95 versus 1.2 ms), and animation cadence remained 16.7 ms
median. A second run with the Structures fixture enabled measured 11.4 / 12.2 ms
overall and 1.1 / 1.2 ms for `sceneEntities`. These are native Canvas command
timings on this Mac mini, not an ESP32/GPU/thermal guarantee.

### Atmospheric Sky V1 (2026-09-20)

The surface sky now uses four zenith-to-horizon color stops blended across
biomes, time-coherent atmospheric mountain layers and a restrained horizon
haze. A visible world-oriented sun follows the same corrected solar phase as
directional lighting: sunrise is `0.25`, noon is `0.5` and sunset is `0.75`.
At night the opposite moon and a deterministic 48-star catalog fade in. Ten
fixed cloud clusters form two slow parallax bands with biome-specific coverage.
The day/night setting still fixes the scene at noon when disabled, and no HUD
or settings controls changed.

Sky work is bounded rather than particle-driven. Stars are submitted in two
paths and clouds in two paths; there are no texture canvases, per-pixel loops,
blur/shadow filters, image readbacks or weather simulation. Unit coverage pins
phase timing, deterministic commands, cave suppression and command-count caps.
On the native 360×240 seed-12345 Approach fixture, the sky stage measured
0.1 / 0.2 ms median / p95 both before and after this pass. Whole-frame render
commands were 11.3 / 11.5 ms before and 11.4–11.5 / 11.8–12.2 ms across two
post-change samples; median animation cadence remained 16.7 ms. These are
Canvas command timings on this Mac mini, not mobile GPU or thermal guarantees.

## Artwork cache pilot and gallery

The **Prop cache (pilot)** checkbox is opt-in and resets on reload. It caches
only 3D floor-scatter `fern`, `fallen_log`, `skull`, and `rubble` artwork. All
other props and the 2D map keep their original drawing paths. The four recipes
are still authored once in `paintFloorItem()` in `07-decorations-lighting.js`;
the original `drawFloorItem()` entry point remains available.

`07-artwork-cache.js` contains the small `FLOOR_ARTWORK` registry, with labels,
anchor-relative bounds and shared-material dependencies. Skull/rubble already
use `bone`/`rubbleStone`; fern/log colors are still local to their recipes. This
is a foundation for designer tooling, not a completed asset/theme system.

Run the regular preview server and visit `/artwork.html` for side-by-side
original/cached artwork on daylight/dusk backdrops. Its verification button
checks 300 native raster cases and the production terrain-depth clip. Its
measurement button compares 120 mixed props at 360×240 and 1280×720, with both
fixed and changing projected sizes. It reports first-frame cost, 80 warmup
frames, 60 alternating-order sample pairs, hit/miss/build counts and raw pixel
memory. It does not measure full gameplay FPS, GPU completion, or the cost of
the game's projection/depth passes. Reload before timing after pixel readback.

Implementation limits:

- Exact seeds/variants/integer sizes (6–70 px) retain procedural variety. No
  seed quantization. Each image is baked at twice its destination resolution;
  fractional placement and antialiasing can differ from direct vector drawing.
  Native visual acceptance is required before enabling the pilot by default.
- LRU eviction caps retained RGBA pixel storage at **4 MiB** and **384 entries**.
  Canvas/JS/driver overhead is additional, not included in that byte budget.
  Cold generation is limited to four images and a soft 1 ms per scatter pass;
  one build can exceed the time limit. A miss over budget uses the old recipe.
- Placement, support, ordering and the existing depth clip stay live. Cached
  transparent rectangles never write opaque depth. Nondefault transforms,
  compositing, filters, shadows, dashes or unsupported inputs keep the old path.
- The four legacy recipes override parent alpha. Cached drawing preserves that
  behavior; this change does not fix/reinterpret fog, spawn fade or lighting.
- Palette catalog replacement or painter-function replacement invalidates the
  cache. Normal source edits still require `build` and reload. In-place hot art
  changes must call `clearFloorArtworkCache()`; this does not rebuild cave meshes.

Developer hooks: `setFloorArtworkCacheEnabled(bool)`,
`getFloorArtworkCacheStats()` (cumulative counters, current retained bytes),
`clearFloorArtworkCache()`. Outside the game scatter pass, call
`beginFloorArtworkFrame()` once per render pass to reset the cold-build budget.
Unit checks cover identity, invalidation, LRU/budgets, allocation fallback,
caller state, production clipping and gallery script assembly. They cannot
substitute for the native-browser measurements and moving-game review.

Pilot checkpoint (2026-09-20): core + smoke tests pass, but native-browser
appearance/timing verification is still pending because this session could not
serve/open the local test page. No performance improvement is claimed yet.
Keep the checkbox off by default until that comparison and a moving-game
occlusion check have been reviewed.

## Articulated hand and robe V1 (2026-09-20, preview-only)

In **Cave Test → Casting studio**, **Hand motion study** is the default study.
Choose **Articulated hand** and **Play casting flourish (¼ speed)** to inspect one
material-finished right hand in the actual game canvas. The phase selector, timeline,
and player/side/back views expose finger curl, thumb opposition, wrist rotation,
forearm deformation, robe construction, and recovery without glow or textures
hiding the shape.
Selecting the hand, a style, or a resting template pauses at Ready. **Handle
grip** is the default: a closed upright fist for a vertical staff. The hand and
forearm cross-sections roll around their longitudinal axis while preserving the
arm's centerline; there is no camera-space twist bending the arm into a hook.
Its arm pose brings the forearm up beneath the wrist, not diagonally across from
the left. Grip orientation is shared across casting styles.
At maximum extension the wrist pitches about 20° forward, so an imagined staff
tips toward the cast, then smoothly returns to its upright resting orientation.
The handle's placement also thrusts away from the camera, slightly up/inward,
then recovers to rest. It shares this motion across styles instead of inheriting
Finger Guns' free-hand recoil; the approved wrist tilt and closed grip remain.
This changes the hand presentation only, not cast timing or projectile direction.
**Reach** remains available as the free-hand casting-flourish study. The Arcane
rest shows the back of the hand with fingers reaching away into the scene. The
palm stays facing away throughout its cast; gathering uses finger curl and a
small wrist flex, not a flip toward the camera or an idle flourish. **Resting pose**
offers Reach, Handle grip, and Cradle; these are shape templates, not new items.
Handle grip stays closed during playback; choose Reach to inspect finger opening.
**Palm study** in Hand inspection view presents the same mesh face-on, with a
fixed wrist orientation and placement, for reference-silhouette comparison.
It keeps the small game canvas and does not change the Player view animation.
**Original baseline** shows the original hands. **Cancel / restore game** returns
the previous gameplay state. This study does not cast spells or simulate damage.
Keep automated/native visual checks in the small preview; the user has explicitly
requested no further fullscreen tests. Manual fullscreen controls are optional.

`15-hand-rig.js` owns the cached mesh topology, authored 2800 ms motion study,
articulation, projection, and material-aware vertex lighting. It draws native Canvas2D
triangles, not a transformed image or WebGL surface. Normal gameplay does not
enable `HAND_RIG_PREVIEW`. The rejected flat drawing remains archived in the
studio mode selector, with `CASTING_ART_ENABLED` off by default.

The model has 1117 vertices and 2136 triangles. Four extra proximal sleeve rings
continue the arm beyond the player viewport without changing the original wrist
or forearm shape. Their width stays bounded; no screen mask hides the arm entry.
Four additional near-wrist rings shape a warm turned-linen edge, a fitted cuff,
and two narrow antique-gold trim bands while preserving every original forearm
station and the exact hand seam. The cloth uses asymmetric 3/5-lobe geometry
instead of a screen texture, so its long folds rotate and deform with the arm. A
two-sided cloth fan closes the normally cropped arm entry when the supported Back
inspection view turns it toward the camera.
Only topology and numeric working arrays are reused; this is not an
image/animation-frame cache. The 106,448-byte numeric working storage excludes
JS topology objects, temporary allocations, and browser/driver memory.
`getHandRigStats()` separates mesh build
and render-command times; `clearHandRigCache()` starts a fresh first-use check.

The robe/skin treatment is the V1 foreground look for evaluation, but this rig is
still a preview gate rather than a finished gameplay replacement. Finger
roots currently intersect the palm volume rather than forming a welded skin
mesh; webbing, thumb saddle, and joint contours need refinement. The wrist seam
has matching positions through deformation. Triangles are culled and sorted by
depth; this is not a general hand self-occlusion solution for arbitrary crossed
fingers or intersecting poses. The hand never changes the world scene-depth
buffer, and world-space spell effects still use its existing clipping helpers.
Lighting retains the study's ambient/diffuse/rim model, now mapped through shared
`casterSkin`, `casterCloth`, and `casterMetal` ramps. Equipped robe colors replace
cloth roles individually; malformed or partial roles fall back safely while
linen, trim, and skin stay shared. There are no nail details, skin textures, or
finished hand-to-missile release integration yet.

The separate combat work from the prior pass retains the user-approved 120 ms
Magic Missile wind-up. It reserves the existing route-specific cost/cooldown on
accepted input, snapshots projectile modifiers, samples aim/position at release,
and cancels/refunds pending reservations on pause/modal/hidden-page transitions.
Existing keyboard/gamepad differences in robe discounts and split-shot behavior
remain unchanged. The slow hand-study timeline is not that gameplay timing.

Verification covers generation, hand winding/deformation/seam/no-gameplay writes,
studio state restoration, and queued-cast/depth regressions. Native measurements
must be taken in fresh pages, without pixel readback before timing. Do not infer
phone, fullscreen, or complete two-hand/spell costs from the one-hand mini view.

Earlier native Mac mini checkpoint (before the outward-rest/style changes),
360×240 preview, seed 12345/Mouth, prop pilot off:

| Measurement | Daylight | Dusk |
| --- | --- | --- |
| Rig mesh first-use build | 0.5 ms | 0.6 ms |
| Rig render commands, median / p95 | 1.0 / 1.1 ms | 0.9 / 1.1 ms |
| Whole frame with original idle hands, median | 10.6 ms | 10.6 ms |
| Whole frame with moving right-hand rig, median / p95 | 11.6 / 13.2 ms | 11.6 / 13.1 ms |
| Browser frame interval, median, both versions | 16.7 ms | 16.7 ms |

Each version had one 144-frame cold/warmup cycle followed by 144 measured frames.
The original baseline is idle artwork, not a matching articulated pose workload.
The original version runs first; its first whole-frame time also includes colder
world rendering. Do not interpret that first-frame comparison as a startup win
for the rig. These are command-submission/cadence measurements, not GPU-fence or
mobile thermal results. A separate geometric depth audit found small painter
ordering errors at finger/palm junctions; production needs welded geometry or a
private hand-depth solution before detailed contact/crossed-finger poses.

### Casting Style preference and animation contract

**Casting Style** beside the main controls is a personal preference, not an
unlock, loot item, spell upgrade, or graphics preset. **Arcane (default)** and
**Finger Guns** are available immediately. The studio has a synchronized style
selector. The articulated mesh is still preview-only: this setting does not silently
replace normal-game hands with an unfinished model. Gameplay timing, effects,
equipment and the existing missile wind-up remain unchanged by style selection.

`01-casting-styles.js` owns stable IDs, labels and preference handling. Selection
is saved independently at `hardwareone.casting-style.v1` in localStorage; it is
browser/origin-local, not a world save or account sync. Unavailable/blocked
storage keeps the current session usable. Unknown IDs fall back to Arcane.
Use `getCastingStyleOptions()`, `getSelectedCastingStyle()`, and
`setSelectedCastingStyle(id)` rather than mutating `settings.castingStyle`.
`onCastingStyleChange(listener)` returns an unsubscribe function for controls.
The preference intentionally survives studio cancellation; temporary mesh,
world, light, and pose state do not.

`15-hand-rig.js` contains data-only `HAND_RIG_ANIMATIONS` recipes sharing one
skeleton and renderer. Add a descriptor in `CASTING_STYLES` and a matching
`handAnimation` recipe to extend the set; do not add style branches to combat.
Recipe frames are Ready / Gather / Release, with local-radian root rotations,
per-digit joint curls (index, middle, ring, little, thumb), thumb opposition/twist,
spread, distance and screen anchors. Finger Guns keeps the index and middle
extended together toward the crosshair through gather, release and recovery.
Ring and little fingers extend nearly straight from the palm, then fold at the
middle knuckle and return toward the palm at the tip joint; the curl no longer
starts with a deep bend at the palm. Finger Guns presses the thumb
with small wrist recoil. The player-view palm-radial axis stays upright; pitch
and yaw supply the aim instead of a sideways whole-hand cant. This does not
change the photo-referenced finger shapes, Palm study view, or Handle override.
The thumb now follows the user's raised/pressed photo
references: a broad radial base, an open L-shaped thumb/index gap at rest, and a
tip that folds forward above the index at release. The previous assumption that
the thumb should lift out of the palm in the fingers' flexion plane was wrong.
Its bend frame lies nearly in the palm outline plane, with a slight depth cant;
the planted base stays steady while the knuckle bends modestly and the distal
joint supplies most of the press. Both signed joint changes share the existing
release/recovery blend; there is no new gameplay timing or topology.
This is foreground pose alignment, not a change to projectile aim.
Handle/cradle digit constraints
remain independent of style and cannot be opened by a casting recipe. Actual
item contact targets and attachments are not implemented by these templates.
`handRigPlacementFrames()` gives an authored grip `placement` priority over the
style's distance/screen anchors. Handle uses this for its shared forward thrust;
Reach and Cradle still resolve their placement from the selected style.

Use `sampleHandRigAction(action, elapsedMs, grip, styleId)` for an explicit
`idle` or one-shot `cast` action. Idle ignores advancing time; completed casts
clamp to rest. `sampleHandRigRestPose(grip, styleId)` samples rest directly.
`sampleHandRigPose(timeMs, grip, styleId)` is the deliberately looping studio
sampler, **not** a gameplay idle driver. Its 2800 ms study timeline still needs
an approved combat-event adapter before production use; it is not permission to
delay projectiles by 2800 ms. World-space spell origins/impacts continue to use
existing world coordinates and depth clipping, not the foreground hand mesh.

This separation is also the foundation for item-use animation: the grip defines
how an item is held, the action defines what the hand does, and Casting Style
chooses a casting recipe. Future left/right instances should share topology and
action sampling while keeping independent pose buffers and correct mirrored
winding. Two-hand coordination, item attachment/contact points, and consume-event
adapters for food or potions are not implemented. Those adapters must connect
approved action events to gameplay, not apply healing from a drawing function.

Earlier style checkpoint (before the aim/sleeve adjustment): native Mac mini,
unchanged 360×240 canvas, seed 12345/Mouth,
Reach grip, prop pilot off, fresh page per run. Each version used 144 warmup
frames and 144 measured frames; no fullscreen, resizing or pixel readback.

| Style / light | Mesh build | Hand commands median / p95 | Whole frame median: original / rig |
| --- | --- | --- | --- |
| Arcane / daylight | 0.4 ms | 0.9 / 1.2 ms | 10.8 / 11.9 ms |
| Arcane / dusk | 0.4 ms | 0.9 / 1.0 ms | 11.0 / 11.8 ms |
| Finger Guns / daylight | 0.5 ms | 1.1 / 1.4 ms | 10.9 / 12.1 ms |
| Finger Guns / dusk | 0.5 ms | 1.1 / 1.3 ms | 11.0 / 12.1 ms |

Median animation-frame interval was 16.7 ms for both versions in these runs.
The first Arcane/dusk run overlapped background regression testing (whole-frame
medians 12.6 / 13.0 ms); the table uses a fresh repeat after tests finished.
These are one-hand render-command/cadence measurements, not GPU completion,
two-hand/effects costs or device-wide performance guarantees. Original hands
are idle and run first, so their cold whole-frame result is not a fair isolated
startup comparison. Native checks also exercised style/grip changes, return to
rest, synchronized controls and reload persistence; grey geometry remains an
unfinished study, with the finger-root and self-occlusion limitations above.

Aim/sleeve follow-up: one fresh-page Finger Guns/Reach daylight run at the same
360×240 size measured a 0.7 ms mesh build, 1.1 / 1.2 ms median / p95 hand commands,
and 10.5 / 11.7 ms original / rig whole-frame medians. Both versions retained a
16.7 ms median animation-frame interval. The four sleeve rings add 96 vertices,
192 triangles, and 9,216 numeric working bytes. This is one local measurement,
not evidence of a speedup or a new dusk/two-hand performance guarantee.
Native pose checks covered daylight and dusk without fullscreen or resizing.

Robe V1 checkpoint: one fresh-page Finger Guns/Reach daylight run at 360×240
measured a 0.7 ms mesh build and 1.3 / 1.4 ms median / p95 hand commands. Whole
frame render-command medians were 11.7 ms for the original baseline and 13.1 ms
for the finished rig; median animation-frame interval remained 16.7 ms. The four
cuff-shaping rings add 96 vertices, 192 triangles, and 9,216 numeric working
bytes over the preceding sleeve checkpoint; the Back-view closure adds one
vertex, 48 two-sided triangles, and 464 more bytes. This is one native local sample,
not a mobile, two-hand, GPU-completion, or thermal guarantee. Manual review
covered Player, Palm, and side views in daylight/dusk at the small preview size;
no fullscreen visual test or UI change was made.

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

`getCaveWallRoofLimit()` bounds each wall using ceiling and exterior-height
samples over its own footprint. Cave walls end inside the rock cover, at the
roof or at least 0.25 mesh units below the lowest exterior sample. A nearby
cap is only a cover tag, never permission to raise a wall to the highest grass
sample: that exposed the cave outline through sloping terrain. Walls without a
ceiling in their footprint retain their normal surface height. This is derived
once during window assembly; movement support and per-pixel occlusion are
unchanged, and the renderer still closes individual wall ends to the ceiling.
Seeded regressions check both above-ground occlusion and visible interior walls,
and deliberately restoring the highest-neighbor-cap limit must fail.

Known separate entrance detail: the broad, flat portal-rock top does not fully
join the curved terrain profile at its rear edge. Seed 12345 still shows a thin
dark lip seam there; reshaping that rock strip to the terrain profile remains
follow-up work. It is distinct from cave walls protruding through the ground.

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
generation and grain are world-coordinate-derived and stable across window rebuilding.

Shipping regressions include real seeded oblique/roof-edge renders, partial
entrance burial, an open doorway sightline, and a mutation check that fails if
terrain depth is omitted. Unit tests cover depth interpolation, clipping,
near-plane crossings, buffer reset/resize, and camera-independent material.
Actor, loot, and spell regressions exercise partial burial, zero height,
support strata, extended effects, and depth-bypass mutations.
The depth field preserves full canvas resolution; benchmark larger canvases
separately because cost scales with covered pixels.

Terrain facing is evaluated against each triangle's actual plane, not the
camera's absolute height relative to its corners. An uphill bank can face a
camera below every corner and must still write opaque depth while the player
walks out of a cave. Nonplanar cells test both triangles independently; ceiling
undersides use the opposite plane side. Flat caps remain invisible from below.
Do not replace this with a player-underground toggle or hide surface clutter
by camera height: visible fragments depend on the terrain's depth coverage.
The cave-exit regression drives keyboard/collision steps through both entrance
profiles and samples 48 views, with a mutation that restores the historical
whole-quad height gate and must fail.

Ground contact shading (AO) must compare a neighboring wall's vertical span
with the floor edge being shaded. A wall in the shared 2D collision grid is
not automatically beside every stacked floor at that XY location. Counting
buried cave walls against surface grass revealed the cave plan as dark bands
even after its geometry was fully hidden. Keep legitimate surface-wall and
interior contact shading; do not remove AO wholesale from caps or key it to
the player's underground state. Shading must not change geometry or depth.

The grass cap also reuses its saved pristine `surfaceBiome` color, without a
cave-only jitter pattern. Natural biome/elevation variation stays intact. Do
not paint a second material map over a buried cave: its cover should match the
same terrain at the same world coordinates.

Upper-terrain point lighting has a separate immutable, mesh-cell bake in
`07-decorations-lighting.js`. Short source-to-receiver rays test the actual
stitched floor/cap/ceiling triangles, including slopes, before adding a light.
This prevents cave torches revealing the cave through its cover while retaining
unobstructed outdoor lamps and entrance spill. Receivers use the rendered cell
center and the same diagonal/clamp as the floor pass. The per-frame multiplier
uses exterior ambient light, never the camera's cave blend. Rebuild with
`buildPointLights()` after static geometry or light changes; stale mesh/stitch
identity fails closed. The bake is shared read-only by profile/studio views,
whose animated multipliers remain isolated from gameplay.

This is **not** a unified shadow system: legacy interior/wall/entity lighting
still uses its existing XY grid, and actors/props are not baked occluders.
The new field is for the highest exposed terrain receiver, not arbitrary
stacked floors. Regression tests compare real seeded draws with buried lights
removed, preserve terrain coordinates and opaque depth, and reject restoration
of either through-roof lighting or cave-only cap colors.

Mini-preview checkpoint (2026-09-20, 360×240, native browser, no fullscreen or
quality reduction): Above cave rendered at 9.9 ms median / 10.6 ms p95;
Approach at 12.0 / 12.6 ms, both with 16.7 ms median animation-frame cadence.
Final Roof edge check after the stacked-layer guard: 10.6 / 11.5 ms with the
same 16.7 ms cadence. On that fresh page load, the first successful terrain-light
bake took 24.0 ms and the latest rebuild 16.4 ms, retaining 313,600 bytes. It
runs on world assembly, not every frame. A separate fresh headless JavaScriptCore
process measured 51.2 ms on its first bake versus 6.7–12.5 ms in warm runs;
those headless values are not native-browser timings. The browser profile
includes first/latest bake diagnostics separately from render timing. These
are single-host samples, not a statistically controlled speedup claim.

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
