// Production actor renderer + scene-depth clipping, without browser or audit
// lab dependencies. The Canvas host records painted pixels after real clips.
(function () {
  var G = (0, eval)('this'), checked = 0, W = 200, H = 160;
  var pixels = new Uint8Array(W * H), clipMask = null, paths = [], stack = [];
  var imageRects = [], callbacks = 0;
  function noop() {}
  function check(name, okay) {
    if (!okay) throw new Error(name);
    checked++; __out('PASS ' + name);
  }
  function rectPixels(x, y, w, h, draw) {
    var x0 = Math.max(0, Math.ceil(x - 0.5)), x1 = Math.min(W - 1, Math.floor(x + w - 0.5));
    var y0 = Math.max(0, Math.ceil(y - 0.5)), y1 = Math.min(H - 1, Math.floor(y + h - 0.5));
    for (var py = y0; py <= y1; py++) for (var px = x0; px <= x1; px++) draw(py * W + px);
  }
  function paintRect(x, y, w, h) {
    rectPixels(x, y, w, h, function (index) { if (!clipMask || clipMask[index]) pixels[index] = 1; });
  }
  G.ctx = {
    save: function () { stack.push(clipMask); }, restore: function () { clipMask = stack.pop(); },
    beginPath: function () { paths = []; },
    rect: function (x, y, w, h) { paths.push({x:x,y:y,w:w,h:h}); },
    ellipse: function (x, y, rx, ry) { paths.push({x:x-rx,y:y-ry,w:rx*2,h:ry*2}); },
    arc: function (x, y, r) { paths.push({x:x-r,y:y-r,w:r*2,h:r*2}); },
    clip: function () {
      var mask = new Uint8Array(W * H);
      paths.forEach(function (p) { rectPixels(p.x,p.y,p.w,p.h,function(i){if(!clipMask || clipMask[i])mask[i]=1;}); });
      clipMask = mask;
    },
    fill: function () { paths.forEach(function (p) { paintRect(p.x,p.y,p.w,p.h); }); },
    stroke: function () { paths.forEach(function (p) { paintRect(p.x,p.y,p.w,p.h); }); },
    fillRect: paintRect, moveTo: noop, lineTo: noop, closePath: noop,
    fillText: function (text, x, y) { paintRect(x - 3, y - 9, 6, 10); },
    drawImage: function () {
      var a = arguments, off = a.length === 9 ? 5 : 1;
      var r = {x:a[off],y:a[off+1],w:a[off+2],h:a[off+3]};
      imageRects.push(r); paintRect(r.x,r.y,r.w,r.h);
    }
  };
  (0, eval)(slurp('assets/games/src/06-floor-queries.js'));
  (0, eval)(slurp('assets/games/src/12-scene-depth.js'));
  if (__argv.indexOf('--omit-actor-depth') >= 0) {
    G.withSceneDepthBillboard=function(bounds,depth,drawCallback){drawCallback();return 1;};
  }
  (0, eval)(slurp('assets/games/src/14-render-entities.js'));
  // Load actual shared helpers without the unrelated device/UI initialization
  // at top level of configuration. Top-level function braces are column zero.
  var shared = slurp('assets/games/src/01-config-state.js');
  function loadFunction(name) {
    var start = shared.indexOf('function ' + name + '(');
    if (start < 0) throw new Error('missing production helper ' + name);
    var end = shared.indexOf('\n}', start) + 2;
    (0, eval)(shared.slice(start, end));
  }
  loadFunction('entityVisible3D');
  G.cam = {x:0,y:40,ang:0};
  G.projScale = 100; G.viewDist = 1000; G.playerUnderground = false;
  G.depthBuffer = new Float32Array(W); G.depthBuffer.fill(Infinity);
  G._cacheStats = {floorH:{hits:0,misses:0,size:0}};
  var C = {w:W,h:H,cosAng:1,sinAng:0,invTanHalf:1,horizonY:40,cameraZ:60};
  G.getCam3D = function () { return C; };
  G.SKEL_W=64; G.SKEL_H=80; G.WOLF_W=80; G.WOLF_H=52;
  G.SKEL_FRAMES=1; G.WOLF_FRAMES=1; G.getDirIndex=function(){return 0;};
  var sprite={}; G.skeletonFrames={normal:[[sprite]],normal_crumble:[[sprite]],tank:[[sprite]]};
  G.wolfFrames=[[sprite]]; G.DEBUG_SKELETON=false; G.ambientLight=1; G._skelScratch={};
  var normal={id:'normal',size:1,speed:20,chaseRange:500,color:'#ffffff',attackWindup:500};
  function mesh(layers) {
    G.floorMesh={w:1,h:1,gridSize:200,layerCount:[layers.length]};
    for(var i=0;i<5;i++) {
      G.floorMesh['l'+i+'Type']=[layers[i]?layers[i][0]:0];
      G.floorMesh['l'+i+'TopZ']=[layers[i]?layers[i][1]:0];
    }
  }
  function enemy(overrides) {
    var e={x:100,y:40,z:0,underground:false,enemyType:normal,health:0.5,maxHealth:1};
    Object.keys(overrides||{}).forEach(function(k){e[k]=overrides[k];});
    return e;
  }
  function quad(x0,y0,x1,y1,depth) {
    return [{x:x0,y:y0,depth:depth},{x:x1,y:y0,depth:depth},
      {x:x1,y:y1,depth:depth},{x:x0,y:y1,depth:depth}];
  }
  function reset() {
    G.beginSceneDepthFrame(W,H); pixels.fill(0); imageRects=[]; clipMask=null; paths=[]; stack=[];
    G.depthBuffer.fill(Infinity); G.playerUnderground=false; C.cameraZ=60;
    mesh([[1,0]]); G.enemies=[enemy()];
  }
  function draw() { G.drawEnemies3D(); }
  function painted(x,y) { return !!pixels[y*W+x]; }
  reset(); draw();
  check('unobstructed enemy head body and health bar remain visible',painted(100,50)&&painted(100,90)&&painted(100,36));
  check('sprite feet project from actual ground with no screen-space lift',imageRects.length===1&&imageRects[0].y+imageRects[0].h===100);
  check('transparent actor does not write opaque terrain depth',G.sceneDepthAt(100,50)===Infinity);
  reset(); G.writeSceneDepthPolygon(quad(0,70,W,H,50)); draw();
  check('foreground terrain hides lower body but retains visible head',painted(100,50)&&!painted(100,90));
  check('partial enemy clipping includes visible health bar above bank',painted(100,36));
  check('shadow cannot bleed through foreground terrain',!painted(100,102));
  check('partial clip restores canvas state',stack.length===0&&clipMask===null);
  reset(); G.writeSceneDepthPolygon(quad(0,0,W,H,50)); draw();
  check('opaque wall fully hides enemy and its health bar',!pixels.some(function(v){return !!v;})&&imageRects.length===0);
  reset(); G.writeSceneDepthPolygon(quad(0,0,W,H,200)); draw();
  check('terrain behind actor never hides its sprite',painted(100,50)&&painted(100,90));
  reset(); G.writeSceneDepthPolygon(quad(0,0,90,H,50)); G.writeSceneDepthPolygon(quad(110,0,W,H,50)); draw();
  check('doorway preserves central body while clipping both covered sides',painted(100,50)&&!painted(80,50)&&!painted(120,50));
  reset(); G.writeSceneDepthPolygon(quad(0,0,W,70,50)); draw();
  check('roof hides head and health bar without blanket-hiding exposed legs',!painted(100,50)&&!painted(100,36)&&painted(100,90));
  reset(); G.depthBuffer.fill(1); draw();
  check('obsolete wall-column depth cannot override full-height visibility',painted(100,50));
  reset(); mesh([[1,-4],[2,0],[4,2]]); draw();
  check('surface enemy uses surface roof instead of underground floor',imageRects.length===1&&imageRects[0].y+imageRects[0].h===50);
  var outsideBottom=imageRects[0].y+imageRects[0].h;
  G.playerUnderground=true; imageRects=[]; draw();
  check('camera underground flag cannot move surface actor to cave floor',imageRects.length===1&&imageRects[0].y+imageRects[0].h===outsideBottom);
  reset(); mesh([[1,-4],[2,0],[4,2]]); G.enemies=[enemy({underground:true,z:0})]; draw();
  check('explicit absolute cave Z zero is not replaced by lowest floor',imageRects.length===1&&imageRects[0].y+imageRects[0].h===100);
  reset(); mesh([[1,-4],[2,0],[4,2]]); C.cameraZ=-60; G.enemies=[enemy({underground:true,z:-100})]; draw();
  check('cave actor is visible from outside through an unobstructed portal',imageRects.length===1&&painted(100,40));
  var caveBottom=imageRects[0].y+imageRects[0].h;
  G.playerUnderground=true; imageRects=[]; draw();
  check('camera crossing portal does not shift cave actor feet',imageRects.length===1&&imageRects[0].y+imageRects[0].h===caveBottom);
  reset(); G.enemies=[enemy({health:1,slowUntil:Date.now()+5000,burnUntil:Date.now()+5000,
    aggroAt:Date.now()-1,attackState:'windup',attackStateUntil:Date.now()+250})];
  G.writeSceneDepthPolygon(quad(0,0,W,70,50)); draw();
  check('burn slow and aggro effects respect overhead occlusion',!painted(100,32)&&!painted(100,40)&&painted(100,90));
  reset(); G.enemies=[enemy({enemyType:{id:'wolf',size:1,speed:20,chaseRange:500,color:'#fff'}})];
  G.writeSceneDepthPolygon(quad(0,80,W,H,50)); draw();
  check('wide wolf sprite uses full pixel footprint for partial occlusion',painted(100,70)&&!painted(100,90));
  reset(); G.enemies=[enemy({enemyType:{id:'missing',size:1,speed:20,chaseRange:500,color:'#fff'}})];
  G.writeSceneDepthPolygon(quad(0,90,W,H,50)); draw();
  check('fallback actor shape obeys same partial terrain clip',painted(100,80)&&!painted(100,96));
  reset(); G.enemies=[];
  for(var n=0;n<513;n++)G.enemies.push(enemy({x:180-n*0.1,health:1}));
  draw();
  check('bounded actor sorting never aliases visible slot with scratch',imageRects.length===512&&stack.length===0);
  reset(); G.goal={x:98,y:38,w:4,h:4}; G.getScale3D=function(){return 1;}; G.drawGoalMarker3D();
  check('unobstructed goal marker remains visible on its actual ground',painted(100,99));
  reset(); G.writeSceneDepthPolygon(quad(0,0,W,H,50)); G.drawGoalMarker3D();
  check('goal marker no longer paints through opaque terrain',!pixels.some(function(v){return !!v;}));
  __out('ACTOR_VISIBILITY_RESULT PASS '+checked);
}());
undefined;
