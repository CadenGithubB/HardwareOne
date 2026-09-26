// Wall attachments use the same cell/top geometry as the wall renderer and
// carry their flame with them. These checks require no browser/audit lab.
(function () {
  var G = (0, eval)('this');
  (0, eval)(slurp('assets/games/src/01-materials.js'));
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

  check('legacy generated names normalize to visible canonical props',
    G.canonicalWallDecorationType('stalactite') === 'stalactite_tip' &&
    G.canonicalWallDecorationType('crack') === 'wall_crack' &&
    G.canonicalWallDecorationType('moss') === 'moss_drip' &&
    G.canonicalWallDecorationType('frost_crack') === 'frost_crystal' &&
    G.canonicalWallDecorationType('vine') === 'vine_growth');
  var endlessSource = slurp('assets/games/src/05-endless-world.js');
  check('endless generation pools emit canonical wall prop names',
    ["'stalactite'", "'crack'", "'moss'", "'frost_crack'", "'vine'"].every(function(name) {
      return endlessSource.indexOf(name) < 0;
    }) && endlessSource.indexOf("'wall_crack'") >= 0);
  var legacyCopy = G.chunkWallDecorationInWindow(
    {worldX:72,worldY:1080,side:'north',type:'crack',gridX:3,gridY:5}, 1, 2);
  check('window assembly repairs cached legacy prop names', legacyCopy.type === 'wall_crack');

  var calls = [];
  function record(name, args) {
    calls.push([name].concat(Array.prototype.slice.call(args).map(function(value) {
      return typeof value === 'number' ? Math.round(value * 1000000) / 1000000 : value;
    })));
  }
  G.ctx = new Proxy({globalAlpha:1}, {
    set:function(target, key, value) { target[key] = value; calls.push(['set', key, value]); return true; },
    get:function(target, key) {
      if (key in target) return target[key];
      return function() { record(key, arguments); };
    }
  });
  G.rgbQ = function(r,g,b) { return 'rgb(' + r + ',' + g + ',' + b + ')'; };
  function render(type, gridX, gridY) {
    calls = [];
    var sample = {type:type, side:'north', gridX:gridX || 3, gridY:gridY || 4};
    G.drawWallAlignedDecoration(type, 120, 80, 32, 36, 'north', 0.9, 1, sample, 2000000000000);
    return JSON.parse(JSON.stringify(calls));
  }
  function hash(value) {
    var text = JSON.stringify(value), h = 2166136261;
    for (var hi = 0; hi < text.length; hi++) h = Math.imul(h ^ text.charCodeAt(hi), 16777619);
    return h >>> 0;
  }
  var allTypes = ['torch','sconce','shield','banner','wall_crack','fungi','moss_drip',
    'stalactite_tip','icicle','frost_crystal','vine_growth','carved_rune'];
  var rendered = {};
  allTypes.forEach(function(type) { rendered[type] = render(type); });
  check('every generated wall prop has visible drawing commands', allTypes.every(function(type) {
    return rendered[type].some(function(call) { return call[0] === 'fill' || call[0] === 'stroke' || call[0] === 'fillRect'; });
  }));
  check('torch includes mount shaft bands and layered flame',
    rendered.torch.filter(function(call){ return call[0] === 'fillRect'; }).length >= 4 &&
    rendered.torch.filter(function(call){ return call[0] === 'ellipse'; }).length >= 3);
  check('sconce includes backplate bracket bowl and layered flame',
    rendered.sconce.filter(function(call){ return call[0] === 'ellipse'; }).length >= 5 &&
    rendered.sconce.some(function(call){ return call[0] === 'lineTo'; }));
  check('shield has a pointed silhouette rim emblem and boss',
    rendered.shield.filter(function(call){ return call[0] === 'quadraticCurveTo'; }).length >= 6 &&
    rendered.shield.some(function(call){ return call[0] === 'stroke'; }) &&
    rendered.shield.some(function(call){ return call[0] === 'arc'; }));
  check('banner has a crossbar swallowtail folds and heraldry',
    rendered.banner.filter(function(call){ return call[0] === 'fillRect'; }).length >= 2 &&
    rendered.banner.filter(function(call){ return call[0] === 'lineTo'; }).length >= 8);
  check('wall crack is a branched non-glowing stroke',
    rendered.wall_crack.filter(function(call){ return call[0] === 'moveTo'; }).length >= 4 &&
    rendered.wall_crack.filter(function(call){ return call[0] === 'stroke'; }).length === 1 &&
    rendered.wall_crack.every(function(call){ return call[0] !== 'ellipse' && call[0] !== 'arc'; }));
  check('fixed prop inputs produce deterministic Canvas commands',
    hash(render('torch', 3, 4)) === hash(render('torch', 3, 4)) &&
    hash(render('banner', 3, 4)) === hash(render('banner', 3, 4)));
  check('stable placement hash gives adjacent props visual variety',
    G.getWallDecorationVariant({side:'north',gridX:3,gridY:4}) !==
    G.getWallDecorationVariant({side:'north',gridX:4,gridY:4}));
  check('wall prop draw recipes remain deliberately bounded', allTypes.every(function(type) {
    return rendered[type].length < 180;
  }));
  check('constructed prop paths stay inside their local wall footprint',
    ['torch','sconce','shield','banner','wall_crack'].every(function(type) {
      return rendered[type].every(function(call) {
        if (call[0] === 'moveTo' || call[0] === 'lineTo')
          return Math.abs(call[1] - 120) <= 48 && Math.abs(call[2] - 80) <= 48;
        if (call[0] === 'quadraticCurveTo')
          return Math.abs(call[1] - 120) <= 48 && Math.abs(call[2] - 80) <= 48 &&
                 Math.abs(call[3] - 120) <= 48 && Math.abs(call[4] - 80) <= 48;
        return true;
      });
    }));
  calls = [];
  G.drawWallAlignedDecoration('fungi', 120, 80, 32, 110, 'north', 0.9, 0.2,
    {type:'fungi',side:'north',gridX:3,gridY:4}, 2000000000000);
  check('natural wall props honor the same distance fade', calls.filter(function(call) {
    return call[0] === 'set' && call[1] === 'globalAlpha' && typeof call[2] === 'number';
  }).every(function(call) { return call[2] <= 0.1800001; }));
  var decorSource = slurp('assets/games/src/07-decorations-lighting.js');
  check('wall props avoid filters readback gradients and per-pixel work',
    !/shadowBlur|\.filter|getImageData|putImageData|createLinearGradient|createRadialGradient/.test(
      decorSource.slice(decorSource.indexOf('function getWallDecorationVariant'),
        decorSource.indexOf('// ── FLOOR SCATTER'))));
  __out('WALL_DECOR_RESULT PASS ' + n);
}());
undefined;
