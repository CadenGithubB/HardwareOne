// Ruin debris and entrance dressing must be authored in world space. Camera
// approach may change projection, never the underlying positions/orientation.
(function () {
  var G = (0, eval)('this'), checks = 0, fillRects = 0;
  function check(name, okay) {
    if (!okay) throw new Error(name);
    checks++; __out('PASS ' + name);
  }
  function rounded(value) { return Math.round(value * 1000000) / 1000000; }
  function normalizePoints(points) {
    return points.map(function (p) { return [rounded(p.x), rounded(p.y), rounded(p.z)]; });
  }
  function hash(value) {
    var text = JSON.stringify(value), h = 2166136261;
    for (var i = 0; i < text.length; i++) h = Math.imul(h ^ text.charCodeAt(i), 16777619);
    return h >>> 0;
  }

  G.ctx = {
    save:function(){}, restore:function(){}, fillRect:function(){fillRects++;},
    fillText:function(){}, measureText:function(text){return {width:String(text).length * 7};},
    fillStyle:'#000', globalAlpha:1, font:'', textAlign:'left'
  };
  G.MODE3D = true; G.viewDist = 1000; G.projScale = 180;
  G.GAME_MATERIALS = {
    rubbleStone:{hex:{base:'#787060',shadow:'#686058',lit:'#888070',dark:'#504840'}},
    palisadeWood:{hex:{lit:'#7a4c2a',mid:'#6b4226',dark:'#5a3720',deep:'#3a2412'}}
  };
  G.getEntityGroundRenderZ = function (x, y) {
    return Math.floor(x / 24) * 0.25 + Math.floor(y / 24) * 0.125;
  };
  G.withSceneDepthBillboard = function (bounds, depth, draw) {
    if (bounds && depth >= 1) { draw(); return 1; }
    return 0;
  };
  G.projToScreen = function (wx, wy, wz, C) {
    var dx = wx - G.cam.x, dy = wy - G.cam.y;
    var fwd = dx * C.cosAng + dy * C.sinAng;
    if (fwd < 1) return null;
    var right = -dx * C.sinAng + dy * C.cosAng;
    return {sx:(right / fwd * C.invTanHalf * 0.5 + 0.5) * C.w,
      sy:C.horizonY + (C.cameraZ - wz) / fwd * G.projScale, fwd:fwd};
  };

  var depthSource = slurp('assets/games/src/12-scene-depth.js');
  var projectStart = depthSource.indexOf('function projectSceneWorldPolygon(');
  check('production near-plane projector found', projectStart >= 0);
  (0, eval)(depthSource.slice(projectStart));
  var source = slurp('assets/games/src/15-render-hud-menu.js');
  var start = source.indexOf('var RUIN_DEBRIS_LAYOUTS =');
  var end = source.indexOf('function drawStructures3D()', start);
  check('production ruin renderer and layout found', start >= 0 && end > start);
  (0, eval)(source.slice(start, end));

  var worldFaces = [], commands = [], lastOptions = null;
  var productionProject = G.projectSceneWorldPolygon;
  G.projectSceneWorldPolygon = function (vertices, C) {
    worldFaces.push(normalizePoints(vertices));
    return productionProject(vertices, C);
  };
  G.fillSceneDepthPolygon = function (points) {
    commands.push(['poly', G.ctx.fillStyle, points.map(function (p) {
      return [rounded(p.x), rounded(p.y), rounded(p.depth)];
    }), G.ctx.globalAlpha]);
    return 1;
  };
  G.renderEntities3D = function (arr, opts, draw) {
    lastOptions = opts;
    var C = {w:360,h:240,cosAng:Math.cos(G.cam.ang),sinAng:Math.sin(G.cam.ang),
      invTanHalf:1,horizonY:120,cameraZ:60};
    for (var i = 0; i < arr.length; i++) {
      var dx = arr[i].x - G.cam.x, dy = arr[i].y - G.cam.y;
      var fwd = dx * C.cosAng + dy * C.sinAng;
      if (fwd < 1) continue;
      draw(arr[i], {fwd:fwd,dist:Math.hypot(dx,dy),floorZ:G.getEntityGroundRenderZ(arr[i].x,arr[i].y),fade:1}, C, G.ctx, 0);
    }
  };

  var ruin = {x:120,y:0,ruinType:'hut',facing:1};
  G.ruins = [ruin];
  function renderAt(x, y, ang) {
    G.cam = {x:x,y:y,ang:ang}; worldFaces = []; commands = []; fillRects = 0;
    G.drawRuins3D();
    return {world:JSON.parse(JSON.stringify(worldFaces)), commands:JSON.parse(JSON.stringify(commands)), rects:fillRects};
  }

  var far = renderAt(0, 0, 0), middle = renderAt(30, 0, 0), close = renderAt(60, 0, 0);
  check('ruin visibility is surface anchored and uses per-pixel scene depth',
    lastOptions.groundAnchor === true && lastOptions.sceneDepth === true && !lastOptions.skipDepth);
  check('approach leaves every authored debris and post world face fixed',
    hash(far.world) === hash(middle.world) && hash(middle.world) === hash(close.world));
  check('camera approach changes only the projected draw commands',
    hash(far.commands) !== hash(middle.commands) && hash(middle.commands) !== hash(close.commands));
  check('ruin dressing uses clipped world polygons rather than screen rectangles',
    far.world.length === 40 && far.commands.length === 40 && far.rects === 0);
  check('hut dressing remains inside its cleared terrain pad', far.world.every(function (face) {
    return face.every(function (p) {
      return Math.abs(p[0] - ruin.x) <= 42 && Math.abs(p[1] - ruin.y) <= 42;
    });
  }));
  check('opaque ruin faces never write depth under a translucent paint alpha',
    far.commands.every(function (command) { return command[3] === 1; }) &&
    middle.commands.every(function (command) { return command[3] === 1; }) &&
    close.commands.every(function (command) { return command[3] === 1; }));

  var stableA = G.getRuinDebrisWorld(ruin, 0, {});
  G.cam.x = 99; G.cam.y = -123;
  var stableB = G.getRuinDebrisWorld(ruin, 0, {});
  check('debris coordinates contain no camera-distance term', JSON.stringify(stableA) === JSON.stringify(stableB));
  var rotations = [];
  for (var facing = 0; facing < 4; facing++) {
    rotations.push(G.getRuinDebrisWorld({x:120,y:0,ruinType:'hut',facing:facing}, 0, {}));
  }
  var radius = Math.hypot(rotations[0].x - 120, rotations[0].y);
  check('authored debris rotates with ruin facing at constant radius', rotations.every(function (p) {
    return Math.abs(Math.hypot(p.x - 120, p.y) - radius) < 1e-9;
  }) && rotations[0].x !== rotations[1].x && rotations[0].y !== rotations[1].y);

  var nearRuin = {x:120,y:0,ruinType:'hut',facing:0};
  G.ruins = [nearRuin];
  var nearStone = G.getRuinDebrisWorld(nearRuin, 0, {});
  var near = renderAt(nearStone.x - 0.5, nearStone.y, 0);
  var finite = near.commands.length > 0, touchedNearPlane = false;
  for (var ci = 0; ci < near.commands.length; ci++) {
    var projected = near.commands[ci][2];
    for (var pi = 0; pi < projected.length; pi++) {
      if (!Number.isFinite(projected[pi][0]) || !Number.isFinite(projected[pi][1]) ||
          !Number.isFinite(projected[pi][2]) || projected[pi][2] < 1) finite = false;
      if (Math.abs(projected[pi][2] - 1) < 1e-9) touchedNearPlane = true;
    }
  }
  check('near-plane crossings are clipped to finite forward geometry', finite && touchedNearPlane && near.rects === 0);
  __out('RUIN_DECOR_RESULT PASS ' + checks);
}());
undefined;
