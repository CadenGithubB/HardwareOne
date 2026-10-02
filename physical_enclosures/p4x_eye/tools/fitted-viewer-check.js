
    (() => {
      const root = document.getElementById('p4x-fitted-midframe');
      const canvas = root.querySelector('#p4x-fitted-midframe-canvas');
      const ctx = canvas.getContext('2d');
      const control = Object.fromEntries(['view','explode','explode-value','clear','lid','pcb','midframe','in','out'].map(k => [k, root.querySelector('#p4x-fitted-midframe-'+k)]));
      if (!ctx) return;
      let state = {yaw:-1.35, pitch:0.45, explode:0, zoom:1, clear:false, lid:true, pcb:true, midframe:true, view:'usb'};
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
      function inPoly(p,poly) {
        let inside=false;
        for(let i=0,j=poly.length-1;i<poly.length;j=i++) {
          const a=poly[i],b=poly[j];
          if((a[1]>p[1])!==(b[1]>p[1])&&p[0]<(b[0]-a[0])*(p[1]-a[1])/(b[1]-a[1])+a[0])inside=!inside;
        }
        return inside;
      }
      function maskedSides(poly,z0,z1,part,masks,keep,invert=false) {
        const cross=(a,b)=>a[0]*b[1]-a[1]*b[0];
        for(let i=0;i<poly.length;i++) {
          const a=poly[i],b=poly[(i+1)%poly.length],r=[b[0]-a[0],b[1]-a[1]],len=Math.hypot(...r),cuts=[0,1];
          if(len<1e-9)continue;
          for(const mask of masks)for(let j=0;j<mask.length;j++) {
            const c=mask[j],d=mask[(j+1)%mask.length],s=[d[0]-c[0],d[1]-c[1]],den=cross(r,s),q=[c[0]-a[0],c[1]-a[1]];
            if(Math.abs(den)<1e-9)continue;
            const t=cross(q,s)/den,u=cross(q,r)/den;
            if(t>1e-8&&t<1-1e-8&&u>=-1e-8&&u<=1+1e-8)cuts.push(t);
          }
          cuts.sort((u,v)=>u-v);
          for(let j=0;j<cuts.length-1;j++) {
            if(cuts[j+1]-cuts[j]<1e-8)continue;
            const t=(cuts[j]+cuts[j+1])/2;
            if(!keep([a[0]+r[0]*t,a[1]+r[1]*t]))continue;
            const p=[a[0]+r[0]*cuts[j],a[1]+r[1]*cuts[j]],q=[a[0]+r[0]*cuts[j+1],a[1]+r[1]*cuts[j+1]],sgn=invert?-1:1;
            face([[[...p,z0],[...q,z0],[...q,z1],[...p,z1]]],[sgn*r[1]/len,-sgn*r[0]/len,0],part);
          }
        }
      }
      function differenceSolid(poly,z0,z1,part,holes) {
        poly=ccw(poly);
        const clipped=holes.map(h=>convexClip(h,poly)).filter(h=>h.length>=3);
        cap(poly,z0,false,part,clipped);cap(poly,z1,true,part,clipped);
        maskedSides(poly,z0,z1,part,holes,p=>!holes.some(h=>inPoly(p,h)));
        for(const h of holes)maskedSides(h,z0,z1,part,[poly],p=>inPoly(p,poly),true);
      }
      const outside=rounded(0,0,80,56,5),inside=rounded(2,2,76,52,3);
      // Contour only the upper shell. The lower battery half retains its full shape.
      const arc=(cx,cy,r,a0,a1,n=12)=>Array.from({length:n+1},(_,i)=>{
        const a=(a0+(a1-a0)*i/n)*Math.PI/180;return [cx+r*Math.cos(a),cy+r*Math.sin(a)];
      });
      const bezier=(a,b,c,d,t)=>a.map((v,i)=>(1-t)**3*v+3*(1-t)**2*t*b[i]+3*(1-t)*t*t*c[i]+t**3*d[i]);
      const upperOutside=[...arc(5,5,5,180,270),...arc(75,5,5,270,360),...arc(75,40.5,5,0,90),[59,45.5],
        ...Array.from({length:24},(_,i)=>bezier([59,45.5],[50,45.5],[48,56],[38,56],(i+1)/24)),...arc(5,51,5,90,180)];
      function inset(poly,d) {
        return poly.map((p,i)=>{
          const a=poly[(i+poly.length-1)%poly.length],b=poly[(i+1)%poly.length];
          const u=[p[0]-a[0],p[1]-a[1]],v=[b[0]-p[0],b[1]-p[1]],lu=Math.hypot(...u),lv=Math.hypot(...v);
          const n=[-u[1]/lu,u[0]/lu],m=[-v[1]/lv,v[0]/lv],k=d/(1+n[0]*m[0]+n[1]*m[1]);
          return [p[0]+k*(n[0]+m[0]),p[1]+k*(n[1]+m[1])];
        });
      }
      const upperInside=inset(upperOutside,1.6);
      const usbWindows=[17.5,32.5].map(y=>rounded(y-6,19.68,12,6.5,1,12));
      function upperSides(poly,invert=false) {
        // Split wall edges at the wheel aperture. Slot corners are simplified here.
        for(let i=0;i<poly.length;i++) {
          const a=poly[i],b=poly[(i+1)%poly.length];
          const dx=b[0]-a[0],dy=b[1]-a[1],len=Math.hypot(dx,dy),sign=invert?-1:1;
          if(len<1e-9) continue;
          if(Math.abs(dx)<1e-8&&a[0]>78&&dy>30) {
            const outer=[[a[0],a[1],14.5],[b[0],b[1],14.5],[b[0],b[1],27],[a[0],a[1],27]];
            face([outer,...usbWindows.map(loop=>loop.map(([y,z])=>[a[0],y,z]))],[sign,0,0],'lid');
            continue;
          }
          const cuts=[0,1];
          if(Math.abs(dx)>1e-9) for(const x of [56.9,68.9]) {const t=(x-a[0])/dx;if(t>0&&t<1)cuts.push(t);}
          cuts.sort((x,y)=>x-y);
          for(let j=0;j<cuts.length-1;j++) {
            const p=[a[0]+dx*cuts[j],a[1]+dy*cuts[j]],q=[a[0]+dx*cuts[j+1],a[1]+dy*cuts[j+1]];
            const slot=(p[0]+q[0])/2>56.9&&(p[0]+q[0])/2<68.9&&(p[1]+q[1])/2>42.5;
            for(const [z0,z1] of slot?[[18.1,27]]:[[14.5,27]])
              face([[[...p,z0],[...q,z0],[...q,z1],[...p,z1]]],[sign*dy/len,-sign*dx/len,0],'lid');
          }
        }
      }
      sides(outside,0,14.5,'base'); sides(inside,2,14.5,'base',true);
      upperSides(upperOutside);upperSides(upperInside,true);
      cap(outside,0,false,'base');cap(inside,2,true,'base');
      cap(outside,14.5,true,'base',[inside]);
      // Open-bottom wheel notch lets the entire upper cover lift off vertically.
      // The narrow wall-bottom faces beside the notch are represented by the side mesh.
      cap(upperOutside,27,true,'lid',[upperInside]);
      const slotClip=[[56.9,42.5],[68.9,42.5],[68.9,54],[56.9,54]];
      const slotOut=convexClip(upperOutside,slotClip),slotIn=convexClip(upperInside,slotClip);
      cap(slotOut,18.1,false,'lid',[slotIn]);
      function topY(poly,x) {
        const ys=[];
        for(let i=0;i<poly.length;i++){const a=poly[i],b=poly[(i+1)%poly.length];if((a[0]<=x&&b[0]>x)||(b[0]<=x&&a[0]>x))ys.push(a[1]+(x-a[0])*(b[1]-a[1])/(b[0]-a[0]));}
        return Math.max(...ys);
      }
      for(const [x,nx] of [[56.9,1],[68.9,-1]]) {
        const yi=topY(upperInside,x),yo=topY(upperOutside,x);
        face([[[x,yi,14.5],[x,yo,14.5],[x,yo,18.1],[x,yi,18.1]]],[nx,0,0],'lid');
      }
      for(const loop of usbWindows) {
        for(let i=0;i<loop.length;i++) {
          const a=loop[i],b=loop[(i+1)%loop.length],dy=b[0]-a[0],dz=b[1]-a[1],len=Math.hypot(dy,dz);
          if(len<1e-9)continue;
          face([[[78.4,...a],[80,...a],[80,...b],[78.4,...b]]],[0,-dz/len,dy/len],'lid');
        }
        line(loop.map(([y,z])=>[80,y,z]),'lid');
      }
      // Convex clipping keeps relief holes and support ledges within their footprint.
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
      const bossCenters=[[3.6,3.6,2.6],[76.4,3.6,2.6],[3.6,52.4,2.6],[77.2,41.8,2.2]];
      for(const [x,y,r] of bossCenters) {
        const out=circle(x,y,r),hole=circle(x,y,0.8);
        sides(out,2,14.5,'base'); cap(out,14.5,true,'base',[hole]);
        sides(hole,6.5,14.5,'base',true); cap(hole,6.5,true,'base');
        solid(out,14.5,27,'lid',[circle(x,y,1.2)]);
      }
      const rect=(x,y,w,h)=>[[x,y],[x+w,y],[x+w,y+h],[x,y+h]];
      const mount=[21.1,8.7],rightMount=[58.9,8.7],thirdMount=[5.5,46],pilot=circle(...mount,0.6);
      const leftShoulder=circle(...mount,3),core=circle(...mount,1.6);
      sides(leftShoulder,2,13,'mount');cap(leftShoulder,13,true,'mount',[core]);
      sides(core,13,19.4,'mount');cap(core,19.4,true,'mount',[pilot]);
      sides(pilot,14.4,19.4,'mountBore',true);cap(pilot,14.4,true,'mountBore');
      const rightShoulder=circle(...rightMount,3),rightPilot=circle(...rightMount,0.6);
      sides(rightShoulder,2,13,'mount');cap(rightShoulder,13,true,'mount',[rightPilot]);
      sides(rightPilot,8,13,'mountBore',true);cap(rightPilot,8,true,'mountBore');
      const thirdShoulder=circle(...thirdMount,2.75),thirdPilot=circle(...thirdMount,.6);
      sides(thirdShoulder,2,13,'mount');cap(thirdShoulder,13,true,'mount',[thirdPilot]);
      sides(thirdPilot,8,13,'mountBore',true);cap(thirdPilot,8,true,'mountBore');
      solid(rect(2,45.2,3.5,1.6),2,6,'mount');
      for(const [x,y] of [mount,rightMount]) solid(rect(x-.8,2,1.6,y-2),2,6,'mount');
      solid(rounded(1.99,1.99,76.02,52.02,3.01),11.5,13,'base',[rounded(3.4,3.4,73.2,49.2,1.6)]);
      const packKeepout=rect(8.55,11.8,66.4,37.4);
      for(const [x,y,r] of bossCenters) differenceSolid(convexClip(circle(x,y,r+.7),outside),11.5,13,'base',[packKeepout]);
      const midOutline=rounded(2.3,2.3,75.4,51.4,2.7);
      const frameClear=circle(...mount,1.9),frameScrew=circle(...rightMount,.9),thirdScrew=circle(...thirdMount,.9);
      const frameReliefs=bossCenters.map(([x,y,r])=>circle(x,y,r+.3));
      differenceSolid(midOutline,13,14.5,'midframe',[frameClear,frameScrew,thirdScrew,...frameReliefs]);
      solid(circle(...mount,3),14.5,19.7,'midframe',[frameClear]);
      for(const x of [10,72.5]) {
        solid(rect(x-1.5,42.7,3,1.2),14.5,19.7,'midframe');
        if(x<50) solid(rect(x-1.5,43.9,3,1.2),14.5,21.1,'midframe');
      }
      solid(rect(69,6.1,3,1.4),14.5,19.7,'midframe');
      solid(rect(69,5,3,1.1),14.5,21.1,'midframe');
      solid(rect(15.5,8,1,3.7),14.5,21.1,'midframe');
      solid(rect(8,10.7,8.5,1),14.5,21.1,'midframe');
      solid(upperOutside,27,29,'lid',bossCenters.map(([x,y])=>circle(x,y,1.2)));
      solid(rounded(9.25,12.5,65,36,1),2.5,12.5,'battery');
      // Low locator rails are simplified; the battery envelope is exact.
      const rawPCB=[[0,0],[57.6,0],[57.6,5.6],[69,5.6],[69,32.6],[68.2,32.6],[68.2,37],[54,37],[54,38.3685],[52.759,43],[36.241,43],[35,38.3685],[35,37],[21.8,37],[21.3,36.5],[21.3,36.3],[20.8,35.8],[16.5,35.8],[16,36.3],[16,36.5],[15.5,37],[0,37]];
      solid(rawPCB.map(([x,y])=>[74.5-x,6.5+y]),19.7,21.3,'pcb',[circle(...mount,1)]);
      // Manufacturer dial envelope. Small flutes identify the grippable rim.
      const wheel=Array.from({length:120},(_,i)=>{const a=i*2*Math.PI/120,r=i%4<2?7.25:7.05;return [62.9+r*Math.cos(a),39.35+r*Math.sin(a)];});
      solid(wheel,15.7,17.7,'wheel');
      solid(circle(62.9,39.35,4.2),17.7,19.7,'wheelHub');
      // Simplified USB receptacles: true outside envelope and face position.
      function alongX(poly,x0,x1,part,holes=[]) {
        const fi=faces.length,pi=paths.length;solid(poly,x0,x1,part,holes);
        for(let i=fi;i<faces.length;i++){faces[i].loops=faces[i].loops.map(loop=>loop.map(p=>[p[2],p[0],p[1]]));const n=faces[i].normal;faces[i].normal=[n[2],n[0],n[1]];}
        for(let i=pi;i<paths.length;i++)paths[i].loop=paths[i].loop.map(p=>[p[2],p[0],p[1]]);
      }
      for(const y of [17.5,32.5]) {
        alongX(rect(y-4.47,21.3,8.94,3.26),68.65,74,'usb');
        alongX(rect(y-4.47,21.3,8.94,3.26),74,76,'usb',[rect(y-4.05,21.8,8.1,2.26)]);
        face([rect(y-4.05,21.8,8.1,2.26).map(([yy,z])=>[74.01,yy,z])],[1,0,0],'usbBore');
      }
      // View-only separation. At zero the positions match enclosure.scad.
      const lift={base:0,mount:0,mountBore:0,battery:25,midframe:42,pcb:65,wheel:65,wheelHub:65,usb:65,usbBore:65,lid:90};
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
        palette={base:mix(bg,fg,0.32),lid:mix(bg,fg,0.28),midframe:color('--viz-series-3'),mount:color('--viz-series-3'),mountBore:mix(bg,fg,0.12),pcb:color('--viz-series-1'),wheel:color('--viz-series-4'),wheelHub:color('--viz-series-4').map(v=>v*0.65),usb:mix(bg,fg,.70),usbBore:mix(bg,fg,.08),battery:color('--viz-series-2'),edge:mix(bg,fg,0.60)};
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
        const transformed=faces.filter(f=>(state.lid||f.part!=='lid')&&(state.pcb||!['pcb','wheel','wheelHub','usb','usbBore'].includes(f.part))&&(state.midframe||f.part!=='midframe')).map(f=>({...f,loops:f.loops.map(loop=>loop.map(p=>transform(p,f.part))),n:rotateNormal(f.normal)}));
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
        control.clear.checked=state.clear; control.lid.checked=state.lid;control.pcb.checked=state.pcb;control.midframe.checked=state.midframe;control.view.value=state.view;
        control.explode.setAttribute('aria-valuetext',state.explode===0?'Assembled':Math.round(state.explode*100)+' percent separated');
      }
      function save() {
        if(window.openai?.setWidgetState) window.openai.setWidgetState({modelContent:{revision:5,caseMm:[80,56,29],batteryMm:[65,36,10],batteryCaseXYmm:[9.25,12.5],batteryShiftRightMm:1.75,boardStandoffMm:[21.1,8.7],midframeScrewsMm:[[58.9,8.7],[5.5,46]],midframeZmm:[13,14.5],midframePerimeterClearanceMm:.3,sharedLeftBoardScrew:true,removableUpperCover:true,lowerCaseFullShape:true,wheelRimExposureMm:1.1,usbOpeningsMm:[12,6.5],usbOpeningYZmm:[[17.5,22.93],[32.5,22.93]],fitVerified:false},privateContent:{p4xFittedMidframe:state}}).catch(()=>{});
      }
      function restore(snapshot) {
        const s=snapshot?.privateContent?.p4xFittedMidframe;
        if(s&&typeof s==='object') {
          for(const [k,a,b] of [['yaw',-100,100],['pitch',-Math.PI/2,Math.PI/2],['explode',0,1],['zoom',0.65,2.5]]) if(Number.isFinite(s[k])) state[k]=clamp(s[k],a,b);
          for(const k of ['clear','lid','pcb','midframe']) if(typeof s[k]==='boolean') state[k]=s[k];
          if(['angle','frame','posts','edge','usb','top','side','bottom'].includes(s.view)) state.view=s.view;
        }
        syncControls();requestDraw();
      }
      control.explode.addEventListener('input',()=>{state.explode=Number(control.explode.value)/100;syncControls();requestDraw();});
      control.explode.addEventListener('change',save);
      for(const k of ['clear','lid','pcb','midframe']) control[k].addEventListener('change',()=>{state[k]=control[k].checked;requestDraw();save();});
      control.view.addEventListener('change',()=>{
        state.view=control.view.value;
        const angles={angle:[-.38,.50],frame:[-.2,.75],posts:[0,Math.PI/2],edge:[2.82,.64],usb:[-1.35,.45],top:[0,Math.PI/2],side:[Math.PI/2,0],bottom:[0,-Math.PI/2]};
        [state.yaw,state.pitch]=angles[state.view];state.zoom=1;
        if(state.view==='frame') Object.assign(state,{explode:.4,clear:true,lid:false,pcb:false,midframe:true});
        if(state.view==='posts') Object.assign(state,{explode:0,clear:true,lid:false,pcb:false,midframe:false});
        if(state.view==='edge'||state.view==='usb') Object.assign(state,{explode:0,clear:false,lid:true,pcb:true,midframe:true});
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
  