// Magic Missile presentation: silver-blue aether held in a fine ivory spine.
// This is paint only. Projectile motion, homing, hit volumes and effect life
// remain owned by combat. No random numbers, gameplay writes or image cache.
// The short wake follows the current world velocity, not a screen-space angle
// or a frame-rate-dependent position history. It is not a replay of homing.
var MISSILE_ART_COLORS = typeof GAME_MATERIALS !== 'undefined' && GAME_MATERIALS.missileMagic ?
  GAME_MATERIALS.missileMagic.hex : Object.freeze({
    core:'#fff3d8', light:'#c4edf0', mid:'#79b9d0', deep:'#315677', rune:'#bfa16a'
  });
// RGB prefixes are prepared once, not reparsed for every moving wake segment.
function missileArtRGBPrefix(hex) {
  var rgb = parseInt(hex.slice(1),16);
  return 'rgba('+((rgb>>16)&255)+','+((rgb>>8)&255)+','+(rgb&255)+',';
}
var _missileArtMidRGBA = missileArtRGBPrefix(MISSILE_ART_COLORS.mid);
var _missileArtLightRGBA = missileArtRGBPrefix(MISSILE_ART_COLORS.light);

function missileArtClamp(value, low, high) {
  return Math.max(low, Math.min(high, value));
}

function missileArtProject(point, C) {
  var dx = point.x - cam.x, dy = point.y - cam.y;
  var fwd = dx * C.cosAng + dy * C.sinAng;
  if (!Number.isFinite(fwd) || fwd < 1 || !Number.isFinite(point.z)) return null;
  return {
    sx: (0.5 + (-dx * C.sinAng + dy * C.cosAng) / fwd * C.invTanHalf * 0.5) * C.w,
    sy: C.horizonY + (C.cameraZ - point.z) / fwd * projScale,
    fwd: fwd
  };
}

function missileArtDiamond(x, y, ux, uy, length, width) {
  var nx = -uy * width, ny = ux * width;
  ctx.beginPath();
  ctx.moveTo(x + ux * length, y + uy * length);
  ctx.lineTo(x + nx, y + ny);
  ctx.lineTo(x - ux * length * 0.72, y - uy * length * 0.72);
  ctx.lineTo(x - nx, y - ny);
  ctx.closePath(); ctx.fill();
}

// Each ribbon has its own near/far depth, and its callback paints only that
// ribbon. Never put this renderer inside another scene-depth clip.
function missileArtWakeSegment(a, b, C, widthA, widthB, alphaA, alphaB) {
  drawSpellWorldSegment(a, b, C, Math.max(widthA, widthB) * 2, 0, function(pa, pb) {
    var dx = pb.x - pa.x, dy = pb.y - pa.y, length = Math.hypot(dx, dy);
    if (length < 0.25) return;
    var nx = -dy / length, ny = dx / length;
    var grad = ctx.createLinearGradient(pa.x, pa.y, pb.x, pb.y);
    grad.addColorStop(0, _missileArtLightRGBA + alphaA + ')');
    grad.addColorStop(1, _missileArtLightRGBA + alphaB + ')');
    ctx.fillStyle = grad;
    ctx.beginPath();
    ctx.moveTo(pa.x + nx * widthA, pa.y + ny * widthA);
    ctx.lineTo(pb.x + nx * widthB, pb.y + ny * widthB);
    ctx.lineTo(pb.x - nx * widthB, pb.y - ny * widthB);
    ctx.lineTo(pa.x - nx * widthA, pa.y - ny * widthA);
    ctx.closePath(); ctx.fill();
    // A single hairline gives the wake a crafted, calligraphic edge. No blur
    // extends beyond the depth mask and no opaque rectangle enters the buffer.
    ctx.strokeStyle = grad;
    ctx.lineWidth = Math.max(0.5, resScale * 0.45);
    ctx.beginPath();
    ctx.moveTo(pa.x + nx * widthA * 0.35, pa.y + ny * widthA * 0.35);
    ctx.lineTo(pb.x + nx * widthB * 0.35, pb.y + ny * widthB * 0.35);
    ctx.stroke();
  });
}

// true means this presentation handled the missile, even if scenery hides all
// of it. false selects the unchanged legacy renderer for every other spell.
function renderMissileProjectileArt(p, C, now) {
  if (!p || !p.spell || p.spell.id !== 'missile') return false;
  if (!Number.isFinite(p.x) || !Number.isFinite(p.y) || !Number.isFinite(p.z) ||
      !Number.isFinite(p.ang) || !C) return true;
  if (Math.hypot(p.x - cam.x, p.y - cam.y) > 600) return true;
  var time = Number.isFinite(now) ? now : Date.now();
  var age = Number.isFinite(p.spawnMs) ? Math.max(0, time - p.spawnMs) : 0;
  var hz = Number.isFinite(p.hz) ? p.hz : Number.isFinite(p.speed) ? p.speed : 0;
  var vz = Number.isFinite(p.vz) ? p.vz : 0;
  var vx = Math.cos(p.ang) * hz, vy = Math.sin(p.ang) * hz;
  // The wake cannot reach behind the launch point during its first moments.
  // Three depth-aware spans describe 115 ms of travel, capped at 42 world units.
  var speed = Math.hypot(hz, vz), seconds = Math.min(0.115, age / 1000, 42 / Math.max(1, speed));
  var head = {x:p.x, y:p.y, z:p.z}, headView = missileArtProject(head, C);
  var RS = Math.max(0.1, Number.isFinite(resScale) ? resScale : 1);
  ctx.save();
  try {
    ctx.shadowBlur = 0; ctx.globalAlpha *= 0.96;
    ctx.lineCap = 'round'; ctx.lineJoin = 'round';
    for (var i = 0; i < 3; i++) {
      var from = (3 - i) / 3, to = (2 - i) / 3;
      var a = {x:p.x-vx*seconds*from, y:p.y-vy*seconds*from, z:p.z-vz*seconds*from};
      var b = {x:p.x-vx*seconds*to, y:p.y-vy*seconds*to, z:p.z-vz*seconds*to};
      var da = (a.x-cam.x)*C.cosAng+(a.y-cam.y)*C.sinAng;
      var db = (b.x-cam.x)*C.cosAng+(b.y-cam.y)*C.sinAng;
      var wa = missileArtClamp(1.4 * projScale / Math.max(1, da), RS * 0.35, RS * 7) * (1-from*0.88);
      var wb = missileArtClamp(1.4 * projScale / Math.max(1, db), RS * 0.35, RS * 7) * (1-to*0.88);
      if (seconds > 0) missileArtWakeSegment(a, b, C, wa, wb,
        Math.pow(1-from,1.3)*0.5, Math.pow(1-to,1.3)*0.5);
    }
    if (headView) {
      // Project a world-velocity step to obtain screen orientation. p.ang is a
      // world heading; using it as a Canvas angle breaks when the camera turns.
      var ahead = missileArtProject({x:p.x+vx*0.025,y:p.y+vy*0.025,z:p.z+vz*0.025}, C);
      var sx = ahead ? ahead.sx-headView.sx : 0, sy = ahead ? ahead.sy-headView.sy : 0;
      var motionLength = Math.hypot(sx, sy), ux = motionLength > 0.04 ? sx/motionLength : 0;
      var uy = motionLength > 0.04 ? sy/motionLength : -1;
      var radius = missileArtClamp(3.1 * projScale / headView.fwd, 1.7 * RS, 13 * RS);
      var elongation = 1.05 + Math.min(0.75, motionLength / Math.max(1, radius));
      var length = radius * elongation, width = radius * 0.48;
      drawSpellBillboard(headView, length + RS * 2, 0, function() {
        var x = headView.sx, y = headView.sy;
        // Facets, not a neon orb: a translucent outer blade, silver edge and
        // warm ivory center remain readable against stone and a bright sky.
        ctx.fillStyle = _missileArtMidRGBA + '0.18)';
        missileArtDiamond(x,y,ux,uy,length*1.05,width*1.65);
        ctx.fillStyle = MISSILE_ART_COLORS.deep;
        missileArtDiamond(x,y,ux,uy,length,width);
        ctx.fillStyle = MISSILE_ART_COLORS.light;
        missileArtDiamond(x-uy*width*0.1,y+ux*width*0.1,ux,uy,length*0.86,width*0.68);
        ctx.fillStyle = MISSILE_ART_COLORS.core;
        missileArtDiamond(x,y,ux,uy,length*0.64,Math.max(RS*0.55,width*0.25));
        // Two separated brackets repeat the casting-hand seal at a useful
        // distance. Far-away missiles retain just the clean luminous spindle.
        if (radius > RS * 3.2) {
          ctx.strokeStyle = _missileArtLightRGBA + '0.64)'; ctx.lineWidth = RS * 0.65;
          for (var side = -1; side <= 1; side += 2) {
            var nx = -uy * side, ny = ux * side;
            ctx.beginPath();
            ctx.moveTo(x-ux*radius*0.35+nx*radius*0.64,y-uy*radius*0.35+ny*radius*0.64);
            ctx.lineTo(x+nx*radius*0.82,y+ny*radius*0.82);
            ctx.lineTo(x+ux*radius*0.3+nx*radius*0.64,y+uy*radius*0.3+ny*radius*0.64);
            ctx.stroke();
          }
        }
      });
    }
  } finally { ctx.restore(); }
  return true;
}

function renderMissileImpactArt(im, C, now) {
  if (!im || im.spellId !== 'missile' || im.isCompanionProj) return false;
  if (!Number.isFinite(im.x) || !Number.isFinite(im.y) || !C) return true;
  var z = Number.isFinite(im.z) ? im.z : getEntityRenderFloorZ(im);
  var time = Number.isFinite(now) ? now : Date.now();
  var age = Math.max(0, time - im.spawnMs), life = im.lifeMs;
  if (!Number.isFinite(age) || !Number.isFinite(life) || life <= 0 || age >= life) return true;
  if (Math.hypot(im.x-cam.x, im.y-cam.y) > viewDist) return true;
  var point = missileArtProject({x:im.x,y:im.y,z:z}, C);
  if (!point) return true;
  var t = age / life, RS = Math.max(0.1, Number.isFinite(resScale) ? resScale : 1);
  var size = missileArtClamp(6.2 * projScale / point.fwd, RS * 4, RS * 24);
  var spread = 0.3 + 0.88 * (1-Math.pow(1-t,3)), radius = size * spread;
  var alpha = Math.pow(1-t, 1.05), angle = (im.spawnMs % 997) * 0.0017;
  drawSpellBillboard(point, size * 1.5 + RS * 2, 0, function() {
    ctx.save();
    try {
      ctx.shadowBlur = 0; ctx.globalAlpha *= alpha;
      ctx.lineCap = 'round'; ctx.lineJoin = 'round';
      // The compressed seal breaks into six angular strokes. Their staggered
      // lengths form a star fracture, then open out and disappear within the
      // existing impact lifetime. No rings expand through surrounding walls.
      for (var i=0; i<6; i++) {
        var a=angle+i*Math.PI/3, ux=Math.cos(a), uy=Math.sin(a);
        var outer=radius*(i%2 ? 0.76 : 1.14), inner=radius*(0.12+0.55*t);
        var bend=radius*0.12*(i%2 ? -1 : 1);
        var bx=point.sx+ux*(inner+outer)*0.5-uy*bend;
        var by=point.sy+uy*(inner+outer)*0.5+ux*bend;
        ctx.strokeStyle = i%2 ? MISSILE_ART_COLORS.mid : MISSILE_ART_COLORS.light;
        ctx.lineWidth = Math.max(RS*0.65,size*0.065*(1-t*0.45));
        ctx.beginPath(); ctx.moveTo(point.sx+ux*inner,point.sy+uy*inner);
        ctx.lineTo(bx,by); ctx.lineTo(point.sx+ux*outer,point.sy+uy*outer); ctx.stroke();
        // The short cross-stroke gives each separated fragment its rune-like
        // silhouette without filling the screen with independent particles.
        if (i%2===0 && t<0.78) {
          ctx.strokeStyle = MISSILE_ART_COLORS.core; ctx.lineWidth *= 0.7;
          ctx.beginPath(); ctx.moveTo(bx-uy*radius*0.1,by+ux*radius*0.1);
          ctx.lineTo(bx+uy*radius*0.09,by-ux*radius*0.09); ctx.stroke();
        }
      }
      if (t<0.5) {
        var flash=size*0.47*Math.pow(1-t*2,1.1);
        ctx.fillStyle=MISSILE_ART_COLORS.core;
        missileArtDiamond(point.sx,point.sy,Math.cos(angle),Math.sin(angle),flash,flash*0.32);
        ctx.fillStyle=_missileArtLightRGBA+'0.76)';
        missileArtDiamond(point.sx,point.sy,-Math.sin(angle),Math.cos(angle),flash*0.66,flash*0.17);
      }
    } finally { ctx.restore(); }
  });
  return true;
}
