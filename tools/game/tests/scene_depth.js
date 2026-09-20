// Shipping depth/partial-occlusion contracts. No browser or optional lab.
(function () {
  var G = (0, eval)('this');
  (0, eval)(slurp('assets/games/src/12-scene-depth.js'));
  var checked = 0, draws = 0, clips = 0, saved = 0, runs = [], path = [], fills = 0;
  G.ctx = {save: function () { saved++; }, restore: function () { saved--; },
    beginPath: function () { runs = []; path = []; }, rect: function (x, y, w, h) {
      // Compare pixel rows, independent of exact-output vertical coalescing.
      for (var ry = y; ry < y + h; ry++) runs.push({x:x,y:ry,w:w,h:1});
    },
    moveTo: function (x,y) { path.push({x:x,y:y}); }, lineTo: function (x,y) { path.push({x:x,y:y}); },
    closePath: function () {}, fill: function () { fills++; },
    clip: function () { clips++; }};
  function check(name, okay) {
    if (!okay) throw new Error(name);
    checked++; __out('PASS ' + name);
  }
  function near(a, b) { return Math.abs(a - b) < 0.0001; }
  function quad(x0, y0, x1, y1, depth) {
    return [{x:x0,y:y0,depth:depth},{x:x1,y:y0,depth:depth},
      {x:x1,y:y1,depth:depth},{x:x0,y:y1,depth:depth}];
  }
  function draw() { draws++; }
  G.beginSceneDepthFrame(10, 10);
  check('uncovered pixel has no depth', G.sceneDepthAt(2, 2) === Infinity);
  G.writeSceneDepthPolygon(quad(0, 5, 10, 10, 10));
  check('floor writes only its actual screen footprint', G.sceneDepthAt(3, 3) === Infinity && near(G.sceneDepthAt(3, 8),10));
  var visible = G.withSceneDepthClip(quad(2, 2, 8, 9, 20), draw);
  check('foreground floor hides only lower pillar pixels', visible === 18 && draws === 1 && clips === 1);
  check('partial mask contains upper pillar and no buried lower portion', runs.length === 3 && runs.every(function(r){return r.x===2 && r.w===6 && r.y>=2 && r.y<5;}));
  check('partial clipping restores canvas state', saved === 0);
  check('transparent draw never writes depth', G.sceneDepthAt(3,3) === Infinity && near(G.sceneDepthAt(3,8),10));
  G.withSceneDepthClip(quad(2, 2, 8, 9, 20), draw, {writeDepth:true});
  check('opaque face writes visible portion without replacing nearer floor', near(G.sceneDepthAt(3,3),20) && near(G.sceneDepthAt(3,8),10));
  var oldDraws = draws, oldClips = clips;
  check('fully occluded polygon skips drawing', G.withSceneDepthClip(quad(2,5,8,9,30),draw) === 0 && draws === oldDraws);
  check('fully occluded polygon creates no canvas clipping', clips === oldClips);
  G.beginSceneDepthFrame(10, 10);
  check('frame reset clears previous occluders', G.sceneDepthAt(3,8) === Infinity);
  oldClips=clips;
  check('fully visible face uses unclipped fast path', G.withSceneDepthClip(quad(0,0,10,10,12),draw,{writeDepth:true})===100 && clips===oldClips);
  G.writeSceneDepthPolygon(quad(4,0,6,10,6));
  visible=G.withSceneDepthClip(quad(0,0,10,10,10),draw);
  check('near wall separates two independently visible regions', visible===80 && runs.length===20 && runs.every(function(r){return r.w===4 && (r.x===0 || r.x===6);}));
  G.beginSceneDepthFrame(10,10);
  G.writeSceneDepthPolygon([{x:0,y:0,depth:10},{x:10,y:0,depth:20},{x:0,y:10,depth:40}]);
  var expected = 1/(0.5/10+0.25/20+0.25/40);
  check('depth interpolates reciprocal distance not linear distance', near(G.sceneDepthAt(2,2),expected));
  check('triangle does not cover exterior bounding-box pixels', G.sceneDepthAt(9,9)===Infinity);
  G.beginSceneDepthFrame(10,10);
  G.writeSceneDepthPolygon(quad(-100,-100,100,100,50).reverse());
  check('reversed winding and offscreen vertices rasterize safely', near(G.sceneDepthAt(0,0),50) && near(G.sceneDepthAt(9,9),50));
  check('outside canvas depth queries stay empty', G.sceneDepthAt(-1,0)===Infinity && G.sceneDepthAt(10,0)===Infinity && G.sceneDepthAt(NaN,2)===Infinity);
  oldDraws=draws;
  check('invalid depths never paint or poison buffer', !G.writeSceneDepthPolygon(quad(0,0,10,10,0)) && G.withSceneDepthClip(quad(0,0,10,10,NaN),draw)===0 && draws===oldDraws);
  G.beginSceneDepthFrame(20,12);
  G.writeSceneDepthPolygon(quad(15,5,20,12,3));
  check('larger canvas allocates complete depth coverage', near(G.sceneDepthAt(19,11),3) && G.sceneDepthAt(3,3)===Infinity);
  var buffer = G._sceneDepthInv;
  G.beginSceneDepthFrame(5,4);
  check('smaller canvas reuses allocation and clears active pixels', G._sceneDepthInv===buffer && G.sceneDepthAt(3,3)===Infinity && G.sceneDepthAt(5,0)===Infinity);
  G.writeSceneDepthPolygon(quad(0,0,5,4,10));
  check('default small bias tolerates nearly coplanar faces', G.withSceneDepthClip(quad(0,0,5,4,10.1),draw)===20);
  check('zero bias enforces strict depth order', G.withSceneDepthClip(quad(0,0,5,4,10.1),draw,{depthBias:0})===0);
  check('bias does not reveal a genuinely hidden face', G.withSceneDepthClip(quad(0,0,5,4,10.5),draw)===0);
  G.beginSceneDepthFrame(5,4);
  G.writeSceneDepthPolygon(quad(0,2,5,4,10));
  var caught=false;
  try { G.withSceneDepthClip(quad(0,0,5,4,20),function(){throw Error('draw failure');},{writeDepth:true}); }
  catch (_) { caught=true; }
  check('failed drawing restores clip and does not write opaque depth', caught && saved===0 && G.sceneDepthAt(0,0)===Infinity);
  G.cam={x:0,y:0}; G.projScale=50;
  var C={cosAng:1,sinAng:0,invTanHalf:1,w:100,h:100,horizonY:50,cameraZ:10};
  var points=G.projectSceneWorldPolygon([{x:-1,y:-2,z:0},{x:3,y:-2,z:0},{x:3,y:2,z:0},{x:-1,y:2,z:0}],C);
  check('near-plane crossing produces clipped face instead of disappearing', points.length===4 && points.every(function(p){return p.depth>=1 && Number.isFinite(p.x) && Number.isFinite(p.y);}) && points.filter(function(p){return p.depth===1;}).length===2);
  check('entirely behind-camera geometry projects to nothing', G.projectSceneWorldPolygon([{x:-3,y:-2,z:0},{x:-2,y:-2,z:0},{x:-2,y:2,z:0}],C).length===0);
  points=G.projectSceneWorldPolygon([{x:10,y:0,z:10},{x:10,y:2,z:10},{x:10,y:2,z:8}],C);
  check('projection shares camera forward-depth and renderer-height units', points.length===3 && near(points[0].x,50) && near(points[0].y,50) && near(points[2].y,60) && points[0].depth===10);
  G.beginSceneDepthFrame(720,480);
  check('scaled game canvas retains pixel-resolution visibility', G.withSceneDepthClip(quad(0,0,720,480,100),draw,{writeDepth:true})===720*480 && near(G.sceneDepthAt(719,479),100));
  G.beginSceneDepthFrame(10,10);
  check('opaque fill helper paints exact integer pixels and records their depth', G.fillSceneDepthPolygon(quad(2,3,7,9,5))===30 && fills===1 && runs.length===6 && runs.every(function(r){return r.x===2 && r.w===5 && r.h===1 && r.y>=3 && r.y<9;}) && near(G.sceneDepthAt(3,4),5));
  G.beginSceneDepthFrame(10,10);
  G.writeSceneDepthPolygon(quad(0,5,10,10,5));
  oldClips=clips;
  G.fillSceneDepthPolygon(quad(2,2,8,9,20));
  check('opaque pixel fill excludes buried portion without redundant Canvas clip', clips===oldClips && runs.length===3 && runs.every(function(r){return r.x===2 && r.w===6 && r.y>=2 && r.y<5;}) && near(G.sceneDepthAt(3,3),20) && near(G.sceneDepthAt(3,8),5));
  G.beginSceneDepthFrame(10,10);
  var first=[{x:0,y:0,depth:5},{x:10,y:0,depth:5},{x:10,y:10,depth:5}];
  var second=[{x:0,y:0,depth:5},{x:10,y:10,depth:5},{x:0,y:10,depth:5}];
  var painted=new Uint8Array(100), paintedDepthMatches=true;
  [first,second].forEach(function(triangle){
    G.fillSceneDepthPolygon(triangle);
    runs.forEach(function(r){for(var ry=r.y;ry<r.y+r.h;ry++)for(var rx=r.x;rx<r.x+r.w;rx++)painted[ry*10+rx]=1;});
    for(var ri=0;ri<100;ri++)if((G.sceneDepthAt(ri%10,Math.floor(ri/10))!==Infinity)!==!!painted[ri])paintedDepthMatches=false;
  });
  check('adjacent triangles paint every depth-covered pixel without diagonal cracks',paintedDepthMatches && painted.every(function(v){return v===1;}));
  var exact=true, randomState=127;
  function random() { randomState=(Math.imul(randomState,1664525)+1013904223)|0; return (randomState>>>0)/4294967296; }
  for(var trial=0;trial<80;trial++) {
    G.beginSceneDepthFrame(24,16);
    var triangle=[];
    for(var vertex=0;vertex<3;vertex++)triangle.push({x:random()*36-6,y:random()*28-6,depth:1+random()*100});
    G.writeSceneDepthPolygon(triangle);
    var a=triangle[0],b=triangle[1],c=triangle[2];
    var determinant=(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
    for(var ty=0;ty<16;ty++)for(var tx=0;tx<24;tx++) {
      var wa=((c.x-b.x)*(ty+0.5-b.y)-(c.y-b.y)*(tx+0.5-b.x))/determinant;
      var wb=((a.x-c.x)*(ty+0.5-c.y)-(a.y-c.y)*(tx+0.5-c.x))/determinant;
      var wc=1-wa-wb, actual=G.sceneDepthAt(tx,ty);
      if(wa>=-1e-10 && wb>=-1e-10 && wc>=-1e-10) {
        var reference=1/(wa/a.depth+wb/b.depth+wc/c.depth);
        if(Math.abs(actual-reference)>0.0001)exact=false;
      } else if(actual!==Infinity)exact=false;
    }
  }
  check('optimized scanlines match independent per-pixel barycentric reference',exact);
  G.beginSceneDepthFrame(0,0);
  check('empty canvas disables rendering without stale depth', G.sceneDepthAt(0,0)===Infinity && G.withSceneDepthClip(quad(0,0,1,1,1),draw)===0);
  __out('SCENE_DEPTH_RESULT PASS '+checked);
}());
undefined;
