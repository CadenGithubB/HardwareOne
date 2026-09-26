// Bounded cache contracts using a recording Canvas host; native pixels/timing
// are checked separately in the /artwork.html browser gallery.
(function() {
  var G = (0,eval)('this'), checks = 0, paints = 0, images = 0, created = [];
  function check(name, okay) { if (!okay) throw new Error(name); checks++; __out('PASS '+name); }
  function context() {
    var states = [], c = {globalAlpha:0.3, globalCompositeOperation:'source-over',
      shadowColor:'rgba(0, 0, 0, 0)', shadowBlur:0, shadowOffsetX:0, shadowOffsetY:0,
      filter:'none', lineCap:'butt', lineJoin:'miter', miterLimit:10,
      imageSmoothingEnabled:false, imageSmoothingQuality:'low',
      save:function(){states.push([this.globalAlpha,this.imageSmoothingEnabled,this.imageSmoothingQuality]);},
      restore:function(){var s=states.pop(); this.globalAlpha=s[0];this.imageSmoothingEnabled=s[1];this.imageSmoothingQuality=s[2];},
      scale:function(){}, getLineDash:function(){return [];},
      getTransform:function(){return {a:1,b:0,c:0,d:1,e:0,f:0};},
      drawImage:function(){images++; this.lastImage=Array.prototype.slice.call(arguments);this.drawAlpha=this.globalAlpha;}};
    return c;
  }
  G.document={createElement:function(){var c=context(), canvas={width:0,height:0,getContext:function(){return c;}};created.push(canvas);return canvas;}};
  G.GAME_MATERIALS = {};
  G.paintFloorItem=function(){paints++;};
  (0,eval)(slurp('assets/games/src/07-artwork-cache.js'));
  var c=context();
  function draw(type,seed,size){G.drawFloorArtwork(c,type||'fern',0,seed===undefined?0.25:seed,100.25,120.75,size||32);}
  G.beginFloorArtworkFrame(); draw();
  check('pilot starts opt-in with original rendering',paints===1 && images===0 && created.length===0);
  G.setFloorArtworkCacheEnabled(true); draw();
  check('cold supported prop builds one canvas and draws it',paints===2 && images===1 && G.getFloorArtworkCacheStats().entries===1);
  draw();
  check('warm draw reuses artwork without painting primitives',paints===2 && images===2 && G.getFloorArtworkCacheStats().hits===1);
  check('legacy intrinsic alpha is not faded twice',c.drawAlpha===1 && c.globalAlpha===0.3);
  check('caller smoothing state restored',c.imageSmoothingEnabled===false && c.imageSmoothingQuality==='low');
  var entry=G._floorArtworkCache.values().next().value;
  check('backing resolution is twice destination in each dimension',entry.canvas.width===entry.width*2 && entry.canvas.height===entry.height*2);
  check('anchor preserves fractional position',c.lastImage[1]===100.25+entry.left && c.lastImage[2]===120.75+entry.top);
  check('raw pixel bytes account for supersampling',G.getFloorArtworkCacheStats().pixelBytes===entry.canvas.width*entry.canvas.height*4);
  G.beginFloorArtworkFrame(); draw('fern',0.25000000000001);
  check('nearby exact seeds are not merged',G.getFloorArtworkCacheStats().entries===2);
  G.beginFloorArtworkFrame(); draw('fern',0.25,33);
  check('exact integer size participates in cache identity',G.getFloorArtworkCacheStats().entries===3);
  var count=created.length;
  draw('crate'); draw('toString'); draw('fern',-1);draw('fern',NaN);draw('fern',1);draw('fern',0.25,71);draw('fern',0.25,32.5);
  check('unsupported types seeds and sizes keep original path',created.length===count);
  c.globalCompositeOperation='lighter'; draw(); c.globalCompositeOperation='source-over';
  c.shadowBlur=2;draw();c.shadowBlur=0;
  c.shadowColor='red';draw();c.shadowColor='rgba(0, 0, 0, 0)';
  c.filter='blur(1px)';draw();c.filter='none';
  c.getTransform=function(){return {a:2,b:0,c:0,d:2,e:0,f:0};};draw();
  c.getTransform=function(){return {a:1,b:0,c:0,d:1,e:0,f:0};};
  c.getLineDash=function(){return [2,2];};draw();c.getLineDash=function(){return [];};
  check('nondefault compositing effects and transforms bypass cache',created.length===count);
  G.beginFloorArtworkFrame(); c.lineCap='round';draw();
  check('inherited stroke settings are included in identity',created.length===count+1);
  check('inherited stroke settings reach baked artwork',created[created.length-1].getContext().lineCap==='round');
  c.lineCap='butt';
  var old=created.slice();G.GAME_MATERIALS={bone:{}};G.beginFloorArtworkFrame();
  check('replacement material catalog invalidates all artwork',G.getFloorArtworkCacheStats().entries===0 && G.getFloorArtworkCacheStats().pixelBytes===0);
  check('invalidated canvas backing stores released',old.every(function(canvas){return canvas.width===1 && canvas.height===1;}));
  draw();G.paintFloorItem=function(){paints++;};G.beginFloorArtworkFrame();
  check('replacement drawing recipe invalidates artwork',G.getFloorArtworkCacheStats().entries===0);
  G.clearFloorArtworkCache();G.beginFloorArtworkFrame();G._floorArtworkBuildMs=1;count=created.length;draw();
  check('soft build-time limit falls back without creating canvas',created.length===count);
  G._floorArtworkBuildMs=0;G._floorArtworkBuilds=4;draw();
  check('build-count limit falls back without creating canvas',created.length===count);
  G.beginFloorArtworkFrame();draw();check('new frame resumes building',created.length===count+1);
  G.clearFloorArtworkCache();G.FLOOR_ARTWORK_CACHE_ENTRY_LIMIT=2;
  G.beginFloorArtworkFrame();draw('fern',0.1);G.beginFloorArtworkFrame();draw('fern',0.2);draw('fern',0.1);
  G.beginFloorArtworkFrame();draw('fern',0.3);var keys=Array.from(G._floorArtworkCache.keys());
  check('LRU evicts least recently drawn rather than newest',keys.length===2 && keys.some(function(k){return k.indexOf(':0.1:')>=0;}) && !keys.some(function(k){return k.indexOf(':0.2:')>=0;}));
  G.clearFloorArtworkCache();G.FLOOR_ARTWORK_CACHE_ENTRY_LIMIT=384;G.FLOOR_ARTWORK_CACHE_BYTE_LIMIT=60000;
  for(var i=0;i<50;i++){G.beginFloorArtworkFrame();draw('rubble',i/51,40);}
  check('long exploration stays inside byte budget',G.getFloorArtworkCacheStats().pixelBytes<=60000 && G.getFloorArtworkCacheStats().evictions>0);
  G.clearFloorArtworkCache();G.FLOOR_ARTWORK_CACHE_BYTE_LIMIT=1;G.beginFloorArtworkFrame();count=created.length;draw();
  check('oversized artwork never allocates',created.length===count && G.getFloorArtworkCacheStats().pixelBytes===0);
  G.FLOOR_ARTWORK_CACHE_BYTE_LIMIT=4*1024*1024;G.beginFloorArtworkFrame();draw();
  G.setFloorArtworkCacheEnabled(false);
  check('disabling releases retained artwork immediately',G.getFloorArtworkCacheStats().entries===0 && G.getFloorArtworkCacheStats().pixelBytes===0);
  G.setFloorArtworkCacheEnabled(true);G.document.createElement=function(){return {getContext:function(){return null;}};};G.beginFloorArtworkFrame();var before=paints;draw();
  check('failed Canvas allocation falls back to original rendering',paints===before+1 && G._floorArtworkFailed);
  G.document.createElement=function(){var art=context();return {width:0,height:0,getContext:function(){return art;}};};
  G.clearFloorArtworkCache();G.beginFloorArtworkFrame();
  (0,eval)(slurp('assets/games/src/12-scene-depth.js'));
  var rectangles=[],clipAtDraw=null;
  c.beginPath=function(){rectangles=[];};
  c.rect=function(x,y,w,h){rectangles.push([x,y,w,h]);};
  c.clip=function(){};
  c.drawImage=function(){images++;clipAtDraw=rectangles.slice();};
  G.ctx=c;
  G.beginSceneDepthFrame(40,40);
  G.writeSceneDepthPolygon([{x:0,y:20,depth:10},{x:40,y:20,depth:10},{x:40,y:40,depth:10},{x:0,y:40,depth:10}]);
  var visible=G.withSceneDepthBillboard({x:0,y:0,width:40,height:40},30,function(){draw();});
  check('cached draw remains inside production partial-depth clip',visible===800 && clipAtDraw.length>0 && clipAtDraw.every(function(r){return r[1]+r[3]<=20;}));
  var imageCount=images,buildCount=G.getFloorArtworkCacheStats().builds;
  G.withSceneDepthBillboard({x:0,y:20,width:40,height:20},30,function(){draw('rubble',0.3);});
  check('fully occluded cache candidates neither bake nor draw',images===imageCount && G.getFloorArtworkCacheStats().builds===buildCount);
  check('cached billboard never stamps transparent bounds into depth',G.sceneDepthAt(10,10)===Infinity && Math.abs(G.sceneDepthAt(10,30)-10)<0.00001);
  // Production scatter still uses the existing ground anchor, bounds and clip.
  var source=slurp('assets/games/src/07-decorations-lighting.js');
  check('production integration uses original scene visibility pipeline',source.indexOf('groundAnchor: true')>=0 && source.indexOf('drawFloorArtwork(ctx, item.type')>=0);
  __out('ARTWORK_CACHE_RESULT PASS '+checks);
}());
undefined;
