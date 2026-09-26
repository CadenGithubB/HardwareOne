// =============================================
// SECTION 1: UTILITIES
// =============================================

function clamp(v, min, max) {
  return v < min ? min : (v > max ? max : v);
}

function deg2rad(d) {
  return d * Math.PI / 180;
}

function angNorm(a) {
  while (a > Math.PI) a -= Math.PI * 2;
  while (a < -Math.PI) a += Math.PI * 2;
  return a;
}

function seedFor(component) {
  return (BASE_SEED + level * 101 + component) | 0;
}

function rectsOverlap(a, b) {
  return !(a.x + a.w < b.x || a.x > b.x + b.w || a.y + a.h < b.y || a.y > b.y + b.h);
}


// =============================================
// SECTION 2: TERRAIN & PATTERNS
// =============================================

function makePattern(kind) {
  var oc = document.createElement('canvas');
  oc.width = 32; oc.height = 32;
  var c = oc.getContext('2d');
  // Preserve the four existing pattern recipes, including legacy fallback.
  var patternKind = (kind === 'ice' || kind === 'cave' || kind === 'expanse') ? kind : 'ground';
  var patternBase = BIOME_PALETTE[patternKind].patternBase;
  if (kind === 'ice') {
    c.fillStyle = patternBase; c.fillRect(0, 0, 32, 32);
    c.strokeStyle = 'rgba(170,210,255,0.35)'; c.lineWidth = 2;
    c.beginPath(); c.moveTo(0, 16); c.lineTo(32, 16); c.moveTo(16, 0); c.lineTo(16, 32); c.stroke();
    c.strokeStyle = 'rgba(120,190,255,0.18)';
    c.beginPath(); c.moveTo(0, 0); c.lineTo(32, 32); c.moveTo(32, 0); c.lineTo(0, 32); c.stroke();
    for (var i = 0; i < 14; i++) {
      var x = Math.random() * 32, y = Math.random() * 32, r = Math.random() * 0.9 + 0.3;
      c.fillStyle = 'rgba(210,235,255,' + (0.05 + Math.random() * 0.05) + ')';
      c.beginPath(); c.arc(x, y, r, 0, Math.PI * 2); c.fill();
    }
    var g = c.createLinearGradient(0, 0, 32, 32);
    g.addColorStop(0, 'rgba(255,255,255,0.02)'); g.addColorStop(1, 'rgba(255,255,255,0.00)');
    c.fillStyle = g; c.fillRect(0, 0, 32, 32);
  } else if (kind === 'cave') {
    c.fillStyle = patternBase; c.fillRect(0, 0, 32, 32);
    for (var i = 0; i < 32; i++) {
      var x = Math.random() * 32, y = Math.random() * 32, r = Math.random() * 1.4 + 0.5;
      var a = 0.08 + Math.random() * 0.12; var pick = Math.random();
      if (pick < 0.4) c.fillStyle = 'rgba(80,80,85,' + a + ')';
      else if (pick < 0.75) c.fillStyle = 'rgba(60,60,65,' + a + ')';
      else c.fillStyle = 'rgba(100,100,105,' + (a * 0.8) + ')';
      c.beginPath(); c.arc(x, y, r, 0, Math.PI * 2); c.fill();
    }
    for (var j = 0; j < 4; j++) {
      var w = 8 + Math.random() * 12, hgt = 8 + Math.random() * 12;
      var px = Math.random() * (32 - w), py = Math.random() * (32 - hgt);
      c.fillStyle = 'rgba(70,70,75,' + (0.03 + Math.random() * 0.04) + ')';
      c.fillRect(px, py, w, hgt);
    }
  } else if (kind === 'expanse') {
    // Cracked sandstone — warm ochre base with hairline fractures
    c.fillStyle = patternBase; c.fillRect(0, 0, 32, 32);
    // Crack lines
    c.strokeStyle = 'rgba(30,15,5,0.55)'; c.lineWidth = 0.8;
    for (var i = 0; i < 6; i++) {
      var ax = Math.random() * 32, ay = Math.random() * 32;
      var bx = ax + (Math.random() - 0.5) * 18, by = ay + (Math.random() - 0.5) * 18;
      c.beginPath(); c.moveTo(ax, ay); c.lineTo(bx, by); c.stroke();
    }
    // Sand grain speckle
    for (var i = 0; i < 24; i++) {
      var x = Math.random() * 32, y = Math.random() * 32, r = Math.random() * 1.0 + 0.3;
      var pick = Math.random();
      if (pick < 0.5)      c.fillStyle = 'rgba(160,100,40,0.14)';
      else if (pick < 0.8) c.fillStyle = 'rgba(200,140,60,0.10)';
      else                 c.fillStyle = 'rgba(80,40,10,0.18)';
      c.beginPath(); c.arc(x, y, r, 0, Math.PI * 2); c.fill();
    }
  } else {
    c.fillStyle = patternBase; c.fillRect(0, 0, 32, 32);
    for (var i = 0; i < 28; i++) {
      var x = Math.random() * 32, y = Math.random() * 32, r = Math.random() * 1.6 + 0.6;
      var a = 0.10 + Math.random() * 0.10; var pick = Math.random();
      if (pick < 0.5) c.fillStyle = 'rgba(124,93,60,' + a + ')';
      else if (pick < 0.8) c.fillStyle = 'rgba(98,72,45,' + a + ')';
      else c.fillStyle = 'rgba(160,120,80,' + (a * 0.9) + ')';
      c.beginPath(); c.arc(x, y, r, 0, Math.PI * 2); c.fill();
    }
    for (var j = 0; j < 3; j++) {
      var w = 10 + Math.random() * 14, hgt = 10 + Math.random() * 14;
      var px = Math.random() * (32 - w), py = Math.random() * (32 - hgt);
      c.fillStyle = 'rgba(255,230,200,' + (0.02 + Math.random() * 0.025) + ')';
      c.fillRect(px, py, w, hgt);
    }
  }
  return ctx.createPattern(oc, 'repeat');
}

function makeCeilPattern(kind) {
  var oc = document.createElement('canvas');
  oc.width = 32; oc.height = 32;
  var c = oc.getContext('2d');
  c.fillStyle = (BIOME_PALETTE[kind] || BIOME_PALETTE.ground).ceilFill;
  c.fillRect(0, 0, 32, 32);
  c.fillStyle = 'rgba(255,255,255,0.02)'; c.fillRect(0, 0, 16, 16);
  return ctx.createPattern(oc, 'repeat');
}

function makeWallColor(kind) {
  return (BIOME_PALETTE[kind] || BIOME_PALETTE.ground).wallColor;
}

function applyPreset() {
  var tp = GAME_CONFIG.terrainPhysics[terrain] || GAME_CONFIG.terrainPhysics.ground;
  damping = tp.damping; bounce = tp.bounce; dead = tp.deadzone;
  var k = (terrain === 'ice') ? 'ice' : (terrain === 'cave') ? 'cave' : (terrain === 'expanse') ? 'expanse' : 'ground';
  bgPattern = makePattern(k);
  ceilPattern = makeCeilPattern(k);
  wallColor = makeWallColor(k);
}

