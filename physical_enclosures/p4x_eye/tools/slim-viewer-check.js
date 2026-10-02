
    (() => {
      const root = document.getElementById('p4x-slim-3d-viewer');
      const canvas = root.querySelector('#p4x-slim-3d-canvas');
      const ctx = canvas.getContext('2d');
      const control = Object.fromEntries(['view','explode','explode-value','clear','lid','pcb','in','out'].map(k => [k, root.querySelector('#p4x-slim-3d-'+k)]));
      if (!ctx) return;
      let state = {yaw:-0.38, pitch:0.50, explode:0, zoom:1, clear:true, lid:false, pcb:true, view:'angle'};
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
      sides(outside,0,26,'base'); sides(inside,2,26,'base',true);
      cap(outside,0,false,'base'); cap(inside,2,true,'base'); cap(outside,26,true,'base',[inside]);
      const bossCenters=[[3.6,3.6],[76.4,3.6],[3.6,52.4],[76.4,52.4]];
      for(const [x,y] of bossCenters) {
        const out=circle(x,y,2.6),hole=circle(x,y,0.8);
        sides(out,2,26,'base'); cap(out,26,true,'base',[hole]);
        sides(hole,18,26,'base',true); cap(hole,18,true,'base');
      }
      // Mount is part of the printed base; contrasting color highlights the change.
      const mount=[21.1,8.7],mountRing=circle(...mount,2.25),pilot=circle(...mount,0.6);
      sides(mountRing,2,18.7,'mount');cap(mountRing,18.7,true,'mount',[pilot]);
      sides(pilot,13.7,18.7,'mount',true);cap(pilot,13.7,true,'mount');
      solid([[20.3,2],[21.9,2],[21.9,8.7],[20.3,8.7]],2,6,'mount');
      solid(rounded(0,0,80,56,5),26,28,'lid',bossCenters.map(([x,y])=>circle(x,y,1.2)));
      solid(rounded(7.5,14,65,36,1),2.5,12.5,'battery');
      // Low locator rails are simplified; the battery envelope is exact.
      const rawPCB=[[0,0],[57.6,0],[57.6,5.6],[69,5.6],[69,32.6],[68.2,32.6],[68.2,37],[54,37],[54,38.3685],[52.759,43],[36.241,43],[35,38.3685],[35,37],[21.8,37],[21.3,36.5],[21.3,36.3],[20.8,35.8],[16.5,35.8],[16,36.3],[16,36.5],[15.5,37],[0,37]];
      solid(rawPCB.map(([x,y])=>[74.5-x,6.5+y]),18.7,20.3,'pcb',[circle(...mount,1)]);
      // View-only separation. At zero the positions match enclosure.scad.
      const lift={base:0,mount:0,battery:25,pcb:48,lid:65};
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
        palette={base:mix(bg,fg,0.32),lid:mix(bg,fg,0.28),mount:color('--viz-series-3'),pcb:color('--viz-series-1'),battery:color('--viz-series-2'),edge:mix(bg,fg,0.60)};
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
        const transformed=faces.filter(f=>(state.lid||f.part!=='lid')&&(state.pcb||f.part!=='pcb')).map(f=>({...f,loops:f.loops.map(loop=>loop.map(p=>transform(p,f.part))),n:rotateNormal(f.normal)}));
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
        const isTransparent=f=>state.clear&&(f.part==='base'||f.part==='lid');
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
        control.clear.checked=state.clear; control.lid.checked=state.lid;control.pcb.checked=state.pcb;control.view.value=state.view;
        control.explode.setAttribute('aria-valuetext',state.explode===0?'Assembled':Math.round(state.explode*100)+' percent separated');
      }
      function save() {
        if(window.openai?.setWidgetState) window.openai.setWidgetState({modelContent:{revision:1,caseMm:[80,56,28],batteryMm:[65,36,10],batteryShiftYmm:4,boardStandoffMm:[21.1,8.7],fitVerified:false},privateContent:{p4xSlim3d:state}}).catch(()=>{});
      }
      function restore(snapshot) {
        const s=snapshot?.privateContent?.p4xSlim3d;
        if(s&&typeof s==='object') {
          for(const [k,a,b] of [['yaw',-100,100],['pitch',-Math.PI/2,Math.PI/2],['explode',0,1],['zoom',0.65,2.5]]) if(Number.isFinite(s[k])) state[k]=clamp(s[k],a,b);
          for(const k of ['clear','lid','pcb']) if(typeof s[k]==='boolean') state[k]=s[k];
          if(['angle','top','side','bottom'].includes(s.view)) state.view=s.view;
        }
        syncControls();requestDraw();
      }
      control.explode.addEventListener('input',()=>{state.explode=Number(control.explode.value)/100;syncControls();requestDraw();});
      control.explode.addEventListener('change',save);
      for(const k of ['clear','lid','pcb']) control[k].addEventListener('change',()=>{state[k]=control[k].checked;requestDraw();save();});
      control.view.addEventListener('change',()=>{
        state.view=control.view.value;
        const angles={angle:[-0.38,0.50],top:[0,Math.PI/2],side:[0,0],bottom:[0,-Math.PI/2]};
        [state.yaw,state.pitch]=angles[state.view];state.zoom=1;requestDraw();save();
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
  