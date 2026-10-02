
    (() => {
      const root = document.getElementById('p4x-unified-body');
      const canvas = root.querySelector('#p4x-unified-body-canvas');
      const ctx = canvas.getContext('2d');
      const control = Object.fromEntries(['view','explode','explode-value','clear','lid','pcb','display','buttons','hardware','midframe','in','out'].map(k => [k, root.querySelector('#p4x-unified-body-'+k)]));
      if (!ctx) return;
      let state = {yaw:2.82, pitch:.64, explode:0, zoom:1, clear:false, lid:true, pcb:true, display:true, buttons:true, hardware:true, midframe:true, view:'edge'};
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
      const scaleInt=100000;
      const intPath=p=>p.map(([x,y])=>({X:Math.round(x*scaleInt),Y:Math.round(y*scaleInt)}));
      const floatPaths=ps=>ps.map(p=>p.map(q=>[q.X/scaleInt,q.Y/scaleInt])).filter(p=>p.length>2&&Math.abs(area(p))>1e-8);
      function booleanPaths(subject,clips=[],operation='difference') {
        const c=new ClipperLib.Clipper(),result=[];
        c.AddPaths(subject.filter(p=>p.length>2).map(intPath),ClipperLib.PolyType.ptSubject,true);
        if(clips.length)c.AddPaths(clips.filter(p=>p.length>2).map(p=>intPath(ccw(p))),ClipperLib.PolyType.ptClip,true);
        c.Execute(ClipperLib.ClipType[{difference:'ctDifference',union:'ctUnion',intersection:'ctIntersection'}[operation]],result,ClipperLib.PolyFillType.pftNonZero,ClipperLib.PolyFillType.pftNonZero);
        return floatPaths(result);
      }
      function offsetPaths(polys,d) {
        const c=new ClipperLib.ClipperOffset(8,.001*scaleInt),result=[];
        c.AddPaths(polys.map(intPath),ClipperLib.JoinType.jtMiter,ClipperLib.EndType.etClosedPolygon);
        c.Execute(result,d*scaleInt);return floatPaths(result);
      }
      const largest=ps=>ps.length?ps.reduce((a,b)=>Math.abs(area(a))>Math.abs(area(b))?a:b):[];
      const resolvedInset=(p,d)=>ccw(largest(offsetPaths([ccw(p)],-d)));
      function capPaths(polys,z,up,part) {
        if(!polys.length)return;
        face(polys.map(p=>at(p,z)),[0,0,up?1:-1],part);
        for(const p of polys)line(at(p,z),part);
      }
      function cap(outline,z,up,part,holes=[]) {
        capPaths(booleanPaths([ccw(outline)],holes),z,up,part);
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
      function regionSolid(polys,z0,z1,part) {
        for(const p of polys)sides(p,z0,z1,part);
        capPaths(polys,z0,false,part);capPaths(polys,z1,true,part);
      }
      function solid(poly,z0,z1,part,holes=[]) {
        regionSolid(booleanPaths([ccw(poly)],holes),z0,z1,part);
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
      function differenceSolid(poly,z0,z1,part,holes=[]) {solid(poly,z0,z1,part,holes);}
      const rect=(x,y,w,h)=>[[x,y],[x+w,y],[x+w,y+h],[x,y+h]];
      const arc=(cx,cy,r,a0,a1,n=12)=>Array.from({length:n+1},(_,i)=>{
        const a=(a0+(a1-a0)*i/n)*Math.PI/180;return [cx+r*Math.cos(a),cy+r*Math.sin(a)];
      });
      const bezier=(a,b,c,d,t)=>a.map((v,i)=>(1-t)**3*v+3*(1-t)**2*t*b[i]+3*(1-t)*t*t*c[i]+t**3*d[i]);
      const curve=c=>Array.from({length:25},(_,i)=>bezier(...c,i/24));
      const nominal=[...arc(5,5,5,180,270),...arc(75,5,5,270,360),
        ...curve([[80,5],[80,7.5],[75.8,8.5],[75.8,11]]),[75.8,38.1],
        ...arc(76.2,38.1,.4,180,90),[77,38.5],...arc(77,41.5,3,-90,0),...arc(76,41.5,4,0,90),
        ...Array.from({length:68},(_,i)=>[76-(i+1)*.25,45.5]),
        ...curve([[59,45.5],[50,45.5],[48,56],[38,56]]),...arc(5,51,5,90,180)];
      const ease=t=>{t=clamp(t,0,1);return t*t*(3-2*t);};
      const wheelRelief=x=>ease((x-53.65)/2);
      const joinY=41-Math.sqrt(3.3**2-.9**2),joinAngle=Math.atan2(joinY-41,-.9)*180/Math.PI;
      const frontOutline=[...arc(5,5,5,180,270),...arc(75,5,5,270,360),
        ...curve([[80,5],[80,7.5],[75.8,8.5],[75.8,11]]),[75.8,joinY],
        ...arc(76.1,joinY,.3,180,joinAngle+180,16),...arc(77,41,3,joinAngle,0,48),...arc(76.5,41,3.5,0,90,36)];
      const rearReference=[[76.5,45.5],[59,45.5],...curve([[59,45.5],[50,45.5],[48,56],[38,56]])];
      const rearSamples=rearReference.flatMap((p,i)=>{
        if(i===rearReference.length-1)return[p];
        const q=rearReference[i+1],n=Math.max(1,Math.ceil(Math.hypot(q[0]-p[0],q[1]-p[1])/.25));
        return Array.from({length:n},(_,j)=>[p[0]+(q[0]-p[0])*j/n,p[1]+(q[1]-p[1])*j/n]);
      });
      const common=[...frontOutline,...rearSamples.map(([x,y])=>[x,y-wheelRelief(x)]),...arc(5,51,5,90,180)];
      const nominalOutside=ccw(largest(booleanPaths([nominal],[],'union')));
      const outside=ccw(largest(booleanPaths([common],[],'union'))),upperOutside=outside;
      function interior(general,local) {
        const extra=booleanPaths(offsetPaths([nominalOutside],-local),[rect(70,7,10,33.8)],'intersection');
        return ccw(largest(booleanPaths([...offsetPaths([nominalOutside],-general),...extra],[],'union')));
      }
      const inside=ccw(largest(booleanPaths([interior(2,1.6)],offsetPaths([outside],-1.5),'intersection')));
      const nominalUpperInside=interior(1.6,1);
      const upperInside=ccw(largest(booleanPaths([nominalUpperInside],offsetPaths([outside],-.6),'intersection')));
      const reinforcedEnvelope=booleanPaths([...offsetPaths([outside],-1.1),rect(-1,-1,82,42.5)],[],'union');
      const upperReinforcedInside=ccw(largest(booleanPaths([upperInside],reinforcedEnvelope,'intersection')));
      const usbWindows=[17.5,32.5].map(y=>rounded(y-4.9,20.83,9.8,4.2,.5,12));
      const bossCenters=[[3.6,3.6,2.6],[76.4,3.6,2.6],[3.6,52.4,2.6],[77,41,2.2]];
      const fastener={headSeat:10.2,headHeight:1.3,screwLength:8,wellTop:11.5};
      const screwTip=fastener.headSeat+fastener.screwLength,headBottom=fastener.headSeat-fastener.headHeight;
      const headRecesses=bossCenters.map(([x,y])=>circle(x,y,2.25));
      sides(outside,2,14.5,'base');
      maskedSides(inside,2,fastener.headSeat,'base',headRecesses,p=>!headRecesses.some(h=>inPoly(p,h)),true);
      sides(inside,fastener.headSeat,14.5,'base',true);
      // Slice the shell at cavity changes and rounded USB aperture corners.
      // Each section is a resolved polygon, so the relocated corner remains closed.
      const cableReliefFootprint=[[75.8,36.8],[81,36.8],[81,38.6],[76,38.6],...arc(76,38.4,.2,90,180,12)];
      function cableReliefAt(z) {
        if(z<19.2||z>26.8)return [];
        const dz=z<19.7?19.7-z:z>26.3?z-26.3:0;
        const spread=Math.sqrt(Math.max(0,.25-dz*dz));
        return booleanPaths([cableReliefFootprint],[rect(75.8,37.3-spread,5.2,.8+2*spread)],'intersection');
      }
      const reliefLevels=[19.2,19.7,26.3,26.8,...Array.from({length:11},(_,i)=>19.7-.5*Math.cos((i+1)*Math.PI/24)),...Array.from({length:11},(_,i)=>26.3+.5*Math.sin((i+1)*Math.PI/24))];
      const upperLevels=[...reliefLevels,14.5,17.7,18.1,21.6,23.03,24.53,...Array.from({length:12},(_,i)=>24.53+.5*Math.sin((i+1)*Math.PI/24)),27].sort((a,b)=>a-b);
      function upperSection(z) {
        const cavity=z<17.7||z>=21.6?upperReinforcedInside:upperInside,holes=[cavity];
        if(z<18.1)holes.push(rect(56.4,42.5,13,11.5));
        if(z<23.03)holes.push(rect(74.7,11,1.3,27));
        if(z>=23.03&&z<25.03){
          const half=z<=24.53?4.9:4.4+Math.sqrt(Math.max(0,.25-(z-24.53)**2));
          for(const y of [17.5,32.5])holes.push(rect(74.3,y-half,2,2*half));
        }
        return booleanPaths([upperOutside],[...holes,...cableReliefAt(z)]);
      }
      for(let i=0;i<upperLevels.length-1;i++){
        const z0=upperLevels[i],z1=upperLevels[i+1];
        if(z1-z0>1e-8)regionSolid(upperSection((z0+z1)/2),z0,z1,'lid');
      }
      for(let i=0;i<64;i++) {
        const a0=Math.PI*i/128,a1=Math.PI*(i+1)/128;
        differenceSolid(resolvedInset(outside,2*(1-Math.cos(a1))),2*(1-Math.sin(a1)),2*(1-Math.sin(a0)),'base',headRecesses);
      }
      cap(inside,2,true,'base',headRecesses.map(p=>convexClip(p,inside)).filter(p=>p.length>2));
      cap(outside,14.5,true,'base',[inside]);
      function topY(poly,x) {
        const ys=[];
        for(let i=0;i<poly.length;i++){const a=poly[i],b=poly[(i+1)%poly.length];if((a[0]<=x&&b[0]>x)||(b[0]<=x&&a[0]>x))ys.push(a[1]+(x-a[0])*(b[1]-a[1])/(b[0]-a[0]));}
        return Math.max(...ys);
      }
      // General polygon intersection supports the common concave housing.
      function convexClip(poly,clip) {return ccw(largest(booleanPaths([ccw(poly)],[clip],'intersection')));}
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
        const lowerPad=convexClip(circle(x,y,i===3?3:3.25),outside);
        // Full-height local pads protect the closed screw mouths from rounding.
        differenceSolid(lowerPad,0,fastener.headSeat,'base',[recess]);
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
      const mount=[21.1,8.7],frontMount=[5,8.7],rearMount=[44,48.5];
      for(const [x,y,r] of [[...frontMount,2.75],[...rearMount,3]]) {
        const shoulder=circle(x,y,r),tip=circle(x,y,1.2),installedBore=circle(x,y,1.5),mouth=circle(x,y,1.55);
        sides(shoulder,2,13,'mount');cap(shoulder,13,true,'mount',[mouth]);
        // Installed insert envelope; the printed CAD pilot is Ø2.7 before heat-setting.
        sides(tip,6,9.6,'mountBore',true);cap(tip,6,true,'mountBore');
        cap(installedBore,9.6,true,'mountBore',[tip]);sides(installedBore,9.6,12.75,'mountBore',true);
        taperedSides(x,y,1.5,1.55,12.75,13,'mountBore',true);
        solid(circle(x,y,1.5),9.8,13,'frameInsert',[circle(x,y,1)]);
        solid(circle(x,y,1),6.5,14.5,'frameScrew');
        solid(circle(x,y,2),14.5,15.45,'frameScrew');
        const slot=rect(x-1.6,y-.3,3.2,.6);
        differenceSolid(circle(x,y,2),15.45,15.8,'frameScrew',[slot]);cap(slot,15.451,true,'frameScrewSlot');
      }
      solid(rect(2,7.9,3,1.6),2,6,'mount');
      // Join the rear rib 0.2 mm into the wall without reaching its outer face.
      const rearRibOutline=convexClip(rect(43.2,48.5,1.6,7.5),largest(offsetPaths([inside],.2)));
      solid(rearRibOutline,2,6,'mount');
      const shaftHoles=bossCenters.map(([x,y])=>circle(x,y,1.15));
      const ledge=booleanPaths(offsetPaths([inside],.01),[...offsetPaths([inside],-1.4),rect(8.3,5.8,65.4,36.4),...shaftHoles,circle(...frontMount,1.55),circle(...rearMount,1.55)]);
      regionSolid(ledge,11.5,13,'base');
      const packKeepout=rect(7.8,5.3,66.4,37.4);
      for(const [x,y,r] of bossCenters) differenceSolid(convexClip(circle(x,y,r+.7),outside),11.5,13,'base',[packKeepout,circle(x,y,1.15)]);
      const midOutline=resolvedInset(inside,.3);
      const batteryCableCuts=[rounded(65.5,20.65,7,8,1),rounded(69.5,4,3,18,.75),rounded(69,4,9,7,1)];
      const frameReliefs=bossCenters.map(([x,y])=>circle(x,y,2.9));
      differenceSolid(midOutline,13,14.5,'midframe',[circle(...frontMount,1.15),circle(...rearMount,1.15),...frameReliefs,...batteryCableCuts]);
      // Base-owned open-top USB saddle; the complete board/frame assembly drops in.
      alongX(rect(11.2,14.5,26.6,8.33),74.8,75.8,'base',usbWindows);
      face([[[74.2,11.2,14.5],[74.8,11.2,14.5],[74.8,11.2,17.5]]],[0,-1,0],'base');
      face([[[74.2,37.8,14.5],[74.8,37.8,17.5],[74.8,37.8,14.5]]],[0,1,0],'base');
      face([[[74.2,11.2,14.5],[74.8,11.2,17.5],[74.8,37.8,17.5],[74.2,37.8,14.5]]],[-.98058,0,.19612],'base');
      solid(circle(...mount,3),14.5,14.7,'midframe');
      solid(circle(...mount,3),14.7,19.7,'midframe',[circle(...mount,.6)]);
      sides(circle(...mount,.6),14.7,19.7,'midBore',true);cap(circle(...mount,.6),14.7,true,'midBore');
      for(const x of [10,72.5]) {
        solid(rect(x-1.5,x<50?42.7:42.1,3,x<50?1.2:.8),14.5,19.7,'midframe');
        if(x<50) solid(rect(x-1.5,43.9,3,1.2),14.5,21.1,'midframe');
      }
      solid(rect(65,6.1,3,1.4),14.5,19.7,'midframe');
      solid(rect(65,5,3,1.1),14.5,21.1,'midframe');
      solid(rect(8,8,8.5,3.7),14.5,21.1,'midframe');
      // Midframe tongues enter blind lid pockets without thinning the shell.
      const lidKeyRects=[[2.3,20,1.5,8],[40,2.3,8,1.5],[24,52.2,8,1.5]];
      const lidReceiverRects=[[1.5,18.8,3.5,10.4],[38.8,1.5,10.4,3.5],[22.8,51,10.4,3.5]];
      const expandedRect=(r,d)=>rect(r[0]-d,r[1]-d,r[2]+2*d,r[3]+2*d);
      function loftSides(low,high,z0,z1,part,inward=false) {
        for(let i=0;i<low.length;i++) {
          const j=(i+1)%low.length,a=[...low[i],z0],b=[...low[j],z0],c=[...high[j],z1],d=[...high[i],z1];
          const u=b.map((v,k)=>v-a[k]),v=c.map((q,k)=>q-a[k]);
          let n=[u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]];
          const m=Math.hypot(...n)*(inward?-1:1);n=n.map(q=>q/m);
          face([[a,b,c,d]],n,part);
        }
        line(at(low,z0),part);line(at(high,z1),part);
      }
      for(let i=0;i<lidKeyRects.length;i++) {
        const k=lidKeyRects[i],key=rect(...k),tip=expandedRect(k,-.3);
        solid(key,14.5,16.6,'midframe');
        loftSides(key,tip,16.6,17,'midframe');cap(tip,17,true,'midframe');
        const pad=rect(...lidReceiverRects[i]),pocket=expandedRect(k,.2),mouth=expandedRect(k,.4);
        sides(pad,14.5,17.8,'lid');cap(pad,14.5,false,'lid',[mouth]);cap(pad,17.8,true,'lid');
        loftSides(mouth,pocket,14.5,14.9,'lid',true);sides(pocket,14.9,17.3,'lid',true);cap(pocket,17.3,false,'lid');
      }
      // Front-loading seat. The central opening and flex relief form one cutout.
      const lcdPocket=rounded(14.04,8.25,31.92,34.12,.3);
      const lcdFlex=rounded(18,5.9,24,6.35,.6);
      const seatRearOpening=ccw(largest(booleanPaths([rounded(15.24,9.45,29.52,31.72,.3),lcdFlex],[],'union')));
      const pocketFlexOpening=ccw(largest(booleanPaths([lcdPocket,lcdFlex],[],'union')));
      const keyYs=[19,25,31];
      const lidHoles=keyYs.map(y=>circle(51.5,y,2.6));
      const keyAnchorHoles=[14,36].map(y=>circle(63,y,.6));
      differenceSolid(rounded(12.54,6.75,34.92,37.12,1),25.9,27,'lid',[seatRearOpening]);
      // R2 quarter-circle face edge, native-offset slices matching the CAD source.
      for(let i=0;i<64;i++) {
        const a0=Math.PI*i/128,a1=Math.PI*(i+1)/128,z0=27+2*Math.sin(a0),z1=27+2*Math.sin(a1),rim=resolvedInset(upperOutside,2*(1-Math.cos(a1)));
        for(const [lo,hi,opening,anchors] of [[27,27.1,seatRearOpening,true],[27.1,28,pocketFlexOpening,true],[28,28.5,lcdPocket,true],[28.5,29,lcdPocket,false]]) {
          const low=Math.max(z0,lo),high=Math.min(z1,hi);
          if(high>low+1e-9)solid(rim,low,high,'lid',[...lidHoles,...(anchors?keyAnchorHoles:[]),opening]);
        }
      }
      for(const y of [14,36]) solid(circle(63,y,2.5),26.6,27,'lid',[circle(63,y,.6)]);
      // Replaceable actuator strip; caps enter lid from inside and tongues return them.
      solid(rect(60.5,12,5,26),25.8,26.6,'buttons',[circle(63,14,.9),circle(63,36,.9)]);
      for(const y of keyYs) {
        solid(rect(51.5,y-1,9.5,2),25.8,26.6,'buttons');
        solid(circle(51.5,y,2.2),25.8,30,'buttons');
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
      solid(rounded(8.5,6,65,36,1),2.5,12.5,'battery');
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
      const lift={base:0,baseBore:0,mount:0,mountBore:0,battery:25,midframe:42,midBore:42,frameScrew:42,frameScrewSlot:42,frameInsert:0,pcb:65,lcd:90,screen:90,buttons:90,switch:65,wheel:65,wheelHub:65,usb:65,usbBore:65,lid:90,lidBore:90,insert:90,screw:0,screwSlot:0,cutBase:0,cutLid:90,cutInsert:90,cutScrew:0};
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
        palette={base:mix(bg,fg,0.32),lid:mix(bg,fg,0.28),midframe:color('--viz-series-3'),mount:color('--viz-series-3'),mountBore:mix(bg,fg,0.12),midBore:mix(bg,fg,.12),pcb:color('--viz-series-1'),buttons:color('--viz-series-5'),switch:mix(bg,fg,.55),lcd:mix(bg,fg,.2),screen:mix(bg,fg,.06),wheel:color('--viz-series-4'),wheelHub:color('--viz-series-4').map(v=>v*0.65),usb:mix(bg,fg,.70),usbBore:mix(bg,fg,.08),battery:color('--viz-series-2'),insert:color('--viz-series-6'),screw:mix(bg,fg,.82),edge:mix(bg,fg,0.60),text:fg};
        Object.assign(palette,{frameInsert:palette.insert,frameScrew:palette.screw,frameScrewSlot:mix(bg,fg,.22),baseBore:mix(bg,fg,.14),lidBore:mix(bg,fg,.11),screwSlot:mix(bg,fg,.22),cutBase:palette.base,cutLid:palette.lid,cutInsert:palette.insert,cutScrew:palette.screw});
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
        const visible=f=>{
          if((state.view==='fastener')!==f.part.startsWith('cut'))return false;
          if(!state.hardware&&['insert','screw','screwSlot','frameInsert','frameScrew','frameScrewSlot','cutInsert','cutScrew'].includes(f.part))return false;
          if(!state.lid&&['lid','lidBore','lcd','screen','insert','cutLid','cutInsert'].includes(f.part))return false;
          if(!state.display&&['lcd','screen'].includes(f.part))return false;
          if(state.view==='seat'&&!['lid','lidBore','lcd','screen','insert'].includes(f.part))return false;
          if(!state.pcb&&['pcb','wheel','wheelHub','usb','usbBore','switch'].includes(f.part))return false;
          if(!state.buttons&&f.part==='buttons')return false;
          if(state.view==='keys'&&!['buttons','switch'].includes(f.part))return false;
          return state.midframe||!['midframe','midBore','frameScrew','frameScrewSlot'].includes(f.part);
        };
        const transformed=faces.filter(visible).map(f=>({...f,loops:f.loops.map(loop=>loop.map(p=>transform(p,f.part))),n:rotateNormal(f.normal)}));
        if(!transformed.length){canvas.dataset.rendered='true';canvas.dataset.faces='0';return;}
        const vertices=transformed.flatMap(f=>f.loops.flat());
        const bounds=[Infinity,Infinity,-Infinity,-Infinity];
        for(const [x,y] of vertices) {bounds[0]=Math.min(bounds[0],x);bounds[1]=Math.min(bounds[1],y);bounds[2]=Math.max(bounds[2],x);bounds[3]=Math.max(bounds[3],y);}
        const scale=Math.min((w-48)/(bounds[2]-bounds[0]),(h-48)/(bounds[3]-bounds[1]))*state.zoom;
        const ox=w*(state.view==='fastener'?.28:.5)-(bounds[0]+bounds[2])*scale/2,oy=h/2-(bounds[1]+bounds[3])*scale/2;
        const project=p=>[ox+p[0]*scale,oy+p[1]*scale];
        const renderFaces=transformed.filter(f=>f.n[2]>0.00001).map(f=>({...f,depth:f.loops[0].reduce((a,p)=>a+p[2],0)/f.loops[0].length})).sort((a,b)=>Math.abs(a.depth-b.depth)<.00001?0:a.depth-b.depth);
        // Per-pixel depth keeps the solid lid in front of the PCB from every angle.
        // Scanline filling preserves component apertures and fastener bores.
        const rw=Math.round(w),rh=Math.round(h);
        raster.width=rw;raster.height=rh;
        const pixels=rasterCtx.createImageData(rw,rh),buffer=pixels.data;
        const depth=new Float32Array(rw*rh);depth.fill(-Infinity);
        const isTransparent=f=>state.clear&&['base','baseBore','lid','lidBore','cutBase','cutLid'].includes(f.part);
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
          if(state.view==='fastener'||(p.part==='lid'&&!state.lid)||!shell||!state.clear) continue;
          ctx.strokeStyle=rgb(palette.edge,0.34); ctx.lineWidth=0.65;
          ctx.beginPath();p.loop.forEach((q,i)=>{const s=project(transform(q,p.part));i?ctx.lineTo(...s):ctx.moveTo(...s);});ctx.closePath();ctx.stroke();
        }
        if(state.view==='fastener') {
          const textX=w*.48,style=getComputedStyle(root.querySelector('.foot'));
          ctx.font=`${style.fontWeight} ${style.fontSize} ${style.fontFamily}`;
          ctx.textBaseline='middle';ctx.lineWidth=1;
          const labels=[['Closed top',26,'cutLid'],['Screw-tip clearance',20.5,'cutLid'],[state.hardware?'Insert Ø3 × 3.2 mm':'Insert pocket',16.1,'cutInsert'],['Recessed M2 head',fastener.headSeat-.65,'cutScrew'],['Driver access',4.3,'cutBase']];
          for(const [label,z,part] of labels) {
            if(!state.lid&&['cutLid','cutInsert'].includes(part))continue;
            const p=project(transform([5.6,3.6,z],part)),y=clamp(p[1],12,h-12);
            ctx.strokeStyle=rgb(palette.edge,.65);ctx.beginPath();ctx.moveTo(...p);ctx.lineTo(textX-8,y);ctx.stroke();
            ctx.fillStyle=rgb(palette.text);ctx.fillText(label,textX,y);
          }
        }
        canvas.dataset.rendered='true';
        canvas.dataset.faces=String(renderFaces.length);
      }
      function requestDraw() {if(!frame) frame=requestAnimationFrame(draw);}
      function syncControls() {
        root.querySelector('#p4x-unified-body-fit-one').textContent=state.view==='interlock'?'3 locating keys · 2.5 mm engagement':state.view==='keys'?'PCB → lid underside · 5.7 mm':state.view==='seat'?'Display flush · 1.9 mm seat':state.view==='frame'?'Battery plug opening · 7 × 8 mm':'Outer face edges · R2 mm';
        root.querySelector('#p4x-unified-body-fit-two').textContent=state.view==='keys'?'Switch rest gap · 0.3 mm':state.view==='interlock'?'Pocket clearance · 0.2 mm per side':state.view==='seat'?'Flex relief · 24 × 6.35 mm':state.view==='fastener'?'M2 × 8 mm · head fit provisional':state.view==='posts'?'Closed wells · battery clearance 0.5 mm':state.view==='frame'?'Wire channel · 3 mm wide':state.view==='edge'?'Wheel exposure · 2.1 mm':state.view==='usb'?'Cable side clearance · 0.98 mm':'Matching upper + lower contour';
        control.explode.value=Math.round(state.explode*100);
        control['explode-value'].textContent=Math.round(state.explode*100)+'%';
        control.clear.checked=state.clear; control.lid.checked=state.lid;control.pcb.checked=state.pcb;control.display.checked=state.display;control.buttons.checked=state.buttons;control.hardware.checked=state.hardware;control.midframe.checked=state.midframe;control.view.value=state.view;
        control.explode.setAttribute('aria-valuetext',state.explode===0?'Assembled':Math.round(state.explode*100)+' percent separated');
      }
      function save() {
        if(window.openai?.setWidgetState) window.openai.setWidgetState({modelContent:{revision:17,lidAlignment:{owner:"midframe",keysXYWHmm:[[2.3,20,1.5,8],[40,2.3,8,1.5],[24,52.2,8,1.5]],engagementMm:2.5,sideClearanceMm:.2,topGapMm:.3,verticalRemoval:true,fitVerified:false},lcdFlexRelief:{sizeMm:[24,6.35],xyMm:[18,5.9],zMm:[25.8,28],radiusMm:.6},batteryCablePassage:{plugWidthMm:5,openingXYmm:[65.5,20.65],openingSizeMm:[7,8],cornerRadiusMm:1,wireChannelXYmm:[69.5,4],wireChannelSizeMm:[3,18],leadEntryXYmm:[69,4],leadEntrySizeMm:[9,7],leadCornerCaseXYmm:[73.5,6],connectorCaseXYmm:[71.375,24.65],physicalFitVerified:false},caseMm:[80,56,29],overallEnvelopeMm:[80,56,30],outerEdgeRadiusMm:2,buttonProtrusionMm:1,solidNotchLocatorMm:[8.5,3.7,6.6],batteryMm:[65,36,10],batteryCaseXYmm:[8.5,6],batteryMoveFromRev10Mm:[0,-6.5],boardStandoffMm:[21.1,8.7],midframeScrewsMm:[[5,8.7],[44,48.5]],midframeFasteners:{thread:"M2",screwLengthMm:8,insertMm:[3,3.2],plateHoleMm:2.3,pilotBoreMm:2.7,pilotDepthMm:3.4,headSeatZmm:14.5,insertZmm:[9.8,13],screwTipZmm:6.5,tipPocketBottomZmm:6,headDimensionsProvisional:true},midframeZmm:[13,14.5],midframePerimeterClearanceMm:.3,sharedLeftBoardScrew:false,boardBossOwnedByMidframe:true,boardPilotFloorMm:1.7,removableUpperCover:true,commonCaseContour:true,usbSaddleOwnedByBase:true,wheelRimExposureMm:2.1,wheelOutsideReliefMm:1,wheelLocalWallMm:{upperReinforced:1.1,encoderPcbBand:.6,lower:1.5},wheelThinBandZmm:[17.7,21.6],wheelApertureWidthMm:13,relocatedCaseScrewXYmm:[77,41],rearUsbCable:{measuredHousingMm:[10.25,6],assumedHousingFrontXmm:76,sideGapBeforeMm:.375,sideGapAfterMm:.975,localReliefMaxYmm:38.6,localReliefZmm:[19.2,26.8],fullDepthZmm:[19.7,26.3],screwPostUnchanged:true,fitVerified:false},usbOpeningsMm:[9.8,4.2],usbWallXmm:75.8,usbFaceProjectionMm:.2,usbSplitZmm:22.93,lcdPocketMm:[31.92,34.12],lcdSeatDepthMm:1.9,lcdSideClearanceMm:.2,lcdSupportThicknessMm:1.2,lcdFrontZmm:29,lcdLidMounted:true,lcdModuleMm:[31.52,33.72,1.9],lcdActiveAreaMm:[27.72,27.72],lcdActiveCenterXYmm:[30,26.86],lcdPositionVerified:false,caseFasteners:{direction:"underside",thread:"M2",insertMm:[3,3.2],pocketBoreMm:2.7,headRecessMm:[4.5,10.2],screwLengthMm:8,screwLengthMeasuredUnderHead:true,headDimensionsProvisional:true,closedScrewWells:true,rightWellWallMm:.75,rightWellBatteryGapMm:.5,tipReliefTopZmm:22.5,topThroughHoles:false},buttonCentersXYmm:[[51.5,19],[51.5,25],[51.5,31]],buttonSwitchHeightMm:2.5,buttonSwitchStrokeMm:.2,buttonRestGapMm:.3,buttonKeyDiameterMm:4.4,buttonHoleDiameterMm:5.2,buttonStripThicknessMm:.8,buttonFitVerified:false,usbOpeningYZmm:[[17.5,22.93],[32.5,22.93]],fitVerified:false},privateContent:{p4xUnifiedBody:state}}).catch(()=>{});
      }
      function restore(snapshot) {
        const s=snapshot?.privateContent?.p4xUnifiedBody;
        if(s&&typeof s==='object') {
          for(const [k,a,b] of [['yaw',-100,100],['pitch',-Math.PI/2,Math.PI/2],['explode',0,1],['zoom',0.65,2.5]]) if(Number.isFinite(s[k])) state[k]=clamp(s[k],a,b);
          for(const k of ['clear','lid','pcb','display','buttons','hardware','midframe']) if(typeof s[k]==='boolean') state[k]=s[k];
          if(['angle','frame','interlock','posts','seat','keys','fastener','edge','usb','top','side','bottom'].includes(s.view)) state.view=s.view;
        }
        syncControls();requestDraw();
      }
      control.explode.addEventListener('input',()=>{state.explode=Number(control.explode.value)/100;syncControls();requestDraw();});
      control.explode.addEventListener('change',save);
      for(const k of ['clear','lid','pcb','display','buttons','hardware','midframe']) control[k].addEventListener('change',()=>{state[k]=control[k].checked;requestDraw();save();});
      control.view.addEventListener('change',()=>{
        state.view=control.view.value;
        const angles={angle:[-.38,.50],frame:[-.2,.75],interlock:[-.38,.6],seat:[-.38,.85],keys:[-.65,.45],fastener:[0,0],posts:[-.3,.95],edge:[2.82,.64],usb:[-1.35,.45],top:[0,Math.PI/2],side:[Math.PI/2,0],bottom:[0,-Math.PI/2]};
        [state.yaw,state.pitch]=angles[state.view];state.zoom=1;
        if(state.view==='interlock') Object.assign(state,{explode:.12,clear:true,lid:true,pcb:false,display:false,buttons:false,hardware:false,midframe:true});
        if(state.view==='frame') Object.assign(state,{explode:.4,clear:true,lid:false,pcb:false,buttons:false,midframe:true});
        if(state.view==='seat') Object.assign(state,{explode:0,clear:false,lid:true,pcb:false,display:false,buttons:false,midframe:false});
        if(state.view==='keys') Object.assign(state,{explode:0,clear:false,lid:false,pcb:true,display:false,buttons:true,midframe:false});
        if(state.view==='fastener') Object.assign(state,{explode:0,clear:false,lid:true,pcb:true,display:true,buttons:true,hardware:true,midframe:true});
        if(state.view==='posts') Object.assign(state,{explode:0,clear:false,lid:false,pcb:false,buttons:false,hardware:false,midframe:false});
        if(state.view==='edge'||state.view==='usb') Object.assign(state,{explode:0,clear:false,lid:true,pcb:true,display:true,buttons:true,hardware:true,midframe:true});
        if(state.view==='angle') Object.assign(state,{explode:.55,clear:true,lid:true,pcb:true,display:true,buttons:true,hardware:true,midframe:true});
        if(['top','side','bottom'].includes(state.view)) Object.assign(state,{explode:0,clear:false,lid:true,pcb:true,display:true,buttons:true,midframe:true});
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
          if(!['seat','keys','fastener','interlock'].includes(state.view)){state.view='angle';control.view.value='angle';}requestDraw();
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
  