// Exact stitched corners remain static until mesh finalization. This exercises
// production helpers, including the existing buildWalkCandZ invalidation stamp.
(function () {
  var G=(0,eval)('this'),checked=0;
  if(!G.console)G.console={log:function(){}};
  (0,eval)(slurp('assets/games/src/13-render-floors-ceilings.js'));
  var config=slurp('assets/games/src/01-config-state.js'),start=config.indexOf('function buildWalkCandZ(m) {');
  if(start<0)throw Error('geometry finalization function missing');
  (0,eval)(config.slice(start,config.indexOf('\n}',start)+2));
  G._cacheStats={matchZ:{hits:0,misses:0,size:0,peakSize:0}};
  function check(name,okay){if(!okay)throw Error(name);checked++;__out('PASS '+name);}
  function mesh(w,h){
    var n=w*h,m={w:w,h:h,gridSize:12,layerCount:new Uint8Array(n),
      surfaceH:new Float64Array(n),walkCandZ:[],ceilAboveMask:new Uint8Array(n)};
    m.layerCount.fill(2);
    for(var k=0;k<5;k++){
      m['l'+k+'TopZ']=new Float64Array(n);m['l'+k+'Type']=new Uint8Array(n);m['l'+k+'Meta']=new Uint8Array(n);
    }
    m.l0Type.fill(1);m.l0Meta.fill(1);m.l1Type.fill(2);
    for(var i=0;i<n;i++){m.l0TopZ[i]=i;m.surfaceH[i]=i;m.l1TopZ[i]=5+i*0.1;}
    return m;
  }
  var m=mesh(2,2),cache=G.getFloorStitchCache(m);
  check('new mesh cache allocates no corner tiles eagerly',cache.bytes===0&&cache.floors.length===0&&cache.ceilings.length===0);
  var tile=G.getFloorStitchTile(m,cache,0,0,1,0,false);
  check('cold floor cache preserves all three exact neighbor heights',tile.values[0]===1&&tile.values[1]===2&&tile.values[2]===3);
  check('first lookup allocates one bounded tile not the whole mesh',cache.bytes===6400&&cache.misses===1);
  var again=G.getFloorStitchTile(m,cache,0,0,1,0,false);
  check('warm lookup reuses its tile and records real cache hits',again===tile&&cache.hits===1&&G._cacheStats.matchZ.hits===3&&G._cacheStats.matchZ.misses===3);
  var ceil=G.getFloorStitchTile(m,cache,0,1,0,5,true);
  check('floor and ceiling results never share an entry',ceil!==tile&&ceil.values[0]===5.1&&ceil.values[2]===5.3);
  check('ceiling matching preserves the strict 0.8 height tolerance',Number.isNaN(G.findCeilingRenderMatchZ(m,1,4))&&G.findCeilingRenderMatchZ(m,1,5)===5.1);
  var precise=mesh(2,2);precise.l0TopZ[1]=1+Math.pow(2,-40);
  var preciseTile=G.getFloorStitchTile(precise,G.getFloorStitchCache(precise),0,0,1,0,false);
  check('cache does not round corner precision down to Float32',preciseTile.values[0]===precise.l0TopZ[1]&&preciseTile.values[0]!==Math.fround(precise.l0TopZ[1]));
  var missing=mesh(2,2);missing.l0Type[1]=0;
  var missingCache=G.getFloorStitchCache(missing),missingTile=G.getFloorStitchTile(missing,missingCache,0,0,1,0,false);
  check('missing semantic neighbor remains NaN rather than a fabricated floor',Number.isNaN(missingTile.values[0]));
  G.getFloorStitchTile(missing,missingCache,0,0,1,0,false);
  check('missing joins are cached too without repeated scans',missingCache.hits===1&&missingCache.misses===1);
  m.l0TopZ[1]=7;G.buildWalkCandZ(m);
  var rebuilt=G.getFloorStitchCache(m),rebuiltTile=G.getFloorStitchTile(m,rebuilt,0,0,1,0,false);
  check('same-object geometry finalization invalidates stale corners',rebuilt!==cache&&rebuiltTile.values[0]===7&&rebuilt.hits===0);
  delete m._floorRenderStitches;
  check('explicit invalidation supports future geometry mutation paths',G.getFloorStitchCache(m)!==rebuilt);
  var resizeCache=G.getFloorStitchCache(m);m.w=1;
  check('dimension changes invalidate existing tile indexing',G.getFloorStitchCache(m)!==resizeCache);
  check('replacement meshes never inherit another worlds cached heights',G.getFloorStitchCache(mesh(2,2))!==G.getFloorStitchCache(m));
  var cap=G.FLOOR_STITCH_CACHE_BYTES;G.FLOOR_STITCH_CACHE_BYTES=6400;
  var large=mesh(32,32),bounded=G.getFloorStitchCache(large);
  G.getFloorStitchTile(large,bounded,0,0,1,0,false);
  var overflow=G.getFloorStitchTile(large,bounded,256,0,1,256,false);
  check('cache budget falls back to exact uncached geometry',bounded.bytes===6400&&bounded.fallbacks===1&&overflow===bounded.scratch&&overflow.values[0]===257&&overflow.values[1]===288&&overflow.values[2]===289);
  large.l0TopZ[257]=1234;
  overflow=G.getFloorStitchTile(large,bounded,256,0,1,256,false);
  check('budget fallback never mistakes scratch results for cached entries',bounded.fallbacks===2&&overflow.values[0]===1234&&bounded.bytes<=G.FLOOR_STITCH_CACHE_BYTES);
  G.FLOOR_STITCH_CACHE_BYTES=cap;
  var lightingMesh=mesh(2,2),staticCache=G.getFloorStitchCache(lightingMesh);
  G.renderSurfaceAmbient=.2;G.renderCaveAmbient=.5;G.playerUnderground=true;G.DEBUG_LAYER_TYPES=true;G.DEBUG_POLY_TYPES=true;
  check('camera lighting and debug changes do not invalidate static geometry',G.getFloorStitchCache(lightingMesh)===staticCache);
  check('default tile storage remains capped at four MiB',G.FLOOR_STITCH_CACHE_BYTES===4*1024*1024);
  __out('FLOOR_STITCH_CACHE_RESULT PASS '+checked);
}());
undefined;
