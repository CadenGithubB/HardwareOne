// Wall height is geometry: actual cave roofs remain covered without hiding
// neighboring surface walls or changing support/collision layers.
(function(){
  var G=(0,eval)('this'),output=(__engine==='node'||__engine==='deno')?console.log.bind(console):__out,checked=0;
  var host=slurp('tools/game/tests/cave_occlusion_integration.js');
  var end=host.indexOf('  function check(name, okay) {');
  if(end<0)throw Error('shipping browser host boundary missing');
  (0,eval)(host.slice(0,end)+'}());');
  function check(name,okay){if(!okay)throw Error(name);checked++;output('PASS '+name);}
  function mesh(w,h){
    var n=w*h,m={w:w,h:h,gridSize:12,layerCount:new Uint8Array(n),surfaceH:new Float32Array(n)};
    for(var k=0;k<3;k++){m['l'+k+'Type']=new Uint8Array(n);m['l'+k+'TopZ']=new Float32Array(n);}
    m.layerCount.fill(1);m.l0Type.fill(1);m.surfaceH.fill(3);m.l0TopZ.fill(3);return m;
  }
  function roof(m,i,z,cover){m.layerCount[i]=3;m.l0TopZ[i]=z-4;m.l1Type[i]=2;m.l1TopZ[i]=z;
    m.l2Type[i]=4;m.l2TopZ[i]=cover;m.surfaceH[i]=cover;}
  var synthetic=mesh(3,3);
  roof(synthetic,0,-.2,2);roof(synthetic,1,.2,2.3);roof(synthetic,3,.4,1.8);roof(synthetic,4,.1,1.9);
  check('sloped cover limits wall to actual roof rather than highest grass',Math.abs(G.getCaveWallRoofLimit(synthetic,0,0,12)-.4)<1e-6);
  synthetic=mesh(2,2);roof(synthetic,0,0,1);
  check('zero ceiling is a real covered roof',G.getCaveWallRoofLimit(synthetic,0,0,12)===0);
  roof(synthetic,0,-3,-2);synthetic.surfaceH.fill(-2);
  check('negative roofs and cover retain their absolute heights',G.getCaveWallRoofLimit(synthetic,0,0,12)===-3);
  synthetic=mesh(3,3);roof(synthetic,5,1,3);
  check('surface wall beside a cave is not shortened by a neighboring cap',G.getCaveWallRoofLimit(synthetic,0,0,12)===Infinity);
  roof(synthetic,4,.5,3);
  check('roof touching a wall corner still supplies genuine cave provenance',G.getCaveWallRoofLimit(synthetic,0,0,12)===.5);
  synthetic=mesh(3,3);roof(synthetic,0,1.1,3);synthetic.surfaceH[4]=1.2;
  check('all footprint vertices bound the cover safety gap',G.getCaveWallRoofLimit(synthetic,0,0,24)<=synthetic.surfaceH[4]-.24999);

  // Independent raw-layer envelope: do not call the production roof helper
  // to decide what its generated result should be allowed to draw.
  function envelope(m,gx,gy){
    var x0=Math.floor(gx*G.cell/m.gridSize),y0=Math.floor(gy*G.cell/m.gridSize);
    var x1=Math.min(m.w-1,Math.ceil((gx+1)*G.cell/m.gridSize));
    var y1=Math.min(m.h-1,Math.ceil((gy+1)*G.cell/m.gridSize));
    var ceil=-Infinity,surface=Infinity;
    for(var y=y0;y<=y1;y++)for(var x=x0;x<=x1;x++){
      var i=y*m.w+x;surface=Math.min(surface,m.surfaceH[i]);
      for(var li=0;li<m.layerCount[i];li++)if(m['l'+li+'Type'][i]===2)ceil=Math.max(ceil,m['l'+li+'TopZ'][i]);
    }
    return{ceil:ceil,surface:surface};
  }
  [[12345,'descending'],[5668,'hillside'],[6290,'hillside']].forEach(function(fixture){
    G._caveTestSeedOverride=fixture[0];G._caveTestKindOverride=fixture[1];
    G.CAVE_TEST_MODE=G.ENDLESS_MODE=G.running=true;G.gameOverState=false;G.terrain='plains';
    Object.keys(G.caveTestFlags).forEach(function(key){G.caveTestFlags[key]=false;});
    G.resetEndlessMode();G.previewCaveView('mouth');
    var m=G.floorMesh,label=fixture[0]+' '+fixture[1]+' ',covered=0,surface=0,buried=true,roofBound=true,untouched=true,closureNeeded=0;
    if(__argv.indexOf('--old-max-cap')>=0)for(var i=0;i<G.wallMaxTopZ.length;i++){
      if(Number.isFinite(G.wallCapZ[i]))G.wallMaxTopZ[i]=G.wallCapZ[i];
    }
    for(var gy=0;gy<G.gridH;gy++)for(var gx=0;gx<G.gridW;gx++){
      var i=gy*G.gridW+gx;if(!G.grid[i])continue;
      var bounds=envelope(m,gx,gy),limit=G.wallMaxTopZ[i];
      if(bounds.ceil>-Infinity){
        covered++;
        if(!Number.isFinite(limit)||limit>bounds.surface-.24999)buried=false;
        if(limit>bounds.ceil+.00001)roofBound=false;
        // At a hillside's open edge the lowest exterior corner may be below
        // the highest roof. Preserve a lower safe base; production wall-face
        // endpoint extensions, covered by the rendering suite, close it up.
        if(bounds.ceil<=bounds.surface-.25&&limit<bounds.ceil-.00001)roofBound=false;
        if(limit<bounds.ceil-.00001)closureNeeded++;
      }else{
        surface++;if(limit!==Infinity)untouched=false;
      }
    }
    check(label+'fixture contains the requested real entrance',G.endlessCaveNetworks['0,0'].entrances[0].kind===fixture[1]);
    check(label+'audit includes both cave and ordinary surface walls',covered>10&&surface>10);
    check(label+'cave walls stay strictly inside the terrain cover',buried);
    check(label+'wall envelope reaches roof wherever cover permits and never exceeds it',roofBound);
    check(label+'ordinary surface walls retain their original height',untouched);
    output('WALL_ENVELOPE_COUNTS '+JSON.stringify({seed:fixture[0],covered:covered,surface:surface,endpointClosureCells:closureNeeded}));
  });
  output('CAVE_WALL_ENVELOPE_RESULT PASS '+checked);
}());
undefined;
