
    (() => {
      const root = document.getElementById('p4x-midframe-3d');
      const canvas = root.querySelector('#p4x-midframe-3d-canvas');
      const ctx = canvas.getContext('2d');
      const control = Object.fromEntries(['view','explode','explode-value','clear','lid','pcb','midframe','in','out'].map(k => [k, root.querySelector('#p4x-midframe-3d-'+k)]));
      if (!ctx) return;
      let state = {yaw:-0.38, pitch:0.50, explode:0.55, zoom:1, clear:true, lid:true, pcb:true, midframe:true, view:'angle'};
      let frame = 0;
      const raster=document.createElement('canvas');
      const rasterCtx=raster.getContext('2d');
      const faces = [];
      const paths = [];
      const clamp = (x,a,b) => Math.max(a, Math.min(b,x));
      const area = p => p.reduce((sum,q,i) => { const r=p[(i+1)%p.length]; return sum+q[0]*r[1]-r[0]*q[1]; },0)/2;
      const ccw = p => area(p)<0 ? [...p].reverse() : p;
      const rounded = (x,y,w,h,r,n=12) => {
        const p=[];
        for (const [cx,cy,start] of [[x+w-r,y+r,-90],[x+w-r,y+h-r,0],[x+r,y+h-r,90],[x+r,y+r,180]]) {
          for(let j=0;j<=n;j++) { const a=(start+j*90/n)*Math.PI/180; p.push([cx+r*Math.cos(a),cy+r*Math.sin(a)]); }
        }
        return p;
      };
      const circle = (x,y,r,n=24) => Array.from({length:n},(_,i) => [x+r*Math.cos(i*2*Math.PI/n), y+r*Math.sin(i*2*Math.PI/n)]);
      const at = (p,z) => p.map(q => [q[0],q[1],z]);
      function face(loops,normal,part) { faces.push({loops,normal,part}); }
      function line(loop,part) { paths.push({loop,part}); }
      function cap(outline,z,up,part,holes=[]) {
        face([at(outline,z),...holes.map(p=>at(p,z))], [0,0,up?1:-1],part);
        line(at(outline,z),part);
        for(const h of holes) line(at(h,z),part);
      }
      function sides(poly,z0,z1,part,invert=false) {
        for(let i=0;i<poly.length;i++) {
          const a=poly[i],b=poly[(i+1)%poly.length];
          const dx=b[0]-a[0],dy=b[1]-a[1],length=Math.hypot(dx,dy);
          if(length<1e-9) continue;
          const sign=invert?-1:1;
          face([[[a[0],a[1],z0],[b[0],b[1],z0],[b[0],b[1],z1],[a[0],a[1],z1]]],[sign*dy/length,-sign*dx/length,0],part);
        }
      }
      function solid(poly,z0,z1,part,holes=[]) {
        poly=ccw(poly); sides(poly,z0,z1,part);
        cap(poly,z0,false,part,holes); cap(poly,z1,true,part,holes);
        for(const h of holes) sides(ccw(h),z0,z1,part,true);
      }
      const outside=rounded(0,0,80,56,5),inside=rounded(2,2,76,52,3);
      // Upper-shell scallop. The battery shell below Z14.5 remains intact.
      const arc=Array.from({length:97},(_,i)=>{
        const a=-Math.PI*i/96;return [62.9+10.5*Math.cos(a),56+11.5*Math.sin(a)];
      });
      function clippedArc(top) {
        const result=[];
        for(let i=0;i<arc.length-1;i++) {
          const a=arc[i],b=arc[i+1];
          if(a[1]<=top+1e-8) result.push(a);
          if((a[1]>top&&b[1]<top)||(a[1]<top&&b[1]>top)) {
            const t=(top-a[1])/(b[1]-a[1]);result.push([a[0]+t*(b[0]-a[0]),top]);
          }
        }
        if(arc.at(-1)[1]<=top+1e-8) result.push(arc.at(-1));
        return result;
      }
      function notched(poly,top) {
        const result=[];
        poly.forEach((a,i)=>{
          const b=poly[(i+1)%poly.length];result.push(a);
          if(Math.abs(a[1]-top)<1e-8&&Math.abs(b[1]-top)<1e-8&&a[0]>73.4&&b[0]<52.4) result.push(...clippedArc(top));
        });
        return result;
      }
      const upperOutside=notched(outside,56),upperInside=notched(inside,54);
      function upperSides(poly,invert=false) {
        // Below Y54, arcs coincide: this is an opening, not a vertical wall.
        for(let i=0;i<poly.length;i++) {
          const a=poly[i],b=poly[(i+1)%poly.length];
          if(a[0]>=52.39&&a[0]<=73.41&&b[0]>=52.39&&b[0]<=73.41&&(a[1]+b[1])/2<54.00001) continue;
          const dx=b[0]-a[0],dy=b[1]-a[1],len=Math.hypot(dx,dy),sign=invert?-1:1;
          if(len<1e-9) continue;
          face([[[...a,14.5],[...b,14.5],[...b,27],[...a,27]]],[sign*dy/len,-sign*dx/len,0],'base');
        }
      }
      sides(outside,0,14.5,'base'); sides(inside,2,14.5,'base',true);
      upperSides(upperOutside);upperSides(upperInside,true);
      cap(outside,0,false,'base');cap(inside,2,true,'base');
      cap(upperOutside,27,true,'base',[upperInside]);
      // The frame is the finger-pocket floor; a short case lip covers its far seam.
      function convexClip(poly,clip) {
        for(let i=0;i<clip.length;i++) {
          const a=clip[i],b=clip[(i+1)%clip.length],dx=b[0]-a[0],dy=b[1]-a[1];
          const side=p=>dx*(p[1]-a[1])-dy*(p[0]-a[0]);
          const next=[];
          for(let j=0;j<poly.length;j++) {
            const p=poly[j],q=poly[(j+1)%poly.length],sp=side(p),sq=side(q);
            if(sp>=-1e-8) next.push(p);
            if((sp<0&&sq>0)||(sp>0&&sq<0)) {const t=sp/(sp-sq);next.push([p[0]+t*(q[0]-p[0]),p[1]+t*(q[1]-p[1])]);}
          }
          poly=next;
        }
        return poly;
      }
      solid(convexClip([[50.4,53.6],[75.4,53.6],[75.4,56],[50.4,56]],outside),13,14.5,'guard');
      const bossCenters=[[3.6,3.6],[76.4,3.6],[3.6,52.4],[76.4,52.4]];
      for(const [x,y] of bossCenters) {
        const out=circle(x,y,2.6),hole=circle(x,y,0.8);
        sides(out,2,27,'base'); cap(out,27,true,'base',[hole]);
        sides(hole,19,27,'base',true); cap(hole,19,true,'base');
      }
      const rect=(x,y,w,h)=>[[x,y],[x+w,y],[x+w,y+h],[x,y+h]];
      const mount=[21.1,8.7],rightMount=[58.9,8.7],pilot=circle(...mount,0.6);
      const leftShoulder=circle(...mount,3),core=circle(...mount,1.6);
      sides(leftShoulder,2,13,'mount');cap(leftShoulder,13,true,'mount',[core]);
      sides(core,13,19.4,'mount');cap(core,19.4,true,'mount',[pilot]);
      sides(pilot,14.4,19.4,'mountBore',true);cap(pilot,14.4,true,'mountBore');
      const rightShoulder=circle(...rightMount,3),rightPilot=circle(...rightMount,0.6);
      sides(rightShoulder,2,13,'mount');cap(rightShoulder,13,true,'mount',[rightPilot]);
      sides(rightPilot,8,13,'mountBore',true);cap(rightPilot,8,true,'mountBore');
      for(const [x,y] of [mount,rightMount]) solid(rect(x-.8,2,1.6,y-2),2,6,'mount');
      for(const [x,width] of [[10,10],[43,8]]) {
        solid(rect(x,51.5,width,2.5),11.5,13,'base');
        solid(rect(x,52.5,width,1.5),14.9,15.9,'base');
      }
      const midOutline=[[7,5],[73,5],[75.5,7.5],[75.5,48.8],[70.9,53.4],[9.1,53.4],[4.5,48.8],[4.5,7.5]];
      const frameClear=circle(...mount,1.9),frameScrew=circle(...rightMount,.9);
      solid(midOutline,13,14.5,'midframe',[frameClear,frameScrew]);
      solid(circle(...mount,3),14.5,19.7,'midframe',[frameClear]);
      for(const x of [10,72.5]) {
        solid(rect(x-1.5,42.7,3,1.2),14.5,19.7,'midframe');
        solid(rect(x-1.5,43.9,3,1.2),14.5,21.1,'midframe');
      }
      solid(rect(69,6.1,3,1.4),14.5,19.7,'midframe');
      solid(rect(69,5,3,1.1),14.5,21.1,'midframe');
      solid(rect(15.5,8,1,3.7),14.5,21.1,'midframe');
      solid(rect(8,10.7,8.5,1),14.5,21.1,'midframe');
      solid(upperOutside,27,29,'lid',bossCenters.map(([x,y])=>circle(x,y,1.2)));
      solid(rounded(7.5,14,65,36,1),2.5,12.5,'battery');
      // Low locator rails are simplified; the battery envelope is exact.
      const rawPCB=[[0,0],[57.6,0],[57.6,5.6],[69,5.6],[69,32.6],[68.2,32.6],[68.2,37],[54,37],[54,38.3685],[52.759,43],[36.241,43],[35,38.3685],[35,37],[21.8,37],[21.3,36.5],[21.3,36.3],[20.8,35.8],[16.5,35.8],[16,36.3],[16,36.5],[15.5,37],[0,37]];
      solid(rawPCB.map(([x,y])=>[74.5-x,6.5+y]),19.7,21.3,'pcb',[circle(...mount,1)]);
      // Manufacturer dial envelope. Small flutes identify the grippable rim.
      const wheel=Array.from({length:120},(_,i)=>{const a=i*2*Math.PI/120,r=i%4<2?7.25:7.05;return [62.9+r*Math.cos(a),39.35+r*Math.sin(a)];});
      solid(wheel,15.7,17.7,'wheel');
      solid(circle(62.9,39.35,4.2),17.7,19.7,'wheelHub');
      // View-only separation. At zero the positions match enclosure.scad.
      const lift={base:0,guard:0,mount:0,mountBore:0,battery:25,midframe:42,pcb:65,wheel:65,wheelHub:65,lid:90};
      const swatch=document.createElement('span');
      swatch.setAttribute('aria-hidden','true');
      swatch.style.cssText='position:absolute;width:0;height:0;overflow:hidden;pointer-events:none';
      root.appendChild(swatch);
      const sample=document.createElement('canvas'); sample.width=sample.height=1;
      const sampleCtx=sample.getContext('2d',{willReadFrequently:true});
      function color(variable) {
        swatch.style.color='var('+variable+')';
        sampleCtx.clearRect(0,0,1,1);
        sampleCtx.fillStyle=getComputedStyle(swatch).color;
        sampleCtx.fillRect(0,0,1,1);
        return Array.from(sampleCtx.getImageData(0,0,1,1).data).slice(0,3);
      }
      let palette;
      const mix=(a,b,t)=>a.map((v,i)=>v*(1-t)+b[i]*t);
      const rgb=(a,alpha=1)=>`rgba(${a.map(v=>Math.round(clamp(v,0,255))).join(',')},${alpha})`;
      function updateTheme() {
        const bg=color('--background'),fg=color('--foreground');
        palette={base:mix(bg,fg,0.32),guard:mix(bg,fg,0.38),lid:mix(bg,fg,0.28),midframe:color('--viz-series-3'),mount:mix(bg,fg,0.5),mountBore:mix(bg,fg,0.12),pcb:color('--viz-series-1'),wheel:color('--viz-series-4'),wheelHub:color('--viz-series-4').map(v=>v*0.65),battery:color('--viz-series-2'),edge:mix(bg,fg,0.60)};
        requestDraw();
      }
      function transform(p,part) {
        const x=p[0]-40,y=p[1]-28,z=p[2]+lift[part]*state.explode;
        const c=Math.cos(state.yaw),s=Math.sin(state.yaw),cp=Math.cos(state.pitch),sp=Math.sin(state.pitch);
        const u=c*x-s*y,v=s*x+c*y;
        return [u,-v*sp-z*cp,-v*cp+z*sp];
      }
      function rotateNormal(n) {
        const c=Math.cos(state.yaw),s=Math.sin(state.yaw),cp=Math.cos(state.pitch),sp=Math.sin(state.pitch);
        const u=c*n[0]-s*n[1],v=s*n[0]+c*n[1];
        return [u,-v*sp-n[2]*cp,-v*cp+n[2]*sp];
      }
      function draw() {
        frame=0;
        if(!palette) return;
        const w=canvas.clientWidth,h=canvas.clientHeight;
        if(w<1||h<1) return;
        const dpr=Math.min(window.devicePixelRatio||1,2);
        if(canvas.width!==Math.round(w*dpr)||canvas.height!==Math.round(h*dpr)) {canvas.width=Math.round(w*dpr);canvas.height=Math.round(h*dpr);}
        ctx.setTransform(dpr,0,0,dpr,0,0); ctx.clearRect(0,0,w,h);
        const transformed=faces.filter(f=>(state.lid||f.part!=='lid')&&(state.pcb||!['pcb','wheel','wheelHub'].includes(f.part))&&(state.midframe||f.part!=='midframe')).map(f=>({...f,loops:f.loops.map(loop=>loop.map(p=>transform(p,f.part))),n:rotateNormal(f.normal)}));
        const vertices=transformed.flatMap(f=>f.loops.flat());
        const bounds=[Infinity,Infinity,-Infinity,-Infinity];
        for(const [x,y] of vertices) {bounds[0]=Math.min(bounds[0],x);bounds[1]=Math.min(bounds[1],y);bounds[2]=Math.max(bounds[2],x);bounds[3]=Math.max(bounds[3],y);}
        const scale=Math.min((w-48)/(bounds[2]-bounds[0]),(h-48)/(bounds[3]-bounds[1]))*state.zoom;
        const ox=w/2-(bounds[0]+bounds[2])*scale/2,oy=h/2-(bounds[1]+bounds[3])*scale/2;
        const project=p=>[ox+p[0]*scale,oy+p[1]*scale];
        const renderFaces=transformed.filter(f=>f.n[2]>0.00001).map(f=>({...f,depth:f.loops[0].reduce((a,p)=>a+p[2],0)/f.loops[0].length})).sort((a,b)=>a.depth-b.depth);
        // Per-pixel depth keeps the solid lid in front of the PCB from every angle.
        // Scanline polygon filling also preserves the actual lid's screw holes.
        const rw=Math.round(w),rh=Math.round(h);
        raster.width=rw;raster.height=rh;
        const pixels=rasterCtx.createImageData(rw,rh),buffer=pixels.data;
        const depth=new Float32Array(rw*rh);depth.fill(-Infinity);
        const isTransparent=f=>state.clear&&(f.part==='base'||f.part==='lid'||f.part==='guard');
        function rasterFace(f,transparent) {
          const loops=f.loops.map(loop=>loop.map(project));
          let low=rh,high=0;
          for(const loop of loops) for(const p of loop) {low=Math.min(low,p[1]);high=Math.max(high,p[1]);}
          const y0=clamp(Math.ceil(low-0.5),0,rh),y1=clamp(Math.ceil(high-0.5),0,rh);
          const p0=f.loops[0][0],nx=f.n[0]/f.n[2],ny=f.n[1]/f.n[2];
          const dzdx=-nx/scale;
          const light=clamp(0.73+f.n[0]*-0.10+f.n[1]*-0.16+f.n[2]*0.14,0.48,1);
          const fill=palette[f.part].map(v=>Math.round(v*light));
          for(let y=y0;y<y1;y++) {
            const scan=y+0.5,intersections=[];
            for(const loop of loops) for(let i=0;i<loop.length;i++) {
              const a=loop[i],b=loop[(i+1)%loop.length];
              if((a[1]<=scan&&b[1]>scan)||(b[1]<=scan&&a[1]>scan)) intersections.push(a[0]+(scan-a[1])*(b[0]-a[0])/(b[1]-a[1]));
            }
            intersections.sort((a,b)=>a-b);
            for(let k=0;k+1<intersections.length;k+=2) {
              const x0=clamp(Math.ceil(intersections[k]-0.5),0,rw),x1=clamp(Math.ceil(intersections[k+1]-0.5),0,rw);
              let z=p0[2]-nx*((x0+0.5-ox)/scale-p0[0])-ny*((scan-oy)/scale-p0[1]);
              let index=y*rw+x0;
              for(let x=x0;x<x1;x++,index++,z+=dzdx) {
                if(z<depth[index]-0.0001) continue;
                const offset=index*4;
                if(!transparent) {depth[index]=z;buffer[offset]=fill[0];buffer[offset+1]=fill[1];buffer[offset+2]=fill[2];buffer[offset+3]=255;}
                else {
                  const alpha=0.24,previous=buffer[offset+3]/255,out=alpha+previous*(1-alpha);
                  for(let c=0;c<3;c++) buffer[offset+c]=(fill[c]*alpha+buffer[offset+c]*previous*(1-alpha))/out;
                  buffer[offset+3]=Math.round(out*255);
                }
              }
            }
          }
        }
        for(const f of renderFaces) if(!isTransparent(f)) rasterFace(f,false);
        for(const f of renderFaces) if(isTransparent(f)) rasterFace(f,true);
        rasterCtx.putImageData(pixels,0,0);ctx.drawImage(raster,0,0,w,h);
        // Outline-only overlay in transparent mode makes the shell readable.
        for(const p of paths) {
          const shell=p.part==='base'||p.part==='lid';
          if((p.part==='lid'&&!state.lid)||!shell||!state.clear) continue;
          ctx.strokeStyle=rgb(palette.edge,0.34); ctx.lineWidth=0.65;
          ctx.beginPath();p.loop.forEach((q,i)=>{const s=project(transform(q,p.part));i?ctx.lineTo(...s):ctx.moveTo(...s);});ctx.closePath();ctx.stroke();
        }
        canvas.dataset.rendered='true';
        canvas.dataset.faces=String(renderFaces.length);
      }
      function requestDraw() {if(!frame) frame=requestAnimationFrame(draw);}
      function syncControls() {
        control.explode.value=Math.round(state.explode*100);
        control['explode-value'].textContent=Math.round(state.explode*100)+'%';
        control.clear.checked=state.clear; control.lid.checked=state.lid;control.pcb.checked=state.pcb;control.midframe.checked=state.midframe;control.view.value=state.view;
        control.explode.setAttribute('aria-valuetext',state.explode===0?'Assembled':Math.round(state.explode*100)+' percent separated');
      }
      function save() {
        if(window.openai?.setWidgetState) window.openai.setWidgetState({modelContent:{revision:3,caseMm:[80,56,29],batteryMm:[65,36,10],batteryShiftYmm:4,boardStandoffMm:[21.1,8.7],midframeScrewMm:[58.9,8.7],midframeZmm:[13,14.5],sharedLeftBoardScrew:true,fingerPocketMm:[21,11.5],wheelDiameterMm:14.5,fitVerified:false},privateContent:{p4xMidframe3d:state}}).catch(()=>{});
      }
      function restore(snapshot) {
        const s=snapshot?.privateContent?.p4xMidframe3d;
        if(s&&typeof s==='object') {
          for(const [k,a,b] of [['yaw',-100,100],['pitch',-Math.PI/2,Math.PI/2],['explode',0,1],['zoom',0.65,2.5]]) if(Number.isFinite(s[k])) state[k]=clamp(s[k],a,b);
          for(const k of ['clear','lid','pcb','midframe']) if(typeof s[k]==='boolean') state[k]=s[k];
          if(['angle','frame','edge','top','side','bottom'].includes(s.view)) state.view=s.view;
        }
        syncControls();requestDraw();
      }
      control.explode.addEventListener('input',()=>{state.explode=Number(control.explode.value)/100;syncControls();requestDraw();});
      control.explode.addEventListener('change',save);
      for(const k of ['clear','lid','pcb','midframe']) control[k].addEventListener('change',()=>{state[k]=control[k].checked;requestDraw();save();});
      control.view.addEventListener('change',()=>{
        state.view=control.view.value;
        const angles={angle:[-.38,.50],frame:[-.2,.75],edge:[2.82,.64],top:[0,Math.PI/2],side:[Math.PI/2,0],bottom:[0,-Math.PI/2]};
        [state.yaw,state.pitch]=angles[state.view];state.zoom=1;
        if(state.view==='frame') Object.assign(state,{explode:.4,clear:true,lid:false,pcb:false,midframe:true});
        if(state.view==='edge') Object.assign(state,{explode:0,clear:false,lid:true,pcb:true,midframe:true});
        if(state.view==='angle') Object.assign(state,{explode:.55,clear:true,lid:true,pcb:true,midframe:true});
        syncControls();requestDraw();save();
      });
      function zoom(factor) {state.zoom=clamp(state.zoom*factor,0.65,2.5);requestDraw();}
      control.in.addEventListener('click',()=>{zoom(1.15);save();});
      control.out.addEventListener('click',()=>{zoom(1/1.15);save();});
      const pointers=new Map();let pinch=null;
      canvas.addEventListener('pointerdown',e=>{canvas.setPointerCapture(e.pointerId);pointers.set(e.pointerId,[e.clientX,e.clientY]);canvas.classList.add('dragging');pinch=null;});
      canvas.addEventListener('pointermove',e=>{
        if(!pointers.has(e.pointerId)) return;
        const last=pointers.get(e.pointerId);pointers.set(e.pointerId,[e.clientX,e.clientY]);
        if(pointers.size===2) {
          const [a,b]=[...pointers.values()],distance=Math.hypot(a[0]-b[0],a[1]-b[1]);
          if(pinch&&distance>0) zoom(distance/pinch);pinch=distance;
        } else {
          state.yaw+=(e.clientX-last[0])*0.009;
          state.pitch=clamp(state.pitch+(e.clientY-last[1])*0.007,-1.55,1.55);
          state.view='angle';control.view.value='angle';requestDraw();
        }
      });
      function endPointer(e) {pointers.delete(e.pointerId);pinch=null;if(!pointers.size) canvas.classList.remove('dragging');save();}
      canvas.addEventListener('pointerup',endPointer);canvas.addEventListener('pointercancel',endPointer);
      let wheelSave;
      canvas.addEventListener('wheel',e=>{e.preventDefault();zoom(Math.exp(-clamp(e.deltaY,-100,100)*0.003));clearTimeout(wheelSave);wheelSave=setTimeout(save,250);},{passive:false});
      new ResizeObserver(requestDraw).observe(canvas);
      new MutationObserver(updateTheme).observe(document.documentElement,{attributes:true,attributeFilter:['class','style','data-theme']});
      window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change',updateTheme);
      window.addEventListener('openai:set_globals',e=>{if(e.detail?.globals&&'widgetState' in e.detail.globals) restore(e.detail.globals.widgetState);updateTheme();});
      restore(window.openai?.widgetState);updateTheme();
    })();
  