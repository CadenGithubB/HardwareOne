// Production material consumers with a recording Canvas host. Compatibility
// hashes were recorded before replacing the old inline colors (7d81615).
// This checks drawing commands/colors, not native browser rasterization.
(function () {
  var G = (0, eval)('this'), checks = 0, calls = [];
  function check(name, okay) {
    if (!okay) throw new Error(name);
    checks++; __out('PASS ' + name);
  }
  function hash(value) {
    var text = JSON.stringify(value), h = 2166136261;
    for (var i = 0; i < text.length; i++) h = Math.imul(h ^ text.charCodeAt(i), 16777619);
    return ('00000000' + (h >>> 0).toString(16)).slice(-8);
  }
  function normalize(value) {
    if (typeof value === 'number') return Math.round(value * 1e8) / 1e8;
    if (Array.isArray(value)) return value.map(normalize);
    if (value && typeof value === 'object') {
      var result = {};
      Object.keys(value).forEach(function(k) { result[k] = normalize(value[k]); });
      return result;
    }
    return value;
  }
  function record(name, args) { calls.push([name].concat(normalize(Array.prototype.slice.call(args)))); }
  var contextState = {};
  var context = new Proxy(contextState, {
    set: function(target, key, value) { calls.push(['set', key, value]); target[key] = value; return true; },
    get: function(target, key) {
      if (key in target) return target[key];
      if (key === 'createPattern') return function() { return 'recorded-pattern'; };
      if (key === 'createLinearGradient') return function() {
        record('createLinearGradient', arguments);
        return {addColorStop: function() { record('addColorStop', arguments); }};
      };
      return function() { record(key, arguments); };
    }
  });
  G.ctx = context;
  G.GAME_CONFIG = {hud: {minimapW: 90, minimapH: 65}};
  G.document = {createElement: function() { return {getContext: function() { return context; }}; }};
  ['01-materials.js', '02-utilities-terrain.js', '07-decorations-lighting.js',
   '15-render-hud-menu.js', '17-input-lighting-update.js'].forEach(function(path) {
    (0, eval)(slurp('assets/games/src/' + path));
  });
  function prop(type) {
    calls = []; G.terrain = 'ground';
    [0, 1, 2].forEach(function(variant) {
      [0.125, 0.731].forEach(function(seed) {
        [12, 48].forEach(function(size) {
          // A real Canvas save/restore pair restores caller alpha. The recording
          // host has no state stack, so restore it silently between recipes.
          contextState.globalAlpha = 1;
          G.drawFloorItem(type, variant, seed, 20, 30, size);
        });
      });
    });
    return hash(calls);
  }
  function pattern(kind) {
    calls = [];
    var originalRandom = Math.random, seed = 12345;
    Math.random = function() { seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0; return seed / 4294967296; };
    try { G.makePattern(kind); } finally { Math.random = originalRandom; }
    return hash(calls);
  }
  G.floorMesh = null; G.MODE3D = true; G.DEBUG_POLY_TYPES = false;
  G.cam = {x: 0, y: 0}; G.pos = {x: 0, y: 0};
  G.getCam3D = function() { return {cosAng: 1, sinAng: 0}; };
  G.projectSceneWorldPolygon = function(points) { return points; };
  G.fillSceneDepthPolygon = function(points) { record('depthPolygon', [points]); };
  G.rgbQ = function(r,g,b) { return 'rgb(' + r + ',' + g + ',' + b + ')'; };
  G.getCaveRenderLightAt = function() { return 0.45; };
  G.projToScreen = function() { return null; };
  function entrance(style) {
    calls = [];
    G.deepCaveEntrances = [{x: 120, y: 0, angle: 0, halfWidth: 54, floorH: -5,
      ceilingH: -1, minCover: 1, style: style}];
    G.drawCaveEntrance3D(); return hash(calls);
  }
  var actual = {biomes: hash(G.BIOME_PALETTE), props: {}, patterns: {}, entrances: {}};
  ['bones','dry_bones','crate','skull','rubble','rock_pile','rib_cage','femur'].forEach(function(type) {
    actual.props[type] = prop(type);
  });
  ['ground','cave','ice','expanse','plains','forest','unknown'].forEach(function(kind) {
    actual.patterns[kind] = pattern(kind);
  });
  [0.1, 0.5, 0.8].forEach(function(style) { actual.entrances[style] = entrance(style); });
  if (__argv.indexOf('--record-compatibility') >= 0) { __out(JSON.stringify(actual)); return; }
  var expected = {"biomes":"ad621a9d","props":{"bones":"27798a8d","dry_bones":"0c0820ed","crate":"ea81616d","skull":"490a18f5","rubble":"66fb80fe","rock_pile":"4e1906cf","rib_cage":"7705d34d","femur":"536b6cb2"},"patterns":{"ground":"9b4af2e2","cave":"295107e0","ice":"fb0567db","expanse":"35c4238a","plains":"9b4af2e2","forest":"9b4af2e2","unknown":"9b4af2e2"},"entrances":{"0.1":"5c5c24d4","0.5":"2f82659e","0.8":"81c98c5e"}};
  check('existing biome colors are unchanged', actual.biomes === expected.biomes);
  Object.keys(actual.props).forEach(function(type) {
    check(type + ' colors geometry alpha and draw order match baseline', actual.props[type] === expected.props[type]);
  });
  Object.keys(actual.patterns).forEach(function(kind) {
    check(kind + ' pattern and random sequence match baseline', actual.patterns[kind] === expected.patterns[kind]);
  });
  Object.keys(actual.entrances).forEach(function(style) {
    check('entrance style ' + style + ' matches baseline', actual.entrances[style] === expected.entrances[style]);
  });
  var compiled = G.compileGameMaterials({sample: {black: '#000000', color: '#12ABef'}});
  check('packed black is zero, not a missing color', compiled.sample.packed.black === 0);
  check('hex RGB and packed forms agree', compiled.sample.hex.color === '#12ABef' &&
    compiled.sample.packed.color === 0x12abef && JSON.stringify(compiled.sample.rgb.color) === '[18,171,239]');
  check('all compiled color forms are read only', Object.isFrozen(compiled) && Object.isFrozen(compiled.sample) &&
    Object.isFrozen(compiled.sample.hex) && Object.isFrozen(compiled.sample.rgb.color) && Object.isFrozen(compiled.sample.swatches));
  check('swatches preserve author order', JSON.stringify(compiled.sample.swatches) === '["#000000","#12ABef"]');
  var bad = [null, [], {empty: {}}, {bad: {base: '#fff'}}, {bad: {base: 'red'}}, {bad: {base: '#gg0000'}}];
  check('invalid definitions fail explicitly', bad.every(function(value) {
    try { G.compileGameMaterials(value); return false; } catch (e) { return true; }
  }));
  var originalMaterials = G.GAME_MATERIALS, custom = JSON.parse(JSON.stringify(G.GAME_MATERIAL_COLORS));
  custom.crateWood.base = '#112233'; custom.rubbleStone.base = '#224466';
  custom.entranceStone.mid = '#556677'; custom.palisadeWood.lit = '#778899';
  custom.caveStone.base = custom.caveStone.damp = custom.caveStone.iron = custom.caveStone.worn = '#204060';
  G.GAME_MATERIALS = G.compileGameMaterials(custom);
  check('wood definition reaches crate renderer', prop('crate') !== expected.props.crate);
  check('one stone definition reaches rubble and rock piles', prop('rubble') !== expected.props.rubble && prop('rock_pile') !== expected.props.rock_pile);
  check('unrelated prop material remains unchanged', prop('bones') === expected.props.bones);
  entrance(0.5);
  check('entrance material reaches constructed frame', calls.some(function(c) { return c[0] === 'set' && c[1] === 'fillStyle' && c[2] === '#556677'; }));
  entrance(0.8);
  check('wood material reaches entrance palisades', calls.some(function(c) { return c[0] === 'set' && c[1] === 'fillStyle' && c[2] === '#778899'; }));
  check('cave lookup fallback uses shared base', G.getCaveMaterialColorAt(0, 0) === 0x204060);
  var deep = G.sampleCaveMaterialColor('#ffffff', 600, 0, []);
  check('authored cave stone uses shared RGB without lighting', ((deep >> 8) & 255) - ((deep >> 16) & 255) === 32 &&
    (deep & 255) - ((deep >> 8) & 255) === 32);
  ['#000000', '#ffffff'].forEach(function(color) {
    custom.caveStone.base = custom.caveStone.damp = custom.caveStone.iron = custom.caveStone.worn = color;
    G.GAME_MATERIALS = G.compileGameMaterials(custom);
    var okay = true;
    for (var cell = 0; cell < 32; cell++) {
      var packed = G.sampleCaveMaterialColor('#ffffff', cell * 12, 0, []);
      var r = (packed >> 16) & 255, g = (packed >> 8) & 255, b = packed & 255;
      if (r !== g || g !== b || (color === '#000000' ? r > 6 : r < 249)) okay = false;
    }
    check(color + ' cave grain clamps channels without packed overflow', okay);
  });
  G.GAME_MATERIALS = originalMaterials;
  var oldBase = G.BIOME_PALETTE.ground.patternBase;
  G.BIOME_PALETTE.ground.patternBase = '#123456';
  check('pattern reads existing biome palette instead of inline copy', pattern('ground') !== expected.patterns.ground);
  G.BIOME_PALETTE.ground.patternBase = oldBase;
  __out('MATERIAL_PALETTE_RESULT PASS ' + checks);
}());
undefined;
