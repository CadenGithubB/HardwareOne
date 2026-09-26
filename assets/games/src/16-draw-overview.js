// =============================================
// SECTION 16: MAIN DRAW DISPATCH
// =============================================

// Top-down tiny icons for floor scatter items — called inside draw2D's translate block.
function drawFloorScatter2D() {
  if (!floorScatter || !floorScatter.length) return;
  ctx.save();
  for (var i = 0; i < floorScatter.length; i++) {
    var item = floorScatter[i];
    var x = item.x, y = item.y;
    var t = item.type;
    ctx.globalAlpha = 0.72;
    if (t === 'bones' || t === 'dry_bones') {
      ctx.strokeStyle = (t === 'dry_bones') ? '#c0a850' : '#aaaaaa';
      ctx.lineWidth = 1.5; ctx.lineCap = 'round';
      ctx.beginPath(); ctx.moveTo(x-4, y-2); ctx.lineTo(x+4, y+2); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(x+3, y-3); ctx.lineTo(x-3, y+3); ctx.stroke();
    } else if (t === 'crate') {
      ctx.fillStyle = '#8b5a2b'; ctx.fillRect(x-3, y-3, 6, 6);
      ctx.strokeStyle = '#4a2808'; ctx.lineWidth = 0.8; ctx.strokeRect(x-3, y-3, 6, 6);
    } else if (t === 'skull') {
      ctx.fillStyle = '#c8c0a8'; ctx.beginPath(); ctx.arc(x, y, 3.5, 0, Math.PI*2); ctx.fill();
      ctx.fillStyle = '#1a1008'; ctx.beginPath(); ctx.arc(x-1.2, y-0.5, 0.9, 0, Math.PI*2); ctx.fill();
      ctx.beginPath(); ctx.arc(x+1.2, y-0.5, 0.9, 0, Math.PI*2); ctx.fill();
    } else if (t === 'rubble' || t === 'rock_pile' || t === 'desert_rock') {
      ctx.fillStyle = (t === 'desert_rock') ? '#b8905a' : '#787060';
      ctx.beginPath(); ctx.arc(x-2, y+1, 2.2, 0, Math.PI*2); ctx.fill();
      ctx.fillStyle = (t === 'desert_rock') ? '#c8a070' : '#888070';
      ctx.beginPath(); ctx.arc(x+2, y-1, 2.0, 0, Math.PI*2); ctx.fill();
      ctx.beginPath(); ctx.arc(x, y+2, 1.6, 0, Math.PI*2); ctx.fill();
    } else if (t === 'ice_shard') {
      ctx.fillStyle = 'rgba(140,200,255,0.75)';
      ctx.beginPath(); ctx.moveTo(x-2,y+2); ctx.lineTo(x+2,y+2); ctx.lineTo(x,y-4); ctx.closePath(); ctx.fill();
      ctx.beginPath(); ctx.moveTo(x+2,y+1); ctx.lineTo(x+4,y+1); ctx.lineTo(x+3,y-3); ctx.closePath(); ctx.fill();
    } else if (t === 'frozen_pool') {
      ctx.fillStyle = 'rgba(100,160,255,0.45)';
      ctx.beginPath(); ctx.ellipse(x, y, 5, 2.5, 0, 0, Math.PI*2); ctx.fill();
    } else if (t === 'crystal') {
      ctx.fillStyle = (terrain === 'expanse' || terrain === 'plains') ? '#c8a000' : '#00c8a8';
      ctx.beginPath(); ctx.moveTo(x,y-5); ctx.lineTo(x+2,y); ctx.lineTo(x,y+1); ctx.lineTo(x-2,y); ctx.closePath(); ctx.fill();
    } else if (t === 'stalagmite') {
      ctx.fillStyle = '#6a5848';
      ctx.beginPath(); ctx.moveTo(x-2,y+2); ctx.lineTo(x+2,y+2); ctx.lineTo(x,y-4); ctx.closePath(); ctx.fill();
    } else if (t === 'puddle') {
      ctx.fillStyle = 'rgba(15,22,35,0.65)';
      ctx.beginPath(); ctx.ellipse(x, y, 5, 2, 0, 0, Math.PI*2); ctx.fill();
    } else if (t === 'dead_shrub') {
      ctx.strokeStyle = '#6a4820'; ctx.lineWidth = 1.2; ctx.lineCap = 'round';
      ctx.beginPath(); ctx.moveTo(x, y+3); ctx.lineTo(x, y-3); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(x, y-1); ctx.lineTo(x-3, y-3); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(x, y-1); ctx.lineTo(x+3, y-3); ctx.stroke();
    } else if (t === 'icicle_cluster') {
      ctx.fillStyle = 'rgba(160,210,255,0.7)';
      ctx.beginPath(); ctx.moveTo(x-2,y+2); ctx.lineTo(x,y-4); ctx.lineTo(x+2,y+2); ctx.closePath(); ctx.fill();
    } else if (t === 'frost_patch') {
      ctx.fillStyle = 'rgba(180,220,255,0.35)';
      ctx.beginPath(); ctx.ellipse(x, y, 4, 2, 0, 0, Math.PI*2); ctx.fill();
    } else if (t === 'frozen_skull') {
      ctx.fillStyle = 'rgba(140,200,255,0.4)'; ctx.beginPath(); ctx.arc(x, y, 3.5, 0, Math.PI*2); ctx.fill();
      ctx.fillStyle = '#b8b0a0'; ctx.beginPath(); ctx.arc(x, y, 2, 0, Math.PI*2); ctx.fill();
    } else if (t === 'tall_grass') {
      ctx.strokeStyle = '#5a7a3a'; ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(x-2, y+2); ctx.lineTo(x-1, y-3); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(x, y+2); ctx.lineTo(x+1, y-4); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(x+2, y+2); ctx.lineTo(x+3, y-3); ctx.stroke();
    } else if (t === 'wildflower') {
      ctx.fillStyle = '#d84040'; ctx.beginPath(); ctx.arc(x, y-2, 2, 0, Math.PI*2); ctx.fill();
      ctx.strokeStyle = '#4a6830'; ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(x, y-2); ctx.lineTo(x, y+3); ctx.stroke();
    } else if (t === 'stone_marker') {
      ctx.fillStyle = '#707868'; ctx.fillRect(x-2, y-4, 4, 6);
    } else if (t === 'barrel') {
      ctx.fillStyle = '#7a5230'; ctx.beginPath(); ctx.arc(x, y, 3, 0, Math.PI*2); ctx.fill();
      ctx.strokeStyle = '#555'; ctx.lineWidth = 0.8; ctx.beginPath(); ctx.arc(x, y, 3, 0, Math.PI*2); ctx.stroke();
    } else if (t === 'bookshelf_debris') {
      ctx.fillStyle = '#5a3e20'; ctx.fillRect(x-3, y-1, 6, 2);
      ctx.fillStyle = '#8b2020'; ctx.fillRect(x-2, y-3, 2, 3);
      ctx.fillStyle = '#1a4a6a'; ctx.fillRect(x+1, y-2, 2, 2);
    } else if (t === 'iron_chain') {
      ctx.strokeStyle = '#707878'; ctx.lineWidth = 1.2;
      ctx.beginPath(); ctx.arc(x, y, 3, 0, Math.PI*1.5); ctx.stroke();
    } else if (t === 'boulder') {
      ctx.fillStyle = '#5a5550'; ctx.beginPath(); ctx.ellipse(x, y, 4, 2.5, 0, 0, Math.PI*2); ctx.fill();
    } else if (t === 'stone_column' || t === 'rock_spire') {
      ctx.fillStyle = '#605850'; ctx.fillRect(x-1.5, y-4, 3, 6);
    } else if (t === 'rock_arch') {
      ctx.strokeStyle = '#585048'; ctx.lineWidth = 1.5;
      ctx.beginPath(); ctx.arc(x, y-1, 4, Math.PI, 0); ctx.stroke();
    } else if (t === 'cave_rubble_pile') {
      ctx.fillStyle = '#504a44'; ctx.beginPath(); ctx.ellipse(x, y, 4, 2, 0, 0, Math.PI*2); ctx.fill();
    } else if (t === 'mushroom') {
      ctx.fillStyle = '#8b3020'; ctx.beginPath(); ctx.arc(x-1, y-1, 2, 0, Math.PI*2); ctx.fill();
      ctx.fillStyle = '#a04030'; ctx.beginPath(); ctx.arc(x+2, y, 1.5, 0, Math.PI*2); ctx.fill();
    } else if (t === 'fern') {
      ctx.strokeStyle = '#3a6a28'; ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(x-3, y-2); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(x+3, y-2); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(x, y-3); ctx.stroke();
    } else if (t === 'leaf_pile') {
      ctx.fillStyle = 'rgba(170,100,30,0.6)'; ctx.beginPath(); ctx.ellipse(x, y, 4, 2, 0, 0, Math.PI*2); ctx.fill();
    } else if (t === 'moss_patch') {
      ctx.fillStyle = 'rgba(60,120,40,0.4)'; ctx.beginPath(); ctx.ellipse(x, y, 4, 2, 0, 0, Math.PI*2); ctx.fill();
    } else if (t === 'tree_stump') {
      ctx.fillStyle = '#5a3e28'; ctx.beginPath(); ctx.arc(x, y, 3, 0, Math.PI*2); ctx.fill();
      ctx.strokeStyle = '#3a2818'; ctx.lineWidth = 0.8; ctx.beginPath(); ctx.arc(x, y, 2, 0, Math.PI*2); ctx.stroke();
    } else if (t === 'fallen_log') {
      ctx.fillStyle = '#5a4535'; ctx.beginPath(); ctx.ellipse(x, y, 5, 1.5, 0.3, 0, Math.PI*2); ctx.fill();
    }
  }
  ctx.globalAlpha = 1.0; ctx.restore();
}

// Tiny top-down icons for wall decorations — called inside draw2D's translate block.
function drawWallDecorations2D() {
  if (!wallDecorations || !wallDecorations.length) return;
  ctx.save(); ctx.globalAlpha = 0.68;
  for (var i = 0; i < wallDecorations.length; i++) {
    var dec = wallDecorations[i];
    var x = dec.worldX, y = dec.worldY;
    var t = canonicalWallDecorationType(dec.type);
    if (t === 'torch' || t === 'sconce') {
      ctx.fillStyle = (t === 'torch') ? '#ff8020' : '#ff5030';
      ctx.beginPath(); ctx.arc(x, y, 2.5, 0, Math.PI*2); ctx.fill();
    } else if (t === 'shield') {
      ctx.fillStyle = '#b0b0b0';
      ctx.beginPath(); ctx.ellipse(x, y, 3.5, 2.5, 0, 0, Math.PI*2); ctx.fill();
    } else if (t === 'banner') {
      ctx.fillStyle = '#800080';
      ctx.fillRect(x-1.5, y-3, 3, 5);
    } else if (t === 'fungi') {
      ctx.fillStyle = '#00c8a8';
      ctx.beginPath(); ctx.arc(x, y, 2.5, 0, Math.PI*2); ctx.fill();
    } else if (t === 'moss_drip') {
      ctx.fillStyle = '#3a7a28';
      ctx.fillRect(x-1, y-2, 2, 4);
    } else if (t === 'stalactite_tip') {
      ctx.fillStyle = '#6a5040';
      ctx.beginPath(); ctx.moveTo(x, y+2); ctx.lineTo(x-2,y-2); ctx.lineTo(x+2,y-2); ctx.closePath(); ctx.fill();
    } else if (t === 'icicle') {
      ctx.fillStyle = 'rgba(140,200,255,0.7)';
      ctx.beginPath(); ctx.moveTo(x, y+3); ctx.lineTo(x-2,y-2); ctx.lineTo(x+2,y-2); ctx.closePath(); ctx.fill();
    } else if (t === 'frost_crystal') {
      ctx.strokeStyle = 'rgba(180,225,255,0.6)'; ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(x-3,y); ctx.lineTo(x+3,y); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(x,y-2); ctx.lineTo(x,y+2); ctx.stroke();
    } else if (t === 'vine_growth') {
      ctx.fillStyle = '#3a7a28';
      ctx.beginPath(); ctx.ellipse(x, y, 2.5, 1.5, 0, 0, Math.PI*2); ctx.fill();
    } else if (t === 'carved_rune') {
      ctx.strokeStyle = 'rgba(180,160,120,0.6)'; ctx.lineWidth = 1;
      ctx.beginPath(); ctx.arc(x, y, 2.5, 0, Math.PI*2); ctx.stroke();
    } else if (t === 'wall_crack') {
      ctx.strokeStyle = 'rgba(42,34,30,0.75)'; ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(x-2,y-3); ctx.lineTo(x,y-1); ctx.lineTo(x-1,y+1); ctx.lineTo(x+2,y+3); ctx.stroke();
    }
  }
  ctx.globalAlpha = 1.0; ctx.restore();
}

// =============================================
// DEBUG OVERVIEW — full zoomed-out map render
// =============================================
var overviewActive = false;

function drawDebugOverview() {
  if (!grid || !gridW) return;
  var cw = canvas.width, ch = canvas.height;
  var scaleX = cw / worldW, scaleY = ch / worldH;
  var scale = Math.min(scaleX, scaleY) * 0.97;
  var offX = (cw - worldW * scale) * 0.5;
  var offY = (ch - worldH * scale) * 0.5;

  function wx(x) { return offX + x * scale; }
  function wy(y) { return offY + y * scale; }
  function ws(s) { return Math.max(1, s * scale); }

  ctx.clearRect(0, 0, cw, ch);
  ctx.fillStyle = '#0a0a0a';
  ctx.fillRect(0, 0, cw, ch);

  // --- 1. Grid cells: wall vs floor, height-tinted ---
  var cellS = ws(cell);
  var mesh = floorMesh;
  var meshGs = mesh ? mesh.gridSize : 12;
  for (var gy = 0; gy < gridH; gy++) {
    for (var gx = 0; gx < gridW; gx++) {
      var isWall = grid[gy * gridW + gx];
      // Sample floor mesh height for brightness modulation
      var heightBright = 0;
      var isWater = false;
      if (mesh && mesh.heights) {
        var mmx = Math.floor((gx * cell + cell * 0.5) / meshGs);
        var mmy = Math.floor((gy * cell + cell * 0.5) / meshGs);
        mmx = Math.min(mmx, mesh.w - 1); mmy = Math.min(mmy, mesh.h - 1);
        var mi = mmy * mesh.w + mmx;
        heightBright = mesh.heights[mi]; // typically -3 to +2.5
        if (mesh.water && mesh.water[mi]) isWater = true;
      }
      // Map height to brightness offset: -3 → dark, +2.5 → bright
      var bright = Math.floor(heightBright * 18); // range ~-54 to +45
      if (isWater) {
        var wb = 20 + Math.max(0, bright + 30);
        ctx.fillStyle = rgbQ(wb, Math.floor(wb * 1.4), Math.floor(wb * 2.2));
      } else if (isWall) {
        var wr = Math.max(10, 42 + bright), wg = Math.max(8, 32 + bright), wb2 = Math.max(12, 48 + bright);
        ctx.fillStyle = rgbQ(wr, wg, wb2);
      } else {
        var fr = Math.max(10, 74 + bright), fg = Math.max(8, 60 + bright), fb = Math.max(6, 40 + bright);
        ctx.fillStyle = rgbQ(fr, fg, fb);
      }
      ctx.fillRect(wx(gx * cell), wy(gy * cell), Math.max(1, cellS), Math.max(1, cellS));
    }
  }

  // --- 2. Border polygon (playable boundary) ---
  if (currentBorderPoly && currentBorderPoly.length > 2) {
    ctx.save();
    ctx.strokeStyle = '#88aaff';
    ctx.lineWidth = 2;
    ctx.setLineDash([4, 3]);
    ctx.beginPath();
    ctx.moveTo(wx(currentBorderPoly[0].x), wy(currentBorderPoly[0].y));
    for (var pi = 1; pi < currentBorderPoly.length; pi++) {
      ctx.lineTo(wx(currentBorderPoly[pi].x), wy(currentBorderPoly[pi].y));
    }
    ctx.closePath();
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.restore();
  }

  // --- 3. Deep cave regions (corridors + chambers) ---
  if (deepCaveRegions && deepCaveRegions.length) {
    for (var ci = 0; ci < deepCaveRegions.length; ci++) {
      var cr = deepCaveRegions[ci];
      var isChamber = cr.type === 'chamber';
      var dx = cr.x2 - cr.x1, dy = cr.y2 - cr.y1;
      var len = Math.hypot(dx, dy) || 1;
      var perpX = -dy / len, perpY = dx / len;
      var hw = ws(cr.width * 0.5);
      ctx.beginPath();
      ctx.moveTo(wx(cr.x1 + perpX * cr.width * 0.5), wy(cr.y1 + perpY * cr.width * 0.5));
      ctx.lineTo(wx(cr.x2 + perpX * cr.width * 0.5), wy(cr.y2 + perpY * cr.width * 0.5));
      ctx.lineTo(wx(cr.x2 - perpX * cr.width * 0.5), wy(cr.y2 - perpY * cr.width * 0.5));
      ctx.lineTo(wx(cr.x1 - perpX * cr.width * 0.5), wy(cr.y1 - perpY * cr.width * 0.5));
      ctx.closePath();
      ctx.fillStyle = isChamber ? 'rgba(120,60,200,0.55)' : 'rgba(60,120,200,0.45)';
      ctx.fill();
      ctx.strokeStyle = isChamber ? '#cc88ff' : '#44aaff';
      ctx.lineWidth = 1;
      ctx.stroke();
    }
  }

  // --- 4. Cave chambers (circle overlays) ---
  if (deepCaveChambers && deepCaveChambers.length) {
    for (var chi = 0; chi < deepCaveChambers.length; chi++) {
      var ch2 = deepCaveChambers[chi];
      ctx.beginPath();
      ctx.arc(wx(ch2.cx), wy(ch2.cy), ws(ch2.radius), 0, Math.PI * 2);
      ctx.fillStyle = 'rgba(160,80,255,0.3)';
      ctx.fill();
      ctx.strokeStyle = '#cc44ff';
      ctx.lineWidth = 1.5;
      ctx.stroke();
    }
  }

  // --- 4b. Endless cave networks (corridors + chambers) ---
  for (var _eck2 in endlessCaveNetworks) {
    var _ecn2 = endlessCaveNetworks[_eck2];
    if (!_ecn2 || !_ecn2.corridors) continue;
    // Convert world coords to window-local for rendering
    for (var _eci = 0; _eci < _ecn2.corridors.length; _eci++) {
      var _ecc = _ecn2.corridors[_eci];
      var _eclx1 = _ecc.x1 - windowOriginX, _ecly1 = _ecc.y1 - windowOriginY;
      var _eclx2 = _ecc.x2 - windowOriginX, _ecly2 = _ecc.y2 - windowOriginY;
      // Skip if outside view
      if (_eclx1 < -200 && _eclx2 < -200) continue;
      if (_eclx1 > worldW + 200 && _eclx2 > worldW + 200) continue;
      if (_ecly1 < -200 && _ecly2 < -200) continue;
      if (_ecly1 > worldH + 200 && _ecly2 > worldH + 200) continue;
      var _ecdx = _eclx2 - _eclx1, _ecdy = _ecly2 - _ecly1;
      var _eclen = Math.hypot(_ecdx, _ecdy) || 1;
      var _ecpx = -_ecdy / _eclen, _ecpy = _ecdx / _eclen;
      ctx.beginPath();
      ctx.moveTo(wx(_eclx1 + _ecpx * _ecc.width * 0.5), wy(_ecly1 + _ecpy * _ecc.width * 0.5));
      ctx.lineTo(wx(_eclx2 + _ecpx * _ecc.width * 0.5), wy(_ecly2 + _ecpy * _ecc.width * 0.5));
      ctx.lineTo(wx(_eclx2 - _ecpx * _ecc.width * 0.5), wy(_ecly2 - _ecpy * _ecc.width * 0.5));
      ctx.lineTo(wx(_eclx1 - _ecpx * _ecc.width * 0.5), wy(_ecly1 - _ecpy * _ecc.width * 0.5));
      ctx.closePath();
      ctx.fillStyle = 'rgba(60,120,200,0.45)';
      ctx.fill();
      ctx.strokeStyle = '#44aaff';
      ctx.lineWidth = 1;
      ctx.stroke();
    }
    for (var _echi = 0; _echi < _ecn2.chambers.length; _echi++) {
      var _ech = _ecn2.chambers[_echi];
      var _echlx = _ech.cx - windowOriginX, _echly = _ech.cy - windowOriginY;
      if (_echlx < -200 || _echlx > worldW + 200 || _echly < -200 || _echly > worldH + 200) continue;
      ctx.beginPath();
      ctx.arc(wx(_echlx), wy(_echly), ws(_ech.radius), 0, Math.PI * 2);
      ctx.fillStyle = 'rgba(160,80,255,0.3)';
      ctx.fill();
      ctx.strokeStyle = '#cc44ff';
      ctx.lineWidth = 1.5;
      ctx.stroke();
    }
  }

  // --- 5. Ore veins ---
  if (oreVeins && oreVeins.length) {
    for (var oi = 0; oi < oreVeins.length; oi++) {
      var ov = oreVeins[oi];
      ctx.beginPath();
      ctx.arc(wx(ov.worldX || ov.x), wy(ov.worldY || ov.y), Math.max(2, ws(12)), 0, Math.PI * 2);
      ctx.fillStyle = ov.veinType === 'teal' ? '#44ffcc' : '#ffcc44';
      ctx.globalAlpha = 0.7;
      ctx.fill();
      ctx.globalAlpha = 1.0;
    }
  }

  // --- 5b. Treasure chests ---
  if (treasureChests && treasureChests.length) {
    for (var tci = 0; tci < treasureChests.length; tci++) {
      var tc = treasureChests[tci];
      if (tc.collected) continue;
      var tcx = wx(tc.x), tcy = wy(tc.y);
      // Glow halo
      ctx.globalAlpha = 0.35;
      ctx.fillStyle = '#ffd700';
      ctx.beginPath(); ctx.arc(tcx, tcy, 8, 0, Math.PI * 2); ctx.fill();
      // Chest icon — larger gold dot with dark outline
      ctx.globalAlpha = 1.0;
      ctx.fillStyle = '#ffd700';
      ctx.beginPath(); ctx.arc(tcx, tcy, 4, 0, Math.PI * 2); ctx.fill();
      ctx.strokeStyle = '#8b5e2b';
      ctx.lineWidth = 1.5;
      ctx.stroke();
    }
  }

  // --- 5c. Enemy spawners ---
  if (enemySpawners && enemySpawners.length) {
    for (var esi2 = 0; esi2 < enemySpawners.length; esi2++) {
      var es = enemySpawners[esi2];
      if (!es.active) continue;
      var esx = wx(es.x), esy = wy(es.y);
      // Red glow
      ctx.globalAlpha = 0.3;
      ctx.fillStyle = '#ff2244';
      ctx.beginPath(); ctx.arc(esx, esy, 10, 0, Math.PI * 2); ctx.fill();
      // Diamond shape
      ctx.globalAlpha = 0.95;
      ctx.fillStyle = '#cc2244';
      ctx.beginPath();
      ctx.moveTo(esx, esy - 7);
      ctx.lineTo(esx + 5, esy);
      ctx.lineTo(esx, esy + 7);
      ctx.lineTo(esx - 5, esy);
      ctx.closePath(); ctx.fill();
      ctx.strokeStyle = '#ff4466';
      ctx.lineWidth = 1.5;
      ctx.stroke();
      ctx.globalAlpha = 1.0;
    }
  }

  // --- 5d. Shrines ---
  if (shrines && shrines.length) {
    for (var shi = 0; shi < shrines.length; shi++) {
      var sh = shrines[shi];
      var shx = wx(sh.x), shy = wy(sh.y);
      var shCol = sh.buffType === 'damage' ? '#ff4444' : sh.buffType === 'speed' ? '#44ffff' : sh.buffType === 'regen' ? '#44ff44' : '#ffaa44';
      // Glow
      ctx.globalAlpha = sh.used ? 0.15 : 0.4;
      ctx.fillStyle = shCol;
      ctx.beginPath(); ctx.arc(shx, shy, 10, 0, Math.PI * 2); ctx.fill();
      // Diamond
      ctx.globalAlpha = sh.used ? 0.4 : 1.0;
      ctx.fillStyle = sh.used ? '#555555' : shCol;
      ctx.beginPath();
      ctx.moveTo(shx, shy - 6); ctx.lineTo(shx + 5, shy);
      ctx.lineTo(shx, shy + 6); ctx.lineTo(shx - 5, shy);
      ctx.closePath(); ctx.fill();
      ctx.strokeStyle = '#ffffff'; ctx.lineWidth = 1; ctx.stroke();
      ctx.globalAlpha = 1.0;
    }
  }

  // --- 5d2. Ruins ---
  if (ruins && ruins.length) {
    for (var rui = 0; rui < ruins.length; rui++) {
      var ruOv = ruins[rui];
      var rux = wx(ruOv.x), ruy = wy(ruOv.y);
      ctx.globalAlpha = 0.8;
      ctx.fillStyle = '#8a6a40';
      ctx.fillRect(rux - 5, ruy - 5, 10, 10);
      ctx.strokeStyle = '#5a4a30'; ctx.lineWidth = 1; ctx.strokeRect(rux - 5, ruy - 5, 10, 10);
      ctx.globalAlpha = 1.0;
    }
  }

  // --- 5d3. Large structures ---
  if (largeStructures && largeStructures.length) {
    for (var lsi = 0; lsi < largeStructures.length; lsi++) {
      var ls = largeStructures[lsi];
      var lsx = wx(ls.x), lsy = wy(ls.y);
      // Large glow halo
      ctx.globalAlpha = 0.25;
      ctx.fillStyle = '#ff8800';
      ctx.beginPath(); ctx.arc(lsx, lsy, 18, 0, Math.PI * 2); ctx.fill();
      // Star/cross icon
      ctx.globalAlpha = 0.9;
      ctx.strokeStyle = '#ffaa33';
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(lsx, lsy - 8); ctx.lineTo(lsx, lsy + 8);
      ctx.moveTo(lsx - 8, lsy); ctx.lineTo(lsx + 8, lsy);
      ctx.moveTo(lsx - 5, lsy - 5); ctx.lineTo(lsx + 5, lsy + 5);
      ctx.moveTo(lsx + 5, lsy - 5); ctx.lineTo(lsx - 5, lsy + 5);
      ctx.stroke();
      // Label
      ctx.globalAlpha = 1.0;
      ctx.fillStyle = '#ffaa33';
      ctx.font = 'bold 11px monospace';
      ctx.fillText((ls.type || 'STRUCTURE').toUpperCase(), lsx + 12, lsy + 4);
    }
  }

  // --- 5e. Market ---
  if (shopMarker) {
    var mkx = wx(shopMarker.x), mky = wy(shopMarker.y);
    ctx.globalAlpha = 0.35;
    ctx.fillStyle = '#ffd700';
    ctx.beginPath(); ctx.arc(mkx, mky, 12, 0, Math.PI * 2); ctx.fill();
    ctx.globalAlpha = 1.0;
    ctx.fillStyle = '#ffd700';
    ctx.fillRect(mkx - 5, mky - 5, 10, 10);
    ctx.strokeStyle = '#aa8800'; ctx.lineWidth = 1.5;
    ctx.strokeRect(mkx - 5, mky - 5, 10, 10);
    ctx.fillStyle = '#ffd700';
    ctx.font = 'bold 11px monospace';
    ctx.fillText('MARKET', mkx + 9, mky + 4);
  }

  // --- 6. Cave entrances ---
  if (deepCaveEntrances && deepCaveEntrances.length) {
    for (var ei = 0; ei < deepCaveEntrances.length; ei++) {
      var ent = deepCaveEntrances[ei];
      var ex2 = wx(ent.x), ey2 = wy(ent.y);
      ctx.beginPath();
      ctx.arc(ex2, ey2, 7, 0, Math.PI * 2);
      ctx.fillStyle = '#00ffcc';
      ctx.fill();
      ctx.strokeStyle = '#ffffff';
      ctx.lineWidth = 2;
      ctx.stroke();
      ctx.fillStyle = '#00ffcc';
      ctx.font = 'bold 11px monospace';
      ctx.fillText('CAVE', ex2 + 9, ey2 + 4);
    }
  }

  // --- 7. Goal ---
  if (goal) {
    ctx.fillStyle = '#00ff44';
    ctx.fillRect(wx(goal.x), wy(goal.y), Math.max(4, ws(goal.w)), Math.max(4, ws(goal.h)));
    ctx.strokeStyle = '#ffffff';
    ctx.lineWidth = 1.5;
    ctx.strokeRect(wx(goal.x), wy(goal.y), Math.max(4, ws(goal.w)), Math.max(4, ws(goal.h)));
  }

  // --- 8. Enemies ---
  if (enemies && enemies.length) {
    ctx.fillStyle = '#ff4444';
    for (var ei2 = 0; ei2 < enemies.length; ei2++) {
      var en = enemies[ei2];
      ctx.beginPath();
      ctx.arc(wx(en.x), wy(en.y), Math.max(2, ws(8)), 0, Math.PI * 2);
      ctx.fill();
    }
  }

  // --- 9. Player ---
  var px = wx(pos.x), py = wy(pos.y);
  ctx.beginPath();
  ctx.arc(px, py, 6, 0, Math.PI * 2);
  ctx.fillStyle = '#ffffff';
  ctx.fill();
  ctx.strokeStyle = '#ff0000';
  ctx.lineWidth = 2;
  ctx.stroke();
  // Facing direction arrow
  var arrowLen = 14;
  ctx.beginPath();
  ctx.moveTo(px, py);
  ctx.lineTo(px + Math.cos(cam.ang) * arrowLen, py + Math.sin(cam.ang) * arrowLen);
  ctx.strokeStyle = '#ff0000';
  ctx.lineWidth = 2;
  ctx.stroke();

  // --- 9b. Chunk grid overlay (endless mode) ---
  if (ENDLESS_MODE) {
    ctx.save();
    ctx.strokeStyle = 'rgba(255,255,100,0.25)';
    ctx.lineWidth = 1;
    ctx.setLineDash([4, 4]);
    for (var ci2 = 0; ci2 <= WINDOW_CHUNKS; ci2++) {
      var cx3 = ci2 * CHUNK_SIZE;
      ctx.beginPath(); ctx.moveTo(wx(cx3), wy(0)); ctx.lineTo(wx(cx3), wy(worldH)); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(wx(0), wy(cx3)); ctx.lineTo(wx(worldW), wy(cx3)); ctx.stroke();
    }
    ctx.setLineDash([]);
    // Biome labels per chunk
    ctx.font = '9px monospace'; ctx.textAlign = 'center'; ctx.globalAlpha = 0.6;
    for (var cdy = 0; cdy < WINDOW_CHUNKS; cdy++) {
      for (var cdx = 0; cdx < WINDOW_CHUNKS; cdx++) {
        var ck2 = (windowCX + cdx) + ',' + (windowCY + cdy);
        var ch3 = chunks[ck2];
        if (ch3) {
          var lcx = cdx * CHUNK_SIZE + CHUNK_SIZE / 2;
          var lcy = cdy * CHUNK_SIZE + CHUNK_SIZE / 2;
          var biomeColors = {cave:'#8888ff', ground:'#88ff88', plains:'#ffff88', expanse:'#ffaa44', ice:'#88ffff'};
          ctx.fillStyle = biomeColors[ch3.biome] || '#ffffff';
          ctx.fillText(ch3.biome, wx(lcx), wy(lcy - 18));
          ctx.fillText('d=' + ch3.difficulty.toFixed(1), wx(lcx), wy(lcy - 6));
          // Terrain feature numbers
          var _cGeo = geographyNoise(ch3.cx * CHUNK_SIZE + CHUNK_SIZE / 2, ch3.cy * CHUNK_SIZE + CHUNK_SIZE / 2);
          var _cWat = 0;
          if (ch3.floorMesh && ch3.floorMesh.water) {
            for (var _cwi = 0; _cwi < ch3.floorMesh.water.length; _cwi++) if (ch3.floorMesh.water[_cwi]) _cWat++;
          }
          ctx.fillStyle = '#aaaaaa';
          ctx.fillText('geo=' + _cGeo.toFixed(2) + ' w=' + _cWat, wx(lcx), wy(lcy + 6));
          if (ch3.structure) {
            ctx.fillStyle = '#e8c868';
            ctx.fillText(ch3.structure.type.toUpperCase(), wx(lcx), wy(lcy + 18));
          }
        }
      }
    }
    ctx.globalAlpha = 1.0;
    ctx.restore();
  }

  // --- 10. Stats panel ---
  // Geometry counts
  var _meshVerts = floorMesh ? (floorMesh.w * floorMesh.h) : 0;
  var _meshQuads = floorMesh ? ((floorMesh.w - 1) * (floorMesh.h - 1)) : 0;
  var _meshEdges = floorMesh ? (_meshQuads * 4) : 0;  // 4 edges per quad (shared)
  var _gridCells = gridW * gridH;
  var _wallCells = 0;
  if (grid) { for (var _wi = 0; _wi < grid.length; _wi++) { if (grid[_wi]) _wallCells++; } }
  var _corridorSegs = deepCaveRegions ? deepCaveRegions.filter(function(r){return r.type==='corridor';}).length : 0;
  var _chamberSegs = deepCaveRegions ? deepCaveRegions.filter(function(r){return r.type==='chamber';}).length : 0;
  // Count endless cave network segments too
  var _ecCorridors = 0, _ecChambers = 0, _ecNetworks = 0;
  for (var _eck in endlessCaveNetworks) {
    var _ecn = endlessCaveNetworks[_eck];
    if (_ecn && _ecn.corridors) {
      _ecNetworks++;
      _ecCorridors += _ecn.corridors.length;
      _ecChambers += _ecn.chambers.length;
    }
  }
  var _borderVarSegs = borderVariations ? borderVariations.length : 0;
  var _scatterItems = floorScatter ? floorScatter.length : 0;
  var _wallDecors = wallDecorations ? wallDecorations.length : 0;

  var stats = [
    'MAP: ' + worldW + ' x ' + worldH + '  (scale 1px=' + (1/scale).toFixed(1) + 'wu)',
    'GRID: ' + gridW + 'x' + gridH + ' (' + _gridCells + ' cells, ' + _wallCells + ' walls)',
    'MESH: ' + (floorMesh ? floorMesh.w + 'x' + floorMesh.h : '—') +
      '  verts=' + _meshVerts + '  quads=' + _meshQuads + '  edges=' + _meshEdges,
    'BORDER POLY: ' + (currentBorderPoly ? currentBorderPoly.length + ' verts' : 'none') +
      '  variations=' + _borderVarSegs,
    'CAVE SEGS: ' + (deepCaveRegions ? deepCaveRegions.length : 0) +
      ' (' + _corridorSegs + ' corr + ' + _chamberSegs + ' cham)' +
      '  chambers=' + (deepCaveChambers ? deepCaveChambers.length : 0) +
      (_ecNetworks ? '  | ENDLESS: ' + _ecNetworks + ' nets (' + _ecCorridors + ' corr + ' + _ecChambers + ' cham)' : ''),
    'CAVE ENTRANCES: ' + (deepCaveEntrances ? deepCaveEntrances.length : 0) +
      (deepCaveEntrances && deepCaveEntrances.length ? '  @ (' + Math.round(deepCaveEntrances[0].x) + ',' + Math.round(deepCaveEntrances[0].y) + ')' : ''),
    'ORE VEINS: ' + (oreVeins ? oreVeins.length : 0) +
      '  scatter=' + _scatterItems + '  wallDecors=' + _wallDecors,
    'CHESTS: ' + (treasureChests ? treasureChests.filter(function(c){return !c.collected}).length : 0) +
      '/' + (treasureChests ? treasureChests.length : 0) +
      '  SPAWNERS: ' + (enemySpawners ? enemySpawners.filter(function(s){return s.active}).length : 0) +
      '/' + (enemySpawners ? enemySpawners.length : 0),
    'ENEMIES: ' + (enemies ? enemies.length : 0),
    'SHRINES: ' + (shrines ? shrines.filter(function(s){return !s.used}).length : 0) +
      '/' + (shrines ? shrines.length : 0) +
      '  MARKETS: ' + (shopMarker ? 1 : 0) +
      '  STRUCTURES: ' + (largeStructures ? largeStructures.length : 0) +
      '  discovered: ' + discoveredMarkets.length + 'M ' + discoveredShrines.length + 'S',
    'PLAYER: (' + Math.round(pos.x) + ', ' + Math.round(pos.y) + ')',
    'TERRAIN: ' + terrain + '  |  LEVEL: ' + level,
  ];
  // Terrain feature stats (endless mode)
  if (ENDLESS_MODE && floorMesh && floorMesh.layerCount) {
    var _ovHMin = Infinity, _ovHMax = -Infinity, _ovWater = 0;
    var _ovN = floorMesh.layerCount.length;
    for (var _ovi = 0; _ovi < _ovN; _ovi++) {
      var _ovH = floorMesh.l0TopZ[_ovi];
      if (_ovH < _ovHMin) _ovHMin = _ovH;
      if (_ovH > _ovHMax) _ovHMax = _ovH;
      if (floorMesh.water && floorMesh.water[_ovi]) _ovWater++;
    }
    stats.push('HEIGHT: [' + _ovHMin.toFixed(1) + ', ' + _ovHMax.toFixed(1) + ']' +
      '  WATER: ' + _ovWater + ' (' + (_ovWater / _ovN * 100).toFixed(1) + '%)');
  }

  // Legend
  var legend = [
    {color:'#2a2030', label:'Wall'},
    {color:'#4a3c28', label:'Floor'},
    {color:'#44aaff', label:'Cave corridor'},
    {color:'#cc44ff', label:'Cave chamber'},
    {color:'#ffcc44', label:'Ore vein'},
    {color:'#ffd700', label:'Treasure chest'},
    {color:'#cc2244', label:'Enemy spawner'},
    {color:'#00ffcc', label:'Cave entrance'},
    {color:'#00ff44', label:'Goal'},
    {color:'#ff4444', label:'Enemy'},
    {color:'#ffaa44', label:'Shrine'},
    {color:'#ffd700', label:'Market'},
    {color:'#ffaa33', label:'Structure'},
    {color:'#ffffff', label:'Player'},
  ];

  var panelX = 8, panelY = 8, lineH = 16, panelW = 420, padY = 8;
  var totalH = padY * 2 + stats.length * lineH + 8 + legend.length * lineH;
  ctx.fillStyle = 'rgba(0,0,0,0.78)';
  ctx.fillRect(panelX, panelY, panelW, totalH);
  ctx.strokeStyle = '#446644';
  ctx.lineWidth = 1.5;
  ctx.strokeRect(panelX, panelY, panelW, totalH);

  ctx.font = '12px monospace';
  for (var si = 0; si < stats.length; si++) {
    ctx.fillStyle = '#aaffaa';
    ctx.fillText(stats[si], panelX + 10, panelY + padY + si * lineH + 12);
  }
  var legStartY = panelY + padY + stats.length * lineH + 10;
  for (var li = 0; li < legend.length; li++) {
    ctx.fillStyle = legend[li].color;
    ctx.fillRect(panelX + 10, legStartY + li * lineH + 2, 12, 12);
    ctx.strokeStyle = '#555';
    ctx.lineWidth = 0.5;
    ctx.strokeRect(panelX + 10, legStartY + li * lineH + 2, 12, 12);
    ctx.fillStyle = '#cccccc';
    ctx.fillText(legend[li].label, panelX + 28, legStartY + li * lineH + 12);
  }

  // "OVERVIEW MODE" banner at top-right
  ctx.fillStyle = 'rgba(0,0,0,0.7)';
  ctx.fillRect(cw - 185, 8, 177, 24);
  ctx.fillStyle = '#88ff88';
  ctx.font = 'bold 13px monospace';
  ctx.fillText('🗺 OVERVIEW MODE  [click to exit]', cw - 183, 24);
}

var _overviewPrevW = 0, _overviewPrevH = 0;

function toggleOverview() {
  overviewActive = !overviewActive;
  if (overviewActive) {
    // Expand canvas to fill the window for a proper full-map view
    _overviewPrevW = canvas.width;
    _overviewPrevH = canvas.height;
    var maxW = Math.min(window.innerWidth - 40, 1400);
    var maxH = Math.min(window.innerHeight - 40, 900);
    canvas.width = maxW;
    canvas.height = maxH;
    canvas.style.width = maxW + 'px';
    canvas.style.height = maxH + 'px';
    drawDebugOverview();
  } else {
    // Restore original canvas size
    canvas.width = _overviewPrevW;
    canvas.height = _overviewPrevH;
    canvas.style.width = '';
    canvas.style.height = '';
  }
}

function draw2D() {
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  if (bgPattern) {
    ctx.save(); ctx.fillStyle = bgPattern; ctx.fillRect(0, 0, canvas.width, canvas.height); ctx.restore();
  }
  ctx.save(); ctx.translate(-viewCam.x, -viewCam.y);

  // Walls
  for (var i = 0; i < walls.length; i++) {
    var w = walls[i];
    ctx.fillStyle = wallColor;
    ctx.fillRect(w.x, w.y, w.w, w.h);
  }
  // Draw irregular border outline for plains/cave terrain
  drawIrregularBorder2D();

  // Goal
  if (goal) {
    ctx.fillStyle = '#00ff00';
    ctx.fillRect(goal.x, goal.y, goal.w, goal.h);
  }

  // Player
  ctx.fillStyle = '#ff0000';
  ctx.beginPath(); ctx.arc(pos.x, pos.y, 6, 0, Math.PI * 2); ctx.fill();

  // Enemies — skeleton sprites, head rotated to face player
  var now = Date.now();
  for (var j = 0; j < enemies.length; j++) {
    var e = enemies[j]; var eType = e.enemyType;
    var r = 8 * eType.size; // used for status rings and health bar offset
    var hpPct2d = e.health / e.maxHealth;
    var isFlashing2d = (e.damageFlash && now < e.damageFlash);
    var msPerFrame2d = Math.max(50, 200 - eType.speed * 2);
    var skelFrameIdx2d = Math.floor(now / msPerFrame2d) % SKEL_FRAMES;
    var skelKey2d = (hpPct2d < 0.25) ? (eType.id + '_crumble') : eType.id;
    var skelCanvas2d = skeletonFrames && skeletonFrames[skelKey2d] && skeletonFrames[skelKey2d][0] && skeletonFrames[skelKey2d][0][skelFrameIdx2d];
    var sw2d = r * 2.8, sh2d = sw2d * (SKEL_H / SKEL_W);
    ctx.save();
    if (skelCanvas2d) {
      ctx.drawImage(skelCanvas2d, e.x - sw2d / 2, e.y - sh2d / 2, sw2d, sh2d);
      if (isFlashing2d) {
        ctx.globalAlpha = 0.5;
        ctx.fillStyle = e.damageFlashColor || '#ffffff';
        ctx.fillRect(e.x - sw2d / 2, e.y - sh2d / 2, sw2d, sh2d);
        ctx.globalAlpha = 1.0;
      }
    } else {
      // Fallback circle if sprites not ready
      ctx.fillStyle = isFlashing2d ? (e.damageFlashColor || '#ffffff') : eType.color;
      ctx.beginPath(); ctx.arc(e.x, e.y, r, 0, Math.PI * 2); ctx.fill();
    }
    ctx.restore();
    // Status rings (drawn in world space, no rotation)
    if (e.slowUntil && now < e.slowUntil) {
      ctx.strokeStyle = '#00ffff'; ctx.lineWidth = 2;
      ctx.beginPath(); ctx.arc(e.x, e.y, r + 2, 0, Math.PI * 2); ctx.stroke();
    }
    if (e.burnUntil && now < e.burnUntil) {
      ctx.fillStyle = 'rgba(255,100,0,0.6)';
      ctx.beginPath(); ctx.arc(e.x, e.y, r * 0.4, 0, Math.PI * 2); ctx.fill();
    }
    // Health bar
    ctx.fillStyle = (hpPct2d > 0.6) ? '#00ff00' : (hpPct2d > 0.3) ? '#ffaa00' : '#ff4444';
    ctx.fillRect(e.x - 10, e.y - 15 - r, 20 * hpPct2d, 3);
    ctx.strokeStyle = '#fff'; ctx.lineWidth = 1; ctx.strokeRect(e.x - 10, e.y - 15 - r, 20, 3);
  }

  // Death effects — bone scatter (tumbling line segments with physics)
  ctx.lineWidth = 2; ctx.lineCap = 'round';
  for (var d = 0; d < deathEffects.length; d++) {
    var de = deathEffects[d];
    var t = Math.max(0, Math.min(1, (now - de.spawnMs) / de.lifeMs));
    ctx.globalAlpha = 1.0 - t * t;
    ctx.strokeStyle = de.color;
    ctx.save();
    ctx.translate(de.x, de.y);
    ctx.rotate(de.rot + de.rotVel * t);
    ctx.beginPath(); ctx.moveTo(-de.len / 2, 0); ctx.lineTo(de.len / 2, 0); ctx.stroke();
    ctx.restore();
  }
  ctx.globalAlpha = 1.0;

  // Floor scatter items and wall decorations — drawn in world space, inside translate
  drawFloorScatter2D();
  drawWallDecorations2D();

  ctx.restore();

  // Timer overlay
  ctx.fillStyle = '#fff'; ctx.font = '14px monospace';
  ctx.fillText('Time: ' + timeSec.toFixed(1) + 's', 10, 20);
  ctx.fillStyle = timeColor;
  ctx.fillText('\u25cf', 90, 20);

}

function draw() {
  if (calibrating) { drawCalibration(); return; }
  projScale = canvas.height * (180 / CANVAS_BASE_H);
  resScale = canvas.height / CANVAS_BASE_H;

  if (MODE3D) {
    var now = Date.now();

    // Gyro aim camera smoothing
    if (CONTROL_MODE === MODE_GYRO_AIM && USE_YAW && hasYaw) {
      var effTarget = yawTarget + yawOffset;
      if (capturingYaw) {
        if (!isFinite(camHold)) camHold = cam.ang;
        cam.ang = camHold;
        if (DEBUG_CAM && now - __camDbgLast > 300) {
          __camDbgLast = now;
          try { console.log('[CAM]', 'capturing=1', 'camHold=', camHold.toFixed(3), 'yawTarget=', yawTarget.toFixed ? yawTarget.toFixed(3) : yawTarget, 'yawOffset=', yawOffset.toFixed ? yawOffset.toFixed(3) : yawOffset); } catch (_) {}
        }
      } else {
        var d = angNorm(effTarget - cam.ang);
        var alpha = (now < yawResumeUntil) ? Math.min(0.5, yawAlpha * 2.0) : yawAlpha;
        cam.ang += d * alpha;
        camHold = cam.ang;
        if (DEBUG_CAM && now - __camDbgLast > 300) {
          __camDbgLast = now;
          try { console.log('[CAM]', 'capturing=0', 'effTarget=', effTarget.toFixed ? effTarget.toFixed(3) : effTarget, 'cam=', cam.ang.toFixed(3), 'd=', d.toFixed ? d.toFixed(3) : d, 'alpha=', alpha.toFixed ? alpha.toFixed(3) : alpha); } catch (_) {}
        }
      }
    }

    // Pitch smoothing — SKIP entirely when mouse controls pitch
    if (!USE_MOUSE) {
      if (CONTROL_MODE === MODE_GYRO_AIM && USE_PITCH) {
        // Gyro mode: smooth toward pitchTarget + transient terrain nudge
        var pitchGoal = pitchTarget + (autoPitchOff || 0);
        var pitchDiff = pitchGoal - cam.pitch;
        cam.pitch += pitchDiff * pitchAlpha;
      } else {
        // Stick / keyboard only — apply transient terrain nudge directly
        cam.pitch = (cam.pitch || 0) + (autoPitchOff || 0) * 0.15;
        cam.pitch = Math.max(-CAM_PITCH_MAX, Math.min(CAM_PITCH_MAX, cam.pitch));
      }
    }

    // Camera follow
    if (CAM_FOLLOW) {
      cam.x = pos.x; cam.y = pos.y;
      if (Number.isFinite(pos.floorZ)) cam.z = pos.floorZ;
    }

    ctx.clearRect(0, 0, canvas.width, canvas.height);
    // When underground, fill the entire canvas with a dark cave floor color
    // so that any gaps between floor quads show cave-colored background
    // instead of the black void/skybox bleeding through.
    if (playerUnderground) {
      ctx.fillStyle = 'rgb(8,6,4)';
      ctx.fillRect(0, 0, canvas.width, canvas.height);
    }
    // Run light grid every 5 frames — flicker at 12Hz still reads natural,
    // scaling work drops 5×. Player movement underground still updates at
    // the reduced rate (unnoticeable at walking speed over ~80ms).
    _floorCacheTick = (_floorCacheTick + 1) | 0;  // invalidate floor cache each frame
    _pt('lightGrid', function(){ if ((++_lightGridFrameCount) % 5 === 0) updateLightGrid(); });
    beginSceneDepthFrame(canvas.width, canvas.height);
    _pt('skybox3D', function(){ drawSkybox3D(); });
    _pt('ceiling3D', function(){ if (!DEBUG_HIDE_CEIL) drawLayersCeiling3D(); });
    _pt('platforms3D', function(){ if (!DEBUG_HIDE_PLATS) drawLayersFloor3D(); });
    _pt('walls3D', function(){ if (!DEBUG_HIDE_WALLS) drawWalls3D(); });
    _pt('caveEntrance3D', function(){ drawCaveEntrance3D(); });
    _pt('sceneEntities', function(){
      drawFloorScatter3D();
      drawTreasureChests3D();
      drawEnemySpawners3D();
      drawOreVeins();
      drawWallDecorations();
    });
    _pt('goalMarker3D', function(){ drawGoalMarker3D(); });
  } else {
    if (CONTROL_MODE === MODE_GYRO_AIM && USE_YAW && hasYaw) {
      var d2 = angNorm(yawTarget - cam.ang);
      cam.ang += d2 * yawAlpha;
    }
    draw2D();
    drawPlatforms2D();
  }
}
