// Plane-facing and real floor/ceiling submission regressions. No optional lab.
(function () {
  var G = (0, eval)('this'), checked = 0;
  function noop() {}
  G.window = G;
  G.ctx = new Proxy({}, {get: function (obj, key) { return key in obj ? obj[key] : noop; }});
  G.projScale = 20;
  G.cam = {x: -20, y: 10, z: 0, pitch: 0};
  G.ambientLight = G.renderSurfaceAmbient = 1;
  G._lightGrid = G.grid = null;
  G.DEBUG_LAYER_TYPES = G.DEBUG_POLY_TYPES = G.DEBUG_CEIL_WIRE = false;
  G.viewDist = 300;
  G.__caveStats = {};
  G._drawFloorCellBuf = new Float64Array(30);
  G._cacheStats = {matchZ: {hits: 0, misses: 0}};
  G.getCaveMaterialColorAt = function () { return 0x778855; };
  G.rgbQ = function (r, g, b) {
    return 'rgb(' + (Math.max(0, Math.min(255, r)) | 0) + ',' +
      (Math.max(0, Math.min(255, g)) | 0) + ',' +
      (Math.max(0, Math.min(255, b)) | 0) + ')';
  };
  (0, eval)(slurp('assets/games/src/01-materials.js'));
  (0, eval)(slurp('assets/games/src/12-scene-depth.js'));
  var wallSource = slurp('assets/games/src/12-render-core-walls.js');
  if (__argv.indexOf('--omit-wall-ramp-key') >= 0) {
    wallSource = wallSource.replace('(caveStyled ? 268435456 : 0)', '0');
  }
  (0, eval)(wallSource);
  var source = slurp('assets/games/src/13-render-floors-ceilings.js');
  if (__argv.indexOf('--legacy-floor-height-cull') >= 0) {
    source = source.replace('var floorVertices = [',
      'if (cameraZ < Math.min(_z0, _z1, _z2, _z3) * 25 - 1) continue;\nvar floorVertices = [');
  }
  (0, eval)(source);
  function check(name, okay) {
    if (!okay) throw new Error(name);
    checked++; __out('PASS ' + name);
  }
  var a = {x: 0, y: 0, z: 25}, b = {x: 20, y: 0, z: 75}, c = {x: 20, y: 20, z: 75};
  check('uphill top faces an eye below every vertex', G.getTerrainTriangleEyeSide(a, b, c, -20, 10, 0) > 0);
  check('downhill backside rejects eye above every vertex', G.getTerrainTriangleEyeSide(a, b, c, 60, 10, 100) < 0);
  check('facing does not depend on winding order', G.getTerrainTriangleEyeSide(a, c, b, -20, 10, 0) ===
    G.getTerrainTriangleEyeSide(a, b, c, -20, 10, 0));
  check('degenerate triangle has no fabricated facing', Number.isNaN(G.getTerrainTriangleEyeSide(a, a, c, 0, 0, 0)));

  var fills, projected, painted, realFill = G.fillSceneDepthPolygon, realProject = G.projectSceneWorldPolygon;
  G.fillSceneDepthPolygon = function (poly) {
    if (poly.length >= 3) { fills.push(poly); painted.push(G.ctx.fillStyle); }
    return realFill(poly);
  };
  G.projectSceneWorldPolygon = function (vertices, C) {
    projected.push(vertices);
    return realProject(vertices, C);
  };
  function render(heights, type, eye, angle, underground) {
    var m = {w: 2, h: 2, gridSize: 20, layerCount: [1, 1, 1, 1],
      ceilAboveMask: [0, 0, 0, 0], surfaceH: heights.slice(), walkCandZ: []};
    for (var k = 0; k < 5; k++) {
      m['l' + k + 'TopZ'] = heights.slice();
      m['l' + k + 'Type'] = [type, type, type, type];
      m['l' + k + 'Meta'] = type === 4 ? [3, 3, 3, 3] : type === 2 ? [0, 0, 0, 0] : [1, 1, 1, 1];
      m['l' + k + 'Color'] = ['#778855', '#778855', '#778855', '#778855'];
    }
    G.floorMesh = m;
    G.cam = {x: eye[0], y: eye[1], z: eye[2], pitch: 0};
    G.playerUnderground = underground;
    G.getCam3D = function () { return {w: 160, h: 120, cosAng: Math.cos(angle), sinAng: Math.sin(angle),
      invTanHalf: 1, horizonY: 60, cameraZ: eye[2]}; };
    fills = []; projected = []; painted = [];
    G.beginSceneDepthFrame(160, 120);
    if (type === 2) G.drawLayersCeiling3D(); else G.drawLayersFloor3D();
    var finiteDepth = 0;
    for (var i = 0; i < G._sceneDepthInv.length; i++) if (G._sceneDepthInv[i] > 0) finiteDepth++;
    return {fills: fills.length, projected: projected.length, finiteDepth: finiteDepth,
      polygons: fills.slice(), vertices: projected.slice(), colors: painted.slice()};
  }
  var r = render([1, 3, 1, 3], 1, [-20, 10, 0], 0, false);
  check('real renderer retains both uphill triangles below all corners', r.projected === 2 && r.fills === 2);
  check('retained uphill bank writes opaque scene depth', r.finiteDepth > 0);
  r = render([1, 1, 1, 5], 1, [-20, 10, 0], 0, false);
  check('nonplanar cell independently rejects only its backward triangle', r.projected === 1 && r.fills === 1);
  r = render([1, 1, 1, 1], 4, [-20, 10, 0], 0, true);
  check('flat cap never paints its grass underside inside a cave', r.projected === 0 && r.finiteDepth === 0);
  r = render([1, 1, 1, 1], 4, [-20, 10, 60], 0, false);
  check('same cap remains visible from above', r.projected === 2 && r.finiteDepth > 0);
  r = render([-2, -2, -2, -2], 1, [-20, 10, 0], 0, true);
  check('interior cave floor remains visible below the eye', r.projected === 2 && r.finiteDepth > 0);
  var outside = render([1, 3, 1, 3], 1, [-20, 10, 0], 0, false);
  var inside = render([1, 3, 1, 3], 1, [-20, 10, 0], 0, true);
  check('terrain visibility never switches with player underground flag', JSON.stringify(outside) === JSON.stringify(inside));
  r = render([1, 3, 1, 3], 1, [10, 10, 60], 0, false);
  check('front-facing terrain crossing near plane keeps clipped coverage', r.fills === 2 && r.finiteDepth > 0 &&
    r.polygons.some(function (p) { return p.some(function (v) { return v.depth === 1; }); }));
  r = render([1, 1, 1, 1], 2, [-20, 10, 0], 0, true);
  check('flat ceiling retains visible underside', r.projected === 2 && r.finiteDepth > 0);
  r = render([1, 1, 1, 1], 2, [-20, 10, 60], 0, false);
  check('flat ceiling top never replaces its cap', r.projected === 0);
  r = render([1, 1.5, 1, 1.5], 2, [60, 10, 55], Math.PI, false);
  check('sloped ceiling underside remains visible above all vertex heights', r.projected === 2 && r.finiteDepth > 0);

  // Pin the actual debug-off renderer output for one deterministically authored
  // covered cell. Floor and ceiling consume the same stone, ambient and local
  // light; only their production semantic roles and ceiling fog differ.
  function coveredCellPass(type) {
    var material = G.sampleInteriorStoneAlbedo(G.GAME_MATERIALS.caveStone.packed.base, 384, -96);
    var color = '#' + ('000000' + material.toString(16)).slice(-6);
    var m = {w: 2, h: 2, gridSize: 20, layerCount: [2, 2, 2, 2],
      ceilAboveMask: [1, 1, 1, 1], surfaceH: [-2, -2, -2, -2], walkCandZ: [],
      caveStone: new Uint32Array([material, material, material, material])};
    for (var k = 0; k < 5; k++) {
      m['l' + k + 'TopZ'] = k === 1 ? [1, 1, 1, 1] : [-2, -2, -2, -2];
      m['l' + k + 'Type'] = k === 0 ? [1, 1, 1, 1] : k === 1 ? [2, 2, 2, 2] : [0, 0, 0, 0];
      m['l' + k + 'Meta'] = k === 0 ? [2, 2, 2, 2] : [0, 0, 0, 0];
      m['l' + k + 'Color'] = [color, color, color, color];
    }
    G.floorMesh = m; G.grid = null;
    G.cam = {x: -20, y: 10, z: 0, pitch: 0};
    G.getCam3D = function () { return {w: 160, h: 120, cosAng: 1, sinAng: 0,
      invTanHalf: 1, horizonY: 60, cameraZ: 0}; };
    G.getCaveMaterialColorAt = function () { return material; };
    G.getCaveRenderLightAt = function () { return 0.42; };
    G._lightCellSize = 20; G._lightGridW = G._lightGridH = 2;
    G._lightGrid = new Float32Array([0.25, 0.25, 0.25, 0.25]);
    fills = []; projected = []; painted = [];
    G.beginSceneDepthFrame(160, 120);
    if (type === 2) G.drawLayersCeiling3D(); else G.drawLayersFloor3D();
    return {material: material, fills: fills.length, colors: painted.slice()};
  }
  G.DEBUG_LAYER_TYPES = G.DEBUG_POLY_TYPES = G.DEBUG_CAVE_COLORS = false;
  var coveredFloor = coveredCellPass(1), coveredCeiling = coveredCellPass(2);
  __out('INTERIOR_COLOR_TRACE ' + JSON.stringify({material: coveredFloor.material,
    floor: coveredFloor.colors, ceiling: coveredCeiling.colors}));
  check('covered-cell regression runs with every material debug palette disabled',
    !G.DEBUG_LAYER_TYPES && !G.DEBUG_POLY_TYPES && !G.DEBUG_CAVE_COLORS);
  check('same authored interior cell reaches both production floor and ceiling draws',
    coveredFloor.material === coveredCeiling.material && coveredFloor.fills === 2 && coveredCeiling.fills === 2);
  check('covered floor emits its pinned warm readable production color',
    JSON.stringify(coveredFloor.colors) === '["rgb(81,70,56)","rgb(81,70,56)"]');
  check('covered ceiling emits its pinned restrained production color',
    JSON.stringify(coveredCeiling.colors) === '["rgb(43,40,35)","rgb(43,40,35)"]');

  // Force an interior and exterior face to the same base RGB and projected
  // y buckets. Their only cache-key distinction is the material-ramp bit: the
  // cave face must keep four stops while the exterior face keeps three.
  function wallRampCollisionPass() {
    var gradients = [], wallPaints = [], originalShade = G.shadeCaveSurfaceColor;
    G.ctx.createLinearGradient = function (x0, y0, x1, y1) {
      var gradient = {coords: [x0, y0, x1, y1], stops: []};
      gradient.addColorStop = function (offset, color) { gradient.stops.push([offset, color]); };
      gradients.push(gradient); return gradient;
    };
    G.canvas = {width: 160, height: 120}; G.CANVAS_BASE_H = 240;
    G.gridW = 3; G.gridH = 1; G.cell = 20;
    G.grid = new Uint8Array([1, 0, 1]); G.gridCave = new Uint8Array([1, 0, 0]);
    G.wallHeights = new Float32Array([1, 0, 1]);
    G.wallFaceBase = G.wallMaxTopZ = G.wallCapZ = null;
    G.wallColorR = new Uint8Array([180, 0, 180]);
    G.wallColorG = new Uint8Array([140, 0, 140]);
    G.wallColorB = new Uint8Array([100, 0, 100]);
    G.floorMesh = null; G.deepCaveRegions = []; G.ENDLESS_MODE = true;
    G.windowOriginX = G.windowOriginY = 0; G.terrain = 'ground';
    G.GAME_CONFIG = {terrain: 'ground'}; G.viewDist = 300;
    G.cam = {x: 30, y: -100, z: 0, pitch: 0};
    G.getCam3D = function () { return {w: 160, h: 120, cosAng: 0, sinAng: 1,
      invTanHalf: 1, horizonY: 60, cameraZ: 0}; };
    G.getFloorHeightAt = function () { return 0; };
    G.getBiomeAt = function () { return 'ground'; };
    G.getWallBiomeRGB = function () { return 0xb48c64; };
    G.isInDeepCave = function () { return false; };
    G.getCaveMaterialColorAt = function () { return 0xb48c64; };
    G.getCaveRenderLightAt = function () { return 1; };
    G.shadeCaveSurfaceColor = function () { return 0xb48c64; };
    G.renderSurfaceFogFloor = G.renderCaveFogFloor = G.fogFloor = 1;
    G.renderSurfaceWallShadeW = G.renderSurfaceWallShadeE = 1;
    G.renderSurfaceWallShadeN = G.renderSurfaceWallShadeS = G.renderSurfaceTopShade = 1;
    G._lightGrid = null; G.depthBuffer = null; G._wallGradCache = new Map();
    G._cacheStats.wallGrad = {hits: 0, misses: 0, size: 0};
    G.projectSceneWorldPolygon = function () {
      return [{x: 20, y: 16, depth: 20}, {x: 60, y: 16, depth: 20},
        {x: 60, y: 80, depth: 20}, {x: 20, y: 80, depth: 20}];
    };
    G.fillSceneDepthPolygon = function () { wallPaints.push(G.ctx.fillStyle); return 1; };
    try { G.drawWalls3D(); } finally { G.shadeCaveSurfaceColor = originalShade; }
    return {gradients: gradients, paints: wallPaints, stats: G._cacheStats.wallGrad};
  }
  var collision = wallRampCollisionPass();
  var caveRamp = collision.gradients.filter(function (g) { return g.stops.length === 4; });
  var exteriorRamp = collision.gradients.filter(function (g) {
    return JSON.stringify(g.stops) === '[[0,"rgb(201,156,112)"],[0.5,"rgb(180,140,100)"],[1,"rgb(158,123,88)"]]';
  });
  check('production cave wall emits the pinned soot upper middle and contact stops',
    caveRamp.length === 1 && JSON.stringify(caveRamp[0].stops) ===
      '[[0,"rgb(136,106,76)"],[0.22,"rgb(169,131,94)"],[0.62,"rgb(185,144,103)"],[1,"rgb(144,112,80)"]]');
  check('same-color same-bucket exterior wall retains its three-stop sunlight ramp',
    exteriorRamp.length === 1 && JSON.stringify(caveRamp[0].coords) === JSON.stringify(exteriorRamp[0].coords) &&
    collision.paints.indexOf(caveRamp[0]) >= 0 && collision.paints.indexOf(exteriorRamp[0]) >= 0);
  check('wall gradient cache stores separate interior and exterior entries on RGB collision',
    collision.gradients.length === 4 && collision.stats.misses === 4 && collision.stats.size === 4);
  __out('TERRAIN_FACING_RESULT PASS ' + checked);
}());
undefined;
