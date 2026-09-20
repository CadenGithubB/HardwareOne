// Wall attachments use the same cell/top geometry as the wall renderer and
// carry their flame with them. These checks require no browser/audit lab.
(function () {
  var G = (0, eval)('this');
  (0, eval)(slurp('assets/games/src/05-endless-world.js'));
  (0, eval)(slurp('assets/games/src/07-decorations-lighting.js'));
  var n = 0;
  function check(name, okay) { if (!okay) throw new Error(name); n++; __out('PASS ' + name); }
  G.windowOriginX = -480; G.windowOriginY = 960; G.CHUNK_CELLS = 20;
  var source = {worldX: 72, worldY: 1080, side:'west', type:'torch', gridX:3, gridY:5};
  var copy = G.chunkWallDecorationInWindow(source, 1, 2);
  check('window assembly rebases decoration XY', copy.worldX === 552 && copy.worldY === 120);
  check('window assembly also rebases supporting cell', copy.gridX === 23 && copy.gridY === 45);
  check('window assembly never mutates stored chunk decoration', source.gridX === 3 && source.gridY === 5 && source.worldX === 72);

  G.gridW = G.gridH = 12; G.cell = 24; G.CANVAS_BASE_H = 240;
  G.grid = new Uint8Array(144); G.wallHeights = new Float32Array(144);
  G.wallMaxTopZ = new Float32Array(144); G.wallMaxTopZ.fill(Infinity);
  G.wallFaceBase = new Float32Array(144 * 4); G.floorMesh = {};
  G.WALL_DECOR_TIER = {torch:'lgWall'}; G.getScale3D = function () { return 1; };
  G.getFloorHeightAt = function () { return 0; };
  G.getCaveSpaceAt = function () { return {ceilingH:Infinity}; };
  var ci = 4 * G.gridW + 3; G.grid[ci] = 1; G.wallHeights[ci] = 1;
  var dec = {worldX:72, worldY:108, gridX:3, gridY:4, side:'west', type:'torch'};
  var a = G.getWallDecorationAttachment(dec);
  check('attachment uses supporting wall height', Math.abs(a.topZ - 240) < 1e-8 && a.baseZ === 0);
  check('torch body and flame fit on supporting face', a.z - a.size >= a.baseZ && a.z + a.size * 0.75 <= a.topZ);
  check('flame origin matches drawn torch geometry', a.flameZ === a.z + a.size / 4);
  check('west attachment offsets into open space', a.x === dec.worldX - 0.5 && a.y === dec.worldY);
  G.wallMaxTopZ[ci] = 0.6;
  a = G.getWallDecorationAttachment(dec);
  check('short clamped wall scales down its torch', Math.abs(a.topZ - 15) < 1e-6 && a.size < 5 && a.z + a.size * 0.75 <= a.topZ);
  G.wallMaxTopZ[ci] = 0;
  check('collapsed wall cannot leave a floating decoration', G.getWallDecorationAttachment(dec) === null);
  G.wallMaxTopZ[ci] = Infinity; G.grid[ci] = 0;
  check('removed supporting cell cannot leave a decoration', G.getWallDecorationAttachment(dec) === null);
  G.grid[ci] = 1;
  G.getFloorHeightAt = function (x, y) { return x === 84 && y === 108 ? 2 : 0; };
  a = G.getWallDecorationAttachment(dec);
  check('wall top is sampled at center not ambiguous face boundary', Math.abs(a.topZ - 290) < 1e-8);
  G.getCaveSpaceAt = function () { return {ceilingH:1}; };
  a = G.getWallDecorationAttachment(dec);
  check('cavity flame cannot mount above actual ceiling', a.topZ === 25 && a.flameZ < 25);
  G.getFloorHeightAt = function () { return 0; };
  G.getCaveSpaceAt = function () { return {ceilingH:Infinity}; };
  G.wallDecorations = ['north','south','west','east'].map(function (side) {
    return {worldX:72,worldY:108,gridX:3,gridY:4,side:side,type:'torch'};
  });
  G.deepCaveEntrances = []; G.worldW = G.worldH = 288;
  G._lightCellSize = 24; G._lightGrid = null; G._lightGridW = G._lightGridH = 0;
  G.console = {log:function () {}};
  G.buildPointLights();
  var outward = [[0,-0.5],[0,0.5],[-0.5,0],[0.5,0]], aligned = G.pointLights.length === 4;
  for (var i = 0; i < 4; i++) {
    var light = G.pointLights[i], at = G.getWallDecorationAttachment(G.wallDecorations[i]);
    if (light.x !== 72 + outward[i][0] || light.y !== 108 + outward[i][1] || light.z !== at.flameZ) aligned = false;
  }
  check('every cardinal light sits outside wall at its actual flame', aligned);
  __out('WALL_DECOR_RESULT PASS ' + n);
}());
undefined;
