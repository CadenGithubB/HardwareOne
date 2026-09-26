// =============================================
// SECTION 15: RENDERING - HUD & MENU
// =============================================

// =============================================
// FPS WIZARD ARMS
// =============================================
// Drawn in screen-space after the 3D world so it always appears on top.
// The right arm holds the casting orb; both arms bob gently with movement.
// castAnimUntil drives a forward-thrust animation on each cast.
// ─────────────────────────────────────────────────────────────────────────────
// drawFPSArms — pixel-art wizard arms (sleeve + cuff + hand)
// ─────────────────────────────────────────────────────────────────────────────
var _armRightIdle = null;
var _armRightCast = null;
var _armLeftIdle = null;
var _armLeftCast = null;
var _armLastRobeId = null;
var ARM_PX_W = 80, ARM_PX_H = 160;

function buildPixelArmSprites() {
  var ac = (equipment.robes && equipment.robes.armColor) ? equipment.robes.armColor : null;
  var robeId = equipment.robes ? equipment.robes.id : '_default';
  if (_armRightIdle && _armLastRobeId === robeId) return;

  // Parse hex color to r,g,b array
  function h2rgb(hex) {
    var v = parseInt(hex.slice(1), 16);
    return [(v>>16)&255, (v>>8)&255, v&255];
  }
  // Lerp two rgb arrays
  function lerpC(a, b, t) {
    return rgbQ(Math.round(a[0]+(b[0]-a[0])*t), Math.round(a[1]+(b[1]-a[1])*t), Math.round(a[2]+(b[2]-a[2])*t));
  }
  function rgbStr(r) { return rgbQ(r[0], r[1], r[2]); }

  // Robe colors — 6 tones from darkest shadow to brightest highlight
  var rDeep = h2rgb(ac ? ac.deep : '#0d0618');
  var rMid  = h2rgb(ac ? ac.mid  : '#190d30');
  var rLit  = h2rgb(ac ? ac.lit  : '#2c1455');
  var rCuff = h2rgb(ac ? ac.cuff : '#3d2068');
  // Derived tones
  var rShadow = [Math.max(0,rDeep[0]-8), Math.max(0,rDeep[1]-6), Math.max(0,rDeep[2]-4)];
  var rHi = [Math.min(255,rLit[0]+30), Math.min(255,rLit[1]+25), Math.min(255,rLit[2]+40)];
  var rTrim = [Math.min(255,rCuff[0]+40), Math.min(255,rCuff[1]+30), Math.min(255,rCuff[2]+60)];
  var rFold = [Math.max(0,rMid[0]-12), Math.max(0,rMid[1]-10), Math.max(0,rMid[2]-8)];

  // Skin tones
  var sk  = [200,168,130];
  var skS = [160,128,92];
  var skH = [228,205,175];
  var skD = [125,98,68];
  var skM = [180,148,110]; // mid skin

  // Build one arm canvas. isCast=fingers open, flipX=mirror for left
  function makeArm(isCast, flipX) {
    var oc = document.createElement('canvas');
    oc.width = ARM_PX_W; oc.height = ARM_PX_H;
    var c = oc.getContext('2d');
    if (flipX) { c.translate(ARM_PX_W, 0); c.scale(-1, 1); }

    var W = ARM_PX_W, H = ARM_PX_H;
    // Layout (top→bottom):
    //   rows 0-11:  tiny hand nub (just a small fist poking out)
    //   rows 12-23: cuff (ornate band)
    //   rows 24-159: SLEEVE — massive, dominates the sprite (~85%)

    // ── SLEEVE (rows 24 to 159) — 136 rows of rich robed fabric ──
    var sleeveTop = 24, sleeveBot = H - 1;
    var sleeveRows = sleeveBot - sleeveTop;
    // Precompute noise-like fold positions
    var foldRows = {};
    for (var fi = 0; fi < 16; fi++) {
      var fRow = sleeveTop + Math.round((fi * 8.5) + (fi * fi * 0.5) % 9);
      if (fRow < sleeveBot) foldRows[fRow] = true;
    }

    for (var sy = sleeveTop; sy <= sleeveBot; sy++) {
      var t = (sy - sleeveTop) / sleeveRows; // 0 at cuff end, 1 at bottom
      // Perspective: wider at bottom (near player), matches cuff width at top
      var slW = 22 + t * 44; // 22px at cuff → 66px at bottom
      var cx = W * 0.48 + t * 4; // slight drift
      var sL = Math.round(cx - slW * 0.5);
      var sR = Math.round(cx + slW * 0.5);

      for (var sx = sL; sx < sR; sx++) {
        if (sx < 0 || sx >= W) continue;
        var frac = (sx - sL) / (sR - sL); // 0=left, 1=right

        // Cylindrical shading — smooth 5-tone gradient across width
        // Left edge = deep shadow, center-right = lit, right edge = mid
        var shade;
        if (frac < 0.08) shade = rShadow;
        else if (frac < 0.2) shade = rDeep;
        else if (frac < 0.4) shade = rMid;
        else if (frac < 0.65) shade = rLit;
        else if (frac < 0.85) shade = rHi;
        else if (frac < 0.95) shade = rLit;
        else shade = rMid;

        // Vertical gradient: slightly lighter toward cuff, darker at bottom
        var vBias = (1 - t) * 0.15;
        c.fillStyle = lerpC(shade, rHi, vBias);
        c.fillRect(sx, sy, 1, 1);
      }

      // Fabric fold lines — horizontal dark stripes at intervals
      if (foldRows[sy]) {
        var foldLen = Math.round(slW * 0.6);
        var foldStart = Math.round(cx - foldLen * 0.4);
        for (var fx = foldStart; fx < foldStart + foldLen; fx++) {
          if (fx < sL || fx >= sR) continue;
          c.fillStyle = rgbStr(rFold);
          c.fillRect(fx, sy, 1, 1);
        }
        // Secondary highlight below fold
        if (sy + 1 <= sleeveBot) {
          var hlLen = Math.round(foldLen * 0.5);
          var hlStart = foldStart + 2;
          for (var hx = hlStart; hx < hlStart + hlLen; hx++) {
            if (hx < sL || hx >= sR) continue;
            c.fillStyle = lerpC(rLit, rHi, 0.4);
            c.fillRect(hx, sy + 1, 1, 1);
          }
        }
      }

      // Edge stitch detail — 1px border on left and right
      if (t > 0.1) {
        c.fillStyle = rgbStr(rShadow);
        if (sL >= 0) c.fillRect(sL, sy, 1, 1);
        c.fillStyle = lerpC(rMid, rDeep, 0.5);
        if (sR - 1 < W) c.fillRect(sR - 1, sy, 1, 1);
      }
    }

    // ── CUFF (rows 12-23) — ornate banded cuff ──
    var cuffTop = 12, cuffBot = 23;
    var cuffW = 22;
    var cuffCx = W * 0.48;
    for (var cy = cuffTop; cy <= cuffBot; cy++) {
      var ct = (cy - cuffTop) / (cuffBot - cuffTop);
      var cL = Math.round(cuffCx - cuffW * 0.5);
      var cR = Math.round(cuffCx + cuffW * 0.5);
      for (var cx2 = cL; cx2 < cR; cx2++) {
        var cfrac = (cx2 - cL) / (cR - cL);
        // Trim bands at top/bottom edges of cuff
        if (cy === cuffTop || cy === cuffTop + 1 || cy === cuffBot || cy === cuffBot - 1) {
          // Gold/bright trim
          c.fillStyle = rgbStr(rTrim);
        } else if (cy === cuffTop + 3 || cy === cuffBot - 3) {
          // Inner trim line
          c.fillStyle = lerpC(rTrim, rCuff, 0.5);
        } else {
          // Main cuff body — cylindrical shading
          if (cfrac < 0.15) c.fillStyle = lerpC(rCuff, rDeep, 0.5);
          else if (cfrac < 0.6) c.fillStyle = rgbStr(rCuff);
          else if (cfrac < 0.85) c.fillStyle = lerpC(rCuff, rTrim, 0.3);
          else c.fillStyle = rgbStr(rCuff);
        }
        c.fillRect(cx2, cy, 1, 1);
      }
      // Embroidery pattern — diamond shapes in center of cuff
      if (cy >= cuffTop + 4 && cy <= cuffBot - 4) {
        var patRow = cy - (cuffTop + 4);
        var patH = cuffBot - 4 - (cuffTop + 4);
        // Diamond: width peaks in middle
        var diamondW = Math.round(4 * (1 - Math.abs(patRow - patH * 0.5) / (patH * 0.5)));
        if (diamondW > 0) {
          var dL = Math.round(cuffCx - diamondW * 0.5);
          for (var dx = dL; dx < dL + diamondW; dx++) {
            c.fillStyle = rgbStr(rTrim);
            c.fillRect(dx, cy, 1, 1);
          }
        }
      }
    }

    // ── FINGERS poking out of cuff (rows 0-11) ──
    var handCx = W * 0.48;
    // 4 thin fingers emerging from the cuff, each 2px wide with 1px gap
    // Idle: fingers together, slightly curled
    // Cast: fingers spread wider apart
    var fingerLen = isCast ? 10 : 8; // longer reach when casting
    var fingerSpacing = isCast ? 4.5 : 3; // spread apart when casting
    var fingerW = 2;
    var fingers = [
      {xOff: -fingerSpacing * 1.5, len: fingerLen - 2},  // pinky (shortest)
      {xOff: -fingerSpacing * 0.5, len: fingerLen},       // ring
      {xOff:  fingerSpacing * 0.5, len: fingerLen + 1},   // middle (longest)
      {xOff:  fingerSpacing * 1.5, len: fingerLen - 1}    // index
    ];
    for (var fi = 0; fi < 4; fi++) {
      var fg = fingers[fi];
      var fBase = 11; // where finger meets cuff
      var fTip = fBase - fg.len;
      var fxC = Math.round(handCx + fg.xOff);
      for (var fy = fTip; fy <= fBase; fy++) {
        var ft = (fBase - fy) / fg.len; // 0=base, 1=tip
        // Taper at tip: 2px wide normally, 1px at very tip
        var fw = (ft > 0.85) ? 1 : fingerW;
        for (var fxi = 0; fxi < fw; fxi++) {
          // Simple left-dark right-light shading
          if (fxi === 0 && fw > 1) c.fillStyle = rgbStr(skS);
          else c.fillStyle = rgbStr(sk);
          c.fillRect(fxC + fxi, fy, 1, 1);
        }
        // Highlight on right edge
        if (fw > 1) {
          c.fillStyle = rgbStr(skH);
          c.fillRect(fxC + fw - 1, fy, 1, 1);
        }
        // Joint crease at ~1/3 from tip
        if (ft > 0.30 && ft < 0.36) {
          for (var jx = 0; jx < fw; jx++) {
            c.fillStyle = rgbStr(skD);
            c.fillRect(fxC + jx, fy, 1, 1);
          }
        }
      }
      // Rounded fingertip — 1px cap
      c.fillStyle = rgbStr(skM);
      c.fillRect(fxC, fTip, 1, 1);
    }

    // Thumb — sticks out to the right, shorter
    var thBase = 11, thLen = 5;
    var thX = Math.round(handCx + fingerSpacing * 2 + 1);
    for (var ty = thBase - thLen; ty <= thBase; ty++) {
      var tt = (thBase - ty) / thLen;
      var tw = (tt > 0.7) ? 1 : 2;
      c.fillStyle = (tw > 1) ? rgbStr(sk) : rgbStr(skM);
      c.fillRect(thX, ty, tw, 1);
      if (tw > 1) {
        c.fillStyle = rgbStr(skH);
        c.fillRect(thX + 1, ty, 1, 1);
      }
    }

    return oc;
  }

  _armRightIdle = makeArm(false, false);
  _armRightCast = makeArm(true, false);
  _armLeftIdle  = makeArm(false, true);
  _armLeftCast  = makeArm(true, true);
  _armLastRobeId = robeId;
  console.log('[ARM] pixel arm sprites built for robe: ' + robeId + ' @ ' + ARM_PX_W + 'x' + ARM_PX_H);
}

function drawFPSArms() {
  if (typeof HAND_RIG_PREVIEW !== 'undefined' && HAND_RIG_PREVIEW &&
      typeof drawFirstPersonHandRig === 'function') {
    drawFirstPersonHandRig(HAND_RIG_TIME_MS,HAND_RIG_VIEW_YAW);return;
  }
  if (typeof CASTING_ART_ENABLED !== 'undefined' && CASTING_ART_ENABLED &&
      getCurrentSpell().id === 'missile' && typeof drawFirstPersonCastingArt === 'function') {
    drawFirstPersonCastingArt(Date.now());
  } else drawFPSArmsLegacy();
}

function drawFPSArmsLegacy() {
  if (!MODE3D || shopOpen) return;
  var w = canvas.width, h = canvas.height;
  var S = resScale;
  var now = Date.now();
  var spd = Math.hypot(vel.x, vel.y);
  walkBobPhase += spd * 0.002;
  var bob  = Math.sin(walkBobPhase) * Math.min(7 * S, spd * 0.05 * S);
  var bobX = Math.cos(walkBobPhase * 0.5) * Math.min(3 * S, spd * 0.02 * S);
  var casting = (now < castAnimUntil);
  var castT   = casting ? Math.max(0, (castAnimUntil - now) / 280) : 0;
  var spell   = getCurrentSpell();

  buildPixelArmSprites();
  if (!_armRightIdle) return;

  ctx.save();
  ctx.globalCompositeOperation = 'source-over';
  ctx.imageSmoothingEnabled = false;

  // Scale — keep arms chunky but not huge
  var pxSize = Math.max(1, Math.round(1.4 * S));
  var sprW = ARM_PX_W * pxSize;
  var sprH = ARM_PX_H * pxSize;

  // Source crop — show top 75% of sprite (hand + cuff + most of sleeve)
  // Only the very bottom of the sleeve stays off-screen
  var cropRatio = 0.75;
  var srcH = Math.round(ARM_PX_H * cropRatio);
  var dstH = Math.round(sprH * cropRatio);

  var rightSpr = casting ? _armRightCast : _armRightIdle;
  var leftSpr  = casting ? _armLeftCast  : _armLeftIdle;

  // Camera-relative anchoring — arms float in lower portion of screen
  var baseY = h * 0.72;
  var baseX = w * 0.5;
  var spread = w * 0.30;

  // ── Right arm (with inward tilt) ──
  var rX = baseX + spread + bobX - castT * 20 * S;
  var rY = baseY + bob * 0.6 - castT * 12 * S;
  ctx.save();
  // Pivot at bottom of sprite (where sleeve exits screen), tilt top inward
  ctx.translate(rX, rY + dstH);
  ctx.rotate(-0.30); // tilt inward — hands point toward center
  ctx.drawImage(rightSpr,
    0, 0, ARM_PX_W, srcH,           // source crop: top portion only
    -sprW * 0.5, -dstH, sprW, dstH  // dest: draw upward from pivot
  );
  ctx.restore();

  // ── Orb — anchored to the hand ──
  var orbR = (8 + castT * 10) * S;
  // The sprite is drawn at (-sprW/2, -dstH) relative to pivot at (rX, rY+dstH).
  // Hand/palm is near the top-center of the sprite.
  // In local (pre-rotation) coords relative to pivot:
  var localX = 0;                 // center of sprite
  var localY = -dstH * 0.85;     // 85% up from pivot toward top
  // Apply the same -0.30 rotation the canvas uses
  var cosT = Math.cos(-0.30), sinT = Math.sin(-0.30);
  var orbX = rX + localX * cosT - localY * sinT;
  var orbY = (rY + dstH) + localX * sinT + localY * cosT;
  orbY -= castT * 8 * S;
  ctx.shadowBlur = (18 + castT * 38) * S; ctx.shadowColor = spell.color;
  ctx.globalAlpha = 0.90 + castT * 0.10;
  ctx.fillStyle = spell.color;
  ctx.beginPath(); ctx.arc(orbX, orbY, orbR, 0, Math.PI * 2); ctx.fill();
  ctx.fillStyle = '#ffffff'; ctx.globalAlpha = 0.28 + castT * 0.52;
  ctx.beginPath();
  ctx.arc(orbX - orbR * 0.30, orbY - orbR * 0.28, orbR * 0.42, 0, Math.PI * 2); ctx.fill();
  ctx.strokeStyle = spell.color; ctx.lineWidth = (2 + castT * 3) * S;
  ctx.globalAlpha = 0.25 + castT * 0.30;
  ctx.beginPath(); ctx.arc(orbX, orbY, orbR + (4 + castT * 5) * S, 0, Math.PI * 2); ctx.stroke();
  ctx.shadowBlur = 0; ctx.globalAlpha = 1.0;

  // ── Left arm (with inward tilt, mirrored) ──
  var lX = baseX - spread - bobX + castT * 10 * S;
  var lY = baseY + bob * 0.4 - castT * 6 * S;
  ctx.save();
  ctx.translate(lX, lY + dstH);
  ctx.rotate(0.30); // tilt inward (mirrored)
  ctx.drawImage(leftSpr,
    0, 0, ARM_PX_W, srcH,
    -sprW * 0.5, -dstH, sprW, dstH
  );
  ctx.restore();

  ctx.restore();
}

// =============================================
// SHOP & COIN RENDERING
// =============================================

function drawCoins2D() {
  if (!coinDrops || !coinDrops.length) return;
  var now = Date.now();
  ctx.save(); ctx.translate(-viewCam.x, -viewCam.y);
  for (var i = 0; i < coinDrops.length; i++) {
    var c = coinDrops[i];
    var bob = Math.sin(now * 0.004 + c.bob) * 3;
    ctx.fillStyle = '#ffd700';
    ctx.strokeStyle = '#b8860b';
    ctx.lineWidth = 1.5;
    ctx.beginPath(); ctx.arc(c.x, c.y + bob, 5, 0, Math.PI * 2);
    ctx.fill(); ctx.stroke();
    ctx.fillStyle = '#fff8'; ctx.font = 'bold 5px Arial'; ctx.textAlign = 'center';
    ctx.fillText('$', c.x, c.y + bob + 2);
  }
  ctx.restore();
}

function drawCoins3D() {
  renderEntities3D(coinDrops, {maxDist: viewDist * 0.5, groundAnchor: true, fadeFraction: 1,
    bounds:function(coin,vis,C,now) {
      var size = Math.max(4,Math.min(14,Math.floor(C.h*0.18/(vis.fwd*0.12+1))));
      var cy = vis.sy-size*1.2+Math.sin(now*0.004+(Number.isFinite(coin.bob)?coin.bob:0))*3;
      return {x:vis.sx-size-22,y:cy-size-22,width:size*2+44,height:size*2+44};
    }},
    function(coin, vis, C, ctx, now) {
      var h = C.h, fwd = vis.fwd;
      var floorY = vis.sy;
      var sz = Math.max(4, Math.min(14, Math.floor(h * 0.18 / (fwd * 0.12 + 1))));
      var bob = Math.sin(now * 0.004 + (Number.isFinite(coin.bob) ? coin.bob : 0)) * 3;
      var cy = floorY - sz * 1.2 + bob;
      ctx.save();
      ctx.shadowBlur = 10; ctx.shadowColor = '#ffd700';
      ctx.fillStyle = '#ffd700'; ctx.strokeStyle = '#b8860b'; ctx.lineWidth = 1.5;
      ctx.beginPath(); ctx.arc(vis.sx, cy, sz, 0, Math.PI * 2); ctx.fill(); ctx.stroke();
      ctx.fillStyle = '#ffffffcc'; ctx.font = 'bold ' + Math.max(6, sz) + 'px Arial'; ctx.textAlign = 'center';
      ctx.fillText('$', vis.sx, cy + sz * 0.35);
      ctx.shadowBlur = 0; ctx.restore();
    });
}

// ── Ambient Atmospheric Particles ──────────────────────────────────────
var AMBIENT_MAX = 35;

function spawnAmbientParticle(nearX, nearY, terrainType) {
  var ang = Math.random() * Math.PI * 2;
  var rad = 30 + Math.random() * 150;
  var px = nearX + Math.cos(ang) * rad;
  var py = nearY + Math.sin(ang) * rad;
  // Base Z at floor level so particles work at any elevation (including underground caves)
  var baseZ = floorMesh ? getFloorHeightAt(px, py) * 25 : 0;
  var p = {x: px, y: py, z: baseZ, vx: 0, vy: 0, vz: 0, alpha: 0, maxAlpha: 0.25,
           life: 0, maxLife: 3, size: 1.5, type: 'dust', r: 180, g: 160, b: 120};

  if (terrainType === 'ice') {
    var roll = Math.random();
    if (roll < 0.6) {
      // snowflake — drifts down slowly
      p.type = 'snowflake'; p.size = 1 + Math.random() * 2;
      p.vx = (Math.random() - 0.5) * 8; p.vy = (Math.random() - 0.5) * 8;
      p.vz = -(5 + Math.random() * 10);
      p.z = baseZ + 30 + Math.random() * 40;
      p.maxAlpha = 0.25 + Math.random() * 0.2;
      p.maxLife = 4 + Math.random() * 3;
      p.r = 210; p.g = 230; p.b = 255;
    } else {
      // ice sparkle — brief flash
      p.type = 'sparkle'; p.size = 1 + Math.random();
      p.z = baseZ + 2 + Math.random() * 15;
      p.maxAlpha = 0.4 + Math.random() * 0.3;
      p.maxLife = 0.8 + Math.random() * 1.2;
      p.r = 180; p.g = 220; p.b = 255;
    }
  } else if (terrainType === 'cave') {
    var roll = Math.random();
    if (roll < 0.5) {
      // water drip — falls straight down
      p.type = 'drip'; p.size = 1.5;
      p.vz = -(20 + Math.random() * 15);
      p.z = baseZ + 35 + Math.random() * 30;
      p.maxAlpha = 0.35 + Math.random() * 0.15;
      p.maxLife = 1.5 + Math.random() * 1.5;
      p.r = 80; p.g = 120; p.b = 200;
    } else {
      // spore mote — drifts up slowly
      p.type = 'spore'; p.size = 1 + Math.random() * 1.5;
      p.vx = (Math.random() - 0.5) * 5; p.vy = (Math.random() - 0.5) * 5;
      p.vz = 2 + Math.random() * 4;
      p.z = baseZ + 3 + Math.random() * 10;
      p.maxAlpha = 0.2 + Math.random() * 0.15;
      p.maxLife = 4 + Math.random() * 4;
      p.r = 120; p.g = 190; p.b = 80;
    }
  } else if (terrainType === 'expanse') {
    var roll = Math.random();
    if (roll < 0.7) {
      // sand dust — drifts horizontally
      p.type = 'dust'; p.size = 1 + Math.random() * 1.5;
      p.vx = 8 + Math.random() * 12; p.vy = (Math.random() - 0.5) * 6;
      p.vz = (Math.random() - 0.5) * 3;
      p.z = baseZ + 3 + Math.random() * 20;
      p.maxAlpha = 0.15 + Math.random() * 0.15;
      p.maxLife = 3 + Math.random() * 3;
      p.r = 200; p.g = 180; p.b = 140;
    } else {
      // heat shimmer
      p.type = 'shimmer'; p.size = 2 + Math.random() * 2;
      p.vz = 1 + Math.random() * 3;
      p.z = baseZ + 5 + Math.random() * 15;
      p.maxAlpha = 0.08 + Math.random() * 0.08;
      p.maxLife = 2 + Math.random() * 2;
      p.r = 255; p.g = 240; p.b = 200;
    }
  } else {
    // ground / plains — dust motes and fireflies
    var roll = Math.random();
    if (roll < 0.8) {
      p.type = 'dust'; p.size = 1 + Math.random();
      p.vx = (Math.random() - 0.5) * 6; p.vy = (Math.random() - 0.5) * 6;
      p.vz = (Math.random() - 0.5) * 2;
      p.z = baseZ + 5 + Math.random() * 20;
      p.maxAlpha = 0.18 + Math.random() * 0.12;
      p.maxLife = 3 + Math.random() * 4;
      p.r = 180; p.g = 160; p.b = 120;
    } else {
      // firefly — gentle green glow
      p.type = 'firefly'; p.size = 1.5;
      p.vx = (Math.random() - 0.5) * 10; p.vy = (Math.random() - 0.5) * 10;
      p.vz = (Math.random() - 0.5) * 5;
      p.z = baseZ + 8 + Math.random() * 20;
      p.maxAlpha = 0.3 + Math.random() * 0.3;
      p.maxLife = 3 + Math.random() * 5;
      p.r = 150; p.g = 255; p.b = 80;
    }
  }
  p.life = p.maxLife;
  return p;
}

function initAmbientParticles() {
  ambientParticles = [];
  var px = pos ? pos.x : worldW * 0.5;
  var py = pos ? pos.y : worldH * 0.5;
  for (var i = 0; i < AMBIENT_MAX; i++) {
    var p = spawnAmbientParticle(px, py, terrain);
    p.life = Math.random() * p.maxLife; // stagger initial lifetimes
    ambientParticles.push(p);
  }
}

function updateAmbientParticles(dt) {
  var px = pos ? pos.x : 0, py = pos ? pos.y : 0;
  for (var i = 0; i < ambientParticles.length; i++) {
    var p = ambientParticles[i];
    p.x += p.vx * dt;
    p.y += p.vy * dt;
    p.z += p.vz * dt;
    // Floor clamp: use terrain height at particle position (supports underground caves)
    var pFloorZ = floorMesh ? getFloorHeightAt(p.x, p.y) * 25 : 0;
    if (p.z < pFloorZ) p.z = pFloorZ;
    p.life -= dt;
    // Fade in during first 20% of life, fade out during last 30%
    var lifeRatio = p.life / p.maxLife;
    if (lifeRatio > 0.8) p.alpha = p.maxAlpha * ((1 - lifeRatio) / 0.2);
    else if (lifeRatio < 0.3) p.alpha = p.maxAlpha * (lifeRatio / 0.3);
    else p.alpha = p.maxAlpha;
    // Firefly pulse
    if (p.type === 'firefly') p.alpha *= 0.5 + 0.5 * Math.sin(Date.now() * 0.005 + i * 2.1);
    // Ember pulse
    if (p.type === 'ember') p.alpha *= 0.6 + 0.4 * Math.sin(Date.now() * 0.01 + i * 1.7);
    // Respawn if expired or too far from player
    var distToPlayer = Math.hypot(p.x - px, p.y - py);
    if (p.life <= 0 || distToPlayer > 250) {
      // ~25% chance to spawn as ember near a torch
      var np = null;
      if (pointLights.length > 0 && Math.random() < 0.25) {
        // Find a nearby torch
        for (var _tli = 0; _tli < pointLights.length; _tli++) {
          var _tl = pointLights[_tli];
          if (_tl.r < 200) continue; // skip non-torch lights (cave entrances)
          var _td = Math.hypot(_tl.x - px, _tl.y - py);
          if (_td < 200) {
            var _eBaseZ = floorMesh ? getFloorHeightAt(_tl.x, _tl.y) * 25 : 0;
            np = {x: _tl.x + (Math.random() - 0.5) * 8, y: _tl.y + (Math.random() - 0.5) * 8,
              z: _eBaseZ + 10 + Math.random() * 5, vx: (Math.random() - 0.5) * 10, vy: (Math.random() - 0.5) * 10,
              vz: 8 + Math.random() * 12, alpha: 0, maxAlpha: 0.4 + Math.random() * 0.3,
              life: 0.8 + Math.random() * 1.2, maxLife: 0.8 + Math.random() * 1.2,
              size: 1 + Math.random(), type: 'ember', r: 255, g: 140 + Math.floor(Math.random() * 60), b: 20};
            break;
          }
        }
      }
      if (!np) np = spawnAmbientParticle(px, py, terrain);
      ambientParticles[i] = np;
    }
  }
}

function drawTorchGlow3D() {
  if (!pointLights.length || !MODE3D) return;
  var C = getCam3D();
  var w = C.w, h = C.h, cosAng = C.cosAng, sinAng = C.sinAng;
  var invTanHalf = C.invTanHalf, horizonY = C.horizonY, cameraZ = C.cameraZ;
  var darkFactor = Math.max(0, 1.0 - ambientLight);
  var glowAlphaBase = 0.08 + 0.22 * darkFactor; // subtle day, bright night
  var now = Date.now();

  ctx.save();
  ctx.globalCompositeOperation = 'lighter';
  for (var i = 0; i < pointLights.length; i++) {
    var light = pointLights[i];
    var dx = light.x - cam.x, dy = light.y - cam.y;
    var distSq = dx * dx + dy * dy;
    if (distSq > 400 * 400) continue;
    var fwd = dx * cosAng + dy * sinAng;
    if (fwd < 5) continue;
    var rgt = dx * (-sinAng) + dy * cosAng;
    var screenX = Math.floor((rgt / fwd * invTanHalf * 0.5 + 0.5) * w);
    var wz = Number.isFinite(light.z) ? light.z : (floorMesh ? getFloorHeightAt(light.x, light.y) * 25 : 0) + 15;
    var screenY = Math.floor(horizonY + ((cameraZ - wz) / fwd) * projScale);
    var glowRadius = Math.max(10, Math.floor(w * 0.12 * (120 * getScale3D('smGlow') / fwd)));
    var flicker = 0.95 + 0.03 * Math.sin(now * 0.007 + i * 2.3) + 0.02 * Math.sin(now * 0.013 + i * 5.7);
    var alpha = glowAlphaBase * flicker * Math.max(0.2, 1.0 - Math.sqrt(distSq) / 400);
    var grad = ctx.createRadialGradient(screenX, screenY, 0, screenX, screenY, glowRadius);
    grad.addColorStop(0, 'rgba(' + light.r + ',' + light.g + ',' + light.b + ',' + (alpha * 0.7).toFixed(3) + ')');
    grad.addColorStop(0.4, 'rgba(' + light.r + ',' + Math.floor(light.g * 0.7) + ',' + Math.floor(light.b * 0.3) + ',' + (alpha * 0.3).toFixed(3) + ')');
    grad.addColorStop(1, 'rgba(' + light.r + ',' + light.g + ',' + light.b + ',0)');
    ctx.fillStyle = grad;
    withSceneDepthClip([
      {x:screenX-glowRadius,y:screenY-glowRadius,depth:fwd},
      {x:screenX+glowRadius,y:screenY-glowRadius,depth:fwd},
      {x:screenX+glowRadius,y:screenY+glowRadius,depth:fwd},
      {x:screenX-glowRadius,y:screenY+glowRadius,depth:fwd}
    ], function() {
      ctx.fillRect(screenX - glowRadius, screenY - glowRadius, glowRadius * 2, glowRadius * 2);
    });
  }
  ctx.restore();
}

function drawAmbientParticles3D() {
  if (!ambientParticles.length || !MODE3D) return;
  var C = getCam3D();
  var w = C.w, h = C.h, cosAng = C.cosAng, sinAng = C.sinAng;
  var invTanHalf = C.invTanHalf, horizonY = C.horizonY, cameraZ = C.cameraZ;

  for (var i = 0; i < ambientParticles.length; i++) {
    var p = ambientParticles[i];
    if (p.alpha < 0.02) continue;
    var dx = p.x - cam.x, dy = p.y - cam.y;
    var fwd = dx * cosAng + dy * sinAng;
    if (fwd < 1) continue;
    var rgt = dx * (-sinAng) + dy * cosAng;
    var screenX = Math.floor((rgt / fwd * invTanHalf * 0.5 + 0.5) * w);
    if (screenX < -5 || screenX > w + 5) continue;
    var screenY = horizonY + Math.floor(((cameraZ - p.z) / fwd) * projScale);
    if (screenY < -5 || screenY > h + 5) continue;
    var sz = Math.max(1, p.size * getScale3D('particle') * projScale / fwd * 0.15);

    withSceneDepthBillboard({x:screenX-sz*4-1,y:screenY-sz*4-1,
      width:sz*8+2,height:sz*8+2}, fwd, function() {
    ctx.globalAlpha = p.alpha;
    if (p.type === 'firefly' || p.type === 'ember') {
      // Glow halo
      ctx.fillStyle = 'rgba(' + p.r + ',' + p.g + ',' + p.b + ',0.15)';
      ctx.beginPath(); ctx.arc(screenX, screenY, sz * 4, 0, Math.PI * 2); ctx.fill();
    }
    ctx.fillStyle = rgbQ(p.r, p.g, p.b);
    if (p.type === 'drip') {
      ctx.fillRect(screenX, screenY, Math.max(1, sz * 0.5), Math.max(2, sz * 2));
    } else {
      ctx.beginPath(); ctx.arc(screenX, screenY, sz, 0, Math.PI * 2); ctx.fill();
    }
    });
  }
  ctx.globalAlpha = 1.0;
}

function drawShopMarker2D() {
  if (!shopMarker) return;
  var now = Date.now();
  ctx.save(); ctx.translate(-viewCam.x, -viewCam.y);
  var awningColors = {weapons: '#cc3333', potions: '#3366cc', scrolls: '#33aa55', trinkets: '#9944cc'};
  for (var i = 0; i < marketStalls.length; i++) {
    var st = marketStalls[i];
    ctx.fillStyle = awningColors[st.stallType] || '#886633';
    ctx.fillRect(st.x - 8, st.y - 5, 16, 10);
    ctx.strokeStyle = '#553311'; ctx.lineWidth = 1;
    ctx.strokeRect(st.x - 8, st.y - 5, 16, 10);
  }
  var pulse = 0.7 + 0.3 * Math.sin(now * 0.003);
  ctx.globalAlpha = pulse;
  ctx.fillStyle = '#ffcc44';
  ctx.beginPath(); ctx.arc(shopMarker.x, shopMarker.y, 6, 0, Math.PI * 2); ctx.fill();
  ctx.fillStyle = '#000'; ctx.font = 'bold 8px Arial'; ctx.textAlign = 'center';
  ctx.fillText('$', shopMarker.x, shopMarker.y + 3);
  ctx.restore();
}

var _shrineBuffColors = { damage: '#ff4444', speed: '#44ffff', regen: '#44ff44', armor: '#ffaa44' };
var _shrineBuffColorsDark = { damage: '#661111', speed: '#116666', regen: '#116611', armor: '#664411' };

function drawShrines3D() {
  renderEntities3D(shrines, {maxDist: viewDist * 0.8, depthOffset: 2, checkMidpoint: true, fadeFraction: 0.8, sort: true, minDist: 3, mode3dOnly: true},
    function(shr, vis, C, ctx, now) {
      function proj(wx, wy, wz) { return projToScreen(wx, wy, wz, C); }
      function fillQuad(p0, p1, p2, p3, color) {
        ctx.fillStyle = color; ctx.beginPath();
        ctx.moveTo(p0.sx, p0.sy); ctx.lineTo(p1.sx, p1.sy);
        ctx.lineTo(p2.sx, p2.sy); ctx.lineTo(p3.sx, p3.sy);
        ctx.closePath(); ctx.fill();
      }
      var fwd = vis.fwd, floorZ = vis.floorZ;
      var fogAlpha = Math.max(0.5, 1.0 - fwd / viewDist * 0.4) * vis.fade;
      ctx.save(); ctx.globalAlpha = fogAlpha;

      var pw = 10, pd = 10, pedestalH = 18;
      var bl = proj(shr.x - pw, shr.y - pd, floorZ);
      var br = proj(shr.x + pw, shr.y - pd, floorZ);
      var fr = proj(shr.x + pw, shr.y + pd, floorZ);
      var fl = proj(shr.x - pw, shr.y + pd, floorZ);
      var tl = proj(shr.x - pw, shr.y - pd, floorZ + pedestalH);
      var tr = proj(shr.x + pw, shr.y - pd, floorZ + pedestalH);
      var tfr = proj(shr.x + pw, shr.y + pd, floorZ + pedestalH);
      var tfl = proj(shr.x - pw, shr.y + pd, floorZ + pedestalH);
      if (!bl || !br || !fr || !fl || !tl || !tr || !tfr || !tfl) { ctx.restore(); return; }

      var dx = cam.x - shr.x, dy = cam.y - shr.y;
      if (dy > 0) fillQuad(fl, fr, tfr, tfl, '#777777');
      else fillQuad(bl, br, tr, tl, '#777777');
      if (dx > 0) fillQuad(fr, br, tr, tfr, '#666666');
      else fillQuad(fl, bl, tl, tfl, '#666666');
      fillQuad(tl, tr, tfr, tfl, '#888888');

      var crystalZ = floorZ + pedestalH + 10 + Math.sin(now * 0.003 + vis.dist * 0.01) * 4;
      var crystalColor = shr.used ? '#555555' : (_shrineBuffColors[shr.buffType] || '#ffffff');
      var crystalSize = 7;
      var ct = proj(shr.x, shr.y, crystalZ + crystalSize * 2);
      var cb = proj(shr.x, shr.y, crystalZ - crystalSize);
      var cl = proj(shr.x - crystalSize, shr.y, crystalZ + crystalSize * 0.5);
      var cr = proj(shr.x + crystalSize, shr.y, crystalZ + crystalSize * 0.5);
      var cf = proj(shr.x, shr.y + crystalSize, crystalZ + crystalSize * 0.5);
      var ck = proj(shr.x, shr.y - crystalSize, crystalZ + crystalSize * 0.5);
      if (ct && cb && cl && cr) {
        ctx.fillStyle = crystalColor;
        ctx.beginPath(); ctx.moveTo(ct.sx, ct.sy); ctx.lineTo(cl.sx, cl.sy); ctx.lineTo(cb.sx, cb.sy); ctx.lineTo(cr.sx, cr.sy); ctx.closePath(); ctx.fill();
        var darkColor = shr.used ? '#444444' : (_shrineBuffColorsDark[shr.buffType] || '#888888');
        if (cf && ck) {
          ctx.fillStyle = darkColor; ctx.globalAlpha = fogAlpha * 0.7;
          ctx.beginPath(); ctx.moveTo(ct.sx, ct.sy); ctx.lineTo(cf.sx, cf.sy); ctx.lineTo(cb.sx, cb.sy); ctx.lineTo(ck.sx, ck.sy); ctx.closePath(); ctx.fill();
          ctx.globalAlpha = fogAlpha;
        }
      }
      if (!shr.used && ct) {
        var glowR = Math.max(16, Math.min(80, 28 * getScale3D('lgGlow') * projScale / fwd));
        var pulse = 0.5 + 0.3 * Math.sin(now * 0.004 + vis.dist * 0.01);
        ctx.globalAlpha = fogAlpha * pulse * 0.5;
        var grad = ctx.createRadialGradient(ct.sx, ct.sy - glowR * 0.3, 0, ct.sx, ct.sy - glowR * 0.3, glowR);
        grad.addColorStop(0, crystalColor); grad.addColorStop(1, 'rgba(0,0,0,0)');
        ctx.fillStyle = grad;
        ctx.beginPath(); ctx.arc(ct.sx, ct.sy - glowR * 0.3, glowR, 0, Math.PI * 2); ctx.fill();
        ctx.globalAlpha = fogAlpha;
      }
      var buffLabel = shr.buffType === 'damage' ? 'Power' : shr.buffType === 'speed' ? 'Swiftness' : shr.buffType === 'regen' ? 'Regeneration' : 'Protection';
      if (!shr.used && vis.dist < viewDist * 0.5) {
        var labelProj = proj(shr.x, shr.y, floorZ + pedestalH + 30);
        if (labelProj) {
          var labelSize = Math.max(11, Math.floor(22 * getScale3D('smText') * projScale / fwd));
          ctx.globalAlpha = fogAlpha * Math.min(1, 0.4 + 0.6 * (1 - vis.dist / (viewDist * 0.5)));
          ctx.fillStyle = crystalColor;
          ctx.font = 'bold ' + labelSize + 'px monospace';
          ctx.textAlign = 'center';
          ctx.fillText(buffLabel, labelProj.sx, labelProj.sy);
        }
      }
      if (nearestShrine === shr && !shr.used) {
        var promptProj = proj(shr.x, shr.y, floorZ + pedestalH + 40);
        if (promptProj) {
          ctx.globalAlpha = 1; ctx.fillStyle = '#ffffff';
          ctx.font = 'bold ' + Math.max(14, Math.floor(28 * getScale3D('lgText') * projScale / fwd)) + 'px monospace';
          ctx.textAlign = 'center';
          ctx.fillText('Press E', promptProj.sx, promptProj.sy);
        }
      }
      ctx.restore();
    });
}

// Ruin dressing is authored in structure-local world units. Local +Y points
// out through the ruin's open face and local +X points to its right. Keeping
// these positions independent of camera distance prevents the old rubble ring
// from orbiting the ruin as the player approached it.
var RUIN_DEBRIS_LAYOUTS = {
  hut: [
    [-15, 37, 5.0, 3.0, 0.20], [18, 35, 3.8, 2.5, 0.85],
    [35, 16, 5.5, 3.4, 0.48], [-37, -24, 4.5, 2.8, 1.18]
  ],
  tower_base: [
    [-31, 30, 4.6, 3.0, 0.30], [12, 38, 5.4, 3.6, 0.94],
    [39, 8, 3.8, 2.5, 0.58], [25, -34, 5.0, 3.1, 1.30],
    [-25, -30, 3.6, 2.4, 0.72]
  ],
  hall: [
    [-34, 63, 5.4, 3.4, 0.20], [3, 66, 4.0, 2.7, 0.86],
    [41, 58, 5.8, 3.8, 0.44], [64, 25, 4.5, 2.8, 1.24],
    [-62, -43, 5.2, 3.3, 0.68], [-34, -63, 4.2, 2.6, 1.48]
  ]
};

function ruinLocalToWorld(ru, localX, localY, out) {
  out = out || {};
  var facing = (ru.facing | 0) & 3;
  if (facing === 0) { out.x = ru.x + localX; out.y = ru.y - localY; }
  else if (facing === 1) { out.x = ru.x + localY; out.y = ru.y + localX; }
  else if (facing === 2) { out.x = ru.x - localX; out.y = ru.y + localY; }
  else { out.x = ru.x - localY; out.y = ru.y - localX; }
  return out;
}

function getRuinDebrisWorld(ru, index, out) {
  var layout = RUIN_DEBRIS_LAYOUTS[ru.ruinType] || RUIN_DEBRIS_LAYOUTS.hut;
  if (index < 0 || index >= layout.length) return null;
  var authored = layout[index];
  out = ruinLocalToWorld(ru, authored[0], authored[1], out);
  out.size = authored[2]; out.height = authored[3];
  out.yaw = authored[4] + ((ru.facing | 0) & 3) * Math.PI * 0.5;
  return out;
}

function getRuinPostWorld(ru, out) {
  var forward = ru.ruinType === 'hall' ? 67 : ru.ruinType === 'tower_base' ? 42 : 39;
  out = ruinLocalToWorld(ru, 18, forward, out);
  var facing = (ru.facing | 0) & 3;
  out.rightX = facing === 0 ? 1 : facing === 2 ? -1 : 0;
  out.rightY = facing === 1 ? 1 : facing === 3 ? -1 : 0;
  out.outX = facing === 1 ? 1 : facing === 3 ? -1 : 0;
  out.outY = facing === 2 ? 1 : facing === 0 ? -1 : 0;
  return out;
}

function ruinDecorSurfaceZ(wx, wy, fallbackZ) {
  var z = typeof getEntityGroundRenderZ === 'function' ? getEntityGroundRenderZ(wx, wy, false) : NaN;
  return Number.isFinite(z) ? z : (Number.isFinite(fallbackZ) ? fallbackZ : 0);
}

function drawRuinDecorFace(vertices, color, C) {
  var projected = projectSceneWorldPolygon(vertices, C);
  if (!projected || projected.length < 3) return 0;
  ctx.fillStyle = color;
  return fillSceneDepthPolygon(projected);
}

function drawRuinStone(point, baseZ, colors, C) {
  var cs = Math.cos(point.yaw), sn = Math.sin(point.yaw);
  var ux = cs * point.size, uy = sn * point.size;
  var vx = -sn * point.size * 0.68, vy = cs * point.size * 0.68;
  var z0 = baseZ + 0.25, z1 = z0 + point.height;
  var topScale = 0.56, leanX = ux * 0.12, leanY = uy * 0.12;
  var b0 = {x:point.x-ux-vx,y:point.y-uy-vy,z:z0};
  var b1 = {x:point.x+ux-vx,y:point.y+uy-vy,z:z0};
  var b2 = {x:point.x+ux+vx,y:point.y+uy+vy,z:z0};
  var b3 = {x:point.x-ux+vx,y:point.y-uy+vy,z:z0};
  var t0 = {x:point.x-ux*topScale-vx*topScale+leanX,y:point.y-uy*topScale-vy*topScale+leanY,z:z1};
  var t1 = {x:point.x+ux*topScale-vx*topScale+leanX,y:point.y+uy*topScale-vy*topScale+leanY,z:z1};
  var t2 = {x:point.x+ux*topScale+vx*topScale+leanX,y:point.y+uy*topScale+vy*topScale+leanY,z:z1};
  var t3 = {x:point.x-ux*topScale+vx*topScale+leanX,y:point.y-uy*topScale+vy*topScale+leanY,z:z1};
  drawRuinDecorFace([b0,b1,t1,t0], colors.shadow, C);
  drawRuinDecorFace([b1,b2,t2,t1], colors.base, C);
  drawRuinDecorFace([b2,b3,t3,t2], colors.dark, C);
  drawRuinDecorFace([b3,b0,t0,t3], colors.shadow, C);
  drawRuinDecorFace([t0,t1,t2,t3], colors.lit, C);
}

function drawRuinDecorBox(center, z0, z1, halfRight, halfOut, colors, C) {
  var rx = center.rightX * halfRight, ry = center.rightY * halfRight;
  var ox = center.outX * halfOut, oy = center.outY * halfOut;
  var b0 = {x:center.x-rx-ox,y:center.y-ry-oy,z:z0};
  var b1 = {x:center.x+rx-ox,y:center.y+ry-oy,z:z0};
  var b2 = {x:center.x+rx+ox,y:center.y+ry+oy,z:z0};
  var b3 = {x:center.x-rx+ox,y:center.y-ry+oy,z:z0};
  var t0 = {x:b0.x,y:b0.y,z:z1}, t1 = {x:b1.x,y:b1.y,z:z1};
  var t2 = {x:b2.x,y:b2.y,z:z1}, t3 = {x:b3.x,y:b3.y,z:z1};
  drawRuinDecorFace([b0,b1,t1,t0], colors.dark, C);
  drawRuinDecorFace([b1,b2,t2,t1], colors.mid, C);
  drawRuinDecorFace([b2,b3,t3,t2], colors.deep, C);
  drawRuinDecorFace([b3,b0,t0,t3], colors.dark, C);
  drawRuinDecorFace([t0,t1,t2,t3], colors.lit, C);
}

function drawRuins3D() {
  // Match the wall renderer's range: opaque dressing is too small to notice at
  // the horizon, and no longer hard-culls while its wall remnants are visible.
  renderEntities3D(ruins, {maxDist: viewDist, groundAnchor: true, sceneDepth: true,
    fadeFraction: 0.8, sort: true, minDist: 3, mode3dOnly: true},
    function(ru, vis, C, ctx, now) {
      function proj(wx, wy, wz) { return projToScreen(wx, wy, wz, C); }
      var fwd = vis.fwd;
      ctx.save();
      // These faces write opaque scene depth, so their paint must be opaque as
      // well. Distance fading is reserved for the non-opaque location label.
      var labelAlpha = Math.max(0.3, 1.0 - fwd / viewDist * 0.6) * vis.fade;
      ctx.globalAlpha = 1;

      var stoneColors = (typeof GAME_MATERIALS !== 'undefined' && GAME_MATERIALS.rubbleStone) ?
        GAME_MATERIALS.rubbleStone.hex : {base:'#787060',shadow:'#686058',lit:'#888070',dark:'#504840'};
      var debrisLayout = RUIN_DEBRIS_LAYOUTS[ru.ruinType] || RUIN_DEBRIS_LAYOUTS.hut;
      var debrisPoint = {};
      for (var rbi = 0; rbi < debrisLayout.length; rbi++) {
        getRuinDebrisWorld(ru, rbi, debrisPoint);
        var debrisZ = ruinDecorSurfaceZ(debrisPoint.x, debrisPoint.y, vis.floorZ);
        drawRuinStone(debrisPoint, debrisZ, stoneColors, C);
      }

      // A small solid waypost sits to the right of the open face. Both its
      // stem and crosspiece are world boxes, so they retain their orientation
      // instead of turning into screen-space lines as the camera moves.
      var post = getRuinPostWorld(ru, {});
      var postZ = ruinDecorSurfaceZ(post.x, post.y, vis.floorZ);
      var woodColors = (typeof GAME_MATERIALS !== 'undefined' && GAME_MATERIALS.palisadeWood) ?
        GAME_MATERIALS.palisadeWood.hex : {lit:'#7a4c2a',mid:'#6b4226',dark:'#5a3720',deep:'#3a2412'};
      drawRuinDecorBox(post, postZ + 0.2, postZ + 21, 0.9, 0.9, woodColors, C);
      drawRuinDecorBox(post, postZ + 14, postZ + 19, 8.5, 1.0, woodColors, C);

      // Two fixed roof-frame remnants give the hut a readable shelter
      // silhouette without closing it in. They use the same facing basis as
      // the waypost and stay attached to the rear/left wall runs.
      if (ru.ruinType === 'hut') {
        var rearBeam = ruinLocalToWorld(ru, 0, -27, {});
        rearBeam.rightX = post.rightX; rearBeam.rightY = post.rightY;
        rearBeam.outX = post.outX; rearBeam.outY = post.outY;
        var rearBeamZ = ruinDecorSurfaceZ(rearBeam.x, rearBeam.y, vis.floorZ);
        drawRuinDecorBox(rearBeam, rearBeamZ + 52, rearBeamZ + 56, 28, 1.8, woodColors, C);
        var sideBeam = ruinLocalToWorld(ru, -27, -5, {});
        sideBeam.rightX = post.rightX; sideBeam.rightY = post.rightY;
        sideBeam.outX = post.outX; sideBeam.outY = post.outY;
        var sideBeamZ = ruinDecorSurfaceZ(sideBeam.x, sideBeam.y, vis.floorZ);
        drawRuinDecorBox(sideBeam, sideBeamZ + 38, sideBeamZ + 42, 1.8, 18, woodColors, C);
      }

      if (fwd < viewDist * 0.3) {
        var labelLift = ru.ruinType === 'hut' ? 68 : ru.ruinType === 'hall' ? 74 : 62;
        var labelP = proj(ru.x, ru.y, vis.floorZ + labelLift);
        if (labelP && labelP.fwd > 1) {
          ctx.globalAlpha = labelAlpha;
          ctx.fillStyle = '#c8b898';
          var labelSize = Math.max(9, Math.min(24, Math.floor(12 * projScale / labelP.fwd)));
          ctx.font = labelSize + 'px monospace';
          ctx.textAlign = 'center';
          var ruinLabel = ru.ruinType === 'hut' ? 'Ruined Hut' : ru.ruinType === 'tower_base' ? 'Tower Ruins' : 'Ruined Hall';
          var labelWidth = ctx.measureText(ruinLabel).width;
          withSceneDepthBillboard({x:labelP.sx-labelWidth*0.5-1,y:labelP.sy-labelSize,
            width:labelWidth+2,height:labelSize+3}, labelP.fwd, function() {
            ctx.fillText(ruinLabel, labelP.sx, labelP.sy);
          });
        }
      }
      ctx.restore();
    });
}

function getStructureShellMetrics(st) {
  var scale = st.scale || 1;
  var normalized = ((st.type === 'fortress') ? 0.9 : (st.type === 'watchtower') ? 1.0 : 0.7) *
    (0.9 + scale * 0.1);
  return {scale:scale, normalized:normalized, wallWorldH:CANVAS_BASE_H * normalized};
}

function structurePaletteRole(st, role) {
  var palette = st.palette;
  if (palette && palette[role]) return palette[role];
  if (st.type === 'arena') return role === 'p' ? [100,60,45] : role === 's' ? [115,65,50] : [130,75,55];
  if (st.type === 'watchtower') return role === 't' ? [120,130,155] : role === 'a' ? [110,120,140] : [110,120,140];
  return role === 'c' ? [155,150,140] : role === 'k' ? [150,145,135] : [140,135,125];
}

function structureColorRamp(st, role) {
  var rgb = structurePaletteRole(st, role);
  function shade(delta) {
    return rgbQ(Math.max(0, Math.min(255, rgb[0] + delta)),
      Math.max(0, Math.min(255, rgb[1] + delta)),
      Math.max(0, Math.min(255, rgb[2] + delta)));
  }
  return {lit:shade(18), mid:shade(4), dark:shade(-18), deep:shade(-34), shadow:shade(-25), base:shade(0)};
}

function drawStructureBoxWorld(x, y, z0, z1, halfRight, halfOut, yaw, colors, C) {
  var cs = Math.cos(yaw), sn = Math.sin(yaw);
  drawRuinDecorBox({x:x,y:y,rightX:cs,rightY:sn,outX:-sn,outY:cs},
    z0,z1,halfRight,halfOut,colors,C);
}

function drawStructurePyramidWorld(x, y, zBase, zPeak, halfRight, halfOut, yaw, colors, C) {
  var cs=Math.cos(yaw), sn=Math.sin(yaw), rx=cs*halfRight, ry=sn*halfRight;
  var ox=-sn*halfOut, oy=cs*halfOut;
  var b0={x:x-rx-ox,y:y-ry-oy,z:zBase}, b1={x:x+rx-ox,y:y+ry-oy,z:zBase};
  var b2={x:x+rx+ox,y:y+ry+oy,z:zBase}, b3={x:x-rx+ox,y:y-ry+oy,z:zBase};
  var apex={x:x,y:y,z:zPeak};
  drawRuinDecorFace([b0,b1,apex],colors.dark,C);
  drawRuinDecorFace([b1,b2,apex],colors.mid,C);
  drawRuinDecorFace([b2,b3,apex],colors.base,C);
  drawRuinDecorFace([b3,b0,apex],colors.shadow,C);
}

function drawStructureGableWorld(x, y, zEave, zPeak, halfRight, halfOut, yaw, colors, C) {
  var cs=Math.cos(yaw), sn=Math.sin(yaw), rx=cs*halfRight, ry=sn*halfRight;
  var ox=-sn*halfOut, oy=cs*halfOut;
  var b0={x:x-rx-ox,y:y-ry-oy,z:zEave}, b1={x:x+rx-ox,y:y+ry-oy,z:zEave};
  var b2={x:x+rx+ox,y:y+ry+oy,z:zEave}, b3={x:x-rx+ox,y:y-ry+oy,z:zEave};
  var r0={x:x-rx,y:y-ry,z:zPeak}, r1={x:x+rx,y:y+ry,z:zPeak};
  drawRuinDecorFace([b0,b1,r1,r0],colors.dark,C);
  drawRuinDecorFace([b3,r0,r1,b2],colors.base,C);
  drawRuinDecorFace([b0,r0,b3],colors.shadow,C);
  drawRuinDecorFace([b1,b2,r1],colors.mid,C);
}

function drawStructureRadialRoof(st, radius, sides, rimZ, peakZ, colors, C) {
  var rotation = (st.rotation || 0) + Math.PI / sides;
  var apex={x:st.x,y:st.y,z:peakZ};
  for (var face=0;face<sides;face++) {
    var a0=rotation+face*Math.PI*2/sides, a1=rotation+(face+1)*Math.PI*2/sides;
    var p0={x:st.x+Math.cos(a0)*radius,y:st.y+Math.sin(a0)*radius,z:rimZ};
    var p1={x:st.x+Math.cos(a1)*radius,y:st.y+Math.sin(a1)*radius,z:rimZ};
    drawRuinDecorFace([p0,p1,apex],
      face%3===0?colors.lit:face%2===0?colors.mid:colors.dark,C);
    var mid=(a0+a1)*0.5, chord=radius*Math.sin(Math.PI/sides);
    drawStructureBoxWorld(st.x+Math.cos(mid)*radius*Math.cos(Math.PI/sides),
      st.y+Math.sin(mid)*radius*Math.cos(Math.PI/sides),rimZ-3,rimZ+3,
      chord,cell*0.12,mid+Math.PI*0.5,colors,C);
  }
}

function drawStructureStandardWorld(x, y, baseZ, yaw, height, color, C) {
  var wood = GAME_MATERIALS.floorPropWood.hex;
  var woodRamp={lit:wood.lit,mid:wood.base,dark:wood.shadow,deep:wood.deep};
  drawStructureBoxWorld(x,y,baseZ+0.2,baseZ+height,1.1,1.1,0,woodRamp,C);
  var rx=Math.cos(yaw)*cell*0.48, ry=Math.sin(yaw)*cell*0.48;
  var ox=-Math.sin(yaw)*1.2, oy=Math.cos(yaw)*1.2;
  var clothTop=baseZ+height*0.88, clothBottom=baseZ+height*0.55;
  drawRuinDecorFace([
    {x:x-rx+ox,y:y-ry+oy,z:clothTop},{x:x+rx+ox,y:y+ry+oy,z:clothTop},
    {x:x+rx+ox,y:y+ry+oy,z:clothBottom},{x:x-rx+ox,y:y-ry+oy,z:clothBottom}
  ],color,C);
}

function drawStructureShellWorld(st, vis, C) {
  if (!Number.isFinite(vis.floorZ)) return;
  var metrics=getStructureShellMetrics(st), scale=metrics.scale, baseZ=vis.floorZ;
  var mainRole=st.type==='fortress'?'w':st.type==='arena'?'w':'t';
  var trimRole=st.type==='fortress'?'k':st.type==='arena'?'p':'a';
  var main=structureColorRamp(st,mainRole), trim=structureColorRamp(st,trimRole);
  var accent=st.type==='fortress'?'#b58a35':st.type==='arena'?'#9f3d2d':'#3f7597';
  ctx.globalAlpha=1;
  if (st.type==='fortress') {
    var extent=CHUNK_SIZE*1.7*scale, gateInset=extent-cell*1.5;
    var gateZ0=baseZ+metrics.wallWorldH*0.68, gateZ1=baseZ+metrics.wallWorldH*0.90;
    drawStructureBoxWorld(st.x,st.y-gateInset,gateZ0,gateZ1,cell*2.9,cell*0.48,0,trim,C);
    drawStructureBoxWorld(st.x,st.y+gateInset,gateZ0,gateZ1,cell*2.9,cell*0.48,0,trim,C);
    drawStructureBoxWorld(st.x-gateInset,st.y,gateZ0,gateZ1,cell*2.9,cell*0.48,Math.PI*0.5,trim,C);
    drawStructureBoxWorld(st.x+gateInset,st.y,gateZ0,gateZ1,cell*2.9,cell*0.48,Math.PI*0.5,trim,C);
    var cornerOffset=extent-cell*2.5;
    for(var fy=-1;fy<=1;fy+=2) for(var fx=-1;fx<=1;fx+=2) {
      var towerX=st.x+fx*cornerOffset,towerY=st.y+fy*cornerOffset;
      var towerTop=baseZ+CANVAS_BASE_H*(metrics.normalized+0.3);
      drawStructureBoxWorld(towerX,towerY,towerTop,towerTop+8,cell*2.35,cell*2.35,0,trim,C);
      drawStructurePyramidWorld(towerX,towerY,towerTop+8,towerTop+cell*1.45,
        cell*1.25,cell*1.25,Math.PI*0.25,trim,C);
    }
    var pillarTop=baseZ+CANVAS_BASE_H*(metrics.normalized+0.6)+3;
    drawStructureRadialRoof(st,cell*10*scale,6,pillarTop,pillarTop+cell*2.3,trim,C);
    var buildingZ=baseZ+CANVAS_BASE_H*(metrics.normalized*0.7)+3;
    if(st.numBuildings>=1) drawStructureGableWorld(st.x+extent*0.45,st.y-extent*0.4,
      buildingZ,buildingZ+cell*1.8,cell*4.25,cell*3.25,0,main,C);
    if(st.numBuildings>=2) drawStructureGableWorld(st.x-extent*0.45,st.y+extent*0.4,
      buildingZ,buildingZ+cell*1.8,cell*4.25,cell*3.25,0,main,C);
    drawStructureStandardWorld(st.x,st.y-gateInset,baseZ,0,metrics.wallWorldH*0.92,accent,C);
    drawStructureStandardWorld(st.x,st.y+gateInset,baseZ,Math.PI,metrics.wallWorldH*0.92,accent,C);
    drawStructureStandardWorld(st.x-gateInset,st.y,baseZ,-Math.PI*0.5,metrics.wallWorldH*0.92,accent,C);
    drawStructureStandardWorld(st.x+gateInset,st.y,baseZ,Math.PI*0.5,metrics.wallWorldH*0.92,accent,C);
  } else if(st.type==='arena') {
    var ringR=CHUNK_SIZE*1.1*scale, rotation=st.rotation||0;
    for(var gi=-1;gi<=1;gi+=2) {
      var gateA=rotation+gi*Math.PI*0.5, gateX=st.x+Math.cos(gateA)*ringR, gateY=st.y+Math.sin(gateA)*ringR;
      drawStructureBoxWorld(gateX,gateY,baseZ+metrics.wallWorldH*0.62,
        baseZ+metrics.wallWorldH*0.88,cell*2.8,cell*0.5,gateA+Math.PI*0.5,main,C);
      drawStructureStandardWorld(gateX,gateY,baseZ,gateA+Math.PI*0.5,metrics.wallWorldH*0.9,accent,C);
    }
    var pillars=Math.max(8,Math.min(15,st.numPillars||8)), pillarR=ringR*0.55;
    var entZ=baseZ+CANVAS_BASE_H*(metrics.normalized+0.2);
    for(var pi=0;pi<pillars;pi++) {
      var midA=rotation+(pi+0.5)*Math.PI*2/pillars;
      drawStructureBoxWorld(st.x+Math.cos(midA)*pillarR*Math.cos(Math.PI/pillars),
        st.y+Math.sin(midA)*pillarR*Math.cos(Math.PI/pillars),entZ-3,entZ+4,
        pillarR*Math.sin(Math.PI/pillars),cell*0.11,midA+Math.PI*0.5,trim,C);
    }
    var pedestalTop=baseZ+metrics.wallWorldH*0.18;
    drawStructureBoxWorld(st.x,st.y,pedestalTop,pedestalTop+6,cell*0.78,cell*0.78,rotation,trim,C);
  } else {
    var towerR=cell*3*scale, towerRoof=baseZ+metrics.wallWorldH*1.4+3;
    drawStructurePyramidWorld(st.x,st.y,towerRoof,towerRoof+cell*3.0,
      towerR*1.08,towerR*1.08,(st.rotation||0)+Math.PI*0.25,trim,C);
    var arms=Math.max(2,Math.min(4,st.armCount||2)), armLen=cell*14*scale;
    for(var ai=0;ai<arms;ai++) {
      var armA=(st.rotation||0)+ai*Math.PI*2/arms;
      var roomDist=towerR+armLen, roomX=st.x+Math.cos(armA)*roomDist, roomY=st.y+Math.sin(armA)*roomDist;
      var roomRoof=baseZ+metrics.wallWorldH*0.9+3, roomR=cell*2.5*scale;
      drawStructurePyramidWorld(roomX,roomY,roomRoof,roomRoof+cell*1.65,
        roomR*1.08,roomR*1.08,armA+Math.PI*0.25,main,C);
      drawStructureStandardWorld(roomX,roomY,baseZ,armA+Math.PI*0.5,metrics.wallWorldH*0.82,accent,C);
    }
  }
}

function drawStructures3D() {
  renderEntities3D(largeStructures, {maxDist: viewDist * 2.6, depthOffset: 2, checkMidpoint: true, fadeFraction: 0.92, sort: true, minDist: 3, mode3dOnly: true, groundAnchor:true},
    function(st, vis, C, ctx, now) {
      function proj(wx, wy, wz) { return projToScreen(wx, wy, wz, C); }
      var fwd = vis.fwd;
      ctx.save();
      ctx.globalAlpha = vis.fade;
      drawStructureShellWorld(st,vis,C);
      ctx.globalAlpha = vis.fade;

      if (fwd < viewDist * 0.6) {
        var labelP = proj(st.x, st.y, vis.floorZ + 40);
        if (labelP) {
          var labelSize = Math.max(10, Math.floor(16 * projScale / labelP.fwd));
          ctx.fillStyle = (st.type === 'fortress') ? '#e8c868' : (st.type === 'arena') ? '#e87848' : '#88c8e8';
          ctx.font = 'bold ' + labelSize + 'px monospace';
          ctx.textAlign = 'center';
          var label = (st.type === 'fortress') ? 'Ancient Fortress' : (st.type === 'arena') ? 'Battle Arena' : 'Watchtower';
          ctx.fillText(label, labelP.sx, labelP.sy);
          ctx.fillStyle = '#aaa';
          ctx.font = Math.max(8, labelSize - 3) + 'px monospace';
          ctx.fillText('Danger Zone', labelP.sx, labelP.sy + labelSize + 2);
        }
      }

      // ── Fortress keep interactables (forge, lectern, garrison banner) ──
      if (st.type === 'fortress') {
        var _fKey2 = st.regionX + ',' + st.regionY;
        var _fComp2 = completedFortresses[_fKey2] || {};
        var _interacts3d = [
          {type: 'forge',    ox: 0,         oy: -cell * 4, used: !!_fComp2.forge,    color: '#ff8800', label: '[E] Enchantment Forge'},
          {type: 'lectern',  ox: cell * 4,  oy: cell * 2,  used: !!_fComp2.lectern,  color: '#6688ff', label: '[E] Ancient Lectern'},
          {type: 'garrison', ox: -cell * 4, oy: cell * 2,  used: !!_fComp2.garrison, color: '#c8a028', label: '[E] Rally Garrison'}
        ];
        for (var _ii3d = 0; _ii3d < _interacts3d.length; _ii3d++) {
          var _int3d = _interacts3d[_ii3d];
          if (_int3d.used) continue;
          var ix = st.x + _int3d.ox, iy = st.y + _int3d.oy;
          // Base platform
          var ib0 = proj(ix - 4, iy - 4, vis.floorZ);
          var ib1 = proj(ix + 4, iy - 4, vis.floorZ);
          var ib2 = proj(ix + 4, iy + 4, vis.floorZ);
          var ib3 = proj(ix - 4, iy + 4, vis.floorZ);
          if (ib0 && ib1 && ib2 && ib3) {
            ctx.fillStyle = '#333'; ctx.globalAlpha = 0.8;
            ctx.beginPath(); ctx.moveTo(ib0.sx, ib0.sy); ctx.lineTo(ib1.sx, ib1.sy);
            ctx.lineTo(ib2.sx, ib2.sy); ctx.lineTo(ib3.sx, ib3.sy); ctx.closePath(); ctx.fill();
          }
          // Object body
          var ip0 = proj(ix, iy, vis.floorZ);
          var ip1 = proj(ix, iy, vis.floorZ + 18);
          if (ip0 && ip1 && ip0.fwd > 1) {
            var ipw = Math.max(3, Math.floor(8 * projScale / ip0.fwd));
            if (_int3d.type === 'forge') {
              // Anvil: dark trapezoid
              ctx.fillStyle = '#444'; ctx.fillRect(ip0.sx - ipw, ip1.sy, ipw * 2, ip0.sy - ip1.sy);
              ctx.fillStyle = '#666'; ctx.fillRect(ip0.sx - ipw * 1.3, ip1.sy, ipw * 2.6, (ip0.sy - ip1.sy) * 0.3);
              // Orange glow
              ctx.globalAlpha = 0.4 + 0.2 * Math.sin(Date.now() * 0.004);
              ctx.fillStyle = '#ff6600';
              ctx.beginPath(); ctx.arc(ip0.sx, ip1.sy, ipw * 1.5, 0, Math.PI * 2); ctx.fill();
            } else if (_int3d.type === 'lectern') {
              // Thin pedestal + book
              ctx.fillStyle = '#555'; ctx.fillRect(ip0.sx - ipw * 0.4, ip1.sy, ipw * 0.8, ip0.sy - ip1.sy);
              ctx.fillStyle = '#8866cc'; ctx.fillRect(ip0.sx - ipw, ip1.sy - ipw * 0.5, ipw * 2, ipw);
              // Blue glow
              ctx.globalAlpha = 0.3 + 0.2 * Math.sin(Date.now() * 0.003);
              ctx.fillStyle = '#4466ff';
              ctx.beginPath(); ctx.arc(ip0.sx, ip1.sy - ipw * 0.3, ipw * 1.5, 0, Math.PI * 2); ctx.fill();
            } else {
              // Garrison banner pole + flag
              ctx.strokeStyle = '#5a4a3a'; ctx.lineWidth = Math.max(2, ipw * 0.4);
              ctx.beginPath(); ctx.moveTo(ip0.sx, ip0.sy); ctx.lineTo(ip1.sx, ip1.sy); ctx.stroke();
              var flagW = Math.max(4, ipw * 1.5);
              var flagH = Math.max(6, ipw * 2);
              var wave = Math.sin(Date.now() * 0.005) * flagW * 0.2;
              ctx.fillStyle = _int3d.color;
              ctx.beginPath();
              ctx.moveTo(ip1.sx, ip1.sy);
              ctx.lineTo(ip1.sx + flagW + wave, ip1.sy + flagH * 0.3);
              ctx.lineTo(ip1.sx + flagW * 0.8, ip1.sy + flagH);
              ctx.lineTo(ip1.sx, ip1.sy + flagH * 0.8);
              ctx.closePath(); ctx.fill();
            }
            ctx.globalAlpha = vis.fade;
          }
          // Prompt text when player is near
          if (nearestFortressInteract && nearestFortressInteract.type === _int3d.type &&
              nearestFortressInteract.structure === st) {
            var tp3 = proj(ix, iy, vis.floorZ + 28);
            if (tp3) {
              var fs3 = Math.max(10, Math.floor(18 * getScale3D('lgText') * projScale / (tp3.fwd || 10)));
              ctx.globalAlpha = 1.0;
              ctx.fillStyle = '#ffffff';
              ctx.font = 'bold ' + fs3 + 'px monospace'; ctx.textAlign = 'center';
              ctx.fillText(_int3d.label, tp3.sx, tp3.sy);
            }
          }
        }

        // ── Sealed rune barrier — pulsing red ring when fortress enemies remain ──
        var _fKey3 = st.regionX + ',' + st.regionY;
        var _fComp3 = completedFortresses[_fKey3] || {};
        var _allUsed = _fComp3.forge && _fComp3.lectern && _fComp3.garrison;
        if (!_allUsed) {
          // Check if any alive enemies are inside the fortress
          var _fortInnerR3 = CHUNK_SIZE * 1.5 * (st.scale || 1);
          var _hasEnemies = false;
          for (var _fei3 = 0; _fei3 < enemies.length; _fei3++) {
            var _fen3 = enemies[_fei3];
            if (_fen3.health <= 0) continue;
            if (Math.hypot(_fen3.x - st.x, _fen3.y - st.y) < _fortInnerR3) { _hasEnemies = true; break; }
          }
          if (_hasEnemies) {
            var _sealT = (Date.now() % 1800) / 1800;
            var _sealPulse = 0.5 + 0.5 * Math.sin(_sealT * Math.PI * 2);
            var _sealR = cell * 7 * (st.scale || 1); // ring just inside the pillars
            var _sealN = 24;
            var _sealPts = [];
            for (var _si2 = 0; _si2 < _sealN; _si2++) {
              var _sA = _si2 * Math.PI * 2 / _sealN;
              _sealPts.push(proj(st.x + Math.cos(_sA) * _sealR, st.y + Math.sin(_sA) * _sealR, vis.floorZ + 4));
            }
            var _sealVisible = true;
            for (var _sk = 0; _sk < _sealN; _sk++) if (!_sealPts[_sk]) { _sealVisible = false; break; }
            if (_sealVisible && _sealPts[0].fwd > 1) {
              ctx.save();
              ctx.globalAlpha = vis.fade * (0.4 + 0.35 * _sealPulse);
              ctx.strokeStyle = 'rgba(255,60,20,0.9)';
              ctx.lineWidth = Math.max(2, Math.floor(4 * projScale / _sealPts[0].fwd));
              ctx.shadowBlur = 14; ctx.shadowColor = '#ff3300';
              ctx.beginPath(); ctx.moveTo(_sealPts[0].sx, _sealPts[0].sy);
              for (var _sk2 = 1; _sk2 < _sealN; _sk2++) ctx.lineTo(_sealPts[_sk2].sx, _sealPts[_sk2].sy);
              ctx.closePath(); ctx.stroke();
              ctx.shadowBlur = 0;
              // "SEALED" label at top of ring
              if (fortressLockedNear && fortressLockedNear.structure === st) {
                var _slP = proj(st.x, st.y, vis.floorZ + 20);
                if (_slP && _slP.fwd > 1) {
                  var _slFs = Math.max(9, Math.floor(14 * projScale / _slP.fwd));
                  ctx.globalAlpha = vis.fade;
                  ctx.fillStyle = '#ff6644'; ctx.font = 'bold ' + _slFs + 'px monospace'; ctx.textAlign = 'center';
                  ctx.fillText('SEALED — defeat all enemies', _slP.sx, _slP.sy);
                }
              }
              ctx.restore();
            }
          }
        }

      }
      ctx.restore();
    });
}

function drawMarketStalls3D() {
  if (!marketStalls || !marketStalls.length || !MODE3D) return;
  var C = getCam3D();
  var w = C.w, h = C.h, cosAng = C.cosAng, sinAng = C.sinAng;
  var invTanHalf = C.invTanHalf, horizonY = C.horizonY, cameraZ = C.cameraZ;
  var halfFov = cam.fov / 2;
  var now = Date.now();

  var proj = function(wx, wy, wz) { return projToScreen(wx, wy, wz, C); };

  var fillQuad = function(p0, p1, p2, p3, color) {
    if (!p0 || !p1 || !p2 || !p3) return;
    ctx.fillStyle = color;
    ctx.beginPath();
    ctx.moveTo(p0.sx, p0.sy);
    ctx.lineTo(p1.sx, p1.sy);
    ctx.lineTo(p2.sx, p2.sy);
    ctx.lineTo(p3.sx, p3.sy);
    ctx.closePath();
    ctx.fill();
  };

  var strokeQuad = function(p0, p1, p2, p3, color, lw) {
    if (!p0 || !p1 || !p2 || !p3) return;
    ctx.strokeStyle = color; ctx.lineWidth = lw || 1;
    ctx.beginPath();
    ctx.moveTo(p0.sx, p0.sy);
    ctx.lineTo(p1.sx, p1.sy);
    ctx.lineTo(p2.sx, p2.sy);
    ctx.lineTo(p3.sx, p3.sy);
    ctx.closePath();
    ctx.stroke();
  };

  var awningColors = {weapons: '#cc3333', potions: '#3366cc', scrolls: '#33aa55', trinkets: '#9944cc'};
  var awningDark   = {weapons: '#882222', potions: '#223388', scrolls: '#227733', trinkets: '#662288'};
  var woodCol = '#6b4226';
  var woodDark = '#4a2e18';
  var woodLit = '#8b5e3c';
  var tableTop = '#7a5030';

  // Stall dimensions in world units
  var STALL_W = 34;   // width
  var STALL_D = 18;   // depth
  var TABLE_H = 18;   // table surface height
  var AWNING_H = 44;  // awning top height
  var POLE_R = 1.5;   // pole visual radius (not used for collision)

  // Draw tower first so stalls paint over it (tower is at center, stalls surround it)
  drawGuardTower3D(C, proj, fillQuad, strokeQuad);

  // Sort stalls back-to-front so near stalls occlude far ones (painter's algorithm)
  var sortedStalls = [];
  for (var si = 0; si < marketStalls.length; si++) {
    var _ss = marketStalls[si];
    // Use entityVisible3D for consistent depth-buffer occlusion (same as other entities)
    var _sv = entityVisible3D(_ss.x, _ss.y, 0, C, {maxDist: viewDist * 0.8, depthOffset: 5, checkMidpoint: true, fadeFraction: 0.8});
    if (!_sv) continue;
    sortedStalls.push({idx: si, dist: _sv.dist, vis: _sv});
  }
  sortedStalls.sort(function(a, b) { return b.dist - a.dist; });

  for (var ssi = 0; ssi < sortedStalls.length; ssi++) {
    var stall = marketStalls[sortedStalls[ssi].idx];
    var sx = stall.x, sy = stall.y;
    var dist = sortedStalls[ssi].dist;

    var fh = floorMesh ? getFloorHeightAt(sx, sy) : 0;
    var gz = fh * 25; // ground Z

    // Stall orientation: facing determines which way the stall opens
    var cf = Math.cos(stall.facing), sf = Math.sin(stall.facing);
    // 4 corners of the table at ground level, then at table height, then awning height
    // right = perpendicular to facing
    var hw = STALL_W * 0.5, hd = STALL_D * 0.5;

    // Corner offsets: front-left, front-right, back-left, back-right
    // "front" = facing direction, "right" = perpendicular
    var flX = sx + cf * hd - sf * hw, flY = sy + sf * hd + cf * hw;
    var frX = sx + cf * hd + sf * hw, frY = sy + sf * hd - cf * hw;
    var blX = sx - cf * hd - sf * hw, blY = sy - sf * hd + cf * hw;
    var brX = sx - cf * hd + sf * hw, brY = sy - sf * hd - cf * hw;

    // Project all key points
    var flG = proj(flX, flY, gz), frG = proj(frX, frY, gz);
    var blG = proj(blX, blY, gz), brG = proj(brX, brY, gz);
    var flT = proj(flX, flY, gz + TABLE_H), frT = proj(frX, frY, gz + TABLE_H);
    var blT = proj(blX, blY, gz + TABLE_H), brT = proj(brX, brY, gz + TABLE_H);
    var flA = proj(flX, flY, gz + AWNING_H), frA = proj(frX, frY, gz + AWNING_H);
    var blA = proj(blX, blY, gz + AWNING_H), brA = proj(brX, brY, gz + AWNING_H);

    // Awning extends past front edge
    var aeX = 10; // awning extension
    var aflX = sx + cf * (hd + aeX) - sf * hw, aflY = sy + sf * (hd + aeX) + cf * hw;
    var afrX = sx + cf * (hd + aeX) + sf * hw, afrY = sy + sf * (hd + aeX) - cf * hw;
    var aflA = proj(aflX, aflY, gz + AWNING_H - 5); // slightly drooping front
    var afrA = proj(afrX, afrY, gz + AWNING_H - 5);

    if (!flG || !frG || !blG || !brG) continue; // behind camera

    ctx.save();

    // ── TABLE LEGS (4 posts from ground to table) ──
    var legs = [[flG, flT], [frG, frT], [blG, blT], [brG, brT]];
    for (var li = 0; li < 4; li++) {
      if (!legs[li][0] || !legs[li][1]) continue;
      ctx.strokeStyle = woodDark; ctx.lineWidth = Math.max(2, 5 / (dist * 0.02));
      ctx.beginPath();
      ctx.moveTo(legs[li][0].sx, legs[li][0].sy);
      ctx.lineTo(legs[li][1].sx, legs[li][1].sy);
      ctx.stroke();
    }

    // ── TABLE TOP (flat quad) ──
    fillQuad(flT, frT, brT, blT, tableTop);
    strokeQuad(flT, frT, brT, blT, woodDark, 1);

    // ── GOODS on table (small colored shapes) ──
    var goodsCol = awningColors[stall.stallType] || '#aa8844';
    var gcx = (flT && frT && blT && brT) ?
      (flT.sx + frT.sx + blT.sx + brT.sx) / 4 : 0;
    var gcy = (flT && frT && blT && brT) ?
      (flT.sy + frT.sy + blT.sy + brT.sy) / 4 : 0;
    if (gcx && gcy && dist < 200) {
      var gs = Math.max(3, 8 / (dist * 0.015));
      ctx.fillStyle = goodsCol;
      ctx.fillRect(gcx - gs * 3, gcy - gs * 0.5, gs * 1.2, gs);
      ctx.fillRect(gcx - gs * 1, gcy - gs * 0.5, gs * 1.2, gs);
      ctx.fillRect(gcx + gs * 1, gcy - gs * 0.5, gs * 1.2, gs);
      ctx.fillRect(gcx + gs * 2.5, gcy - gs * 0.5, gs * 1.2, gs);
    }

    // ── AWNING SUPPORT POLES (front 2, from ground to awning) ──
    var frontPoles = [[flG, flA], [frG, frA]];
    for (var pi = 0; pi < 2; pi++) {
      if (!frontPoles[pi][0] || !frontPoles[pi][1]) continue;
      ctx.strokeStyle = woodCol; ctx.lineWidth = Math.max(2, 6 / (dist * 0.02));
      ctx.beginPath();
      ctx.moveTo(frontPoles[pi][0].sx, frontPoles[pi][0].sy);
      ctx.lineTo(frontPoles[pi][1].sx, frontPoles[pi][1].sy);
      ctx.stroke();
    }

    // ── AWNING (canopy quad — colored fabric) ──
    var ac = awningColors[stall.stallType] || '#886633';
    var acd = awningDark[stall.stallType] || '#553311';
    // Back edge at full height, front edge slightly lower (droop)
    if (aflA && afrA && brA && blA) {
      fillQuad(aflA, afrA, brA, blA, ac);
      strokeQuad(aflA, afrA, brA, blA, acd, 2);
      // Stripe detail on awning
      if (dist < 250) {
        var stripeMid = {
          sx: (aflA.sx + blA.sx) * 0.5,
          sy: (aflA.sy + blA.sy) * 0.5
        };
        var stripeMid2 = {
          sx: (afrA.sx + brA.sx) * 0.5,
          sy: (afrA.sy + brA.sy) * 0.5
        };
        ctx.strokeStyle = acd; ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(stripeMid.sx, stripeMid.sy);
        ctx.lineTo(stripeMid2.sx, stripeMid2.sy);
        ctx.stroke();
      }
    }

    ctx.restore();
  }

}

// Guard tower — stone column with golden glow on top, drawn at market center.
// Extracted from drawMarketStalls3D for clarity; uses early returns instead of deep nesting.
function drawGuardTower3D(C, proj, fillQuad, strokeQuad) {
  if (!shopMarker) return;
  var towerVis = entityVisible3D(shopMarker.x, shopMarker.y, 0, C, {maxDist: 900, depthOffset: 5, checkMidpoint: true, fadeFraction: 0.8});
  if (!towerVis) return;

  var bDist = towerVis.dist;
  var bFwd = towerVis.fwd;
  var bfh = floorMesh ? getFloorHeightAt(shopMarker.x, shopMarker.y) : 0;
  var bgz = bfh * 25;
  var TW = 8;
  var halfFov = cam.fov / 2;
  var w = C.w;
  var now = Date.now();

  var tCorners = [
    {x: shopMarker.x - TW, y: shopMarker.y - TW},
    {x: shopMarker.x + TW, y: shopMarker.y - TW},
    {x: shopMarker.x + TW, y: shopMarker.y + TW},
    {x: shopMarker.x - TW, y: shopMarker.y + TW}
  ];

  var tG = [], tT = [], tC = [];
  for (var ti = 0; ti < 4; ti++) {
    tG.push(proj(tCorners[ti].x, tCorners[ti].y, bgz));
    tT.push(proj(tCorners[ti].x, tCorners[ti].y, bgz + TOWER_HEIGHT));
    tC.push(proj(tCorners[ti].x, tCorners[ti].y, bgz + TOWER_HEIGHT + 12));
  }

  // Draw tower body
  if (tG[0] && tG[1] && tG[2] && tG[3]) {
    ctx.save();
    var stoneLit = '#9b8b7b', stoneDark = '#6b5d4b', stoneTop = '#8b7d6b';

    // Build list of visible faces with distance, then sort back-to-front
    var toCamAng = Math.atan2(cam.y - shopMarker.y, cam.x - shopMarker.x);
    var visibleFaces = [];
    var faceCorners = [[0,1],[1,2],[2,3],[3,0]];
    for (var fi = 0; fi < 4; fi++) {
      var c0 = faceCorners[fi][0], c1 = faceCorners[fi][1];
      if (!tG[c0] || !tG[c1] || !tT[c0] || !tT[c1]) continue;

      // Face outward normal direction
      var fmx = (tCorners[c0].x + tCorners[c1].x) * 0.5 - shopMarker.x;
      var fmy = (tCorners[c0].y + tCorners[c1].y) * 0.5 - shopMarker.y;
      var faceAng = Math.atan2(fmy, fmx);
      var diff = faceAng - toCamAng;
      while (diff > Math.PI) diff -= 2 * Math.PI;
      while (diff < -Math.PI) diff += 2 * Math.PI;
      if (Math.abs(diff) > Math.PI * 0.5) continue; // strict 90° back-face cull

      // Distance from camera to face midpoint (for sorting)
      var faceMidX = (tCorners[c0].x + tCorners[c1].x) * 0.5;
      var faceMidY = (tCorners[c0].y + tCorners[c1].y) * 0.5;
      var faceDist = Math.hypot(faceMidX - cam.x, faceMidY - cam.y);
      visibleFaces.push({c0:c0, c1:c1, diff:diff, dist:faceDist});
    }
    // Sort back-to-front (farthest first)
    visibleFaces.sort(function(a, b) { return b.dist - a.dist; });

    for (var vfi = 0; vfi < visibleFaces.length; vfi++) {
      var vf = visibleFaces[vfi];
      var c0 = vf.c0, c1 = vf.c1;
      var faceCol = (Math.abs(vf.diff) < 0.4) ? stoneLit : stoneDark;
      fillQuad(tG[c0], tG[c1], tT[c1], tT[c0], faceCol);
      strokeQuad(tG[c0], tG[c1], tT[c1], tT[c0], '#4a3d2e', 1);

      // Crenellation merlons
      if (!tC[c0] || !tC[c1]) continue;
      var mw = 0.3;
      for (var mi = 0; mi < 2; mi++) {
        var mt = mi === 0 ? 0.15 : 0.65;
        var ml0 = {sx: tT[c0].sx + (tT[c1].sx - tT[c0].sx) * mt,
                   sy: tT[c0].sy + (tT[c1].sy - tT[c0].sy) * mt};
        var ml1 = {sx: tT[c0].sx + (tT[c1].sx - tT[c0].sx) * (mt + mw),
                   sy: tT[c0].sy + (tT[c1].sy - tT[c0].sy) * (mt + mw)};
        var mu0 = {sx: tC[c0].sx + (tC[c1].sx - tC[c0].sx) * mt,
                   sy: tC[c0].sy + (tC[c1].sy - tC[c0].sy) * mt};
        var mu1 = {sx: tC[c0].sx + (tC[c1].sx - tC[c0].sx) * (mt + mw),
                   sy: tC[c0].sy + (tC[c1].sy - tC[c0].sy) * (mt + mw)};
        fillQuad(ml0, ml1, mu1, mu0, stoneTop);
      }
    }

    if (tT[0] && tT[1] && tT[2] && tT[3]) {
      fillQuad(tT[0], tT[1], tT[2], tT[3], stoneTop);
    }
    ctx.restore();
  }

  // Golden glow beacon
  var glowBot = proj(shopMarker.x, shopMarker.y, bgz + TOWER_HEIGHT);
  var glowTop = proj(shopMarker.x, shopMarker.y, bgz + TOWER_HEIGHT + 50);
  if (!glowBot || !glowTop) return;

  var gTopY = glowTop.sy, gBotY = glowBot.sy;
  var gBeamH = Math.max(3, gBotY - gTopY);
  var gHW = Math.max(2, Math.floor(8 * w / (2 * bFwd * halfFov)));
  var gSX = (glowBot.sx + glowTop.sx) * 0.5;
  var bPulse = 0.6 + 0.4 * Math.sin(now * 0.002);
  var bAlpha = Math.min(0.7, 0.6 * bPulse * (1.0 - bDist / 1000));
  ctx.save();
  ctx.globalAlpha = bAlpha;
  var gGrad = ctx.createLinearGradient(gSX - gHW * 3, 0, gSX + gHW * 3, 0);
  gGrad.addColorStop(0,    'rgba(255,200,50,0)');
  gGrad.addColorStop(0.35, 'rgba(255,180,40,0.12)');
  gGrad.addColorStop(0.5,  'rgba(255,210,60,0.30)');
  gGrad.addColorStop(0.65, 'rgba(255,180,40,0.12)');
  gGrad.addColorStop(1,    'rgba(255,200,50,0)');
  ctx.fillStyle = gGrad;
  ctx.fillRect(gSX - gHW * 3, gTopY - 4, gHW * 6, gBeamH + 8);
  var cGrad = ctx.createLinearGradient(gSX - gHW, 0, gSX + gHW, 0);
  cGrad.addColorStop(0,   'rgba(255,200,50,0)');
  cGrad.addColorStop(0.4, 'rgba(255,220,100,0.4)');
  cGrad.addColorStop(0.5, 'rgba(255,240,180,0.75)');
  cGrad.addColorStop(0.6, 'rgba(255,220,100,0.4)');
  cGrad.addColorStop(1,   'rgba(255,200,50,0)');
  ctx.fillStyle = cGrad;
  ctx.fillRect(gSX - gHW, gTopY, gHW * 2, gBeamH);
  ctx.restore();
}

// Cave entrance beacon — eerie teal glow pillar visible from far in 3D.
// Shows up on every cave entrance stored in deepCaveEntrances[].
function drawCaveEntrance3D() {
  if (!deepCaveEntrances || !deepCaveEntrances.length || !MODE3D) return;
  var C = getCam3D();
  // Clip complete world-space faces at the near plane before projection.
  var proj = function(wx, wy, wz) { return {x:wx, y:wy, z:wz}; };
  var fillQuad = function(p0, p1, p2, p3, color) {
    if (!p0 || !p1 || !p2 || !p3) return;
    ctx.fillStyle = color;
    return fillSceneDepthPolygon(projectSceneWorldPolygon([p0,p1,p2,p3], C));
  };

  // Dimensions are populated from each authoritative portal record. Every
  // entrance gets physical rock jamb/lintel faces at the terrain cut;
  // increasingly constructed styles add a decorative frame and, at the high
  // end, short reinforcements.
  var ARCH_W, ARCH_H, PILLAR_W, PILLAR_D, LINTEL_H, LINTEL_OVERHANG, OPENING_W;

  var stoneColors = GAME_MATERIALS.entranceStone.hex;
  var STONE_LIT = stoneColors.lit;
  var STONE_MID = stoneColors.mid;
  var STONE_DARK = stoneColors.dark;
  var STONE_SHADOW = stoneColors.shadow;

  var visibleCaves = [];
  for (var ei = 0; ei < deepCaveEntrances.length; ei++) {
    var ce = deepCaveEntrances[ei];
    var dx = ce.x - cam.x, dy = ce.y - cam.y;
    var dist = Math.hypot(dx, dy);
    if (dist < 1 || dist > 900) continue;
    var fwd = dx * C.cosAng + dy * C.sinAng;
    if (fwd < 1) continue;
    visibleCaves.push({ce: ce, dist: dist});
  }
  visibleCaves.sort(function(a, b) { return b.dist - a.dist; });

  for (var ci = 0; ci < visibleCaves.length; ci++) {
    var ce = visibleCaves[ci].ce;
    var dist = visibleCaves[ci].dist;

    var portalStyle = typeof ce.style === 'number' ? Math.max(0, Math.min(1, ce.style)) : 0.8;
    PILLAR_W = 8 + portalStyle * 6;
    PILLAR_D = 8 + portalStyle * 6;
    LINTEL_H = 9 + portalStyle * 9;
    LINTEL_OVERHANG = 4 + portalStyle * 4;
    OPENING_W = Math.max(60, (ce.halfWidth || 45) * 2);
    ARCH_W = OPENING_W + PILLAR_W * 2;

    // The frame shares the portal's floor and roof rather than resampling l0.
    var fh = typeof ce.floorH === 'number' && isFinite(ce.floorH) ?
      ce.floorH : (floorMesh ? getFloorHeightAt(ce.x, ce.y) : 0);
    var portalCeilH = typeof ce.ceilingH === 'number' && isFinite(ce.ceilingH) ?
      ce.ceilingH : fh + (ce.ceilH || 4.2);
    var gz = fh * 25;
    ARCH_H = Math.max(78, Math.min(130, (portalCeilH - fh) * 25));

    // Through-direction (walk axis) and perpendicular (arch-width axis).
    // Uses baked cosA/sinA from entrance creation; falls back if missing.
    var fx = ce.cosA, fy = ce.sinA;
    if (fx === undefined) {
      var ang = ce.angle || 0;
      fx = Math.cos(ang); fy = Math.sin(ang);
      ce.cosA = fx; ce.sinA = fy;
    }
    var px = -fy, py = fx;

    // ── PORTAL ROCK CUT ────────────────────────────────────────────────
    // Layer-role boundaries intentionally stop floor/cap quads at the mouth.
    // Their missing vertical faces are real rock volume, not empty sky: draw
    // two jamb solids and the roof lintel from the same portal contract. This
    // is structural geometry and therefore exists even for a natural style;
    // the constructed pillars below remain optional decoration.
    function _portalSurfaceH(cross, along, fallbackH) {
      if (!floorMesh || !floorMesh.surfaceH || !floorMesh.gridSize) return fallbackH;
      var swx = ce.x + px * cross + fx * along;
      var swy = ce.y + py * cross + fy * along;
      var sgx = Math.round(swx / floorMesh.gridSize);
      var sgy = Math.round(swy / floorMesh.gridSize);
      if (sgx < 0 || sgy < 0 || sgx >= floorMesh.w || sgy >= floorMesh.h) return fallbackH;
      var sh = floorMesh.surfaceH[sgy * floorMesh.w + sgx];
      return typeof sh === 'number' && isFinite(sh) ? sh : fallbackH;
    }
    var _rockHalf = ce.halfWidth || 54;
    var _rockOuter = _rockHalf + Math.min(48, Math.max(30, _rockHalf * 0.7));
    var _rockMinTop = portalCeilH + Math.max(0.25, ce.minCover || 1);
    function _rockTopAt(cross) {
      return Math.max(_rockMinTop,
        _portalSurfaceH(cross, 0, _rockMinTop),
        _portalSurfaceH(cross, 14, _rockMinTop));
    }
    var _rockTopNegInner = _rockTopAt(-_rockHalf - 2);
    var _rockTopNegOuter = _rockTopAt(-_rockOuter);
    var _rockTopPosInner = _rockTopAt(_rockHalf + 2);
    var _rockTopPosOuter = _rockTopAt(_rockOuter);
    var _rockFrontAlong = -3;
    var _rockBackAlong = 16;
    var rockMaterial = getCaveMaterialColorAt(ce.x, ce.y);
    var rockLight = getCaveRenderLightAt(ce.x, ce.y, true);
    function rockShade(shade) {
      return rgbQ(((rockMaterial >> 16) & 255) * rockLight * shade | 0,
        ((rockMaterial >> 8) & 255) * rockLight * shade | 0,
        (rockMaterial & 255) * rockLight * shade | 0);
    }
    var _rockFront = DEBUG_POLY_TYPES ? '#d91f1f' : rockShade(1);
    var _rockSide = DEBUG_POLY_TYPES ? '#a51414' : rockShade(0.88);
    var _rockBack = DEBUG_POLY_TYPES ? '#771010' : rockShade(0.8);
    var _rockTop = DEBUG_POLY_TYPES ? '#ef3535' : rockShade(1);

    function _rockPoint(cross, along, heightH) {
      return proj(ce.x + px * cross + fx * along,
                  ce.y + py * cross + fy * along, heightH * 25);
    }
    function _drawRockBand(c0, c1, bottomH, top0H, top1H) {
      if (top0H <= bottomH || top1H <= bottomH) return;
      var f0b = _rockPoint(c0, _rockFrontAlong, bottomH);
      var f1b = _rockPoint(c1, _rockFrontAlong, bottomH);
      var b0b = _rockPoint(c0, _rockBackAlong, bottomH);
      var b1b = _rockPoint(c1, _rockBackAlong, bottomH);
      var f0t = _rockPoint(c0, _rockFrontAlong, top0H);
      var f1t = _rockPoint(c1, _rockFrontAlong, top1H);
      var b0t = _rockPoint(c0, _rockBackAlong, top0H);
      var b1t = _rockPoint(c1, _rockBackAlong, top1H);
      // Far face, edge returns, mouth face, then top. The decorative frame is
      // painted later and naturally sits in front of this rock cut.
      fillQuad(b0t, b1t, b1b, b0b, _rockBack);
      fillQuad(f0t, b0t, b0b, f0b, _rockSide);
      fillQuad(f1t, b1t, b1b, f1b, _rockSide);
      fillQuad(f0t, f1t, f1b, f0b, _rockFront);
      fillQuad(f0t, f1t, b1t, b0t, _rockTop);
    }
    _drawRockBand(-_rockOuter, -_rockHalf, fh, _rockTopNegOuter, _rockTopNegInner);
    _drawRockBand(_rockHalf, _rockOuter, fh, _rockTopPosInner, _rockTopPosOuter);
    _drawRockBand(-_rockHalf, _rockHalf, portalCeilH, _rockTopNegInner, _rockTopPosInner);

    // At the natural end, the rock cut is the entrance. Avoid placing a
    // freestanding constructed monument in front of every cave.
    if (portalStyle < 0.28) continue;

    // Pillar centers: flank the opening left/right
    var halfStride = (OPENING_W + PILLAR_W) * 0.5;
    var lcX = ce.x + px * halfStride, lcY = ce.y + py * halfStride;
    var rcX = ce.x - px * halfStride, rcY = ce.y - py * halfStride;

    // (Dark portal backdrop removed — lets the actual cave interior show
    // through the opening instead of a flat black rectangle.)
    var pHD = PILLAR_D * 0.5;

    // ── PILLARS (stone boxes flanking the opening) ──
    var pHW = PILLAR_W * 0.5;
    function drawPillar(cx, cy) {
      // Corners in world: front=+fx direction, back=-fx, right=+px, left=-px
      var flX = cx + fx * pHD - px * pHW, flY = cy + fy * pHD - py * pHW;
      var frX = cx + fx * pHD + px * pHW, frY = cy + fy * pHD + py * pHW;
      var blX = cx - fx * pHD - px * pHW, blY = cy - fy * pHD - py * pHW;
      var brX = cx - fx * pHD + px * pHW, brY = cy - fy * pHD + py * pHW;
      var flB = proj(flX, flY, gz),            frB = proj(frX, frY, gz);
      var blB = proj(blX, blY, gz),            brB = proj(brX, brY, gz);
      var flT = proj(flX, flY, gz + ARCH_H),   frT = proj(frX, frY, gz + ARCH_H);
      var blT = proj(blX, blY, gz + ARCH_H),   brT = proj(brX, brY, gz + ARCH_H);
      // Back face (away from viewer if facing through-direction)
      fillQuad(blT, brT, brB, blB, STONE_SHADOW);
      // Side faces
      fillQuad(flT, blT, blB, flB, STONE_DARK);
      fillQuad(frT, brT, brB, frB, STONE_DARK);
      // Front face (toward ramp / most-visible side)
      fillQuad(flT, frT, frB, flB, STONE_MID);
      // Top cap
      fillQuad(flT, frT, brT, blT, STONE_LIT);
    }
    drawPillar(lcX, lcY);
    drawPillar(rcX, rcY);

    // ── FLANKING WALLS: stone segments extending sideways from each pillar ──
    // Match pillar Z range (gz to gz+ARCH_H) so they sit beside the pillars, not
    // above them. Extend outward along the cross axis to block side approach.
    var FLANK_LEN = Math.round((ce.halfWidth || 54) * (0.5 + portalStyle * 0.7));
    var FLANK_THK = PILLAR_D;
    // Reach to the bottom of the lintel — the visual "ceiling" of the archway.
    var FLANK_H = ARCH_H;
    // Wooden palisade — vertical planks with per-plank shading. Both front &
    // back faces get the same per-plank shade so the variation reads from any
    // side. Shades are strongly contrasted so the planks are distinguishable.
    var woodColors = GAME_MATERIALS.palisadeWood.hex;
    var WOOD_LIT = woodColors.lit;
    var WOOD_MID = woodColors.mid;
    var WOOD_DARK = woodColors.dark;
    var WOOD_DEEP = woodColors.deep;
    var WOOD_SIDE = woodColors.deep;
    var PLANK_SHADES = [WOOD_MID, WOOD_LIT, WOOD_MID, WOOD_DARK, WOOD_MID, WOOD_LIT, WOOD_MID];
    var NUM_PLANKS = 7;
    function drawFlank(pillarCX, pillarCY, sideSign) {
      var hzD = FLANK_THK * 0.5;
      var plankLen = FLANK_LEN / NUM_PLANKS;
      // Camera-relative draw order: outer planks further → draw first
      var camToInner = Math.hypot((pillarCX + px * sideSign * pHW) - cam.x,
                                  (pillarCY + py * sideSign * pHW) - cam.y);
      var camToOuter = Math.hypot((pillarCX + px * sideSign * (pHW + FLANK_LEN)) - cam.x,
                                  (pillarCY + py * sideSign * (pHW + FLANK_LEN)) - cam.y);
      var innerFirst = camToOuter > camToInner;
      for (var pk = 0; pk < NUM_PLANKS; pk++) {
        var k = innerFirst ? (NUM_PLANKS - 1 - pk) : pk;
        var c0 = sideSign * (pHW + k * plankLen);
        var c1 = sideSign * (pHW + (k + 1) * plankLen);
        var p0X = pillarCX + px * c0, p0Y = pillarCY + py * c0;
        var p1X = pillarCX + px * c1, p1Y = pillarCY + py * c1;
        var f0X = p0X + fx * hzD, f0Y = p0Y + fy * hzD;
        var b0X = p0X - fx * hzD, b0Y = p0Y - fy * hzD;
        var f1X = p1X + fx * hzD, f1Y = p1Y + fy * hzD;
        var b1X = p1X - fx * hzD, b1Y = p1Y - fy * hzD;
        var f0B = proj(f0X, f0Y, gz), f0T = proj(f0X, f0Y, gz + FLANK_H);
        var b0B = proj(b0X, b0Y, gz), b0T = proj(b0X, b0Y, gz + FLANK_H);
        var f1B = proj(f1X, f1Y, gz), f1T = proj(f1X, f1Y, gz + FLANK_H);
        var b1B = proj(b1X, b1Y, gz), b1T = proj(b1X, b1Y, gz + FLANK_H);
        if (!f0B || !b0B || !f1B || !b1B || !f0T || !b0T || !f1T || !b1T) continue;
        var shade = PLANK_SHADES[k % PLANK_SHADES.length];
        // Darker shade for back face — same per-plank variation but dimmer
        var backShade = shade === WOOD_LIT ? WOOD_MID : (shade === WOOD_MID ? WOOD_DARK : WOOD_DEEP);
        // Back face (dimmer per-plank)
        fillQuad(b0T, b1T, b1B, b0B, backShade);
        // Front face (lit per-plank)
        fillQuad(f0T, f1T, f1B, f0B, shade);
        // End caps — only on terminal planks
        if (k === NUM_PLANKS - 1) fillQuad(f1T, b1T, b1B, f1B, WOOD_SIDE);
        if (k === 0) fillQuad(f0T, b0T, b0B, f0B, WOOD_SIDE);
        // Top cap — use the plank shade (slightly lit)
        fillQuad(f0T, f1T, b1T, b0T, shade);
      }
    }
    if (portalStyle >= 0.72) {
      drawFlank(lcX, lcY, 1);
      drawFlank(rcX, rcY, -1);
    }

    // ── LINTEL (horizontal stone beam across the top) ──
    var lntHW = ARCH_W * 0.5 + LINTEL_OVERHANG;
    var lntHD = pHD + 2;
    var lz0 = gz + ARCH_H, lz1 = gz + ARCH_H + LINTEL_H;
    var lflX = ce.x + fx * lntHD - px * lntHW, lflY = ce.y + fy * lntHD - py * lntHW;
    var lfrX = ce.x + fx * lntHD + px * lntHW, lfrY = ce.y + fy * lntHD + py * lntHW;
    var lblX = ce.x - fx * lntHD - px * lntHW, lblY = ce.y - fy * lntHD - py * lntHW;
    var lbrX = ce.x - fx * lntHD + px * lntHW, lbrY = ce.y - fy * lntHD + py * lntHW;
    var lflB = proj(lflX, lflY, lz0), lfrB = proj(lfrX, lfrY, lz0);
    var lblB = proj(lblX, lblY, lz0), lbrB = proj(lbrX, lbrY, lz0);
    var lflT = proj(lflX, lflY, lz1), lfrT = proj(lfrX, lfrY, lz1);
    var lblT = proj(lblX, lblY, lz1), lbrT = proj(lbrX, lbrY, lz1);
    // Every lintel face uses the same per-pixel depth as the ground.
    {
      fillQuad(lblT, lbrT, lbrB, lblB, STONE_SHADOW);
      fillQuad(lflT, lblT, lblB, lflB, STONE_DARK);
      fillQuad(lfrT, lbrT, lbrB, lfrB, STONE_DARK);
      fillQuad(lflT, lfrT, lfrB, lflB, STONE_MID);
      fillQuad(lflT, lfrT, lbrT, lblT, STONE_LIT);
    }

    // "CAVE" label above the lintel when close
    if (portalStyle >= 0.82 && dist < 500) {
      var labelP = projToScreen(ce.x, ce.y, lz1 + 10, C);
      var labelDepth = (ce.x - cam.x) * C.cosAng + (ce.y - cam.y) * C.sinAng;
      if (labelP && labelDepth <= sceneDepthAt(labelP.sx, labelP.sy) + 0.15) {
        var a = Math.min(1.0, (500 - dist) / 200);
        ctx.save();
        ctx.globalAlpha = a;
        ctx.font = 'bold 12px monospace';
        ctx.textAlign = 'center';
        ctx.fillStyle = '#e8d8a0';
        ctx.shadowBlur = 4; ctx.shadowColor = '#000';
        // Label flips to EXIT only once the player is past the archway and
        // inside the cave proper. playerUnderground flips too early (on the
        // approach ramp, since it has ceiling overhead). Signed along-axis
        // distance from the entrance is the actual boundary the user crosses.
        var _lbldx = pos.x - ce.x, _lbldy = pos.y - ce.y;
        // Use baked cosA/sinA from entrance creation.
        var _lblC = ce.cosA !== undefined ? ce.cosA : Math.cos(ce.angle || 0);
        var _lblS = ce.sinA !== undefined ? ce.sinA : Math.sin(ce.angle || 0);
        var _lblAlong = _lbldx * _lblC + _lbldy * _lblS;
        ctx.fillText(_lblAlong < -40 ? 'CAVE' : 'EXIT', labelP.sx, labelP.sy);
        ctx.restore();
      }
    }
  }
}

// Cave entrance beacon — 2D top-down: pulsing teal ring with "C" label.
function drawCaveEntrance2D() {
  if (!deepCaveEntrances || !deepCaveEntrances.length) return;
  var now = Date.now();
  ctx.save(); ctx.translate(-viewCam.x, -viewCam.y);
  for (var ei = 0; ei < deepCaveEntrances.length; ei++) {
    var ce = deepCaveEntrances[ei];
    var pulse = 0.55 + 0.45 * Math.sin(now * 0.003 + ei * 1.7);
    ctx.globalAlpha = 0.7 * pulse + 0.15;
    // Outer glow ring
    ctx.strokeStyle = '#00ffcc'; ctx.lineWidth = 3;
    ctx.shadowBlur = 10; ctx.shadowColor = '#00ffcc';
    ctx.beginPath(); ctx.arc(ce.x, ce.y, 13, 0, Math.PI * 2); ctx.stroke();
    // Inner filled dot
    ctx.fillStyle = 'rgba(0,220,180,0.55)';
    ctx.beginPath(); ctx.arc(ce.x, ce.y, 7, 0, Math.PI * 2); ctx.fill();
    ctx.shadowBlur = 0;
    // Label
    ctx.globalAlpha = 0.9;
    ctx.fillStyle = '#00ffcc'; ctx.font = 'bold 9px monospace'; ctx.textAlign = 'center';
    ctx.fillText('C', ce.x, ce.y + 3);
  }
  ctx.restore();
}

// Shop overlay — two-zone layout:
//   Top:    large detail card for the selected item (name, desc, cost, buy prompt)
//   Bottom: strip of 5 compact numbered tabs; A/D or ←/→ cycles selection
// No scrolling needed — everything fits at any canvas size.
function drawShopOverlay() {
  if (!shopOpen) return;
  var w = canvas.width, h = canvas.height;
  ctx.save();

  // Get dynamic item list (filters out already-bought spells/upgrades)
  var visItems = getVisibleShopItems();
  if (visItems.length === 0) visItems = SHOP_ITEMS_BASE.slice(0, 5); // fallback to consumables

  // Paging: max 6 tabs visible at a time
  var maxTabs = 6;
  var shopPage = Math.floor(shopSelIdx / maxTabs);
  var pageStart = shopPage * maxTabs;
  var pageItems = visItems.slice(pageStart, pageStart + maxTabs);
  var pageIdx = shopSelIdx - pageStart;
  // Clamp selection
  if (shopSelIdx >= visItems.length) shopSelIdx = visItems.length - 1;
  if (shopSelIdx < 0) shopSelIdx = 0;

  // ── Dim background ──────────────────────────────────────────────────
  ctx.fillStyle = 'rgba(0,0,0,0.78)'; ctx.fillRect(0, 0, w, h);

  // ── Panel bounds ────────────────────────────────────────────────────
  var panW = Math.min(440, Math.floor(w * 0.88));
  var panH = Math.min(340, Math.floor(h * 0.86));
  var px = Math.floor((w - panW) / 2), py = Math.floor((h - panH) / 2);

  // Panel background + border
  ctx.fillStyle = 'rgba(14,9,2,0.97)';
  ctx.strokeStyle = '#c8a020'; ctx.lineWidth = 2;
  ctx.fillRect(px, py, panW, panH);
  ctx.strokeRect(px, py, panW, panH);

  // Inner inset line (decorative)
  ctx.strokeStyle = 'rgba(200,160,32,0.25)'; ctx.lineWidth = 1;
  ctx.strokeRect(px + 5, py + 5, panW - 10, panH - 10);

  // ── Header ──────────────────────────────────────────────────────────
  var hdY = py + 26;
  ctx.fillStyle = '#ffd700'; ctx.font = 'bold 16px serif'; ctx.textAlign = 'center';
  ctx.fillText('= Wandering Merchant =', px + panW / 2, hdY);
  ctx.fillStyle = '#aa9944'; ctx.font = '11px Arial';
  var pageHint = visItems.length > maxTabs ? ('  Page ' + (shopPage + 1) + '/' + Math.ceil(visItems.length / maxTabs)) : '';
  ctx.fillText('Coins: ' + coins + '   \u25C4 Tab \u25BA  select   [E] buy   [Esc] close' + pageHint, px + panW / 2, hdY + 17);

  // ── Tab strip (bottom) ──────────────────────────────────────────────
  var tabStripH = 54;
  var tabY = py + panH - tabStripH - 8;
  var tabW = Math.floor((panW - 28) / Math.max(1, pageItems.length));
  var tabPad = 4;

  for (var ti = 0; ti < pageItems.length; ti++) {
    var titem = pageItems[ti];
    var canBuyT = (coins >= titem.cost);
    var sel = (ti === pageIdx);
    var tx = px + 14 + ti * tabW;

    // Tab background — spell/upgrade tomes get different tints
    var isSpellItem = (titem.type === 'spell' || titem.type === 'upgrade');
    if (sel) {
      ctx.fillStyle = canBuyT ? (isSpellItem ? 'rgba(20,40,90,0.95)' : 'rgba(90,70,10,0.95)') : 'rgba(60,20,20,0.95)';
    } else {
      ctx.fillStyle = canBuyT ? (isSpellItem ? 'rgba(10,20,45,0.7)' : 'rgba(40,30,5,0.7)') : 'rgba(22,14,14,0.7)';
    }
    ctx.fillRect(tx, tabY, tabW - tabPad, tabStripH);

    // Tab border
    ctx.strokeStyle = sel ? (canBuyT ? '#ffd700' : '#882222') : (canBuyT ? '#665500' : '#333333');
    ctx.lineWidth = sel ? 2 : 1;
    ctx.strokeRect(tx, tabY, tabW - tabPad, tabStripH);

    // Number key
    ctx.fillStyle = sel ? '#ffffff' : (canBuyT ? '#ccaa44' : '#555555');
    ctx.font = 'bold 11px Arial'; ctx.textAlign = 'center';
    ctx.fillText('' + (ti + 1), tx + (tabW - tabPad) / 2, tabY + 13);

    // Short name
    var shortName = titem.name.split(' ')[0];
    ctx.fillStyle = sel ? '#ffd700' : (canBuyT ? '#ccaa44' : '#444444');
    if (isSpellItem && titem.spellId && spells[titem.spellId]) {
      ctx.fillStyle = sel ? spells[titem.spellId].color : (canBuyT ? spells[titem.spellId].color : '#444444');
    }
    ctx.font = (sel ? 'bold ' : '') + '10px Arial';
    ctx.fillText(shortName, tx + (tabW - tabPad) / 2, tabY + 27);

    // Cost
    ctx.fillStyle = canBuyT ? '#ffdd55' : '#553333';
    ctx.font = '10px Arial';
    ctx.fillText(titem.cost + 'g', tx + (tabW - tabPad) / 2, tabY + 42);
  }

  // ── Detail card (between header and tab strip) ───────────────────────
  var cardY = py + 52, cardH = tabY - cardY - 8;
  var cardX = px + 14, cardW = panW - 28;
  var selItem = visItems[shopSelIdx] || visItems[0];
  var canBuySel = (coins >= selItem.cost);

  ctx.fillStyle = canBuySel ? 'rgba(55,42,8,0.85)' : 'rgba(35,18,18,0.85)';
  ctx.fillRect(cardX, cardY, cardW, cardH);
  ctx.strokeStyle = canBuySel ? '#aa8800' : '#552222'; ctx.lineWidth = 1.5;
  ctx.strokeRect(cardX, cardY, cardW, cardH);

  // Item icon glyph
  var icons = {heal:'\u2665', mana:'\u2726', speed:'\u27A4', ward:'\u26E8', dmg:'\u2605',
               spell_fire:'\u2622', spell_ice:'\u2744', spell_lightning:'\u26A1',
               spell_poison:'\u2623', spell_arcane:'\u2728',
               upg_missile:'\u2B06', upg_fire:'\u2B06', upg_ice:'\u2B06',
               upg_lightning:'\u2B06', upg_poison:'\u2B06', upg_arcane:'\u2B06',
               companion_slime:'\u25CF'};
  var iconGlyph = icons[selItem.id] || '\u25C6';
  var iconSize  = Math.min(36, Math.floor(cardH * 0.45));
  // Spell items get their spell color for the icon
  var iconColor = canBuySel ? '#ffd700' : '#773333';
  if (selItem.spellId && spells[selItem.spellId] && canBuySel) iconColor = spells[selItem.spellId].color;
  ctx.fillStyle = iconColor;
  ctx.font = 'bold ' + iconSize + 'px serif'; ctx.textAlign = 'center';
  var iconX = cardX + Math.floor(cardW * 0.15);
  var midY  = cardY + cardH / 2;
  ctx.fillText(iconGlyph, iconX, midY + iconSize * 0.35);

  // Type label (small, above name)
  if (selItem.type === 'spell') {
    ctx.fillStyle = '#6688cc'; ctx.font = '9px Arial'; ctx.textAlign = 'left';
    ctx.fillText('SPELL TOME', cardX + Math.floor(cardW * 0.28), midY - Math.floor(cardH * 0.25));
  } else if (selItem.type === 'upgrade') {
    ctx.fillStyle = '#cc8844'; ctx.font = '9px Arial'; ctx.textAlign = 'left';
    ctx.fillText('UPGRADE', cardX + Math.floor(cardW * 0.28), midY - Math.floor(cardH * 0.25));
  }

  // Name
  var textX = cardX + Math.floor(cardW * 0.28);
  ctx.fillStyle = canBuySel ? '#ffd700' : '#884444';
  ctx.font = 'bold ' + Math.min(16, Math.floor(cardH * 0.22)) + 'px serif';
  ctx.textAlign = 'left';
  ctx.fillText(selItem.name, textX, midY - Math.floor(cardH * 0.08));

  // Description
  ctx.fillStyle = canBuySel ? '#ddddcc' : '#665555';
  ctx.font = Math.min(13, Math.floor(cardH * 0.17)) + 'px Arial';
  ctx.fillText(selItem.desc, textX, midY + Math.floor(cardH * 0.12));

  // Cost badge
  var badgeX = cardX + cardW - 12, badgeY = cardY + 12;
  ctx.fillStyle = canBuySel ? '#ffd700' : '#553333';
  ctx.font = 'bold ' + Math.min(15, Math.floor(cardH * 0.20)) + 'px Arial';
  ctx.textAlign = 'right';
  ctx.fillText(selItem.cost + 'g', badgeX, badgeY + Math.floor(cardH * 0.20));

  // Buy prompt
  ctx.fillStyle = canBuySel ? 'rgba(100,255,100,0.90)' : 'rgba(180,60,60,0.80)';
  ctx.font = 'bold ' + Math.min(12, Math.floor(cardH * 0.15)) + 'px Arial';
  ctx.textAlign = 'center';
  var promptY = cardY + cardH - 10;
  ctx.fillText(canBuySel ? '[E] Buy' : 'Need ' + (selItem.cost - coins) + ' more coins', cardX + cardW / 2, promptY);

  ctx.restore();
}

// ── Minimap ───────────────────────────────────────────────────────────
var MINIMAP_W = GAME_CONFIG.hud.minimapW, MINIMAP_H = GAME_CONFIG.hud.minimapH;

function initMinimap() {
  if (!grid || gridW < 2 || gridH < 2) return;
  exploredCells = new Uint8Array(gridW * gridH);
  // Pre-reveal a large area around the player start position
  if (pos) {
    var _pgx0 = Math.floor(pos.x / cell), _pgy0 = Math.floor(pos.y / cell);
    var _initR = 30;
    for (var _idy = -_initR; _idy <= _initR; _idy++) {
      for (var _idx = -_initR; _idx <= _initR; _idx++) {
        if (_idx * _idx + _idy * _idy > _initR * _initR) continue;
        var _igx = _pgx0 + _idx, _igy = _pgy0 + _idy;
        if (_igx >= 0 && _igy >= 0 && _igx < gridW && _igy < gridH)
          exploredCells[_igy * gridW + _igx] = 1;
      }
    }
  }
  minimapDirty = true;
  // Create offscreen canvas for terrain base
  minimapCanvas = document.createElement('canvas');
  minimapCanvas.width = MINIMAP_W;
  minimapCanvas.height = MINIMAP_H;
  renderMinimapBase();
}

function renderMinimapBase() {
  if (!minimapCanvas || !grid) return;
  var mc = minimapCanvas.getContext('2d');
  mc.fillStyle = '#000000';
  mc.fillRect(0, 0, MINIMAP_W, MINIMAP_H);
  var scaleX = MINIMAP_W / gridW;
  var scaleY = MINIMAP_H / gridH;
  // Terrain-specific floor color
  var floorCol, wallCol;
  if (terrain === 'ice')         { floorCol = '#1a2535'; wallCol = '#3a4a60'; }
  else if (terrain === 'cave')   { floorCol = '#1a1818'; wallCol = '#3a3535'; }
  else if (terrain === 'expanse'){ floorCol = '#1e1508'; wallCol = '#3a2a15'; }
  else if (terrain === 'plains') { floorCol = '#1a1a0e'; wallCol = '#35351a'; }
  else                           { floorCol = '#1a1510'; wallCol = '#3a3025'; }
  for (var gy = 0; gy < gridH; gy++) {
    for (var gx = 0; gx < gridW; gx++) {
      var px = Math.floor(gx * scaleX);
      var py = Math.floor(gy * scaleY);
      var pw = Math.max(1, Math.ceil(scaleX));
      var ph = Math.max(1, Math.ceil(scaleY));
      if (grid[gy * gridW + gx] === 1) {
        mc.fillStyle = wallCol;
      } else {
        // Check for underground cave floor
        var _isUnderground = false;
        if (meshCave) {
          var _mmx = Math.floor(gx * cell / 12);
          var _mmy = Math.floor(gy * cell / 12);
          if (_mmx >= 0 && _mmx < floorMesh.w && _mmy >= 0 && _mmy < floorMesh.h) {
            _isUnderground = !!meshCave[_mmy * floorMesh.w + _mmx];
          }
        }
        mc.fillStyle = _isUnderground ? '#0a0808' : floorCol;
      }
      mc.fillRect(px, py, pw, ph);
    }
  }
}

var _lastExploredUpdate = 0;
function updateExplored() {
  var now = Date.now();
  if (now - _lastExploredUpdate < 100) return; // throttle to 10Hz
  _lastExploredUpdate = now;
  if (!exploredCells || !grid || !pos) return;
  var pgx = Math.floor(pos.x / cell);
  var pgy = Math.floor(pos.y / cell);
  var reveal = 22; // reveal radius in grid cells
  var changed = false;
  for (var dy = -reveal; dy <= reveal; dy++) {
    for (var dx = -reveal; dx <= reveal; dx++) {
      if (dx * dx + dy * dy > reveal * reveal) continue;
      var gx = pgx + dx, gy = pgy + dy;
      if (gx < 0 || gy < 0 || gx >= gridW || gy >= gridH) continue;
      var idx = gy * gridW + gx;
      if (!exploredCells[idx]) { exploredCells[idx] = 1; changed = true; }
    }
  }
  if (changed) minimapDirty = true;
}

function drawMinimap() {
  if (minimapMode === 2 || !exploredCells || !minimapCanvas || !grid) return;
  var w = canvas.width, h = canvas.height;
  var S = resScale;
  var pad = 10 * S;
  var mmW, mmH, mmX, mmY, markerScale;

  if (minimapMode === 1) {
    // ── Large centered map ──
    var aspect = MINIMAP_W / MINIMAP_H;
    // Fill as much of the screen as possible while preserving aspect ratio
    mmW = w * 0.90;
    mmH = mmW / aspect;
    if (mmH > h * 0.90) { mmH = h * 0.90; mmW = mmH * aspect; }
    mmX = Math.floor((w - mmW) / 2);
    mmY = Math.floor((h - mmH) / 2);
    markerScale = mmW / (MINIMAP_W * S);
    // Dark overlay behind the map
    ctx.save();
    ctx.globalAlpha = 0.55;
    ctx.fillStyle = '#000000';
    ctx.fillRect(0, 0, w, h);
    ctx.restore();
  } else {
    // ── Small corner map ──
    mmW = MINIMAP_W * S;
    mmH = MINIMAP_H * S;
    mmX = pad;
    mmY = pad;
    markerScale = 1;
  }

  ctx.save();
  ctx.globalAlpha = minimapMode === 1 ? 0.92 : 0.85;

  // Background
  ctx.fillStyle = minimapMode === 1 ? 'rgba(0,0,0,0.8)' : 'rgba(0,0,0,0.6)';
  ctx.fillRect(mmX - 1 * S, mmY - 1 * S, mmW + 2 * S, mmH + 2 * S);

  // Composite: draw terrain base, then punch out fog for explored areas
  // Use a second offscreen canvas for the fog mask, updated only when dirty
  if (!drawMinimap._fogCanvas) {
    drawMinimap._fogCanvas = document.createElement('canvas');
    drawMinimap._fogCanvas.width = MINIMAP_W;
    drawMinimap._fogCanvas.height = MINIMAP_H;
    minimapDirty = true;
  }
  if (minimapDirty) {
    var fc = drawMinimap._fogCanvas.getContext('2d');
    // Start fully black (fog), then clear explored pixels to transparent
    fc.fillStyle = 'rgba(0,0,0,0.88)';
    fc.fillRect(0, 0, MINIMAP_W, MINIMAP_H);
    fc.globalCompositeOperation = 'destination-out';
    fc.fillStyle = 'rgba(0,0,0,1)';
    var scaleX = MINIMAP_W / gridW;
    var scaleY = MINIMAP_H / gridH;
    for (var gy = 0; gy < gridH; gy++) {
      for (var gx = 0; gx < gridW; gx++) {
        if (exploredCells[gy * gridW + gx]) {
          fc.fillRect(Math.floor(gx * scaleX), Math.floor(gy * scaleY),
                      Math.max(1, Math.ceil(scaleX)), Math.max(1, Math.ceil(scaleY)));
        }
      }
    }
    fc.globalCompositeOperation = 'source-over';
    minimapDirty = false;
  }

  // Draw terrain base then fog overlay (scaled to display size)
  ctx.drawImage(minimapCanvas, mmX, mmY, mmW, mmH);
  ctx.drawImage(drawMinimap._fogCanvas, mmX, mmY, mmW, mmH);

  // ── Minimap marker helpers (reusable for any marker type) ──
  ctx.globalAlpha = 1.0;
  var toMmX = function(wx) { return mmX + (wx / worldW) * mmW; };
  var toMmY = function(wy) { return mmY + (wy / worldH) * mmH; };

  // Convert world coords to window-local, return null if outside window
  function worldToMinimap(wx, wy) {
    var lx = wx - windowOriginX, ly = wy - windowOriginY;
    if (lx < 0 || lx > worldW || ly < 0 || ly > worldH) return null;
    return { mx: toMmX(lx), my: toMmY(ly) };
  }

  // Draw a square marker at minimap coords
  var mS = S * markerScale;  // marker-aware scale factor
  function mmSquare(mx, my, size, fillColor, strokeColor) {
    var hs = size * mS * 0.5;
    ctx.fillStyle = fillColor;
    ctx.fillRect(mx - hs, my - hs, size * mS, size * mS);
    if (strokeColor) {
      ctx.strokeStyle = strokeColor; ctx.lineWidth = 0.5 * mS;
      ctx.strokeRect(mx - hs, my - hs, size * mS, size * mS);
    }
  }

  // Draw a diamond marker at minimap coords
  function mmDiamond(mx, my, size, fillColor, strokeColor) {
    var r = size * mS * 0.5;
    ctx.fillStyle = fillColor;
    ctx.beginPath();
    ctx.moveTo(mx, my - r); ctx.lineTo(mx + r, my);
    ctx.lineTo(mx, my + r); ctx.lineTo(mx - r, my);
    ctx.closePath(); ctx.fill();
    if (strokeColor) {
      ctx.strokeStyle = strokeColor; ctx.lineWidth = 0.5 * mS;
      ctx.stroke();
    }
  }

  // Draw a circle marker at minimap coords
  function mmCircle(mx, my, radius, fillColor) {
    ctx.fillStyle = fillColor;
    ctx.beginPath(); ctx.arc(mx, my, radius * mS, 0, Math.PI * 2); ctx.fill();
  }

  // Draw an array of world-coord markers with a given shape function
  // items: [{wx,wy,...}], shapeFn(mx,my,item), throughFog: show even in unexplored areas
  function mmDrawMarkers(items, shapeFn, throughFog) {
    for (var _mi = 0; _mi < items.length; _mi++) {
      var item = items[_mi];
      var mp = worldToMinimap(item.wx, item.wy);
      if (!mp) continue;
      shapeFn(mp.mx, mp.my, item);
    }
  }

  // Draw markers for window-local coord items (existing entities like caves)
  function mmDrawLocalMarkers(items, xKey, yKey, shapeFn) {
    for (var _mi = 0; _mi < items.length; _mi++) {
      var item = items[_mi];
      shapeFn(toMmX(item[xKey]), toMmY(item[yKey]), item);
    }
  }

  // ── Goal marker (green square) ──
  if (goalSpawned && goal) {
    mmSquare(toMmX(goal.x + (goal.w || 0) * 0.5), toMmY(goal.y + (goal.h || 0) * 0.5), 4, '#44ff44', null);
  }

  // ── Market markers (gold squares, persistent through fog) ──
  mmDrawMarkers(discoveredMarkets, function(mx, my) {
    mmSquare(mx, my, 5, '#ffd700', '#aa8800');
  }, true);

  // ── Shrine markers (colored diamonds, persistent through fog) ──
  var _shrineColors = { damage: '#ff4444', speed: '#44ffff', regen: '#44ff44', armor: '#ffaa44' };
  mmDrawMarkers(discoveredShrines, function(mx, my, item) {
    var col = _shrineColors[item.buffType] || '#ffffff';
    mmDiamond(mx, my, 4, col, '#333333');
  }, true);

  // ── Ruin markers (brown squares, persistent through fog) ──
  mmDrawMarkers(discoveredRuins, function(mx, my) {
    mmSquare(mx, my, 3, '#8a6a40', '#555555');
  }, true);

  // ── Large structure markers ──
  if (largeStructures && largeStructures.length) {
    mmDrawLocalMarkers(largeStructures, 'x', 'y', function(mx, my, st) {
      var col = (st.type === 'fortress') ? '#c8a028' : (st.type === 'arena') ? '#c84020' : '#2080c0';
      mmSquare(mx, my, 5, col, '#333333');
    });
  }

  // ── Cave entrances (cyan squares) ──
  if (deepCaveEntrances) {
    mmDrawLocalMarkers(deepCaveEntrances, 'x', 'y', function(mx, my) {
      mmSquare(mx, my, 3, '#00ffcc', null);
    });
  }

  // ── Player (white dot + direction line) ──
  if (pos) {
    var pmx = toMmX(pos.x), pmy = toMmY(pos.y);
    mmCircle(pmx, pmy, 2, '#ffffff');
    // Direction indicator
    var dirLen = 5 * mS;
    var ang = cam ? cam.ang : 0;
    ctx.strokeStyle = '#ffffff'; ctx.lineWidth = 1 * mS;
    ctx.beginPath();
    ctx.moveTo(pmx, pmy);
    ctx.lineTo(pmx + Math.cos(ang) * dirLen, pmy + Math.sin(ang) * dirLen);
    ctx.stroke();
  }

  // Border
  ctx.strokeStyle = 'rgba(255,255,255,0.25)'; ctx.lineWidth = 1 * S;
  ctx.strokeRect(mmX - 1 * S, mmY - 1 * S, mmW + 2 * S, mmH + 2 * S);

  ctx.restore();
}

// Screen-space UI uses opaque ink and warm edges to stay readable over both
// sunlit terrain and dark interiors. No blur/gradient allocation in the HUD.
var HUD_COLORS = {
  panel: 'rgba(15,18,22,0.93)', edge: '#665c49', ink: '#eee6d5',
  muted: '#b5ad9d', accent: '#c9aa71', track: '#292d32',
  health: '#b84d51', mana: '#539ab6', warning: '#f2a193'
};

function drawHudPanel(x, y, w, h, S) {
  ctx.fillStyle = HUD_COLORS.panel;
  ctx.fillRect(x, y, w, h);
  ctx.strokeStyle = HUD_COLORS.edge;
  ctx.lineWidth = Math.max(1, S);
  ctx.strokeRect(x + S * 0.5, y + S * 0.5, w - S, h - S);
}

function drawHudOverlay() {
  var w = canvas.width, h = canvas.height;
  var S = resScale, now = Date.now();
  ctx.save();
  var pad = 10 * S;
  var barW = Math.min(100 * S, Math.floor(w * 0.25)), barH = 6 * S;

  // A labelled pair of resource bars stays beside the compact map. Values
  // remain readable at empty mana/low health instead of relying on color alone.
  var hudX = (minimapMode === 0) ? (pad + MINIMAP_W * S + 8 * S) : pad;
  var hy = pad + 12 * S;
  var hpct = Math.max(0, Math.min(1, health / HEALTH_MAX));
  var pct = Math.max(0, Math.min(1, mana / MANA_MAX));
  var blink = now < manaBlinkUntil;
  var my = hy + 22 * S;
  drawHudPanel(hudX - 4 * S, pad - 3 * S, barW + 8 * S, 48 * S, S);
  ctx.font = 'bold ' + Math.max(8, Math.floor(8 * S)) + 'px Arial';
  ctx.textAlign = 'left';
  ctx.fillStyle = hpct <= 0.25 ? HUD_COLORS.warning : HUD_COLORS.ink;
  ctx.fillText('HEALTH', hudX, hy - 3 * S);
  ctx.textAlign = 'right';
  ctx.fillText(Math.ceil(Math.max(0, health)) + '/' + HEALTH_MAX, hudX + barW, hy - 3 * S);
  ctx.fillStyle = HUD_COLORS.track;
  ctx.fillRect(hudX, hy, barW, barH);
  ctx.fillStyle = hpct <= 0.25 ? '#e47668' : HUD_COLORS.health;
  ctx.fillRect(hudX, hy, Math.floor(barW * hpct), barH);
  ctx.fillStyle = blink ? HUD_COLORS.warning : HUD_COLORS.ink;
  ctx.textAlign = 'left';
  ctx.fillText('MANA', hudX, my - 3 * S);
  ctx.textAlign = 'right';
  ctx.fillText(Math.floor(Math.max(0, mana)) + '/' + MANA_MAX, hudX + barW, my - 3 * S);
  ctx.fillStyle = HUD_COLORS.track;
  ctx.fillRect(hudX, my, barW, barH);
  ctx.fillStyle = blink ? HUD_COLORS.warning : HUD_COLORS.mana;
  ctx.fillRect(hudX, my, Math.floor(barW * pct), barH);

  // ── Active buff indicators ──
  var _buffNow = Date.now();
  var _buffs = [];
  if (_buffNow < dmgBoostUntil) _buffs.push({name: 'PWR', color: '#ff4444', remaining: dmgBoostUntil - _buffNow});
  if (_buffNow < speedBoostUntil) _buffs.push({name: 'SPD', color: '#44ffff', remaining: speedBoostUntil - _buffNow});
  if (_buffNow < regenBoostUntil) _buffs.push({name: 'REG', color: '#44ff44', remaining: regenBoostUntil - _buffNow});
  if (_buffNow < armorBoostUntil) _buffs.push({name: 'ARM', color: '#ffaa44', remaining: armorBoostUntil - _buffNow});
  if (_buffs.length > 0) {
    var bx = hudX, by = my + barH + 6 * S;
    ctx.font = 'bold ' + Math.floor(9 * S) + 'px monospace';
    ctx.textAlign = 'left';
    for (var _bi = 0; _bi < _buffs.length; _bi++) {
      var _bf = _buffs[_bi];
      var secs = Math.ceil(_bf.remaining / 1000);
      ctx.globalAlpha = 0.85;
      ctx.fillStyle = _bf.color;
      ctx.fillRect(bx, by, 4 * S, 8 * S);
      ctx.fillStyle = '#ffffff';
      ctx.fillText(_bf.name + ' ' + secs + 's', bx + 6 * S, by + 7 * S);
      by += 10 * S;
    }
  }

  // ── Shop prompt — centered ──
  if (shopNearby && !shopOpen) {
    ctx.globalAlpha = 0.9 + 0.1 * Math.sin(Date.now() * 0.006);
    ctx.fillStyle = '#ffd700'; ctx.font = 'bold ' + Math.floor(14 * S) + 'px Arial'; ctx.textAlign = 'center';
    ctx.fillText('[E]  Enter Shop', w / 2, h / 2 + 40 * S);
  }

  // Keep the equipped spell visible. The thin readiness line communicates a
  // cooldown, while a written warning remains useful when the mana bar is empty.
  var spell = getCurrentSpell();
  var spellCost = spell.manaCost * ((equipment.robes && equipment.robes.manaCostReduction) ?
    (1 - equipment.robes.manaCostReduction) : 1);
  var canAfford = mana >= spellCost;
  var spellReady = spell.attackType === 'stream' ? 1 :
    Math.max(0, Math.min(1, (now - lastShotMs) / getEffectiveCooldown()));
  var spellDetail = !canAfford ? 'Low mana' :
    (Math.round(spellCost * 10) / 10) + (spell.attackType === 'stream' ? ' mana / tick' : ' mana');
  if (canAfford && spell.attackType !== 'stream') spellDetail += spellReady < 1 ? ' · Recharging' : ' · Ready';
  ctx.font = Math.max(9, Math.floor(10 * S)) + 'px Arial';
  var spellW = Math.min(164 * S, Math.max(112 * S, ctx.measureText(spellDetail).width + 18 * S,
    ctx.measureText(spell.name).width + 28 * S));
  var spellX = w - pad - spellW, spellY = h - pad - 38 * S;
  ctx.globalAlpha = 1;
  drawHudPanel(spellX, spellY, spellW, 30 * S, S);
  ctx.fillStyle = spell.color;
  ctx.fillRect(spellX + 7 * S, spellY + 7 * S, 4 * S, 4 * S);
  ctx.fillStyle = HUD_COLORS.ink; ctx.textAlign = 'left';
  ctx.fillText(spell.name, spellX + 16 * S, spellY + 12 * S, spellW - 22 * S);
  ctx.font = Math.max(8, Math.floor(8 * S)) + 'px Arial';
  ctx.fillStyle = canAfford ? HUD_COLORS.muted : HUD_COLORS.warning;
  ctx.fillText(spellDetail, spellX + 7 * S, spellY + 23 * S, spellW - 14 * S);
  ctx.fillStyle = canAfford ? spell.color : HUD_COLORS.warning;
  ctx.fillRect(spellX + 1 * S, spellY + 28 * S, (spellW - 2 * S) * (canAfford ? spellReady : 1), S);

  // Short onboarding hint uses the active control mode, then leaves the view.
  var hintAge = (now - startMs) / 1000;
  if (hintAge < 10 && USE_KEYBOARD && !menuOpen && !settingsOpen) {
    ctx.globalAlpha = Math.min(1, Math.max(0, (10 - hintAge) / 2));
    drawHudPanel(pad - 3 * S, h - pad - 24 * S, 146 * S, 27 * S, S);
    ctx.fillStyle = HUD_COLORS.muted;
    ctx.font = Math.max(8, Math.floor(8 * S)) + 'px Arial'; ctx.textAlign = 'left';
    ctx.fillText('WASD move · Click cast · E interact', pad + 3 * S, h - pad - 13 * S);
    ctx.fillText('Space jump · Shift dash · P pause', pad + 3 * S, h - pad - 3 * S);
  }

  // ── Toast notifications — small corner stack ──
  var _tNow = Date.now();
  toasts = toasts.filter(function(t){ return _tNow - t.spawnMs < t.lifeMs; });
  if (toasts.length > 0) {
    ctx.font = Math.floor(11 * S) + 'px monospace';
    var _tH = Math.floor(17 * S);
    var _tPad = Math.floor(7 * S);
    var _tGap = Math.floor(3 * S);
    var _tBaseY = h - Math.floor(78 * S);
    for (var _ti = toasts.length - 1; _ti >= 0; _ti--) {
      var _t = toasts[_ti];
      var _tAge = _tNow - _t.spawnMs;
      var _tFade = _tAge < 150 ? _tAge / 150 : (_t.lifeMs - _tAge < 350 ? (_t.lifeMs - _tAge) / 350 : 1.0);
      _tFade = Math.max(0, Math.min(1, _tFade));
      var _tW = Math.min(Math.ceil(ctx.measureText(_t.text).width) + _tPad * 2, Math.floor(260 * S), w - 16 * S);
      var _tY = _tBaseY - (toasts.length - 1 - _ti) * (_tH + _tGap);
      ctx.globalAlpha = _tFade * 0.92;
      ctx.fillStyle = 'rgba(8,8,12,0.86)';
      ctx.fillRect(Math.floor(8 * S), _tY, _tW, _tH);
      ctx.strokeStyle = _t.color; ctx.lineWidth = 1;
      ctx.strokeRect(Math.floor(8 * S) + 0.5, _tY + 0.5, _tW - 1, _tH - 1);
      ctx.fillStyle = _t.color;
      ctx.textAlign = 'left';
      ctx.fillText(_t.text, Math.floor(8 * S) + _tPad, _tY + Math.floor(11.5 * S), _tW - _tPad * 2);
    }
    ctx.globalAlpha = 1.0;
  }

  // ── Toast log panel (C key) — sits below the minimap, above the toast stack ──
  if (toastLogOpen && toastLog.length > 0) {
    var _logW = Math.floor(260 * S);
    var _logX = Math.floor(8 * S);
    // Top: just below minimap (pad + MINIMAP_H + gap)
    var _logTop = Math.floor((10 + MINIMAP_H + 6) * S);
    // Bottom: just above the toast stack anchor
    var _logBottom = Math.floor(h - 56 * S);
    var _logH = Math.max(Math.floor(40 * S), _logBottom - _logTop);
    var _logY = _logTop;
    var _logLineH = Math.floor(15 * S);
    var _logVisible = Math.floor((_logH - 18 * S) / _logLineH);
    var _logTotal = toastLog.length;
    toastLogScroll = Math.max(0, Math.min(toastLogScroll, _logTotal - _logVisible));
    var _logStart = Math.max(0, _logTotal - _logVisible - toastLogScroll);
    var _logEnd = Math.min(_logTotal, _logStart + _logVisible);
    ctx.globalAlpha = 0.93;
    ctx.fillStyle = 'rgba(6,6,10,0.92)';
    ctx.fillRect(_logX, _logY, _logW, _logH);
    ctx.strokeStyle = 'rgba(120,120,140,0.6)'; ctx.lineWidth = 1;
    ctx.strokeRect(_logX + 0.5, _logY + 0.5, _logW - 1, _logH - 1);
    // Header
    ctx.fillStyle = 'rgba(80,80,100,0.8)';
    ctx.fillRect(_logX, _logY, _logW, Math.floor(15 * S));
    ctx.fillStyle = '#aaaacc'; ctx.font = 'bold ' + Math.floor(9 * S) + 'px monospace'; ctx.textAlign = 'left';
    ctx.fillText('JOURNAL  [scroll \u2191\u2193]  [C] close', _logX + Math.floor(6 * S), _logY + Math.floor(10 * S));
    // Scrollbar
    if (_logTotal > _logVisible) {
      var _sbH = Math.floor((_logH - 18 * S) * _logVisible / _logTotal);
      var _sbY = _logY + Math.floor(16 * S) + Math.floor((_logH - 18 * S - _sbH) * (1 - (_logTotal - _logVisible - toastLogScroll) / (_logTotal - _logVisible)));
      ctx.fillStyle = 'rgba(100,100,130,0.5)';
      ctx.fillRect(_logX + _logW - Math.floor(4 * S), _logY + Math.floor(16 * S), Math.floor(3 * S), _logH - Math.floor(18 * S));
      ctx.fillStyle = 'rgba(180,180,220,0.7)';
      ctx.fillRect(_logX + _logW - Math.floor(4 * S), _sbY, Math.floor(3 * S), _sbH);
    }
    // Entries
    ctx.font = Math.floor(9 * S) + 'px monospace';
    for (var _li = _logStart; _li < _logEnd; _li++) {
      var _le = toastLog[_li];
      var _ly = _logY + Math.floor(16 * S) + (_li - _logStart) * _logLineH + Math.floor(10 * S);
      ctx.fillStyle = 'rgba(100,100,110,0.8)'; ctx.textAlign = 'left';
      ctx.fillText(_le.timeLabel, _logX + Math.floor(4 * S), _ly);
      ctx.fillStyle = _le.color;
      ctx.fillText(_le.text, _logX + Math.floor(44 * S), _ly);
    }
    ctx.globalAlpha = 1.0; ctx.textAlign = 'left';
  }

  // ── Equipment slots — below mana bar ──
  var eqSlotSize = 16 * S, eqGap = 3 * S;
  var eqX = hudX;
  var eqY = my + barH + 6 * S + (_buffs.length > 0 ? _buffs.length * 10 * S + 4 * S : 0);
  for (var eqi = 0; eqi < EQUIP_SLOTS.length; eqi++) {
    var _esd = EQUIP_SLOTS[eqi];
    var slotX = eqX + eqi * (eqSlotSize + eqGap);
    var eqItem = equipment[_esd.key];
    var isRelic = _esd.key === 'relic';
    ctx.globalAlpha = 0.8;
    ctx.fillStyle = HUD_COLORS.panel;
    ctx.fillRect(slotX, eqY, eqSlotSize, eqSlotSize);
    if (eqItem) {
      ctx.strokeStyle = isRelic ? RARITY_COLORS.epic : (RARITY_COLORS[eqItem.rarity] || '#888');
      ctx.lineWidth = 2 * S;
      ctx.strokeRect(slotX, eqY, eqSlotSize, eqSlotSize);
      if (isRelic && eqItem.shape) {
        drawRelicIcon(eqItem, slotX + eqSlotSize/2, eqY + eqSlotSize/2, eqSlotSize * 0.8, ctx);
      } else {
        ctx.fillStyle = '#fff'; ctx.font = 'bold ' + Math.floor(10 * S) + 'px Arial'; ctx.textAlign = 'center';
        ctx.fillText(_esd.icon, slotX + eqSlotSize/2, eqY + eqSlotSize - 3 * S);
      }
    } else {
      ctx.strokeStyle = isRelic ? 'rgba(180,80,255,0.3)' : 'rgba(255,255,255,0.2)';
      ctx.lineWidth = 1 * S;
      ctx.strokeRect(slotX, eqY, eqSlotSize, eqSlotSize);
    }
    // Yellow glow outline on selected slot
    if (eqi === inventorySelIdx) {
      ctx.save();
      ctx.globalAlpha = 1;
      ctx.strokeStyle = HUD_COLORS.accent;
      ctx.lineWidth = 2 * S;
      ctx.strokeRect(slotX - 1 * S, eqY - 1 * S, eqSlotSize + 2 * S, eqSlotSize + 2 * S);
      ctx.restore();
    }
  }

  // ── Equipment tooltip for selected slot ──
  if (inventorySelIdx >= 0 && inventorySelIdx < EQUIP_SLOTS.length) {
    var _tipDef = EQUIP_SLOTS[inventorySelIdx];
    var _tipItem = equipment[_tipDef.key];
    var _tipIsRelic = _tipDef.key === 'relic';
    var _tipX = eqX;
    var _tipY = eqY + eqSlotSize + 4 * S;
    var _tipW = Math.min(barW + 40 * S, 180 * S);
    var _tipH, _tipLines = [];
    _tipLines.push({text: _tipDef.label, color: '#8888aa', bold: false, size: Math.floor(10 * S)});
    if (_tipItem) {
      var _tipNameCol = _tipIsRelic ? RARITY_COLORS.epic : (RARITY_COLORS[_tipItem.rarity] || '#ccc');
      _tipLines.push({text: _tipItem.displayName || _tipItem.name, color: _tipNameCol, bold: true, size: Math.floor(12 * S)});
      _tipLines.push({text: _tipItem.desc, color: '#bbbbbb', bold: false, size: Math.floor(10 * S)});
      if (_tipItem.quality && Math.abs(_tipItem.quality - 1.0) > 0.01) {
        _tipLines.push({text: 'Quality: ' + Math.round(_tipItem.quality * 100) + '%', color: '#aaaacc', bold: false, size: Math.floor(9 * S)});
      }
    } else {
      _tipLines.push({text: 'Empty', color: '#555555', bold: false, size: Math.floor(11 * S)});
    }
    _tipH = 8 * S;
    for (var _tli = 0; _tli < _tipLines.length; _tli++) _tipH += _tipLines[_tli].size + 3 * S;
    // Draw tooltip background
    ctx.globalAlpha = 0.9;
    ctx.fillStyle = HUD_COLORS.panel;
    ctx.fillRect(_tipX, _tipY, _tipW, _tipH);
    ctx.strokeStyle = HUD_COLORS.edge; ctx.lineWidth = 1 * S;
    ctx.strokeRect(_tipX, _tipY, _tipW, _tipH);
    // Draw relic icon in tooltip
    if (_tipIsRelic && _tipItem && _tipItem.shape) {
      drawRelicIcon(_tipItem, _tipX + _tipW - 16 * S, _tipY + _tipH / 2, 22 * S, ctx);
    }
    // Draw text lines
    var _tlY = _tipY + 4 * S;
    ctx.textAlign = 'left';
    for (var _tli2 = 0; _tli2 < _tipLines.length; _tli2++) {
      var _tl = _tipLines[_tli2];
      _tlY += _tl.size + 2 * S;
      ctx.globalAlpha = 0.95;
      ctx.fillStyle = _tl.color;
      ctx.font = (_tl.bold ? 'bold ' : '') + _tl.size + 'px Arial';
      ctx.fillText(_tl.text, _tipX + 6 * S, _tlY, _tipW - (_tipIsRelic && _tipItem ? 38 : 12) * S);
    }
  }

  // ── Endless mode HUD — biome + debug info (top-right) ──
  if (ENDLESS_MODE) {
    var trueX = pos.x + windowOriginX;
    var trueY = pos.y + windowOriginY;
    var distFromOrigin = Math.floor(Math.hypot(trueX, trueY));
    var biomeLabel = terrain.charAt(0).toUpperCase() + terrain.slice(1);
    var diff = getDifficultyAt(trueX, trueY);
    ctx.globalAlpha = 1;
    var regionW = Math.max(60 * S, w - pad - hudX - barW - 16 * S);
    regionW = Math.min(regionW, 128 * S);
    drawHudPanel(w - pad - regionW, pad - 3 * S, regionW, 32 * S, S);
    ctx.font = 'bold ' + Math.max(8, Math.floor(9 * S)) + 'px Arial'; ctx.textAlign = 'right';
    ctx.fillStyle = HUD_COLORS.ink;
    ctx.fillText(biomeLabel, w - pad - 5 * S, pad + 9 * S, regionW - 10 * S);
    ctx.font = Math.max(8, Math.floor(8 * S)) + 'px Arial';
    ctx.fillStyle = HUD_COLORS.muted;
    ctx.fillText(distFromOrigin + 'm · Danger ' + diff.toFixed(1), w - pad - 5 * S, pad + 22 * S, regionW - 10 * S);
    // Chunk debug: show player chunk, window bounds, distance to edge (requires 3D debug overlay)
    if (DEBUG_3D) {
      var pcx = Math.floor(trueX / CHUNK_SIZE);
      var pcy = Math.floor(trueY / CHUNK_SIZE);
      var half = Math.floor(WINDOW_CHUNKS / 2);
      var wCenterCX = windowCX + half, wCenterCY = windowCY + half;
      var edgeDist = Math.min(
        pos.x, pos.y,
        worldW - pos.x, worldH - pos.y
      );
      var _chkCol = (pcx !== wCenterCX || pcy !== wCenterCY) ? '#ff4444' : '#88ff88';
      ctx.fillStyle = _chkCol;
      ctx.fillText('chunk(' + pcx + ',' + pcy + ')', w - pad, pad + 49 * S);
      ctx.fillText('win(' + windowCX + ',' + windowCY + ')-(' + (windowCX+WINDOW_CHUNKS-1) + ',' + (windowCY+WINDOW_CHUNKS-1) + ')', w - pad, pad + 61 * S);
      ctx.fillText('edge:' + Math.floor(edgeDist) + 'px', w - pad, pad + 73 * S);
    }
  }

  if (coins > 0 || (shopMarker && shopNearby)) {
    ctx.globalAlpha = 1; ctx.fillStyle = HUD_COLORS.accent;
    ctx.font = 'bold ' + Math.max(8, Math.floor(9 * S)) + 'px Arial'; ctx.textAlign = 'right';
    ctx.fillText(coins + ' gold', w - pad, pad + (ENDLESS_MODE ? 39 : 12) * S);
  }

  // Time-of-day indicator — bottom-right
  if (settings.dayNight) {
    var timeLabel, timeIcon;
    if (dayTime < 0.15 || dayTime >= 0.85) { timeLabel = 'Night'; timeIcon = '\u263E'; }
    else if (dayTime < 0.30) { timeLabel = 'Dawn'; timeIcon = '\u2600'; }
    else if (dayTime < 0.70) { timeLabel = 'Day'; timeIcon = '\u2600'; }
    else { timeLabel = 'Dusk'; timeIcon = '\u263E'; }
    ctx.globalAlpha = 0.7;
    ctx.font = Math.floor(11 * S) + 'px monospace';
    ctx.textAlign = 'right';
    ctx.fillStyle = dayTime >= 0.30 && dayTime < 0.70 ? '#ffdd66' : '#8899bb';
    ctx.fillText(timeIcon + ' ' + timeLabel, w - pad, h - pad);
  }

  // FPS counter — top-right
  if (settings.showFPS) {
    var fpsNow = Date.now();
    var frameDt = fpsNow - (_lastFrameTimeMs || fpsNow);
    _lastFrameTimeMs = fpsNow;
    if (frameDt > 0 && frameDt < 500) _fpsSmooth = _fpsSmooth * 0.9 + (1000 / frameDt) * 0.1;
    ctx.font = 'bold ' + Math.floor(12 * S) + 'px monospace';
    ctx.textAlign = 'right';
    ctx.fillStyle = _fpsSmooth > 45 ? '#44ff44' : _fpsSmooth > 25 ? '#ffcc00' : '#ff4444';
    var _fpsY = pad + (ENDLESS_MODE ? (DEBUG_3D ? 86 : 52) : 26) * S;
    ctx.fillText(Math.round(_fpsSmooth) + ' FPS', w - pad, _fpsY);
  }

  // Inventory overlay (I key toggle)
  // inventory tooltip drawn inline in HUD equip section

  // Minimap
  drawMinimap();

  ctx.restore();
}

function drawInventoryOverlay() {
  var w = canvas.width, h = canvas.height;
  var S = resScale;
  var panW = Math.min(340 * S, w * 0.7), panH = Math.min(420 * S, h * 0.9);
  var px = Math.floor((w - panW) / 2), py = Math.floor((h - panH) / 2);

  ctx.globalAlpha = 0.92;
  ctx.fillStyle = HUD_COLORS.panel;
  ctx.fillRect(px, py, panW, panH);
  ctx.strokeStyle = 'rgba(100,150,255,0.7)'; ctx.lineWidth = 2 * S;
  ctx.strokeRect(px, py, panW, panH);

  // Title
  ctx.fillStyle = '#ffffff'; ctx.font = 'bold ' + Math.floor(16 * S) + 'px Arial'; ctx.textAlign = 'center';
  ctx.fillText('Equipment  [I to close]', px + panW / 2, py + 22 * S);

  // Slot entries
  var ly = py + 40 * S;
  var slotH = Math.min(60 * S, (panH - 80 * S) / EQUIP_SLOTS.length);

  for (var si = 0; si < EQUIP_SLOTS.length; si++) {
    var _esDef = EQUIP_SLOTS[si];
    var slotItem = equipment[_esDef.key];
    var isRelic = _esDef.key === 'relic';
    var slotY = ly + si * slotH;

    // Slot background
    ctx.globalAlpha = 0.7;
    ctx.fillStyle = slotItem ? 'rgba(30,30,60,0.8)' : 'rgba(20,20,30,0.5)';
    ctx.fillRect(px + 12 * S, slotY, panW - 24 * S, slotH - 6 * S);

    // Proportional text positions within each slot
    var _sh = slotH - 6 * S; // inner slot height (minus gap)
    var _labelFontSz = Math.max(8, Math.floor(Math.min(11 * S, _sh * 0.22)));
    var _nameFontSz  = Math.max(9, Math.floor(Math.min(13 * S, _sh * 0.26)));
    var _descFontSz  = Math.max(8, Math.floor(Math.min(11 * S, _sh * 0.20)));
    var _yLabel = slotY + _sh * 0.26;
    var _yName  = slotY + _sh * 0.55;
    var _yDesc  = slotY + _sh * 0.82;

    // Slot label
    ctx.globalAlpha = 0.9;
    ctx.fillStyle = '#8888aa'; ctx.font = _labelFontSz + 'px Arial'; ctx.textAlign = 'left';
    ctx.fillText(_esDef.label, px + 18 * S, _yLabel);

    if (slotItem) {
      // Rarity-colored border (relics use epic color)
      var _invRarCol = isRelic ? RARITY_COLORS.epic : (RARITY_COLORS[slotItem.rarity] || '#888');
      ctx.strokeStyle = _invRarCol;
      ctx.lineWidth = 2 * S;
      ctx.strokeRect(px + 12 * S, slotY, panW - 24 * S, _sh);

      // Item name (use displayName if available for quality prefix)
      ctx.fillStyle = _invRarCol;
      ctx.font = 'bold ' + _nameFontSz + 'px Arial'; ctx.textAlign = 'left';
      ctx.fillText(slotItem.displayName || slotItem.name, px + 18 * S, _yName);

      // Description
      ctx.fillStyle = '#bbbbbb'; ctx.font = _descFontSz + 'px Arial';
      ctx.fillText(slotItem.desc, px + 18 * S, _yDesc);
      // Draw relic icon in slot
      if (isRelic && slotItem.shape) {
        ctx.globalAlpha = 0.9;
        var _iconSz = Math.min(28 * S, _sh * 0.7);
        drawRelicIcon(slotItem, px + panW - 36 * S, slotY + _sh / 2, _iconSz, ctx);
      }
    } else {
      ctx.strokeStyle = isRelic ? 'rgba(180,80,255,0.15)' : 'rgba(255,255,255,0.15)';
      ctx.lineWidth = 1 * S;
      ctx.strokeRect(px + 12 * S, slotY, panW - 24 * S, _sh);
      ctx.fillStyle = '#555555'; ctx.font = _nameFontSz + 'px Arial'; ctx.textAlign = 'left';
      ctx.fillText('Empty', px + 18 * S, _yName);
    }
  }

  // Stats summary
  var statY = ly + EQUIP_SLOTS.length * slotH + 10 * S;
  ctx.fillStyle = '#aaaacc'; ctx.font = Math.floor(11 * S) + 'px Arial'; ctx.textAlign = 'left';
  var statLines = [];
  if (equipment.armor) statLines.push('-' + Math.round(equipment.armor.damageReduction * 100) + '% damage taken');
  if (equipment.hat && equipment.hat.manaRegen) statLines.push('+' + equipment.hat.manaRegen + ' mana/s');
  if (equipment.hat && equipment.hat.hpRegen) statLines.push('+' + equipment.hat.hpRegen + ' HP/s');
  if (equipment.hat && equipment.hat.speedBonus) statLines.push('+' + Math.round(equipment.hat.speedBonus * 100) + '% speed');
  if (equipment.robes && equipment.robes.spellDmgBonus) statLines.push('+' + Math.round(equipment.robes.spellDmgBonus * 100) + '% spell damage');
  if (equipment.robes && equipment.robes.manaCostReduction) statLines.push('-' + Math.round(equipment.robes.manaCostReduction * 100) + '% mana cost');
  if (equipment.boots && equipment.boots.canDash) statLines.push('Dash (Shift)');
  if (equipment.boots && equipment.boots.canJump) statLines.push('Jump (Space)');
  if (equipment.relic) statLines.push(equipment.relic.name + ': ' + equipment.relic.desc);
  if (statLines.length > 0) {
    ctx.fillStyle = '#88cc88';
    ctx.fillText('Active bonuses: ' + statLines.join(', '), px + 18 * S, statY);
  } else {
    ctx.fillStyle = '#666666';
    ctx.fillText('No equipment bonuses active', px + 18 * S, statY);
  }
}

// Procedural relic icon drawing
function drawRelicIcon(rDef, cx, cy, sz, _ctx) {
  _ctx.save();
  _ctx.fillStyle = rDef.color;
  _ctx.strokeStyle = rDef.accent;
  _ctx.lineWidth = Math.max(1, sz * 0.1);
  var r = sz * 0.4;
  if (rDef.shape === 'orb') {
    _ctx.beginPath(); _ctx.arc(cx, cy, r, 0, Math.PI * 2); _ctx.fill(); _ctx.stroke();
    _ctx.fillStyle = rDef.accent; _ctx.globalAlpha = 0.5;
    _ctx.beginPath(); _ctx.arc(cx - r * 0.25, cy - r * 0.25, r * 0.3, 0, Math.PI * 2); _ctx.fill();
  } else if (rDef.shape === 'feather') {
    _ctx.beginPath(); _ctx.moveTo(cx, cy - r);
    _ctx.quadraticCurveTo(cx + r * 1.2, cy, cx, cy + r);
    _ctx.quadraticCurveTo(cx - r * 0.4, cy, cx, cy - r);
    _ctx.fill(); _ctx.stroke();
  } else if (rDef.shape === 'crystal') {
    _ctx.beginPath();
    _ctx.moveTo(cx, cy - r * 1.2); _ctx.lineTo(cx + r * 0.7, cy);
    _ctx.lineTo(cx, cy + r * 1.2); _ctx.lineTo(cx - r * 0.7, cy);
    _ctx.closePath(); _ctx.fill(); _ctx.stroke();
  } else if (rDef.shape === 'sphere') {
    _ctx.beginPath(); _ctx.arc(cx, cy, r, 0, Math.PI * 2); _ctx.fill(); _ctx.stroke();
    _ctx.strokeStyle = rDef.accent; _ctx.lineWidth = Math.max(1, sz * 0.06);
    for (var zi = 0; zi < 3; zi++) {
      var za = (zi / 3) * Math.PI + 0.3;
      _ctx.beginPath();
      _ctx.moveTo(cx + Math.cos(za) * r * 0.7, cy + Math.sin(za) * r * 0.7);
      _ctx.lineTo(cx + Math.cos(za + 0.8) * r * 0.5, cy + Math.sin(za + 0.8) * r * 0.3);
      _ctx.lineTo(cx + Math.cos(za + 1.6) * r * 0.7, cy + Math.sin(za + 1.6) * r * 0.7);
      _ctx.stroke();
    }
  } else if (rDef.shape === 'lens') {
    _ctx.beginPath(); _ctx.ellipse(cx, cy, r * 1.1, r * 0.6, 0, 0, Math.PI * 2); _ctx.fill(); _ctx.stroke();
    _ctx.strokeStyle = rDef.accent; _ctx.lineWidth = Math.max(1, sz * 0.06);
    _ctx.beginPath(); _ctx.moveTo(cx - r * 0.5, cy - r * 0.5); _ctx.lineTo(cx + r * 0.5, cy + r * 0.5); _ctx.stroke();
    _ctx.beginPath(); _ctx.moveTo(cx + r * 0.5, cy - r * 0.5); _ctx.lineTo(cx - r * 0.5, cy + r * 0.5); _ctx.stroke();
  } else if (rDef.shape === 'heart') {
    _ctx.beginPath();
    _ctx.moveTo(cx, cy + r * 0.8);
    _ctx.bezierCurveTo(cx - r * 1.5, cy - r * 0.2, cx - r * 0.5, cy - r * 1.3, cx, cy - r * 0.5);
    _ctx.bezierCurveTo(cx + r * 0.5, cy - r * 1.3, cx + r * 1.5, cy - r * 0.2, cx, cy + r * 0.8);
    _ctx.fill(); _ctx.stroke();
  }
  _ctx.restore();
}

function drawMenuOverlay() {
  if (!menuOpen) return;
  var w = canvas.width, h = canvas.height;
  ctx.save(); ctx.globalAlpha = 0.85;
  ctx.fillStyle = 'rgba(0,0,0,0.75)'; ctx.fillRect(0, 0, w, h);
  var menuW = Math.min(400, w * 0.6), menuH = Math.min(300, h * 0.6);
  var mx = (w - menuW) / 2, my = (h - menuH) / 2;
  ctx.fillStyle = 'rgba(20,20,40,0.95)'; ctx.strokeStyle = 'rgba(100,150,255,0.8)'; ctx.lineWidth = 2;
  ctx.fillRect(mx, my, menuW, menuH); ctx.strokeRect(mx, my, menuW, menuH);
  ctx.fillStyle = '#ffffff'; ctx.font = 'bold 20px Arial'; ctx.textAlign = 'center';
  ctx.fillText('Player Stats', mx + menuW / 2, my + 30);
  ctx.font = '16px Arial'; ctx.textAlign = 'left';
  var lineY = my + 60, lineH = 24, leftX = mx + 20;
  ctx.fillStyle = '#aaddff'; ctx.fillText('Mana:', leftX, lineY);
  ctx.fillStyle = '#ffffff';
  var manaPct = ((mana / MANA_MAX) * 100).toFixed(0);
  ctx.fillText(Math.floor(mana) + ' / ' + MANA_MAX + ' (' + manaPct + '%)', leftX + 120, lineY);
  lineY += lineH;
  ctx.fillStyle = '#aaddff'; ctx.fillText('Health:', leftX, lineY);
  ctx.fillStyle = '#ffffff';
  var hpPct = ((health / HEALTH_MAX) * 100).toFixed(0);
  ctx.fillText(Math.floor(health) + ' / ' + HEALTH_MAX + ' (' + hpPct + '%)', leftX + 120, lineY);
  lineY += lineH;
  ctx.fillStyle = '#aaddff'; ctx.fillText('Level:', leftX, lineY);
  ctx.fillStyle = '#ffffff'; ctx.fillText(level.toString(), leftX + 120, lineY);
  lineY += lineH;
  ctx.fillStyle = '#aaddff'; ctx.fillText('Enemies Defeated:', leftX, lineY);
  ctx.fillStyle = '#ffffff'; ctx.fillText(stats.totalEnemiesKilled.toString(), leftX + 120, lineY);
  lineY += lineH + 10;
  var barW2 = menuW - 40, barH2 = 20;
  var barX = mx + 20, barY = lineY;
  ctx.fillStyle = 'rgba(0,0,0,0.5)'; ctx.fillRect(barX, barY, barW2, barH2);
  var manaPct2 = Math.max(0, Math.min(1, mana / MANA_MAX));
  ctx.fillStyle = '#4db6ff'; ctx.fillRect(barX, barY, Math.floor(barW2 * manaPct2), barH2);
  ctx.strokeStyle = 'rgba(255,255,255,0.6)'; ctx.lineWidth = 1; ctx.strokeRect(barX, barY, barW2, barH2);
  ctx.fillStyle = '#ffffff'; ctx.font = '12px Arial'; ctx.textAlign = 'center';
  ctx.fillText('Mana', barX + barW2 / 2, barY + 14);
  ctx.font = '14px Arial'; ctx.textAlign = 'center'; ctx.fillStyle = '#ffdd88';
  ctx.fillText('Press Start to close', mx + menuW / 2, my + menuH - 20);
  ctx.restore();
}


function applySettings() {
  viewDist = settings.viewDist;
  AMBIENT_MAX = settings.particles;
  if (WINDOW_CHUNKS !== settings.chunkWindow) {
    WINDOW_CHUNKS = settings.chunkWindow;
    // assembleWindow() is intentionally NOT called here — it is expensive and
    // freezes the main thread. The Apply button already closed the overlay;
    // updateChunks() in the game loop will trigger a natural window rebuild on
    // the next frame when the player moves, or we kick one immediately below.
    if (ENDLESS_MODE && pos) {
      var pcx = Math.floor((pos.x + windowOriginX) / CHUNK_SIZE);
      var pcy = Math.floor((pos.y + windowOriginY) / CHUNK_SIZE);
      // Schedule the rebuild for the next animation frame so the overlay has
      // already been dismissed and the browser can paint before we block.
      var _rebuildPcx = pcx, _rebuildPcy = pcy;
      requestAnimationFrame(function() { assembleWindow(_rebuildPcx, _rebuildPcy); });
    }
  }
  // Resolution scaling — apply to canvas if in fullscreen
  var fs = document.fullscreenElement || document.webkitFullscreenElement
         || document.mozFullScreenElement || document.msFullscreenElement;
  if (fs === canvas) {
    var baseW = window.innerWidth || screen.width;
    var baseH = window.innerHeight || screen.height;
    canvas.width  = Math.floor(baseW * settings.resolution);
    canvas.height = Math.floor(baseH * settings.resolution);
  }
}

function detectPreset() {
  var presetNames = ['low', 'medium', 'high', 'ultra'];
  for (var pi = 0; pi < presetNames.length; pi++) {
    var p = QUALITY_PRESETS[presetNames[pi]];
    if (settings.viewDist === p.viewDist && settings.chunkWindow === p.chunkWindow &&
        settings.particles === p.particles && settings.resolution === p.resolution) {
      qualityPreset = presetNames[pi];
      return;
    }
  }
  qualityPreset = 'custom';
}

function drawSettingsOverlay() {
  if (!settingsOpen) return;
  var w = canvas.width, h = canvas.height;
  var S = resScale;
  ctx.save();

  // Dim background
  ctx.fillStyle = 'rgba(0,0,0,0.7)';
  ctx.fillRect(0, 0, w, h);

  // Panel — scale-aware sizing, themed to match page
  var panW = Math.min(320 * S, w * 0.85), panH = Math.min(260 * S, h * 0.92);
  var px = (w - panW) / 2, py = (h - panH) / 2;
  ctx.fillStyle = '#1a1a2e';
  ctx.strokeStyle = '#333355';
  ctx.lineWidth = Math.max(1, 2 * S);
  var panR = Math.max(4, 12 * S);
  ctx.beginPath();
  ctx.moveTo(px + panR, py); ctx.lineTo(px + panW - panR, py); ctx.arcTo(px + panW, py, px + panW, py + panR, panR);
  ctx.lineTo(px + panW, py + panH - panR); ctx.arcTo(px + panW, py + panH, px + panW - panR, py + panH, panR);
  ctx.lineTo(px + panR, py + panH); ctx.arcTo(px, py + panH, px, py + panH - panR, panR);
  ctx.lineTo(px, py + panR); ctx.arcTo(px, py, px + panR, py, panR);
  ctx.closePath(); ctx.fill(); ctx.stroke();

  _settingsClickAreas = [];
  var pad = 12 * S;
  var leftX = px + pad, rightX = px + panW - pad;
  var curY = py + 8 * S;
  var fontTitle = Math.max(9, Math.floor(14 * S));
  var fontLabel = Math.max(7, Math.floor(10 * S));
  var fontSmall = Math.max(6, Math.floor(8 * S));

  // Title
  ctx.fillStyle = '#ffffff';
  ctx.font = 'bold ' + fontTitle + 'px Arial';
  ctx.textAlign = 'center';
  ctx.fillText('Settings', px + panW / 2, curY + fontTitle);
  curY += fontTitle + 10 * S;

  // Separator
  ctx.strokeStyle = '#333355'; ctx.lineWidth = 1;
  ctx.beginPath(); ctx.moveTo(leftX, curY); ctx.lineTo(rightX, curY); ctx.stroke();
  curY += 6 * S;

  // Preset buttons
  ctx.font = 'bold ' + fontSmall + 'px Arial';
  ctx.textAlign = 'center';
  ctx.fillStyle = '#e0e0e0';
  ctx.fillText('Quality Preset', px + panW / 2, curY + fontSmall);
  curY += fontSmall + 4 * S;
  var presetNames = ['low', 'medium', 'high', 'ultra'];
  var presetLabels = ['Low', 'Medium', 'High', 'Ultra'];
  var btnGap = 4 * S;
  var btnW = Math.floor((panW - pad * 2 - btnGap * 3) / 4);
  var btnH = Math.floor(16 * S);
  for (var bi = 0; bi < 4; bi++) {
    var bx = leftX + bi * (btnW + btnGap);
    var isActive = (_pendingQualityPreset === presetNames[bi]);
    // Match page .btn theme: #2a2a4a base, #3a3a5a hover/active, #444466 border
    ctx.fillStyle = isActive ? '#3a3a5a' : '#2a2a4a';
    // Rounded rect
    var br = Math.max(2, 4 * S);
    ctx.beginPath();
    ctx.moveTo(bx + br, curY); ctx.lineTo(bx + btnW - br, curY); ctx.arcTo(bx + btnW, curY, bx + btnW, curY + br, br);
    ctx.lineTo(bx + btnW, curY + btnH - br); ctx.arcTo(bx + btnW, curY + btnH, bx + btnW - br, curY + btnH, br);
    ctx.lineTo(bx + br, curY + btnH); ctx.arcTo(bx, curY + btnH, bx, curY + btnH - br, br);
    ctx.lineTo(bx, curY + br); ctx.arcTo(bx, curY, bx + br, curY, br);
    ctx.closePath(); ctx.fill();
    ctx.strokeStyle = isActive ? '#6688bb' : '#444466';
    ctx.lineWidth = 1;
    ctx.stroke();
    ctx.fillStyle = isActive ? '#ffffff' : '#e0e0e0';
    ctx.font = 'bold ' + fontSmall + 'px Arial';
    ctx.textAlign = 'center';
    ctx.fillText(presetLabels[bi], bx + btnW / 2, curY + btnH * 0.7);
    _settingsClickAreas.push({x: bx, y: curY, w: btnW, h: btnH, action: 'preset', value: presetNames[bi]});
  }
  curY += btnH + 8 * S;

  // Separator
  ctx.strokeStyle = '#333355'; ctx.lineWidth = 1;
  ctx.beginPath(); ctx.moveTo(leftX, curY); ctx.lineTo(rightX, curY); ctx.stroke();
  curY += 6 * S;

  // Slider rows
  var sliders = [
    {key: 'viewDist', label: 'Render Distance', min: 400, max: 1400, step: 100, fmt: function(v) { return v; }},
    {key: 'chunkWindow', label: 'Chunk Window', min: 5, max: 9, step: 2, fmt: function(v) { return v + 'x' + v; }},
    {key: 'particles', label: 'Particles', min: 0, max: 60, step: 5, fmt: function(v) { return v; }},
    {key: 'resolution', label: 'Resolution', min: 0.5, max: 1.0, step: 0.25, fmt: function(v) { return Math.round(v * 100) + '%'; }}
  ];

  var trackH = Math.max(3, 4 * S);
  var thumbR = Math.max(3, 4 * S);
  var sliderRowH = fontLabel + trackH + thumbR + 6 * S;
  for (var si = 0; si < sliders.length; si++) {
    var sl = sliders[si];
    var val = pendingSettings[sl.key]; // read from staging copy

    // Label + value on same line
    ctx.font = fontLabel + 'px Arial'; ctx.textAlign = 'left';
    ctx.fillStyle = '#e0e0e0';
    ctx.fillText(sl.label, leftX, curY + fontLabel);
    ctx.textAlign = 'right';
    ctx.fillStyle = '#e0e0e0';
    ctx.font = 'bold ' + fontLabel + 'px Arial';
    ctx.fillText(sl.fmt(val), rightX, curY + fontLabel);

    // Slider track — themed
    var trackX = leftX, trackW = panW - pad * 2;
    var trackY = curY + fontLabel + 4 * S;
    ctx.fillStyle = '#2a2a4a';
    ctx.fillRect(trackX, trackY, trackW, trackH);
    ctx.strokeStyle = '#444466'; ctx.lineWidth = 1;
    ctx.strokeRect(trackX, trackY, trackW, trackH);

    // Fill
    var pct = (val - sl.min) / (sl.max - sl.min);
    ctx.fillStyle = '#4a5a8a';
    ctx.fillRect(trackX, trackY, Math.floor(trackW * pct), trackH);

    // Thumb
    var thumbX = trackX + Math.floor(trackW * pct);
    ctx.fillStyle = '#e0e0e0';
    ctx.beginPath(); ctx.arc(thumbX, trackY + trackH / 2, thumbR, 0, Math.PI * 2); ctx.fill();

    // Click area
    _settingsClickAreas.push({x: trackX, y: trackY - thumbR, w: trackW, h: trackH + thumbR * 2, action: 'slider', key: sl.key, min: sl.min, max: sl.max, step: sl.step, trackX: trackX, trackW: trackW});

    curY += sliderRowH;
  }

  curY += 2 * S;

  // FPS toggle
  var fpsBoxSize = Math.floor(10 * S);
  var fpsX = leftX, fpsY = curY;
  ctx.fillStyle = pendingSettings.showFPS ? '#3a3a5a' : '#2a2a4a';
  ctx.fillRect(fpsX, fpsY, fpsBoxSize, fpsBoxSize);
  ctx.strokeStyle = pendingSettings.showFPS ? '#6688bb' : '#444466';
  ctx.lineWidth = 1;
  ctx.strokeRect(fpsX, fpsY, fpsBoxSize, fpsBoxSize);
  if (pendingSettings.showFPS) {
    ctx.fillStyle = '#e0e0e0'; ctx.font = 'bold ' + fontSmall + 'px Arial'; ctx.textAlign = 'center';
    ctx.fillText('\u2713', fpsX + fpsBoxSize / 2, fpsY + fpsBoxSize * 0.8);
  }
  ctx.font = fontLabel + 'px Arial'; ctx.textAlign = 'left'; ctx.fillStyle = '#e0e0e0';
  ctx.fillText('Show FPS Counter', fpsX + fpsBoxSize + 6 * S, fpsY + fpsBoxSize * 0.8);
  _settingsClickAreas.push({x: fpsX, y: fpsY, w: panW - pad * 2, h: fpsBoxSize, action: 'toggle', key: 'showFPS'});

  // Day/Night toggle
  curY += fpsBoxSize + 4 * S;
  var dnX = leftX, dnY = curY;
  ctx.fillStyle = pendingSettings.dayNight ? '#3a3a5a' : '#2a2a4a';
  ctx.fillRect(dnX, dnY, fpsBoxSize, fpsBoxSize);
  ctx.strokeStyle = pendingSettings.dayNight ? '#6688bb' : '#444466';
  ctx.lineWidth = 1;
  ctx.strokeRect(dnX, dnY, fpsBoxSize, fpsBoxSize);
  if (pendingSettings.dayNight) {
    ctx.fillStyle = '#e0e0e0'; ctx.font = 'bold ' + fontSmall + 'px Arial'; ctx.textAlign = 'center';
    ctx.fillText('\u2713', dnX + fpsBoxSize / 2, dnY + fpsBoxSize * 0.8);
  }
  ctx.font = fontLabel + 'px Arial'; ctx.textAlign = 'left'; ctx.fillStyle = '#e0e0e0';
  ctx.fillText('Day/Night Cycle', dnX + fpsBoxSize + 6 * S, dnY + fpsBoxSize * 0.8);
  _settingsClickAreas.push({x: dnX, y: dnY, w: panW - pad * 2, h: fpsBoxSize, action: 'toggle', key: 'dayNight'});

  // Apply button — bottom right of panel
  var applyBtnW = Math.floor(60 * S), applyBtnH = Math.floor(18 * S);
  var applyBtnX = px + panW - pad - applyBtnW;
  var applyBtnY = py + panH - pad - applyBtnH;
  var applyBr = Math.max(2, 4 * S);
  ctx.globalAlpha = _settingsDirty ? 1.0 : 0.35;
  ctx.fillStyle = '#2a2a4a';
  ctx.beginPath();
  ctx.moveTo(applyBtnX + applyBr, applyBtnY);
  ctx.lineTo(applyBtnX + applyBtnW - applyBr, applyBtnY);
  ctx.arcTo(applyBtnX + applyBtnW, applyBtnY, applyBtnX + applyBtnW, applyBtnY + applyBr, applyBr);
  ctx.lineTo(applyBtnX + applyBtnW, applyBtnY + applyBtnH - applyBr);
  ctx.arcTo(applyBtnX + applyBtnW, applyBtnY + applyBtnH, applyBtnX + applyBtnW - applyBr, applyBtnY + applyBtnH, applyBr);
  ctx.lineTo(applyBtnX + applyBr, applyBtnY + applyBtnH);
  ctx.arcTo(applyBtnX, applyBtnY + applyBtnH, applyBtnX, applyBtnY + applyBtnH - applyBr, applyBr);
  ctx.lineTo(applyBtnX, applyBtnY + applyBr);
  ctx.arcTo(applyBtnX, applyBtnY, applyBtnX + applyBr, applyBtnY, applyBr);
  ctx.closePath(); ctx.fill();
  ctx.strokeStyle = '#444466';
  ctx.lineWidth = 1; ctx.stroke();
  ctx.fillStyle = '#e0e0e0';
  ctx.font = 'bold ' + fontSmall + 'px Arial'; ctx.textAlign = 'center';
  ctx.fillText('Apply', applyBtnX + applyBtnW / 2, applyBtnY + applyBtnH * 0.72);
  ctx.globalAlpha = 1.0;
  _settingsClickAreas.push({x: applyBtnX, y: applyBtnY, w: applyBtnW, h: applyBtnH, action: 'apply'});

  // Close hint — centered below apply button at panel bottom
  ctx.font = fontSmall + 'px Arial'; ctx.textAlign = 'center'; ctx.fillStyle = '#778899';
  ctx.fillText('[O] or [ESC] to close', px + panW / 2, py + panH - 4 * S);

  ctx.restore();
}

function _settingsDirtyCheck() {
  _settingsDirty = (
    pendingSettings.viewDist    !== settings.viewDist   ||
    pendingSettings.chunkWindow !== settings.chunkWindow ||
    pendingSettings.particles   !== settings.particles  ||
    pendingSettings.resolution  !== settings.resolution ||
    pendingSettings.showFPS     !== settings.showFPS    ||
    pendingSettings.dayNight    !== settings.dayNight
  );
}

function handleSettingsClick(mx, my) {
  for (var i = 0; i < _settingsClickAreas.length; i++) {
    var a = _settingsClickAreas[i];
    if (mx >= a.x && mx <= a.x + a.w && my >= a.y && my <= a.y + a.h) {
      if (a.action === 'apply') {
        // Commit pendingSettings → settings and run the real apply
        settings.viewDist    = pendingSettings.viewDist;
        settings.chunkWindow = pendingSettings.chunkWindow;
        settings.particles   = pendingSettings.particles;
        settings.resolution  = pendingSettings.resolution;
        settings.showFPS     = pendingSettings.showFPS;
        settings.dayNight    = pendingSettings.dayNight;
        qualityPreset        = _pendingQualityPreset;
        _settingsDirty = false;
        applySettings();
        settingsOpen = false;
        return true;
      } else if (a.action === 'preset') {
        var p = QUALITY_PRESETS[a.value];
        pendingSettings.viewDist    = p.viewDist;
        pendingSettings.chunkWindow = p.chunkWindow;
        pendingSettings.particles   = p.particles;
        pendingSettings.resolution  = p.resolution;
        _pendingQualityPreset = a.value;
        _settingsDirtyCheck();
        return true;
      } else if (a.action === 'slider') {
        var pct = Math.max(0, Math.min(1, (mx - a.trackX) / a.trackW));
        var steps = Math.round(pct * (a.max - a.min) / a.step);
        pendingSettings[a.key] = a.min + steps * a.step;
        if (pendingSettings[a.key] > a.max) pendingSettings[a.key] = a.max;
        // Re-detect which preset (if any) the pending values match
        var presetNames = ['low', 'medium', 'high', 'ultra'];
        _pendingQualityPreset = 'custom';
        for (var pi = 0; pi < presetNames.length; pi++) {
          var pp = QUALITY_PRESETS[presetNames[pi]];
          if (pendingSettings.viewDist === pp.viewDist && pendingSettings.chunkWindow === pp.chunkWindow &&
              pendingSettings.particles === pp.particles && pendingSettings.resolution === pp.resolution) {
            _pendingQualityPreset = presetNames[pi]; break;
          }
        }
        _settingsDirtyCheck();
        return true;
      } else if (a.action === 'toggle') {
        pendingSettings[a.key] = !pendingSettings[a.key];
        _settingsDirtyCheck();
        return true;
      }
    }
  }
  return false;
}

function drawGameOver() {
  var w = canvas.width, h = canvas.height;
  ctx.save();

  // Dimmed background
  ctx.fillStyle = 'rgba(0,0,0,0.80)';
  ctx.fillRect(0, 0, w, h);

  // Layout constants — panel is sized to fit content, never by canvas height
  var titleH  = 46;  // baseline of title text from panel top
  var dividerY = titleH + 12;
  var statsTop = dividerY + 24;
  var statRows = ['Level reached:', 'Enemies defeated:', 'Damage taken:', 'Time survived:'];
  var statLH   = 26;
  var statsH   = statRows.length * statLH;
  var hintH    = 36;  // space for the "press start" line + bottom padding
  var padTop   = 16;  // extra top padding above title

  var panW = Math.min(300, w - 16);
  var panH = padTop + statsTop + statsH + hintH;
  var px = Math.floor((w - panW) / 2);
  var py = Math.floor((h - panH) / 2);

  // Panel background + border
  ctx.fillStyle = 'rgba(40,0,0,0.96)';
  ctx.strokeStyle = '#cc2222'; ctx.lineWidth = 2;
  ctx.fillRect(px, py, panW, panH);
  ctx.strokeRect(px, py, panW, panH);

  // Title
  ctx.textAlign = 'center'; ctx.fillStyle = '#ff3333';
  ctx.font = 'bold ' + Math.max(20, Math.min(28, Math.floor(panW * 0.09))) + 'px Arial';
  ctx.fillText('GAME OVER', px + panW / 2, py + padTop + titleH);

  // Divider
  ctx.strokeStyle = '#661111'; ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(px + 16, py + padTop + dividerY);
  ctx.lineTo(px + panW - 16, py + padTop + dividerY);
  ctx.stroke();

  // Stats — label on left, value right-aligned
  ctx.font = '13px Arial'; ctx.textAlign = 'left';
  var lx  = px + 18, rx = px + panW - 18;
  var ly  = py + padTop + statsTop;
  var statValues = [
    level.toString(),
    (stats.totalEnemiesKilled || 0).toString(),
    Math.floor(stats.totalDamageTaken || 0).toString(),
    timeSec.toFixed(1) + 's'
  ];
  for (var si = 0; si < statRows.length; si++) {
    ctx.fillStyle = '#ffaaaa'; ctx.textAlign = 'left';
    ctx.fillText(statRows[si], lx, ly);
    ctx.fillStyle = '#ffffff'; ctx.textAlign = 'right';
    ctx.fillText(statValues[si], rx, ly);
    ly += statLH;
  }

  // Restart hint — always sits in reserved space below stats
  ctx.textAlign = 'center'; ctx.font = '12px Arial'; ctx.fillStyle = '#ff7777';
  ctx.fillText('Press Start to play again', px + panW / 2, py + panH - 12);

  ctx.restore();
}
