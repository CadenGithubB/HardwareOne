// Seeded end-to-end wall/roof regression. Only browser APIs are substituted:
// production generation, wall projection, terrain and per-pixel depth all run.
(function () {
  var G = (0, eval)('this'), errors = [], checked = 0;
  var output = (__engine === 'node' || __engine === 'deno') ? console.log.bind(console) : __out;
  function noop() {}
  function stub(values) {
    return new Proxy(values || {}, {get: function (obj, key) {
      if (key === 'then') return undefined;
      return key in obj ? obj[key] : noop;
    }});
  }
  function gradient() { return {addColorStop: noop}; }
  var context = stub({measureText: function (s) { return {width: String(s).length * 6}; },
    createLinearGradient: gradient, createRadialGradient: gradient,
    createPattern: function () { return {}; },
    createImageData: function (w, h) { return {data: new Uint8ClampedArray(w * h * 4)}; },
    getImageData: function (x, y, w, h) { return {data: new Uint8ClampedArray(w * h * 4)}; }});
  function makeCanvas() {
    return stub({width: 360, height: 240, style: {}, getContext: function () { return context; },
      getBoundingClientRect: function () { return {left: 0, top: 0, width: this.width, height: this.height}; }});
  }
  var elements = {maze: makeCanvas()};
  function element(id) {
    if (!elements[id]) elements[id] = stub({id: id, checked: id === 'ctFixedNoon', value: '',
      textContent: '', style: {}, classList: stub(), dataset: {}});
    return elements[id];
  }
  G.window = G;
  G.document = stub({getElementById: element,
    createElement: function (tag) { return tag === 'canvas' ? makeCanvas() : stub({style: {}}); },
    querySelectorAll: function () { return []; }, querySelector: function () { return null; },
    body: stub({style: {}}), documentElement: stub({style: {}})});
  G.console = {log: noop, info: noop, warn: noop, error: function () {
    errors.push(Array.prototype.join.call(arguments, ' '));
  }};
  G.setTimeout = G.setInterval = G.requestAnimationFrame = function () { return 1; };
  G.clearTimeout = G.clearInterval = G.cancelAnimationFrame = noop;
  G.addEventListener = G.removeEventListener = noop;
  G.innerWidth = 1280; G.innerHeight = 720; G.devicePixelRatio = 1;
  G.hw = {fetchJSON: function () { return new Promise(noop); },
    postFormText: function () { return new Promise(noop); }, _auth401: noop};
  var manifest = JSON.parse(slurp('assets/games/manifest.json'));
  manifest.parts.MAIN.forEach(function (path) { (0, eval)(slurp('assets/games/' + path)); });
  function check(name, okay) {
    if (!okay) throw new Error(name);
    checked++; output('PASS ' + name);
  }
  function start(seed, kind) {
    G._caveTestSeedOverride = seed; G._caveTestKindOverride = kind;
    G.CAVE_TEST_MODE = true; G.ENDLESS_MODE = true; G.running = true;
    G.gameOverState = false; G.terrain = 'plains';
    Object.keys(G.caveTestFlags).forEach(function (key) { G.caveTestFlags[key] = false; });
    G.resetEndlessMode();
    G.MODE3D = true; G.overviewActive = false; G.settings.dayNight = true;
    G.dayTime = 0.5; G.daySpeed = 0;
  }
  function selectView(view) {
    G.previewCaveView(view);
    // A downward look puts the buried chamber walls on-screen. Merely counting
    // zero pixels when they are below the viewport would not test occlusion.
    if (view === 'above') G.cam.pitch = 0.5;
    var roofWalls = new Uint8Array(G.grid.length);
    for (var i = 0; i < roofWalls.length; i++) {
      roofWalls[i] = G.grid[i] && Number.isFinite(G.wallMaxTopZ[i]) ? 1 : 0;
    }
    if (__argv.indexOf('--restore-max-cap') >= 0) {
      // Historical bug: a flat wall top used the HIGHEST neighboring cap.
      // Retain the original true-roof provenance; an adjacent surface tree is
      // not a buried wall merely because the old broad cap tag included it.
      for (var j = 0; j < roofWalls.length; j++) {
        if (Number.isFinite(G.wallCapZ[j])) G.wallMaxTopZ[j] = G.wallCapZ[j];
      }
    }
    return roofWalls;
  }
  function renderWalls(roofWalls, terrain) {
    G.beginSceneDepthFrame(G.canvas.width, G.canvas.height);
    if (terrain) { G.drawLayersCeiling3D(); G.drawLayersFloor3D(); }
    var project = G.projectSceneWorldPolygon, fill = G.fillSceneDepthPolygon;
    var result = {faces: 0, visibleFaces: 0, pixels: 0, closurePixels: 0};
    G.projectSceneWorldPolygon = function (vertices, camera) {
      var polygon = project(vertices, camera);
      polygon.testWorldVertices = vertices;
      return polygon;
    };
    G.fillSceneDepthPolygon = function (polygon) {
      var pixels = fill(polygon), vertices = polygon.testWorldVertices;
      if (!vertices || vertices.length !== 4) return pixels;
      var x = 0, y = 0;
      vertices.forEach(function (p) { x += p.x / 4; y += p.y / 4; });
      // Faces lie exactly on the gameplay grid edge. Check both sides rather
      // than assigning a face to whichever cell Math.floor happens to choose.
      var closure = false;
      var buried = [[-0.1,0],[0.1,0],[0,-0.1],[0,0.1]].some(function (offset) {
        var gx = Math.floor((x + offset[0]) / G.cell), gy = Math.floor((y + offset[1]) / G.cell);
        if (gx < 0 || gy < 0 || gx >= G.gridW || gy >= G.gridH || !roofWalls[gy * G.gridW + gx]) return false;
        // A mixed mouth/bank cell can need an upper closure to its real ceiling.
        // This is a second actual world polygon whose bottom is the wall limit,
        // not the wall body's floor. Preserve its visible contribution inside.
        closure = Math.abs(vertices[0].z - G.wallMaxTopZ[gy * G.gridW + gx] * 25) < 0.0001 &&
          Math.max(vertices[2].z, vertices[3].z) > vertices[0].z + 0.001;
        return true;
      });
      if (buried) {
        result.faces++; result.visibleFaces += pixels > 0 ? 1 : 0; result.pixels += pixels;
        if (closure) result.closurePixels += pixels;
      }
      return pixels;
    };
    try { G.drawWalls3D(); }
    finally { G.projectSceneWorldPolygon = project; G.fillSceneDepthPolygon = fill; }
    return result;
  }
  [[12345, 'descending'], [5668, 'hillside']].forEach(function (fixture) {
    start(fixture[0], fixture[1]);
    var label = fixture[1] + ' ';
    ['above', 'roofedge'].forEach(function (view) {
      var roofWalls = selectView(view), covered = renderWalls(roofWalls, true);
      var uncovered = renderWalls(roofWalls, false);
      output('WALL_COVER_TRACE ' + JSON.stringify({kind: fixture[1], view: view,
        covered: covered, withoutTerrain: uncovered}));
      check(label + view + ' submits actual buried wall faces', covered.faces > 20);
      check(label + view + ' walls are on-screen without terrain rather than camera-hidden', uncovered.pixels > 100);
      check(label + view + ' roof fully occludes buried cave walls', covered.pixels === 0);
    });
    ['inside', 'chamber'].forEach(function (view) {
      var inside = renderWalls(selectView(view), true);
      check(label + view + ' retains visible cave walls beneath the same roof',
        inside.faces > 20 && inside.visibleFaces > 10 && inside.pixels > 1000);
      if (fixture[1] === 'hillside' && view === 'chamber') {
        check('hillside boundary wall still visibly closes up to the real ceiling', inside.closurePixels > 100);
      }
    });
    // Drawing inside must not leave camera-dependent material or cached
    // visibility state that causes the next surface render to leak again.
    check(label + 'returning above after inside remains occluded', renderWalls(selectView('above'), true).pixels === 0);
    check(label + 'production render emits no swallowed errors', errors.length === 0);
  });
  output('CAVE_WALL_COVER_RESULT PASS ' + checked);
}());
undefined;
