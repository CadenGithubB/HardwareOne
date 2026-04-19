# Cave Entrance Visibility — Issue Tracking

## Goal
Player should be able to see INTO the cave from the surface before entering.

## Root Cause (Identified)

### Why the entrance was invisible from the surface
Three separate systems prevented seeing into the cave:

1. **Floor tiles covered the entrance**: Opaque floor quads at the entrance were drawn
   as a "pit/divot" — they covered the view even when at negative heights.
   
2. **Ceiling didn't render on surface**: `drawCeiling3D()` had `if (!playerUnderground) return;`
   which prevented the cave ceiling/overhang from being visible when approaching.

3. **Painter's algorithm occlusion**: Even deferred floor tiles just showed opaque colored
   surfaces, not the cave interior. The entrance quads were a "lid" over the cave.

## Current Fix (Applied)

### Three-part approach:

**A. Skip deep floor quads at entrance** (line ~10556)
- Quads with `origMinH < -1.0` AND `ceilH > 0.5` AND `nearEntr > 0.25`
- These are SKIPPED ENTIRELY — no floor drawn, leaving a gap
- The ceiling (drawn BEFORE the floor) shows through the gap
- Cave walls (drawn AFTER the floor) frame the sides
- Result: visible cave opening, not an opaque cover

**B. Enable ceiling at entrance from surface** (line ~10920)
- Removed blanket `if (!playerUnderground) return;` from `drawCeiling3D()`
- Replaced with conditional: render all ceiling when underground, render only
  entrance-nearby ceiling cells (within 180 world units) when on surface
- Per-cell `deepCaveEntrances` distance check prevents ceiling patches on
  surface above deep underground corridors

**C. Deferred rim quads** (unchanged from before)
- Transition quads (shallow depth or no ceiling) still defer to second pass
- Heavy darkening (88% at center) makes rim very dark
- Edge face polygons at pit boundary give depth cues

### Draw pipeline at entrance (surface view):
```
1. Sky → fills canvas
2. Ceiling → renders at entrance (now enabled from surface!)
3. Floor → SKIPS deep entrance quads (gap), defers rim quads
4. Floor second pass → draws dark rim quads around the opening  
5. Floor edge faces → cliff wall polygons at pit boundary
6. Walls → cave walls at entrance frame the sides
```

Player sees: dark cave ceiling visible through the gap, with walls framing the
sides and a dark rim around the opening. Looks like a cave entrance.

## Status Checklist

### CONFIRMED WORKING
- [x] Floor mesh has negative heights at entrance (44695 cells negative)
- [x] Floor mesh has ceiling data at entrance (822 cells with ceilH > 0)
- [x] `deepCaveEntrances` array populated during window assembly (1 entrance)
- [x] Entrance grid positions computed correctly in floor renderer
- [x] `_surfaceFloorClamp` prevents player sinking outside cave ceiling areas  
- [x] Player physics tracks correct floor height when entering cave
- [x] Cave renders correctly when playerUnderground = true
- [x] Deferred rendering code added (second pass for rim quads)
- [x] Entrance approach ramp creates depression in floor mesh heights
- [x] Deep entrance floor quads skipped (gap created)
- [x] Ceiling rendering enabled at entrance from surface
- [x] Darkening increased to 88% at center
- [x] Edge face polygons added at pit boundary

### FIXED
- [x] Floating ceiling patches above entrance — ceilH values (4.0-5.6) are absolute Z/25,
  projecting to Z=100-140 which is ABOVE the surface camera (Z=60). Fix: when rendering
  ceiling from surface (`_ceilEntranceOnly`), compute physical ceiling Z as
  `min(floorH + 1.8, -0.1) * 25` — places ceiling below surface, visible as cave overhang.
  Underground ceiling rendering unchanged.

### NEEDS VERIFICATION
- [ ] `[CAVE-ENTRANCE]` log shows `skipped > 0` when near entrance on surface
- [ ] Ceiling is actually visible through the floor gap (visual check) — should now
  appear BELOW horizon as dark overhang, not floating in sky
- [ ] Cave walls visible at entrance edges from surface
- [ ] No visual artifacts (ceiling patches, dark spots) on surface away from entrance
- [ ] Smooth transition when walking from surface into cave
- [ ] Edge face polygons look correct (cliff walls at pit boundary)
- [ ] Underground ceiling unchanged (still renders correctly overhead)

## Key Debug Output
- `[CAVE-ENTRANCE] skipped=N deferred=M clamped=K entrances=E underground=false`
  - skipped: floor quads NOT drawn (gap at entrance) — should be > 0
  - deferred: rim quads drawn in second pass — should be > 0  
  - clamped: far-from-entrance quads clamped to 0 — expected to be large
  - entrances: number of entrance positions — should be >= 1
  - underground: should be false when on surface

## Code Locations
| What | Line |
|------|------|
| Deep quad skip (gap) | ~10556 |
| Transition rim defer | ~10564 |
| Deferred collection | ~10668 |
| Second pass + edge faces | ~10727 |
| Always-on debug log | ~10826 |
| Ceiling guard (modified) | ~10920 |
| Ceiling Z fix (surface) | ~11046 |
| Ceiling entrance proximity | ~10960 |
| playerUnderground detection | ~16917 |
| _surfaceFloorClamp | ~8161 |
