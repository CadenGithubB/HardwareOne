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
      const originalUpper=[...arc(5,5,5,180,270),...arc(75,5,5,270,360),...arc(75,40.5,5,0,90),[59,45.5],
        ...Array.from({length:24},(_,i)=>bezier([59,45.5],[50,45.5],[48,56],[38,56],(i+1)/24)),...arc(5,51,5,90,180)];
      function inset(poly,d) {
        return poly.map((p,i)=>{
          const a=poly[(i+poly.length-1)%poly.length],b=poly[(i+1)%poly.length];
          const u=[p[0]-a[0],p[1]-a[1]],v=[b[0]-p[0],b[1]-p[1]],lu=Math.hypot(...u),lv=Math.hypot(...v);
          const n=[-u[1]/lu,u[0]/lu],m=[-v[1]/lv,v[0]/lv],k=d/(1+n[0]*m[0]+n[1]*m[1]);
          return [p[0]+k*(n[0]+m[0]),p[1]+k*(n[1]+m[1])];
        });
      }
      const transitionA=[[80,5],[80,7.5],[75.8,8.5],[75.8,11]];
      const transitionB=[[75.8,38],[75.8,39.25],[80,39.25],[80,40.5]];
      const curvePoints=c=>Array.from({length:25},(_,i)=>bezier(...c,i/24));
      const rightIndex=originalUpper.findIndex(p=>Math.abs(p[0]-80)<1e-8&&Math.abs(p[1]-5)<1e-8);
      const upperOutside=[...originalUpper.slice(0,rightIndex),...curvePoints(transitionA),...curvePoints(transitionB),...originalUpper.slice(rightIndex+2)];
      // Independent inner curves keep the tight return at least 1 mm thick.
      const innerTransitionA=[[78.4,5],[78.4,6.9],[74.8,7.9],[74.8,11]];
      const innerTransitionB=[[74.8,38],[74.8,40.25],[78.4,40.25],[78.4,40.5]];
      const originalInner=inset(originalUpper,1.6);
      const upperInside=[...originalInner.slice(0,rightIndex),...curvePoints(innerTransitionA),...curvePoints(innerTransitionB),...originalInner.slice(rightIndex+2)];
      const usbWindows=[17.5,32.5].map(y=>rounded(y-4.9,20.83,9.8,4.2,.5,12));
      function upperSides(poly,invert=false) {
        // Split wall edges at the wheel aperture. Slot corners are simplified here.
        for(let i=0;i<poly.length;i++) {
          const a=poly[i],b=poly[(i+1)%poly.length];
          const dx=b[0]-a[0],dy=b[1]-a[1],len=Math.hypot(dx,dy),sign=invert?-1:1;
          if(len<1e-9) continue;
          if(Math.abs(dx)<1e-8&&a[0]>74.7&&dy>20) {
            const panel=[[a[1],23.03],[b[1],23.03],[b[1],27],[a[1],27]];
            const loops=[panel,...usbWindows.map(loop=>convexClip(loop,panel)).filter(p=>p.length>2)];
            face(loops.map(loop=>loop.map(([y,z])=>[a[0],y,z])),[sign,0,0],'lid');
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
      const bossCenters=[[3.6,3.6,2.6],[76.4,3.6,2.6],[3.6,52.4,2.6],[77,41.6,2.2]];
      const fastener={headSeat:10.2,headHeight:1.3,screwLength:8,wellTop:11.5};
      const screwTip=fastener.headSeat+fastener.screwLength,headBottom=fastener.headSeat-fastener.headHeight;
      const sideAngle=Math.asin(1.5/2.25)*180/Math.PI;
      const rightRecess=ccw([...arc(77,41.6,2.25,0,180-sideAngle,36),[74.7,43.1],[74.7,40.1],...arc(77,41.6,2.25,180+sideAngle,360,36)]);
      const headRecesses=bossCenters.map(([x,y],i)=>i===3?rightRecess:circle(x,y,2.25));
      sides(outside,0,14.5,'base');
      maskedSides(inside,2,fastener.headSeat,'base',headRecesses,p=>!headRecesses.some(h=>inPoly(p,h)),true);
      sides(inside,fastener.headSeat,14.5,'base',true);
      upperSides(upperOutside);upperSides(upperInside,true);
      cap(outside,0,false,'base',headRecesses);
      cap(inside,2,true,'base',headRecesses.map(p=>convexClip(p,inside)).filter(p=>p.length>2));
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
      for(const [y,ny] of [[11,1],[38,-1]]) face([[[74.8,y,14.5],[75.8,y,14.5],[75.8,y,23.03],[74.8,y,23.03]]],[0,ny,0],'lid');
      for(const [y0,y1] of [[11,12.6],[22.4,27.6],[37.4,38]]) face([[[74.8,y0,23.03],[75.8,y0,23.03],[75.8,y1,23.03],[74.8,y1,23.03]]],[0,0,-1],'lid');
      for(const loop of usbWindows) {
        for(let i=0;i<loop.length;i++) {
          let a=loop[i],b=loop[(i+1)%loop.length];
          if(a[1]<23.03&&b[1]<23.03)continue;
          if(a[1]<23.03){const t=(23.03-a[1])/(b[1]-a[1]);a=[a[0]+t*(b[0]-a[0]),23.03];}
          if(b[1]<23.03){const t=(23.03-b[1])/(a[1]-b[1]);b=[b[0]+t*(a[0]-b[0]),23.03];}
          const dy=b[0]-a[0],dz=b[1]-a[1],len=Math.hypot(dy,dz);
          if(len<1e-9)continue;
          face([[[74.8,...a],[75.8,...a],[75.8,...b],[74.8,...b]]],[0,-dz/len,dy/len],'lid');
        }
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
      function taperedSides(x,y,r0,r1,z0,z1,part,invert=false) {
        const a=circle(x,y,r0),b=circle(x,y,r1);
        for(let i=0;i<a.length;i++) {
          const j=(i+1)%a.length,dx=a[j][0]-a[i][0],dy=a[j][1]-a[i][1],ux=b[i][0]-a[i][0],uy=b[i][1]-a[i][1],dz=z1-z0;
          let n=[dy*dz,-dx*dz,dx*uy-dy*ux],length=Math.hypot(...n);n=n.map(v=>v/length*(invert?-1:1));
          face([[[...a[i],z0],[...a[j],z0],[...b[j],z1],[...b[i],z1]]],n,part);
        }
      }
      for(const [i,[x,y,r]] of bossCenters.entries()) {
        const out=circle(x,y,r),hole=circle(x,y,1.15),recess=headRecesses[i];
        const lowerPad=i===3?[[74.8,37.6],[80,37.6],[80,45.6],[74.8,45.6]]:convexClip(circle(x,y,3.25),outside);
        differenceSolid(lowerPad,2,fastener.headSeat,'base',[recess]);
        differenceSolid(lowerPad,fastener.headSeat,fastener.wellTop,'base',[hole]);
        sides(out,fastener.wellTop,14.5,'base');cap(out,14.5,true,'base',[hole]);
        sides(recess,0,2,'baseBore',true);
        maskedSides(recess,2,fastener.headSeat,'baseBore',[lowerPad],p=>inPoly(p,lowerPad),true);
        cap(convexClip(recess,lowerPad),fastener.headSeat,false,'baseBore',[hole]);
        sides(hole,fastener.headSeat,14.5,'baseBore',true);
        const pad=circle(x,y,2.6),mouth=circle(x,y,1.55),installedBore=circle(x,y,1.5),tip=circle(x,y,1.2);
        sides(pad,14.5,18.2,'lid');taperedSides(x,y,2.6,r,18.2,19.2,'lid');sides(out,19.2,27,'lid');
        cap(pad,14.5,false,'lid',[mouth]);cap(out,27,true,'lid');
        // The installed insert occupies its melted-in envelope; the CAD pilot is Ø2.7.
        taperedSides(x,y,1.55,1.5,14.5,14.75,'lidBore',true);
        sides(installedBore,14.75,17.9,'lidBore',true);cap(installedBore,17.9,false,'lidBore',[tip]);
        sides(tip,17.9,22.5,'lidBore',true);cap(tip,22.5,false,'lidBore');
        solid(circle(x,y,1.5),14.5,17.7,'insert',[circle(x,y,1)]);
        const slot=[[x-1.6,y-.3],[x+1.6,y-.3],[x+1.6,y+.3],[x-1.6,y+.3]];
        differenceSolid(circle(x,y,2),headBottom,headBottom+.35,'screw',[slot]);
        solid(circle(x,y,2),headBottom+.35,fastener.headSeat,'screw');solid(circle(x,y,1),fastener.headSeat,screwTip,'screw');
        cap(slot,headBottom+.349,false,'screwSlot');
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
      const frameReliefs=bossCenters.map(([x,y])=>circle(x,y,2.9));
      differenceSolid(midOutline,13,14.5,'midframe',[frameClear,frameScrew,thirdScrew,...frameReliefs]);
      // Open-top USB saddle: fit the PCB first, then lower the lid around its sockets.
      alongX(rect(11.2,14.5,26.6,8.33),74.8,75.8,'midframe',usbWindows);
      face([[[75.8,11.2,14.5],[77.4,11.2,14.5],[75.8,11.2,17.5]]],[0,-1,0],'midframe');
      face([[[75.8,37.8,14.5],[75.8,37.8,17.5],[77.4,37.8,14.5]]],[0,1,0],'midframe');
      face([[[77.4,11.2,14.5],[77.4,37.8,14.5],[75.8,37.8,17.5],[75.8,11.2,17.5]]],[.882,0,.471],'midframe');
      solid(circle(...mount,3),14.5,19.7,'midframe',[frameClear]);
      for(const x of [10,72.5]) {
        solid(rect(x-1.5,42.7,3,1.2),14.5,19.7,'midframe');
        if(x<50) solid(rect(x-1.5,43.9,3,1.2),14.5,21.1,'midframe');
      }
      solid(rect(69,6.1,3,1.4),14.5,19.7,'midframe');
      solid(rect(69,5,3,1.1),14.5,21.1,'midframe');
      solid(rect(15.5,8,1,3.7),14.5,21.1,'midframe');
      solid(rect(8,10.7,8.5,1),14.5,21.1,'midframe');
      // Front-loading seat. The central opening and flex relief form one cutout.
      const lcdPocket=rounded(14.04,8.25,31.92,34.12,.3);
      const seatRearOpening=[[15.24,9.45],[19,9.45],[19,6.65],[41,6.65],[41,9.45],[44.76,9.45],[44.76,41.17],[15.24,41.17]];
      const pocketFlexOpening=[[14.04,8.25],[19,8.25],[19,6.65],[41,6.65],[41,8.25],[45.96,8.25],[45.96,42.37],[14.04,42.37]];
      const keyYs=[19,25,31];
      const lidHoles=keyYs.map(y=>circle(51.5,y,2.6));
      const keyAnchorHoles=[14,36].map(y=>circle(63,y,.6));
      differenceSolid(rounded(12.54,6.75,34.92,37.12,1),25.9,27,'lid',[seatRearOpening]);
      solid(upperOutside,27,27.1,'lid',[...lidHoles,...keyAnchorHoles,seatRearOpening]);
      solid(upperOutside,27.1,27.5,'lid',[...lidHoles,...keyAnchorHoles,pocketFlexOpening]);
      solid(upperOutside,27.5,28.5,'lid',[...lidHoles,...keyAnchorHoles,lcdPocket]);
      solid(upperOutside,28.5,29,'lid',[...lidHoles,lcdPocket]);
      for(const y of [14,36]) solid(circle(63,y,2.5),26.6,27,'lid',[circle(63,y,.6)]);
      // Replaceable actuator strip; caps enter lid from inside and tongues return them.
      solid(rect(60.5,12,5,26),25.8,26.6,'buttons',[circle(63,14,.9),circle(63,36,.9)]);
      for(const y of keyYs) {
        solid(rect(51.5,y-1,9.5,2),25.8,26.6,'buttons');
        solid(circle(51.5,y,2.2),25.8,29,'buttons');
        solid(circle(51.5,y,.9),24.1,25.8,'buttons');
        for(const sign of [-1,1]) {
          const yc=y+sign*1.3;
          const rootPatch=[[60.5,y+sign],[60.2,y+sign],...arc(60.2,yc,.3,sign>0?-90:90,0,8)];
          solid(ccw(rootPatch),25.8,26.6,'buttons');
        }
        // Total switch envelope is sourced; small actuator details are simplified.
        solid(rect(49.4,y-1.6,4.2,3.2),21.3,23.5,'switch');
        solid(circle(51.5,y,.8),23.5,23.8,'switch');
      }
      solid(rounded(9.25,12.5,65,36,1),2.5,12.5,'battery');
      // Low locator rails are simplified; the battery envelope is exact.
      const rawPCB=[[0,0],[57.6,0],[57.6,5.6],[69,5.6],[69,32.6],[68.2,32.6],[68.2,37],[54,37],[54,38.3685],[52.759,43],[36.241,43],[35,38.3685],[35,37],[21.8,37],[21.3,36.5],[21.3,36.3],[20.8,35.8],[16.5,35.8],[16,36.3],[16,36.5],[15.5,37],[0,37]];
      solid(rawPCB.map(([x,y])=>[74.5-x,6.5+y]),19.7,21.3,'pcb',[circle(...mount,1)]);
      // The lid now carries the module. Its front is coplanar with the case.
      // Actual fit, border surface steps and flex routing still require measurement.
      const lcdOutline=rect(14.24,8.45,31.52,33.72),activeArea=rect(16.14,13,27.72,27.72);
      sides(lcdOutline,27.1,29,'lcd');cap(lcdOutline,27.1,false,'lcd');
      cap(lcdOutline,29,true,'lcd',[activeArea]);cap(activeArea,29,true,'screen');
      // Manufacturer dial envelope. Small flutes identify the grippable rim.
      const wheel=Array.from({length:120},(_,i)=>{const a=i*2*Math.PI/120,r=i%4<2?7.25:7.05;return [62.9+r*Math.cos(a),39.35+r*Math.sin(a)];});
      solid(wheel,15.7,17.7,'wheel');
      solid(circle(62.9,39.35,4.2),17.7,19.7,'wheelHub');
      // Simplified USB receptacles: true outside envelope and face position.
      function alongX(poly,x0,x1,part,holes=[]) {
        const fi=faces.length,pi=paths.length;differenceSolid(poly,x0,x1,part,holes);
        for(let i=fi;i<faces.length;i++){faces[i].loops=faces[i].loops.map(loop=>loop.map(p=>[p[2],p[0],p[1]]));const n=faces[i].normal;faces[i].normal=[n[2],n[0],n[1]];}
        for(let i=pi;i<paths.length;i++)paths[i].loop=paths[i].loop.map(p=>[p[2],p[0],p[1]]);
      }
      for(const y of [17.5,32.5]) {
        alongX(rect(y-4.47,21.3,8.94,3.26),68.65,74,'usb');
        alongX(rect(y-4.47,21.3,8.94,3.26),74,76,'usb',[rect(y-4.05,21.8,8.1,2.26)]);
        face([rect(y-4.05,21.8,8.1,2.26).map(([yy,z])=>[74.01,yy,z])],[1,0,0],'usbBore');
      }
      // Section through one typical case fastener; threads are schematic.
      const cutCenter=[3.6,3.6],half=r=>ccw(arc(...cutCenter,r,0,180,36));
      differenceSolid(half(3.25),0,fastener.headSeat,'cutBase',[circle(...cutCenter,2.25)]);
      differenceSolid(half(3.25),fastener.headSeat,fastener.wellTop,'cutBase',[circle(...cutCenter,1.15)]);
      differenceSolid(half(2.6),fastener.wellTop,14.5,'cutBase',[circle(...cutCenter,1.15)]);
      differenceSolid(half(2.6),14.5,17.9,'cutLid',[circle(...cutCenter,1.5)]);
      differenceSolid(half(2.6),17.9,22.5,'cutLid',[circle(...cutCenter,1.2)]);
      solid(half(2.6),22.5,29,'cutLid');
      differenceSolid(half(1.5),14.5,17.7,'cutInsert',[circle(...cutCenter,1)]);
      solid(half(2),headBottom,fastener.headSeat,'cutScrew');solid(half(1),fastener.headSeat,screwTip,'cutScrew');
      // View-only separation. At zero the positions match enclosure.scad.


for(const p of [[77,41.6],[78.8,41.6],[75.2,41.6],[77,43.5]])console.log(JSON.stringify({p,covering:faces.filter(f=>f.normal[2]<-.99 && f.loops.filter(l=>inPoly(p,l)).length%2===1).map(f=>({part:f.part,z:f.loops[0][0][2],loops:f.loops.length}))}));
