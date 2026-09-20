// Exercise production loot draw functions and their actual depth clips. The
// browser host is inert; geometry/anchors/bounds/rendering are game source.
(function () {
  var G=(0,eval)('this'), checked=0, paints=0, saves=0, errors=[];
  var output=(__engine==='node'||__engine==='deno')?console.log.bind(console):__out;
  function noop() {}
  function stub(values) {return new Proxy(values||{}, {get:function(o,k){return k==='then'?undefined:k in o?o[k]:noop;}});}
  function paint(){paints++;}
  function gradient(){return {addColorStop:noop};}
  var context=stub({save:function(){saves++;},restore:function(){saves--;},
    fill:paint,stroke:paint,fillRect:paint,strokeRect:paint,fillText:paint,drawImage:paint,
    measureText:function(s){return {width:String(s).length*8};},
    createLinearGradient:gradient,createRadialGradient:gradient,createPattern:function(){return {};},
    createImageData:function(w,h){return {data:new Uint8ClampedArray(w*h*4)};},
    getImageData:function(x,y,w,h){return {data:new Uint8ClampedArray(w*h*4)};}});
  function canvas(){return stub({width:360,height:240,style:{},getContext:function(){return context;},
    getBoundingClientRect:function(){return {left:0,top:0,width:360,height:240};}});}
  var elements={maze:canvas()};
  function element(id){return elements[id]||(elements[id]=stub({id:id,checked:false,value:'',textContent:'',style:{},classList:stub(),dataset:{}}));}
  G.window=G;G.document=stub({getElementById:element,querySelectorAll:function(){return [];},querySelector:function(){return null;},
    createElement:function(tag){return tag==='canvas'?canvas():stub({style:{}});},body:stub({style:{}}),documentElement:stub({style:{}})});
  G.console={log:noop,info:noop,warn:noop,error:function(){errors.push(Array.prototype.join.call(arguments,' '));}};
  G.setTimeout=G.setInterval=G.requestAnimationFrame=function(){return 1;};
  G.clearTimeout=G.clearInterval=G.cancelAnimationFrame=G.addEventListener=G.removeEventListener=noop;
  G.innerWidth=1280;G.innerHeight=720;G.devicePixelRatio=1;
  G.hw={fetchJSON:function(){return new Promise(noop);},postFormText:function(){return new Promise(noop);},_auth401:noop};
  JSON.parse(slurp('assets/games/manifest.json')).parts.MAIN.forEach(function(path){(0,eval)(slurp('assets/games/'+path));});
  function check(name,okay){if(!okay)throw Error(name);checked++;output('PASS '+name);}
  function quad(x0,y0,x1,y1,depth){return [{x:x0,y:y0,depth:depth},{x:x1,y:y0,depth:depth},{x:x1,y:y1,depth:depth},{x:x0,y:y1,depth:depth}];}
  function frame(){G.beginSceneDepthFrame(360,240);paints=0;saves=0;}
  function cover(){G.writeSceneDepthPolygon(quad(0,0,360,240,10));}
  function mesh(heights,cap){
    var m={w:2,h:2,gridSize:200,layerCount:[cap?2:1,cap?2:1,cap?2:1,cap?2:1],
      surfaceH:cap?[3,3,3,3]:heights,ceilAboveMask:[0,0,0,0],
      l0Type:[1,1,1,1],l0Meta:cap?[2,2,2,2]:[1,1,1,1],l0TopZ:heights};
    if(cap){m.l1Type=[4,4,4,4];m.l1Meta=[3,3,3,3];m.l1TopZ=[3,3,3,3];}
    return m;
  }
  G.MODE3D=true;G.floorMesh=null;G.viewDist=1000;G.ambientLight=1;G.projScale=180;
  G.cam={x:0,y:0,z:60,ang:0,pitch:0,fov:Math.PI/2};G.depthBuffer=new Float32Array(360);
  var originalClip=G.withSceneDepthClip, clips=[];
  G.withSceneDepthClip=function(points,draw,options){
    var result;
    if(__argv.indexOf('--omit-loot-depth')>=0){draw();result=1;}
    else result=originalClip(points,draw,options);
    clips.push({points:points,result:result,hidden:G._sceneDepthClipHidden});return result;
  };
  function entity(extra){var e={x:120,y:0,renderFloorZ:0,bob:0};Object.keys(extra||{}).forEach(function(k){e[k]=extra[k];});return e;}
  var compType=Object.keys(G.COMPANION_DEFS)[0];
  var cases=[
    ['coins','coinDrops','drawCoins3D',entity()],
    ['soul orbs','soulOrbs','drawSoulOrbs3D',entity()],
    ['arcane tomes','arcaneTomes','drawArcaneTomes3D',entity()],
    ['stat pickups','statPickups','drawStatPickups3D',entity({type:'heartCrystal'})],
    ['companions','companions','drawCompanions3D',entity({type:compType,z:3,squash:1,lastAttackMs:0})],
    ['fortress allies','fortressAllies','drawFortressAllies3D',entity({phase:0,health:10,maxHealth:10})],
    ['floor scatter','floorScatter','drawFloorScatter3D',entity({type:'bones',variant:0,seed:0.4})],
    ['spawners','enemySpawners','drawEnemySpawners3D',entity({active:true,hp:5,maxHp:10,pulsePhase:0})]
  ];
  cases.forEach(function(c){
    G[c[1]]=[c[3]];G.playerUnderground=false;
    frame();cover();clips=[];G[c[2]]();
    check(c[0]+' including glow and label cannot paint through solid roof',paints===0&&clips.length>0);
    frame();clips=[];G[c[2]]();
    check(c[0]+' remains visible through an unobstructed doorway',paints>0&&clips.some(function(c){return c.result>0;})&&saves===0);
    check(c[0]+' transparent bounds do not write opaque depth',G.sceneDepthAt(180,150)===Infinity);
  });
  G.floorMesh=mesh([-2,-2,-2,-2],true);
  var loot={x:120,y:50,caveSpawnId:'seed:chamber:chest'};
  G.playerUnderground=false;var outside=G.getEntityRenderFloorZ(loot);
  G.playerUnderground=true;var inside=G.getEntityRenderFloorZ(loot);
  check('cave chest anchor does not change when camera crosses the mouth',outside===-50&&inside===outside);
  check('surface scatter chooses its own surface above a cave',G.getEntityRenderFloorZ({x:120,y:50})===75);
  check('explicit zero support is retained below a nonzero surface',G.getEntityRenderFloorZ({x:120,y:50,renderFloorZ:0})===0);
  G.floorMesh=mesh([0,2,1,3],false);
  check('loot follows the terrain triangle on sloped ground',Math.abs(G.getEntityRenderFloorZ({x:100,y:100})-37.5)<1e-8);
  G.floorMesh=null;G.playerUnderground=false;
  G.gridW=G.gridH=16;G.cell=12;G.grid=new Uint8Array(256);G.grid[4*16+10]=1;
  G.wallHeights=new Float32Array(256);G.wallHeights.fill(0.2);G.wallFaceBase=null;G.wallMaxTopZ=null;
  G.oreVeins=[{gx:10,gy:4,worldX:126,worldY:54,side:'west',veinType:'gold',hp:2}];G.cam.y=54;
  frame();cover();clips=[];G.drawOreVeins();
  check('ore and its halo stay behind solid terrain',paints===0&&clips.length===1);
  frame();clips=[];G.drawOreVeins();
  check('ore renders on the actual exposed wall face not its cell center',paints>0&&clips.length===1&&Math.abs(clips[0].points[0].depth-119.5)<1e-8&&saves===0);
  G.cam.y=0;
  G.treasureChests=[entity({tier:'epic',facing:0,lidAngle:0.5,opened:true,gold:10,seed:0.2})];
  frame();cover();clips=[];G.drawTreasureChests3D();
  check('hidden chest suppresses every box face glow sparkle lock and loot label',paints===0&&clips.length>=12&&saves===0);
  frame();clips=[];G.drawTreasureChests3D();
  var clearPaints=paints;
  check('open doorway preserves chest and its floating loot label',clearPaints>10&&clips.some(function(c){return c.result>0;}));
  check('chest face clipping uses distinct corner depths not a center billboard',clips.some(function(c){return c.points.length>2&&c.points.some(function(p){return Math.abs(p.depth-c.points[0].depth)>0.1;});}));
  frame();G.writeSceneDepthPolygon(quad(0,202,360,240,40));clips=[];G.drawTreasureChests3D();
  check('foreground slope clips buried chest portion without hiding exposed lid',paints>0&&clips.some(function(c){return c.hidden>0&&c.result>0;})&&saves===0);
  G.treasureChests=[entity({x:4,renderFloorZ:55,tier:'common',facing:0,lidAngle:0})];
  frame();clips=[];G.drawTreasureChests3D();
  check('near-plane chest keeps clipped faces when back corners are behind camera',paints>0&&clips.some(function(c){return c.points.some(function(p){return p.depth===1;});})&&clips.every(function(c){return c.points.every(function(p){return Number.isFinite(p.x)&&Number.isFinite(p.y)&&p.depth>=1;});})&&saves===0);
  var high=G.lootPickupRenderLayout('tome',{bob:0},{sx:180,sy:800,fwd:120},G.getCam3D(),0);
  check('offscreen loot stays anchored instead of being clamped back onto screen',high.centerY>600);
  check('tome depth bounds include its wide label',high.bounds.width>=context.measureText('Arcane Tome').width);
  G.gridW=G.gridH=30;G.grid=new Uint8Array(900);G.cell=12;
  for(var gy=0;gy<30;gy++)for(var gx=0;gx<30;gx++)if(gx===0||gx===29||gy===0||gy===29||gx===15)G.grid[gy*30+gx]=1;
  G.pos.x=G.pos.y=-1000;G.goal=null;G.shopMarker=null;G.currentBorderPoly=null;G.deepCaveEntrances=[];
  G.terrain='plains';G._caveGrid=null;G.deepCaveRegions=[];
  G.deepCaveChambers=[{cx:180,cy:180,radius:1000,ceilZ:0}];
  G.generateFloorScatter();
  var singles=G.floorScatter.filter(function(p){return !p.grouped&&!p.edge;}),
    groups=G.floorScatter.filter(function(p){return p.grouped;}),
    edges=G.floorScatter.filter(function(p){return p.edge;});
  check('fixed-mode single cave clutter retains its generation stratum',singles.length>0&&singles.every(function(p){return p.underground===true;}));
  check('fixed-mode grouped cave clutter retains its generation stratum',groups.length>0&&groups.every(function(p){return p.underground===true;}));
  check('fixed-mode edge cave clutter retains its generation stratum',edges.length>0&&edges.every(function(p){return p.underground===true;}));
  G.floorMesh=mesh([-2,-2,-2,-2],true);G.playerUnderground=false;
  var caveScatterZ=G.getEntityRenderFloorZ(singles[0]);G.playerUnderground=true;
  check('generated cave clutter remains on cave support from either camera stratum',caveScatterZ===-50&&G.getEntityRenderFloorZ(singles[0])===-50);
  G.deepCaveChambers=[];G.generateFloorScatter();
  check('fixed-mode surface clutter is not relabeled as underground',G.floorScatter.length>0&&G.floorScatter.every(function(p){return p.underground===false;}));
  check('actual renderer reports no swallowed errors',errors.length===0);
  output('LOOT_VISIBILITY_RESULT PASS '+checked);
}());
undefined;
