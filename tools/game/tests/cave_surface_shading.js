// Actual seeded floor drawing: buried walls cannot shade the upper stratum.
// This records production colors, projection and depth; it is not a screenshot
// hash, a replacement renderer, or a camera-height visibility workaround.
(function () {
  var G = (0, eval)('this'), checked = 0, errors = [];
  var output = (__engine === 'node' || __engine === 'deno') ? console.log.bind(console) : __out;
  var host = slurp('tools/game/tests/cave_occlusion_integration.js');
  var boundary = host.indexOf('  function check(name, okay) {');
  if (boundary < 0) throw Error('shipping browser host boundary missing');
  (0, eval)(host.slice(0, boundary) + '}());');
  G.console.error = function () { errors.push(Array.prototype.join.call(arguments, ' ')); };
  function check(name, okay) {
    if (!okay) throw Error(name);
    checked++; output('PASS ' + name);
  }
  function sameNumbers(a, b) {
    if (a.length !== b.length) return false;
    for (var i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
  }
  if (__argv.indexOf('--height-blind-ao') >= 0) {
    // Restore the historical 2D neighbor test, independently of all layer and
    // wall-height metadata. This must make the actual roof color test fail.
    G.floorWallOccludesAO = function (gx, gy) {
      return gx >= 0 && gy >= 0 && gx < G.gridW && gy < G.gridH && !!G.grid[gy * G.gridW + gx];
    };
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
  // Independent provenance comes from raw stamped ceiling layers, never the
  // helper/height-limit being tested. Alter ONLY the grid consulted by this
  // isolated floor pass; no wall geometry or collision simulation is changed.
  function masks() {
    var mesh = G.floorMesh, original = G.grid;
    var noBuried = new Uint8Array(original), noSurface = new Uint8Array(original);
    var buriedCount = 0, surfaceCount = 0;
    for (var gy = 0; gy < G.gridH; gy++) for (var gx = 0; gx < G.gridW; gx++) {
      var wi = gy * G.gridW + gx;
      if (!original[wi]) continue;
      var covered = false;
      var x0 = Math.max(0, Math.floor(gx * G.cell / mesh.gridSize));
      var y0 = Math.max(0, Math.floor(gy * G.cell / mesh.gridSize));
      var x1 = Math.min(mesh.w - 1, Math.ceil((gx + 1) * G.cell / mesh.gridSize));
      var y1 = Math.min(mesh.h - 1, Math.ceil((gy + 1) * G.cell / mesh.gridSize));
      for (var y = y0; y <= y1; y++) for (var x = x0; x <= x1; x++) {
        var i = y * mesh.w + x;
        for (var li = 0; li < mesh.layerCount[i]; li++) {
          if (mesh['l' + li + 'Type'][i] === 2) covered = true;
        }
      }
      if (covered) { noBuried[wi] = 0; buriedCount++; }
      else { noSurface[wi] = 0; surfaceCount++; }
    }
    return {original: original, noBuried: noBuried, noSurface: noSurface,
      buriedCount: buriedCount, surfaceCount: surfaceCount};
  }
  function recordFloor(grid) {
    var originalGrid = G.grid, project = G.projectSceneWorldPolygon, fill = G.fillSceneDepthPolygon;
    var mesh = G.floorMesh, records = {}, geometry = [];
    G.grid = grid;
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
      records[JSON.stringify(vertices)] = {color: G.ctx.fillStyle, pixels: count, role: role};
      return count;
    };
    try { G.drawLayersFloor3D(); }
    finally { G.projectSceneWorldPolygon = project; G.fillSceneDepthPolygon = fill; G.grid = originalGrid; }
    return {records: records, geometry: new Float64Array(geometry), depth: new Float32Array(G._sceneDepthInv)};
  }
  function compare(a, b, role) {
    var count = 0, changed = 0, pixels = 0, onlyBrightened = true;
    Object.keys(a.records).forEach(function (key) {
      var before = a.records[key], after = b.records[key];
      if (before.role !== role || !after) return;
      count++;
      if (before.color === after.color) return;
      changed++; pixels += before.pixels;
      var ac = before.color.match(/\d+/g).map(Number), bc = after.color.match(/\d+/g).map(Number);
      if (bc.some(function (v, i) { return v < ac[i]; })) onlyBrightened = false;
    });
    return {count: count, changed: changed, pixels: pixels, onlyBrightened: onlyBrightened};
  }
  [[12345, 'descending'], [5668, 'hillside']].forEach(function (fixture) {
    start(fixture[0], fixture[1]);
    G.previewCaveView('above'); G.cam.pitch = 0.5;
    var label = fixture[1] + ' ', selection = masks();
    check(label + 'fixture independently identifies buried and surface walls',
      selection.buriedCount > 100 && selection.surfaceCount > 100);
    var normal = recordFloor(selection.original), noBuried = recordFloor(selection.noBuried);
    var caps = compare(normal, noBuried, 3);
    output('SURFACE_SHADING_TRACE ' + JSON.stringify({kind: fixture[1], capComparison: caps}));
    check(label + 'comparison includes visible grass above the cave', caps.count > 20);
    check(label + 'buried walls do not darken upper grass', caps.changed === 0);
    check(label + 'AO-only comparison preserves every projected terrain coordinate', sameNumbers(normal.geometry, noBuried.geometry));
    check(label + 'AO-only comparison preserves every opaque depth sample', sameNumbers(normal.depth, noBuried.depth));
    var noSurface = recordFloor(selection.noSurface), surface = compare(normal, noSurface, 1);
    check(label + 'real aboveground walls retain their contact shading',
      surface.changed > 20 && surface.pixels > 100 && surface.onlyBrightened);
    check(label + 'retained surface shading also changes no geometry or depth',
      sameNumbers(normal.geometry, noSurface.geometry) && sameNumbers(normal.depth, noSurface.depth));
    G.cam.ang += 0.12;
    var angled = recordFloor(selection.original), cameraComparison = compare(normal, angled, 3);
    check(label + 'same ground keeps its shading from another camera angle',
      cameraComparison.count > 20 && cameraComparison.changed === 0);
    G.previewCaveView('inside');
    selection = masks();
    var inside = recordFloor(selection.original), insideNoWalls = recordFloor(selection.noBuried);
    var cave = compare(inside, insideNoWalls, 2);
    check(label + 'cave floor keeps shading beside its own underground walls',
      cave.changed > 20 && cave.pixels > 100 && cave.onlyBrightened);
    check(label + 'interior shading changes no projected geometry or depth',
      sameNumbers(inside.geometry, insideNoWalls.geometry) && sameNumbers(inside.depth, insideNoWalls.depth));
    check(label + 'test restores the live grid and emits no rendering errors', G.grid === selection.original && errors.length === 0);
  });
  output('CAVE_SURFACE_SHADING_RESULT PASS ' + checked);
}());
undefined;
