// =============================================
// SECTION 14: RENDERING - ENTITIES
// =============================================

function drawGoalMarker3D() {
  if (!goal) return;
  var C = getCam3D();
  var w = C.w, h = C.h;
  var gx = goal.x + goal.w / 2, gy = goal.y + goal.h / 2;
  var vis = entityVisible3D(gx, gy, getEntityGroundRenderZ(gx, gy, false), C,
    { maxDist: viewDist, sceneDepth: true, fadeFraction: 1 });
  if (!vis) return;
  var screenX = vis.sx, fwd = vis.fwd, floorY = vis.sy;
  var s3g = getScale3D('smStructure');
  var size = Math.max(6, Math.min(28, Math.floor(240 * s3g / fwd)));
  var flagTop = floorY - Math.floor(size / 4);
  var flagHeight = Math.max(0, Math.min(40, Math.floor(280 * s3g / fwd)));
  withSceneDepthBillboard({x: screenX - size / 2 - 1, y: flagTop - flagHeight - 1,
    width: size + 2, height: flagHeight + Math.floor(size / 3) + 2}, fwd, function () {
  ctx.save(); ctx.globalAlpha = 0.95; ctx.fillStyle = '#00ff00';
  ctx.strokeStyle = 'rgba(0,255,0,0.7)'; ctx.lineWidth = 2;
  ctx.fillRect(screenX - size / 2, floorY - Math.floor(size / 4), size, Math.floor(size / 3));
  ctx.beginPath(); ctx.moveTo(screenX, floorY - Math.floor(size / 4));
  ctx.lineTo(screenX, flagTop - flagHeight);
  ctx.stroke(); ctx.restore();
  });
}

function drawSkeletonFrame(oc, typeId, frame, crumble, dir) {
  var w = SKEL_W, h = SKEL_H;
  var c = oc.getContext('2d');
  var phase = (frame / SKEL_FRAMES) * Math.PI * 2;
  var isFast = typeId === 'fast', isTank = typeId === 'tank';
  c.clearRect(0, 0, w, h);

  var bone  = '#cfc8b8';
  var dark  = '#8a7e6c';
  var black = '#111111';
  var lw    = isTank ? 2.5 : (isFast ? 1.5 : 2.0);
  c.strokeStyle = dark; c.lineWidth = lw; c.lineCap = 'round'; c.lineJoin = 'round';

  var cx     = w / 2;
  var headR  = isTank ? 9  : (isFast ? 7  : 8);
  var shldrW = isTank ? 17 : (isFast ? 12 : 14);
  var hipHW  = isTank ? 12 : (isFast ? 8  : 10);
  var ribH   = isTank ? 22 : 18;
  var nRibs  = isTank ? 5  : 4;
  var uLeg = 14, lLeg = 14, uArm = 11, lArm = 11;
  var jointR = lw * 0.9;

  var swing     = isFast ? 0.38 : (isTank ? 0.24 : 0.32);
  var leftLegA  =  Math.sin(phase) * swing;
  var rightLegA = -Math.sin(phase) * swing;
  var kneeB     = -Math.abs(Math.sin(phase)) * 0.28 - 0.12;
  var leftArmA  = -Math.sin(phase) * swing * 0.55 - 0.18;
  var rightArmA =  Math.sin(phase) * swing * 0.55 + 0.18;
  var bob       =  Math.abs(Math.sin(phase * 2)) * 1.5;

  var headY   = 10 + bob;
  var neckY   = headY + headR;
  var shldrY  = neckY + 3;
  var pelvisY = shldrY + ribH + 2;
  var hipY    = pelvisY + 3;

  // Draw a two-segment limb; returns the end point
  function limb(fromX, fromY, ang, s1, bend, s2) {
    var kx = fromX + Math.sin(ang) * s1, ky = fromY + Math.cos(ang) * s1;
    c.beginPath(); c.moveTo(fromX, fromY); c.lineTo(kx, ky); c.stroke();
    var a2 = ang + bend;
    var ex = kx + Math.sin(a2) * s2, ey = ky + Math.cos(a2) * s2;
    c.beginPath(); c.moveTo(kx, ky); c.lineTo(ex, ey); c.stroke();
    c.fillStyle = bone;
    c.beginPath(); c.arc(kx, ky, jointR, 0, Math.PI * 2); c.fill();
    return {x: ex, y: ey};
  }

  // ─── Direction-dependent drawing ───
  if (!dir || dir === 0) {
    // === FRONT VIEW (toward camera) ===
    var lhX = cx - hipHW * 0.6, rhX = cx + hipHW * 0.6;
    var lf = limb(lhX, hipY, leftLegA, uLeg, kneeB, lLeg);
    var rf = limb(rhX, hipY, rightLegA, uLeg, kneeB, lLeg);
    c.beginPath(); c.moveTo(lf.x, lf.y); c.lineTo(lf.x - Math.sin(leftLegA) * 4, lf.y + 1); c.stroke();
    c.beginPath(); c.moveTo(rf.x, rf.y); c.lineTo(rf.x - Math.sin(rightLegA) * 4, rf.y + 1); c.stroke();
    c.beginPath(); c.moveTo(cx, neckY); c.lineTo(cx, pelvisY); c.stroke();
    c.beginPath(); c.ellipse(cx, pelvisY, hipHW * 0.85, 4, 0, 0, Math.PI * 2); c.stroke();
    for (var r = 0; r < nRibs; r++) {
      var ribY = shldrY + 3 + r * (ribH - 5) / nRibs;
      var rw = shldrW * (1.0 - r * 0.07);
      c.beginPath(); c.moveTo(cx, ribY);
        c.quadraticCurveTo(cx - rw * 0.55, ribY - 1, cx - rw, ribY + 4); c.stroke();
      c.beginPath(); c.moveTo(cx, ribY);
        c.quadraticCurveTo(cx + rw * 0.55, ribY - 1, cx + rw, ribY + 4); c.stroke();
    }
    var lsX = cx - shldrW, rsX = cx + shldrW;
    c.beginPath(); c.moveTo(lsX, shldrY); c.lineTo(rsX, shldrY); c.stroke();
    limb(lsX, shldrY, leftArmA, uArm, 0.35, lArm);
    if (!crumble) limb(rsX, shldrY, rightArmA, uArm, -0.35, lArm);
    c.fillStyle = bone; c.strokeStyle = dark;
    c.beginPath(); c.arc(cx, headY, headR, 0, Math.PI * 2); c.fill(); c.stroke();
    if (crumble) {
      c.strokeStyle = dark; c.lineWidth = 1;
      c.beginPath(); c.moveTo(cx - 3, headY - headR * 0.5); c.lineTo(cx + 1, headY + headR * 0.1); c.lineTo(cx + 4, headY - headR * 0.2); c.stroke();
      c.lineWidth = lw; c.strokeStyle = dark;
    }
    c.beginPath(); c.ellipse(cx, headY + headR * 0.15, headR * 1.05, headR * 0.55, 0, 0, Math.PI); c.stroke();
    var eyeR = headR * 0.32, eyeOX = headR * 0.38;
    c.fillStyle = black;
    c.beginPath(); c.ellipse(cx - eyeOX, headY - 1, eyeR * 0.85, eyeR, 0, 0, Math.PI * 2); c.fill();
    c.beginPath(); c.ellipse(cx + eyeOX, headY - 1, eyeR * 0.85, eyeR, 0, 0, Math.PI * 2); c.fill();
    c.fillStyle = black;
    c.beginPath();
      c.moveTo(cx, headY + headR * 0.18);
      c.lineTo(cx - 1.5, headY + headR * 0.38);
      c.lineTo(cx + 1.5, headY + headR * 0.38);
    c.closePath(); c.fill();
    if (!crumble) {
      var jawTop = headY + headR * 0.45;
      var nTeeth = isFast ? 3 : (isTank ? 5 : 4);
      var tSpan = headR * 1.1;
      c.fillStyle = bone;
      for (var t = 0; t < nTeeth; t++) {
        var tx = cx - tSpan / 2 + (t + 0.5) * (tSpan / nTeeth);
        c.fillRect(tx - 1.5, jawTop, 3, 4);
        c.strokeStyle = dark; c.lineWidth = 0.5;
        c.strokeRect(tx - 1.5, jawTop, 3, 4);
        c.lineWidth = lw; c.strokeStyle = dark;
      }
    }

  } else if (dir === 2) {
    // === RIGHT PROFILE ===
    var bodyW2 = shldrW * 0.35;
    // Far leg (behind body)
    limb(cx, hipY, rightLegA, uLeg, kneeB, lLeg);
    // Spine
    c.beginPath(); c.moveTo(cx, neckY); c.lineTo(cx, pelvisY); c.stroke();
    // Vertebra marks
    for (var v2 = 0; v2 < 4; v2++) {
      var vy2 = neckY + (pelvisY - neckY) * (v2 + 0.5) / 4;
      c.fillStyle = dark;
      c.beginPath(); c.arc(cx - 1, vy2, 1.2, 0, Math.PI * 2); c.fill();
    }
    // Pelvis (narrow from side)
    c.beginPath(); c.ellipse(cx, pelvisY, 4, hipHW * 0.4, 0, 0, Math.PI * 2); c.stroke();
    // Ribs as stubs from spine
    for (var r2 = 0; r2 < nRibs; r2++) {
      var ribY2 = shldrY + 3 + r2 * (ribH - 5) / nRibs;
      var ribLen2 = bodyW2 * (1.0 - r2 * 0.12);
      c.beginPath(); c.moveTo(cx, ribY2);
      c.quadraticCurveTo(cx + ribLen2 * 0.6, ribY2 - 1.5, cx + ribLen2, ribY2 + 3); c.stroke();
      c.beginPath(); c.moveTo(cx, ribY2);
      c.quadraticCurveTo(cx - ribLen2 * 0.3, ribY2 - 1, cx - ribLen2 * 0.5, ribY2 + 2); c.stroke();
    }
    // Shoulder joint
    c.fillStyle = bone;
    c.beginPath(); c.arc(cx, shldrY, jointR * 1.3, 0, Math.PI * 2); c.fill(); c.stroke();
    // Far arm (behind body)
    limb(cx - 2, shldrY, rightArmA * 0.7, uArm * 0.8, -0.3, lArm * 0.8);
    // Near arm (missing in crumble)
    if (!crumble) limb(cx + 1, shldrY, leftArmA, uArm, 0.35, lArm);
    // Near leg (in front)
    var nearLf2 = limb(cx, hipY, leftLegA, uLeg, kneeB, lLeg);
    c.beginPath(); c.moveTo(nearLf2.x, nearLf2.y); c.lineTo(nearLf2.x + 4, nearLf2.y + 1); c.stroke();
    // Skull from side — elongated oval
    c.fillStyle = bone; c.strokeStyle = dark;
    c.beginPath(); c.ellipse(cx + 2, headY, headR * 1.15, headR * 0.9, 0, 0, Math.PI * 2); c.fill(); c.stroke();
    if (crumble) {
      c.strokeStyle = dark; c.lineWidth = 1;
      c.beginPath(); c.moveTo(cx, headY - headR * 0.5); c.lineTo(cx + 3, headY + headR * 0.1); c.stroke();
      c.lineWidth = lw; c.strokeStyle = dark;
    }
    // Single eye socket
    c.fillStyle = black;
    c.beginPath(); c.ellipse(cx + headR * 0.5, headY - headR * 0.05, headR * 0.35, headR * 0.3, 0, 0, Math.PI * 2); c.fill();
    // Nasal cavity (front edge)
    c.beginPath();
    c.moveTo(cx + headR * 0.95, headY + headR * 0.1);
    c.lineTo(cx + headR * 0.8, headY + headR * 0.35);
    c.lineTo(cx + headR * 1.05, headY + headR * 0.35);
    c.closePath(); c.fill();
    // Jaw arc + teeth from side
    if (!crumble) {
      c.strokeStyle = dark; c.lineWidth = lw;
      c.beginPath();
      c.moveTo(cx - headR * 0.3, headY + headR * 0.5);
      c.quadraticCurveTo(cx + headR * 0.5, headY + headR * 1.1, cx + headR * 0.9, headY + headR * 0.45);
      c.stroke();
      var nTS = isFast ? 2 : (isTank ? 4 : 3);
      c.fillStyle = bone;
      for (var ts = 0; ts < nTS; ts++) {
        var tF = (ts + 0.5) / nTS;
        var ttx = cx + headR * (-0.1 + tF * 0.9);
        var tty = headY + headR * 0.5 + tF * headR * 0.1;
        c.fillRect(ttx - 1, tty, 2.5, 3.5);
        c.strokeStyle = dark; c.lineWidth = 0.5;
        c.strokeRect(ttx - 1, tty, 2.5, 3.5);
        c.lineWidth = lw; c.strokeStyle = dark;
      }
    }

  } else if (dir === 4) {
    // === BACK VIEW (away from camera) ===
    var lhX4 = cx - hipHW * 0.6, rhX4 = cx + hipHW * 0.6;
    limb(lhX4, hipY, leftLegA, uLeg, kneeB, lLeg);
    limb(rhX4, hipY, rightLegA, uLeg, kneeB, lLeg);
    // Spine with vertebra bumps
    c.beginPath(); c.moveTo(cx, neckY); c.lineTo(cx, pelvisY); c.stroke();
    c.fillStyle = bone;
    for (var v4 = 0; v4 < 5; v4++) {
      var vy4 = neckY + 2 + (pelvisY - neckY - 4) * v4 / 4;
      c.beginPath(); c.arc(cx, vy4, 1.5, 0, Math.PI * 2); c.fill();
      c.strokeStyle = dark; c.beginPath(); c.arc(cx, vy4, 1.5, 0, Math.PI * 2); c.stroke();
    }
    // Pelvis
    c.beginPath(); c.ellipse(cx, pelvisY, hipHW * 0.85, 4, 0, 0, Math.PI * 2); c.stroke();
    // Ribs from behind (reversed curve)
    for (var r4 = 0; r4 < nRibs; r4++) {
      var ribY4 = shldrY + 3 + r4 * (ribH - 5) / nRibs;
      var rw4 = shldrW * (0.85 - r4 * 0.06);
      c.beginPath(); c.moveTo(cx, ribY4);
        c.quadraticCurveTo(cx - rw4 * 0.5, ribY4 + 2, cx - rw4, ribY4 + 5); c.stroke();
      c.beginPath(); c.moveTo(cx, ribY4);
        c.quadraticCurveTo(cx + rw4 * 0.5, ribY4 + 2, cx + rw4, ribY4 + 5); c.stroke();
    }
    // Scapulae (shoulder blades)
    c.strokeStyle = dark; c.lineWidth = lw * 0.8;
    c.beginPath(); c.moveTo(cx - 3, shldrY + 2); c.lineTo(cx - shldrW * 0.6, shldrY + 5);
    c.lineTo(cx - 4, shldrY + ribH * 0.55); c.closePath(); c.stroke();
    c.beginPath(); c.moveTo(cx + 3, shldrY + 2); c.lineTo(cx + shldrW * 0.6, shldrY + 5);
    c.lineTo(cx + 4, shldrY + ribH * 0.55); c.closePath(); c.stroke();
    c.lineWidth = lw;
    // Shoulder bar + arms
    var lsX4 = cx - shldrW, rsX4 = cx + shldrW;
    c.beginPath(); c.moveTo(lsX4, shldrY); c.lineTo(rsX4, shldrY); c.stroke();
    limb(lsX4, shldrY, leftArmA, uArm, 0.35, lArm);
    if (!crumble) limb(rsX4, shldrY, rightArmA, uArm, -0.35, lArm);
    // Skull dome (back of head — no face)
    c.fillStyle = bone; c.strokeStyle = dark;
    c.beginPath(); c.arc(cx, headY, headR, 0, Math.PI * 2); c.fill(); c.stroke();
    if (crumble) {
      c.strokeStyle = dark; c.lineWidth = 1;
      c.beginPath(); c.moveTo(cx - 3, headY - headR * 0.4); c.lineTo(cx + 2, headY + headR * 0.2); c.stroke();
      c.lineWidth = lw; c.strokeStyle = dark;
    }
    // Skull plate lines
    c.strokeStyle = dark; c.lineWidth = 0.8;
    c.beginPath(); c.moveTo(cx, headY - headR * 0.8); c.lineTo(cx, headY + headR * 0.3); c.stroke();
    c.beginPath();
    c.moveTo(cx - headR * 0.6, headY);
    c.quadraticCurveTo(cx, headY - headR * 0.2, cx + headR * 0.6, headY);
    c.stroke();
    c.lineWidth = lw;

  } else if (dir === 1) {
    // === 3/4 FRONT VIEW ===
    var offX1 = 2;
    var lhX1 = cx - hipHW * 0.5 + offX1, rhX1 = cx + hipHW * 0.7 + offX1;
    var lf1 = limb(lhX1, hipY, leftLegA, uLeg, kneeB, lLeg);
    var rf1 = limb(rhX1, hipY, rightLegA, uLeg, kneeB, lLeg);
    c.beginPath(); c.moveTo(lf1.x, lf1.y); c.lineTo(lf1.x - 3, lf1.y + 1); c.stroke();
    c.beginPath(); c.moveTo(rf1.x, rf1.y); c.lineTo(rf1.x + 3, rf1.y + 1); c.stroke();
    c.beginPath(); c.moveTo(cx + offX1, neckY); c.lineTo(cx + offX1, pelvisY); c.stroke();
    c.beginPath(); c.ellipse(cx + offX1, pelvisY, hipHW * 0.8, 4, 0.15, 0, Math.PI * 2); c.stroke();
    // Ribs — near side wider, far side narrower
    for (var r1 = 0; r1 < nRibs; r1++) {
      var ribY1 = shldrY + 3 + r1 * (ribH - 5) / nRibs;
      var rwN1 = shldrW * (1.05 - r1 * 0.07);
      var rwF1 = shldrW * (0.7 - r1 * 0.05);
      c.beginPath(); c.moveTo(cx + offX1, ribY1);
        c.quadraticCurveTo(cx + offX1 - rwF1 * 0.5, ribY1 - 1, cx + offX1 - rwF1, ribY1 + 4); c.stroke();
      c.beginPath(); c.moveTo(cx + offX1, ribY1);
        c.quadraticCurveTo(cx + offX1 + rwN1 * 0.55, ribY1 - 1, cx + offX1 + rwN1, ribY1 + 4); c.stroke();
    }
    var lsX1 = cx + offX1 - shldrW * 0.75, rsX1 = cx + offX1 + shldrW;
    c.beginPath(); c.moveTo(lsX1, shldrY); c.lineTo(rsX1, shldrY); c.stroke();
    limb(lsX1, shldrY, leftArmA, uArm * 0.85, 0.3, lArm * 0.85);
    if (!crumble) limb(rsX1, shldrY, rightArmA, uArm, -0.35, lArm);
    // Skull — shifted for perspective
    c.fillStyle = bone; c.strokeStyle = dark;
    c.beginPath(); c.ellipse(cx + offX1 + 1, headY, headR * 1.05, headR, 0, 0, Math.PI * 2); c.fill(); c.stroke();
    if (crumble) {
      c.strokeStyle = dark; c.lineWidth = 1;
      c.beginPath(); c.moveTo(cx + offX1 - 2, headY - headR * 0.5); c.lineTo(cx + offX1 + 2, headY + headR * 0.1); c.stroke();
      c.lineWidth = lw; c.strokeStyle = dark;
    }
    c.beginPath(); c.ellipse(cx + offX1 + 1, headY + headR * 0.15, headR * 1.0, headR * 0.55, 0, 0, Math.PI); c.stroke();
    // Eyes — far eye smaller
    var eyeR1 = headR * 0.32, eyeOX1 = headR * 0.35;
    c.fillStyle = black;
    c.beginPath(); c.ellipse(cx + offX1 - eyeOX1 * 0.6, headY - 1, eyeR1 * 0.55, eyeR1 * 0.75, 0, 0, Math.PI * 2); c.fill();
    c.beginPath(); c.ellipse(cx + offX1 + eyeOX1 * 1.1, headY - 1, eyeR1 * 0.85, eyeR1, 0, 0, Math.PI * 2); c.fill();
    c.beginPath();
      c.moveTo(cx + offX1 + 1, headY + headR * 0.18);
      c.lineTo(cx + offX1 - 0.5, headY + headR * 0.38);
      c.lineTo(cx + offX1 + 2.5, headY + headR * 0.38);
    c.closePath(); c.fill();
    if (!crumble) {
      var jawTop1 = headY + headR * 0.45;
      var nTeeth1 = isFast ? 3 : (isTank ? 5 : 4);
      var tSpan1 = headR * 1.0;
      c.fillStyle = bone;
      for (var t1 = 0; t1 < nTeeth1; t1++) {
        var tx1 = cx + offX1 - tSpan1 * 0.3 + (t1 + 0.5) * (tSpan1 / nTeeth1);
        c.fillRect(tx1 - 1.5, jawTop1, 3, 4);
        c.strokeStyle = dark; c.lineWidth = 0.5;
        c.strokeRect(tx1 - 1.5, jawTop1, 3, 4);
        c.lineWidth = lw; c.strokeStyle = dark;
      }
    }

  } else if (dir === 3) {
    // === 3/4 BACK VIEW ===
    var offX3 = 2;
    var lhX3 = cx - hipHW * 0.5 + offX3, rhX3 = cx + hipHW * 0.7 + offX3;
    limb(lhX3, hipY, leftLegA, uLeg, kneeB, lLeg);
    limb(rhX3, hipY, rightLegA, uLeg, kneeB, lLeg);
    // Spine with vertebra bumps
    c.beginPath(); c.moveTo(cx + offX3, neckY); c.lineTo(cx + offX3, pelvisY); c.stroke();
    c.fillStyle = bone;
    for (var v3 = 0; v3 < 4; v3++) {
      var vy3 = neckY + 2 + (pelvisY - neckY - 4) * v3 / 3;
      c.beginPath(); c.arc(cx + offX3, vy3, 1.2, 0, Math.PI * 2); c.fill();
    }
    c.beginPath(); c.ellipse(cx + offX3, pelvisY, hipHW * 0.8, 4, 0.15, 0, Math.PI * 2); c.stroke();
    // Ribs from behind (asymmetric)
    for (var r3 = 0; r3 < nRibs; r3++) {
      var ribY3 = shldrY + 3 + r3 * (ribH - 5) / nRibs;
      var rwN3 = shldrW * (0.9 - r3 * 0.06);
      var rwF3 = shldrW * (0.65 - r3 * 0.05);
      c.beginPath(); c.moveTo(cx + offX3, ribY3);
        c.quadraticCurveTo(cx + offX3 - rwF3 * 0.5, ribY3 + 1.5, cx + offX3 - rwF3, ribY3 + 5); c.stroke();
      c.beginPath(); c.moveTo(cx + offX3, ribY3);
        c.quadraticCurveTo(cx + offX3 + rwN3 * 0.5, ribY3 + 1.5, cx + offX3 + rwN3, ribY3 + 5); c.stroke();
    }
    // Scapulae
    c.strokeStyle = dark; c.lineWidth = lw * 0.8;
    c.beginPath(); c.moveTo(cx + offX3 + 3, shldrY + 2); c.lineTo(cx + offX3 + shldrW * 0.6, shldrY + 5);
    c.lineTo(cx + offX3 + 4, shldrY + ribH * 0.5); c.closePath(); c.stroke();
    c.beginPath(); c.moveTo(cx + offX3 - 2, shldrY + 3); c.lineTo(cx + offX3 - shldrW * 0.4, shldrY + 6);
    c.lineTo(cx + offX3 - 3, shldrY + ribH * 0.4); c.closePath(); c.stroke();
    c.lineWidth = lw;
    // Shoulder bar + arms
    var lsX3 = cx + offX3 - shldrW * 0.75, rsX3 = cx + offX3 + shldrW;
    c.beginPath(); c.moveTo(lsX3, shldrY); c.lineTo(rsX3, shldrY); c.stroke();
    limb(lsX3, shldrY, leftArmA, uArm * 0.85, 0.3, lArm * 0.85);
    if (!crumble) limb(rsX3, shldrY, rightArmA, uArm, -0.35, lArm);
    // Skull (back of head with partial cheekbone)
    c.fillStyle = bone; c.strokeStyle = dark;
    c.beginPath(); c.ellipse(cx + offX3 + 1, headY, headR * 1.05, headR, 0, 0, Math.PI * 2); c.fill(); c.stroke();
    if (crumble) {
      c.strokeStyle = dark; c.lineWidth = 1;
      c.beginPath(); c.moveTo(cx + offX3, headY - headR * 0.4); c.lineTo(cx + offX3 + 3, headY + headR * 0.2); c.stroke();
      c.lineWidth = lw; c.strokeStyle = dark;
    }
    // Skull plate line
    c.strokeStyle = dark; c.lineWidth = 0.8;
    c.beginPath(); c.moveTo(cx + offX3 + 1, headY - headR * 0.7); c.lineTo(cx + offX3 + 1, headY + headR * 0.3); c.stroke();
    c.lineWidth = lw;
    // Partial cheekbone edge (near side)
    c.beginPath();
    c.arc(cx + offX3 + headR * 0.3, headY + headR * 0.2, headR * 0.7, 0.3, Math.PI * 0.7);
    c.stroke();
    // Ear holes
    c.fillStyle = dark;
    c.beginPath(); c.arc(cx + offX3 + headR * 0.95, headY + headR * 0.05, 1.5, 0, Math.PI * 2); c.fill();
  }
}

function buildSkeletonSprites() {
  skeletonFrames = {};
  var types = ['fast', 'normal', 'tank'];
  var t0 = Date.now();
  var totalCanvases = 0;
  for (var ti = 0; ti < types.length; ti++) {
    var id = types[ti];
    skeletonFrames[id] = [];
    for (var d = 0; d < SPRITE_DIRS; d++) {
      skeletonFrames[id][d] = [];
      var srcDir = MIRROR_DIR[d];
      var flip = MIRROR_FLIP[d];
      for (var f = 0; f < SKEL_FRAMES; f++) {
        var oc = document.createElement('canvas');
        oc.width = SKEL_W; oc.height = SKEL_H;
        if (!flip) {
          drawSkeletonFrame(oc, id, f, false, srcDir);
        } else {
          // Mirror: draw source dir then flip
          var tmp = document.createElement('canvas');
          tmp.width = SKEL_W; tmp.height = SKEL_H;
          drawSkeletonFrame(tmp, id, f, false, srcDir);
          var mc = oc.getContext('2d');
          mc.translate(SKEL_W, 0); mc.scale(-1, 1);
          mc.drawImage(tmp, 0, 0);
        }
        skeletonFrames[id][d].push(oc);
        totalCanvases++;
      }
    }
  }
  // Crumble variants (< 25% HP): cracked skull, missing jaw, one arm gone
  for (var ci = 0; ci < types.length; ci++) {
    var cid = types[ci] + '_crumble';
    skeletonFrames[cid] = [];
    for (var d2 = 0; d2 < SPRITE_DIRS; d2++) {
      skeletonFrames[cid][d2] = [];
      var srcDir2 = MIRROR_DIR[d2];
      var flip2 = MIRROR_FLIP[d2];
      for (var cf = 0; cf < SKEL_FRAMES; cf++) {
        var coc = document.createElement('canvas');
        coc.width = SKEL_W; coc.height = SKEL_H;
        if (!flip2) {
          drawSkeletonFrame(coc, types[ci], cf, true, srcDir2);
        } else {
          var ctmp = document.createElement('canvas');
          ctmp.width = SKEL_W; ctmp.height = SKEL_H;
          drawSkeletonFrame(ctmp, types[ci], cf, true, srcDir2);
          var cc = coc.getContext('2d');
          cc.translate(SKEL_W, 0); cc.scale(-1, 1);
          cc.drawImage(ctmp, 0, 0);
        }
        skeletonFrames[cid][d2].push(coc);
        totalCanvases++;
      }
    }
  }
  console.log('[SKEL] buildSkeletonSprites complete — ' + totalCanvases + ' canvases in ' + (Date.now() - t0) + 'ms');
}

// ── Wolf sprite ──────────────────────────────────────────────────────────────
// 8-directional quadruped. Joint-based approach matching skeleton methodology.
// Canvas is WOLF_W × WOLF_H (80×52). Diagonal gait: front/back counter-swing.
function drawWolfFrame(oc, frame, dir) {
  var c = oc.getContext('2d');
  var W = WOLF_W, H = WOLF_H;
  var phase = (frame / WOLF_FRAMES) * Math.PI * 2;
  c.clearRect(0, 0, W, H);

  // Colors
  var fur      = '#7a7a88';
  var furDark  = '#484850';
  var furLight = '#b0b0bc';
  var belly    = '#9a9aaa';
  var eyeCol   = '#f5c518';
  var noseCol  = '#1a1a1a';
  var earInner = '#b07070';

  // Line weights and joint size
  var lw = 2.5;
  var jointR = lw * 0.85;

  // Animation drives — STANDARDIZED across ALL directions (matches skeleton 0.32)
  var swing   = 0.32;
  var frontLA =  Math.sin(phase) * swing;
  var frontRA = -Math.sin(phase) * swing;
  var backLA  = -Math.sin(phase) * swing;
  var backRA  =  Math.sin(phase) * swing;
  var kneeB   = -Math.abs(Math.sin(phase)) * 0.25 - 0.10;
  var hockB   =  Math.abs(Math.sin(phase)) * 0.20 + 0.15;
  var bob     =  Math.abs(Math.sin(phase * 2)) * 1.5;
  var wag     =  Math.sin(phase * 2) * 5;

  // 3-segment digitigrade leg with VISIBLE JOINT CIRCLES
  function wolfLeg(fromX, fromY, ang, thighL, shinL, metaL, kB, hB, col, lw2) {
    var kx = fromX + Math.sin(ang) * thighL;
    var ky = fromY + Math.cos(ang) * thighL;
    var a2 = ang + kB;
    var hx = kx + Math.sin(a2) * shinL;
    var hy = ky + Math.cos(a2) * shinL;
    var a3 = a2 + hB;
    var px = hx + Math.sin(a3) * metaL;
    var py = hy + Math.cos(a3) * metaL;
    c.strokeStyle = col; c.lineWidth = lw2; c.lineCap = 'round'; c.lineJoin = 'round';
    c.beginPath(); c.moveTo(fromX, fromY); c.lineTo(kx, ky); c.stroke();
    c.beginPath(); c.moveTo(kx, ky); c.lineTo(hx, hy); c.stroke();
    c.beginPath(); c.moveTo(hx, hy); c.lineTo(px, py); c.stroke();
    c.fillStyle = col;
    c.beginPath(); c.arc(kx, ky, jointR, 0, Math.PI * 2); c.fill();
    c.beginPath(); c.arc(hx, hy, jointR, 0, Math.PI * 2); c.fill();
    return {x: px, y: py};
  }

  // Ear helper: pointed triangle with optional inner color
  function ear(x1, y1, tipX, tipY, x2, y2, showInner) {
    c.fillStyle = fur;
    c.beginPath(); c.moveTo(x1, y1); c.lineTo(tipX, tipY); c.lineTo(x2, y2); c.closePath(); c.fill();
    if (showInner) {
      c.fillStyle = earInner;
      var ix1 = x1 * 0.7 + tipX * 0.3, iy1 = y1 * 0.7 + tipY * 0.3;
      var ix2 = x2 * 0.7 + tipX * 0.3, iy2 = y2 * 0.7 + tipY * 0.3;
      var itx = x1 * 0.15 + tipX * 0.7 + x2 * 0.15, ity = y1 * 0.15 + tipY * 0.7 + y2 * 0.15;
      c.beginPath(); c.moveTo(ix1, iy1); c.lineTo(itx, ity); c.lineTo(ix2, iy2); c.closePath(); c.fill();
    }
  }

  if (!dir || dir === 0) {
    // === FRONT VIEW ===
    var fcx = W / 2, fcy = H * 0.42 + bob;
    var chW = W * 0.21, chH = H * 0.13;
    var legY0 = fcy + chH * 0.7;
    // Far legs (back pair, behind body)
    wolfLeg(fcx - 10, legY0 + 2, backLA * 0.7, 5, 5, 4, kneeB, hockB, furDark, lw * 0.85);
    wolfLeg(fcx + 10, legY0 + 2, backRA * 0.7, 5, 5, 4, kneeB, hockB, furDark, lw * 0.85);
    // Body (chest facing camera)
    c.fillStyle = fur;
    c.beginPath(); c.ellipse(fcx, fcy + chH * 0.2, chW, chH, 0, 0, Math.PI * 2); c.fill();
    c.fillStyle = furDark;
    c.beginPath(); c.ellipse(fcx, fcy - chH * 0.2, chW * 0.75, chH * 0.4, 0, Math.PI, Math.PI * 2); c.fill();
    c.fillStyle = belly;
    c.beginPath(); c.ellipse(fcx, fcy + chH * 0.5, chW * 0.45, chH * 0.35, 0, 0, Math.PI * 2); c.fill();
    // Near legs (front pair, over body)
    wolfLeg(fcx - 12, legY0, frontLA, 5, 5, 4, kneeB, hockB, fur, lw);
    wolfLeg(fcx + 12, legY0, frontRA, 5, 5, 4, kneeB, hockB, furLight, lw);
    // Neck + head
    var fHY = fcy - chH * 0.6, fHR = H * 0.16;
    c.fillStyle = fur;
    c.beginPath();
    c.moveTo(fcx - chW * 0.45, fcy - chH * 0.1);
    c.quadraticCurveTo(fcx - fHR * 0.7, fHY + fHR * 0.5, fcx - fHR * 0.5, fHY);
    c.lineTo(fcx + fHR * 0.5, fHY);
    c.quadraticCurveTo(fcx + fHR * 0.7, fHY + fHR * 0.5, fcx + chW * 0.45, fcy - chH * 0.1);
    c.fill();
    c.beginPath(); c.arc(fcx, fHY, fHR, 0, Math.PI * 2); c.fill();
    c.fillStyle = furDark;
    c.beginPath(); c.ellipse(fcx, fHY - fHR * 0.3, fHR * 0.8, fHR * 0.3, 0, Math.PI, Math.PI * 2); c.fill();
    c.fillStyle = furLight;
    c.beginPath(); c.ellipse(fcx, fHY + fHR * 0.4, fHR * 0.4, fHR * 0.3, 0, 0, Math.PI * 2); c.fill();
    // Eyes
    var fEO = fHR * 0.38;
    c.fillStyle = eyeCol;
    c.beginPath(); c.ellipse(fcx - fEO, fHY - fHR * 0.05, fHR * 0.15, fHR * 0.11, 0, 0, Math.PI * 2); c.fill();
    c.beginPath(); c.ellipse(fcx + fEO, fHY - fHR * 0.05, fHR * 0.15, fHR * 0.11, 0, 0, Math.PI * 2); c.fill();
    c.fillStyle = noseCol;
    c.beginPath(); c.arc(fcx - fEO, fHY - fHR * 0.05, fHR * 0.06, 0, Math.PI * 2); c.fill();
    c.beginPath(); c.arc(fcx + fEO, fHY - fHR * 0.05, fHR * 0.06, 0, Math.PI * 2); c.fill();
    // Nose
    c.fillStyle = noseCol;
    c.beginPath(); c.ellipse(fcx, fHY + fHR * 0.32, fHR * 0.14, fHR * 0.09, 0, 0, Math.PI * 2); c.fill();
    // Ears
    ear(fcx - fHR * 0.5, fHY - fHR * 0.5, fcx - fHR * 0.6, fHY - fHR * 1.3, fcx - fHR * 0.15, fHY - fHR * 0.6, true);
    ear(fcx + fHR * 0.5, fHY - fHR * 0.5, fcx + fHR * 0.6, fHY - fHR * 1.3, fcx + fHR * 0.15, fHY - fHR * 0.6, true);

  } else if (dir === 2) {
    // === RIGHT PROFILE ===
    var Bx2 = W * 0.42, By2 = H * 0.50 + bob;
    var shldrX = Bx2 + 13, shldrY = By2 + 4;
    var hipX = Bx2 - 13, hipY2 = By2 + 4;
    // Far legs (behind body)
    wolfLeg(shldrX - 2, shldrY, frontRA, 7, 7, 5, kneeB, hockB, furDark, lw * 0.9);
    wolfLeg(hipX - 2, hipY2, backRA, 7, 7, 5, kneeB, hockB, furDark, lw * 0.9);
    // Body contour (bezier anchored to shoulder/hip)
    c.fillStyle = fur;
    c.beginPath();
    c.moveTo(shldrX + 4, shldrY - 7);
    c.bezierCurveTo(Bx2 + 5, By2 - 8, Bx2 - 8, By2 - 7, hipX - 4, hipY2 - 4);
    c.bezierCurveTo(hipX - 7, hipY2, hipX - 5, hipY2 + 4, hipX, hipY2 + 3);
    c.bezierCurveTo(Bx2 - 5, By2 + 5, Bx2 + 5, By2 + 4, shldrX, shldrY + 2);
    c.bezierCurveTo(shldrX + 5, shldrY - 1, shldrX + 6, shldrY - 4, shldrX + 4, shldrY - 7);
    c.closePath(); c.fill();
    // Spine stripe
    c.fillStyle = furDark;
    c.beginPath();
    c.moveTo(shldrX + 2, shldrY - 6);
    c.bezierCurveTo(Bx2 + 3, By2 - 7, Bx2 - 6, By2 - 7, hipX - 2, hipY2 - 3);
    c.bezierCurveTo(hipX, hipY2 - 2, Bx2 - 2, By2 - 4, shldrX, shldrY - 3);
    c.closePath(); c.fill();
    // Belly
    c.fillStyle = belly;
    c.beginPath();
    c.moveTo(shldrX - 2, shldrY + 1);
    c.bezierCurveTo(Bx2 + 2, By2 + 4, Bx2 - 4, By2 + 4, hipX + 2, hipY2 + 2);
    c.bezierCurveTo(Bx2 - 2, By2 + 2, Bx2 + 2, By2 + 1, shldrX - 2, shldrY + 1);
    c.closePath(); c.fill();
    // Near legs (over body)
    wolfLeg(shldrX + 2, shldrY, frontLA, 7, 7, 5, kneeB, hockB, furLight, lw);
    wolfLeg(hipX + 2, hipY2, backLA, 7, 7, 5, kneeB, hockB, furLight, lw);
    // Neck
    var nkX = Bx2 + 17, nkY = By2 - 3;
    var hdX = Bx2 + 26, hdY = By2 - 8 + bob;
    c.fillStyle = fur;
    c.beginPath();
    c.moveTo(shldrX, shldrY - 4);
    c.bezierCurveTo(nkX - 3, nkY + 2, hdX - 8, hdY + 8, hdX - 5, hdY + 6);
    c.lineTo(hdX + 4, hdY + 3);
    c.bezierCurveTo(nkX + 4, nkY - 2, shldrX + 4, shldrY - 2, shldrX + 3, shldrY);
    c.fill();
    // Head
    var hR = H * 0.16;
    c.fillStyle = fur;
    c.beginPath(); c.arc(hdX, hdY, hR, 0, Math.PI * 2); c.fill();
    c.fillStyle = furDark;
    c.beginPath(); c.ellipse(hdX + hR * 0.15, hdY - hR * 0.35, hR * 0.65, hR * 0.25, -0.1, Math.PI, Math.PI * 2); c.fill();
    // Muzzle
    c.fillStyle = fur;
    c.beginPath();
    c.moveTo(hdX + hR * 0.4, hdY - hR * 0.15);
    c.bezierCurveTo(hdX + hR * 1.0, hdY - hR * 0.2, hdX + hR * 1.4, hdY + hR * 0.05, hdX + hR * 1.55, hdY + hR * 0.18);
    c.bezierCurveTo(hdX + hR * 1.4, hdY + hR * 0.35, hdX + hR * 1.0, hdY + hR * 0.55, hdX + hR * 0.3, hdY + hR * 0.55);
    c.closePath(); c.fill();
    c.fillStyle = furDark;
    c.beginPath();
    c.moveTo(hdX + hR * 0.3, hdY + hR * 0.4);
    c.bezierCurveTo(hdX + hR * 0.8, hdY + hR * 0.7, hdX + hR * 1.2, hdY + hR * 0.55, hdX + hR * 1.45, hdY + hR * 0.28);
    c.bezierCurveTo(hdX + hR * 1.1, hdY + hR * 0.5, hdX + hR * 0.7, hdY + hR * 0.55, hdX + hR * 0.3, hdY + hR * 0.55);
    c.closePath(); c.fill();
    // Ears
    ear(hdX - hR * 0.42, hdY - hR * 0.48, hdX - hR * 0.18, hdY - hR * 1.38, hdX + hR * 0.22, hdY - hR * 0.55, true);
    ear(hdX + hR * 0.12, hdY - hR * 0.52, hdX + hR * 0.42, hdY - hR * 1.28, hdX + hR * 0.70, hdY - hR * 0.42, true);
    // Eye — almond + slit pupil
    c.fillStyle = eyeCol;
    c.beginPath();
    c.moveTo(hdX + hR * 0.3, hdY - hR * 0.08);
    c.bezierCurveTo(hdX + hR * 0.38, hdY - hR * 0.22, hdX + hR * 0.58, hdY - hR * 0.22, hdX + hR * 0.65, hdY - hR * 0.08);
    c.bezierCurveTo(hdX + hR * 0.58, hdY + hR * 0.06, hdX + hR * 0.38, hdY + hR * 0.06, hdX + hR * 0.3, hdY - hR * 0.08);
    c.closePath(); c.fill();
    c.fillStyle = noseCol;
    c.beginPath(); c.ellipse(hdX + hR * 0.48, hdY - hR * 0.08, hR * 0.04, hR * 0.1, 0, 0, Math.PI * 2); c.fill();
    // Nose
    c.fillStyle = noseCol;
    c.beginPath(); c.ellipse(hdX + hR * 1.5, hdY + hR * 0.16, hR * 0.18, hR * 0.13, -0.2, 0, Math.PI * 2); c.fill();
    // Tail
    var txB = hipX - 4, tyB = hipY2 - 5;
    c.strokeStyle = fur; c.lineWidth = 4; c.lineCap = 'round';
    c.beginPath(); c.moveTo(txB, tyB);
    c.bezierCurveTo(txB - 5, tyB - 4 + wag * 0.2, txB - 10, tyB - 12 + wag * 0.5, txB - 6, tyB - 22 + wag);
    c.stroke();
    c.strokeStyle = furLight; c.lineWidth = 2.5;
    c.beginPath(); c.moveTo(txB - 7, tyB - 16 + wag * 0.7); c.lineTo(txB - 5, tyB - 24 + wag); c.stroke();

  } else if (dir === 4) {
    // === BACK VIEW ===
    var bcx = W / 2, bcy = H * 0.42 + bob;
    var rW = W * 0.20, rH = H * 0.15;
    var legY4 = bcy + rH * 0.7;
    // Far legs (front pair, hidden behind body)
    wolfLeg(bcx - 10, legY4 + 2, frontLA * 0.7, 5, 5, 4, kneeB, hockB, furDark, lw * 0.85);
    wolfLeg(bcx + 10, legY4 + 2, frontRA * 0.7, 5, 5, 4, kneeB, hockB, furDark, lw * 0.85);
    // Body (rump facing camera)
    c.fillStyle = fur;
    c.beginPath(); c.ellipse(bcx, bcy + rH * 0.15, rW, rH, 0, 0, Math.PI * 2); c.fill();
    c.fillStyle = furDark;
    c.beginPath(); c.ellipse(bcx, bcy - rH * 0.1, rW * 0.5, rH * 0.5, 0, 0, Math.PI * 2); c.fill();
    // Near legs (back pair, over body)
    wolfLeg(bcx - 11, legY4, backLA, 5, 5, 4, kneeB, hockB, fur, lw);
    wolfLeg(bcx + 11, legY4, backRA, 5, 5, 4, kneeB, hockB, furLight, lw);
    // Tail (prominent, curving up)
    c.strokeStyle = fur; c.lineWidth = 4; c.lineCap = 'round';
    c.beginPath(); c.moveTo(bcx, bcy - rH * 0.6);
    c.bezierCurveTo(bcx + wag * 0.3, bcy - rH * 1.2, bcx + wag * 0.5, bcy - rH * 1.7, bcx + wag * 0.4, bcy - rH * 2.2);
    c.stroke();
    c.strokeStyle = furLight; c.lineWidth = 2.5;
    c.beginPath(); c.moveTo(bcx + wag * 0.35, bcy - rH * 1.8); c.lineTo(bcx + wag * 0.4, bcy - rH * 2.3); c.stroke();
    // Head (back of head, small)
    var bHY = bcy - rH * 0.8, bHR = H * 0.11;
    c.fillStyle = fur;
    c.beginPath(); c.arc(bcx, bHY, bHR, 0, Math.PI * 2); c.fill();
    c.fillStyle = furDark;
    c.beginPath(); c.ellipse(bcx, bHY, bHR * 0.7, bHR * 0.55, 0, 0, Math.PI * 2); c.fill();
    // Ears from behind (no inner color)
    ear(bcx - bHR * 0.65, bHY - bHR * 0.3, bcx - bHR * 0.85, bHY - bHR * 1.2, bcx - bHR * 0.2, bHY - bHR * 0.5, false);
    ear(bcx + bHR * 0.65, bHY - bHR * 0.3, bcx + bHR * 0.85, bHY - bHR * 1.2, bcx + bHR * 0.2, bHY - bHR * 0.5, false);

  } else if (dir === 1) {
    // === 3/4 FRONT VIEW ===
    var cx1 = W * 0.47, cy1 = H * 0.44 + bob;
    var bw1 = W * 0.24, bh1 = H * 0.13;
    var legY1 = cy1 + bh1 * 0.7;
    var offX = 3;
    // Far legs (behind body, thinner)
    wolfLeg(cx1 + offX - 8, legY1 + 2, backLA * 0.8, 6, 6, 4, kneeB, hockB, furDark, lw * 0.85);
    wolfLeg(cx1 + offX + 10, legY1 + 2, frontRA, 6, 6, 4, kneeB, hockB, furDark, lw * 0.85);
    // Body (angled)
    c.fillStyle = fur;
    c.beginPath(); c.ellipse(cx1, cy1, bw1, bh1, -0.2, 0, Math.PI * 2); c.fill();
    c.fillStyle = furDark;
    c.beginPath(); c.ellipse(cx1, cy1 - bh1 * 0.5, bw1 * 0.65, bh1 * 0.3, -0.15, 0, Math.PI * 2); c.fill();
    c.fillStyle = belly;
    c.beginPath(); c.ellipse(cx1 + offX, cy1 + bh1 * 0.35, bw1 * 0.3, bh1 * 0.25, 0, 0, Math.PI * 2); c.fill();
    // Near legs (over body, thicker)
    wolfLeg(cx1 + offX - 5, legY1, backRA * 0.8, 6, 6, 4, kneeB, hockB, fur, lw);
    wolfLeg(cx1 + offX + 13, legY1, frontLA, 6, 6, 4, kneeB, hockB, furLight, lw);
    // Neck + head (3/4 view)
    var hx1 = cx1 + bw1 * 0.55, hy1 = cy1 - bh1 * 0.9, hr1 = H * 0.15;
    c.fillStyle = fur;
    c.beginPath();
    c.moveTo(cx1 + bw1 * 0.2, cy1 - bh1 * 0.3);
    c.quadraticCurveTo(hx1 - hr1 * 0.5, hy1 + hr1 * 0.8, hx1 - hr1 * 0.3, hy1 + hr1 * 0.4);
    c.lineTo(hx1 + hr1 * 0.5, hy1 + hr1 * 0.3);
    c.quadraticCurveTo(cx1 + bw1 * 0.6, cy1 - bh1 * 0.4, cx1 + bw1 * 0.5, cy1 - bh1 * 0.1);
    c.fill();
    c.beginPath(); c.arc(hx1, hy1, hr1, 0, Math.PI * 2); c.fill();
    // Muzzle
    c.fillStyle = furDark;
    c.beginPath();
    c.moveTo(hx1 + hr1 * 0.4, hy1 - hr1 * 0.1);
    c.bezierCurveTo(hx1 + hr1 * 0.9, hy1 - hr1 * 0.1, hx1 + hr1 * 1.2, hy1 + hr1 * 0.15, hx1 + hr1 * 1.25, hy1 + hr1 * 0.25);
    c.bezierCurveTo(hx1 + hr1 * 1.0, hy1 + hr1 * 0.4, hx1 + hr1 * 0.5, hy1 + hr1 * 0.45, hx1 + hr1 * 0.3, hy1 + hr1 * 0.4);
    c.closePath(); c.fill();
    // Ears
    ear(hx1 - hr1 * 0.3, hy1 - hr1 * 0.5, hx1 - hr1 * 0.4, hy1 - hr1 * 1.25, hx1 + hr1 * 0.05, hy1 - hr1 * 0.6, false);
    ear(hx1 + hr1 * 0.2, hy1 - hr1 * 0.5, hx1 + hr1 * 0.4, hy1 - hr1 * 1.2, hx1 + hr1 * 0.6, hy1 - hr1 * 0.45, true);
    // Near eye (larger)
    c.fillStyle = eyeCol;
    c.beginPath(); c.ellipse(hx1 + hr1 * 0.42, hy1 - hr1 * 0.05, hr1 * 0.16, hr1 * 0.11, 0, 0, Math.PI * 2); c.fill();
    c.fillStyle = noseCol; c.beginPath(); c.arc(hx1 + hr1 * 0.42, hy1 - hr1 * 0.05, hr1 * 0.05, 0, Math.PI * 2); c.fill();
    // Far eye (smaller)
    c.fillStyle = eyeCol;
    c.beginPath(); c.ellipse(hx1 - hr1 * 0.08, hy1 - hr1 * 0.02, hr1 * 0.09, hr1 * 0.07, 0, 0, Math.PI * 2); c.fill();
    // Nose
    c.fillStyle = noseCol;
    c.beginPath(); c.ellipse(hx1 + hr1 * 1.2, hy1 + hr1 * 0.22, hr1 * 0.12, hr1 * 0.09, 0, 0, Math.PI * 2); c.fill();
    // Tail (partially visible behind far hip)
    var tx1 = cx1 - bw1 * 0.7, ty1 = cy1 - bh1 * 0.3;
    c.strokeStyle = fur; c.lineWidth = 3; c.lineCap = 'round';
    c.beginPath(); c.moveTo(tx1, ty1); c.quadraticCurveTo(tx1 - 5, ty1 - 7 + wag * 0.3, tx1 - 3, ty1 - 16 + wag); c.stroke();

  } else if (dir === 3) {
    // === 3/4 BACK VIEW ===
    var cx3 = W * 0.47, cy3 = H * 0.44 + bob;
    var bw3 = W * 0.24, bh3 = H * 0.13;
    var legY3 = cy3 + bh3 * 0.7;
    var offX3 = 3;
    // Far legs (behind body)
    wolfLeg(cx3 + offX3 - 5, legY3 + 2, frontLA * 0.8, 6, 6, 4, kneeB, hockB, furDark, lw * 0.85);
    wolfLeg(cx3 + offX3 + 10, legY3 + 2, backRA * 0.8, 6, 6, 4, kneeB, hockB, furDark, lw * 0.85);
    // Body (angled, showing rump)
    c.fillStyle = fur;
    c.beginPath(); c.ellipse(cx3, cy3, bw3, bh3, 0.2, 0, Math.PI * 2); c.fill();
    c.fillStyle = furDark;
    c.beginPath(); c.ellipse(cx3, cy3 - bh3 * 0.45, bw3 * 0.6, bh3 * 0.3, 0.15, 0, Math.PI * 2); c.fill();
    // Near legs (over body)
    wolfLeg(cx3 + offX3 - 8, legY3, frontRA * 0.8, 6, 6, 4, kneeB, hockB, fur, lw);
    wolfLeg(cx3 + offX3 + 13, legY3, backLA, 6, 6, 4, kneeB, hockB, furLight, lw);
    // Tail (prominent)
    var tx3 = cx3 - bw3 * 0.45, ty3 = cy3 - bh3 * 0.45;
    c.strokeStyle = fur; c.lineWidth = 3.5; c.lineCap = 'round';
    c.beginPath(); c.moveTo(tx3, ty3);
    c.bezierCurveTo(tx3 - 4, ty3 - 6 + wag * 0.2, tx3 - 8, ty3 - 14 + wag * 0.5, tx3 - 5, ty3 - 22 + wag);
    c.stroke();
    c.strokeStyle = furLight; c.lineWidth = 2;
    c.beginPath(); c.moveTo(tx3 - 6, ty3 - 16 + wag * 0.7); c.lineTo(tx3 - 4, ty3 - 23 + wag); c.stroke();
    // Head (back, partially turned)
    var hx3 = cx3 + bw3 * 0.5, hy3 = cy3 - bh3 * 0.85, hr3 = H * 0.13;
    c.fillStyle = fur;
    c.beginPath();
    c.moveTo(cx3 + bw3 * 0.15, cy3 - bh3 * 0.3);
    c.quadraticCurveTo(hx3 - hr3, hy3 + hr3, hx3 - hr3 * 0.3, hy3 + hr3 * 0.3);
    c.lineTo(hx3 + hr3 * 0.4, hy3 + hr3 * 0.3);
    c.quadraticCurveTo(cx3 + bw3 * 0.6, cy3 - bh3 * 0.4, cx3 + bw3 * 0.45, cy3 - bh3 * 0.15);
    c.fill();
    c.beginPath(); c.arc(hx3, hy3, hr3, 0, Math.PI * 2); c.fill();
    c.fillStyle = furDark;
    c.beginPath(); c.ellipse(hx3 - hr3 * 0.1, hy3, hr3 * 0.65, hr3 * 0.55, 0, 0, Math.PI * 2); c.fill();
    // Ears from back
    ear(hx3 - hr3 * 0.5, hy3 - hr3 * 0.3, hx3 - hr3 * 0.7, hy3 - hr3 * 1.15, hx3 - hr3 * 0.1, hy3 - hr3 * 0.5, false);
    ear(hx3 + hr3 * 0.3, hy3 - hr3 * 0.35, hx3 + hr3 * 0.5, hy3 - hr3 * 1.1, hx3 + hr3 * 0.65, hy3 - hr3 * 0.35, false);
    // Partial muzzle edge
    c.fillStyle = fur;
    c.beginPath();
    c.moveTo(hx3 + hr3 * 0.6, hy3 + hr3 * 0.1);
    c.quadraticCurveTo(hx3 + hr3 * 0.95, hy3 + hr3 * 0.15, hx3 + hr3 * 0.85, hy3 + hr3 * 0.35);
    c.quadraticCurveTo(hx3 + hr3 * 0.7, hy3 + hr3 * 0.4, hx3 + hr3 * 0.5, hy3 + hr3 * 0.35);
    c.fill();
  }
}

function buildWolfSprites() {
  wolfFrames = [];
  var t0 = Date.now();
  var totalCanvases = 0;
  for (var d = 0; d < SPRITE_DIRS; d++) {
    wolfFrames[d] = [];
    var srcDir = MIRROR_DIR[d];
    var flip = MIRROR_FLIP[d];
    for (var f = 0; f < WOLF_FRAMES; f++) {
      var oc = document.createElement('canvas');
      oc.width = WOLF_W; oc.height = WOLF_H;
      if (!flip) {
        drawWolfFrame(oc, f, srcDir);
      } else {
        var tmp = document.createElement('canvas');
        tmp.width = WOLF_W; tmp.height = WOLF_H;
        drawWolfFrame(tmp, f, srcDir);
        var mc = oc.getContext('2d');
        mc.translate(WOLF_W, 0); mc.scale(-1, 1);
        mc.drawImage(tmp, 0, 0);
      }
      wolfFrames[d].push(oc);
      totalCanvases++;
    }
  }
  console.log('[WOLF] buildWolfSprites complete — ' + totalCanvases + ' canvases @ ' + WOLF_W + 'x' + WOLF_H + ' in ' + (Date.now() - t0) + 'ms');
}

var _debugDir = 0;
var _dirNames = ['Front','3/4F-R','Right','3/4B-R','Back','3/4B-L','Left','3/4F-L'];
function drawSkeletonDebug() {
  if (!skeletonFrames) { return; }
  // Cycle direction every 2 seconds
  _debugDir = Math.floor(Date.now() / 2000) % SPRITE_DIRS;
  var pad = 6, slotW = 26, slotH = 42, labelW = 52, titleH = 18, rowGap = 2;
  var panW = labelW + SKEL_FRAMES * (slotW + 1) + pad * 2;
  var panH = titleH + 3 * (slotH + rowGap) + pad;
  var px = pad, py = pad;
  var now2 = Date.now();
  var typesMeta = [
    {id:'fast',   label:'Scout',   spd:55},
    {id:'normal', label:'Soldier', spd:35},
    {id:'tank',   label:'Brute',   spd:20}
  ];

  ctx.save();

  // Panel background
  ctx.fillStyle = 'rgba(0,0,0,0.72)';
  ctx.fillRect(px, py, panW, panH);
  ctx.strokeStyle = '#666'; ctx.lineWidth = 1;
  ctx.strokeRect(px, py, panW, panH);

  // Title with direction
  ctx.fillStyle = '#cccccc'; ctx.font = 'bold 11px monospace';
  ctx.fillText('SKEL dir=' + _debugDir + ' ' + _dirNames[_debugDir], px + 4, py + 13);

  for (var ti = 0; ti < typesMeta.length; ti++) {
    var tm = typesMeta[ti];
    var msPerFrame = Math.max(50, 200 - tm.spd * 2);
    var activeFrame = Math.floor(now2 / msPerFrame) % SKEL_FRAMES;
    var rowY = py + titleH + ti * (slotH + rowGap);
    var dirFrames = skeletonFrames[tm.id] && skeletonFrames[tm.id][_debugDir];

    // Row label
    ctx.fillStyle = '#aaaaaa'; ctx.font = '10px monospace';
    ctx.fillText(tm.label, px + 3, rowY + slotH / 2 + 4);

    if (!dirFrames) { continue; }
    for (var f = 0; f < SKEL_FRAMES; f++) {
      var fx = px + labelW + f * (slotW + 1);
      var fy = rowY;
      var isActive = (f === activeFrame);

      // Slot background + border
      if (isActive) {
        ctx.fillStyle = 'rgba(255,255,80,0.18)'; ctx.fillRect(fx, fy, slotW, slotH);
        ctx.strokeStyle = '#ffff44'; ctx.lineWidth = 1.5;
      } else {
        ctx.strokeStyle = '#444444'; ctx.lineWidth = 0.5;
      }
      ctx.strokeRect(fx, fy, slotW, slotH);

      // Sprite scaled to fit slot
      if (dirFrames[f]) {
        var scale = Math.min(slotW / SKEL_W, slotH / SKEL_H);
        var dw = SKEL_W * scale, dh = SKEL_H * scale;
        ctx.drawImage(dirFrames[f], fx + (slotW - dw) / 2, fy + (slotH - dh) / 2, dw, dh);
      }

      // Frame index label (bottom of slot)
      ctx.fillStyle = isActive ? '#ffff44' : '#666666'; ctx.font = '8px monospace';
      ctx.fillText(f, fx + 2, fy + slotH - 2);
    }
  }
  ctx.restore();
}

// Bounds include every actual draw component, not only a point at the feet.
// The scene-depth mask can therefore keep a visible head/health bar while a
// foreground bank covers the lower body, without letting status effects leak.
function getEnemyBillboardBounds(e, screenX, floorY, spriteW, spriteH, hasSprite, now) {
  var size = spriteH * 0.43, top = floorY - spriteH;
  var x0 = screenX - spriteW / 2, x1 = screenX + spriteW / 2;
  var y0 = top, y1 = floorY;
  function include(cx, cy, rx, ry) {
    x0 = Math.min(x0, cx - rx); x1 = Math.max(x1, cx + rx);
    y0 = Math.min(y0, cy - ry); y1 = Math.max(y1, cy + ry);
  }
  if (!hasSprite) {
    var fallbackStroke = Math.max(1, Math.min(3, Math.floor(size / 15))) / 2;
    include(screenX, floorY - size / 2, size / 2 + fallbackStroke, size / 2 + fallbackStroke);
  }
  include(screenX, floorY, size * 0.45, size * 0.2); // Shadow.
  if (e.slowUntil && now < e.slowUntil) {
    var slowRadius = size * 0.65 + Math.max(2, Math.min(4, Math.floor(size / 12))) / 2;
    include(screenX, top + spriteH * 0.45, slowRadius, slowRadius);
  }
  if (e.burnUntil && now < e.burnUntil) {
    include(screenX, top + spriteH * 0.08, size * 0.3, size * 0.3);
    include(screenX, top - size * 0.1, size * 0.18, size * 0.18);
  }
  if (e.attackState === 'windup' && e.attackStateUntil) {
    var windupDur = e.enemyType.attackWindup || 500;
    var windupT = Math.max(0, Math.min(1, 1 - (e.attackStateUntil - now) / windupDur));
    var ringR = size * (0.45 + windupT * 0.45);
    var ringStroke = Math.max(2, Math.floor(size / 6)) / 2;
    include(screenX, floorY, ringR + ringStroke, ringR * 0.28 + ringStroke);
  }
  var hpPct = e.health / e.maxHealth;
  if (hpPct < 1) {
    var barW = Math.max(20, size * 1.2), barH = Math.max(2, Math.floor(size / 10));
    include(screenX, top - barH / 2 - 2, barW / 2, barH / 2);
  }
  if (e.aggroAt && now - e.aggroAt < 600) {
    var fontSize = Math.max(10, Math.floor(size * 0.5));
    var baseline = top - (hpPct < 1 ? 10 : 4);
    // Monospace glyph bounds include the full em and possible descent. This
    // avoids a second text-metrics pass for every aggro animation frame.
    include(screenX, baseline - fontSize * 0.4, fontSize * 0.65, fontSize * 0.65);
  }
  // One pixel also includes Canvas stroke antialiasing at the footprint edge.
  return {x: Math.floor(x0) - 1, y: Math.floor(y0) - 1,
    width: Math.ceil(x1) - Math.floor(x0) + 2, height: Math.ceil(y1) - Math.floor(y0) + 2};
}

function drawEnemies3D() {
  if (!enemies || !enemies.length) return;
  var C = getCam3D();
  var w = C.w, h = C.h, cosAng = C.cosAng, sinAng = C.sinAng;
  var invTanHalf = C.invTanHalf, horizonY = C.horizonY, cameraZ = C.cameraZ;
  var now = Date.now();
  var _eRenderStats = {total: enemies.length, tooFar: 0, behind: 0, depthOccl: 0, belowFloor: 0, midOccl: 0, rendered: 0};
  // Collect visible enemies into reusable sort buffer (no per-frame allocations).
  // _eSortIdx[i] = enemy index, _eSortDist[i] = distance for sorting.
  if (!drawEnemies3D._sortIdx) {
    drawEnemies3D._sortIdx = new Uint16Array(512);
    drawEnemies3D._sortDist = new Float32Array(512);
    drawEnemies3D._visCache = new Array(512); // reusable vis result slots
    for (var _vi = 0; _vi < 512; _vi++) drawEnemies3D._visCache[_vi] = {sx:0,sy:0,fwd:0,dist:0,floorZ:0,fade:0};
    drawEnemies3D._sortTemp = {sx:0,sy:0,fwd:0,dist:0,floorZ:0,fade:0};
  }
  var _eSortIdx = drawEnemies3D._sortIdx;
  var _eSortDist = drawEnemies3D._sortDist;
  var _eVisCache = drawEnemies3D._visCache;
  var _eVisCount = 0;
  for (var i = 0; i < enemies.length; i++) {
    var e = enemies[i];
    // Surface enemies historically store a placeholder z:0; cave enemies
    // store absolute render-world Z. Neither depends on the camera's stratum.
    var enemyZ = e.underground && Number.isFinite(e.z) ? e.z : getEntityRenderFloorZ(e);
    var vis = entityVisible3D(e.x, e.y, enemyZ, C,
      { maxDist: viewDist, sceneDepth: true, fadeFraction: 0.8, stats: _eRenderStats });
    if (!vis) continue;
    if (_eVisCount >= _eSortIdx.length) break;
    // Copy vis data into reusable cache slot (no object allocation)
    var vc = _eVisCache[_eVisCount];
    vc.sx = vis.sx; vc.sy = vis.sy; vc.fwd = vis.fwd;
    vc.dist = vis.dist; vc.floorZ = enemyZ; vc.fade = vis.fade;
    _eSortIdx[_eVisCount] = i;
    _eSortDist[_eVisCount] = vis.dist;
    _eVisCount++;
  }
  // Simple insertion sort (faster than Array.sort for small N, zero allocation).
  // The scratch slot must not alias the final visible enemy's own data.
  var _eTmp = drawEnemies3D._sortTemp;
  for (var si = 1; si < _eVisCount; si++) {
    var sd = _eSortDist[si], sIdx = _eSortIdx[si];
    _eTmp.sx = _eVisCache[si].sx; _eTmp.sy = _eVisCache[si].sy;
    _eTmp.fwd = _eVisCache[si].fwd; _eTmp.dist = _eVisCache[si].dist;
    _eTmp.floorZ = _eVisCache[si].floorZ; _eTmp.fade = _eVisCache[si].fade;
    var sj = si - 1;
    while (sj >= 0 && _eSortDist[sj] < sd) {
      _eSortDist[sj+1] = _eSortDist[sj]; _eSortIdx[sj+1] = _eSortIdx[sj];
      _eVisCache[sj+1].sx = _eVisCache[sj].sx; _eVisCache[sj+1].sy = _eVisCache[sj].sy;
      _eVisCache[sj+1].fwd = _eVisCache[sj].fwd; _eVisCache[sj+1].dist = _eVisCache[sj].dist;
      _eVisCache[sj+1].floorZ = _eVisCache[sj].floorZ; _eVisCache[sj+1].fade = _eVisCache[sj].fade;
      sj--;
    }
    _eSortDist[sj+1] = sd; _eSortIdx[sj+1] = sIdx;
    _eVisCache[sj+1].sx = _eTmp.sx; _eVisCache[sj+1].sy = _eTmp.sy;
    _eVisCache[sj+1].fwd = _eTmp.fwd; _eVisCache[sj+1].dist = _eTmp.dist;
    _eVisCache[sj+1].floorZ = _eTmp.floorZ; _eVisCache[sj+1].fade = _eTmp.fade;
  }
  for (var vi = 0; vi < _eVisCount; vi++) {
    var e = enemies[_eSortIdx[vi]], eType = e.enemyType, vis = _eVisCache[vi];
    var _eFade = vis.fade;
    var screenX = vis.sx, fwd = vis.fwd, dist = vis.dist;
    var floorY = vis.sy;
    var isWolf = eType.id === 'wolf';
    // Wolves are lower to the ground than upright skeletons
    var worldEnemyH = (isWolf ? 38 : 60) * (eType.size || 1);
    var spriteH = Math.max(6, Math.min(h * 2, Math.floor(worldEnemyH * projScale / fwd)));
    var spriteW = isWolf ? Math.floor(spriteH * WOLF_W / WOLF_H) : Math.floor(spriteH * SKEL_W / SKEL_H);
    var size = spriteH * 0.43;
    // Project the actual feet once. Screen-space clamps/lift invented another
    // base when standing on hills and when the camera crossed a cave portal.
    var centerY = floorY - size / 2;
    var isFlashing = (e.damageFlash && now < e.damageFlash);
    var hpPct = e.health / e.maxHealth;

    // Pick walk frame: cycle speed proportional to enemy speed; freeze on frame 0 when idle
    var isMoving = (dist > 10 && dist < eType.chaseRange);
    var msPerFrame = Math.max(50, 200 - eType.speed * 2);
    var frameCount = isWolf ? WOLF_FRAMES : SKEL_FRAMES;
    var skelFrameIdx = isMoving ? (Math.floor(now / msPerFrame) % frameCount) : 0;
    var skelKey = (hpPct < 0.25) ? (eType.id + '_crumble') : eType.id;
    var dirIdx = getDirIndex(e.facing || 0, cam.ang);
    var skelCanvas = isWolf
      ? (wolfFrames && wolfFrames[dirIdx] && wolfFrames[dirIdx][skelFrameIdx])
      : (skeletonFrames && skeletonFrames[skelKey] && skeletonFrames[skelKey][dirIdx] && skeletonFrames[skelKey][dirIdx][skelFrameIdx]);

    if (DEBUG_SKELETON) {
      // Per-enemy flicker diagnostics — log any frame where something looks wrong
      var _ei = _eSortIdx[vi];
      var _prev = drawEnemies3D._dbgPrev || (drawEnemies3D._dbgPrev = {});
      var _p = _prev[_ei] || (_prev[_ei] = {});
      var _fadeDelta = Math.abs((_eFade) - (_p.fade || 0));
      var _hDelta = Math.abs(spriteH - (_p.spriteH || 0));
      var _frameChanged = skelFrameIdx !== _p.skelFrameIdx;
      // Log if fade jumped >0.15, sprite height jumped >10px, or no canvas found
      if (!skelCanvas || _fadeDelta > 0.15 || _hDelta > 10) {
        console.log('[SKEL #' + _ei + ' ' + eType.id + ']' +
          ' dist=' + dist.toFixed(1) +
          ' fade=' + _eFade.toFixed(3) + ((_fadeDelta > 0.15) ? ' FADE_JUMP(Δ' + _fadeDelta.toFixed(3) + ')' : '') +
          ' spriteH=' + spriteH + ((_hDelta > 10) ? ' H_JUMP(Δ' + _hDelta.toFixed(0) + ')' : '') +
          ' spriteW=' + spriteW +
          ' floorY=' + floorY.toFixed(1) +
          ' z=' + (e.z || 0).toFixed(1) +
          ' frame=' + skelFrameIdx + '/' + SKEL_FRAMES +
          ' key=' + skelKey +
          (!skelCanvas ? ' NO_CANVAS' : '') +
          ' ambient=' + ambientLight.toFixed(3) +
          ' isMoving=' + isMoving +
          ' hp=' + hpPct.toFixed(2));
      }
      _p.fade = _eFade; _p.spriteH = spriteH; _p.skelFrameIdx = skelFrameIdx;
    }

    // Sprite screen rect — spriteH and spriteW computed from perspective above
    var spriteX = screenX - spriteW / 2;
    var spriteTop = floorY - spriteH;
    var enemyBounds = getEnemyBillboardBounds(e, screenX, floorY, spriteW, spriteH, !!skelCanvas, now);
    var visiblePixels = withSceneDepthBillboard(enemyBounds, fwd, function () {
    ctx.save();
    var shadowAlpha = Math.max(0.15, Math.min(0.5, 0.6 - dist / 1000)) * _eFade;
    ctx.globalAlpha = shadowAlpha; ctx.fillStyle = '#000000';
    ctx.beginPath(); ctx.ellipse(screenX, floorY, size * 0.45, size * 0.2, 0, 0, Math.PI * 2); ctx.fill();
    ctx.globalAlpha = 0.95 * _eFade;
    if (skelCanvas) {
      var drawSrc = skelCanvas;
      if (ambientLight < 0.85) {
        // Tint bones directly without a bounding-box fillRect artifact.
        // source-atop on the scratch canvas only affects non-transparent pixels
        // (the actual bone strokes), leaving the transparent surroundings untouched.
        if (_skelScratch.width < spriteW) _skelScratch.width = spriteW;
        if (_skelScratch.height < spriteH) _skelScratch.height = spriteH;
        _skelScratchCtx.clearRect(0, 0, spriteW, spriteH);
        _skelScratchCtx.drawImage(skelCanvas, 0, 0, spriteW, spriteH);
        _skelScratchCtx.globalCompositeOperation = 'source-atop';
        _skelScratchCtx.globalAlpha = (1.0 - ambientLight) * 0.85;
        _skelScratchCtx.fillStyle = '#000000';
        _skelScratchCtx.fillRect(0, 0, spriteW, spriteH);
        _skelScratchCtx.globalCompositeOperation = 'source-over';
        _skelScratchCtx.globalAlpha = 1.0;
        drawSrc = _skelScratch;
      }
      ctx.globalAlpha = 0.95 * _eFade;
      if (drawSrc === _skelScratch) {
        ctx.drawImage(_skelScratch, 0, 0, spriteW, spriteH, spriteX, spriteTop, spriteW, spriteH);
      } else {
        ctx.drawImage(drawSrc, spriteX, spriteTop, spriteW, spriteH);
      }
      if (isFlashing) {
        ctx.globalAlpha = 0.5 * _eFade;
        ctx.fillStyle = e.damageFlashColor || '#ffffff';
        ctx.fillRect(spriteX, spriteTop, spriteW, spriteH);
        ctx.globalAlpha = 0.95 * _eFade;
      }
    } else {
      // Fallback to circle if sprites not ready
      var _fbLight = ambientLight < 0.85 ? ambientLight : 1.0;
      ctx.fillStyle = isFlashing ? '#ffffff' : eType.color;
      ctx.globalAlpha = 0.95 * _eFade * _fbLight;
      ctx.strokeStyle = '#990000';
      ctx.lineWidth = Math.max(1, Math.min(3, Math.floor(size / 15)));
      ctx.beginPath(); ctx.arc(screenX, centerY, size / 2, 0, Math.PI * 2); ctx.fill(); ctx.stroke();
      ctx.globalAlpha = 0.95 * _eFade;
    }

    // Status effects — positioned at mid-torso of the sprite
    var effectCY = spriteTop + spriteH * 0.45;
    if (e.slowUntil && now < e.slowUntil) {
      ctx.strokeStyle = '#00ffff';
      ctx.lineWidth = Math.max(2, Math.min(4, Math.floor(size / 12)));
      ctx.beginPath(); ctx.arc(screenX, effectCY, size * 0.65, 0, Math.PI * 2); ctx.stroke();
    }
    if (e.burnUntil && now < e.burnUntil) {
      ctx.fillStyle = 'rgba(255,100,0,0.7)';
      ctx.beginPath(); ctx.arc(screenX, spriteTop + spriteH * 0.08, size * 0.3, 0, Math.PI * 2); ctx.fill();
      ctx.fillStyle = 'rgba(255,200,0,0.5)';
      ctx.beginPath(); ctx.arc(screenX, spriteTop - size * 0.1, size * 0.18, 0, Math.PI * 2); ctx.fill();
    }

    // Attack windup telegraph — ground ring grows and reddens as strike approaches
    if (e.attackState === 'windup' && e.attackStateUntil) {
      var windupDur = eType.attackWindup || 500;
      var windupT = Math.max(0, Math.min(1, 1 - (e.attackStateUntil - now) / windupDur));
      ctx.strokeStyle = windupT > 0.65 ? '#ff2200' : '#ff8800';
      ctx.lineWidth = Math.max(2, Math.floor(size / 6));
      ctx.globalAlpha = (0.35 + 0.55 * windupT + 0.1 * Math.sin(now * 0.025)) * _eFade;
      var ringR = size * (0.45 + windupT * 0.45);
      ctx.beginPath(); ctx.ellipse(screenX, floorY, ringR, ringR * 0.28, 0, 0, Math.PI * 2); ctx.stroke();
      ctx.globalAlpha = 0.95 * _eFade;
    }

    // Health bar above the head (hpPct already computed above for sprite selection)
    if (hpPct < 1.0) {
      var barW = Math.max(20, size * 1.2); var barH = Math.max(2, Math.floor(size / 10));
      var barY = spriteTop - barH - 2;
      ctx.fillStyle = 'rgba(0,0,0,0.5)'; ctx.fillRect(screenX - barW / 2, barY, barW, barH);
      ctx.fillStyle = (hpPct > 0.5) ? '#00ff00' : (hpPct > 0.25) ? '#ffaa00' : '#ff4444';
      ctx.fillRect(screenX - barW / 2, barY, barW * hpPct, barH);
    }
    // Aggro indicator — red ! above head for 600ms when enemy first spots you
    if (e.aggroAt && now - e.aggroAt < 600) {
      var aggroT = (now - e.aggroAt) / 600;
      ctx.globalAlpha = Math.max(0, 1 - aggroT * aggroT) * _eFade;
      ctx.fillStyle = '#ff3300';
      ctx.font = 'bold ' + Math.max(10, Math.floor(size * 0.5)) + 'px monospace';
      ctx.textAlign = 'center';
      ctx.fillText('!', screenX, spriteTop - (hpPct < 1 ? 10 : 4));
      ctx.textAlign = 'left';
      ctx.globalAlpha = 0.95 * _eFade;
    }
    ctx.restore();
    });
    if (visiblePixels) _eRenderStats.rendered++;
    else _eRenderStats.depthOccl++;
  }
  // Periodic enemy render debug (every 10s)
  if (false && (!window._eRenderLogLast || now - window._eRenderLogLast > 10000)) { // TEMP DISABLED
    window._eRenderLogLast = now;
    console.log('[ENEMY-RENDER] total=' + _eRenderStats.total +
      ' rendered=' + _eRenderStats.rendered +
      ' tooFar=' + _eRenderStats.tooFar +
      ' behind=' + _eRenderStats.behind +
      ' depthOccl=' + _eRenderStats.depthOccl +
      ' belowFloor=' + _eRenderStats.belowFloor +
      ' midOccl=' + _eRenderStats.midOccl);
  }
}

function drawLightningBolt(x, y, ang, size) {
  var len = size * 3.5;
  var endX = x + Math.cos(ang) * len;
  var endY = y + Math.sin(ang) * len;
  var segments = 6;
  ctx.shadowBlur = 8; ctx.shadowColor = '#ffff00';
  ctx.lineWidth = Math.max(2, ctx.lineWidth);
  ctx.beginPath(); ctx.moveTo(x, y);
  for (var s = 1; s <= segments; s++) {
    var t = s / segments;
    var baseX = x + (endX - x) * t;
    var baseY = y + (endY - y) * t;
    var perpAng = ang + Math.PI / 2;
    var offset = (s % 2 === 0 ? 1 : -1) * size * 0.6;
    var segX = baseX + Math.cos(perpAng) * offset;
    var segY = baseY + Math.sin(perpAng) * offset;
    ctx.lineTo(segX, segY);
  }
  ctx.lineTo(endX, endY); ctx.stroke();
  var branch1Ang = ang + 0.7, branch1Len = len * 0.5;
  var branch1X = x + Math.cos(ang) * len * 0.25, branch1Y = y + Math.sin(ang) * len * 0.25;
  ctx.beginPath(); ctx.moveTo(branch1X, branch1Y);
  var b1endX = branch1X + Math.cos(branch1Ang) * branch1Len, b1endY = branch1Y + Math.sin(branch1Ang) * branch1Len;
  ctx.lineTo(b1endX, b1endY); ctx.stroke();
  ctx.beginPath(); ctx.moveTo(b1endX, b1endY);
  ctx.lineTo(b1endX + Math.cos(branch1Ang + 0.4) * branch1Len * 0.3, b1endY + Math.sin(branch1Ang + 0.4) * branch1Len * 0.3);
  ctx.stroke();
  var branch2Ang = ang - 0.8, branch2Len = len * 0.55;
  var branch2X = x + Math.cos(ang) * len * 0.5, branch2Y = y + Math.sin(ang) * len * 0.5;
  ctx.beginPath(); ctx.moveTo(branch2X, branch2Y);
  var b2endX = branch2X + Math.cos(branch2Ang) * branch2Len, b2endY = branch2Y + Math.sin(branch2Ang) * branch2Len;
  ctx.lineTo(b2endX, b2endY); ctx.stroke();
  ctx.beginPath(); ctx.moveTo(b2endX, b2endY);
  ctx.lineTo(b2endX + Math.cos(branch2Ang - 0.5) * branch2Len * 0.35, b2endY + Math.sin(branch2Ang - 0.5) * branch2Len * 0.35);
  ctx.stroke();
  var branch3Ang = ang + 0.5, branch3Len = len * 0.4;
  var branch3X = x + Math.cos(ang) * len * 0.75, branch3Y = y + Math.sin(ang) * len * 0.75;
  ctx.beginPath(); ctx.moveTo(branch3X, branch3Y);
  ctx.lineTo(branch3X + Math.cos(branch3Ang) * branch3Len, branch3Y + Math.sin(branch3Ang) * branch3Len);
  ctx.stroke();
  ctx.shadowBlur = 0;
}

function drawProjectiles2D() {
  if (!projectiles || !projectiles.length) return;
  var now = Date.now();
  ctx.save(); ctx.translate(-viewCam.x, -viewCam.y);
  for (var i = 0; i < projectiles.length; i++) {
    var p = projectiles[i];
    var age = (now - p.spawnMs);
    var t = Math.max(0, Math.min(1, age / (p.lifeMs || 1000)));
    ctx.globalAlpha = 1.0 - t;
    var spellId = (p.spell && p.spell.id) || 'missile';
    if (spellId === 'lightning') {
      ctx.strokeStyle = p.spell.color; ctx.lineWidth = 2; ctx.lineCap = 'round';
      drawLightningBolt(p.x, p.y, p.ang, p.r * 1.5);
    } else if (spellId === 'fire') {
      ctx.shadowBlur = 12; ctx.shadowColor = '#ff6600'; ctx.fillStyle = p.spell.color;
      ctx.beginPath(); ctx.arc(p.x, p.y, p.r * 1.3, 0, Math.PI * 2); ctx.fill();
      ctx.fillStyle = '#ffaa00'; ctx.beginPath(); ctx.arc(p.x, p.y, p.r * 0.7, 0, Math.PI * 2); ctx.fill();
      var flameAng = p.ang + Math.PI;
      for (var f = 0; f < 3; f++) {
        var fang = flameAng + (f - 1) * 0.4;
        var fx = p.x + Math.cos(fang) * (p.r * 1.8);
        var fy = p.y + Math.sin(fang) * (p.r * 1.8);
        ctx.fillStyle = 'rgba(255,100,0,0.6)';
        ctx.beginPath(); ctx.arc(fx, fy, p.r * 0.5, 0, Math.PI * 2); ctx.fill();
      }
      ctx.shadowBlur = 0;
    } else if (spellId === 'ice') {
      ctx.shadowBlur = 10; ctx.shadowColor = '#00ffff'; ctx.strokeStyle = p.spell.color; ctx.lineWidth = 2;
      ctx.fillStyle = 'rgba(0,255,255,0.3)';
      ctx.beginPath(); ctx.arc(p.x, p.y, p.r * 1.2, 0, Math.PI * 2); ctx.fill(); ctx.stroke();
      for (var s = 0; s < 6; s++) {
        var sang = p.ang + (s * Math.PI / 3);
        var sx1 = p.x + Math.cos(sang) * (p.r * 0.4), sy1 = p.y + Math.sin(sang) * (p.r * 0.4);
        var sx2 = p.x + Math.cos(sang) * (p.r * 1.5), sy2 = p.y + Math.sin(sang) * (p.r * 1.5);
        ctx.beginPath(); ctx.moveTo(sx1, sy1); ctx.lineTo(sx2, sy2); ctx.stroke();
      }
      ctx.shadowBlur = 0;
    } else if (spellId === 'poison') {
      ctx.shadowBlur = 10; ctx.shadowColor = '#88ff44'; ctx.fillStyle = 'rgba(136,255,68,0.7)';
      ctx.beginPath(); ctx.arc(p.x, p.y, p.r * 1.4, 0, Math.PI * 2); ctx.fill();
      ctx.fillStyle = 'rgba(50,200,20,0.5)';
      ctx.beginPath(); ctx.arc(p.x, p.y, p.r * 0.8, 0, Math.PI * 2); ctx.fill();
      // Shadow on ground showing actual landing spot for lob
      if (p.isLob && p.targetX !== undefined) {
        ctx.globalAlpha = 0.35 * (1.0 - t * 0.5);
        ctx.fillStyle = '#88ff44';
        ctx.beginPath(); ctx.arc(p.targetX, p.targetY, 14, 0, Math.PI * 2); ctx.fill();
        ctx.strokeStyle = 'rgba(100,255,50,0.6)'; ctx.lineWidth = 1.5;
        ctx.stroke();
        ctx.globalAlpha = 1.0 - t;
      }
      ctx.shadowBlur = 0;
    } else if (spellId === 'arcane') {
      ctx.shadowBlur = 12; ctx.shadowColor = '#cc66ff'; ctx.fillStyle = 'rgba(204,102,255,0.8)';
      ctx.beginPath(); ctx.arc(p.x, p.y, p.r * 1.2, 0, Math.PI * 2); ctx.fill();
      ctx.shadowBlur = 0;
    } else if (spellId === 'missile') {
      // Comet tail — gradient line behind the missile
      var mTLen = p.r * 5.5;
      var mTx0 = p.x - Math.cos(p.ang) * mTLen;
      var mTy0 = p.y - Math.sin(p.ang) * mTLen;
      var mGrad = ctx.createLinearGradient(mTx0, mTy0, p.x, p.y);
      mGrad.addColorStop(0, 'rgba(77,182,255,0)');
      mGrad.addColorStop(1, 'rgba(100,200,255,0.65)');
      ctx.strokeStyle = mGrad; ctx.lineWidth = p.r * 1.8; ctx.lineCap = 'round';
      ctx.shadowBlur = 8; ctx.shadowColor = '#4db6ff';
      ctx.beginPath(); ctx.moveTo(mTx0, mTy0); ctx.lineTo(p.x, p.y); ctx.stroke();
      // Bright orb
      ctx.fillStyle = '#88ccff';
      ctx.beginPath(); ctx.arc(p.x, p.y, p.r * 0.95, 0, Math.PI * 2); ctx.fill();
      ctx.fillStyle = '#e8f8ff';
      ctx.beginPath(); ctx.arc(p.x, p.y, p.r * 0.42, 0, Math.PI * 2); ctx.fill();
      ctx.shadowBlur = 0;
    } else {
      ctx.shadowBlur = 5; ctx.shadowColor = p.spell.color;
      ctx.fillStyle = (p.spell && p.spell.color) || '#ffd54f';
      ctx.beginPath(); ctx.arc(p.x, p.y, p.r, 0, Math.PI * 2); ctx.fill();
      ctx.shadowBlur = 0;
    }
  }
  ctx.restore(); ctx.globalAlpha = 1.0;
}

// Transparent spells share the opaque scene's visibility, but never contribute
// opaque depth themselves. Sprites use padded billboard bounds; extended spells
// use actual per-segment/per-surface depth rather than one center-depth box.
function spellFloorRenderZ(effect, x, y) {
  var referenceZ = Number.isFinite(effect.renderFloorZ) ? effect.renderFloorZ :
    getEntityRenderFloorZ(effect);
  return sampleEntitySupportRenderZ(x, y, referenceZ, effect.underground, true);
}

function drawSpellBillboard(point, radius, glow, draw) {
  if (!point || !Number.isFinite(point.fwd) || point.fwd < 1) return 0;
  var pad = Math.max(0, radius) + Math.max(0, glow) * 2.5 + 2;
  return withSceneDepthBillboard({x:point.sx-pad,y:point.sy-pad,width:pad*2,height:pad*2},
    point.fwd, draw);
}

// Clip a world segment at the near plane before building its screen ribbon.
// The two ribbon ends retain their different forward depths, including glow.
function drawSpellWorldSegment(a, b, C, width, glow, draw) {
  if (!a || !b || !Number.isFinite(a.z) || !Number.isFinite(b.z)) return 0;
  var da = (a.x-cam.x)*C.cosAng+(a.y-cam.y)*C.sinAng;
  var db = (b.x-cam.x)*C.cosAng+(b.y-cam.y)*C.sinAng;
  if (da < 1 && db < 1) return 0;
  if ((da < 1) !== (db < 1)) {
    var t = (1-Math.min(da,db))/Math.abs(db-da);
    var behind = da < 1 ? a : b, front = da < 1 ? b : a;
    var clipped = {x:behind.x+(front.x-behind.x)*t,
      y:behind.y+(front.y-behind.y)*t,z:behind.z+(front.z-behind.z)*t};
    if (da < 1) { a=clipped; da=1; } else { b=clipped; db=1; }
  }
  function project(v, depth) {
    var dx=v.x-cam.x, dy=v.y-cam.y;
    return {x:(0.5+(-dx*C.sinAng+dy*C.cosAng)/depth*C.invTanHalf*0.5)*C.w,
      y:C.horizonY+(C.cameraZ-v.z)/depth*projScale,depth:depth};
  }
  var pa=project(a,da), pb=project(b,db), dx=pb.x-pa.x, dy=pb.y-pa.y;
  var length=Math.hypot(dx,dy), pad=Math.max(1,width*0.5+glow*2.5+2);
  if (length < 0.01) {
    return drawSpellBillboard({sx:pa.x,sy:pa.y,fwd:Math.min(da,db)},width,glow,
      function(){draw(pa,pb);});
  }
  var ux=dx/length, uy=dy/length, nx=-uy*pad, ny=ux*pad;
  var ribbon=[{x:pa.x+nx,y:pa.y+ny,depth:da},{x:pb.x+nx,y:pb.y+ny,depth:db},
    {x:pb.x-nx,y:pb.y-ny,depth:db},{x:pa.x-nx,y:pa.y-ny,depth:da}];
  // Keep glow caps at their endpoint's depth without stretching the main
  // ribbon's reciprocal-depth gradient. One union mask paints the stroke once.
  var startCap=[ribbon[0],ribbon[3],
    {x:pa.x-ux*pad-nx,y:pa.y-uy*pad-ny,depth:da},
    {x:pa.x-ux*pad+nx,y:pa.y-uy*pad+ny,depth:da}];
  var endCap=[ribbon[1],{x:pb.x+ux*pad+nx,y:pb.y+uy*pad+ny,depth:db},
    {x:pb.x+ux*pad-nx,y:pb.y+uy*pad-ny,depth:db},ribbon[2]];
  return withSceneDepthClip(ribbon,function(){draw(pa,pb);},{extraPolygons:[startCap,endCap]});
}

function strokeSpellWorldSegment(a,b,C,width,glow) {
  return drawSpellWorldSegment(a,b,C,width,glow,function(pa,pb){
    ctx.lineWidth=width; ctx.beginPath(); ctx.moveTo(pa.x,pa.y); ctx.lineTo(pb.x,pb.y); ctx.stroke();
  });
}

function spellDiscPoints(x,y,radius,segments,start,end) {
  var points=[], from=Number.isFinite(start)?start:0, to=Number.isFinite(end)?end:Math.PI*2;
  for(var i=0;i<segments;i++) {
    var angle=from+(to-from)*i/segments;
    points.push({x:x+Math.cos(angle)*radius,y:y+Math.sin(angle)*radius});
  }
  return points;
}

// Cache only terrain geometry, not visibility or camera projections. Each
// footprint is clipped to the very same TL–BR terrain triangles as the floor
// pass, avoiding flat decals cutting into banks or jumping to a cave below.
function spellGroundTriangles(effect, minX,minY,maxX,maxY) {
  var gs=floorMesh ? floorMesh.gridSize : 12;
  var x0=Math.floor(minX/gs), y0=Math.floor(minY/gs), x1=Math.floor(maxX/gs), y1=Math.floor(maxY/gs);
  var reference=Number.isFinite(effect.renderFloorZ)?effect.renderFloorZ:getEntityRenderFloorZ(effect);
  var cache=effect._spellGroundCache;
  if(cache && cache.mesh===floorMesh && cache.x0<=x0 && cache.y0<=y0 && cache.x1>=x1 && cache.y1>=y1 && cache.reference===reference)
    return cache.triangles;
  var triangles=[], epsilon=gs*1e-6;
  if(floorMesh) { x0=Math.max(0,x0);y0=Math.max(0,y0);x1=Math.min(floorMesh.w-2,x1);y1=Math.min(floorMesh.h-2,y1); }
  for(var gy=y0;gy<=y1;gy++)for(var gx=x0;gx<=x1;gx++) {
    var wx=gx*gs, wy=gy*gs;
    var vertices=[{x:wx,y:wy},{x:wx+gs,y:wy},{x:wx+gs,y:wy+gs},{x:wx,y:wy+gs}];
    var okay=true;
    for(var vi=0;vi<4;vi++) {
      var vx=vertices[vi].x, vy=vertices[vi].y;
      var z=sampleEntitySupportRenderZ(vx+(vx===wx?epsilon:-epsilon),
        vy+(vy===wy?epsilon:-epsilon),reference,effect.underground,true);
      if(!Number.isFinite(z)) {okay=false;break;}
      vertices[vi].z=z+0.8;
    }
    if(okay) {triangles.push([vertices[0],vertices[1],vertices[2]]);triangles.push([vertices[0],vertices[2],vertices[3]]);}
  }
  effect._spellGroundCache={mesh:floorMesh,x0:x0,y0:y0,x1:x1,y1:y1,reference:reference,triangles:triangles};
  return triangles;
}

function drawSpellGroundPolygon(effect, footprint, C) {
  if(!footprint || footprint.length<3) return;
  var minX=Infinity,minY=Infinity,maxX=-Infinity,maxY=-Infinity;
  for(var pi=0;pi<footprint.length;pi++) {
    minX=Math.min(minX,footprint[pi].x);maxX=Math.max(maxX,footprint[pi].x);
    minY=Math.min(minY,footprint[pi].y);maxY=Math.max(maxY,footprint[pi].y);
  }
  var triangles=spellGroundTriangles(effect,minX,minY,maxX,maxY);
  for(var ti=0;ti<triangles.length;ti++) {
    var triangle=triangles[ti], polygon=footprint;
    var tx0=Math.min(triangle[0].x,triangle[1].x,triangle[2].x),tx1=Math.max(triangle[0].x,triangle[1].x,triangle[2].x);
    var ty0=Math.min(triangle[0].y,triangle[1].y,triangle[2].y),ty1=Math.max(triangle[0].y,triangle[1].y,triangle[2].y);
    if(tx1<minX || tx0>maxX || ty1<minY || ty0>maxY)continue;
    for(var edge=0;edge<3 && polygon.length;edge++) {
      var a=triangle[edge],b=triangle[(edge+1)%3], output=[];
      var prev=polygon[polygon.length-1],pd=(b.x-a.x)*(prev.y-a.y)-(b.y-a.y)*(prev.x-a.x);
      for(var k=0;k<polygon.length;k++) {
        var current=polygon[k],cd=(b.x-a.x)*(current.y-a.y)-(b.y-a.y)*(current.x-a.x);
        if((pd>=0)!==(cd>=0)) {
          var ratio=pd/(pd-cd);output.push({x:prev.x+(current.x-prev.x)*ratio,y:prev.y+(current.y-prev.y)*ratio});
        }
        if(cd>=0)output.push(current);
        prev=current;pd=cd;
      }
      polygon=output;
    }
    if(polygon.length<3)continue;
    var a0=triangle[0],a1=triangle[1],a2=triangle[2];
    var determinant=(a1.x-a0.x)*(a2.y-a0.y)-(a1.y-a0.y)*(a2.x-a0.x);
    var world=[];
    for(var v=0;v<polygon.length;v++) {
      var q=polygon[v],u=((q.x-a0.x)*(a2.y-a0.y)-(q.y-a0.y)*(a2.x-a0.x))/determinant;
      var vv=((a1.x-a0.x)*(q.y-a0.y)-(a1.y-a0.y)*(q.x-a0.x))/determinant;
      world.push({x:q.x,y:q.y,z:a0.z+u*(a1.z-a0.z)+vv*(a2.z-a0.z)});
    }
    var projected=projectSceneWorldPolygon(world,C);
    withSceneDepthClip(projected,function(){traceSceneDepthPolygon(projected);ctx.fill();});
  }
}

function drawSpellGroundRing(effect,x,y,radius,C,width,glow) {
  if(radius<=0)return;
  var count=Math.max(16,Math.min(96,Math.ceil(radius*Math.PI*2/10)));
  var pts=spellDiscPoints(x,y,radius,count),previous=null;
  for(var i=0;i<=count;i++) {
    var point=pts[i%count],z=spellFloorRenderZ(effect,point.x,point.y);
    var vertex=Number.isFinite(z)?{x:point.x,y:point.y,z:z+1.0}:null;
    if(previous && vertex)strokeSpellWorldSegment(previous,vertex,C,width,glow);
    previous=vertex;
  }
}

function drawSpellLandingReticle(p,C) {
  if(!p.spell || p.spell.id!=='poison' || !Number.isFinite(p.targetX) || !Number.isFinite(p.targetY))return;
  if(Math.hypot(p.targetX-cam.x,p.targetY-cam.y)>700)return;
  var cloudR=p.spell.cloudRadius||50,pulse=0.55+0.45*Math.sin((Date.now()-p.spawnMs)*0.014);
  var targetEffect=p._targetGroundEffect;
  if(!targetEffect || targetEffect.x!==p.targetX || targetEffect.y!==p.targetY)
    targetEffect=p._targetGroundEffect={x:p.targetX,y:p.targetY,renderFloorZ:p.targetZ,underground:p.underground};
  ctx.save();ctx.shadowBlur=0;ctx.globalAlpha=0.95;
  ctx.fillStyle='rgba(80,220,30,'+(0.13*pulse)+')';
  drawSpellGroundPolygon(targetEffect,spellDiscPoints(p.targetX,p.targetY,cloudR,32),C);
  ctx.strokeStyle='rgba(120,255,60,'+(0.65*pulse)+')';
  drawSpellGroundRing(targetEffect,p.targetX,p.targetY,cloudR,C,Math.max(1,1.5*resScale),0);
  ctx.fillStyle='rgba(180,255,80,'+(0.55*pulse)+')';
  drawSpellGroundPolygon(targetEffect,spellDiscPoints(p.targetX,p.targetY,cloudR*0.14,12),C);
  ctx.restore();
}

function drawProjectiles3D() {
  if (!projectiles || !projectiles.length) return;
  var C = getCam3D(), artNow = Date.now();
  var horizonY = C.horizonY, cameraZ = C.cameraZ;
  for (var i = 0; i < projectiles.length; i++) {
    var p = projectiles[i];
    // The ground indicator remains independently visible when its airborne
    // projectile is behind the camera or hidden by terrain.
    drawSpellLandingReticle(p,C);
    if (typeof CASTING_ART_ENABLED !== 'undefined' && CASTING_ART_ENABLED &&
        typeof renderMissileProjectileArt === 'function' && renderMissileProjectileArt(p,C,artNow)) continue;
    var vis = entityVisible3D(p.x, p.y, Number.isFinite(p.z) ? p.z : 0, C,
      { maxDist: 600, sceneDepth:true, checkMidpoint: false, fadeFraction: 1 });
    if (!vis) continue;
    var screenX = vis.sx, fwd = vis.fwd;
    var size = Math.max(10 * resScale, Math.min(40 * resScale, Math.floor(560 * resScale * getScale3D('projectile') / fwd)));
    var zVal = Number.isFinite(p.z) ? p.z : 0;
    // Proper perspective: project projectile Z the same way floor/camera Z is projected
    var projWorldZ = zVal;
    var centerY = horizonY + Math.floor(((cameraZ - projWorldZ) / fwd) * projScale);
    ctx.save(); ctx.globalAlpha = 0.95;
    var spellId = (p.spell && p.spell.id) || 'missile';
    var RS = resScale;
    // Lightning branches and comet tails extend well beyond the orb center.
    var extent = spellId==='lightning'?size*4.5:spellId==='missile'?size*3.2:size;
    drawSpellBillboard({sx:screenX,sy:centerY,fwd:fwd},extent,12*RS,function(){
    if (spellId === 'lightning') {
      ctx.strokeStyle = p.spell.color;
      ctx.lineWidth = Math.max(2 * RS, Math.min(4 * RS, size / 5));
      ctx.lineCap = 'round';
      drawLightningBolt(screenX, centerY, p.ang, size * 0.8);
    } else if (spellId === 'fire') {
      ctx.shadowBlur = 10 * RS; ctx.shadowColor = '#ff6600'; ctx.fillStyle = p.spell.color;
      ctx.beginPath(); ctx.arc(screenX, centerY, size * 0.65, 0, Math.PI * 2); ctx.fill();
      ctx.fillStyle = '#ffaa00'; ctx.beginPath(); ctx.arc(screenX, centerY, size * 0.35, 0, Math.PI * 2); ctx.fill();
      ctx.shadowBlur = 0;
    } else if (spellId === 'ice') {
      ctx.shadowBlur = 8 * RS; ctx.shadowColor = '#00ffff'; ctx.strokeStyle = p.spell.color;
      ctx.lineWidth = Math.max(1.5 * RS, size / 10);
      ctx.fillStyle = 'rgba(0,255,255,0.3)';
      ctx.beginPath(); ctx.arc(screenX, centerY, size * 0.6, 0, Math.PI * 2); ctx.fill(); ctx.stroke();
      for (var s = 0; s < 6; s++) {
        var sang = p.ang + (s * Math.PI / 3);
        var sx1 = screenX + Math.cos(sang) * (size * 0.2), sy1 = centerY + Math.sin(sang) * (size * 0.2);
        var sx2 = screenX + Math.cos(sang) * (size * 0.75), sy2 = centerY + Math.sin(sang) * (size * 0.75);
        ctx.beginPath(); ctx.moveTo(sx1, sy1); ctx.lineTo(sx2, sy2); ctx.stroke();
      }
      ctx.shadowBlur = 0;
    } else if (spellId === 'poison') {
      // The ball
      ctx.shadowBlur = 8 * RS; ctx.shadowColor = '#88ff44';
      ctx.fillStyle = 'rgba(136,255,68,0.75)';
      ctx.beginPath(); ctx.arc(screenX, centerY, size * 0.7, 0, Math.PI * 2); ctx.fill();
      ctx.fillStyle = 'rgba(200,255,140,0.55)';
      ctx.beginPath(); ctx.arc(screenX, centerY, size * 0.38, 0, Math.PI * 2); ctx.fill();
      ctx.shadowBlur = 0;
    } else if (spellId === 'arcane') {
      ctx.shadowBlur = 10 * RS; ctx.shadowColor = '#cc66ff'; ctx.fillStyle = 'rgba(204,102,255,0.8)';
      ctx.beginPath(); ctx.arc(screenX, centerY, size * 0.6, 0, Math.PI * 2); ctx.fill();
      ctx.shadowBlur = 0;
    } else if (spellId === 'tower') {
      // Golden bolt with amber glow
      ctx.shadowBlur = 12 * RS; ctx.shadowColor = '#ffd54f';
      ctx.fillStyle = '#ffe082';
      ctx.beginPath(); ctx.arc(screenX, centerY, size * 0.5, 0, Math.PI * 2); ctx.fill();
      ctx.fillStyle = '#fff8e1';
      ctx.beginPath(); ctx.arc(screenX, centerY, size * 0.25, 0, Math.PI * 2); ctx.fill();
      ctx.shadowBlur = 0;
    } else if (spellId === 'missile') {
      // Comet tail — gradient streak behind the orb
      var mTLen3d = size * 2.6;
      var mTx0_3d = screenX - Math.cos(p.ang) * mTLen3d;
      var mTy0_3d = centerY - Math.sin(p.ang) * mTLen3d;
      var mGrad3d = ctx.createLinearGradient(mTx0_3d, mTy0_3d, screenX, centerY);
      mGrad3d.addColorStop(0, 'rgba(77,182,255,0)');
      mGrad3d.addColorStop(1, 'rgba(100,200,255,0.6)');
      ctx.strokeStyle = mGrad3d; ctx.lineWidth = size * 0.65; ctx.lineCap = 'round';
      ctx.shadowBlur = 10 * RS; ctx.shadowColor = '#4db6ff';
      ctx.beginPath(); ctx.moveTo(mTx0_3d, mTy0_3d); ctx.lineTo(screenX, centerY); ctx.stroke();
      // Bright orb
      ctx.fillStyle = '#88ccff';
      ctx.beginPath(); ctx.arc(screenX, centerY, size * 0.52, 0, Math.PI * 2); ctx.fill();
      ctx.fillStyle = '#e8f8ff';
      ctx.beginPath(); ctx.arc(screenX, centerY, size * 0.23, 0, Math.PI * 2); ctx.fill();
      ctx.shadowBlur = 0;
    } else {
      ctx.shadowBlur = 5 * RS; ctx.shadowColor = p.spell.color;
      ctx.fillStyle = (p.spell && p.spell.color) || '#ffd54f';
      ctx.beginPath(); ctx.arc(screenX, centerY, size / 2, 0, Math.PI * 2); ctx.fill();
      ctx.shadowBlur = 0;
    }
    });
    ctx.restore();
  }
}

function drawImpacts2D() {
  if (!impacts || !impacts.length) return;
  var now = Date.now();
  ctx.save(); ctx.translate(-viewCam.x, -viewCam.y);
  for (var i = 0; i < impacts.length; i++) {
    var im = impacts[i];
    var t = Math.max(0, Math.min(1, (now - im.spawnMs) / im.lifeMs));
    ctx.globalAlpha = 1.0 - t; ctx.strokeStyle = 'rgba(255,212,79,0.9)'; ctx.lineWidth = 2;
    ctx.beginPath(); ctx.arc(im.x, im.y, 4 + 18 * t, 0, Math.PI * 2); ctx.stroke();
  }
  ctx.restore(); ctx.globalAlpha = 1.0;
}

function drawImpacts3D() {
  if(!impacts || !impacts.length)return;
  var C=getCam3D(),now=Date.now();
  for(var i=0;i<impacts.length;i++) {
      var im=impacts[i],z=Number.isFinite(im.z)?im.z:getEntityRenderFloorZ(im);
      if (typeof CASTING_ART_ENABLED !== 'undefined' && CASTING_ART_ENABLED &&
          typeof renderMissileImpactArt === 'function' && renderMissileImpactArt(im,C,now)) continue;
      // Every producer stores absolute render-world Z, including companions
      // and synergy bursts. Do not re-add their support or apply a second lift.
      var vis=entityVisible3D(im.x,im.y,z,C,{maxDist:viewDist,sceneDepth:true,fadeFraction:1});
      if(!vis)continue;
      var fwd = vis.fwd, centerY=vis.sy;
      var t = Math.max(0, Math.min(1, (now - im.spawnMs) / im.lifeMs));

      if (im.isCompanionProj) {
        // Companion projectile — glowing orb with trail
        var sz = Math.max(5, Math.min(22, Math.floor(320 * getScale3D('smProjectile') / fwd)));
        drawSpellBillboard(vis,sz,10,function(){
        ctx.save();
        ctx.globalAlpha = 0.9;
        ctx.shadowBlur = 10; ctx.shadowColor = im.color || '#44dd55';
        ctx.fillStyle = im.color || '#44dd55';
        ctx.beginPath(); ctx.arc(vis.sx, centerY, sz, 0, Math.PI * 2); ctx.fill();
        ctx.fillStyle = 'rgba(255,255,255,0.5)';
        ctx.beginPath(); ctx.arc(vis.sx, centerY, sz * 0.5, 0, Math.PI * 2); ctx.fill();
        ctx.shadowBlur = 0;
        ctx.restore();
        });
      } else {
        // Normal impact — expanding ring
        var size = Math.max(10, Math.min(50, Math.floor(510 * getScale3D('projectile') / fwd)));
        var r = (size * 0.5) + 10 * t;
        drawSpellBillboard(vis,r+2,0,function(){
        ctx.save(); ctx.globalAlpha = 1.0 - t; ctx.strokeStyle = 'rgba(255,212,79,0.95)'; ctx.lineWidth = 2;
        ctx.beginPath(); ctx.arc(vis.sx, centerY, r, 0, Math.PI * 2); ctx.stroke(); ctx.restore();
        });
      }
  }
}

// ── Ground Effects Rendering ──────────────────────────────────────────
function drawGroundEffects2D() {
  if (!groundEffects || !groundEffects.length) return;
  var now = Date.now();
  ctx.save(); ctx.translate(-viewCam.x, -viewCam.y);
  for (var i = 0; i < groundEffects.length; i++) {
    var ge = groundEffects[i];
    var age = now - ge.spawnMs;
    var t = Math.min(1, age / ge.duration);
    var fadeIn = Math.min(1, age / 200);
    var fadeOut = t > 0.7 ? 1.0 - (t - 0.7) / 0.3 : 1.0;
    ctx.globalAlpha = 0.5 * fadeIn * fadeOut;
    if (ge.spellId === 'fire') {
      // Flickering orange-red
      var flicker = 0.8 + 0.2 * Math.sin(now * 0.01 + i * 3);
      ctx.fillStyle = 'rgba(255,' + Math.floor(80 * flicker) + ',0,0.6)';
      ctx.beginPath(); ctx.arc(ge.x, ge.y, ge.radius * flicker, 0, Math.PI * 2); ctx.fill();
      ctx.fillStyle = 'rgba(255,200,0,0.3)';
      ctx.beginPath(); ctx.arc(ge.x, ge.y, ge.radius * 0.6 * flicker, 0, Math.PI * 2); ctx.fill();
    } else if (ge.spellId === 'ice') {
      ctx.fillStyle = 'rgba(0,200,255,0.3)';
      ctx.beginPath(); ctx.arc(ge.x, ge.y, ge.radius, 0, Math.PI * 2); ctx.fill();
      ctx.strokeStyle = 'rgba(150,240,255,0.5)'; ctx.lineWidth = 1.5;
      ctx.stroke();
    } else if (ge.spellId === 'poison') {
      var bubble = 0.9 + 0.1 * Math.sin(now * 0.008 + i * 5);
      ctx.shadowBlur = 14; ctx.shadowColor = '#88ff44';
      // Main cloud body
      ctx.fillStyle = 'rgba(80,200,40,0.38)';
      ctx.beginPath(); ctx.arc(ge.x, ge.y, ge.radius * bubble, 0, Math.PI * 2); ctx.fill();
      // Offset wisps give it a billow shape
      ctx.fillStyle = 'rgba(120,255,60,0.22)';
      ctx.beginPath(); ctx.arc(ge.x + ge.radius * 0.32, ge.y - ge.radius * 0.22, ge.radius * 0.52, 0, Math.PI * 2); ctx.fill();
      ctx.beginPath(); ctx.arc(ge.x - ge.radius * 0.28, ge.y + ge.radius * 0.18, ge.radius * 0.44, 0, Math.PI * 2); ctx.fill();
      // Dim outer glow ring
      ctx.strokeStyle = 'rgba(100,255,60,0.28)'; ctx.lineWidth = 1.5;
      ctx.beginPath(); ctx.arc(ge.x, ge.y, ge.radius, 0, Math.PI * 2); ctx.stroke();
      ctx.shadowBlur = 0;
    }
  }
  ctx.restore(); ctx.globalAlpha = 1.0;
}

function drawGroundEffects3D() {
  if (!groundEffects || !groundEffects.length) return;
  var now = Date.now();
  var C = getCam3D();
  for (var i = 0; i < groundEffects.length; i++) {
    var ge = groundEffects[i];
    var dx = ge.x - cam.x, dy = ge.y - cam.y;
    var dist = Math.hypot(dx, dy);
    if (dist-ge.radius > 500) continue;
    var age = now - ge.spawnMs;
    var t = Math.min(1, age / ge.duration);
    var fadeOut = t > 0.7 ? 1.0 - (t - 0.7) / 0.3 : 1.0;
    var fadeIn=Math.min(1,age/200);
    ctx.save(); ctx.globalAlpha = 0.5 * fadeOut*fadeIn;
    if (ge.spellId === 'fire') {
      var flicker = 0.8 + 0.2 * Math.sin(now * 0.01 + i * 3);
      ctx.fillStyle = 'rgba(255,' + Math.floor(80 * flicker) + ',0,0.6)';
      drawSpellGroundPolygon(ge,spellDiscPoints(ge.x,ge.y,ge.radius*flicker,32),C);
      ctx.fillStyle='rgba(255,200,0,0.3)';
      drawSpellGroundPolygon(ge,spellDiscPoints(ge.x,ge.y,ge.radius*0.6*flicker,24),C);
    } else if (ge.spellId === 'ice') {
      ctx.fillStyle = 'rgba(0,200,255,0.35)';
      drawSpellGroundPolygon(ge,spellDiscPoints(ge.x,ge.y,ge.radius,32),C);
      ctx.strokeStyle = 'rgba(150,240,255,0.4)'; ctx.lineWidth = 1;
      drawSpellGroundRing(ge,ge.x,ge.y,ge.radius,C,1,0);
    } else if (ge.spellId === 'poison') {
      var bubble = 0.9 + 0.1 * Math.sin(now * 0.008 + i * 5);
      ctx.shadowColor = '#88ff44';
      // Main cloud body
      ctx.fillStyle = 'rgba(80,200,40,0.38)';
      drawSpellGroundPolygon(ge,spellDiscPoints(ge.x,ge.y,ge.radius*bubble,32),C);
      // Side wisps for billow shape
      ctx.fillStyle = 'rgba(120,230,60,0.22)';
      drawSpellGroundPolygon(ge,spellDiscPoints(ge.x-ge.radius*0.28,ge.y,ge.radius*0.58,24),C);
      drawSpellGroundPolygon(ge,spellDiscPoints(ge.x+ge.radius*0.28,ge.y,ge.radius*0.52,24),C);
      // Glow ring outline
      ctx.strokeStyle = 'rgba(100,255,60,0.22)'; ctx.lineWidth = 1;
      ctx.shadowBlur=10;
      drawSpellGroundRing(ge,ge.x,ge.y,ge.radius,C,1,10);
      ctx.shadowBlur = 0;
    }
    ctx.restore();
  }
}

// ── Chain Lightning Arcs ──────────────────────────────────────────────
function drawChainEffects2D() {
  if (!chainEffects || !chainEffects.length) return;
  var now = Date.now();
  ctx.save(); ctx.translate(-viewCam.x, -viewCam.y);
  var alive = [];
  for (var i = 0; i < chainEffects.length; i++) {
    var ce = chainEffects[i];
    var age = now - ce.spawnMs;
    if (age > ce.lifeMs) continue;
    alive.push(ce);
    var t = age / ce.lifeMs;
    ctx.globalAlpha = 1.0 - t;
    ctx.strokeStyle = '#ffff00'; ctx.lineWidth = 2; ctx.lineCap = 'round';
    for (var j = 0; j < ce.targets.length; j++) {
      var tgt = ce.targets[j];
      drawLightningBoltBetween(ce.fromX, ce.fromY, tgt.x, tgt.y);
    }
  }
  chainEffects = alive;
  ctx.restore(); ctx.globalAlpha = 1.0;
}

function drawChainEffects3D() {
  if (!chainEffects || !chainEffects.length) return;
  var now = Date.now();
  var C = getCam3D();
  var alive = [];
  for (var i = 0; i < chainEffects.length; i++) {
    var ce = chainEffects[i];
    var age = now - ce.spawnMs;
    if (age > ce.lifeMs) continue;
    alive.push(ce);
    var t = age / ce.lifeMs;
    ctx.save(); ctx.globalAlpha = 1.0 - t;
    ctx.strokeStyle = '#ffff00'; ctx.lineWidth = 2; ctx.lineCap = 'round';
    var fromZ=Number.isFinite(ce.fromZ)?ce.fromZ:spellFloorRenderZ(ce,ce.fromX,ce.fromY)+30;
    var from={x:ce.fromX,y:ce.fromY,z:fromZ};
    for (var j = 0; j < ce.targets.length; j++) {
      var tgt = ce.targets[j];
      var toZ=Number.isFinite(tgt.z)?tgt.z:spellFloorRenderZ(ce,tgt.x,tgt.y)+30;
      var to={x:tgt.x,y:tgt.y,z:toZ},distance=Math.hypot(to.x-from.x,to.y-from.y);
      var count=Math.max(3,Math.min(32,Math.ceil(distance/16))),previous=from;
      for(var seg=1;seg<=count;seg++) {
        var along=seg/count,jitter=seg===count?0:(Math.random()-0.5)*9;
        var point={x:from.x+(to.x-from.x)*along-(to.y-from.y)/(distance||1)*jitter,
          y:from.y+(to.y-from.y)*along+(to.x-from.x)/(distance||1)*jitter,
          z:from.z+(to.z-from.z)*along+(seg===count?0:(Math.random()-0.5)*6)};
        drawSpellWorldSegment(previous,point,C,8,14,function(pa,pb){
          ctx.shadowBlur=14;ctx.shadowColor='#ffff00';ctx.strokeStyle='rgba(255,230,60,0.35)';ctx.lineWidth=8;
          ctx.beginPath();ctx.moveTo(pa.x,pa.y);ctx.lineTo(pb.x,pb.y);ctx.stroke();
          ctx.shadowBlur=6;ctx.shadowColor='#ffffa0';ctx.strokeStyle='#ffffff';ctx.lineWidth=1.4;
          ctx.beginPath();ctx.moveTo(pa.x,pa.y);ctx.lineTo(pb.x,pb.y);ctx.stroke();
        });
        previous=point;
      }
    }
    ctx.restore();
  }
  chainEffects = alive;
}

function worldToScreen3D(wx, wy, wz, cam2, halfFov, w, h, horizon, pitchOff) {
  var dx = wx - cam2.x, dy = wy - cam2.y;
  var dist = Math.hypot(dx, dy);
  if (dist < 1 || dist > 600) return null;
  var cosA = Math.cos(cam2.ang), sinA = Math.sin(cam2.ang);
  var fwd = dx * cosA + dy * sinA;
  if (fwd < 1) return null;
  var rgt = dx * (-sinA) + dy * cosA;
  var invTanHalf = 1 / Math.tan(halfFov);
  var screenX = Math.floor((rgt / fwd * invTanHalf * 0.5 + 0.5) * w);
  var cameraZ = 60 + ((Number.isFinite(cam2.z) ? cam2.z : 60) - 60) * (25 / 40);
  var horizonY = Math.floor(h * 0.5) + pitchOff;
  var screenY = horizonY + Math.floor(((cameraZ - wz) / fwd) * projScale);
  return {x:screenX, y:screenY};
}

function drawLightningBoltBetween(x1, y1, x2, y2) {
  var dx = x2 - x1, dy = y2 - y1;
  var dist = Math.hypot(dx, dy);
  var segments = Math.max(3, Math.floor(dist / 12));
  // Build path with random jitter
  var pts = [{x:x1, y:y1}];
  for (var s = 1; s < segments; s++) {
    var t = s / segments;
    pts.push({x: x1 + dx * t + (Math.random() - 0.5) * 14,
              y: y1 + dy * t + (Math.random() - 0.5) * 14});
  }
  pts.push({x:x2, y:y2});
  var prevLW = ctx.lineWidth;
  // Outer glow
  ctx.save();
  ctx.shadowBlur = 14; ctx.shadowColor = '#ffff00';
  ctx.strokeStyle = 'rgba(255,230,60,0.35)';
  ctx.lineWidth = prevLW * 4;
  ctx.beginPath(); ctx.moveTo(pts[0].x, pts[0].y);
  for (var pi = 1; pi < pts.length; pi++) ctx.lineTo(pts[pi].x, pts[pi].y);
  ctx.stroke();
  // Bright white core
  ctx.strokeStyle = '#ffffff';
  ctx.lineWidth = Math.max(1, prevLW * 0.7);
  ctx.shadowBlur = 6; ctx.shadowColor = '#ffffa0';
  ctx.beginPath(); ctx.moveTo(pts[0].x, pts[0].y);
  for (var pi2 = 1; pi2 < pts.length; pi2++) ctx.lineTo(pts[pi2].x, pts[pi2].y);
  ctx.stroke();
  ctx.restore();
}

// ── Nova Effects (expanding rings) ────────────────────────────────────
function drawNovaEffects2D() {
  if (!novaEffects || !novaEffects.length) return;
  var now = Date.now();
  ctx.save(); ctx.translate(-viewCam.x, -viewCam.y);
  var alive = [];
  for (var i = 0; i < novaEffects.length; i++) {
    var ne = novaEffects[i];
    var age = now - ne.spawnMs;
    if (age > ne.lifeMs) continue;
    alive.push(ne);
    var t = age / ne.lifeMs;
    var r = ne.radius * t;
    ctx.shadowBlur = 22; ctx.shadowColor = ne.color;
    // Outer ring
    ctx.globalAlpha = 0.8 * (1.0 - t);
    ctx.strokeStyle = ne.color; ctx.lineWidth = 4;
    ctx.beginPath(); ctx.arc(ne.x, ne.y, r, 0, Math.PI * 2); ctx.stroke();
    // Mid ring (slightly lagging)
    var tMid = Math.min(1, t / 0.7);
    var rMid = ne.radius * 0.7 * tMid;
    ctx.globalAlpha = 0.55 * (1.0 - t);
    ctx.lineWidth = 2;
    ctx.beginPath(); ctx.arc(ne.x, ne.y, rMid, 0, Math.PI * 2); ctx.stroke();
    // Filled glow interior
    ctx.fillStyle = ne.color; ctx.globalAlpha = 0.12 * (1.0 - t);
    ctx.beginPath(); ctx.arc(ne.x, ne.y, r, 0, Math.PI * 2); ctx.fill();
    // Central flash — bright burst that fades out in first 35% of lifetime
    if (t < 0.35) {
      var flashT = t / 0.35;
      ctx.globalAlpha = 0.95 * (1.0 - flashT);
      ctx.fillStyle = '#ffffff';
      ctx.beginPath(); ctx.arc(ne.x, ne.y, ne.radius * 0.20 * (1 - flashT * 0.55), 0, Math.PI * 2); ctx.fill();
      ctx.fillStyle = ne.color;
      ctx.globalAlpha = 0.65 * (1.0 - flashT);
      ctx.beginPath(); ctx.arc(ne.x, ne.y, ne.radius * 0.38 * (1 - flashT * 0.3), 0, Math.PI * 2); ctx.fill();
    }
    // 8 radial spark dots riding the expanding ring
    for (var nsp = 0; nsp < 8; nsp++) {
      var nsAng = (nsp / 8) * Math.PI * 2 + t * 1.1;
      var nsx = ne.x + Math.cos(nsAng) * r;
      var nsy = ne.y + Math.sin(nsAng) * r;
      ctx.globalAlpha = 0.9 * (1.0 - t) * Math.max(0, 1 - t * 1.5);
      ctx.fillStyle = '#ffffff';
      ctx.beginPath(); ctx.arc(nsx, nsy, 3, 0, Math.PI * 2); ctx.fill();
    }
    ctx.shadowBlur = 0;
  }
  novaEffects = alive;
  ctx.restore(); ctx.globalAlpha = 1.0;
}

function drawNovaEffects3D() {
  if (!novaEffects || !novaEffects.length) return;
  var now = Date.now();
  var C = getCam3D();
  var alive = [];
  for (var i = 0; i < novaEffects.length; i++) {
    var ne = novaEffects[i];
    var age = now - ne.spawnMs;
    if (age > ne.lifeMs) continue;
    alive.push(ne);
    var t = age / ne.lifeMs;
    var dx = ne.x - cam.x, dy = ne.y - cam.y;
    var dist = Math.hypot(dx, dy);
    if (dist-ne.radius > 500) continue;
    var radius=ne.radius*t;
    ctx.save();
    ctx.shadowBlur = 18; ctx.shadowColor = ne.color;
    // World-space rings cross the near plane and hills one segment at a time.
    ctx.globalAlpha = 0.75 * (1.0 - t);
    ctx.strokeStyle = ne.color; ctx.lineWidth = 3;
    drawSpellGroundRing(ne,ne.x,ne.y,radius,C,3,18);
    // Mid ring
    var tMid3d = Math.min(1, t / 0.7);
    ctx.globalAlpha = 0.5 * (1.0 - t) * tMid3d;
    ctx.lineWidth = 2;
    drawSpellGroundRing(ne,ne.x,ne.y,radius*0.68,C,2,18);
    // Filled interior glow
    ctx.shadowBlur=0;ctx.fillStyle = ne.color; ctx.globalAlpha = 0.10 * (1.0 - t);
    drawSpellGroundPolygon(ne,spellDiscPoints(ne.x,ne.y,radius,40),C);
    // Central flash
    if (t < 0.35) {
      var ft3d = t / 0.35;
      var flash=projToScreen(ne.x,ne.y,spellFloorRenderZ(ne,ne.x,ne.y)+2,C);
      if(flash) {
        var fR3d=Math.max(3,ne.radius*0.20*projScale/flash.fwd*(1-ft3d*0.5));
        drawSpellBillboard(flash,fR3d*1.9,18,function(){
          ctx.shadowBlur=18;ctx.globalAlpha=0.9*(1-ft3d);ctx.fillStyle='#ffffff';
          ctx.beginPath();ctx.arc(flash.sx,flash.sy,fR3d,0,Math.PI*2);ctx.fill();
          ctx.globalAlpha=0.6*(1-ft3d);ctx.fillStyle=ne.color;
          ctx.beginPath();ctx.arc(flash.sx,flash.sy,fR3d*1.9,0,Math.PI*2);ctx.fill();
        });
      }
    }
    ctx.shadowBlur = 0;
    ctx.restore();
  }
  novaEffects = alive;
}

function drawConeEffects2D() {
  if (!coneEffects || !coneEffects.length) return;
  var now = Date.now();
  ctx.save(); ctx.translate(-viewCam.x, -viewCam.y);
  for (var i = 0; i < coneEffects.length; i++) {
    var ce = coneEffects[i];
    var t = Math.max(0, Math.min(1, (now - ce.spawnMs) / ce.lifeMs));
    var isFire = (ce.spellId === 'fire');
    if (isFire) {
      ctx.shadowBlur = 15; ctx.shadowColor = '#ff6600';
      var particles = 8;
      for (var p = 0; p < particles; p++) {
        var pct = p / particles;
        var dist = ce.range * (0.2 + 0.8 * pct);
        var spread = ce.halfAngle * 2 * pct;
        var pAng = ce.ang + (Math.random() - 0.5) * spread;
        var px = ce.x + Math.cos(pAng) * dist;
        var py = ce.y + Math.sin(pAng) * dist;
        var size = 8 * (1 - pct) * (1 - t);
        ctx.globalAlpha = (0.6 - 0.4 * pct) * (1.0 - t);
        ctx.fillStyle = (p % 2 === 0) ? '#ff6600' : '#ffaa00';
        ctx.beginPath(); ctx.arc(px, py, size, 0, Math.PI * 2); ctx.fill();
      }
      ctx.shadowBlur = 0;
    } else {
      ctx.globalAlpha = 0.4 * (1.0 - t); ctx.fillStyle = ce.color;
      ctx.beginPath(); ctx.moveTo(ce.x, ce.y);
      var leftAng = ce.ang - ce.halfAngle;
      var rightAng = ce.ang + ce.halfAngle;
      ctx.arc(ce.x, ce.y, ce.range, leftAng, rightAng);
      ctx.closePath(); ctx.fill();
      ctx.globalAlpha = 0.7 * (1.0 - t); ctx.strokeStyle = ce.color; ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(ce.x, ce.y); ctx.lineTo(ce.x + Math.cos(leftAng) * ce.range, ce.y + Math.sin(leftAng) * ce.range);
      ctx.moveTo(ce.x, ce.y); ctx.lineTo(ce.x + Math.cos(rightAng) * ce.range, ce.y + Math.sin(rightAng) * ce.range);
      ctx.stroke();
    }
  }
  ctx.restore(); ctx.globalAlpha = 1.0;
}

function drawConeEffects3D() {
  if (!coneEffects || !coneEffects.length) return;
  var C = getCam3D();
  var now = Date.now();
  for (var i = 0; i < coneEffects.length; i++) {
    var ce = coneEffects[i];
    var t = Math.max(0, Math.min(1, (now - ce.spawnMs) / ce.lifeMs));
    var isFire = (ce.spellId === 'fire');
    if (isFire) {
      ctx.save(); ctx.shadowBlur = 15; ctx.shadowColor = '#ff6600';
      var particles = 20;
      for (var p = 0; p < particles; p++) {
        var pct = p / particles;
        var dist = ce.range * (0.1 + 0.9 * pct);
        var spread = ce.halfAngle * 2 * pct;
        var pAng = ce.ang + (Math.random() - 0.5) * spread;
        var px = ce.x + Math.cos(pAng) * dist;
        var py = ce.y + Math.sin(pAng) * dist;
        var floorZ=spellFloorRenderZ(ce,px,py);
        var pt = Number.isFinite(floorZ)?projToScreen(px,py,floorZ+8,C):null;
        if (!pt || pt.fwd > 300) continue;
        var size = Math.max(6, Math.min(24, Math.floor(220 / pt.fwd))) * (1 - pct * 0.5);
        ctx.globalAlpha = (0.7 - 0.4 * pct) * (1.0 - t);
        var hue = p % 3;
        ctx.fillStyle = (hue === 0) ? '#ff6600' : (hue === 1 ? '#ffaa00' : '#ff8800');
        drawSpellBillboard(pt,size,15,function(){
          ctx.beginPath(); ctx.arc(pt.sx, pt.sy, size, 0, Math.PI * 2); ctx.fill();
        });
      }
      ctx.restore();
    } else {
      ctx.save(); ctx.shadowBlur = 10; ctx.shadowColor = '#00ffff';
      var leftAng = ce.ang - ce.halfAngle;
      var rightAng = ce.ang + ce.halfAngle;
      var rays = 8;
      for (var r = 0; r < rays; r++) {
        var rayAng = leftAng + (rightAng - leftAng) * (r / (rays - 1));
        ctx.globalAlpha = 0.5 * (1.0 - t);
        ctx.strokeStyle = 'rgba(0,255,255,0.8)'; ctx.lineWidth = 3;
        var steps = Math.max(6,Math.ceil(ce.range/10)),previous=null;
        for (var s = 0; s <= steps; s++) {
          var dist = ce.range * (s / steps);
          var rpx = ce.x + Math.cos(rayAng) * dist;
          var rpy = ce.y + Math.sin(rayAng) * dist;
          var rz=spellFloorRenderZ(ce,rpx,rpy);
          var rpt=Number.isFinite(rz)?{x:rpx,y:rpy,z:rz+1}:null;
          if(previous && rpt)strokeSpellWorldSegment(previous,rpt,C,3,10);
          previous=rpt;
        }
      }
      ctx.globalAlpha = 0.2 * (1.0 - t); ctx.fillStyle = 'rgba(0,255,255,0.3)';
      ctx.shadowBlur=0;
      var arcSteps = 16,footprint=[{x:ce.x,y:ce.y}];
      for (var s = 0; s <= arcSteps; s++) {
        var a = leftAng + (rightAng - leftAng) * (s / arcSteps);
        var apx = ce.x + Math.cos(a) * ce.range;
        var apy = ce.y + Math.sin(a) * ce.range;
        footprint.push({x:apx,y:apy});
      }
      drawSpellGroundPolygon(ce,footprint,C);
      ctx.restore();
    }
  }
}

// Flame stream renderer — persistent beam while flameStreamActive
function drawFlameStream2D() {
  if (Date.now() - flameStreamLastTick > 180) return;
  var now = Date.now();
  var rampT = Math.min(1, (now - flameStreamStartMs) / 300);
  var ang = flameStreamAng;
  var range = flameStreamRange * rampT;
  var spell = spells.fire;
  var halfWidth = spell.streamWidth || 0.40;
  if (spell.tier >= 2) halfWidth = 0.55;
  var coneSpread = halfWidth;

  // Origin: wizard's casting hand
  var cosA = Math.cos(ang), sinA = Math.sin(ang);
  var rightCos = Math.cos(ang + Math.PI * 0.5), rightSin = Math.sin(ang + Math.PI * 0.5);
  var originX = pos.x + cosA * 12 + rightCos * 5;
  var originY = pos.y + sinA * 12 + rightSin * 5;

  ctx.save(); ctx.translate(-viewCam.x, -viewCam.y);
  ctx.shadowBlur = 20; ctx.shadowColor = '#ff6600';
  var segments = 12;
  for (var s = 0; s < segments; s++) {
    var t0 = s / segments, t1 = (s + 1) / segments;
    var d0 = range * t0, d1 = range * t1;
    var fanT0 = t0 * t0, fanT1 = t1 * t1;
    var w0 = 2 + d0 * Math.tan(coneSpread) * (0.3 + 0.7 * fanT0);
    var w1 = 2 + d1 * Math.tan(coneSpread) * (0.3 + 0.7 * fanT1);
    var tMid = (t0 + t1) * 0.5;
    var alpha = 0.7 * (1 - tMid * tMid) * rampT;
    if (alpha < 0.02) continue;
    ctx.globalAlpha = alpha;
    // Color gradient: white-yellow → orange → red
    var r, g, b;
    if (tMid < 0.2) { r = 255; g = 240; b = 180; }
    else if (tMid < 0.5) { r = 255; g = Math.floor(180 - (tMid - 0.2) * 500); b = 0; }
    else { r = Math.floor(255 - (tMid - 0.5) * 300); g = Math.floor(30 - (tMid - 0.5) * 50); b = 0; }
    ctx.fillStyle = rgbQ(Math.max(20, r), Math.max(0, g), Math.max(0, b));

    var px0L = originX + cosA * d0 + (-sinA) * w0;
    var py0L = originY + sinA * d0 + cosA * w0;
    var px0R = originX + cosA * d0 - (-sinA) * w0;
    var py0R = originY + sinA * d0 - cosA * w0;
    var px1L = originX + cosA * d1 + (-sinA) * w1;
    var py1L = originY + sinA * d1 + cosA * w1;
    var px1R = originX + cosA * d1 - (-sinA) * w1;
    var py1R = originY + sinA * d1 - cosA * w1;
    ctx.beginPath();
    ctx.moveTo(px0L, py0L); ctx.lineTo(px1L, py1L);
    ctx.lineTo(px1R, py1R); ctx.lineTo(px0R, py0R);
    ctx.closePath(); ctx.fill();
  }

  // Ember particles scattered across the cone
  var numP = 16;
  for (var p = 0; p < numP; p++) {
    var seed = (p * 7919 + Math.floor(now * 0.01)) % 100;
    var pct = (p + ((now * 0.005) % 1)) / numP;
    if (pct > 1) pct -= 1;
    var dist = range * (0.05 + 0.95 * pct);
    var spreadAng = coneSpread * pct * 1.5;
    var pAng = ang + (((seed % 50) / 50) - 0.5) * spreadAng * 2;
    var px = originX + Math.cos(pAng) * dist;
    var py = originY + Math.sin(pAng) * dist;
    var size = (10 - 7 * pct) * rampT;
    ctx.globalAlpha = (0.85 - 0.8 * pct) * rampT;
    if (ctx.globalAlpha < 0.03) continue;
    var hue = (seed + Math.floor(now * 0.01)) % 4;
    ctx.fillStyle = hue === 0 ? '#ffffaa' : hue === 1 ? '#ffcc44' : hue === 2 ? '#ff8800' : '#ff4400';
    ctx.beginPath(); ctx.arc(px, py, size, 0, Math.PI * 2); ctx.fill();
  }

  ctx.shadowBlur = 0;
  ctx.restore(); ctx.globalAlpha = 1.0;
}

function drawFlameStream3D() {
  // Only render if a tick fired recently (within 2 tick intervals)
  if (Date.now() - flameStreamLastTick > 180) return;
  var now = Date.now();
  var C = getCam3D();
  var rampT = Math.min(1, (now - flameStreamStartMs) / 300);
  var ang = flameStreamAng;
  var range = flameStreamRange * rampT;
  var spell = spells.fire;
  var halfWidth = spell.streamWidth || 0.40;
  if (spell.tier >= 2) halfWidth = 0.55;
  var coneSpread = halfWidth;

  // Origin: wizard's casting hand (forward + slightly right of facing)
  var handFwd = 14, handRight = 6;
  var cosA = Math.cos(ang), sinA = Math.sin(ang);
  var rightCos = Math.cos(ang + Math.PI * 0.5), rightSin = Math.sin(ang + Math.PI * 0.5);
  var originX = pos.x + cosA * handFwd + rightCos * handRight;
  var originY = pos.y + sinA * handFwd + rightSin * handRight;
  // Hand height in renderer Z: convert from game floorZ space to renderer space
  var handZ = ((Number.isFinite(pos.floorZ) ? pos.floorZ : 60) - 60) * (25 / 40) + 55;
  var flameGround={x:pos.x,y:pos.y,renderFloorZ:getPlayerFloorH()*25,underground:playerUnderground};

  function worldPt(px, py, t) {
    var floorZ = spellFloorRenderZ(flameGround,px,py);
    if(!Number.isFinite(floorZ))return null;
    // Near origin: at hand height. At distance: descends to floor level.
    // Smooth lerp so the flame arcs down naturally
    var zz = handZ * (1 - t * t) + (floorZ+1) * (t * t);
    return {x:px,y:py,z:zz};
  }

  ctx.save();

  // Draw fanning cone segments — widens dramatically with distance
  var segments = 20;
  var prevL = null, prevR = null;

  for (var s = 0; s <= segments; s++) {
    var t = s / segments;
    var dist = range * t;
    var cx2 = originX + cosA * dist;
    var cy2 = originY + sinA * dist;
    // Fan outward: narrow at hand, wide cone at end
    var fanT = t * t;
    var w = (1 + dist * Math.tan(coneSpread) * (0.3 + 0.7 * fanT)) * rampT;
    var perpX = -sinA * w, perpY = cosA * w;

    var ptL = worldPt(cx2 + perpX, cy2 + perpY, t);
    var ptR = worldPt(cx2 - perpX, cy2 - perpY, t);

    if (prevL && prevR && ptL && ptR) {
      // Alpha: bright at hand, fading to nearly transparent at tip
      var alpha = (0.80 * (1 - t * t)) * rampT;
      if (alpha < 0.02) { prevL = ptL; prevR = ptR; continue; }
      ctx.globalAlpha = alpha;

      // Color: white-yellow core near hand → orange → dim red at edges
      var r, g, b;
      if (t < 0.15) {
        r = 255; g = Math.floor(250 - t * 500); b = Math.floor(200 - t * 1000);
      } else if (t < 0.45) {
        r = 255; g = Math.floor(180 - (t - 0.15) * 450); b = 0;
      } else if (t < 0.75) {
        r = Math.floor(255 - (t - 0.45) * 300); g = Math.floor(45 - (t - 0.45) * 100); b = 0;
      } else {
        r = Math.floor(165 - (t - 0.75) * 400); g = Math.floor(15 - (t - 0.75) * 50); b = 0;
      }
      ctx.fillStyle = rgbQ(Math.max(20, r), Math.max(0, g), Math.max(0, b));

      // A fanning quad may be nonplanar on terrain. Its two triangles retain
      // forward depth at all vertices and clip independently across the mouth.
      var flameA=projectSceneWorldPolygon([prevL,ptL,ptR],C);
      var flameB=projectSceneWorldPolygon([prevL,ptR,prevR],C);
      withSceneDepthClip(flameA,function(){traceSceneDepthPolygon(flameA);ctx.fill();});
      withSceneDepthClip(flameB,function(){traceSceneDepthPolygon(flameB);ctx.fill();});
    }
    prevL = ptL; prevR = ptR;
  }

  // Animated ember particles scattered across the cone
  ctx.shadowBlur = 10; ctx.shadowColor = '#ff6600';
  var numP = 24;
  for (var p = 0; p < numP; p++) {
    var seed = (p * 7919 + Math.floor(now * 0.015)) % 100;
    var pct = (p + ((now * 0.006) % 1)) / numP;
    if (pct > 1) pct -= 1;
    var dist2 = range * (0.03 + 0.97 * pct);
    var spreadAtDist = coneSpread * pct * 1.5;
    var pAng = ang + (((seed % 50) / 50) - 0.5) * spreadAtDist * 2;
    var px = originX + Math.cos(pAng) * dist2;
    var py = originY + Math.sin(pAng) * dist2;
    var world=worldPt(px,py,pct);
    var pt=world?projToScreen(world.x,world.y,world.z,C):null;
    if (!pt || pt.fwd > 400 || pt.fwd < 1) continue;
    // Particles shrink and fade with distance
    var size = Math.max(2, Math.min(16, Math.floor(160 / pt.fwd))) * (1.2 - pct * 0.8) * rampT;
    var pAlpha = (0.9 - 0.85 * pct) * rampT; // strong falloff
    if (pAlpha < 0.03) continue;
    ctx.globalAlpha = pAlpha;
    var hue = (seed + Math.floor(now * 0.012)) % 5;
    ctx.fillStyle = hue === 0 ? '#ffffcc' : hue === 1 ? '#ffdd44' : hue === 2 ? '#ffaa00' : hue === 3 ? '#ff6600' : '#ff3300';
    drawSpellBillboard(pt,size,10,function(){
      ctx.beginPath(); ctx.arc(pt.sx, pt.sy, size, 0, Math.PI * 2); ctx.fill();
    });
  }

  ctx.shadowBlur = 0;
  ctx.restore(); ctx.globalAlpha = 1.0;
}
