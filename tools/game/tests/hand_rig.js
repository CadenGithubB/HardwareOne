// Articulated hand/robe proof: real mesh deformation and CPU Canvas projection,
// not prerecorded poses. Geometry contracts intentionally avoid image hashes.
(function () {
  var G=(0,eval)('this'),checks=0,commands=[],stack=[],gradientId=0,clock=0;
  function check(name,ok){if(!ok)throw Error(name);checks++;__out('PASS '+name);}
  function forbidden(){throw Error('hand rig used a forbidden gameplay/raster API');}
  function record(name,args){commands.push([name,Array.prototype.slice.call(args)]);}
  var stateKeys=['globalAlpha','globalCompositeOperation','shadowBlur','lineJoin','lineWidth','fillStyle','strokeStyle'];
  G.ctx={globalAlpha:.43,globalCompositeOperation:'multiply',shadowBlur:3,lineJoin:'miter',lineWidth:7,
    fillStyle:'#123456',strokeStyle:'#654321',
    save:function(){var s={};stateKeys.forEach(function(k){s[k]=G.ctx[k];});stack.push(s);},
    restore:function(){var s=stack.pop();if(!s)throw Error('unbalanced restore');stateKeys.forEach(function(k){G.ctx[k]=s[k];});},
    beginPath:function(){record('begin',arguments);},moveTo:function(){record('move',arguments);},
    lineTo:function(){record('line',arguments);},closePath:function(){record('close',arguments);},
    fill:function(){commands.push(['fill',G.ctx.fillStyle,G.ctx.globalAlpha]);},
    stroke:function(){commands.push(['stroke',G.ctx.strokeStyle,G.ctx.lineWidth]);},
    createLinearGradient:function(){var id=++gradientId;record('gradient',arguments);return {id:id,
      addColorStop:function(){commands.push(['stop',id,Array.prototype.slice.call(arguments)]);}};},
    drawImage:forbidden,putImageData:forbidden,getImageData:forbidden};
  G.document={createElement:forbidden};
  G.beginSceneDepthFrame=G.writeSceneDepthPolygon=G.fillSceneDepthPolygon=forbidden;
  G.performance={now:function(){clock+=.01;return clock;}};
  G.MODE3D=true;G.shopOpen=false;G.canvas={width:360,height:240};G.ambientLight=.8;
  G.pos={x:123,y:456,floorZ:60};G.vel={x:4,y:5};G.mana=82;G.health=7;
  G.projectiles=[{x:8,y:9,z:10,speed:220}];G.impacts=[{x:6,y:7,z:8,lifeMs:220}];
  G.equipment={robes:{armColor:{mid:'#abcdef'}},relic:{value:.4}};
  var oldRandom=Math.random;
  Math.random=forbidden;
  function snapshot(){return JSON.stringify([G.pos,G.vel,G.mana,G.health,G.projectiles,G.impacts,G.equipment]);}
  function finite(array){return Array.prototype.every.call(array,Number.isFinite);}
  function maxDelta(a,b){var result=0;for(var i=0;i<a.length;i++)result=Math.max(result,Math.abs(a[i]-b[i]));return result;}
  function pointDistance(a,b){return Math.hypot(a[0]-b[0],a[1]-b[1],a[2]-b[2]);}
  function cross(a,b){return [a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];}
  function subtract(a,b){return [a[0]-b[0],a[1]-b[1],a[2]-b[2]];}
  function meanPosition(mesh,predicate){var out=[0,0,0],count=0;mesh.desc.forEach(function(d,i){if(predicate(d)){for(var k=0;k<3;k++)out[k]+=mesh.positions[i*3+k];count++;}});return out.map(function(v){return v/count;});}
  function meanScreen(mesh,predicate){var out=[0,0,0],count=0;mesh.desc.forEach(function(d,i){if(predicate(d)){for(var k=0;k<3;k++)out[k]+=mesh.screen[i*3+k];count++;}});return out.map(function(v){return v/count;});}
  function palmNormal(mesh,front){var out=[0,0,0],P=mesh.positions;
    mesh.faces.forEach(function(f){for(var k=0;k<3;k++){var d=mesh.desc[f[k]];if(d.kind!==0||d.y>=7||d.z*(front?1:-1)<=.1)return;}
      var a=f[0]*3,b=f[1]*3,c=f[2]*3;
      var abx=P[b]-P[a],aby=P[b+1]-P[a+1],abz=P[b+2]-P[a+2];
      var acx=P[c]-P[a],acy=P[c+1]-P[a+1],acz=P[c+2]-P[a+2];
      out[0]+=aby*acz-abz*acy;out[1]+=abz*acx-abx*acz;out[2]+=abx*acy-aby*acx;
    });var length=Math.hypot.apply(Math,out)||1;return out.map(function(v){return v/length;});}
  function resetCommands(){commands=[];gradientId=0;}
  function commandHash(){var s=JSON.stringify(commands),h=2166136261;for(var i=0;i<s.length;i++)h=Math.imul(h^s.charCodeAt(i),16777619);return h>>>0;}
  try {
    (0,eval)(slurp('assets/games/src/01-casting-styles.js'));
    var rigSource=slurp('assets/games/src/15-hand-rig.js');
    if(__argv.indexOf('--restore-short-sleeve')>=0)rigSource=rigSource.replace('var proximalRings=4;','var proximalRings=0;');
    (0,eval)(rigSource);
    check('neutral rig is opt-in and does not create a cache while loading',G.HAND_RIG_PREVIEW===false&&G._handRigMesh===null);
    var mesh=G.buildHandRigMesh(),stats=G.getHandRigStats();
    check('one hand has bounded real vertices and indexed triangles',mesh.desc.length>100&&mesh.desc.length<2000&&mesh.faces.length>100&&mesh.faces.length<4000);
    check('topology is built once and reused',G.buildHandRigMesh()===mesh&&G.getHandRigStats().builds===1);
    check('reported working bytes match typed array storage',stats.topologyBytes===mesh.positions.byteLength+mesh.normals.byteLength+mesh.screen.byteLength+mesh.light.byteLength+mesh.faceDepth.byteLength);
    check('every face references three distinct valid vertices',mesh.faces.every(function(f){return f[0]!==f[1]&&f[0]!==f[2]&&f[1]!==f[2]&&f.slice(0,3).every(function(i){return Number.isInteger(i)&&i>=0&&i<mesh.desc.length;});}));
    var materialCounts={};mesh.faces.forEach(function(f){materialCounts[f[3]]=(materialCounts[f[3]]||0)+1;});
    check('robe topology separates skin cloth cuff lining and metallic trim',
      Object.keys(G.HAND_RIG_MATERIAL).every(function(key){return materialCounts[G.HAND_RIG_MATERIAL[key]]>0;})&&
      mesh.faces.every(function(f){return Number.isInteger(f[3])&&f[3]>=0&&f[3]<=G.HAND_RIG_MATERIAL.trim;}));
    var authoredStations=[-20,-17.5,-15,-12.5,-10,-7.5,-5,-2.5,0],cuffStations=[-4.15,-3.45,-1.55,-.75];
    var actualStations=[];mesh.desc.forEach(function(d){if(d.kind===1&&actualStations.indexOf(d.y)<0)actualStations.push(d.y);});
    check('shaped cuff adds four bounded rings without moving original forearm stations',
      authoredStations.every(function(y){return actualStations.indexOf(y)>=0;})&&cuffStations.every(function(y){return actualStations.indexOf(y)>=0;}));
    var sleeveCap=mesh.desc.findIndex(function(d){return d.sleeveCap;}),sleeveCapFaces=[];
    mesh.faces.forEach(function(f,i){if(f[0]===sleeveCap||f[1]===sleeveCap||f[2]===sleeveCap)sleeveCapFaces.push(i);});
    var capFace=mesh.faces[sleeveCapFaces[0]],backCapFace=mesh.faces[sleeveCapFaces[1]];
    var ca=mesh.desc[capFace[0]],cb=mesh.desc[capFace[1]],cc=mesh.desc[capFace[2]];
    var capNormalY=(cb.z-ca.z)*(cc.x-ca.x)-(cb.x-ca.x)*(cc.z-ca.z);
    var ba=mesh.desc[backCapFace[0]],bb=mesh.desc[backCapFace[1]],bc=mesh.desc[backCapFace[2]];
    var backCapNormalY=(bb.z-ba.z)*(bc.x-ba.x)-(bb.x-ba.x)*(bc.z-ba.z);
    check('proximal robe end has a complete two-sided cloth closure',sleeveCap>=0&&sleeveCapFaces.length===48&&capNormalY<0&&backCapNormalY>0&&
      sleeveCapFaces.every(function(i){return mesh.faces[i][3]===G.HAND_RIG_MATERIAL.cloth;}));
    var previewPalette=G.handRigMaterialPalette();
    check('partial equipped robe colors override role by role with shared fallbacks',
      JSON.stringify(previewPalette[G.HAND_RIG_MATERIAL.cloth][0])==='[22,36,46]'&&
      JSON.stringify(previewPalette[G.HAND_RIG_MATERIAL.cloth][1])==='[171,205,239]'&&
      JSON.stringify(previewPalette[G.HAND_RIG_MATERIAL.cloth][2])==='[96,122,131]');
    check('all five material ramps retain authored color instead of neutral gray',previewPalette.length===5&&previewPalette.every(function(ramp){
      return ramp.length===3&&ramp.some(function(rgb){return rgb[0]!==rgb[1]||rgb[1]!==rgb[2];});
    }));
    if(__argv.indexOf('--mirror-hand-chirality')>=0){
      mesh.desc.forEach(function(d){if(d.kind===2)d.side=-d.side;else d.x=-d.x;});
      mesh.digits.forEach(function(d){d.x=-d.x;d.splay=-d.splay;d.handedness=-(d.handedness||1);});
      mesh.faces.forEach(function(f){var t=f[1];f[1]=f[2];f[2]=t;});
    }
    if(__argv.indexOf('--invert-hand-winding')>=0)mesh.faces.forEach(function(f){var t=f[1];f[1]=f[2];f[2]=t;});
    var firstSide=mesh.faces.filter(function(f){return f.slice(0,3).every(function(i){return mesh.desc[i].kind===0;})&&
      f.some(function(i){return mesh.desc[i].y===0;})&&f.some(function(i){return mesh.desc[i].y>0;});})[0];
    var a=mesh.desc[firstSide[0]],b=mesh.desc[firstSide[1]],c=mesh.desc[firstSide[2]];
    var nx=(b.y-a.y)*(c.z-a.z)-(b.z-a.z)*(c.y-a.y);
    var nz=(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
    check('palm exterior triangles wind outward rather than displaying inner backs',nx*(a.x+b.x+c.x)+nz*(a.z+b.z+c.z)>0);
    var tipFaces=mesh.faces.filter(function(f){var p=mesh.desc[f[0]],q=mesh.desc[f[1]],r=mesh.desc[f[2]];
      return p.kind===2&&q.kind===2&&r.kind===2&&p.digit===q.digit&&p.digit===r.digit&&p.distance===q.distance&&p.distance===r.distance;});
    check('rounded fingertip caps also wind toward the exterior',tipFaces.length>=5&&tipFaces.every(function(f){var p=mesh.desc[f[0]],q=mesh.desc[f[1]],r=mesh.desc[f[2]];
      return (q.pad-p.pad)*(r.side-p.side)-(q.side-p.side)*(r.pad-p.pad)>0;}));
    var bind=JSON.stringify([mesh.desc,mesh.faces,mesh.digits,mesh.groups]);
    check('finite time sampling wraps deterministically including negative time',JSON.stringify(G.sampleHandRigPose(0))===JSON.stringify(G.sampleHandRigPose(2800))&&G.sampleHandRigPose(-1).timeMs===2799&&finite(['timeMs','gather','release','settle'].map(function(k){return G.sampleHandRigPose(NaN)[k];})));
    check('proof sequence contains gathering release follow-through and recovery',G.sampleHandRigPose(800).phase==='gather'&&G.sampleHandRigPose(1280).phase==='release'&&G.sampleHandRigPose(1600).phase==='follow-through'&&G.sampleHandRigPose(2200).phase==='recovery');
    var frameA=G.handRigDigitFrame(mesh.digits[0],G.sampleHandRigPose(0,'reach'));
    var frameB=G.handRigDigitFrame(mesh.digits[0],G.sampleHandRigPose(1050,'reach'));
    check('finger bones change joint angles during gathering',maxDelta(frameA.angles,frameB.angles)>.5);
    check('bone lengths remain rigid across articulated poses',mesh.digits.every(function(d){return [0,800,1050,1280,1600,2200].every(function(t){var f=G.handRigDigitFrame(d,G.sampleHandRigPose(t,'reach'));return d.lengths.every(function(length,i){return Math.abs(pointDistance(f.centers[i],f.centers[i+1])-length)<1e-10;});});}));
    var thumb=mesh.digits.filter(function(d){return d.thumb;})[0];
    var thumbA=G.handRigDigitFrame(thumb,G.sampleHandRigPose(0,'reach')),thumbB=G.handRigDigitFrame(thumb,G.sampleHandRigPose(1050,'reach'));
    check('thumb opposition includes independent root rotation and pad twist',Math.abs(thumbA.opposition-thumbB.opposition)>.3&&Math.abs(thumbA.twist-thumbB.twist)>.3);
    if(__argv.indexOf('--freeze-hand-motion')>=0){var realPose=G.poseHandRigMesh;G.poseHandRigMesh=function(m,p,y){return realPose(m,G.sampleHandRigPose(0,'reach'),y);};}
    G.poseHandRigMesh(mesh,G.sampleHandRigPose(0,'reach'),0);var ready=Array.prototype.slice.call(mesh.positions);
    G.poseHandRigMesh(mesh,G.sampleHandRigPose(1050,'reach'),0);var gather=Array.prototype.slice.call(mesh.positions);
    check('animation changes actual 3D vertex positions instead of swapping images',maxDelta(ready,gather)>.5);
    G.poseHandRigMesh(mesh,G.sampleHandRigPose(0,'reach'),.8);
    check('wrist yaw changes three-dimensional depth as well as screen lateral position',mesh.positions.some(function(v,i){return i%3===2&&Math.abs(v-ready[i])>.5;}));
    G.poseHandRigMesh(mesh,G.sampleHandRigPose(0,'reach'),0);
    check('returning to the same pose has no accumulated deformation',maxDelta(ready,mesh.positions)===0);
    var wristCenter=meanPosition(mesh,function(d){return d.kind===0&&d.y===0;});
    var indexRoot=meanPosition(mesh,function(d){return d.kind===2&&d.digit===0&&d.distance===0;});
    var littleRoot=meanPosition(mesh,function(d){return d.kind===2&&d.digit===3&&d.distance===0;});
    var middleRoot=meanPosition(mesh,function(d){return d.kind===2&&d.digit===1&&d.distance===0;});
    var palmar=palmNormal(mesh,true),dorsal=palmNormal(mesh,false);
    var handed=cross(subtract(indexRoot,littleRoot),subtract(middleRoot,wristCenter));
    check('hand has right-handed anatomical chirality in three dimensions',handed[0]*palmar[0]+handed[1]*palmar[1]+handed[2]*palmar[2]>0);
    check('freehand reach rest shows the back of the hand with palm facing away',palmar[2]<-.4&&dorsal[2]>.4);
    var thumbId=mesh.digits.indexOf(thumb),thumbRoot=meanPosition(mesh,function(d){return d.kind===2&&d.digit===thumbId&&d.distance===0;});
    var palmCenter=meanPosition(mesh,function(d){return d.kind===0;});
    check('right freehand reach thumb is toward screen center rather than the outer edge',thumbRoot[0]<palmCenter[0]);
    var middleLength=mesh.digits[1].lengths.reduce(function(a,b){return a+b;},0);
    var middleTip=meanPosition(mesh,function(d){return d.kind===2&&d.digit===1&&d.distance>middleLength;});
    check('resting fingers reach away into the scene',middleTip[2]<middleRoot[2]);
    var familyPositions=[];
    ['reach','handle','cradle'].forEach(function(grip){G.poseHandRigMesh(mesh,G.sampleHandRigPose(0,grip),0);familyPositions.push(Array.prototype.slice.call(mesh.positions));});
    check('shared grip families create distinct real finger configurations',maxDelta(familyPositions[0],familyPositions[1])>.5&&maxDelta(familyPositions[0],familyPositions[2])>.25&&maxDelta(familyPositions[1],familyPositions[2])>.25);
    check('unknown grip names safely use the handle family',G.sampleHandRigPose(0,'missing').grip==='handle'&&G.sampleHandRigPose(0,'toString').grip==='handle');
    var defaultHandle=G.HAND_RIG_DEFAULT_GRIP==='handle'&&G.HAND_RIG_GRIP==='handle'&&
      G.sampleHandRigRestPose().grip==='handle'&&G.sampleHandRigAction('idle',1050).grip==='handle'&&
      G.sampleHandRigAction('cast',1050).grip==='handle'&&G.sampleHandRigPose(1050).grip==='handle';
    var savedGrip=G.HAND_RIG_GRIP;G.HAND_RIG_GRIP='reach';
    defaultHandle=defaultHandle&&G.sampleHandRigAction('idle',1050).grip==='handle';G.HAND_RIG_GRIP=savedGrip;
    check('Handle is the default rest and pure sampler fallback independent of the studio selection',defaultHandle);
    var continuous=true;
    [500,1050,1420,1730,2450,2800].forEach(function(boundary){G.poseHandRigMesh(mesh,G.sampleHandRigPose(boundary-.001,'reach'),0);var previous=Array.prototype.slice.call(mesh.positions);
      G.poseHandRigMesh(mesh,G.sampleHandRigPose(boundary+.001,'reach'),0);if(maxDelta(previous,mesh.positions)>.02)continuous=false;});
    check('casting phase boundaries do not snap the mesh between unrelated poses',continuous);
    G.poseHandRigMesh(mesh,G.sampleHandRigPose(0,'reach'),0);
    if(__argv.indexOf('--idle-replays-cast')>=0){var originalAction=G.sampleHandRigAction;G.sampleHandRigAction=function(action,time,grip,style){return originalAction(action==='idle'?'cast':action,time,grip,style);};}
    var styles=G.getCastingStyleOptions().map(function(style){return style.id;});
    check('registry styles share the same rig and each have an animation recipe',G.HAND_RIG_STYLE==='arcane'&&styles.indexOf('finger_guns')>=0&&styles.every(function(style){return !!G.HAND_RIG_ANIMATIONS[G.getCastingStyle(style).handAnimation];}));
    var idleStable=true,completionStable=true,styleBoundaries=true;
    styles.forEach(function(style){['reach','handle','cradle'].forEach(function(grip){
      var rest=G.sampleHandRigRestPose(grip,style),restText=JSON.stringify(rest);
      G.poseHandRigMesh(mesh,rest,0);var restPositions=Array.prototype.slice.call(mesh.positions);
      [0,500,800,1050,1420,1730,2200,2450,2800,10800,1e12].forEach(function(time){
        if(JSON.stringify(G.sampleHandRigAction('idle',time,grip,style))!==restText)idleStable=false;
      });
      [2800,3600,10000,1e12].forEach(function(time){var pose=G.sampleHandRigAction('cast',time,grip,style);
        G.poseHandRigMesh(mesh,pose,0);if(pose.gather!==0||pose.release!==0||pose.settle!==0||maxDelta(restPositions,mesh.positions)!==0)completionStable=false;
      });
      [500,1050,1420,1730,2450,2800].forEach(function(boundary){G.poseHandRigMesh(mesh,G.sampleHandRigPose(boundary-.001,grip,style),0);var previous=Array.prototype.slice.call(mesh.positions);
        G.poseHandRigMesh(mesh,G.sampleHandRigPose(boundary+.001,grip,style),0);if(maxDelta(previous,mesh.positions)>.02)styleBoundaries=false;});
    });});
    check('advancing idle clocks never replay a casting flourish in any grip or style',idleStable);
    check('completed non-looping cast actions return to rest and never restart',completionStable);
    check('only the explicitly looped studio sampler repeats the casting sequence',G.sampleHandRigPose(3600,'reach','arcane').gather>0&&G.sampleHandRigAction('cast',3600,'reach','arcane').gather===0);
    check('all style and grip boundaries preserve continuous mesh motion',styleBoundaries);
    check('unknown actions and styles resolve to safe ordinary rest',G.sampleHandRigAction('missing',1050,'reach','missing').gather===0&&G.sampleHandRigRestPose('reach','missing').style==='arcane');
    if(__argv.indexOf('--restore-curled-middle-finger')>=0)G.HAND_RIG_ANIMATIONS.finger_guns.fingers.forEach(function(frame){frame[1]=[.97,1.18,.72];});
    var gunPhases=[0,1050,1500,2200].map(function(time){return G.sampleHandRigAction('cast',time,'reach','finger_guns');});
    check('Finger Guns keeps index and middle extended with ring and little tucked',gunPhases.every(function(pose){return [0,1].every(function(id){return G.handRigDigitFrame(mesh.digits[id],pose).angles[2]<.35;})&&[2,3].every(function(id){return G.handRigDigitFrame(mesh.digits[id],pose).angles[2]>1.8;});}));
    var thumbReady=G.handRigDigitFrame(thumb,gunPhases[0]),thumbCocked=G.handRigDigitFrame(thumb,gunPhases[1]),thumbPressed=G.handRigDigitFrame(thumb,gunPhases[2]);
    check('Finger Guns thumb FK tip raises in anticipation then presses toward the finger direction',
      thumbCocked.centers[3][2]>thumbReady.centers[3][2]&&thumbPressed.centers[3][1]>thumbReady.centers[3][1]+.5&&thumbPressed.centers[3][2]<thumbReady.centers[3][2]-.5);
    var gunRecipe=G.HAND_RIG_ANIMATIONS.finger_guns;
    if(__argv.indexOf('--restore-palm-first-finger-curl')>=0){
      var previousPalmFold=[[[1.42,1.64,.82],[1.45,1.66,.80]],[[1.44,1.65,.82],[1.46,1.67,.80]],[[1.44,1.65,.82],[1.46,1.67,.80]]];
      gunRecipe.fingers.forEach(function(frame,i){frame[2]=previousPalmFold[i][0];frame[3]=previousPalmFold[i][1];});
    }
    if(__argv.indexOf('--restore-loose-finger-curl')>=0)gunRecipe.fingers.forEach(function(frame){frame[2]=[.04,.90,1.0];frame[3]=[.04,.90,1.0];});
    if(__argv.indexOf('--unfold-returning-fingertips')>=0)gunRecipe.fingers.forEach(function(frame){frame[2][2]=.15;frame[3][2]=.15;});
    function boneDirections(frame){return [0,1,2].map(function(i){var v=subtract(frame.centers[i+1],frame.centers[i]),length=Math.hypot.apply(Math,v);return v.map(function(n){return n/length;});});}
    function measuredHinges(frame){var dirs=boneDirections(frame);return [0,1].map(function(i){var a=dirs[i],b=dirs[i+1];return Math.atan2(a[1]*b[2]-a[2]*b[1],a[0]*b[0]+a[1]*b[1]+a[2]*b[2]);});}
    var straightProximal=true,strongPip=true,returningDip=true,curlMeasurements=[];
    var curledTimes=[0,500,680,850,1050,1140,1230,1320,1420,1730,1850,2000,2200,2450,2800];
    curledTimes.forEach(function(time){var pose=G.sampleHandRigAction('cast',time,'reach','finger_guns');
      var index=boneDirections(G.handRigDigitFrame(mesh.digits[0],pose))[0];
      [2,3].forEach(function(digit){var frame=G.handRigDigitFrame(mesh.digits[digit],pose),dirs=boneDirections(frame),hinges=measuredHinges(frame);
        // These bounds describe the requested pose, not a copied recipe: the
        // first bone follows the extended fingers, PIP supplies the main fold,
        // then the distal bone points back toward its own palm-side knuckle.
        var aligned=dirs[0][0]*index[0]+dirs[0][1]*index[1]+dirs[0][2]*index[2];
        var returning=dirs[0][0]*dirs[2][0]+dirs[0][1]*dirs[2][1]+dirs[0][2]*dirs[2][2];
        if(dirs[0][1]<Math.cos(8*Math.PI/180)||aligned<Math.cos(6*Math.PI/180))straightProximal=false;
        if(hinges[0]<110*Math.PI/180||hinges[0]>145*Math.PI/180)strongPip=false;
        if(hinges[1]<35*Math.PI/180||hinges[1]>80*Math.PI/180||returning>-.94||frame.centers[3][1]>=frame.centers[1][1])returningDip=false;
        curlMeasurements.push({time:time,digit:digit,proximalDegrees:Math.atan2(dirs[0][2],dirs[0][1])*180/Math.PI,
          pipDegrees:hinges[0]*180/Math.PI,dipDegrees:hinges[1]*180/Math.PI,tipForwardFromMcp:frame.centers[3][1]});
      });
    });
    check('Finger Guns ring and little proximal bones stay straight out from the palm like the aiming fingers',straightProximal);
    check('Finger Guns ring and little concentrate their main fold at the second knuckle',strongPip);
    check('Finger Guns ring and little distal bones return toward the palm after the PIP fold',returningDip);
    var recoveryExact=true,hingesMonotonic=true,posedProximalStraight=true;
    [2,3].forEach(function(digit){
      var readyFrame=G.handRigDigitFrame(mesh.digits[digit],G.sampleHandRigRestPose('reach','finger_guns'));
      var recoveredFrame=G.handRigDigitFrame(mesh.digits[digit],G.sampleHandRigAction('cast',2450,'reach','finger_guns'));
      if(JSON.stringify(readyFrame)!==JSON.stringify(recoveredFrame))recoveryExact=false;
      [[500,680,850,1050],[1050,1140,1230,1320,1420],[1730,1850,2000,2200,2450]].forEach(function(times){
        var endpoints=[times[0],times[times.length-1]].map(function(t){return measuredHinges(G.handRigDigitFrame(mesh.digits[digit],G.sampleHandRigAction('cast',t,'reach','finger_guns')));});
        var previous=[0,0];times.forEach(function(time){var hinges=measuredHinges(G.handRigDigitFrame(mesh.digits[digit],G.sampleHandRigAction('cast',time,'reach','finger_guns')));
          hinges.forEach(function(value,i){var range=endpoints[1][i]-endpoints[0][i],progress=range?(value-endpoints[0][i])/range:0;
            if(progress<previous[i]-1e-8||progress<-.000001||progress>1.000001)hingesMonotonic=false;previous[i]=progress;});
        });
      });
    });
    curledTimes.forEach(function(time){var pose=G.sampleHandRigAction('cast',time,'reach','finger_guns');G.poseHandRigMesh(mesh,pose,0);
      var root=G.handRigRootAngles(pose,0),palmForward=G.handRigRotate(0,1,0,root[0],root[1],root[2]);
      [2,3].forEach(function(digit){var rootPoint=meanPosition(mesh,function(d){return d.kind===2&&d.digit===digit&&d.distance===0;});
        var proximal=meanPosition(mesh,function(d){return d.kind===2&&d.digit===digit&&d.distance===mesh.digits[digit].lengths[0]*.42;});
        var direction=subtract(proximal,rootPoint),length=Math.hypot.apply(Math,direction);
        if((direction[0]*palmForward[0]+direction[1]*palmForward[1]+direction[2]*palmForward[2])/length<Math.cos(10*Math.PI/180))posedProximalStraight=false;
      });
    });
    check('posed ring and little mesh proximal sections extend along the palm rather than plunging into it',posedProximalStraight);
    check('ring and little PIP and DIP folds blend monotonically and recover exact resting curl',hingesMonotonic&&recoveryExact);
    function palmarHeight(x,y){var height=-Infinity;
      mesh.faces.forEach(function(face){var a=mesh.desc[face[0]],b=mesh.desc[face[1]],c=mesh.desc[face[2]];
        if(a.kind!==0||b.kind!==0||c.kind!==0||a.z<0||b.z<0||c.z<0)return;
        var den=(b.y-c.y)*(a.x-c.x)+(c.x-b.x)*(a.y-c.y);if(Math.abs(den)<1e-10)return;
        var u=((b.y-c.y)*(x-c.x)+(c.x-b.x)*(y-c.y))/den,v=((c.y-a.y)*(x-c.x)+(a.x-c.x)*(y-c.y))/den,w=1-u-v;
        if(u>=-1e-9&&v>=-1e-9&&w>=-1e-9)height=Math.max(height,u*a.z+v*b.z+w*c.z);
      });return height;
    }
    var tipClearance=Infinity,tipSamples=0;
    [0,1050,1500,2200].forEach(function(time){
      var pose=G.sampleHandRigPose(time,'reach','finger_guns');G.poseHandRigMesh(mesh,pose,0);
      var root=G.handRigRootAngles(pose,0),basis=[[1,0,0],[0,1,0],[0,0,1]].map(function(axis){return G.handRigRotate(axis[0],axis[1],axis[2],root[0],root[1],root[2]);});
      [2,3].forEach(function(digit){var length=mesh.digits[digit].lengths.reduce(function(a,b){return a+b;},0);
        mesh.desc.forEach(function(d,i){if(d.kind!==2||d.digit!==digit||d.distance<length-.5)return;
          var p=i*3,local=basis.map(function(axis){return axis[0]*mesh.positions[p]+axis[1]*mesh.positions[p+1]+axis[2]*mesh.positions[p+2];});
          var height=palmarHeight(local[0],local[1]);if(Number.isFinite(height)){tipClearance=Math.min(tipClearance,local[2]-height);tipSamples++;}
        });
      });
    });
    if(__argv.indexOf('--report-finger-clearance')>=0)__out('FINGER_TIP_PALM_CLEARANCE '+JSON.stringify({samples:tipSamples,minimum:tipClearance,joints:curlMeasurements}));
    check('tightly curled ring and little fingertip surfaces remain outside the palmar mesh',tipSamples>0&&tipClearance>0);
    if(__argv.indexOf('--restore-base-swing-thumb')>=0){
      [[.02,.08,.08],[-.12,.03,.06],[.40,.50,.25]].forEach(function(flex,i){gunRecipe.fingers[i][4]=flex;});
      gunRecipe.thumbSplay=[1.10,1.22,.73];gunRecipe.thumbOpposition=[.08,.02,.35];gunRecipe.thumbTwist=[-.20,-.12,-.40];
    }
    function thumbHinges(frame){var directions=[];for(var i=0;i<3;i++){var v=subtract(frame.centers[i+1],frame.centers[i]),length=Math.hypot.apply(Math,v);directions.push(v.map(function(n){return n/length;}));}
      // Signed flexion is essential: the reference thumb's distal joint passes
      // through straight on its way from a raised tip to the pressed tip.
      return [0,1].map(function(i){var a=directions[i],b=directions[i+1],dot=a[0]*b[0]+a[1]*b[1]+a[2]*b[2];return Math.atan2(a[1]*b[2]-a[2]*b[1],dot);});}
    var thumbSamples={},thumbMotionTimes=[0,1050,1140,1230,1320,1420,1730,1850,2000,2200,2450];
    thumbMotionTimes.forEach(function(time){var frame=G.handRigDigitFrame(thumb,G.sampleHandRigPose(time,'reach','finger_guns'));thumbSamples[time]={frame:frame,hinges:thumbHinges(frame)};});
    var startThumb=thumbSamples[1050],peakThumb=thumbSamples[1420],endThumb=thumbSamples[2450],readyThumb=thumbSamples[0];
    var hingeChanges=peakThumb.hinges.map(function(v,i){return v-startThumb.hinges[i];});
    var thumbJointsPress=hingeChanges.every(function(change){return Math.abs(change)>.15;})&&hingeChanges[0]*hingeChanges[1]>0,thumbSync=true;
    [[1050,1140,1230,1320,1420],[1730,1850,2000,2200,2450]].forEach(function(times,phase){
      var previous=phase===0?-Infinity:Infinity;
      times.forEach(function(time){var hinges=thumbSamples[time].hinges;
        var origin=phase===0?startThumb.hinges:readyThumb.hinges;
        var progress=hinges.map(function(v,i){return (v-origin[i])/(peakThumb.hinges[i]-origin[i]);});
        if(Math.abs(progress[0]-progress[1])>1e-8||(phase===0?progress[0]<previous-1e-8:progress[0]>previous+1e-8))thumbSync=false;
        previous=progress[0];
      });
    });
    check('Finger Guns thumb presses both visible FK hinges and recovers them together',thumbJointsPress&&thumbSync&&maxDelta(endThumb.hinges,readyThumb.hinges)<1e-10);
    var thumbBaseQuiet=thumbMotionTimes.every(function(time){var frame=thumbSamples[time].frame;
      return Math.abs(frame.angles[0]-readyThumb.frame.angles[0])<.08&&Math.abs(frame.splay-readyThumb.frame.splay)<.09&&
        Math.abs(frame.opposition-readyThumb.frame.opposition)<.09&&Math.abs(frame.twist-readyThumb.frame.twist)<.09;});
    check('Finger Guns thumb press keeps its buried base and whole-thumb swing restrained',thumbBaseQuiet);
    if(__argv.indexOf('--restore-palm-out-thumb')>=0)gunRecipe.thumbOpposition=[0,0,0];
    function unit(v){var length=Math.hypot.apply(Math,v);return v.map(function(n){return n/length;});}
    function dot(a,b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
    var digitRingDistances={};
    [0,1,thumbId].forEach(function(digit){var distances=[];mesh.desc.forEach(function(d){if(d.kind===2&&d.digit===digit&&distances.indexOf(d.distance)<0)distances.push(d.distance);});
      digitRingDistances[digit]=distances.sort(function(a,b){return a-b;});});
    function posedRing(digit,index){var distance=digitRingDistances[digit][index];return meanPosition(mesh,function(d){return d.kind===2&&d.digit===digit&&d.distance===distance;});}
    // The supplied photographs show a raised thumb in the palm silhouette,
    // with an open web gap above the index. They do NOT require the thumb's
    // flexion plane to match the fingers' Y/Z plane.
    var thumbGapOpen=true,thumbPosed={},minimumThumbGap=Infinity,thumbBasePlanted=true,thumbLengthsRigid=true;
    thumbMotionTimes.forEach(function(time){var pose=G.sampleHandRigPose(time,'reach','finger_guns');G.poseHandRigMesh(mesh,pose,0);
      var root=G.handRigRootAngles(pose,0),basis=[[1,0,0],[0,1,0],[0,0,1]].map(function(axis){return G.handRigRotate(axis[0],axis[1],axis[2],root[0],root[1],root[2]);});
      function localVector(v){return basis.map(function(axis){return dot(axis,v);});}
      var base=localVector(posedRing(thumbId,0)),tip=localVector(posedRing(thumbId,8)),mcp=localVector(posedRing(thumbId,3));
      var fingerDirection=unit(localVector(subtract(posedRing(0,8),posedRing(0,0))));
      var indexContour=-Infinity,thumbContour=Infinity;
      mesh.desc.forEach(function(d,i){if(d.kind!==2)return;var isIndex=d.digit===0,isThumb=d.digit===thumbId&&d.distance>=thumb.lengths[0]+.2;
        if(!isIndex&&!isThumb)return;var p=i*3,x=dot(basis[0],[mesh.positions[p],mesh.positions[p+1],mesh.positions[p+2]]);
        if(isIndex)indexContour=Math.max(indexContour,x);if(isThumb)thumbContour=Math.min(thumbContour,x);
      });
      var gap=thumbContour-indexContour;minimumThumbGap=Math.min(minimumThumbGap,gap);
      if(!Number.isFinite(gap)||gap<.25||tip[0]<=indexContour+.5||Math.abs(tip[2]-base[2])>(tip[0]-base[0])*.3)thumbGapOpen=false;
      if(pointDistance(base,[thumb.x,thumb.y,thumb.z])>.2||(time!==0&&pointDistance(base,thumbPosed[0].base)>.1))thumbBasePlanted=false;
      var centers=thumbSamples[time].frame.centers;
      if(!thumb.lengths.every(function(length,i){return Math.abs(pointDistance(centers[i],centers[i+1])-length)<1e-10;}))thumbLengthsRigid=false;
      thumbPosed[time]={base:base,tip:tip,mcp:mcp,fingerDirection:fingerDirection};
    });
    if(!thumbGapOpen)__out('THUMB_INDEX_SILHOUETTE_GAP '+minimumThumbGap);
    check('reference thumb silhouette keeps an open raised gap above the index throughout the press',thumbGapOpen);
    var thumbTipPress=subtract(thumbPosed[1420].tip,thumbPosed[0].tip),fingerDirection=thumbPosed[1420].fingerDirection;
    var distalPress=unit(subtract(thumbPosed[1420].tip,thumbPosed[1420].mcp));
    check('reference thumb tip lowers and advances over the index rather than sweeping under it',
      thumbTipPress[0]<-.5&&thumbTipPress[1]>.5&&Math.abs(thumbTipPress[2])<.5&&dot(distalPress,fingerDirection)>.7);
    check('reference thumb keeps its base planted and all three bone lengths unchanged',thumbBasePlanted&&thumbLengthsRigid);
    check('posed reference thumb returns to its exact raised rest after pressing',maxDelta(thumbPosed[2450].tip,thumbPosed[0].tip)<1e-10);
    var occupiedStable=true;
    ['handle','cradle'].forEach(function(grip){[0,800,1050,1500,2200].forEach(function(time){mesh.digits.forEach(function(d){
      if(JSON.stringify(G.handRigDigitFrame(d,G.sampleHandRigPose(time,grip,'arcane')))!==JSON.stringify(G.handRigDigitFrame(d,G.sampleHandRigPose(time,grip,'finger_guns'))))occupiedStable=false;
    });});});
    check('occupied handle and cradle digit templates are independent of casting style',occupiedStable);
    G.poseHandRigMesh(mesh,G.sampleHandRigRestPose('reach','arcane'),0);var arcanePositions=Array.prototype.slice.call(mesh.positions);
    G.poseHandRigMesh(mesh,G.sampleHandRigRestPose('reach','finger_guns'),0);
    check('style choice changes real articulated geometry while reusing one mesh',maxDelta(arcanePositions,mesh.positions)>.5&&G.buildHandRigMesh()===mesh&&G.getHandRigStats().builds===1);
    var thumbLength=thumb.lengths.reduce(function(a,b){return a+b;},0);
    var gunThumbBase=meanPosition(mesh,function(d){return d.kind===2&&d.digit===thumbId&&d.distance===0;});
    var gunThumbTip=meanPosition(mesh,function(d){return d.kind===2&&d.digit===thumbId&&d.distance>thumbLength;});
    var thumbUp=subtract(gunThumbTip,gunThumbBase);
    check('Finger Guns resting thumb points mostly upward instead of along the index barrel',thumbUp[1]>.6*Math.hypot.apply(Math,thumbUp));
    G.poseHandRigMesh(mesh,G.sampleHandRigRestPose('reach','arcane'),0);
    check('deformation preserves bind topology and authored bone dimensions',JSON.stringify([mesh.desc,mesh.faces,mesh.digits,mesh.groups])===bind);
    var palmWrist=[],armWrist=[];
    mesh.desc.forEach(function(d,i){if(d.kind===0&&Math.abs(d.y)<1e-10)palmWrist.push(i);if(d.kind===1&&Math.abs(d.y)<1e-10)armWrist.push(i);});
    var pairs=palmWrist.map(function(i){var d=mesh.desc[i],best=-1,error=Infinity;armWrist.forEach(function(j){var q=mesh.desc[j],delta=Math.hypot(d.x-q.x,d.y-q.y,d.z-q.z);if(delta<error){error=delta;best=j;}});return [i,best,error];});
    check('palm and forearm meet at identical wrist-ring positions',palmWrist.length>3&&palmWrist.length===armWrist.length&&pairs.every(function(pair){return pair[2]<1e-9;}));
    var seamGood=true;
    ['reach','handle','cradle'].forEach(function(grip){[0,1050,1600,2200].forEach(function(t){[-1,0,1].forEach(function(yaw){G.poseHandRigMesh(mesh,G.sampleHandRigPose(t,grip),yaw);pairs.forEach(function(pair){var i=pair[0]*3,j=pair[1]*3;if(Math.hypot(mesh.positions[i]-mesh.positions[j],mesh.positions[i+1]-mesh.positions[j+1],mesh.positions[i+2]-mesh.positions[j+2])>1e-9)seamGood=false;});});});});
    check('wrist attachment remains closed through bend and turn poses',seamGood);
    if(__argv.indexOf('--omit-handle-roll')>=0)G.HAND_RIG_REST_POSES.handle.axialRoll=0;
    // Local +X runs across the grip opening. Read its projected direction from
    // opposite actual wrist vertices, rather than checking a recipe angle.
    var acrossWrist=palmWrist.slice().sort(function(i,j){return mesh.desc[i].x-mesh.desc[j].x;});
    resetCommands();G.drawFirstPersonHandRig(0,0);
    var defaultCommands=commandHash(),axisA=acrossWrist[0]*3,axisB=acrossWrist[acrossWrist.length-1]*3;
    var heldAxis=[mesh.screen[axisB]-mesh.screen[axisA],mesh.screen[axisB+1]-mesh.screen[axisA+1]];
    check('default Handle projects its held-item axis upright in the player view',heldAxis[1]<0&&-heldAxis[1]>10*Math.abs(heldAxis[0]));
    resetCommands();G.drawFirstPersonHandRig(0,0,'handle','arcane');
    check('default draw is the explicit Arcane Handle resting pose',defaultCommands===commandHash());
    if(__argv.indexOf('--hook-forearm-centerline')>=0){
      var correctPose=G.poseHandRigMesh;
      G.poseHandRigMesh=function(m,pose,yaw){
        correctPose(m,pose,yaw);
        if(pose.grip==='handle'&&G.HAND_RIG_REST_POSES.handle.axialRoll)m.desc.forEach(function(d,i){
          if(d.kind!==1||d.y===0)return;
          var p=i*3,angle=-Math.PI/2*G.handRigEase((d.y+20)/20),x=m.positions[p],y=m.positions[p+1];
          m.positions[p]=x*Math.cos(angle)-y*Math.sin(angle);m.positions[p+1]=x*Math.sin(angle)+y*Math.cos(angle);
        });return m;
      };
    }
    var armRings=[],armRingHeights=[];
    mesh.desc.forEach(function(d){if(d.kind===1&&armRingHeights.indexOf(d.y)<0)armRingHeights.push(d.y);});
    armRingHeights.sort(function(a,b){return a-b;}).forEach(function(y){var ids=[];mesh.desc.forEach(function(d,i){if(d.kind===1&&d.y===y)ids.push(i);});armRings.push(ids);});
    var elbowRing=armRings[armRingHeights.indexOf(-20)];
    function ringCenter(positions,ids){
      // The wrist copies the slightly asymmetric palm padding. Its lateral
      // extrema identify the authored axis; other ring folds average to zero.
      if(mesh.desc[ids[0]].y===0){ids=ids.slice().sort(function(i,j){return mesh.desc[i].x-mesh.desc[j].x;});ids=[ids[0],ids[ids.length-1]];}
      var out=[0,0,0];ids.forEach(function(i){for(var k=0;k<3;k++)out[k]+=positions[i*3+k]/ids.length;});return out;
    }
    function vertexDistance(positions,a,b){return Math.hypot(positions[a*3]-positions[b*3],positions[a*3+1]-positions[b*3+1],positions[a*3+2]-positions[b*3+2]);}
    var savedRoll=G.HAND_RIG_REST_POSES.handle.axialRoll,centerlineGood=true,crossSectionsGood=true,heldPoseShared=true,rollMoves=true;
    styles.forEach(function(style){[0,800,1050,1420,1600,2200,2700].forEach(function(time){
      var pose=G.sampleHandRigPose(time,'handle',style);
      G.poseHandRigMesh(mesh,pose,0);var rolled=Array.prototype.slice.call(mesh.positions);
      G.HAND_RIG_REST_POSES.handle.axialRoll=0;
      try {G.poseHandRigMesh(mesh,pose,0);} finally {G.HAND_RIG_REST_POSES.handle.axialRoll=savedRoll;}
      if(maxDelta(rolled,mesh.positions)<.5)rollMoves=false;
      armRings.forEach(function(ids){
        if(pointDistance(ringCenter(rolled,ids),ringCenter(mesh.positions,ids))>1e-10)centerlineGood=false;
        ids.forEach(function(a,index){ids.slice(index+1).forEach(function(b){
          if(Math.abs(vertexDistance(rolled,a,b)-vertexDistance(mesh.positions,a,b))>1e-10)crossSectionsGood=false;
        });});
      });
      G.poseHandRigMesh(mesh,G.sampleHandRigPose(time,'handle',style==='arcane'?'finger_guns':'arcane'),0);
      if(maxDelta(rolled,mesh.positions)>1e-10)heldPoseShared=false;
    });});
    check('Handle axial rotation preserves every authored forearm centerline point',centerlineGood&&rollMoves);
    check('Handle axial rotation preserves every forearm cross-section pair distance',crossSectionsGood);
    check('held Handle geometry and arm orientation stay shared between casting styles',heldPoseShared);
    if(__argv.indexOf('--restore-lateral-forearm')>=0)G.HAND_RIG_REST_POSES.handle.root=[[.80,2.65,-.43],[.70,2.62,-.40],[.85,2.63,-.43]];
    var armAligned=true;
    styles.forEach(function(style){[0,800,1050,1420,1600,2200,2700].forEach(function(time){
      resetCommands();G.drawFirstPersonHandRig(time,0,'handle',style);
      var elbowScreen=ringCenter(mesh.screen,elbowRing),wristScreen=ringCenter(mesh.screen,armRings[armRings.length-1]);
      var dx=elbowScreen[0]-wristScreen[0],dy=elbowScreen[1]-wristScreen[1];
      if(dy<=0||Math.abs(dx)/dy>=.5)armAligned=false;
    });});
    check('Handle forearm projects below the wrist rather than far across the screen',armAligned);
    if(__argv.indexOf('--restore-rigid-handle-release')>=0)G.HAND_RIG_REST_POSES.handle.root[2][0]=.85;
    function heldAxis3D(){var out=[mesh.positions[axisB]-mesh.positions[axisA],mesh.positions[axisB+1]-mesh.positions[axisA+1],mesh.positions[axisB+2]-mesh.positions[axisA+2]];
      var length=Math.hypot.apply(Math,out);return out.map(function(v){return v/length;});}
    var forwardTilt=true,tiltRecovers=true,gripClosed=true;
    styles.forEach(function(style){
      resetCommands();G.drawFirstPersonHandRig(0,0,'handle',style);
      var restAxis=heldAxis3D(),restPitch=Math.atan2(restAxis[2],restAxis[1]),restMesh=Array.prototype.slice.call(mesh.positions);
      var restDigits=JSON.stringify(mesh.digits.map(function(d){return G.handRigDigitFrame(d,G.sampleHandRigRestPose('handle',style));}));
      [1420,1600,1730].forEach(function(time){
        resetCommands();G.drawFirstPersonHandRig(time,0,'handle',style);
        var axis=heldAxis3D(),forwardPitch=restPitch-Math.atan2(axis[2],axis[1]);
        if(axis[1]<=0||axis[2]>=restAxis[2]-.15||forwardPitch<.20||forwardPitch>.60)forwardTilt=false;
      });
      var previousPitch=Infinity;
      [1730,1850,2000,2200,2450].forEach(function(time){
        resetCommands();G.drawFirstPersonHandRig(time,0,'handle',style);
        var axis=heldAxis3D(),pitchError=Math.abs(restPitch-Math.atan2(axis[2],axis[1]));
        if(pitchError>previousPitch+1e-10)tiltRecovers=false;previousPitch=pitchError;
      });
      if(previousPitch>1e-10||maxDelta(restMesh,mesh.positions)>1e-10)tiltRecovers=false;
      [0,800,1050,1420,1600,1730,2000,2200,2450,2800].forEach(function(time){
        var pose=G.sampleHandRigAction('cast',time,'handle',style);
        if(JSON.stringify(mesh.digits.map(function(d){return G.handRigDigitFrame(d,pose);}))!==restDigits)gripClosed=false;
      });
    });
    check('Handle held-item axis tilts naturally forward at maximum extension',forwardTilt);
    check('Handle held-item tilt smoothly returns to the exact upright resting mesh',tiltRecovers);
    check('Handle wrist tilt keeps every finger and thumb in the same closed grip',gripClosed);
    if(__argv.indexOf('--restore-groundward-finger-guns')>=0)G.HAND_RIG_ANIMATIONS.finger_guns.root=[[-1.05,0,1.48],[-1.08,0,1.44],[-.90,.08,1.25]];
    var aimPointsInward=true,aimNearCrosshair=true,firstAimFailure=null;
    var indexLength=mesh.digits[0].lengths.reduce(function(a,b){return a+b;},0),savedCanvas=G.canvas;
    [[360,240],[1280,720]].forEach(function(size){G.canvas={width:size[0],height:size[1]};
      [0,800,1050,1280,1420,1600,2200,2450].forEach(function(time){
        resetCommands();G.drawFirstPersonHandRig(time,0,'reach','finger_guns');
        var base=meanScreen(mesh,function(d){return d.kind===2&&d.digit===0&&d.distance===0;});
        var tip=meanScreen(mesh,function(d){return d.kind===2&&d.digit===0&&d.distance>indexLength;});
        var dx=tip[0]-base[0],dy=tip[1]-base[1],tx=G.canvas.width*.5-tip[0],ty=G.canvas.height*.5-tip[1];
        var length=Math.hypot(dx,dy),miss=Math.abs(dx*ty-dy*tx)/(length||1);
        if(dx>=0||dy>=0||dx*tx+dy*ty<=0)aimPointsInward=false;
        if(length<2||miss>G.canvas.height*.1)aimNearCrosshair=false;
        if((!aimPointsInward||!aimNearCrosshair)&&!firstAimFailure)firstAimFailure={time:time,size:size,base:base,tip:tip,miss:miss};
      });
    });G.canvas=savedCanvas;
    if(firstAimFailure)__out('FINGER_GUNS_AIM_FIRST_FAILURE '+JSON.stringify(firstAimFailure));
    check('Finger Guns index points up and inward toward the crosshair through each phase',aimPointsInward);
    check('Finger Guns projected index aim passes near the crosshair at preview and fullscreen sizes',aimNearCrosshair);
    if(__argv.indexOf('--restore-tilted-finger-guns')>=0)G.HAND_RIG_ANIMATIONS.finger_guns.root=[[-1.22,0,.73],[-1.24,0,.72],[-1.20,.02,.76]];
    var gunUpright=true,firstTiltFailure=null,gunUprightTimes=[1050,1420,1730,2450];
    for(var gunTime=0;gunTime<=2800;gunTime+=100)gunUprightTimes.push(gunTime);
    gunUprightTimes.forEach(function(time){
      resetCommands();G.drawFirstPersonHandRig(time,0,'reach','finger_guns');
      // Use the palm's radial axis, not the thumb, whose silhouette changes
      // intentionally during the photo-referenced pressing motion.
      var dx=mesh.screen[axisB]-mesh.screen[axisA],dy=mesh.screen[axisB+1]-mesh.screen[axisA+1];
      if((!finite([dx,dy])||dy>=0||Math.abs(dx)>-dy*.2)&&!firstTiltFailure){gunUpright=false;firstTiltFailure={time:time,dx:dx,dy:dy};}
    });
    if(firstTiltFailure)__out('FINGER_GUNS_UPRIGHT_FIRST_FAILURE '+JSON.stringify(firstTiltFailure));
    check('Finger Guns palm radial axis stays upright through the entire casting cycle',gunUpright);
    // Recreate both ingredients of the reported exposed sleeve: the original
    // short topology and its original near-horizontal Finger Guns arm pose.
    // Correcting the aim by itself hides that older short rim in these samples.
    if(__argv.indexOf('--restore-short-sleeve')>=0)G.HAND_RIG_ANIMATIONS.finger_guns.root=[[-1.05,0,1.48],[-1.08,0,1.44],[-.90,.08,1.25]];
    var sleeveEndHidden=true,firstSleeveFailure=null,sleeveEnd=armRings[0];
    [[360,240],[1280,720]].forEach(function(size){G.canvas={width:size[0],height:size[1]};
      styles.forEach(function(style){['reach','handle','cradle'].forEach(function(grip){
        [0,800,1050,1280,1420,1600,2200,2450].forEach(function(time){
          resetCommands();G.drawFirstPersonHandRig(time,0,grip,style);
          var minX=Infinity,minY=Infinity,maxX=-Infinity,maxY=-Infinity,minDepth=Infinity;
          sleeveEnd.forEach(function(i){var p=i*3;minX=Math.min(minX,mesh.screen[p]);maxX=Math.max(maxX,mesh.screen[p]);
            minY=Math.min(minY,mesh.screen[p+1]);maxY=Math.max(maxY,mesh.screen[p+1]);minDepth=Math.min(minDepth,mesh.screen[p+2]);});
          // A common outside half-plane keeps the entire terminal rim out of
          // view, including edges between vertices and its antialias footprint.
          var margin=Math.max(-maxX,minX-G.canvas.width,-maxY,minY-G.canvas.height);
          if((margin<2||minDepth<=3)&&!firstSleeveFailure){sleeveEndHidden=false;firstSleeveFailure={time:time,style:style,grip:grip,size:size,margin:margin,minDepth:minDepth};}
        });
      });});
    });G.canvas=savedCanvas;
    if(firstSleeveFailure)__out('SLEEVE_END_FIRST_FAILURE '+JSON.stringify(firstSleeveFailure));
    check('proximal sleeve entry stays beyond the player viewport in all styles grips and phases',sleeveEndHidden);
    resetCommands();G.canvas={width:360,height:240};G.drawFirstPersonHandRig(0,Math.PI,'reach','finger_guns');
    check('Back inspection renders the proximal cloth closure instead of an open sleeve',sleeveCapFaces.some(function(i){return mesh.order.indexOf(i)>=0;}));
    G.canvas=savedCanvas;
    var before=snapshot(),savedState=JSON.stringify(stateKeys.map(function(k){return G.ctx[k];}));
    resetCommands();G.drawFirstPersonHandRig(800,.3,'reach');var first=commandHash();
    check('proof produces Canvas geometry without a raster image or DOM allocation',commands.some(function(c){return c[0]==='fill';})&&commands.some(function(c){return c[0]==='gradient';}));
    resetCommands();G.drawFirstPersonHandRig(800,.3,'reach');
    check('repeated time and camera produce deterministic drawing commands',first===commandHash());
    check('Canvas caller state is restored after drawing',stack.length===0&&JSON.stringify(stateKeys.map(function(k){return G.ctx[k];}))===savedState);
    var savedEquipment=G.equipment,alternateHash=first,samePaletteMesh=false;
    try {
      G.equipment={robes:{id:'same-id',armColor:{deep:'#321342',mid:'#633071',lit:'#9a67b0',cuff:'#754a88'}},relic:savedEquipment.relic};
      resetCommands();G.drawFirstPersonHandRig(800,.3,'reach');alternateHash=commandHash();
      samePaletteMesh=G.buildHandRigMesh()===mesh&&G.getHandRigStats().builds===1;
    } finally {G.equipment=savedEquipment;}
    check('equipment robe colors update drawing without rebuilding or deforming topology',alternateHash!==first&&samePaletteMesh&&JSON.stringify([mesh.desc,mesh.faces,mesh.digits,mesh.groups])===bind);
    if(__argv.indexOf('--shared-palm-thrust')>=0){
      ['distance','anchorX','anchorY'].forEach(function(key){G.HAND_RIG_ANIMATIONS.finger_guns[key]=G.HAND_RIG_ANIMATIONS.arcane[key].slice();});
    }
    var placement={},placementGood=true;
    styles.forEach(function(style){placement[style]=[];[0,1050,1500].forEach(function(time){
      resetCommands();G.drawFirstPersonHandRig(time,0,'reach',style);
      var pose=G.sampleHandRigPose(time,'reach',style),recipe=G.handRigAnimation(pose),depth=mesh.screen[2],focal=G.canvas.height*1.28;
      var actual={distance:depth+mesh.positions[2],anchorX:(mesh.screen[0]-mesh.positions[0]*focal/depth)/G.canvas.width,
        anchorY:(mesh.screen[1]+mesh.positions[1]*focal/depth)/G.canvas.height};
      ['distance','anchorX','anchorY'].forEach(function(key){if(Math.abs(actual[key]-G.handRigBlend(recipe[key],pose))>1e-8)placementGood=false;});
      placement[style].push(actual);
    });});
    check('production projection consumes each style authored distance and screen placement',placementGood);
    check('Arcane reaches away while Finger Guns recoils toward the camera',
      placement.arcane[2].distance>placement.arcane[0].distance&&placement.finger_guns[2].distance<placement.finger_guns[0].distance&&
      placement.arcane[2].anchorX<placement.arcane[0].anchorX&&placement.finger_guns[2].anchorX>placement.finger_guns[0].anchorX);
    if(__argv.indexOf('--restore-handle-style-recoil')>=0)delete G.HAND_RIG_REST_POSES.handle.placement;
    var handleMovesAway=true,handleConverges=true,handleRecovers=true,handleProjectionShared=true,handleReference={};
    var handleTimes=[0,1050,1140,1230,1320,1420,1600,1730,1850,2000,2200,2450,2800];
    styles.forEach(function(style,styleIndex){var samples={},restScreen;
      handleTimes.forEach(function(time){
        resetCommands();G.drawFirstPersonHandRig(time,0,'handle',style);
        var depth=mesh.screen[2],focal=G.canvas.height*1.28;
        samples[time]={distance:depth+mesh.positions[2],anchorX:(mesh.screen[0]-mesh.positions[0]*focal/depth)/G.canvas.width,
          anchorY:(mesh.screen[1]+mesh.positions[1]*focal/depth)/G.canvas.height,
          wristDepth:meanScreen(mesh,function(d){return d.kind===0&&d.y===0;})[2],
          palmDepth:meanScreen(mesh,function(d){return d.kind===0;})[2]};
        if(time===0)restScreen=Array.prototype.slice.call(mesh.screen);
        if((time===2450||time===2800)&&maxDelta(restScreen,mesh.screen)>1e-10)handleRecovers=false;
        if(styleIndex===0)handleReference[time]=Array.prototype.slice.call(mesh.screen);
        else if(maxDelta(handleReference[time],mesh.screen)>1e-10)handleProjectionShared=false;
      });
      var rest=samples[0],peak=samples[1420],previous=samples[1050];
      if(peak.distance<=rest.distance+8||peak.wristDepth<=rest.wristDepth+8||peak.palmDepth<=rest.palmDepth+8)handleMovesAway=false;
      if(peak.anchorX>=rest.anchorX-.025||peak.anchorY>=rest.anchorY-.025)handleConverges=false;
      [1140,1230,1320,1420].forEach(function(time){var sample=samples[time];
        if(sample.distance<=previous.distance||sample.wristDepth<=previous.wristDepth||sample.palmDepth<=previous.palmDepth)handleMovesAway=false;
        if(sample.anchorX>=previous.anchorX||sample.anchorY>=previous.anchorY)handleConverges=false;
        previous=sample;
      });
    });
    check('Handle release moves the actual wrist and palm farther from the camera in both styles',handleMovesAway);
    check('Handle release converges its actual projection slightly inward and upward',handleConverges);
    check('Handle placement recovers to the exact resting screen geometry',handleRecovers);
    check('Handle screen projection is shared across casting styles throughout the sequence',handleProjectionShared);
    var count=0,valid=true,frontMost=true;
    styles.forEach(function(style){[0,800,1050,1280,1600,2200,2700].forEach(function(t){[-Math.PI,-Math.PI/2,0,Math.PI/2,Math.PI].forEach(function(yaw){
      resetCommands();G.drawFirstPersonHandRig(t,yaw,'reach',style);count++;
      if(!finite(mesh.positions)||!finite(mesh.normals)||!finite(mesh.screen)||!finite(mesh.light))valid=false;
      if(!mesh.order.length||mesh.order.some(function(f,i){return i>0&&mesh.faceDepth[mesh.order[i-1]]<mesh.faceDepth[f];}))frontMost=false;
    });});});
    check('all 70 style motion and yaw samples project finite geometry',count===70&&valid);
    check('visible faces are ordered far to near at every sample',frontMost);
    resetCommands();G.drawFirstPersonHandRig(1420,0,'reach','finger_guns');var playerBeforeStudy=commandHash();
    var studyGameplay=snapshot(),studySettings=JSON.stringify([G.HAND_RIG_PREVIEW,G.HAND_RIG_TIME_MS,G.HAND_RIG_VIEW_YAW,G.HAND_RIG_GRIP,G.HAND_RIG_STYLE]);
    var studyValid=true;
    styles.forEach(function(style){[0,1050,1420].forEach(function(time){
      resetCommands();G.drawFirstPersonHandRig(time,'palm','reach',style);
      if(!finite(mesh.positions)||!finite(mesh.normals)||!finite(mesh.screen)||!finite(mesh.light)||palmNormal(mesh,true)[2]<.9||!commands.some(function(c){return c[0]==='fill';}))studyValid=false;
    });});
    resetCommands();G.drawFirstPersonHandRig(1420,'palm','reach','finger_guns');var palmStudyHash=commandHash();
    resetCommands();G.drawFirstPersonHandRig(1420,'palm','reach','finger_guns');
    check('Palm study renders finite face-on geometry with deterministic Canvas commands',studyValid&&palmStudyHash===commandHash()&&palmStudyHash!==playerBeforeStudy);
    resetCommands();G.drawFirstPersonHandRig(1420,0,'reach','finger_guns');
    check('leaving Palm study restores the player render without changing gameplay or view settings',
      commandHash()===playerBeforeStudy&&snapshot()===studyGameplay&&stack.length===0&&studySettings===JSON.stringify([G.HAND_RIG_PREVIEW,G.HAND_RIG_TIME_MS,G.HAND_RIG_VIEW_YAW,G.HAND_RIG_GRIP,G.HAND_RIG_STYLE]));
    if(__argv.indexOf('--restore-arcane-palm-flip')>=0)G.HAND_RIG_ANIMATIONS.arcane.root[1]=[-.35,.40,.13];
    // Exercise the production draw/deformation path at50 Hz, including the
    // reported600–1080 ms interval and an explicit1050 ms maximum-gather sample.
    // Facing comes from actual posed triangle normals, not recipe Euler angles.
    var facingTimes=[],facingCount=0,firstFacingFailure=null;
    for(var facingTime=0;facingTime<=2800;facingTime+=20)facingTimes.push(facingTime);
    facingTimes.push(1050);facingTimes.sort(function(a,b){return a-b;});
    ['reach','handle','cradle'].forEach(function(grip){facingTimes.forEach(function(time){
      resetCommands();G.drawFirstPersonHandRig(time,0,grip,'arcane');facingCount++;
      // The upright staff grip is intentionally nearly edge-on; require its
      // palm to remain away without forcing the broad freehand-back silhouette.
      var palm=palmNormal(mesh,true),back=palmNormal(mesh,false),facingMargin=grip==='handle'?.05:.4;
      if((palm[2]>=-facingMargin||back[2]<=facingMargin)&&!firstFacingFailure)firstFacingFailure={time:time,grip:grip,palmarZ:palm[2],dorsalZ:back[2]};
    });});
    if(firstFacingFailure)__out('ARCANE_FACING_FIRST_FAILURE '+JSON.stringify(firstFacingFailure));
    check('Arcane palm stays away and dorsal surface stays toward camera throughout all grip cycles',
      facingCount===426&&facingTimes.indexOf(600)>=0&&facingTimes.indexOf(1080)>=0&&facingTimes.indexOf(1050)>=0&&!firstFacingFailure);
    check('drawing leaves gameplay and equipment unchanged',snapshot()===before);
    check('all sampled draws leave Canvas stack balanced',stack.length===0);
    G.canvas={width:1280,height:720};resetCommands();G.drawFirstPersonHandRig(800,.3,'reach');
    check('fullscreen preserves geometry resolution and topology',G.canvas.width===1280&&G.canvas.height===720&&G.buildHandRigMesh()===mesh&&finite(mesh.screen)&&commands.some(function(c){return c[0]==='fill';}));
    resetCommands();G.MODE3D=false;G.drawFirstPersonHandRig(800,0,'reach');G.MODE3D=true;G.shopOpen=true;G.drawFirstPersonHandRig(800,0,'reach');G.shopOpen=false;
    check('2D and shop views do not draw the proof',commands.length===0);
    G.clearHandRigCache();var cleared=G.getHandRigStats();
    check('clearing the cache releases the mesh and working-byte counters',G._handRigMesh===null&&cleared.vertices===0&&cleared.topologyBytes===0);
    var rebuilt=G.buildHandRigMesh();
    check('cache rebuild makes a fresh bounded mesh',rebuilt!==mesh&&rebuilt.desc.length===mesh.desc.length&&G.getHandRigStats().builds===1);
    __out('HAND_RIG_RESULT PASS '+checks);
  } finally {Math.random=oldRandom;}
}());
undefined;
