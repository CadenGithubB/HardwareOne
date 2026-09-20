// Shared entity-height/visibility contracts using the actual shipping helpers.
(function () {
  var G = (0, eval)('this'), n = 0;
  function check(name, okay) { if (!okay) throw new Error(name); n++; __out('PASS ' + name); }
  G.ctx = {save:function(){}, restore:function(){}, beginPath:function(){}, rect:function(){}, clip:function(){}};
  (0, eval)(slurp('assets/games/src/12-scene-depth.js'));
  (0, eval)(slurp('assets/games/src/13-render-floors-ceilings.js'));
  var source = slurp('assets/games/src/01-config-state.js');
  var start = source.indexOf('function entityVisible3D('), end = source.indexOf('function updateCollectibles(', start);
  check('production entity helpers found', start >= 0 && end > start);
  (0, eval)(source.slice(start, end));
  (0, eval)(slurp('assets/games/src/10-combat.js'));
  G.floorMesh = {gridSize:10,w:2,h:2,layerCount:[2,2,2,2],
    l0Type:[1,1,1,1], l0TopZ:[-5,-5,-5,-5], l0Meta:[2,2,2,2],
    l1Type:[4,4,4,4], l1TopZ:[0,1,2,4], l1Meta:[3,3,3,3]};
  check('surface support selects cap rather than buried cave', G.getEntityGroundRenderZ(0,0,false) === 0);
  check('cave support remains below its cap', G.getEntityGroundRenderZ(5,5,true) === -125);
  check('upper terrain triangle uses renderer diagonal', G.getEntityGroundRenderZ(7.5,2.5,false) === 37.5);
  check('lower terrain triangle uses renderer diagonal', G.getEntityGroundRenderZ(2.5,7.5,false) === 50);
  check('reference height preserves cave stratum', G.sampleEntitySupportRenderZ(5,5,-120,false) === -125);
  check('finite authored floor zero is valid', G.getEntityRenderFloorZ({x:5,y:5,renderFloorZ:0,underground:true}) === 0);
  check('cave drop provenance survives player outdoors', G.getEntityRenderFloorZ({x:5,y:5,caveSpawnId:'room:1'}) === -125);
  check('outside mesh stays missing', Number.isNaN(G.getEntityGroundRenderZ(-1,5,false)));
  G.floorMesh.layerCount[3] = 0;
  check('decals reject absent rendered triangle', Number.isNaN(G.sampleEntitySupportRenderZ(5,5,-125,true,true)));
  check('actors retain real cell support at mesh seam', G.sampleEntitySupportRenderZ(5,5,-125,true) === -125);
  G.floorMesh.layerCount[3] = 2;
  G.cam = {x:0,y:0}; G.viewDist=1000; G.projScale=100;
  G.getFloorHeightAt=function(){return -5;}; G.playerUnderground=false;
  G.depthBuffer=new Array(100).fill(1);
  var C={w:100,h:100,cosAng:1,sinAng:0,invTanHalf:1,horizonY:50,cameraZ:60};
  var v=G.entityVisible3D(100,0,0,C,{sceneDepth:true});
  check('absolute world zero never falls back to cave floor', v && v.sy===110 && v.renderZ===0);
  var a=G.entityVisible3D(100,0,-125,C,{sceneDepth:true});
  G.playerUnderground=true;
  var b=G.entityVisible3D(100,0,-125,C,{sceneDepth:true});
  check('camera state does not cull migrated cave entity', a && b && a.sy===b.sy);
  check('legacy column cannot cull partially visible actor', a && a.fwd===100);
  check('nonfinite position rejected', G.entityVisible3D(NaN,0,0,C,{sceneDepth:true})===null);
  G.beginSceneDepthFrame(20,20);
  G.writeSceneDepthPolygon([{x:0,y:10,depth:20},{x:20,y:10,depth:20},{x:20,y:20,depth:20},{x:0,y:20,depth:20}]);
  var called=0;
  var pixels=G.withSceneDepthBillboard({x:0,y:0,width:20,height:20},40,function(){called++;},{writeDepth:true});
  check('billboard preserves upper half above bank', pixels===200 && called===1);
  check('billboard does not write transparent bounding rectangle', G.sceneDepthAt(2,2)===Infinity);
  check('fully hidden billboard skips draw', G.withSceneDepthBillboard({x:0,y:10,width:20,height:10},40,function(){called++;})===0 && called===1);
  check('invalid or offscreen billboards are rejected', G.withSceneDepthBillboard({x:21,y:0,width:20,height:20},40,function(){called++;})===0);
  G.MODE3D=true; G.pos={x:0,y:0,floorZ:-100}; G.cam={pitch:0};
  G.getAimAngle=function(){return 0;}; G.getCurrentSpell=function(){return {id:'missile',speed:200};};
  G.equipment={}; G.PROJ_RADIUS=8; G.PROJ_LIFE_MS=1000; G.projectiles=[];
  G.console={log:function(){}};
  G.spawnProjectile();
  check('cave projectile hand height converts player units once', G.projectiles[0].z===-45 && G.projectiles[0].renderFloorZ===-100);
  G.pos.floorZ=0; G.spawnProjectile();
  check('zero stored player height does not reset to surface', G.projectiles[1].z===17.5);
  __out('ENTITY_DEPTH_RESULT PASS '+n);
}());
undefined;
