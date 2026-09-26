// Shared material contracts using production helpers, without browser or audit
// lab dependencies. Lighting may change; the albedo of a fixed rock may not.
(function () {
  var G = (0, eval)('this');
  (0, eval)(slurp('assets/games/src/01-materials.js'));
  (0, eval)(slurp('assets/games/src/05-endless-world.js'));
  (0, eval)(slurp('assets/games/src/17-input-lighting-update.js'));
  var n = 0;
  function check(name, okay) {
    if (!okay) throw new Error(name);
    n++; __out('PASS ' + name);
  }
  var portal = {x: 120, y: 18, angle: 0, halfWidth: 54,
    approachLength: 480, innerLength: 240};
  var portals = [portal], surface = '#8aa869';
  function color(x) { return G.sampleCaveMaterialColor(surface, x, 18, portals); }
  function blend(x) { return G.getCavePortalDepthBlendAt(x, 18, portals); }
  check('material is exactly neighboring terrain at mouth', color(120) === 0x8aa869);
  check('open approach retains neighboring terrain', color(72) === 0x8aa869);
  check('signed mouth depth begins at zero', blend(120) === 0);
  check('middle passage blends continuously', Math.abs(blend(192) - 0.5) < 1e-12);
  check('deep passage reaches stone', blend(264) === 1);
  check('beyond portal footprint stays deep stone', blend(600) === 1);
  check('no portal is a stable deep-interior material',
    G.getCavePortalDepthBlendAt(192, 18, []) === 1);
  check('mouth transition does not contain a color step', color(119.999) === color(120.001));
  var deep = color(264), dr = (deep >> 16) & 255, dg = (deep >> 8) & 255, db = deep & 255;
  check('deep stone is warm neutral instead of purple',
    dr >= dg && dg >= db && dr - db >= 8 && dr - db <= 24);
  check('deep stone remains bright enough for independent readable lighting', dr >= 102 && dg >= 95 && db >= 82);
  check('deep stone no longer inherits bright terrain hue',
    deep === G.sampleCaveMaterialColor('#ff0000', 264, 18, portals));
  check('both packed and hex terrain colors author identical rock',
    color(192) === G.sampleCaveMaterialColor(0x8aa869, 192, 18, portals));
  check('invalid terrain palette has safe neutral fallback',
    G.sampleCaveMaterialColor(undefined, 120, 18, portals) === 0x686156);
  check('material hex preserves leading zeroes', G.caveMaterialColorHex(0x00000f) === '#00000f');
  var stone = G.GAME_MATERIALS.caveStone;
  check('interior stone exposes restrained base damp iron and worn roles',
    stone.packed.base === 0x686156 && stone.packed.damp === 0x50595b &&
    stone.packed.iron === 0x73513d && stone.packed.worn === 0x83755f);
  var authoredA = G.sampleInteriorStoneAlbedo(stone.packed.base, 384, -96);
  var authoredB = G.sampleInteriorStoneAlbedo(stone.packed.base, 384, -96);
  check('multi-scale stone authoring is deterministic without random state', authoredA === authoredB);
  var authoredColors = {}, authoredBounded = true;
  for (var ay = -192; ay <= 192; ay += 12) for (var ax = -192; ax <= 192; ax += 12) {
    var authored = G.sampleInteriorStoneAlbedo(stone.packed.base, ax, ay);
    authoredColors[authored] = 1;
    var ar = (authored >>> 16) & 255, ag = (authored >>> 8) & 255, ab = authored & 255;
    if (ar < 70 || ar > 145 || ag < 70 || ag > 135 || ab < 65 || ab > 130) authoredBounded = false;
  }
  check('multi-scale mineral variation stays broad subtle and bounded',
    authoredBounded && Object.keys(authoredColors).length >= 12 && Object.keys(authoredColors).length <= 80);
  function brightness(packed) { return ((packed >>> 16) & 255) + ((packed >>> 8) & 255) + (packed & 255); }
  var floorShade = G.shadeCaveSurfaceColor(authoredA, G.CAVE_SURFACE_FLOOR, 0.42, 0, 1);
  var wallShade = G.shadeCaveSurfaceColor(authoredA, G.CAVE_SURFACE_WALL, 0.42, 0, 1);
  var ceilingShade = G.shadeCaveSurfaceColor(authoredA, G.CAVE_SURFACE_CEILING, 0.42, 0, 1);
  check('semantic shading keeps ceiling darker than wall and floor',
    brightness(ceilingShade) < brightness(wallShade) && brightness(wallShade) < brightness(floorShade));
  var torchWall = G.shadeCaveSurfaceColor(authoredA, G.CAVE_SURFACE_WALL, 0.42, 0.35, 1);
  check('local light warms stone while increasing its readable value',
    brightness(torchWall) > brightness(wallShade) &&
    (((torchWall >>> 16) & 255) - (torchWall & 255)) > (((wallShade >>> 16) & 255) - (wallShade & 255)));
  check('invalid semantic role safely uses wall presentation',
    G.shadeCaveSurfaceColor(authoredA, 99, 0.42, 0, 1) === wallShade);

  G.pos = {x: 0, y: 0, floorZ: 60}; G.cam = {x: 0, y: 0, z: 60};
  G.playerUnderground = false; G.dayTime = 0.5;
  G.deepCaveEntrances = portals;
  var fixed = color(192);
  G.pos = {x: 192, y: 18, floorZ: -140}; G.cam = {x: 192, y: 18, z: -140};
  G.playerUnderground = true; G.dayTime = 0;
  G.renderSurfaceAmbient = 0.32; G.renderCaveAmbient = 0.4;
  check('fixed rock albedo ignores camera player and daylight changes', color(192) === fixed);
  G.windowOriginX = 480; G.windowOriginY = -480;
  G.deepCaveEntrances = [{x: portal.x - 480, y: portal.y + 480,
    angle: portal.angle, halfWidth: portal.halfWidth,
    approachLength: portal.approachLength, innerLength: portal.innerLength}];
  check('chunk authoring ignores rebased render portals', color(192) === fixed);
  check('portal depth is translation invariant',
    G.getCavePortalDepthBlendAt(192 - 480, 18 + 480, G.deepCaveEntrances) === blend(192));

  G.floorMesh = {w: 3, h: 2, gridSize: 12,
    caveStone: new Uint32Array([0x010203, fixed, 0x111213, 0x212223, 0x313233, deep])};
  // These deliberately fail if the render hot path starts recomputing world
  // material, portal geometry, biome or noise instead of reading its mesh.
  G.sampleCaveMaterialColor = G.sampleCavePortal = G.getBiomeAt = G.getFloorColorBlended =
    G.terrainNoise = function () { throw new Error('material lookup recomputed world data'); };
  check('render material comes directly from authored mesh cell', G.getCaveMaterialColorAt(18, 6) === fixed);
  G.cam = {x: 500, y: 500, z: 800}; G.playerUnderground = false;
  check('cached face color remains invariant when camera moves above cave', G.getCaveMaterialColorAt(18, 6) === fixed);
  // Simulate the same authored world vertex moving to a different local index
  // when the assembled window shifts. Colors are copied, not re-randomized.
  G.floorMesh = {w: 2, h: 1, gridSize: 12, caveStone: new Uint32Array([fixed, deep])};
  check('same authored world rock survives window rebase', G.getCaveMaterialColorAt(6, 6) === fixed);
  check('mesh replacement cannot return old cell cache', G.getCaveMaterialColorAt(18, 6) === deep);
  check('out of bounds lookup has stable legacy fallback', G.getCaveMaterialColorAt(-1, 6) === 0x686156);
  check('nonfinite lookup has stable fallback', G.getCaveMaterialColorAt(NaN, 6) === 0x686156);
  G.floorMesh = {w: 1, h: 1, gridSize: 12};
  check('legacy fixed-level mesh needs no cave material field', G.getCaveMaterialColorAt(6, 6) === 0x686156);
  __out('CAVE_MATERIAL_RESULT PASS ' + n);
}());
undefined;
