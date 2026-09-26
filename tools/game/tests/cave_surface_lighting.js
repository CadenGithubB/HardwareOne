// Exercise the shipping cave generator, point-light bake and floor draw pass.
// Canvas is an API host only: assertions compare actual projected geometry,
// opaque depth and draw colors, not a second renderer or screenshot hash.
(function () {
  var G = (0, eval)('this'), checked = 0, errors = [];
  var output = (__engine === 'node' || __engine === 'deno') ? console.log.bind(console) : __out;
  var host = slurp('tools/game/tests/cave_occlusion_integration.js');
  var boundary = host.indexOf('  function check(name, okay) {');
  if (boundary < 0) throw Error('shipping browser host boundary missing');
  (0, eval)(host.slice(0, boundary) + '}());');
  G.console.error = function () { errors.push(Array.prototype.join.call(arguments, ' ')); };
  var realNow = Date.now;
  Date.now = function () { return 2000000000000; };
  function check(name, okay) {
    if (!okay) throw Error(name);
    checked++; output('PASS ' + name);
  }
  function sameNumbers(a, b) {
    if (!a || !b || a.length !== b.length) return false;
    for (var i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
  }
  function start(seed, kind) {
    G._caveTestSeedOverride = seed; G._caveTestKindOverride = kind;
    G.CAVE_TEST_MODE = G.ENDLESS_MODE = G.running = true;
    G.gameOverState = false; G.terrain = 'plains';
    Object.keys(G.caveTestFlags).forEach(function (key) { G.caveTestFlags[key] = false; });
    G.resetEndlessMode();
    G.MODE3D = true; G.overviewActive = false; G.settings.dayNight = true;
    G.dayTime = 0.5; G.daySpeed = 0;
  }
  function recordFloor() {
    var project = G.projectSceneWorldPolygon, fill = G.fillSceneDepthPolygon;
    var mesh = G.floorMesh, records = {}, geometry = [];
    G.beginSceneDepthFrame(G.canvas.width, G.canvas.height);
    G.projectSceneWorldPolygon = function (vertices, camera) {
      var polygon = project(vertices, camera);
      polygon.testVertices = vertices;
      return polygon;
    };
    G.fillSceneDepthPolygon = function (polygon) {
      var count = fill(polygon), vertices = polygon.testVertices;
      if (!vertices) return count;
      geometry.push(vertices.length, polygon.length);
      vertices.forEach(function (p) { geometry.push(p.x, p.y, p.z); });
      polygon.forEach(function (p) { geometry.push(p.x, p.y, p.depth); });
      if (!count) return count;
      var ci = Math.floor(vertices[0].y / mesh.gridSize) * mesh.w + Math.floor(vertices[0].x / mesh.gridSize);
      var role = 0;
      for (var li = 0; li < mesh.layerCount[ci]; li++) {
        var type = mesh['l' + li + 'Type'][ci];
        if (type !== 1 && type !== 3 && type !== 4) continue;
        if (Math.abs(mesh['l' + li + 'TopZ'][ci] * 25 - vertices[0].z) < 0.00001) {
          role = G.getFloorRenderLayerRole(mesh, ci, li, type); break;
        }
      }
      records[JSON.stringify(vertices)] = {color: G.ctx.fillStyle, pixels: count, role: role, cell: ci};
      return count;
    };
    try { G.drawLayersFloor3D(); }
    finally { G.projectSceneWorldPolygon = project; G.fillSceneDepthPolygon = fill; }
    return {records: records, geometry: new Float64Array(geometry), depth: new Float32Array(G._sceneDepthInv)};
  }
  function compare(a, b, role) {
    var count = 0, changed = 0, pixels = 0;
    Object.keys(a.records).forEach(function (key) {
      var before = a.records[key], after = b.records[key];
      if (before.role !== role || !after) return;
      count++;
      if (before.color !== after.color) { changed++; pixels += before.pixels; }
    });
    return {count: count, changed: changed, pixels: pixels};
  }
  // Raw ceiling layers independently classify sources; do not consult the
  // ray blocker being tested or infer underground from the player's state.
  function belowRoof(light, mesh) {
    var x = Math.floor(light.x / mesh.gridSize), y = Math.floor(light.y / mesh.gridSize);
    if (x < 0 || y < 0 || x >= mesh.w || y >= mesh.h) return false;
    var ci = y * mesh.w + x;
    for (var li = 0; li < mesh.layerCount[ci]; li++) {
      if (mesh['l' + li + 'Type'][ci] === 2 && mesh['l' + li + 'TopZ'][ci] * 25 > light.z + 0.1) return true;
    }
    return false;
  }
  function legacyBake(lights) {
    var bake = new Float32Array(G._lightGridW * G._lightGridH), size = G._lightCellSize;
    lights.forEach(function (light) {
      var cx = Math.floor(light.x / size), cy = Math.floor(light.y / size), radius = Math.ceil(light.radius / size);
      for (var y = Math.max(0, cy - radius); y <= Math.min(G._lightGridH - 1, cy + radius); y++) {
        for (var x = Math.max(0, cx - radius); x <= Math.min(G._lightGridW - 1, cx + radius); x++) {
          var dx = (x + 0.5) * size - light.x, dy = (y + 0.5) * size - light.y;
          var d2 = dx * dx + dy * dy, r2 = light.radius * light.radius;
          if (d2 < r2) bake[y * G._lightGridW + x] += light.intensity * (1 - d2 / r2);
        }
      }
    });
    return bake;
  }
  function rawMeshSnapshot(mesh) {
    var copies = {};
    Object.keys(mesh).forEach(function (key) {
      if (ArrayBuffer.isView(mesh[key])) copies[key] = {reference: mesh[key], values: new mesh[key].constructor(mesh[key])};
    });
    return function () {
      return Object.keys(copies).every(function (key) {
        return mesh[key] === copies[key].reference && sameNumbers(mesh[key], copies[key].values);
      });
    };
  }
  function upperCenter(mesh, ci) {
    // Derive the exact center of the NW-SE diagonal independently from the
    // receiver helper. Test fixtures below separately pin its triangulation.
    var height = -Infinity, opposite = -Infinity;
    [ci, ci + mesh.w + 1].forEach(function (index, corner) {
      for (var li = 0; li < mesh.layerCount[index]; li++) {
        var type = mesh['l' + li + 'Type'][index], value = mesh['l' + li + 'TopZ'][index];
        if (type !== 1 && type !== 3 && type !== 4) continue;
        if (corner === 0) height = Math.max(height, value);
        else opposite = Math.max(opposite, value);
      }
    });
    return (height + Math.max(height - 4.5, Math.min(height + 4.5, opposite))) * 12.5;
  }
  if (__argv.indexOf('--bypass-surface-light-roof') >= 0) G.surfaceLightRayBlocked = function () { return false; };
  if (__argv.indexOf('--share-upper-light-with-lower-layer') >= 0) {
    var realGetSurfaceLight = G.getSurfaceFloorLightAt;
    G.getSurfaceFloorLightAt = function (mesh, ci) { return realGetSurfaceLight(mesh, ci); };
  }
  [[12345, 'descending'], [5668, 'hillside']].forEach(function (fixture) {
    start(fixture[0], fixture[1]);
    G.previewCaveView('above'); G.cam.pitch = 0.5;
    var mesh = G.floorMesh, lights = G.pointLights, label = fixture[1] + ' ';
    var buried = lights.filter(function (light) { return belowRoof(light, mesh); });
    var exterior = lights.filter(function (light) { return !belowRoof(light, mesh); });
    if (__argv.indexOf('--inspect') >= 0) {
      output('LIGHT_INSPECT ' + JSON.stringify({kind: fixture[1], mesh: [mesh.w, mesh.h, mesh.gridSize],
        entrance: G.deepCaveEntrances[0], lights: lights.map(function (light) {
          return {x:light.x, y:light.y, z:light.z, buried:belowRoof(light,mesh)};
        })}));
      return;
    }
    check(label + 'fixture contains independently classified buried point lights', buried.length > 5);
    check(label + 'interior static light bake exactly retains historical arithmetic', sameNumbers(G._bakedLightGrid, legacyBake(lights)));
    check(label + 'surface bake is attached to the exact mesh', G._surfaceFloorLightMesh === mesh &&
      G._surfaceFloorLightBake instanceof Float32Array && G._surfaceFloorLightBake.length === mesh.w * mesh.h);
    var beforeBake = new Float32Array(G._bakedLightGrid), beforeDynamic = new Float32Array(G._lightGrid);
    var unchangedMesh = rawMeshSnapshot(mesh), originalStats = G.getSurfaceFloorLightStats();
    var allLights = recordFloor();
    G.pointLights = exterior;
    G.buildSurfaceFloorLighting();
    var removedBuried = recordFloor(), cap = compare(allLights, removedBuried, 3);
    output('SURFACE_LIGHT_TRACE ' + JSON.stringify({kind: fixture[1], buried: buried.length, cap: cap,
      stats: G.getSurfaceFloorLightStats()}));
    check(label + 'comparison includes visible upper grass', cap.count > 20);
    check(label + 'buried lights do not brighten upper grass', cap.changed === 0);
    check(label + 'light removal preserves every projected terrain coordinate', sameNumbers(allLights.geometry, removedBuried.geometry));
    check(label + 'light removal preserves every opaque depth sample', sameNumbers(allLights.depth, removedBuried.depth));
    check(label + 'upper-light rebuild leaves both legacy interior grids untouched',
      sameNumbers(beforeBake, G._bakedLightGrid) && sameNumbers(beforeDynamic, G._lightGrid));
    check(label + 'light rebuild does not mutate any mesh data or collision support', unchangedMesh());

    // A real above-ground lamp must still illuminate this exact roof. Select
    // a visible cap cell rather than depending on a hard-coded camera pixel.
    var capCells = Object.keys(removedBuried.records).map(function (key) { return removedBuried.records[key]; })
      .filter(function (record) { return record.role === 3; })
      .sort(function (a, b) { return b.pixels - a.pixels; });
    var outdoorCell = capCells[0].cell;
    var outdoor = {x: (outdoorCell % mesh.w + 0.5) * mesh.gridSize,
      y: (Math.floor(outdoorCell / mesh.w) + 0.5) * mesh.gridSize,
      z: upperCenter(mesh, outdoorCell) + 20, intensity: 0.55, radius: 90};
    G.pointLights = [outdoor]; G.buildSurfaceFloorLighting();
    var outdoorRender = recordFloor(), outdoorValue = G.getSurfaceFloorLightAt(mesh, outdoorCell);
    G.pointLights = []; G.buildSurfaceFloorLighting();
    var darkRender = recordFloor(), outdoorComparison = compare(outdoorRender, darkRender, 3);
    check(label + 'legitimate outdoor light above the cap is retained', outdoorValue > 0.1 && outdoorComparison.changed > 0);
    check(label + 'outdoor illumination also preserves geometry and depth',
      sameNumbers(outdoorRender.geometry, darkRender.geometry) && sameNumbers(outdoorRender.depth, darkRender.depth));

    // The generated entrance source is deliberately below natural ground, but
    // its unobstructed rays to the open approach must not be blanket-disabled.
    G.pointLights = lights;
    G.previewCaveView('mouth');
    G.cam.ang += Math.PI; G.cam.pitch = 0.5;
    mesh = G.floorMesh; lights = G.pointLights;
    var entranceLight = lights[lights.length - 1];
    G.pointLights = [entranceLight]; G.buildSurfaceFloorLighting();
    var mouthReceivers = 0;
    for (var ci = 0; ci < G._surfaceFloorLightBake.length; ci++) {
      if (!(G._surfaceFloorLightBake[ci] > 0)) continue;
      for (var li = 0; li < mesh.layerCount[ci]; li++) {
        var type = mesh['l' + li + 'Type'][ci];
        if (type === 1 && G.getFloorRenderLayerRole(mesh, ci, li, type) === 1) mouthReceivers++;
      }
    }
    check(label + 'real entrance source still spills onto the open approach', belowRoof(entranceLight, mesh) && mouthReceivers > 3);
    var mouthLit = recordFloor();
    G.pointLights = []; G.buildSurfaceFloorLighting();
    var mouthDark = recordFloor(), mouthComparison = compare(mouthLit, mouthDark, 1);
    // The descending fixture exposes the lit approach immediately underfoot;
    // hillside's supported mouth camera faces a bank. Its independent positive
    // receiver count above verifies spill without moving below the terrain.
    if (fixture[1] === 'descending') check(label + 'entrance spill reaches actual visible approach triangles', mouthComparison.changed > 0);

    G.pointLights = lights; G.buildSurfaceFloorLighting();
    G.previewCaveView('inside');
    mesh = G.floorMesh; lights = G.pointLights;
    var inside = recordFloor();
    G.pointLights = []; G.buildSurfaceFloorLighting();
    var insideWithoutUpperBake = recordFloor(), interior = compare(inside, insideWithoutUpperBake, 2);
    check(label + 'interior floor retains its legacy illumination', interior.count > 20 && interior.changed === 0);
    check(label + 'interior lighting comparison preserves geometry and depth',
      sameNumbers(inside.geometry, insideWithoutUpperBake.geometry) && sameNumbers(inside.depth, insideWithoutUpperBake.depth));

    // Named viewpoints can stream a new window. Select this window's raw cap
    // anew rather than accidentally testing a rejected/stale mesh cache.
    outdoorCell = -1;
    for (var si = 0; si < mesh.layerCount.length && outdoorCell < 0; si++) {
      for (var sl = 0; sl < mesh.layerCount[si]; sl++) {
        if (mesh['l' + sl + 'Type'][si] === 4) { outdoorCell = si; break; }
      }
    }
    outdoor = {x: (outdoorCell % mesh.w + 0.5) * mesh.gridSize,
      y: (Math.floor(outdoorCell / mesh.w) + 0.5) * mesh.gridSize,
      z: upperCenter(mesh, outdoorCell) + 20, intensity: 0.55, radius: 90};
    G.pointLights = [outdoor]; G.buildSurfaceFloorLighting();
    var oldAmbient = G.ambientLight, oldSurface = G.renderSurfaceAmbient, oldUnderground = G.playerUnderground;
    G.renderSurfaceAmbient = 0.9; G.ambientLight = 0.9; G.playerUnderground = false; G.updateLightGrid();
    var daylight = G.getSurfaceFloorLightAt(mesh, outdoorCell);
    G.ambientLight = 0.42; G.playerUnderground = true; G.updateLightGrid();
    check(label + 'upper light is independent of camera cave ambient and player light',
      G.getSurfaceFloorLightAt(mesh, outdoorCell) === daylight);
    G.renderSurfaceAmbient = 0.32; G.updateLightGrid();
    check(label + 'upper lights follow the exterior dusk ambient', daylight > 0 && G.getSurfaceFloorLightAt(mesh, outdoorCell) > daylight);
    G.ambientLight = oldAmbient; G.renderSurfaceAmbient = oldSurface; G.playerUnderground = oldUnderground;
    var oldBlocker = G.surfaceLightRayBlocked, raysDuringFrames = 0;
    G.surfaceLightRayBlocked = function () { raysDuringFrames++; return oldBlocker.apply(G, arguments); };
    var bakeReference = G._surfaceFloorLightBake, frameStats = G.getSurfaceFloorLightStats();
    for (var frame = 0; frame < 100; frame++) { G.updateLightGrid(); G.getSurfaceFloorLightAt(mesh, outdoorCell); }
    G.surfaceLightRayBlocked = oldBlocker;
    check(label + 'steady-state light sampling performs no new bake or terrain rays',
      raysDuringFrames === 0 && G._surfaceFloorLightBake === bakeReference && G.getSurfaceFloorLightStats().builds === frameStats.builds);
    var stamp = mesh.walkCandZ;
    mesh.walkCandZ = new Float32Array(0);
    var staleValue = G.getSurfaceFloorLightAt(mesh, outdoorCell);
    mesh.walkCandZ = stamp;
    check(label + 'stale geometry and another mesh cannot reuse cached light',
      staleValue === 0 && G.getSurfaceFloorLightAt({}, outdoorCell) === 0);
    check(label + 'cached upper light uses one bounded float per mesh sample',
      frameStats.bytes === mesh.w * mesh.h * 4 && frameStats.bytes === G._surfaceFloorLightBake.byteLength);
    frameStats.bytes = -1;
    check(label + 'diagnostic stats do not expose mutable live counters', G.getSurfaceFloorLightStats().bytes > 0);
    G.pointLights = lights; G.buildSurfaceFloorLighting();
    output('SURFACE_LIGHT_COST ' + JSON.stringify({kind: fixture[1], initial: originalStats,
      warm: G.getSurfaceFloorLightStats(), mouthReceivers: mouthReceivers, mouthTriangles: mouthComparison.changed,
      outdoorTriangles: outdoorComparison.changed}));
    check(label + 'no rendering errors are hidden', errors.length === 0);
  });
  if (__argv.indexOf('--inspect') < 0) {
    // Two uncovered walkable strata at the same XY: only the upper ledge may
    // consume the upper receiver bake; lower floors retain their legacy path.
    var mesh = {w:2,h:2,gridSize:12,layerCount:new Uint8Array([2,2,2,2]),
      ceilAboveMask:new Uint8Array(4),walkCandZ:new Float32Array(8),
      l0Type:new Uint8Array([1,1,1,1]),l0Meta:new Uint8Array([1,1,1,1]),l0TopZ:new Float32Array(4),
      l1Type:new Uint8Array([3,3,3,3]),l1Meta:new Uint8Array([4,4,4,4]),l1TopZ:new Float32Array([2,2,2,2])};
    G.floorMesh = mesh; G.pointLights = [{x:6,y:6,z:70,radius:20,intensity:0.5}];
    G._surfaceFloorLightScale = 1; G.buildSurfaceFloorLighting();
    check('stacked upper ledge retains its outdoor receiver light', G.getSurfaceFloorLightAt(mesh,0,1) === 0.5);
    check('stacked lower floor cannot borrow upper receiver light', Number.isNaN(G.getSurfaceFloorLightAt(mesh,0,0)));
    check('vertical ray from below the ledge is blocked', G.surfaceLightRayBlocked(6,6,20,6,6,50.05,mesh));
    check('vertical ray above the ledge reaches its surface', !G.surfaceLightRayBlocked(6,6,70,6,6,50.05,mesh));
    check('horizontal ray in clear space remains unblocked', !G.surfaceLightRayBlocked(1,6,70,11,6,70,mesh));
    check('invalid light ray fails closed', G.surfaceLightRayBlocked(NaN,6,70,6,6,50,mesh));
  }
  Date.now = realNow;
  if (__argv.indexOf('--inspect') < 0) output('CAVE_SURFACE_LIGHTING_RESULT PASS ' + checked);
}());
undefined;
