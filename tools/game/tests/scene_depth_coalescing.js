// Exact mask coverage and depth parity against the original scanline emitter.
(function () {
  var G=(0,eval)('this'), checked=0, rectangles=[], paints=0;
  G.ctx={beginPath:function(){rectangles=[];},rect:function(x,y,w,h){rectangles.push([x,y,w,h]);},
    fill:function(){paints++;},save:function(){},restore:function(){},clip:function(){}};
  (0,eval)(slurp('assets/games/src/12-scene-depth.js'));
  function check(name,okay){if(!okay)throw Error(name);checked++;__out('PASS '+name);}
  function legacyTrace(generation,minY,maxY) {
    G.ctx.beginPath();
    for(var y=minY;y<=maxY;y++) {
      if(G._sceneDepthClipRowMarks[y]!==generation)continue;
      var row=y*G._sceneDepthW,x=G._sceneDepthClipRowMin[y],end=G._sceneDepthClipRowMax[y];
      while(x<=end) {
        while(x<=end&&G._sceneDepthClipMarks[row+x]!==generation)x++;
        var start=x;
        while(x<=end&&G._sceneDepthClipMarks[row+x]===generation)x++;
        if(x>start)G.ctx.rect(start,y,x-start,1);
      }
    }
  }
  function referenceFill(points) {
    return G.withSceneDepthClip(points,function(){
      legacyTrace(G._sceneDepthClipGeneration,G._sceneDepthClipMinY,G._sceneDepthClipMaxY);G.ctx.fill();
    },{writeDepth:true,_paintsDepthPixels:true});
  }
  function quad(x0,y0,x1,y1,z){return [{x:x0,y:y0,depth:z},{x:x1,y:y0,depth:z},{x:x1,y:y1,depth:z},{x:x0,y:y1,depth:z}];}
  function coverage(rects,w,h) {
    var pixels=new Uint8Array(w*h);
    rects.forEach(function(r){for(var y=r[1];y<r[1]+r[3];y++)for(var x=r[0];x<r[0]+r[2];x++)pixels[y*w+x]++;});
    return pixels;
  }
  function same(a,b){return a.length===b.length&&a.every(function(v,i){return v===b[i];});}
  G.beginSceneDepthFrame(1920,1080);
  var visible=G.fillSceneDepthPolygon(quad(0,0,1920,1080,50));
  check('full-resolution rectangular face coalesces1080 scanlines into one exact rectangle',visible===1920*1080&&rectangles.length===1&&same(rectangles[0],[0,0,1920,1080]));
  check('coalescing leaves full-resolution corner depths intact',Math.abs(G.sceneDepthAt(0,0)-50)<1e-4&&Math.abs(G.sceneDepthAt(1919,1079)-50)<1e-4);
  G.beginSceneDepthFrame(30,20);G.writeSceneDepthPolygon(quad(0,10,30,20,5));
  var fillCount=paints;visible=G.fillSceneDepthPolygon(quad(4,3,26,18,20));
  check('partially buried face merges only exposed rows',visible===22*7&&rectangles.length===1&&same(rectangles[0],[4,3,22,7])&&paints===fillCount+1);
  check('partial coalescing preserves nearer terrain depth',Math.abs(G.sceneDepthAt(8,8)-20)<1e-4&&Math.abs(G.sceneDepthAt(8,15)-5)<1e-4);
  G.beginSceneDepthFrame(30,20);G.writeSceneDepthPolygon(quad(0,10,30,20,5));
  visible=G.withSceneDepthClip(quad(4,3,26,18,20),function(){});
  check('transparent clip retains original one-pixel rows for nativeCanvas alpha parity',visible===22*7&&rectangles.length===7&&rectangles.every(function(r,i){return same(r,[4,3+i,22,1]);}));
  G.beginSceneDepthFrame(20,20);G.writeSceneDepthPolygon(quad(0,8,20,12,5));
  visible=G.fillSceneDepthPolygon(quad(2,2,18,18,20));
  check('identical spans across hidden row gaps never merge over foreground',visible===16*12&&rectangles.length===2&&same(rectangles[0],[2,2,16,6])&&same(rectangles[1],[2,12,16,6]));
  G.beginSceneDepthFrame(20,20);G.writeSceneDepthPolygon(quad(8,0,12,20,5));
  visible=G.withSceneDepthClip(quad(2,2,18,18,20),function(){});
  var split=coverage(rectangles,20,20);
  check('partial transparent clip keeps two visible regions and their central hole',visible===12*16&&split[5*20+5]===1&&split[5*20+14]===1&&split[5*20+10]===0);
  G.beginSceneDepthFrame(20,20);G.writeSceneDepthPolygon(quad(0,10,20,20,5));
  var savedFill=G.ctx.fill,caught=false;
  G.ctx.fill=function(){throw Error('synthetic canvas failure');};
  try{G.fillSceneDepthPolygon(quad(0,0,20,20,10));}catch(_){caught=true;}
  G.ctx.fill=savedFill;
  check('failed Canvas fill still cannot commit opaque depth',caught&&G.sceneDepthAt(5,5)===Infinity&&Math.abs(G.sceneDepthAt(5,15)-5)<1e-4);
  var randomState=193,parity=true,oldCount=0,newCount=0;
  function random(){randomState=(Math.imul(randomState,1664525)+1013904223)|0;return(randomState>>>0)/4294967296;}
  for(var trial=0;trial<400;trial++) {
    G.beginSceneDepthFrame(96,64);
    for(var wall=0;wall<4;wall++) {
      var x=random()*96,y=random()*64;
      G.writeSceneDepthPolygon(quad(x,y,x+random()*40,y+random()*40,10+random()*30));
    }
    var triangle=[];
    for(var v=0;v<3;v++)triangle.push({x:random()*160-32,y:random()*110-22,depth:3+random()*100});
    var activeDepth=G._sceneDepthInv.subarray(0,96*64);
    var before=new Float32Array(activeDepth);
    rectangles=[];var oldVisible=referenceFill(triangle),oldRects=rectangles.slice(),oldDepth=new Float32Array(activeDepth);
    G._sceneDepthInv.set(before);
    rectangles=[];var newVisible=G.fillSceneDepthPolygon(triangle),newRects=rectangles.slice();
    if(oldVisible!==newVisible||!same(oldDepth,activeDepth)||!same(coverage(oldRects,96,64),coverage(newRects,96,64)))parity=false;
    oldCount+=oldRects.length;newCount+=newRects.length;
  }
  check('400 randomized occluded triangles match original pixel multiplicity and everyFloat32 depth',parity);
  check('coalescing decreases Canvas path operations without adding any rectangles',newCount<oldCount);
  __out('SCENE_DEPTH_COALESCING_RESULT PASS '+checked);
}());
undefined;
