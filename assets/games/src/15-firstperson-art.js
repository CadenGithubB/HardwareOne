// First-person presentation only. Combat owns the explicitly approved 120 ms
// gathering interval; this artwork follows its accepted cast/release timestamps.
// Body artwork is cached; articulated fingers remain native-resolution paths.
// Rejected flat study: retained for comparison, never the default artwork.
var CASTING_ART_ENABLED = false;
var _castingPoseState = {castAt:-1e12,spellId:'missile',cooldown:450,lastNow:0,walkPhase:0};
var _castingArtwork = null;
var _castingArtStats = {builds:0,buildMs:0,lastBuildMs:0,bytes:0,entries:0,maxBytes:8*1024*1024,fallbacks:0};

function noteFirstPersonCast(spell, now) {
  if (!spell || spell.id !== 'missile' || !Number.isFinite(now)) return;
  _castingPoseState.castAt=now;
  _castingPoseState.spellId=spell.id;
  _castingPoseState.cooldown=typeof getEffectiveCooldown==='function'?getEffectiveCooldown():450;
}
function cancelFirstPersonCast() {_castingPoseState.castAt=-1e12;}
function clearCastingArtworkCache() {
  _castingArtwork=null;
  _castingArtStats={builds:0,buildMs:0,lastBuildMs:0,bytes:0,entries:0,maxBytes:8*1024*1024,fallbacks:0};
}
function getCastingArtworkStats() { return Object.assign({},_castingArtStats); }
function castingMix(a,b,t) {
  var av=parseInt(a.slice(1),16),bv=parseInt(b.slice(1),16);
  return 'rgb('+Math.round(((av>>16)&255)*(1-t)+((bv>>16)&255)*t)+','+
    Math.round(((av>>8)&255)*(1-t)+((bv>>8)&255)*t)+','+
    Math.round((av&255)*(1-t)+(bv&255)*t)+')';
}
function castingHexMix(a,b,t) {
  var av=parseInt(a.slice(1),16),bv=parseInt(b.slice(1),16);
  var r=Math.round(((av>>16)&255)*(1-t)+((bv>>16)&255)*t);
  var g=Math.round(((av>>8)&255)*(1-t)+((bv>>8)&255)*t);
  var bl=Math.round((av&255)*(1-t)+(bv&255)*t);
  return '#'+('000000'+((r<<16)|(g<<8)|bl).toString(16)).slice(-6);
}
function castingPalette() {
  var M=typeof GAME_MATERIALS!=='undefined'?GAME_MATERIALS:{};
  var ac=equipment.robes&&equipment.robes.armColor;
  var skin=M.casterSkin?M.casterSkin.hex:{shadow:'#714b40',base:'#b98265',light:'#e2bb94',crease:'#755047',nail:'#d9b59c'};
  var cloth=M.casterCloth?M.casterCloth.hex:{deep:'#16242e',mid:'#344b58',lit:'#607a83',cuff:'#4c3c2c',linen:'#c4b493'};
  var metal=M.casterMetal?M.casterMetal.hex:{shadow:'#50422c',base:'#a78b54',light:'#dfc78d'};
  return {skin:skin,cloth:ac?{deep:ac.deep,mid:ac.mid,lit:ac.lit,cuff:ac.cuff,linen:cloth.linen}:cloth,metal:metal};
}
function castingLitPalette(p, exposure) {
  var out={skin:{},cloth:{},metal:{}};
  ['skin','cloth','metal'].forEach(function(group){
    Object.keys(p[group]).forEach(function(key){
      out[group][key]=castingHexMix('#1c273b',p[group][key],0.43+exposure*0.57);
    });
  });
  return out;
}
function castingStroke(c,color,width,points) {
  c.strokeStyle=color;c.lineWidth=width;c.beginPath();c.moveTo(points[0],points[1]);
  if(points.length===8)c.bezierCurveTo.apply(c,points.slice(2));
  else for(var i=2;i<points.length;i+=2)c.lineTo(points[i],points[i+1]);
  c.stroke();
}
function paintCastingArmBody(c,p) {
  var cloth=p.cloth,skin=p.skin,gold=p.metal;
  c.save();c.lineJoin='round';c.lineCap='round';
  // Swept forearm, generous wool folds and a separate turned linen lining.
  c.beginPath();c.moveTo(54,118);c.bezierCurveTo(47,149,32,190,45,256);
  c.lineTo(174,256);c.bezierCurveTo(164,213,131,157,111,117);c.closePath();
  var sleeve=c.createLinearGradient(43,156,157,184);
  sleeve.addColorStop(0,cloth.deep);sleeve.addColorStop(.34,cloth.mid);
  sleeve.addColorStop(.57,cloth.lit);sleeve.addColorStop(.78,cloth.mid);sleeve.addColorStop(1,cloth.deep);
  c.fillStyle=sleeve;c.fill();c.strokeStyle=cloth.deep;c.lineWidth=2;c.stroke();
  c.save();c.clip();
  c.fillStyle=cloth.deep;c.globalAlpha=.65;
  c.beginPath();c.moveTo(59,141);c.bezierCurveTo(34,207,55,228,69,258);c.lineTo(81,256);c.bezierCurveTo(63,200,55,171,70,144);c.fill();
  c.beginPath();c.moveTo(104,139);c.bezierCurveTo(94,185,124,222,134,261);c.lineTo(156,261);c.bezierCurveTo(122,198,109,170,116,146);c.fill();
  c.globalAlpha=.48;
  castingStroke(c,cloth.lit,2,[74,142,69,179,81,224,91,262]);
  castingStroke(c,cloth.lit,1.4,[113,160,111,191,148,233,149,261]);
  c.globalAlpha=.5;
  castingStroke(c,cloth.deep,1.2,[44,201,62,186,87,197,109,212]);
  castingStroke(c,cloth.lit,.8,[43,204,67,191,85,201,108,214]);
  // Stitched border follows fabric rather than a screen-aligned texture grid.
  c.globalAlpha=.72;
  for(var st=0;st<15;st++) {
    var yy=148+st*7,xx=109+(yy-148)*.44;
    castingStroke(c,gold.base,.75,[xx,yy,xx+2.5,yy+3]);
  }
  c.restore();
  // Turned lining; cuff is a fitted leather band with sewn brass filigree.
  c.fillStyle=cloth.linen;c.beginPath();c.moveTo(54,116);c.quadraticCurveTo(82,124,112,115);
  c.lineTo(116,128);c.quadraticCurveTo(84,140,50,130);c.closePath();c.fill();
  c.beginPath();c.moveTo(51,126);c.quadraticCurveTo(83,135,114,125);c.lineTo(121,147);
  c.quadraticCurveTo(83,158,48,145);c.closePath();
  var cuff=c.createLinearGradient(50,133,118,141);cuff.addColorStop(0,cloth.deep);cuff.addColorStop(.5,cloth.cuff);cuff.addColorStop(1,cloth.deep);
  c.fillStyle=cuff;c.fill();c.strokeStyle=gold.shadow;c.lineWidth=1.4;c.stroke();
  castingStroke(c,gold.base,1.2,[52,130,74,138,100,137,115,129]);
  castingStroke(c,gold.light,.8,[51,144,73,152,101,152,119,144]);
  for(var e=0;e<5;e++) {
    var ex=57+e*12,ey=139+Math.sin(e*.8)*3;
    castingStroke(c,gold.base,.9,[ex-3,ey,ex,ey-4,ex+3,ey,ex,ey+4,ex-3,ey]);
    c.fillStyle=gold.light;c.beginPath();c.arc(ex,ey,.7,0,Math.PI*2);c.fill();
  }
  // Palm, thenar volume and wrist: the fingers join behind the knuckles.
  c.beginPath();c.moveTo(53,61);c.bezierCurveTo(63,47,82,44,101,60);
  c.bezierCurveTo(115,71,109,86,103,104);c.bezierCurveTo(98,113,102,119,109,124);
  c.quadraticCurveTo(81,135,56,124);c.bezierCurveTo(66,111,58,101,50,89);
  c.bezierCurveTo(41,77,45,69,53,61);c.closePath();
  var palm=c.createLinearGradient(44,94,104,64);palm.addColorStop(0,skin.shadow);palm.addColorStop(.42,skin.base);palm.addColorStop(.8,skin.light);palm.addColorStop(1,skin.base);
  c.fillStyle=palm;c.fill();c.strokeStyle=skin.shadow;c.lineWidth=1.2;c.stroke();
  c.save();c.clip();
  c.globalAlpha=.37;c.fillStyle=skin.light;c.beginPath();c.ellipse(63,88,13,22,-.4,0,Math.PI*2);c.fill();
  c.globalAlpha=.46;castingStroke(c,skin.crease,1,[57,75,71,81,76,94,69,103]);
  c.globalAlpha=.40;castingStroke(c,skin.crease,.9,[65,78,83,69,94,77,101,78]);
  castingStroke(c,skin.crease,.7,[75,94,84,91,93,95,96,99]);
  c.globalAlpha=.33;castingStroke(c,skin.light,.8,[61,77,76,82,76,94,72,101]);
  castingStroke(c,skin.crease,.9,[65,114,76,117,89,117,98,112]);
  c.restore();
  c.restore();
}
function getCastingArmArtwork(S) {
  var p=castingPalette(),raster=Math.max(1,Math.ceil(.68*S));
  var key=[raster,p.skin.shadow,p.skin.base,p.skin.light,p.skin.crease,p.skin.nail,
    p.cloth.deep,p.cloth.mid,p.cloth.lit,p.cloth.cuff,p.cloth.linen,
    p.metal.shadow,p.metal.base,p.metal.light].join('|');
  if(_castingArtwork&&_castingArtwork.key===key)return _castingArtwork;
  var art={key:key,palette:p,shadow:null,day:null,raster:raster};
  var bytes=176*256*raster*raster*4*2;
  _castingArtStats.bytes=0;_castingArtStats.entries=0;
  if(bytes>_castingArtStats.maxBytes){_castingArtStats.fallbacks++;return _castingArtwork=art;}
  var start=performance.now();
  try {
    [0,1].forEach(function(light){
      var img=document.createElement('canvas');img.width=176*raster;img.height=256*raster;
      var brush=img.getContext('2d');
      if(!brush)throw Error('Arm artwork allocation unavailable');
      brush.scale(raster,raster);
      paintCastingArmBody(brush,castingLitPalette(p,light));
      if(light)art.day=img;else art.shadow=img;
    });
  } catch(error) {
    art.day=null;art.shadow=null;_castingArtStats.fallbacks++;
    _castingArtStats.lastBuildMs=performance.now()-start;
    _castingArtStats.buildMs+=_castingArtStats.lastBuildMs;
    return _castingArtwork=art;
  }
  var cost=performance.now()-start;
  _castingArtStats.builds+=2;_castingArtStats.buildMs+=cost;_castingArtStats.lastBuildMs=cost;
  _castingArtStats.bytes=bytes;_castingArtStats.entries=2;
  return _castingArtwork=art;
}
// Curved, tapered digits with independent joints. Open/closed interpolation is
// anatomical geometry, not a crossfade between two sets of ghost fingers.
function paintCastingDigit(c,base,knee,tip,width,skin,curl) {
  var dx=tip.x-base.x,dy=tip.y-base.y,len=Math.hypot(dx,dy)||1;
  var nx=-dy/len,ny=dx/len;
  c.beginPath();c.moveTo(base.x+nx*width,base.y+ny*width);
  c.bezierCurveTo(knee.x+nx*width,knee.y+ny*width,tip.x+nx*width*.6,tip.y+ny*width*.6,tip.x,tip.y);
  c.bezierCurveTo(tip.x-nx*width*.8,tip.y-ny*width*.8,knee.x-nx*width,knee.y-ny*width,base.x-nx*width,base.y-ny*width);
  c.closePath();
  var grad=c.createLinearGradient(base.x-width,base.y,base.x+width,base.y-7);
  grad.addColorStop(0,skin.shadow);grad.addColorStop(.42,skin.base);grad.addColorStop(.72,skin.light);grad.addColorStop(1,skin.base);
  c.fillStyle=grad;c.fill();c.strokeStyle=skin.shadow;c.lineWidth=.8;c.stroke();
  c.globalAlpha=.55;
  castingStroke(c,skin.crease,.75,[knee.x-nx*width*.6,knee.y-ny*width*.6,knee.x+nx*width*.6,knee.y+ny*width*.6]);
  c.globalAlpha=1;
  if(curl>.4){
    c.save();c.translate(tip.x+(base.x-tip.x)*.16,tip.y+(base.y-tip.y)*.16);c.rotate(Math.atan2(dy,dx)+Math.PI/2);
    c.fillStyle=skin.nail;c.globalAlpha=.72;c.beginPath();c.ellipse(0,1,width*.40,width*.65,0,0,Math.PI*2);c.fill();c.restore();
  }
}
function paintCastingFingers(c,skin,curl,thumb) {
  if(thumb){
    paintCastingDigit(c,{x:56,y:92},{x:37-curl*2,y:77-curl*5},
      {x:32+curl*14,y:56+curl*6},7.5,skin,curl);return;
  }
  var digits=[
    [59,63,54,35,46,10,54,36,58,43,5.9],
    [73,55,73,25,74,0,75,17,80,31,6.5],
    [87,59,93,32,101,14,95,28,98,39,6.0],
    [100,69,112,49,123,34,113,47,109,59,4.8]
  ];
  for(var i=3;i>=0;i--){var d=digits[i];
    paintCastingDigit(c,{x:d[0],y:d[1]},
      {x:d[2]+(d[6]-d[2])*curl,y:d[3]+(d[7]-d[3])*curl},
      {x:d[4]+(d[8]-d[4])*curl,y:d[5]+(d[9]-d[5])*curl},d[10],skin,curl);
  }
}
function getCastingArtPose(now) {
  var elapsed=Math.max(0,now-_castingPoseState.castAt);
  var duration=Math.max(210,Math.min(420,_castingPoseState.cooldown*.9));
  var windup=typeof MISSILE_CAST_WINDUP_MS==='number'?MISSILE_CAST_WINDUP_MS:120;
  if(elapsed<windup){
    var anticipation=elapsed/windup;
    anticipation=anticipation*anticipation*(3-2*anticipation);
    return {phase:'anticipation / gathering',thrust:-.25*anticipation,curl:.84+.14*anticipation,
      energy:.55+.45*anticipation,elapsed:elapsed,duration:duration};
  }
  var t=(elapsed-windup)/duration,thrust=0,curl=.84,energy=.55,phase='ready / gathering';
  if(t<1){
    if(t<.18){var u=t/.18;var push=1-Math.pow(1-u,3);thrust=-.25+push*1.25;curl=.98*(1-push);energy=1-u;phase='release';}
    else if(t<.43){thrust=1;curl=0;energy=0;phase='follow-through';}
    else {var v=(t-.43)/.57;var ease=v*v*(3-2*v);thrust=1-ease;curl=.84*ease;energy=.55*Math.max(0,(v-.45)/.55);phase='recovery / gathering';}
  }
  return {phase:phase,thrust:thrust,curl:curl,energy:energy,elapsed:elapsed,duration:duration};
}
function drawCastingHandFocus(x,y,r,energy,now,side,S) {
  if(energy<=.005)return;
  var mp=typeof GAME_MATERIALS!=='undefined'&&GAME_MATERIALS.missileMagic?GAME_MATERIALS.missileMagic.hex:
    {core:'#fff3d8',light:'#c4edf0',mid:'#79b9d0',deep:'#315677',rune:'#bfa16a'};
  ctx.save();ctx.globalAlpha=energy;
  var glow=ctx.createRadialGradient(x,y,0,x,y,r*2.6);
  glow.addColorStop(0,'rgba(160,221,238,.28)');glow.addColorStop(.35,'rgba(96,174,205,.15)');glow.addColorStop(1,'rgba(64,111,153,0)');
  ctx.fillStyle=glow;ctx.beginPath();ctx.arc(x,y,r*2.6,0,Math.PI*2);ctx.fill();
  ctx.strokeStyle=mp.mid;ctx.lineWidth=.7*S;
  var angle=now*.0011*side;
  for(var i=0;i<3;i++){
    var a=angle+i*Math.PI*2/3;
    ctx.beginPath();ctx.ellipse(x,y,r*1.55,r*.7,a,a+.15,a+1.0);ctx.stroke();
  }
  ctx.fillStyle=mp.light;ctx.beginPath();ctx.moveTo(x,y-r);ctx.quadraticCurveTo(x+r*.45,y-r*.2,x+r*.5,y);
  ctx.lineTo(x,y+r*.8);ctx.lineTo(x-r*.45,y);ctx.closePath();ctx.fill();
  ctx.fillStyle=mp.core;ctx.beginPath();ctx.moveTo(x,y-r*.72);ctx.lineTo(x+r*.17,y);ctx.lineTo(x,y+r*.35);ctx.lineTo(x-r*.1,y);ctx.closePath();ctx.fill();
  ctx.restore();
}
function drawFirstPersonCastingArt(now) {
  if(!MODE3D||shopOpen)return;
  now=Number.isFinite(now)?now:Date.now();
  var S=resScale,w=canvas.width,h=canvas.height,pose=getCastingArtPose(now);
  var dt=_castingPoseState.lastNow?Math.max(0,Math.min(.05,(now-_castingPoseState.lastNow)/1000)):0;
  _castingPoseState.lastNow=now;
  var spd=Math.hypot(vel.x,vel.y);_castingPoseState.walkPhase+=spd*dt*.048;
  var stride=Math.min(1,spd/180),bob=Math.sin(_castingPoseState.walkPhase)*1.9*S*stride;
  var breath=Math.sin(now*.0017)*.6*S;
  var exposure=Math.max(0,Math.min(1,((typeof ambientLight==='number'?ambientLight:.9)-.28)/.62));
  var art=getCastingArmArtwork(S),lit=castingLitPalette(art.palette,exposure);
  var scale=.68*S;
  // Both hands leave the aim line clear. The casting hand releases near the
  // existing right-offset projectile lane; simulation origin is never moved.
  var right={x:w*.72-pose.thrust*27*S,y:h*.70+pose.thrust*16*S+bob+breath,angle:-.25-pose.thrust*.10};
  var left={x:w*.245+pose.thrust*9*S,y:h*.78+pose.thrust*5*S-bob*.7+breath,angle:.34+pose.thrust*.12};
  ctx.save();ctx.globalCompositeOperation='source-over';ctx.globalAlpha=1;ctx.shadowBlur=0;ctx.imageSmoothingEnabled=true;
  function hand(anchor,flip,curl,focus) {
    ctx.save();ctx.translate(anchor.x,anchor.y);ctx.rotate(anchor.angle);ctx.scale(flip*scale,scale);ctx.translate(-79,-83);
    paintCastingFingers(ctx,lit.skin,curl,false);
    if(art.shadow){
      ctx.drawImage(art.shadow,0,0,176,256);
      if(exposure>0){ctx.globalAlpha=exposure;ctx.drawImage(art.day,0,0,176,256);ctx.globalAlpha=1;}
    } else paintCastingArmBody(ctx,lit);
    paintCastingFingers(ctx,lit.skin,curl,true);
    // Small local reflected light follows the palm contours, not a bloom wash.
    ctx.globalAlpha=focus*.35;castingStroke(ctx,'#bde8e9',1,[61,74,69,81,74,87,71,95]);ctx.globalAlpha=1;
    ctx.restore();
  }
  hand(left,-1,.46+pose.thrust*.25,pose.energy*.4);
  // The focus is behind the leading fingers, so the hand appears to cup it.
  var fx=right.x-11*S,fy=right.y-33*S;
  drawCastingHandFocus(fx,fy,4.9*S,pose.energy,now,1,S);
  hand(right,1,pose.curl,pose.energy);
  // One fine strand visibly ties the gathered focus to the fingertip gesture.
  if(pose.energy>.05){
    ctx.globalAlpha=pose.energy*.55;ctx.strokeStyle='#c4edf0';ctx.lineWidth=.7*S;
    ctx.beginPath();ctx.moveTo(fx-8*S,fy+7*S);ctx.bezierCurveTo(fx-11*S,fy-5*S,fx+8*S,fy-9*S,fx+8*S,fy);ctx.stroke();
  }
  ctx.restore();
}
