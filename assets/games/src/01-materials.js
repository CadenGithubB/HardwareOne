// Pure appearance data: loaded before DOM/game configuration.
// Canvas2D rendering, world generation, and gameplay are not initialized here.

// =============================================
// BIOME_PALETTE — Centralized per-biome color definitions
// =============================================
// Existing biome API and values are preserved. Named object materials below
// complement these environmental colors; lighting and geometry stay separate.
// Consumers include floors, walls, patterns and the sky. This is not yet a
// palette for every prop, structure, spell or HUD element in the game.
var BIOME_PALETTE = {
  cave: {
    wallColor:  '#6a6a70',
    ceilFill:   '#1a1a1e',
    patternBase:'#2a2a2e',
    wallBaseRGB: [95, 95, 105],
    // Floor height bands: 8 entries from deepest (-0.9) to highest (>0.5)
    floorBands: ['#18181c','#28282e','#3a3a42','#4e4e58','#7a7a80','#8a8a90','#9a9aa0','#aaaab0'],
    // Sky: [top, bottom] RGB arrays
    sky:     [[0x12,0x12,0x1a], [0x1a,0x1a,0x1e]],
    // Mountain layers: [far, mid, near] RGB arrays (hidden for cave)
    mountain:[[0x12,0x12,0x1a], [0x12,0x12,0x1a], [0x12,0x12,0x1a]],
    foothills: [0x12,0x12,0x1a],
    haze:      [15,15,25],
    mountainVisible: 0
  },
  ground: {
    wallColor:  '#a77a45',
    ceilFill:   '#3f2f1c',
    patternBase:'#3b2a18',
    wallBaseRGB: [180, 140, 100],
    floorBands: ['#0e1a12','#1e2e22','#2a4232','#3a5642','#5a8a69','#6a9a79','#7aaa89','#8aba99'],
    sky:     [[0x06,0x06,0x08], [0x0e,0x0c,0x08]],
    mountain:[[0x22,0x1a,0x0e], [0x1a,0x14,0x08], [0x12,0x0e,0x05]],
    foothills: [0x0c,0x0a,0x04],
    haze:      [30,22,12],
    mountainVisible: 1
  },
  plains: {
    wallColor:  '#a77a45',
    ceilFill:   '#3f2f1c',
    patternBase:'#3b2a18',
    wallBaseRGB: [180, 140, 100],
    floorBands: ['#12180a','#222e10','#344218','#4a5a28','#6a7a40','#808e50','#96a260','#a8b870'],
    sky:     [[0x06,0x06,0x08], [0x0e,0x0c,0x08]],
    mountain:[[0x22,0x1a,0x0e], [0x1a,0x14,0x08], [0x12,0x0e,0x05]],
    foothills: [0x0c,0x0a,0x04],
    haze:      [30,22,12],
    mountainVisible: 1
  },
  forest: {
    wallColor:  '#4b3723',
    ceilFill:   '#1a2a10',
    patternBase:'#2a3a18',
    wallBaseRGB: [75, 55, 35],
    floorBands: ['#0e1608','#1a2810','#283a18','#385020','#4a6830','#5a7a40','#6a8a50','#7a9a60'],
    sky:     [[0x08,0x0a,0x06], [0x12,0x18,0x0c]],
    mountain:[[0x1a,0x2a,0x12], [0x14,0x22,0x0c], [0x0e,0x1a,0x08]],
    foothills: [0x0c,0x14,0x06],
    haze:      [20,30,15],
    mountainVisible: 1
  },
  expanse: {
    wallColor:  '#c07838',
    ceilFill:   '#5a3010',
    patternBase:'#7a4e22',
    wallBaseRGB: [180, 140, 100],
    floorBands: ['#1a0e08','#2e1808','#4a2a10','#6b3e1a','#8b5a28','#a87040','#c48a52','#d8a86a'],
    sky:     [[0x06,0x04,0x08], [0x0e,0x0a,0x06]],
    mountain:[[0x2a,0x1c,0x10], [0x1e,0x14,0x08], [0x14,0x0e,0x05]],
    foothills: [0x0e,0x0a,0x04],
    haze:      [50,30,12],
    mountainVisible: 1
  },
  ice: {
    wallColor:  '#7aa7ff',
    ceilFill:   '#0d1a2e',
    patternBase:'#0a1322',
    wallBaseRGB: [140, 170, 240],
    floorBands: ['#0a0e1a','#141c30','#1e2c48','#2a3c5e','#4a6888','#6888a8','#88a8c8','#a0c0e0'],
    sky:     [[0x05,0x07,0x0f], [0x0b,0x0d,0x12]],
    mountain:[[0x1a,0x25,0x40], [0x14,0x1c,0x35], [0x0e,0x14,0x28]],
    foothills: [0x0c,0x12,0x20],
    haze:      [15,20,35],
    mountainVisible: 1
  }
};

// Low-cost sky appearance data. The renderer interpolates these daylight
// anchors across biome borders, then applies one shared dawn/day/dusk/night
// curve. Keeping the colors here avoids branching art direction into the
// Canvas renderer and makes the sky independently tunable from terrain.
var SKY_ATMOSPHERE = {
  cave: {
    zenith:[10,12,18], mid:[16,16,22], horizon:[23,22,24], cloud:[52,52,58], cloudiness:0
  },
  ground: {
    zenith:[44,64,101], mid:[82,99,126], horizon:[154,129,103], cloud:[182,175,165], cloudiness:0.72
  },
  plains: {
    zenith:[52,78,124], mid:[98,121,151], horizon:[178,158,121], cloud:[205,198,182], cloudiness:0.58
  },
  forest: {
    zenith:[38,62,75], mid:[69,91,91], horizon:[126,126,96], cloud:[163,166,145], cloudiness:0.78
  },
  expanse: {
    zenith:[63,66,102], mid:[127,99,98], horizon:[205,134,79], cloud:[210,171,130], cloudiness:0.34
  },
  ice: {
    zenith:[42,73,126], mid:[91,128,166], horizon:[181,207,218], cloud:[218,230,235], cloudiness:0.66
  }
};

var SKY_TIME_COLORS = {
  nightZenith:[2,4,14], nightMid:[7,10,22], nightHorizon:[15,18,31],
  dawn:[236,132,78], dusk:[226,91,56],
  sunCore:[255,239,185], sunEdge:[255,177,83],
  moon:[207,218,226], star:[218,229,242]
};

// Floor height thresholds — shared by getFloorColor, maps heightPercent to band index
var _floorBandThresholds = [-0.9, -0.6, -0.3, -0.1, 0.1, 0.3, 0.5];

// =============================================
// SHARED OBJECT MATERIALS
// =============================================
// Author colors here as six-digit hex. The compiled read-only forms below are
// derived once at startup, not parsed/allocated for each painted face. Keep
// lighting, transparency and shape in the renderer, not in these base colors.
// Edit these definitions and reload the page: live theme switching would also
// have to rebuild authored chunk colors and cached artwork and is not provided.
var GAME_MATERIAL_COLORS = {
  missileMagic: {core:'#fff3d8', light:'#c4edf0', mid:'#79b9d0', deep:'#315677', rune:'#bfa16a'},
  casterSkin: {shadow:'#714b40', base:'#b98265', light:'#e2bb94', crease:'#755047', nail:'#d9b59c'},
  casterCloth: {deep:'#16242e', mid:'#344b58', lit:'#607a83', cuff:'#4c3c2c', linen:'#c4b493'},
  casterMetal: {shadow:'#50422c', base:'#a78b54', light:'#dfc78d'},
  // Ashen Reliquary interior stone. `base` remains the compatibility/fallback
  // color; the secondary roles are blended softly into world-authored albedo.
  caveStone: {
    base:'#686156', damp:'#50595b', iron:'#73513d', worn:'#83755f'
  },
  entranceStone: {
    lit: '#a89787', mid: '#776859', dark: '#473d32', shadow: '#2a2320'
  },
  // Order also defines the stable swatch cycle for rubble/rock-pile variants.
  rubbleStone: {
    base: '#787060', shadow: '#686058', lit: '#888070', dark: '#504840'
  },
  crateWood: {
    base: '#8b5a2b', bracing: '#6b3a1b', interior: '#3a1a08', outline: '#4a2808'
  },
  palisadeWood: {lit: '#7a4c2a', mid: '#6b4226', dark: '#5a3720', deep: '#3a2412'},
  // Constructed wall props share one compact material vocabulary. These are
  // flat authored colors; the renderer applies its existing face brightness
  // and distance fade without gradients, filters or per-pixel effects.
  wallPropWood: {deep:'#2b1b12', shadow:'#4a2f1f', base:'#7c5030', lit:'#ad7a48'},
  wallPropIron: {deep:'#22252a', shadow:'#3f444b', base:'#737b84', lit:'#b9c1c6', edge:'#e1d7bc'},
  wallHeraldry: {
    redDeep:'#47151a', red:'#8f2830', redLit:'#c34a48',
    blueDeep:'#172a45', blue:'#315b83', blueLit:'#5d86a6',
    purpleDeep:'#2f1c43', purple:'#684184', purpleLit:'#9870ad',
    gold:'#b58a35', goldLit:'#e0bf69', linen:'#d8cfb2'
  },
  wallFlame: {ember:'#8f2416', outer:'#e55220', inner:'#f6a329', core:'#fff1a6'},
  floorPropWood: {deep:'#2f2118', shadow:'#4c3424', base:'#765238', lit:'#a27950', cut:'#b99a72'},
  floorPropIron: {deep:'#24272b', shadow:'#3d4247', base:'#656c72', lit:'#a2a9ac', rust:'#80513a'},
  floorFoliage: {deep:'#243d20', shadow:'#35562b', base:'#52763d', lit:'#779657', dry:'#77613a'},
  bone: {
    base: '#c8c8c8', dry: '#c8b870', lit: '#e8e0c8', shadow: '#d0c8b0',
    outline: '#2a2018', cavity: '#000000', gap: '#1a1008',
    aged: '#c0b8a0', knuckle: '#b8b098'
  }
};

// Pure compiler; safe to test without a DOM or Canvas. Invalid color values
// fail at startup rather than silently inheriting a previous canvas color.
function compileGameMaterials(definitions) {
  if (!definitions || typeof definitions !== 'object' || Array.isArray(definitions))
    throw new Error('Material definitions must be an object');
  var materials = Object.create(null);
  Object.keys(definitions).forEach(function(id) {
    var colors = definitions[id];
    if (!colors || typeof colors !== 'object' || Array.isArray(colors) || !Object.keys(colors).length)
      throw new Error('Material ' + id + ' needs named colors');
    var hex = Object.create(null), rgb = Object.create(null), packed = Object.create(null), swatches = [];
    Object.keys(colors).forEach(function(role) {
      var color = colors[role];
      if (typeof color !== 'string' || !/^#[0-9a-fA-F]{6}$/.test(color))
        throw new Error('Invalid material color: ' + id + '.' + role);
      var value = parseInt(color.slice(1), 16);
      hex[role] = color;
      packed[role] = value;
      rgb[role] = Object.freeze([(value >> 16) & 255, (value >> 8) & 255, value & 255]);
      swatches.push(color);
    });
    materials[id] = Object.freeze({hex: Object.freeze(hex), rgb: Object.freeze(rgb),
      packed: Object.freeze(packed), swatches: Object.freeze(swatches)});
  });
  return Object.freeze(materials);
}
var GAME_MATERIALS = compileGameMaterials(GAME_MATERIAL_COLORS);

// Stable semantic roles shared by the cave floor, wall and ceiling renderers.
// They alter only the lit presentation of one authored stone albedo; geometry,
// depth, portals and the exterior cap material remain unchanged.
var CAVE_SURFACE_FLOOR = 0, CAVE_SURFACE_WALL = 1, CAVE_SURFACE_CEILING = 2;

function _caveStoneHash(worldX, worldY, scale, salt) {
  var x = Math.floor(worldX / scale), y = Math.floor(worldY / scale);
  var h = (Math.imul(x, 374761393) + Math.imul(y, 668265263) + salt) | 0;
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  return (h ^ (h >>> 16)) >>> 0;
}

function _mixPackedMaterial(a, b, amount) {
  var ar = (a >>> 16) & 255, ag = (a >>> 8) & 255, ab = a & 255;
  var br = (b >>> 16) & 255, bg = (b >>> 8) & 255, bb = b & 255;
  var r = Math.round(ar + (br - ar) * amount);
  var g = Math.round(ag + (bg - ag) * amount);
  var blue = Math.round(ab + (bb - ab) * amount);
  return (r << 16) | (g << 8) | blue;
}

// Generation-time, allocation-free stone authoring. Fine grain is quiet at
// the 12-unit mesh cadence while broader 48/96-unit fields form natural slabs
// instead of a high-contrast checkerboard. No camera, seed or random state is
// consulted, so streaming-window rebases cannot make the material crawl.
function sampleInteriorStoneAlbedo(basePacked, worldX, worldY) {
  var stone = GAME_MATERIALS.caveStone;
  var base = Number.isFinite(basePacked) ? basePacked >>> 0 : stone.packed.base;
  var variant = _caveStoneHash(worldX, worldY, 96, 0x51ed270b) % 100;
  var target = base, mix = 0;
  if (variant >= 96) { target = stone.packed.worn; mix = 0.22; }
  else if (variant >= 90) { target = stone.packed.iron; mix = 0.18; }
  else if (variant >= 72) { target = stone.packed.damp; mix = 0.20; }
  var packed = mix ? _mixPackedMaterial(base, target, mix) : base;
  var fine = (_caveStoneHash(worldX, worldY, 12, 0x1b873593) % 5) - 2;
  var broad = (_caveStoneHash(worldX, worldY, 48, 0x7f4a7c15) % 7) - 3;
  var r = Math.max(0, Math.min(255, ((packed >>> 16) & 255) + fine + broad));
  var g = Math.max(0, Math.min(255, ((packed >>> 8) & 255) + fine + broad));
  var b = Math.max(0, Math.min(255, (packed & 255) + fine + broad));
  return (r << 16) | (g << 8) | b;
}

// Hot-path presentation shared by every interior plane. It returns packed RGB
// so callers can use the existing rgbQ cache without allocating arrays or CSS
// strings. The floor is the navigation plane, walls carry the mid values, and
// the ceiling stays dark; local lights warm all three consistently.
function shadeCaveSurfaceColor(material, role, ambient, pointLight, fog, occlusion) {
  if (!Number.isFinite(material)) material = GAME_MATERIALS.caveStone.packed.base;
  if (role !== CAVE_SURFACE_FLOOR && role !== CAVE_SURFACE_CEILING) role = CAVE_SURFACE_WALL;
  ambient = Number.isFinite(ambient) ? Math.max(0, ambient) : 0;
  pointLight = Number.isFinite(pointLight) ? Math.max(0, pointLight) : 0;
  fog = Number.isFinite(fog) ? Math.max(0, Math.min(1, fog)) : 1;
  occlusion = Number.isFinite(occlusion) ? Math.max(0, Math.min(1, occlusion)) : 1;
  var pointScale = role === CAVE_SURFACE_CEILING ? 0.62 : 1;
  var light = Math.min(1, ambient + pointLight * pointScale);
  var value = role === CAVE_SURFACE_FLOOR ? 1.06 : role === CAVE_SURFACE_CEILING ? 0.74 : 1;
  var redRole = role === CAVE_SURFACE_FLOOR ? 1.03 : role === CAVE_SURFACE_CEILING ? 0.94 : 1;
  var greenRole = role === CAVE_SURFACE_FLOOR ? 1.01 : role === CAVE_SURFACE_CEILING ? 0.98 : 1;
  var blueRole = role === CAVE_SURFACE_FLOOR ? 0.97 : role === CAVE_SURFACE_CEILING ? 1.03 : 1;
  var warmR = 1 + pointLight * 0.28, warmG = 1 + pointLight * 0.06;
  var warmB = Math.max(0.72, 1 - pointLight * 0.17);
  var r = Math.max(0, Math.min(255, Math.floor(((material >>> 16) & 255) * light * value * fog * occlusion * redRole * warmR)));
  var g = Math.max(0, Math.min(255, Math.floor(((material >>> 8) & 255) * light * value * fog * occlusion * greenRole * warmG)));
  var b = Math.max(0, Math.min(255, Math.floor((material & 255) * light * value * fog * occlusion * blueRole * warmB)));
  return (r << 16) | (g << 8) | b;
}
