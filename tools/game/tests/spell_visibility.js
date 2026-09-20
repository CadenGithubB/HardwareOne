// Shipping spell visibility contracts: production draw functions and depth
// rasterizer, with only a small Canvas host. No optional lab or browser needed.
(function(){
  var G=(0,eval)('this'),checked=0,draws=0,clips=0,saved=0,path=[],runs=[],lastRuns=[],arcs=[];
  function noop(){}
  G.ctx={save:function(){saved++;},restore:function(){saved--;},
    beginPath:function(){path=[];runs=[];},moveTo:function(x,y){path.push({x:x,y:y});},
    lineTo:function(x,y){path.push({x:x,y:y});},closePath:noop,
    rect:function(x,y,w,h){runs.push({x:x,y:y,w:w,h:h});},
    clip:function(){clips++;lastRuns=runs.slice();},
    arc:function(x,y,r){arcs.push({x:x,y:y,r:r});},ellipse:noop,
    fill:function(){draws++;},stroke:function(){draws++;},
    createLinearGradient:function(){return{addColorStop:noop};},
    globalAlpha:1,shadowBlur:0,lineWidth:1};
  (0,eval)(slurp('assets/games/src/12-scene-depth.js'));
  (0,eval)(slurp('assets/games/src/13-render-floors-ceilings.js'));
  (0,eval)(slurp('assets/games/src/14-render-entities.js'));
  var shared=slurp('assets/games/src/01-config-state.js');
  (0,eval)(shared.slice(shared.indexOf('function projToScreen('),shared.indexOf('// GENERIC 3D ENTITY RENDERER')));
  G.cam={x:0,y:100,z:60,ang:0,pitch:0};G.projScale=80;G.viewDist=700;
  var C={w:120,h:90,cosAng:1,sinAng:0,invTanHalf:1,horizonY:35,cameraZ:30};
  G.getCam3D=function(){return C;};G.getScale3D=function(){return 1;};
  G.resScale=1;G.playerUnderground=false;G.getPlayerFloorH=function(){return 0;};
  G.getFloorHeightAt=function(){return 0;};G.rgbQ=function(r,g,b){return'rgb('+r+','+g+','+b+')';};
  G.pos={x:8,y:100,floorZ:60};G.spells={fire:{streamWidth:0.4,tier:1}};
  G.depthBuffer=new Float32Array(120);G.depthBuffer.fill(1);
  function flatMesh(height){
    var mesh={w:40,h:30,gridSize:10,layerCount:new Uint8Array(1200),l0Type:new Uint8Array(1200),
      l0TopZ:new Float32Array(1200),l0Meta:new Uint8Array(1200)};
    mesh.layerCount.fill(1);mesh.l0Type.fill(1);mesh.l0TopZ.fill(height);mesh.l0Meta.fill(1);return mesh;
  }
  G.floorMesh=flatMesh(0);
  function check(name,okay){if(!okay)throw Error(name);checked++;__out('PASS '+name);}
  function wall(){G.writeSceneDepthPolygon([{x:0,y:0,depth:1},{x:120,y:0,depth:1},{x:120,y:90,depth:1},{x:0,y:90,depth:1}]);}
  function reset(hidden){G.beginSceneDepthFrame(120,90);draws=clips=0;arcs=[];lastRuns=[];if(hidden)wall();}
  var originalClip=G.withSceneDepthClip;
  if(__argv.indexOf('--bypass-spell-depth')>=0)G.withSceneDepthClip=function(points,draw){draw();return 1;};
  function visibleAndHidden(name,fn){
    reset(false);fn();var visibleDraws=draws;
    check(name+' is visible with clear terrain',visibleDraws>0);
    check(name+' does not write opaque depth',G.sceneDepthAt(60,50)===Infinity);
    reset(true);fn();check(name+' cannot paint through opaque terrain',draws===0);
    check(name+' restores Canvas state',saved===0);
  }
  ['fire','ice','lightning','arcane','tower','missile','poison'].forEach(function(id){
    G.projectiles=[{x:100,y:100,z:20,ang:0.2,spawnMs:Date.now()-100,spell:{id:id,color:'#ffaa00'}}];
    visibleAndHidden(id+' projectile',G.drawProjectiles3D);
  });
  [false,true].forEach(function(companion){
    G.impacts=[{x:100,y:100,z:0,spawnMs:Date.now()-50,lifeMs:400,isCompanionProj:companion,color:'#55ffaa'}];
    visibleAndHidden(companion?'companion impact':'normal impact',G.drawImpacts3D);
    reset(false);G.drawImpacts3D();
    check((companion?'companion':'normal')+' impact projects absolute zero height exactly once',arcs.length>0 && arcs[0].y===59);
  });
  ['fire','ice','poison'].forEach(function(id){
    G.groundEffects=[{x:100,y:100,radius:24,renderFloorZ:0,spawnMs:Date.now()-300,duration:2000,spellId:id}];
    visibleAndHidden(id+' ground patch',G.drawGroundEffects3D);
  });
  G.chainEffects=[{fromX:40,fromY:90,fromZ:20,targets:[{x:150,y:110,z:30}],spawnMs:Date.now()-30,lifeMs:300}];
  visibleAndHidden('chain lightning',G.drawChainEffects3D);
  G.novaEffects=[{x:80,y:100,radius:70,renderFloorZ:0,color:'#ffffff',spawnMs:Date.now()-150,lifeMs:400}];
  visibleAndHidden('nova ring',G.drawNovaEffects3D);
  ['fire','ice'].forEach(function(id){
    G.coneEffects=[{x:30,y:100,ang:0,range:100,halfAngle:0.25,renderFloorZ:0,spellId:id,spawnMs:Date.now()-30,lifeMs:300}];
    visibleAndHidden(id+' cone',G.drawConeEffects3D);
  });
  G.flameStreamLastTick=Date.now();G.flameStreamStartMs=Date.now()-400;G.flameStreamAng=0;G.flameStreamRange=140;
  visibleAndHidden('flame stream and embers',G.drawFlameStream3D);
  G.projectiles=[{x:-20,y:100,z:20,ang:0,spawnMs:Date.now()-100,spell:{id:'poison',cloudRadius:20},
    targetX:100,targetY:100,targetZ:0,underground:false}];
  visibleAndHidden('reticle with projectile behind camera',G.drawProjectiles3D);
  reset(false);
  G.writeSceneDepthPolygon([{x:0,y:50,depth:5},{x:120,y:50,depth:5},{x:120,y:90,depth:5},{x:0,y:90,depth:5}]);
  G.projectiles=[{x:100,y:100,z:15,ang:0,spawnMs:Date.now()-100,spell:{id:'fire',color:'#ffaa00'}}];
  G.drawProjectiles3D();
  check('partly buried orb draws through a pixel clip',draws>0 && clips>0);
  check('projectile clip keeps upper glow but excludes buried lower pixels',lastRuns.length>0 && lastRuns.every(function(r){return r.y<50;}) && lastRuns.some(function(r){return r.y<35;}));
  reset(false);var segmentPolygons=[],segmentCaps=[];
  G.withSceneDepthClip=function(points,callback,options){segmentPolygons.push(points);segmentCaps.push(options&&options.extraPolygons);return originalClip(points,callback,options);};
  var segmentDraws=0;
  G.drawSpellWorldSegment({x:20,y:95,z:25},{x:140,y:115,z:5},C,4,10,function(){segmentDraws++;});
  check('extended beam retains different endpoint depths instead of center billboard',segmentDraws>0 && segmentPolygons[0][0].depth===20 && segmentPolygons[0][1].depth===140 &&
    segmentCaps[0].length===2 && segmentCaps[0][0].every(function(p){return p.depth===20;}) && segmentCaps[0][1].every(function(p){return p.depth===140;}));
  reset(false);segmentPolygons=[];
  G.drawSpellWorldSegment({x:-20,y:100,z:20},{x:80,y:100,z:10},C,3,0,function(){segmentDraws++;});
  check('world beam crossing camera near plane retains its visible section',segmentPolygons.length===1 && segmentPolygons[0][0].depth===1 && segmentPolygons[0][1].depth===80);
  G.withSceneDepthClip=originalClip;
  reset(false);
  G.writeSceneDepthPolygon([{x:0,y:0,depth:60},{x:120,y:0,depth:60},{x:120,y:90,depth:60},{x:0,y:90,depth:60}]);
  var partialSegments=0;
  G.drawSpellWorldSegment({x:20,y:95,z:25},{x:140,y:115,z:5},C,4,0,function(){partialSegments++;});
  check('intervening terrain clips far segment while retaining near segment',partialSegments===1 && clips===1 && G._sceneDepthClipHidden>0 && G._sceneDepthClipCount>0);
  reset(false);G.playerUnderground=false;G.drawProjectiles3D();var outsideDraws=draws;
  reset(false);G.playerUnderground=true;G.drawProjectiles3D();G.playerUnderground=false;
  check('camera underground flag never changes spell visibility',outsideDraws>0 && draws===outsideDraws);
  var slope=flatMesh(0);
  for(var gy=0;gy<slope.h;gy++)for(var gx=0;gx<slope.w;gx++)slope.l0TopZ[gy*slope.w+gx]=gx*0.1+gy*0.05;
  G.floorMesh=slope;
  var effect={x:100,y:100,renderFloorZ:37.5};
  var patch=G.spellGroundTriangles(effect,90,90,110,110);
  check('ground patch conforms to actual sloped terrain triangles',patch.length>0 && patch.every(function(tri){return tri.every(function(v){return Math.abs(v.z-(v.x*0.25+v.y*0.125+0.8))<0.001;});}));
  var cache=effect._spellGroundCache;
  G.spellGroundTriangles(effect,92,92,108,108);
  check('fixed ground patch reuses camera-independent terrain geometry',effect._spellGroundCache===cache);
  G.floorMesh=flatMesh(-3);
  var changed=G.spellGroundTriangles(effect,90,90,110,110);
  check('ground geometry cache invalidates after world mesh rebuild',effect._spellGroundCache!==cache && changed[0][0].z===-74.2);
  var layered=flatMesh(-4);layered.l0Meta.fill(2);layered.layerCount.fill(2);
  layered.l1Type=new Uint8Array(1200);layered.l1Type.fill(4);layered.l1TopZ=new Float32Array(1200);layered.l1TopZ.fill(2);
  layered.l1Meta=new Uint8Array(1200);layered.l1Meta.fill(3);
  G.floorMesh=layered;
  var above=G.spellGroundTriangles({x:100,y:100,renderFloorZ:50},95,95,105,105);
  var below=G.spellGroundTriangles({x:100,y:100,renderFloorZ:-100,underground:true},95,95,105,105);
  check('surface spell stays on cap above cave instead of lowest floor',above.length>0 && above.every(function(tri){return tri.every(function(v){return v.z===50.8;});}));
  check('cave spell stays on its own underground support',below.length>0 && below.every(function(tri){return tri.every(function(v){return v.z===-99.2;});}));
  var missing=flatMesh(0);missing.layerCount[10*missing.w+11]=0;G.floorMesh=missing;
  check('ground decals reject missing stitched floor corners',G.spellGroundTriangles({x:100,y:100,renderFloorZ:0},100,100,109,109).length===0);
  G.floorMesh=flatMesh(0);G.cam.x=100;
  G.novaEffects=[{x:85,y:100,radius:100,renderFloorZ:0,color:'#aaffff',spawnMs:Date.now()-220,lifeMs:400}];
  reset(false);G.drawNovaEffects3D();
  check('nova can remain visible while its center is behind the camera',draws>0);
  check('all extended-effect paths leave Canvas balanced',saved===0);
  __out('SPELL_VISIBILITY_RESULT PASS '+checked);
}());
undefined;
