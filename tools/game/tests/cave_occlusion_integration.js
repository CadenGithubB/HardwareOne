// Production-world rendering regression. This deliberately loads the source
// manifest and real generator/draw passes; the host supplies browser APIs only.
// No optional audit lab or generated-header snapshot is required.
(function () {
  var G = (0, eval)('this'), errors = [], checked = 0;
  // The portable bootstrap's Node/Deno writer resolves console.log lazily;
  // preserve a real stdout function before silencing production debug logs.
  var output = (__engine === 'node' || __engine === 'deno') ? console.log.bind(console) : __out;
  function noop() {}
  function stub(values) {
    return new Proxy(values || {}, {get: function (obj, key) {
      if (key === 'then') return undefined;
      return key in obj ? obj[key] : noop;
    }});
  }
  function gradient() { return {addColorStop: noop}; }
  var context = stub({
    measureText: function (s) { return {width: String(s).length * 6}; },
    createLinearGradient: gradient, createRadialGradient: gradient,
    createPattern: function () { return {}; },
    createImageData: function (w, h) { return {data: new Uint8ClampedArray(w * h * 4)}; },
    getImageData: function (x, y, w, h) { return {data: new Uint8ClampedArray(w * h * 4)}; }
  });
  function canvas() {
    return stub({width: 360, height: 240, style: {}, getContext: function () { return context; },
      getBoundingClientRect: function () { return {left: 0, top: 0, width: this.width, height: this.height}; }});
  }
  var elements = {maze: canvas()};
  function element(id) {
    if (!elements[id]) elements[id] = stub({id: id, checked: id === 'ctFixedNoon', value: '', textContent: '',
      style: {}, classList: stub(), dataset: {}});
    return elements[id];
  }
  G.window = G;
  G.document = stub({getElementById: element,
    createElement: function (tag) { return tag === 'canvas' ? canvas() : stub({style: {}}); },
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
    G.MODE3D = true; G.overviewActive = false; G.USE_KEYBOARD = true; G.USE_MOUSE = true;
    G.USE_GAMEPAD = false; G.settings.dayNight = true;
    G.dayTime = 0.5; G.daySpeed = 0;
  }
  function terrainPasses() {
    G.beginSceneDepthFrame(G.canvas.width, G.canvas.height);
    var originalFill = G.fillSceneDepthPolygon;
    // Mutation mode demonstrates that the regression detects the historical
    // missing terrain contribution, rather than passing on wall depth alone.
    if (__argv.indexOf('--omit-terrain-depth') >= 0) G.fillSceneDepthPolygon = function (points) {
      G.traceSceneDepthPolygon(points); G.ctx.fill();
    };
    try { G.drawLayersCeiling3D(); G.drawLayersFloor3D(); }
    finally { G.fillSceneDepthPolygon = originalFill; }
    G.drawWalls3D();
  }
  // A tiny camera-facing target patch avoids fragile integer rounding at a
  // single pixel. The real projection and depth clip decide its visibility.
  function visiblePatch(x, y, z) {
    var C = G.getCam3D(), dx = -C.sinAng * 2, dy = C.cosAng * 2;
    var points = G.projectSceneWorldPolygon([
      {x: x - dx, y: y - dy, z: z - 2}, {x: x + dx, y: y + dy, z: z - 2},
      {x: x + dx, y: y + dy, z: z + 2}, {x: x - dx, y: y - dy, z: z + 2}
    ], C);
    return G.withSceneDepthClip(points, noop, {writeDepth: false, depthBias: 0.15});
  }
  [[12345, 'descending'], [5668, 'hillside']].forEach(function (fixture) {
    start(fixture[0], fixture[1]);
    var label = fixture[1] + ' ';
    ['sideleft', 'sideright', 'oblique', 'roofedge'].forEach(function (view) {
      var before = errors.length;
      G.previewCaveView(view);
      check(label + view + ' uses finite supported camera', Number.isFinite(G.cam.x) &&
        Number.isFinite(G.cam.y) && Number.isFinite(G.cam.z) && Number.isFinite(G.cam.ang));
      if (errors.length !== before) output('DRAW_ERRORS ' + errors.slice(before).join('\n'));
      check(label + view + ' real draw has no swallowed errors', errors.length === before);
    });
  });
  start(12345, 'descending');
  G.previewCaveView('sideleft');
  terrainPasses();
  var e = G.deepCaveEntrances[0], pillarWidth = 8 + e.style * 6;
  var offset = (Math.max(60, e.halfWidth * 2) + pillarWidth) / 2;
  // Sample the actual outward face, not the pillar center embedded inside
  // the cave wall. The named side view pitches down so both samples are
  // on-screen; an off-screen zero must never count as terrain occlusion.
  var px = e.x - Math.sin(e.angle) * offset - Math.cos(e.angle) * pillarWidth / 2;
  var py = e.y + Math.cos(e.angle) * offset - Math.sin(e.angle) * pillarWidth / 2;
  var lower = G.projToScreen(px, py, e.floorH * 25 + 4, G.getCam3D());
  var upper = G.projToScreen(px, py, (e.floorH + e.ceilH) * 25 - 4, G.getCam3D());
  check('both jamb probes are on-screen', lower && upper && lower.sx >= 0 && lower.sx < G.canvas.width &&
    upper.sx >= 0 && upper.sx < G.canvas.width && lower.sy >= 0 && lower.sy < G.canvas.height &&
    upper.sy >= 0 && upper.sy < G.canvas.height);
  check('side view terrain hides buried lower jamb', visiblePatch(px, py, e.floorH * 25 + 4) === 0);
  check('side view retains exposed upper jamb', visiblePatch(px, py, (e.floorH + e.ceilH) * 25 - 4) > 0);
  var clipCalls = 0, clippedCalls = 0, originalClip = G.withSceneDepthClip;
  G.withSceneDepthClip = function (points, callback, options) {
    clipCalls++;
    var result = originalClip(points, callback, options);
    if (result === 0) clippedCalls++;
    return result;
  };
  G.drawCaveEntrance3D();
  G.withSceneDepthClip = originalClip;
  check('actual entrance renderer uses shared depth clipping', clipCalls > 0);
  check('actual entrance renderer rejects terrain-hidden faces', clippedCalls > 0);
  G.previewCaveView('mouth');
  terrainPasses();
  e = G.deepCaveEntrances[0];
  check('mouth keeps a sightline into entrance air', visiblePatch(e.x + Math.cos(e.angle) * 8,
    e.y + Math.sin(e.angle) * 8, (e.floorH + 2) * 25) > 0);
  var speed = 0.008;
  G.setCavePreviewNoon(false); G.daySpeed = speed;
  G.setCavePreviewNoon(true); G.updateDayNight(10);
  check('fixed noon remains stable during cave test', G.dayTime === 0.5 && G.daySpeed === 0);
  G.setCavePreviewNoon(false);
  check('leaving fixed noon restores previous cycle speed', G.daySpeed === speed);
  output('CAVE_OCCLUSION_INTEGRATION_RESULT PASS ' + checked);
}());
undefined;
