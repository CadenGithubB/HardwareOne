// Seeded production-runtime exit regression. Only browser APIs are stubbed:
// keyboard acceleration, player collision/support, generation and depth drawing
// are the shipping functions. No optional audit fixtures or header snapshots.
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
    if (!elements[id]) elements[id] = stub({id: id, checked: id === 'ctFixedNoon', value: '', textContent: '',
      style: {}, classList: stub(), dataset: {}});
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
  var mutation = __argv.indexOf('--height-only-cull') >= 0;
  var reportOnly = __argv.indexOf('--report-only') >= 0;
  var manifest = JSON.parse(slurp('assets/games/manifest.json'));
  manifest.parts.MAIN.forEach(function (path) {
    var source = slurp('assets/games/' + path);
    if (mutation && path === 'src/13-render-floors-ceilings.js') {
      var marker = 'var floorVertices = [';
      if (source.indexOf(marker) < 0) throw new Error('height-cull mutation anchor missing');
      // Reintroduce the exact historical quad-wide vertical-height rejection.
      source = source.replace(marker,
        'if (cameraZ < Math.min(_z0, _z1, _z2, _z3) * 25 - 1) continue;\n' + marker);
    }
    (0, eval)(source);
  });
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
    G.USE_GAMEPAD = false; G.settings.dayNight = true; G.NOCLIP = false;
    G.dayTime = 0.5; G.daySpeed = 0;
    G.previewCaveView('back');
    G.cam.ang = G.endlessCaveNetworks['0,0'].entrances[0].angle + Math.PI;
    G.cam.pitch = 0;
  }
  function cameraFollow() { G.cam.x = G.pos.x; G.cam.y = G.pos.y; G.cam.z = G.pos.floorZ; }
  // Independent geometric oracle: enumerate stamped semantic floor triangles,
  // reject backsides by an explicit unnormalised normal dot eye vector, then
  // raster only uphill triangles that the historical height-only rule omitted.
  // It shares the established stitch/diagonal/projection/depth contracts, but
  // never calls the production face-culling helper or copied culling logic.
  function expectedUphillDepth() {
    var C = G.getCam3D(), mesh = G.floorMesh, gs = mesh.gridSize;
    G.beginSceneDepthFrame(C.w, C.h);
    var count = 0, rad = Math.min(600, G.viewDist * 0.6), radSq = rad * rad;
    var gx0 = Math.max(0, Math.floor((G.cam.x - rad) / gs));
    var gy0 = Math.max(0, Math.floor((G.cam.y - rad) / gs));
    var gx1 = Math.min(mesh.w - 2, Math.ceil((G.cam.x + rad) / gs));
    var gy1 = Math.min(mesh.h - 2, Math.ceil((G.cam.y + rad) / gs));
    for (var gy = gy0; gy <= gy1; gy++) for (var gx = gx0; gx <= gx1; gx++) {
      var dx = (gx + 0.5) * gs - G.cam.x, dy = (gy + 0.5) * gs - G.cam.y;
      var fwd = dx * C.cosAng + dy * C.sinAng;
      var right = -dx * C.sinAng + dy * C.cosAng;
      // Deliberately inside the renderer's distance/FOV limits; a cull at the
      // outer view boundary must not be misdiagnosed as a missing near slope.
      if (dx * dx + dy * dy > radSq || fwd < 24 || Math.abs(right) > fwd / C.invTanHalf * 0.8) continue;
      var ci = gy * mesh.w + gx;
      for (var li = 0; li < mesh.layerCount[ci]; li++) {
        var type = mesh['l' + li + 'Type'][ci], z = mesh['l' + li + 'TopZ'][ci];
        if (type !== 1 && type !== 3 && type !== 4) continue;
        var role = G.getFloorRenderLayerRole(mesh, ci, li, type);
        var z1 = G.findFloorRenderRoleZ(mesh, ci + 1, role, z, ci);
        var z2 = G.findFloorRenderRoleZ(mesh, ci + mesh.w, role, z, ci);
        var z3 = G.findFloorRenderRoleZ(mesh, ci + mesh.w + 1, role, z, ci);
        if (!Number.isFinite(z1) || !Number.isFinite(z2) || !Number.isFinite(z3)) continue;
        z1 = Math.max(z - 4.5, Math.min(z + 4.5, z1));
        z2 = Math.max(z - 4.5, Math.min(z + 4.5, z2));
        z3 = Math.max(z - 4.5, Math.min(z + 4.5, z3));
        if (C.cameraZ >= Math.min(z, z1, z2, z3) * 25 - 1) continue;
        var x = gx * gs, y = gy * gs;
        var vertices = [{x:x,y:y,z:z*25}, {x:x+gs,y:y,z:z1*25},
          {x:x+gs,y:y+gs,z:z3*25}, {x:x,y:y+gs,z:z2*25}];
        [[0,1,2],[0,2,3]].forEach(function (indices) {
          var a = vertices[indices[0]], b = vertices[indices[1]], c = vertices[indices[2]];
          var ux = b.x-a.x, uy = b.y-a.y, uz = b.z-a.z;
          var vx = c.x-a.x, vy = c.y-a.y, vz = c.z-a.z;
          var nx = uy*vz-uz*vy, ny = uz*vx-ux*vz, nz = ux*vy-uy*vx;
          var side = nx*(G.cam.x-a.x)+ny*(G.cam.y-a.y)+nz*(C.cameraZ-a.z);
          if (side <= 0) return;
          var polygon = G.projectSceneWorldPolygon([a,b,c], C);
          if (polygon.length < 3) return;
          G.writeSceneDepthPolygon(polygon); count++;
        });
      }
    }
    return {triangles: count, depth: new Float32Array(G._sceneDepthInv)};
  }
  function compareFrame() {
    var expected = expectedUphillDepth(), missing = 0, visible = 0;
    G.beginSceneDepthFrame(G.canvas.width, G.canvas.height);
    G.drawLayersCeiling3D(); G.drawLayersFloor3D(); G.drawWalls3D();
    for (var i = 0; i < G.canvas.width * G.canvas.height; i++) {
      var reference = expected.depth[i];
      if (reference <= 0) continue;
      visible++;
      // Float32 storage only: nearer real geometry legitimately occludes the
      // reference slope. A farther sample or an empty pixel cannot replace it.
      if (G._sceneDepthInv[i] + 0.000001 < reference) missing++;
    }
    return {triangles: expected.triangles, pixels: visible, missing: missing};
  }
  [[12345,'descending'],[5668,'hillside']].forEach(function (fixture) {
    start(fixture[0], fixture[1]);
    var e = G.endlessCaveNetworks['0,0'].entrances[0], kind = fixture[1];
    var cos = Math.cos(e.angle), sin = Math.sin(e.angle), firstH = G.getPlayerFloorH();
    var targets = [-85,-25,35,95,170,260,400,520], next = 0;
    var frames = [], steps = 0, maxStep = 0, underground = false, outdoors = false;
    var totalPixels = 0, totalMissing = 0, exercisedFrames = 0;
    G.kbState.up = true;
    while (steps++ < 2400 && next < targets.length) {
      var beforeX = G.pos.x + G.windowOriginX, beforeY = G.pos.y + G.windowOriginY;
      G.handleInput(G.FIXED_DT); G.step(G.FIXED_DT); cameraFollow();
      var worldX = G.pos.x + G.windowOriginX, worldY = G.pos.y + G.windowOriginY;
      maxStep = Math.max(maxStep, Math.hypot(worldX-beforeX,worldY-beforeY));
      var along = (e.x-worldX)*cos+(e.y-worldY)*sin;
      underground = underground || G.playerUnderground; outdoors = outdoors || !G.playerUnderground;
      if (along < targets[next]) continue;
      var sums = {along:along, floorH:G.getPlayerFloorH(), underground:G.playerUnderground,
        triangles:0,pixels:0,missing:0};
      // Yaw changes do not move the player, then keyboard travel resumes down
      // the same centreline. Include both oblique sides and a downward glance.
      [-0.22,0,0.22].forEach(function (yaw) {
        G.cam.ang = e.angle+Math.PI+yaw; G.cam.pitch = yaw === 0 ? 0 : 0.12;
        var result = compareFrame();
        sums.triangles += result.triangles; sums.pixels += result.pixels; sums.missing += result.missing;
        if (result.pixels > 0) exercisedFrames++;
      });
      G.cam.ang = e.angle+Math.PI; G.cam.pitch = 0;
      totalPixels += sums.pixels; totalMissing += sums.missing; frames.push(sums); next++;
    }
    G.kbState.up = false;
    output('EXIT_TRACE ' + JSON.stringify({kind:kind,steps:steps,maxStep:maxStep,frames:frames}));
    check(kind+' exits through real keyboard and collision steps', next === targets.length && maxStep < 12);
    check(kind+' crosses covered and open-air space without noclip', underground && outdoors && !G.NOCLIP);
    check(kind+' climbs from tunnel support onto surface', G.getPlayerFloorH() > firstH);
    check(kind+' continuously sampled exit includes vulnerable uphill pixels', exercisedFrames >= 3 && totalPixels > 100);
    if (!reportOnly) check(kind+' exit slopes never expose deeper terrain or sky', totalMissing === 0);
    check(kind+' runtime emits no swallowed rendering errors', errors.length === 0);
  });
  output('CAVE_EXIT_TRANSITION_RESULT PASS '+checked);
}());
undefined;
