// Articulated first-person visual study, deliberately separate from the shipping
// hand artwork. A hand-local mesh and finished robe materials are projected and
// shaded by Canvas2D. It never writes to world depth or gameplay state. No
// raster pose sheets or WebGL.
var HAND_RIG_PREVIEW = false;
var HAND_RIG_TIME_MS = 0;
var HAND_RIG_VIEW_YAW = 0;
var HAND_RIG_DEFAULT_GRIP = 'handle';
var HAND_RIG_GRIP = HAND_RIG_DEFAULT_GRIP;
var HAND_RIG_STYLE = 'arcane';
var HAND_RIG_MATERIAL = {skin:0,cloth:1,cuff:2,lining:3,trim:4};
// Reusable hand shapes only; these do not equip or create an item.
var HAND_RIG_REST_POSES = {
  reach:{finger:[.25,.32,.15],thumb:[.14,.23,.16],axialRoll:0},
  // A staff grip rolls around the forearm's own longitudinal axis, not around
  // the camera. Rest is upright; full extension pitches the wrist forward by
  // about 20 degrees, carrying a held shaft toward the cast before recovering.
  handle:{finger:[.82,1.04,.64],thumb:[.35,.70,.54],axialRoll:Math.PI/2,
    root:[[.80,3.02,-.07],[.70,3.01,-.04],[1.15,3.01,-.07]],
    // A held handle thrusts away even when the free-hand style has recoil.
    placement:{distance:[57,58,70],anchorX:[.735,.732,.680],anchorY:[.930,.928,.855]}},
  cradle:{finger:[.48,.63,.32],thumb:[.22,.39,.28],axialRoll:0}
};
// Animation recipes share one skeleton and renderer. The preference catalog
// maps a stable style ID to one of these recipes; spells never read this data.
// Digit order: index, middle, ring, little, thumb. Angles are local radians.
var HAND_RIG_ANIMATIONS = {
  arcane:{
    // Gathering flexes the wrist without turning the palm toward the player.
    // Keep all phases near the outward rest; finger curl carries the flourish.
    root:[[.72,3.45,-.07],[.56,3.34,.02],[.82,3.20,-.17]],
    distance:[57,55,67],anchorX:[.735,.735,.640],anchorY:[.930,.955,.880],
    fingers:[
      [[.25,.32,.15],[.30,.37,.18],[.35,.42,.21],[.43,.50,.258],[.14,.23,.16]],
      [[.92,1.23,.75],[.97,1.28,.78],[1.02,1.33,.81],[1.10,1.41,.858],[.32,.71,.60]],
      [[.06,.08,.04],[.11,.13,.07],[.16,.18,.10],[.24,.26,.148],[.04,.12,.08]]
    ],
    thumbSplay:[.88,.48,.88],thumbOpposition:[.20,.88,.10],thumbTwist:[-.45,-1.10,-.45],
    fingerSpread:[1,.3,2.5]
  },
  finger_guns:{
    // Index and middle aim together. Ring/little first extend from the palm,
    // like the aiming fingers, then fold back at PIP with a returning DIP.
    // Photo-referenced thumb: a broad radial base opens an L-shaped gap above
    // the index. Its tip folds forward in the palm silhouette, not out of the
    // palm toward the viewer. The distal joint leads; the knuckle follows.
    // Keep the radial/thumb side upright like a gun held level. Pitch/yaw aim
    // the two extended fingers toward the reticle without canting the hand.
    root:[[-1.39,-.29,Math.PI/2],[-1.40,-.29,Math.PI/2],[-1.38,-.28,Math.PI/2]],
    distance:[57,58,53],anchorX:[.735,.739,.750],anchorY:[.930,.931,.905],
    fingers:[
      [[.035,.045,.025],[.035,.045,.025],[.04,2.18,.96],[.04,2.20,.96],[1.00,-.10,.55]],
      [[.04,.05,.025],[.04,.05,.025],[.045,2.21,.94],[.045,2.23,.94],[1.00,-.08,.58]],
      [[.035,.045,.025],[.035,.045,.025],[.04,2.20,.95],[.04,2.22,.95],[1.00,-.35,-.80]]
    ],
    // Right-hand opposition maps flexion into radial X/forward Y. A small
    // depth cant keeps the thumb pad rounded; twist orients its pad palmward.
    thumbSplay:[0,0,0],thumbOpposition:[-1.50,-1.50,-1.50],thumbTwist:[-Math.PI/2,-Math.PI/2,-Math.PI/2],
    fingerSpread:[.35,.30,.35]
  }
};
function handRigAnimationId(styleId) {
  var id=typeof getCastingStyle==='function'?getCastingStyle(styleId).handAnimation:styleId;
  return Object.prototype.hasOwnProperty.call(HAND_RIG_ANIMATIONS,id)?id:'arcane';
}
function handRigAnimation(pose) {return HAND_RIG_ANIMATIONS[handRigAnimationId(pose.style)];}
function handRigBlend(values,pose) {
  return values[0]+(values[1]-values[0])*pose.gather+(values[2]-values[0])*pose.release;
}
var _handRigMesh = null;
var _handRigStats = {vertices:0,triangles:0,visibleFaces:0,buildMs:0,builds:0,lastRenderMs:0,topologyBytes:0};
function clearHandRigCache() {
  _handRigMesh=null;
  _handRigStats={vertices:0,triangles:0,visibleFaces:0,buildMs:0,builds:0,lastRenderMs:0,topologyBytes:0};
}
function getHandRigStats() {return Object.assign({},_handRigStats);}
function handRigEase(t) {t=Math.max(0,Math.min(1,t));return t*t*(3-2*t);}
function sampleHandRigAction(action,elapsedMs,grip,styleId) {
  // Gameplay adapters must call this non-looping sampler, never the studio
  // loop below. An advancing idle clock cannot accidentally trigger a cast.
  var t=action==='cast'?Math.max(0,Math.min(2800,Number.isFinite(elapsedMs)?elapsedMs:0)):0;
  var gather=0,release=0,phase='ready';
  if(t>=500&&t<1050){gather=handRigEase((t-500)/550);phase='gather';}
  else if(t>=1050&&t<1420){gather=1-handRigEase((t-1050)/370);release=handRigEase((t-1050)/370);phase='release';}
  else if(t>=1420&&t<1730){release=1;phase='follow-through';}
  else if(t>=1730&&t<2450){release=1-handRigEase((t-1730)/720);phase='recovery';}
  // Small delayed forearm response is authored, not a frame-rate spring.
  var settle=t>=1420&&t<2450?Math.sin((t-1420)*.010)*Math.exp(-(t-1420)/290)*.055:0;
  grip=Object.prototype.hasOwnProperty.call(HAND_RIG_REST_POSES,grip)?grip:HAND_RIG_DEFAULT_GRIP;
  return {timeMs:t,phase:phase,gather:gather,release:release,settle:settle,grip:grip,style:handRigAnimationId(styleId)};
}
function sampleHandRigRestPose(grip,styleId) {return sampleHandRigAction('idle',0,grip,styleId);}
function sampleHandRigPose(timeMs,grip,styleId) {
  var t=((Number.isFinite(timeMs)?timeMs:0)%2800+2800)%2800;
  return sampleHandRigAction('cast',t,grip,styleId);
}
function handRigRootAngles(pose,yaw) {
  // Arcane rests palm-away; Finger Guns turns sideways with the thumb raised.
  // Each recipe recovers to its own rest instead of sharing a palm-thrust pose.
  var root=handRigRootFrames(pose);
  return [0,1,2].map(function(i){return handRigBlend([root[0][i],root[1][i],root[2][i]],pose)+
    (i===1&&Number.isFinite(yaw)?yaw:0);});
}
function handRigRootFrames(pose) {
  return HAND_RIG_REST_POSES[pose.grip].root||handRigAnimation(pose).root;
}
function handRigPlacementFrames(pose) {
  return HAND_RIG_REST_POSES[pose.grip].placement||handRigAnimation(pose);
}
function handRigRotate(x,y,z,rx,ry,rz) {
  var a=y*Math.cos(rx)-z*Math.sin(rx),b=y*Math.sin(rx)+z*Math.cos(rx);
  var c=x*Math.cos(ry)+b*Math.sin(ry),d=-x*Math.sin(ry)+b*Math.cos(ry);
  return [c*Math.cos(rz)-a*Math.sin(rz),c*Math.sin(rz)+a*Math.cos(rz),d];
}
function buildHandRigMesh() {
  if(_handRigMesh)return _handRigMesh;
  var start=performance.now(),desc=[],faces=[],groups=[];
  // Authored proportions in arbitrary model units, not a medical model.
  var digits=[
    {x:-2.65,y:8.10,z:.0,lengths:[3.4,2.3,1.5],radius:.82,splay:.070,curl:.00},
    {x:-.77,y:8.75,z:0,lengths:[3.8,2.5,1.7],radius:.88,splay:.010,curl:.05},
    {x:1.22,y:8.30,z:0,lengths:[3.6,2.3,1.6],radius:.81,splay:-.050,curl:.10},
    {x:2.98,y:7.20,z:.05,lengths:[2.7,1.8,1.3],radius:.68,splay:-.115,curl:.18},
    {x:-2.45,y:2.45,z:.20,lengths:[3.0,2.8,2.1],radius:1.0,splay:.88,curl:0,thumb:true}
  ];
  function surface(rings,sides,make,material,capEnd) {
    var first=desc.length;
    for(var r=0;r<rings;r++)for(var j=0;j<sides;j++)desc.push(make(r,j,sides));
    for(var row=0;row<rings-1;row++)for(var col=0;col<sides;col++){
      var a=first+row*sides+col,b=first+row*sides+(col+1)%sides,c=b+sides,d=a+sides;
      var faceMaterial=typeof material==='function'?material(row,col,sides):material;
      faces.push([a,c,b,faceMaterial],[a,d,c,faceMaterial]);
    }
    if(capEnd){
      var end=first+(rings-1)*sides;
      // Tiny rounded tip ring; fan closes the end without a blunt cylinder cap.
      for(var k=1;k<sides-1;k++)faces.push([end,end+k+1,end+k,material]);
    }
    return first;
  }
  // A rounded palm volume, narrower at the wrist, with an unequal MCP line.
  var palmWidths=[2.60,2.78,3.30,3.73,3.87,3.88,3.67];
  var palmDepth=[.92,1.00,1.06,1.08,.96,.80,.65];
  surface(7,24,function(r,j,sides){
    var v=r/6,angle=j*Math.PI*2/sides,co=Math.cos(angle),si=Math.sin(angle);
    var x=palmWidths[r]*Math.sign(co)*Math.pow(Math.abs(co),.83);
    var top=8.78-.17*x-.060*x*x;
    var y=v*top,z=palmDepth[r]*si;
    // Thenar and hypothenar pads are part of the surface, not attached spheres.
    if(si>0)z+=si*(.40*Math.exp(-((x+1.9)*(x+1.9)+(y-3.0)*(y-3.0))/5.5)+
      .16*Math.exp(-((x-2.4)*(x-2.4)+(y-3.7)*(y-3.7))/6.0));
    return {kind:0,x:x,y:y,z:z};
  },HAND_RIG_MATERIAL.skin,true);
  // Robe forearm. Ring deformation tapers wrist rotation toward the elbow;
  // shared wrist dimensions keep the attachment closed while turning. Four
  // proximal rings carry the sleeve beyond the player viewport. The original
  // nine wrist/elbow stations stay exact; four extra rings shape a turned linen
  // edge, fitted cuff and narrow trim without changing the hand seam.
  var proximalRings=4;
  var forearmY=[];
  for(var proximal=0;proximal<proximalRings;proximal++)forearmY.push(-20-(proximalRings-proximal)*5);
  forearmY=forearmY.concat([-20,-17.5,-15,-12.5,-10,-7.5,-5,-4.15,-3.45,-2.5,-1.55,-.75,0]);
  var forearmFirst=surface(forearmY.length,24,function(r,j,sides){
    var y=forearmY[r];
    var u=Math.min(1,-y/20),extension=Math.max(0,-20-y),angle=j*Math.PI*2/sides;
    var width=2.6+u*1.1+Math.max(0,u-.14)*.8+.55*(1-Math.exp(-extension*.095/.55));
    var depth=.92+u*1.85+.55*(1-Math.exp(-extension*.0925/.55));
    // Long, offset lobes read as gathered wool rather than a regular ribbed
    // tube. The fitted cuff remains smooth and slightly flares at both trims.
    var fold=y<=-5?1+(.035+.025*u)*Math.sin(angle*5+u*2.2)*Math.sin(Math.min(1,u)*Math.PI)+
      .022*Math.sin(angle*3-u*1.7):1;
    if(y>-5){
      var cuff=Math.max(0,1-Math.abs(y+2.45)/2.75);
      width+=cuff*.34;depth+=cuff*.18;
    }
    var axisX=u*u*3.5+extension*.35;
    return {kind:1,x:Math.cos(angle)*width*fold+axisX,y:y,z:Math.sin(angle)*depth*fold,axisX:axisX};
  },function(row){
    var middle=(forearmY[row]+forearmY[row+1])*.5;
    if(middle>-.75)return HAND_RIG_MATERIAL.lining; // turned linen at the hand opening
    if(middle>-1.55)return HAND_RIG_MATERIAL.trim;  // upper brass trim
    if(middle>-3.45)return HAND_RIG_MATERIAL.cuff;
    if(middle>-4.15)return HAND_RIG_MATERIAL.trim; // lower brass trim
    return HAND_RIG_MATERIAL.cloth;               // gathered robe cloth
  },false);
  for(var seam=0;seam<24;seam++){
    var wrist=desc[seam];
    desc[forearmFirst+(forearmY.length-1)*24+seam]={kind:1,x:wrist.x,y:wrist.y,z:wrist.z,axisX:0};
  }
  // Inspection views can turn the normally off-screen shoulder end toward the
  // camera. Close it with one cloth center and a two-sided fan so no
  // background-colored hole appears when reviewing the existing Back view.
  var proximalAxis=desc[forearmFirst].axisX,proximalCenter=desc.length;
  desc.push({kind:1,x:proximalAxis,y:forearmY[0],z:0,axisX:proximalAxis,sleeveCap:true});
  for(var cap=0;cap<24;cap++){
    var rim=forearmFirst+cap,nextRim=forearmFirst+(cap+1)%24;
    // Both sides are intentional: Back inspection looks into the arm entry,
    // while ordinary player views see (or crop) the exterior-facing side.
    faces.push([proximalCenter,rim,nextRim,HAND_RIG_MATERIAL.cloth],
      [proximalCenter,nextRim,rim,HAND_RIG_MATERIAL.cloth]);
  }
  digits.forEach(function(d,id){
    d.slot=id;
    var L=d.lengths,total=L[0]+L[1]+L[2];
    // Each joint has neighboring rings: deformation changes direction around
    // a hinge without scaling phalanges into rubber strips.
    var distances=[0,L[0]*.42,L[0]-.20,L[0]+.20,L[0]+L[1]-.15,
      L[0]+L[1]+.15,total-.48,total-.18,total+.06];
    var first=surface(distances.length,12,function(r,j,sides){
      var distance=distances[r],t=distance/total,angle=j*Math.PI*2/sides;
      var radius=d.radius*(1-.27*t);
      // The thumb's buried metacarpal broadens into the thenar mass. A uniform
      // tube here reads as a fifth finger glued to the side of the palm.
      if(d.thumb&&r<3)radius*=[1.72,1.48,1.12][r];
      if(r===7)radius*=.68;if(r===8)radius*=.10;
      if(r===0)radius*=1.04;
      return {kind:2,digit:id,distance:distance,side:Math.cos(angle)*radius,
        pad:Math.sin(angle)*radius*(Math.sin(angle)>0?.96:.79)};
    },HAND_RIG_MATERIAL.skin,true);
    groups.push({first:first,count:distances.length*12,digit:id});
  });
  // Construction above uses a radial-negative reference. Reflect the entire
  // local mesh to make a RIGHT hand (+x thumb, +y fingers, +z palmar surface),
  // including digit frames and triangle winding. A camera turn cannot correct
  // chirality, and a screen-only mirror would leave lighting/winding wrong.
  desc.forEach(function(d){if(d.kind===2)d.side=-d.side;else d.x=-d.x;if(d.kind===1)d.axisX=-d.axisX;});
  digits.forEach(function(d){d.x=-d.x;d.splay=-d.splay;d.handedness=-1;});
  faces.forEach(function(f){var b=f[1];f[1]=f[2];f[2]=b;});
  var n=desc.length;
  _handRigMesh={desc:desc,faces:faces,digits:digits,groups:groups,
    positions:new Float64Array(n*3),normals:new Float64Array(n*3),screen:new Float64Array(n*3),
    light:new Float64Array(n),order:[],faceDepth:new Float64Array(faces.length)};
  _handRigStats.vertices=n;_handRigStats.triangles=faces.length;
  _handRigStats.buildMs=performance.now()-start;_handRigStats.builds++;
  // Numeric working storage; JS topology objects and Canvas internals are extra.
  _handRigStats.topologyBytes=n*10*8+faces.length*8;
  return _handRigMesh;
}
function handRigDigitFrame(d,pose) {
  var rest=HAND_RIG_REST_POSES[pose.grip]||HAND_RIG_REST_POSES.reach;
  var animation=handRigAnimation(pose),slot=d.slot;
  var flex=[0,1,2].map(function(i){return handRigBlend(animation.fingers.map(function(frame){return frame[slot][i];}),pose);});
  // Item grip is an independent constraint, not a style or unlock. Until an
  // item author supplies contact targets, occupied hands retain their template.
  if(pose.grip!=='reach')flex=(d.thumb?rest.thumb:rest.finger).map(function(value,i){return value+(d.thumb?0:d.curl*(i===2?.6:1));});
  var handedness=d.handedness||1;
  var occupied=pose.grip!=='reach';
  var splay=d.thumb?(occupied?.88:handRigBlend(animation.thumbSplay,pose))*handedness:
    d.splay*(occupied?1:handRigBlend(animation.fingerSpread,pose));
  var opposition=d.thumb?(occupied?(pose.grip==='handle'?.55:.20):handRigBlend(animation.thumbOpposition,pose))*handedness:0;
  var twist=d.thumb?(occupied?-.45:handRigBlend(animation.thumbTwist,pose))*handedness:0;
  var angles=[flex[0],flex[0]+flex[1],flex[0]+flex[1]+flex[2]];
  var centers=[[0,0,0]],cy=0,cz=0;
  for(var i=0;i<3;i++){
    cy+=Math.cos(angles[i])*d.lengths[i];cz+=Math.sin(angles[i])*d.lengths[i];
    centers.push([0,cy,cz]);
  }
  return {angles:angles,centers:centers,splay:splay,opposition:opposition,twist:twist};
}
function poseHandRigMesh(mesh,pose,yaw) {
  var P=mesh.positions,N=mesh.normals,F=mesh.faces;
  N.fill(0);
  var digitFrames=mesh.digits.map(function(d){return handRigDigitFrame(d,pose);});
  // A face-on inspection isolates the silhouette from first-person wrist pose
  // and foreshortening. The ordinary player/side views retain their full motion.
  var palmStudy=yaw==='palm';
  var angles=palmStudy?[0,0,Math.PI/2]:handRigRootAngles(pose,0);
  var neutral=palmStudy?angles:handRigRootFrames(pose)[0],inspectionYaw=Number.isFinite(yaw)?yaw:0;
  var axialRoll=HAND_RIG_REST_POSES[pose.grip].axialRoll;
  var axialCos=Math.cos(axialRoll),axialSin=Math.sin(axialRoll);
  for(var i=0;i<mesh.desc.length;i++){
    var d=mesh.desc[i],x=d.x,y=d.y,z=d.z,weight=1,axisX=0,axisZ=0;
    if(d.kind===2){
      var finger=mesh.digits[d.digit],frame=digitFrames[d.digit],L=finger.lengths;
      var seg=d.distance<L[0]?0:d.distance<L[0]+L[1]?1:2;
      var start=seg===0?0:seg===1?L[0]:L[0]+L[1],along=d.distance-start;
      var angle=frame.angles[seg],center=frame.centers[seg];
      var ringAngle=angle;
      if(seg>0&&along<.35)ringAngle=frame.angles[seg-1]+(angle-frame.angles[seg-1])*(.5+along/.7);
      if(seg<2&&along>L[seg]-.35)ringAngle=angle+(frame.angles[seg+1]-angle)*(.5-(L[seg]-along)/.7);
      var side=d.side*Math.cos(frame.twist)-d.pad*Math.sin(frame.twist);
      var pad=d.side*Math.sin(frame.twist)+d.pad*Math.cos(frame.twist);
      var local=handRigRotate(side,center[1]+Math.cos(angle)*along-Math.sin(ringAngle)*pad,
        center[2]+Math.sin(angle)*along+Math.cos(ringAngle)*pad,0,frame.opposition,frame.splay);
      x=finger.x+local[0];y=finger.y+local[1];z=finger.z+local[2];
    } else if(d.kind===1){
      weight=handRigEase((y+20)/20);
      // A small delayed cuff bend visibly differs from rigid hand rotation.
      axisX=d.axisX;axisZ=Math.sin((-y/20)*Math.PI)*pose.settle*5;z+=axisZ;
    }
    // Rotate each cross-section about the actual forearm centerline. Rotating
    // its position around the wrist by a varying camera angle bends the whole
    // arm into a hook; axial roll preserves its length and authored centerline.
    if(axialRoll){
      var crossX=x-axisX,crossZ=z-axisZ;
      x=axisX+crossX*axialCos+crossZ*axialSin;
      z=axisZ-crossX*axialSin+crossZ*axialCos;
    }
    // The entire forearm shares the outward rest orientation. Only additional
    // casting twist fades toward the elbow, avoiding a permanently twisted arm.
    var rotated=handRigRotate(x,y,z,neutral[0]+(angles[0]-neutral[0])*weight,
      neutral[1]+(angles[1]-neutral[1])*weight+inspectionYaw,neutral[2]+(angles[2]-neutral[2])*weight);
    P[i*3]=rotated[0];P[i*3+1]=rotated[1];P[i*3+2]=rotated[2];
  }
  // Area-weighted vertex normals follow the deformed surface, not a painted
  // highlight that remains attached to the camera while the wrist turns.
  for(var f=0;f<F.length;f++){
    var face=F[f],a=face[0]*3,b=face[1]*3,c=face[2]*3;
    var abx=P[b]-P[a],aby=P[b+1]-P[a+1],abz=P[b+2]-P[a+2];
    var acx=P[c]-P[a],acy=P[c+1]-P[a+1],acz=P[c+2]-P[a+2];
    var nx=aby*acz-abz*acy,ny=abz*acx-abx*acz,nz=abx*acy-aby*acx;
    for(var k=0;k<3;k++){var index=face[k]*3;N[index]+=nx;N[index+1]+=ny;N[index+2]+=nz;}
  }
  return mesh;
}
function handRigHexRgb(value,fallback) {
  value=typeof value==='string'&&/^#[0-9a-f]{6}$/i.test(value)?value:fallback;
  var n=parseInt(value.slice(1),16);return [(n>>16)&255,(n>>8)&255,n&255];
}
function handRigRgbMix(a,b,t) {
  return [Math.round(a[0]+(b[0]-a[0])*t),Math.round(a[1]+(b[1]-a[1])*t),Math.round(a[2]+(b[2]-a[2])*t)];
}
function handRigMaterialPalette() {
  var M=typeof GAME_MATERIALS!=='undefined'?GAME_MATERIALS:{};
  var skin=M.casterSkin&&M.casterSkin.hex?M.casterSkin.hex:
    {shadow:'#714b40',base:'#b98265',light:'#e2bb94'};
  var cloth=M.casterCloth&&M.casterCloth.hex?M.casterCloth.hex:
    {deep:'#16242e',mid:'#344b58',lit:'#607a83',cuff:'#4c3c2c',linen:'#c4b493'};
  var metal=M.casterMetal&&M.casterMetal.hex?M.casterMetal.hex:
    {shadow:'#50422c',base:'#a78b54',light:'#dfc78d'};
  var arm=typeof equipment!=='undefined'&&equipment.robes&&equipment.robes.armColor?equipment.robes.armColor:{};
  var sharedClothDeep=handRigHexRgb(cloth.deep,'#16242e');
  var clothDeep=handRigHexRgb(arm.deep,cloth.deep),clothMid=handRigHexRgb(arm.mid,cloth.mid);
  var clothLit=handRigHexRgb(arm.lit,cloth.lit),cuff=handRigHexRgb(arm.cuff,cloth.cuff);
  var linen=handRigHexRgb(cloth.linen,'#c4b493'),metalLight=handRigHexRgb(metal.light,'#dfc78d');
  return [
    [handRigHexRgb(skin.shadow,'#714b40'),handRigHexRgb(skin.base,'#b98265'),handRigHexRgb(skin.light,'#e2bb94')],
    [clothDeep,clothMid,clothLit],
    [handRigRgbMix(clothDeep,cuff,.38),cuff,handRigRgbMix(cuff,metalLight,.24)],
    [handRigRgbMix(linen,sharedClothDeep,.34),linen,handRigRgbMix(linen,[255,246,222],.22)],
    [handRigHexRgb(metal.shadow,'#50422c'),handRigHexRgb(metal.base,'#a78b54'),metalLight]
  ];
}
function handRigColor(level,material,palette) {
  var ramp=palette[material]||palette[0],t=Math.max(0,Math.min(1,(level-.25)/.78)),rgb;
  if(t<.58)rgb=handRigRgbMix(ramp[0],ramp[1],t/.58);
  else rgb=handRigRgbMix(ramp[1],ramp[2],(t-.58)/.42);
  return 'rgb('+rgb[0]+','+rgb[1]+','+rgb[2]+')';
}
function drawFirstPersonHandRig(timeMs,yaw,grip,styleId) {
  if(!MODE3D||shopOpen)return;
  var begin=performance.now(),mesh=buildHandRigMesh(),pose=sampleHandRigPose(timeMs,grip||HAND_RIG_GRIP,styleId||HAND_RIG_STYLE);
  var palmStudy=yaw==='palm';
  yaw=palmStudy?'palm':Number.isFinite(yaw)?yaw:0;poseHandRigMesh(mesh,pose,yaw);
  var P=mesh.positions,N=mesh.normals,S=mesh.screen,I=mesh.light,faces=mesh.faces;
  var palette=handRigMaterialPalette();
  var placement=handRigPlacementFrames(pose),distance=palmStudy?48:handRigBlend(placement.distance,pose);
  var focal=canvas.height*1.28,anchorX=canvas.width*(palmStudy?.70:handRigBlend(placement.anchorX,pose));
  var anchorY=canvas.height*(palmStudy?.78:handRigBlend(placement.anchorY,pose));
  var ambient=typeof ambientLight==='number'?Math.max(.25,Math.min(1,ambientLight)):.8;
  for(var v=0;v<mesh.desc.length;v++){
    var p=v*3,depth=distance-P[p+2];
    S[p]=anchorX+P[p]*focal/depth;S[p+1]=anchorY-P[p+1]*focal/depth;S[p+2]=depth;
    var length=Math.hypot(N[p],N[p+1],N[p+2])||1;
    var diffuse=Math.max(0,(-.40*N[p]+.62*N[p+1]+.67*N[p+2])/length);
    var rim=Math.max(0,(.65*N[p]-.2*N[p+1]-.72*N[p+2])/length);
    I[v]=.27+ambient*.16+diffuse*.55+rim*.13;
  }
  var order=mesh.order;order.length=0;
  for(var f=0;f<faces.length;f++){
    var F=faces[f],a=F[0]*3,b=F[1]*3,c=F[2]*3;
    if(S[a+2]<3||S[b+2]<3||S[c+2]<3)continue;
    var area=(S[b]-S[a])*(S[c+1]-S[a+1])-(S[b+1]-S[a+1])*(S[c]-S[a]);
    if(area>=-.015)continue;
    if(Math.max(S[a],S[b],S[c])<0||Math.min(S[a],S[b],S[c])>canvas.width||
      Math.max(S[a+1],S[b+1],S[c+1])<0||Math.min(S[a+1],S[b+1],S[c+1])>canvas.height)continue;
    mesh.faceDepth[f]=(S[a+2]+S[b+2]+S[c+2])/3;order.push(f);
  }
  order.sort(function(a,b){return mesh.faceDepth[b]-mesh.faceDepth[a]||a-b;});
  ctx.save();
  try {
    ctx.globalAlpha=1;ctx.globalCompositeOperation='source-over';ctx.shadowBlur=0;
    ctx.lineJoin='round';ctx.lineWidth=.55;
    for(var fi=0;fi<order.length;fi++){
      var face=faces[order[fi]],ia=face[0],ib=face[1],ic=face[2],aa=ia*3,bb=ib*3,cc=ic*3;
      var x0=S[aa],y0=S[aa+1],x1=S[bb],y1=S[bb+1],x2=S[cc],y2=S[cc+1];
      var den=(x1-x0)*(y2-y0)-(x2-x0)*(y1-y0);
      var gx=((I[ib]-I[ia])*(y2-y0)-(I[ic]-I[ia])*(y1-y0))/den;
      var gy=((x1-x0)*(I[ic]-I[ia])-(x2-x0)*(I[ib]-I[ia]))/den;
      var mag=gx*gx+gy*gy,shade,material=face[3];
      var materialLift=material===HAND_RIG_MATERIAL.trim ? .07 :
        material===HAND_RIG_MATERIAL.lining ? .025 : 0;
      var rawLo=Math.min(I[ia],I[ib],I[ic]),rawHi=Math.max(I[ia],I[ib],I[ic]);
      var lo=rawLo+materialLift,hi=rawHi+materialLift;
      if(mag>1e-9&&hi-lo>.006){
        var start=(rawLo-I[ia])/mag,end=(rawHi-I[ia])/mag;
        shade=ctx.createLinearGradient(x0+gx*start,y0+gy*start,x0+gx*end,y0+gy*end);
        shade.addColorStop(0,handRigColor(lo,material,palette));shade.addColorStop(1,handRigColor(hi,material,palette));
      } else shade=handRigColor((I[ia]+I[ib]+I[ic])/3+materialLift,material,palette);
      ctx.fillStyle=shade;ctx.strokeStyle=shade;
      ctx.beginPath();ctx.moveTo(x0,y0);ctx.lineTo(x1,y1);ctx.lineTo(x2,y2);ctx.closePath();ctx.fill();
      // Subpixel seam cover, not an outline or lower-resolution raster layer.
      ctx.stroke();
    }
  } finally {ctx.restore();}
  _handRigStats.visibleFaces=order.length;_handRigStats.lastRenderMs=performance.now()-begin;
}
