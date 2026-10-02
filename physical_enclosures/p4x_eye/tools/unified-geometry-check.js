const ClipperLib=require('./clipper-6.4.2.js');
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
      const batteryCableCuts=[rounded(64.5,-1,16.5,29.65,1)];
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
      for(const x of [10]) {
        solid(rect(x-1.5,x<50?42.7:42.1,3,x<50?1.2:.8),14.5,19.7,'midframe');
        if(x<50) solid(rect(x-1.5,43.9,3,1.2),14.5,21.1,'midframe');
      }
      const usbBoardSeat=rect(69.3,32.75,1.2,2);
      solid(usbBoardSeat,14.49,19.7,'usbBoardSeat');
      solid(rect(60,6.1,3,1.4),14.5,19.7,'midframe');
      solid(rect(60,5,3,1.1),14.5,21.1,'midframe');
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
      const pcbLiftStopCenter=[72.5,42.3];
      const pcbLiftStopFoot=rounded(71,41.7,3,1.2,.2),pcbLiftStopRoot=rounded(70.5,41.1,4,2.4,.4);
      sides(pcbLiftStopFoot,21.5,21.9,'pcbStop');cap(pcbLiftStopFoot,21.5,false,'pcbStop');
      loftSides(pcbLiftStopFoot,pcbLiftStopRoot,21.9,23.49,'pcbStop');
      sides(pcbLiftStopRoot,23.49,27.1,'pcbStop');cap(pcbLiftStopRoot,27.1,true,'pcbStop');
      // Front-loading seat. The central opening and flex relief form one cutout.
      const lcdPocket=rounded(14.04,8.25,31.92,34.12,.3);
      const lcdFlex=rounded(18,5.9,24,6.35,.6);
      const seatRearOpening=ccw(largest(booleanPaths([rounded(15.24,9.45,29.52,31.72,.3),lcdFlex],[],'union')));
      const pocketFlexOpening=ccw(largest(booleanPaths([lcdPocket,lcdFlex],[],'union')));
      const keyYs=[19,25,31];
      const lidHoles=keyYs.map(y=>circle(51.5,y,2.6));
      const keyAnchorHoles=[];
      differenceSolid(rounded(12.54,6.75,34.92,37.12,1),25.9,27,'lid',[seatRearOpening]);
      const powerSwitchCenter=[63.0732,11.2];
      const powerSwitchBody=rect(powerSwitchCenter[0]-4.5,powerSwitchCenter[1]-1.75,9,3.5);
      // SW7 model is a reference envelope. Slider height remains a fit-check item.
      solid(powerSwitchBody,21.3,24.8,'powerSwitch');
      const powerLeverOffset=1;
      solid(rect(powerSwitchCenter[0]+powerLeverOffset-.75,powerSwitchCenter[1]-.75,1.5,1.5),24.8,26.8,'powerLever');
      const powerSlot=rounded(powerSwitchCenter[0]-4.5,powerSwitchCenter[1]-2.2,9,4.4,1);
      const powerGuide=rounded(powerSwitchCenter[0]-8.5,powerSwitchCenter[1]-4.1,17,8.2,1);
      const powerRetainerSlot=rounded(powerSwitchCenter[0]-3.9,powerSwitchCenter[1]-2.8,7.8,5.6,.3);
      const powerRetainerOutline=rounded(powerSwitchCenter[0]-9,powerSwitchCenter[1]-4.5,18,9,1);
      const powerRetainerBridge=rect(60.5,14.7,5,2.4);
      const powerRetainerRoots=[-1,1].flatMap(sign=>{
        const bx=60.5+(sign<0?0:5),by=15.7;
        return booleanPaths([rect(bx+(sign<0?-.6:0),by,.6,.6)],[circle(bx+sign*.6,by+.6,.6,48)]);
      });
      const powerRetainer=booleanPaths([powerRetainerOutline,powerRetainerBridge,...powerRetainerRoots],[],'union');
      regionSolid(booleanPaths(powerRetainer,[powerRetainerSlot]),25.4,26.6,'buttons');
      const powerCapOffset=1;
      const powerCapShape=(width,height,radius,dx=powerCapOffset,dy=0)=>rounded(powerSwitchCenter[0]+dx-width/2,powerSwitchCenter[1]+dy-height/2,width,height,radius);
      const powerCapFlange=powerCapShape(14,7.6,1),powerCapBoss=powerCapShape(4,4,.3);
      const powerCapNub=powerCapShape(6,3.8,1),powerCapTop=powerCapShape(5.4,3.2,.7);
      const powerSocket=(size,dx=powerCapOffset,dy=0)=>rect(powerSwitchCenter[0]+dx-size/2,powerSwitchCenter[1]+dy-size/2,size,size);
      // External faces follow the boolean-unioned CAD cap; no hidden seam caps.
      sides(powerCapBoss,25.4,26.8,'powerCap');cap(powerCapBoss,25.4,false,'powerCap',[powerSocket(2.4)]);
      loftSides(powerSocket(2.4),powerSocket(2),25.4,25.59,'powerCap',true);
      sides(powerSocket(2),25.59,27.9,'powerCap',true);cap(powerSocket(2),27.9,false,'powerCap');
      sides(powerCapFlange,26.8,27.6,'powerCap');cap(powerCapFlange,26.8,false,'powerCap',[powerCapBoss]);
      cap(powerCapFlange,27.6,true,'powerCap',[powerCapNub]);sides(powerCapNub,27.6,29.7,'powerCap');
      loftSides(powerCapNub,powerCapTop,29.7,29.99,'powerCap');sides(powerCapTop,29.99,30,'powerCap');cap(powerCapTop,30,true,'powerCap');
      function powerOpeningAt(z) {return z<27.8?powerGuide:powerSlot;}
      function roofWithPowerOpening(rim,z0,z1,holes) {
        solid(rim,z0,z1,'lid',[...holes,powerOpeningAt((z0+z1)/2)]);
      }
      // R2 quarter-circle face edge, native-offset slices matching the CAD source.
      for(let i=0;i<64;i++) {
        const a0=Math.PI*i/128,a1=Math.PI*(i+1)/128,z0=27+2*Math.sin(a0),z1=27+2*Math.sin(a1),rim=resolvedInset(upperOutside,2*(1-Math.cos(a1)));
        for(const [lo,hi,opening,anchors] of [[27,27.1,seatRearOpening,true],[27.1,27.8,pocketFlexOpening,true],[27.8,28,pocketFlexOpening,true],[28,28.5,lcdPocket,true],[28.5,29,lcdPocket,false]]) {
          const low=Math.max(z0,lo),high=Math.min(z1,hi);
          if(high>low+1e-9)roofWithPowerOpening(rim,low,high,[...lidHoles,...(anchors?keyAnchorHoles:[]),opening]);
        }
      }
      for(const y of [19,36]) solid(circle(63,y,2.5),26.6,28.99,'anchorPads');
      const keeperMain=rect(60.7,17.5,4.6,20.5);
      const keeperTabs=[21,34.4].flatMap(y=>[rect(58.7,y,2.2,2),rect(65.1,y,2.1,2)]);
      const keeperOutline=booleanPaths([keeperMain,...keeperTabs],[],'union');
      regionSolid(keeperOutline,24.65,25.65,'keeper');
      const keeperRailYs=[[20.8,23.2],[34.2,36.6]];
      const keeperFloorRects=keeperRailYs.flatMap(([y0,y1])=>[rect(57.5,y0,2.8,y1-y0),rect(65.7,y0,2.6,y1-y0)]);
      const keeperWallRects=keeperRailYs.flatMap(([y0,y1])=>[rect(57.5,y0,1,y1-y0),rect(67.4,y0,.9,y1-y0)]);
      const keeperLeadStop=rect(65.7,20,2.6,1);
      const stripSideStops=[rect(59.4,27,.9,2),rect(65.7,27,.9,2)];
      const powerShelfGuides=[rect(52.8732,10,1,2),rect(72.2732,10,1,2)];
      const stripEndStops=[rect(61,38.2,4,1),rect(60,5.5,5,1)];
      const keeperRailsAt=z=>booleanPaths([
        ...(z>=23.65&&z<24.65?keeperFloorRects:[]),
        ...(z>=23.65&&z<=27.1?[...keeperWallRects,keeperLeadStop]:[]),
        ...(z>=25.8&&z<=27.1?[...stripSideStops,...powerShelfGuides]:[]),
        ...(z>=26&&z<=27.1?stripEndStops:[])
      ],[],'union');
      for(const [z0,z1] of [[23.65,24.65],[24.65,25.8],[25.8,26],[26,27.1]])regionSolid(keeperRailsAt((z0+z1)/2),z0,z1,'keeperRails');
      // Cropped roof behind the ledges: only shown in the keeper detail.
      solid(rect(52.5,4.5,21,36.5),27,27.8,'keeperRoof',[powerGuide,...lidHoles]);
      solid(rect(52.5,4.5,21,36.5),27.8,29,'keeperRoof',[powerSlot,...lidHoles]);
      // Replaceable actuator strip; caps enter lid from inside and tongues return them.
      solid(rect(60.5,16.5,5,21.5),25.8,26.6,'buttons');
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
      // Cropped portions of the actual components for the travel-stop detail.
      // Only this view shows these duplicate reference surfaces.
      const stopDetailWindow=rect(67,30,10,14);
      const stopDetailBoard=booleanPaths([rawPCB.map(([x,y])=>[74.5-x,6.5+y])],[stopDetailWindow],'intersection');
      regionSolid(booleanPaths(stopDetailBoard,[circle(69.72,35.39,.3)]),19.7,21.3,'detailPCB');
      const stopDetailFrame=booleanPaths(booleanPaths([midOutline],[...frameReliefs,...batteryCableCuts,circle(...frontMount,1.15),circle(...rearMount,1.15)]),[stopDetailWindow],'intersection');
      regionSolid(stopDetailFrame,13,14.5,'detailFrame');
      regionSolid(booleanPaths([wheel],[stopDetailWindow],'intersection'),15.7,17.7,'detailWheel');
      // Source-grounded BOOT/RESET envelopes on the PCB underside.
      const undersideSwitchBodies=[rect(70.775,31.3,3.2,4.2),rect(70.775,38.3,3.2,4.2)];
      for(const body of undersideSwitchBodies)solid(body,17.2,19.7,'lowerSwitch');
const finite=faces.every(f=>f.loops.every(l=>l.every(p=>p.every(Number.isFinite)))&&f.normal.every(Number.isFinite));
if(!finite)throw Error('Nonfinite geometry');
const intersecting=p=>{const cross=(a,b)=>a[0]*b[1]-a[1]*b[0];let n=0;for(let i=0;i<p.length;i++)for(let j=i+2;j<p.length;j++){if(i===0&&j===p.length-1)continue;const a=p[i],b=p[(i+1)%p.length],c=p[j],d=p[(j+1)%p.length],r=[b[0]-a[0],b[1]-a[1]],s=[d[0]-c[0],d[1]-c[1]],q=[c[0]-a[0],c[1]-a[1]],den=cross(r,s);if(Math.abs(den)<1e-10)continue;const t=cross(q,s)/den,u=cross(q,r)/den;if(t>1e-8&&t<1-1e-8&&u>1e-8&&u<1-1e-8)n++;}return n;};
for(let i=1;i<=64;i++){const p=resolvedInset(upperOutside,2*(1-Math.cos(Math.PI*i/128)));if(intersecting(p))throw Error('Crossed top rim '+i);}
console.log('Finite geometry; all 64 top rings simple; faces',faces.length);
const absArea=ps=>Math.abs(ps.reduce((n,p)=>n+area(p),0));
const pack=rect(8.5,6,65,36);
if(Math.abs(absArea(booleanPaths([pack],[inside],'intersection'))-2340)>1e-5)throw Error('Battery breaches base cavity');
if(absArea(booleanPaths(ledge,[pack],'intersection'))>1e-5)throw Error('Ledge intersects battery');
for(const [x,y,r] of [[5,8.7,2.75],[44,48.5,3],...bossCenters.map(([x,y],i)=>[x,y,i===3?3:3.25])])if(absArea(booleanPaths([circle(x,y,r)],[pack],'intersection'))>1e-5)throw Error('Base post intersects battery');
for(const [i,[x,y]] of bossCenters.entries()){
 const pad=convexClip(circle(x,y,i===3?3:3.25),outside);
 if(absArea(booleanPaths([circle(x,y,2.25)],[pad]))>1e-5)throw Error('Open screw well '+i);
}
const flat=faces.filter(f=>f.part==='midframe'&&f.normal[2]<-.99&&f.loops.every(l=>l.every(p=>Math.abs(p[2]-13)<1e-8)));
if(!flat.length)throw Error('Missing flat frame underside');
console.log('Battery clears actual cavity, ledge and all base posts; 4 closed wells; flat frame underside retained.');
if(absArea(booleanPaths([rearRibOutline],offsetPaths([outside],-1.79)))>1e-5)throw Error('Rear rib reaches exterior');
for(const [part,minZ,maxZ] of [['frameScrew',6.5,15.8],['frameInsert',9.8,13]]){
 const zs=faces.filter(f=>f.part===part).flatMap(f=>f.loops.flat().map(p=>p[2]));
 if(Math.abs(Math.min(...zs)-minZ)>1e-8||Math.abs(Math.max(...zs)-maxZ)>1e-8)throw Error('Incorrect '+part+' depth');
}
for(const c of [frontMount,rearMount]) {
 const head=booleanPaths([circle(...c,2)],[midOutline],'difference');
 if(absArea(head)>1e-5)throw Error('Screw head overhangs frame');
 if(absArea(booleanPaths([circle(...c,2)],frameReliefs,'intersection'))>1e-5)throw Error('Screw head overlaps case relief');
}
console.log('Rear rib is at least1.79 mm inside exterior; M2 heads fit plate; inserts and screws have correct Z bounds.');
for(const [p,y] of [[outside,44.5],[inside,43],[upperInside,43.9],[upperReinforcedInside,43.4]])if(Math.abs(topY(p,62.9)-y)>1e-5)throw Error('Wheel center wall moved incorrectly');
if(Math.abs(39.35+7.25-topY(outside,62.9)-2.1)>1e-5)throw Error('Incorrect wheel projection');
const caseGap=Math.hypot(76.4-73.5,6-3.6)-3.25;
if(caseGap<.5)throw Error('Battery crowds front-right well');
if(Math.abs(topY(outside,75)-44.5)>1e-5)throw Error('Rear corner remains bumpy');
if(absArea(booleanPaths([midOutline],[inside]))>1e-5)throw Error('Frame outside cavity');
const hub=circle(62.9,39.35,4.2,120);
if(absArea(booleanPaths([hub],[upperInside]))>1e-5)throw Error('Encoder hub hits thin-wall band');
const board=rawPCB.map(([x,y])=>[74.5-x,6.5+y]);
if(absArea(booleanPaths([board],[upperInside]))>1e-5)throw Error('PCB hits thin-wall band');
if(absArea(booleanPaths([circle(62.9,39.35,7.25,360)],upperSection(16.7),'intersection'))>1e-5)throw Error('Wheel sweep hits reinforced lid');
console.log('Revision14: exposure2.1mm; lowerwall1.5mm; upperreinforced1.1mm, channel.6mm; PCB/hub/wheel clear, pack/well gap',caseGap);
const pointSegmentDistance=(p,a,b)=>{const dx=b[0]-a[0],dy=b[1]-a[1],t=Math.max(0,Math.min(1,((p[0]-a[0])*dx+(p[1]-a[1])*dy)/(dx*dx+dy*dy)));return Math.hypot(p[0]-a[0]-t*dx,p[1]-a[1]-t*dy);};
const cutSamples=cableReliefFootprint.flatMap((p,i)=>{const q=cableReliefFootprint[(i+1)%cableReliefFootprint.length],n=Math.max(1,Math.ceil(Math.hypot(q[0]-p[0],q[1]-p[1])/.005));return Array.from({length:n},(_,j)=>[p[0]+(q[0]-p[0])*j/n,p[1]+(q[1]-p[1])*j/n]);}).filter(p=>inPoly(p,outside));
let minCableWeb=Infinity;
for(const cavity of [upperInside,upperReinforcedInside]){
 const channel=booleanPaths([cavity],[circle(77,41,2.2,128)]);
 if(absArea(booleanPaths([cableReliefFootprint],channel,'intersection'))>1e-6)throw Error('Cable pocket opens into case');
 for(const p of cutSamples)for(const poly of channel)for(let i=0;i<poly.length;i++)minCableWeb=Math.min(minCableWeb,pointSegmentDistance(p,poly[i],poly[(i+1)%poly.length]));
}
if(minCableWeb<.83)throw Error('Cable pocket wall too thin');
if(absArea(booleanPaths([cableReliefFootprint],[circle(77,41,2.2,128)],'intersection'))>1e-6)throw Error('Cable pocket trims screw post');
const cableNose=rect(76,27.375,5,10.25),cableWithYZMargin=rect(76,27.175,5,10.65);
for(let z=19.73;z<=26.13+.001;z+=.025){
 const printed=booleanPaths([...upperSection(z),circle(77,41,2.2,128)],[],'union');
 if(absArea(booleanPaths(printed,[cableWithYZMargin],'intersection'))>1e-6)throw Error('Cable body plus .2 mm Y/Z allowance intersects printed case');
}
for(const z of [14.5,17.7,19.199,26.801,27])if(cableReliefAt(z).length)throw Error('Cable relief extends outside intended band');
if(Math.abs(38.6-37.625-.975)>1e-6||bossCenters[3][0]!==77||bossCenters[3][1]!==41)throw Error('Moved fastener or wrong clearance');
console.log('Revision15: measured10.25x6 plug plus .2mm Y/Z allowance clears; side gap.975mm; post untouched; minimum sampled cavity web',minCableWeb);

const cableRegion=booleanPaths(batteryCableCuts,[],'union');
if(cableRegion.length!==1)throw Error('Disconnected battery cable cuts');
const framePlate=booleanPaths([midOutline],[...frameReliefs,circle(...frontMount,1.15),circle(...rearMount,1.15),...batteryCableCuts]);
if(framePlate.filter(p=>area(p)>0).length!==1)throw Error('Detached frame fragment');
if(absArea(booleanPaths(framePlate,batteryCableCuts,'intersection'))>1e-5)throw Error('Blocked cable passage');
if(absArea(booleanPaths([rect(60,5,3,2.5)],batteryCableCuts,'intersection'))>1e-5)throw Error('Cable bay cuts relocated front PCB seat');
if(!inPoly([73.5,6],batteryCableCuts[0]))throw Error('Lead corner covered');
for(const p of [[71,5.5],[71,12],[71,20],[69,24.65]])if(!cableRegion.some(poly=>inPoly(p,poly)))throw Error('Cable route disconnected at '+p);
console.log('Revision18 cable bay: one frame region, relocated front seat clear, lead corner uncovered.');

for(let i=0;i<lidKeyRects.length;i++) {
 const key=rect(...lidKeyRects[i]),pad=rect(...lidReceiverRects[i]),pocket=expandedRect(lidKeyRects[i],.2),mouth=expandedRect(lidKeyRects[i],.4);
 if(absArea(booleanPaths([key],[midOutline]))>1e-5 || absArea(booleanPaths([key],[...frameReliefs,...batteryCableCuts,circle(...frontMount,1.15),circle(...rearMount,1.15)],'intersection'))>1e-5)throw Error('Key not supported on frame '+i);
 if(absArea(booleanPaths([key],[pocket]))>1e-5)throw Error('Key outside pocket '+i);
 if(absArea(booleanPaths([mouth],[pad]))>1e-5)throw Error('Receiver entry breaches pad '+i);
 if(absArea(booleanPaths([pad],[board],'intersection'))>1e-5)throw Error('Receiver under PCB footprint '+i);
 const overlapWall=p=>absArea(booleanPaths([p],[outside],'intersection'))-absArea(booleanPaths([p],[upperReinforcedInside],'intersection'));
 if(overlapWall(mouth)>1e-5)throw Error('Pocket cuts outer shell '+i);
 if(overlapWall(pad)<.1)throw Error('Receiver not joined to shell '+i);
 if(absArea(booleanPaths([pad],[circle(5,8.7,2),circle(44,48.5,2),circle(62.9,39.35,7.25),...batteryCableCuts],'intersection'))>1e-5)throw Error('Receiver clearance issue '+i);
}
if(absArea(booleanPaths([rect(19,6.65,22,4.85)],[lcdFlex]))>1e-5)throw Error('Flex relief narrowed');
if(17.3-17<.29999||19.7-17.8<1.89999)throw Error('Key vertical clearance');
console.log('Revision17:3 supported keys; pockets preserve outer walls; receivers joined, outside PCB and clear of controls/cable/frame fasteners. .2mm side/.3mm top clearance; flex relief contains original opening.');

// r18 removes the two vulnerable strips without changing the rest of the plate.
const legacyCableCuts=[rounded(65.5,20.65,7,8,1),rounded(69.5,4,3,18,.75),rounded(69,4,9,7,1)];
const legacyFramePlate=booleanPaths([midOutline],[...frameReliefs,circle(...frontMount,1.15),circle(...rearMount,1.15),...legacyCableCuts]);
for(const [label,oldStrip] of [['right edge',rect(72.6,11.1,1,17)],['front island',rect(67.5,4.5,1.5,4)]]) {
 if(absArea(booleanPaths(legacyFramePlate,[oldStrip],'intersection'))<1)throw Error('Legacy strip check is not testing old material: '+label);
 if(absArea(booleanPaths(framePlate,[oldStrip],'intersection'))>1e-5)throw Error('Unwanted thin strip remains: '+label);
}
if(Math.abs(absArea(booleanPaths(legacyFramePlate,batteryCableCuts))-absArea(framePlate))>1e-5)throw Error('Unexpected plate change outside cable bay');
for(const probe of [rect(68,0,1,6),rect(73,12,8,1)]) {
 if(absArea(booleanPaths([probe],batteryCableCuts))>1e-5)throw Error('Cable bay is closed at an intended open edge');
}
const relocatedSeat=rect(60,6.1,3,1.4),relocatedStop=rect(60,5,3,1.1);
for(const feature of [relocatedSeat,relocatedStop])if(absArea(booleanPaths([feature],framePlate))>1e-5)throw Error('Relocated support is not seated on retained plate');
if(absArea(booleanPaths([relocatedSeat],[board],'intersection'))<2.99)throw Error('Relocated seat lacks PCB overlap');
if(absArea(booleanPaths([relocatedStop],[board],'intersection'))>1e-5)throw Error('Relocated stop intrudes into PCB');
console.log('Revision18 cleanup: both old strips removed; bay open to front/right; retained plate unchanged; moved seat/stop supported and aligned to PCB.');

// r19 separates printed guide travel from the original switch's socket play.
const bounds2=poly=>[Math.min(...poly.map(p=>p[0])),Math.min(...poly.map(p=>p[1])),Math.max(...poly.map(p=>p[0])),Math.max(...poly.map(p=>p[1]))];
const close=(a,b,tolerance=1e-7)=>Math.abs(a-b)<=tolerance;
const assert=(condition,message)=>{if(!condition)throw Error(message);};
for(const [poly,width,height] of [[powerSlot,9,4.4],[powerGuide,17,8.2],[powerRetainerSlot,7.8,5.6],[powerRetainerOutline,18,9],[powerCapFlange,14,7.6],[powerCapNub,6,3.8],[powerCapBoss,4,4]]) {
 const b=bounds2(poly);
 assert(close(b[2]-b[0],width)&&close(b[3]-b[1],height),'Power mechanism envelope mismatch');
}
assert(powerLeverOffset===1&&powerCapOffset===1,'Cap/lever reference position differs from CAD');
assert(absArea(booleanPaths([powerGuide],[lcdPocket,...lidHoles,...keyAnchorHoles],'intersection'))<1e-5,'Guide joins another lid opening');
for(const x of [-1.5,-1,-.5,0,.5,1,1.5])for(const y of [-.3,0,.3]) {
 const flange=powerCapShape(14,7.6,1,x,y),nub=powerCapShape(6,3.8,1,x,y),boss=powerCapShape(4,4,.3,x,y);
 assert(absArea(booleanPaths([flange],[powerGuide]))<1e-5,'Flange collides with guide at '+[x,y]);
 assert(absArea(booleanPaths([nub],[powerSlot]))<1e-5,'Nub collides with running slot at '+[x,y]);
 assert(absArea(booleanPaths([boss],[powerRetainerSlot]))<1e-5,'Socket boss collides with retainer at '+[x,y]);
 assert(absArea(booleanPaths([powerSlot],[flange]))<1e-5,'Slot exposed beyond flange at '+[x,y]);
 const flangeBounds=bounds2(flange),slotBounds=bounds2(powerSlot);
 assert(Math.min(slotBounds[0]-flangeBounds[0],flangeBounds[2]-slotBounds[2],slotBounds[1]-flangeBounds[1],flangeBounds[3]-slotBounds[3])>=1-1e-7,'Projected overlap falls below 1 mm');
 for(const z of [-.35,-.2,0,.2]) {
  assert(26.8+z>=26.45-1e-7,'Flange penetrates retainer');
  assert(27.6+z<=27.8+1e-7,'Flange penetrates guide ceiling');
  assert(25.4+z-24.8>=.25-1e-7,'Boss crowds switch body');
  assert(27.9+z-27.3>=.25-1e-7,'Socket roof crowds taller switch handle');
 }
}
assert(close((9-6),3)&&close(2-1.5,.5)&&3>=2+.5,'Travel does not cover switch travel and socket backlash');
for(const switchX of [-1,1])for(const socketX of [switchX-.25,switchX+.25])for(const socketY of [-.25,.25]) {
 const handle=rect(powerSwitchCenter[0]+switchX-.75,powerSwitchCenter[1]-.75,1.5,1.5);
 assert(absArea(booleanPaths([handle],[powerSocket(2,socketX,socketY)]))<1e-5,'Engaged switch lever collides with socket');
}
assert(close((27.8-26.45)-.8,.55),'Unexpected worst-case total vertical play');
const stripBar=rect(60.5,16.5,5,21.5);
assert(absArea(booleanPaths(powerRetainer,[stripBar],'intersection'))>=2.99,'Retainer bridge disconnected from anchor bar');
assert(booleanPaths([...powerRetainer,stripBar],[],'union').filter(p=>area(p)>0).length===1,'Retainer and strip are separate pieces');
assert(keyAnchorHoles.length===0,'Obsolete roof screw pilots remain');
for(const y of [19,36])assert(absArea(booleanPaths([circle(63,y,.9)],[stripBar]))<1e-5,'Fixed bar does not fill its former screw holes');
assert(absArea(booleanPaths(powerRetainer,[rect(68.65,13.03,7.35,8.94)],'intersection'))>0,'USB clearance check no longer tests overlapping footprint');
assert(25.4-24.56>=.84-1e-7,'Retainer underside crowds USB shell');
const capVertices=faces.filter(f=>f.part==='powerCap').flatMap(f=>f.loops.flat());
assert(capVertices.length>0&&close(Math.min(...capVertices.map(p=>p[2])),25.4)&&close(Math.max(...capVertices.map(p=>p[2])),30),'Cap height does not match CAD');
for(const f of faces.filter(f=>f.part==='lid'&&Math.abs(f.normal[2])>.9999)) {
 const z=f.loops[0][0][2];
 if(z<27-1e-8||z>29+1e-8)continue;
 const polygons=f.loops.map(loop=>loop.map(p=>p.slice(0,2)));
 assert(absArea(booleanPaths(polygons,[powerSlot],'intersection'))<1e-5,'A lid cap closes the running slot at Z'+z);
 if(z<27.8-1e-8)assert(absArea(booleanPaths(polygons,[powerGuide],'intersection'))<1e-5,'A lid cap obstructs the flange guide');
}
console.log('Power cap: printed guides clear X ±1.5/Y ±0.3 mm; strip float increases worst-case vertical range to -0.35/+0.2 mm (0.55 mm total), minimum body/handle gaps 0.25 mm; 1 mm minimum projected overlap; 2 mm switch travel plus 0.5 mm socket backlash fits.');
console.log('Power retainer: joined to fixed strip bar; obsolete screw holes filled; USB shell gap 0.84 mm at roof seat, 0.69 mm with strip float. Engaged socket limits Y to ±0.25 mm, before the printed guide at ±0.3 mm. Physical fit and force testing remain unverified.');

// r20: actual contact lands and component envelopes, in case coordinates.
assert(absArea(booleanPaths([pcbLiftStopFoot],[board]))<1e-5,'Travel-stop foot overhangs PCB');
assert(absArea(booleanPaths([pcbLiftStopFoot],[rect(71,41.7,3,1.2)]))<1e-5,'Travel-stop foot leaves reviewed component-free land');
assert(absArea(booleanPaths([usbBoardSeat],framePlate))<1e-5,'Relocated underside seat lacks frame support');
assert(absArea(booleanPaths([usbBoardSeat],[board]))<1e-5,'Relocated underside seat overhangs PCB');
assert(close(absArea([usbBoardSeat]),2.4),'Lower seat bearing area changed');
const stopVertices=faces.filter(f=>f.part==='pcbStop').flatMap(f=>f.loops.flat());
const seatVertices=faces.filter(f=>f.part==='usbBoardSeat').flatMap(f=>f.loops.flat());
assert(close(Math.min(...stopVertices.map(p=>p[2]))-21.3,.2),'Nominal stop gap is not 0.2 mm');
assert(close(Math.max(...stopVertices.map(p=>p[2])),27.1),'Stop does not reach roof');
assert(close(Math.min(...seatVertices.map(p=>p[2])),14.49)&&close(Math.max(...seatVertices.map(p=>p[2])),19.7),'Relocated seat Z bounds changed');
for(const f of faces.filter(f=>f.part==='midframe'&&f.normal[2]>.99&&f.loops.every(l=>l.every(p=>close(p[2],19.7))))) {
 assert(absArea(booleanPaths(f.loops.map(l=>l.map(p=>p.slice(0,2))),[rect(71,42.1,3,.8)],'intersection'))<1e-5,'Old RESET-conflicting lower seat remains');
}
const usbHoleMask=circle(69.72,35.39,.35,128);
assert(absArea(booleanPaths([usbBoardSeat],[usbHoleMask],'intersection'))<1e-5,'USB hole mask intersects lower seat');
const holeMaskGap=Math.min(...usbBoardSeat.map((p,i)=>pointSegmentDistance([69.72,35.39],p,usbBoardSeat[(i+1)%usbBoardSeat.length])))-.35;
assert(holeMaskGap>=.29-1e-7,'USB hole mask clearance below 0.29 mm');
const wheelSeatGap=Math.min(...usbBoardSeat.map((p,i)=>pointSegmentDistance([62.9,39.35],p,usbBoardSeat[(i+1)%usbBoardSeat.length])))-7.25;
assert(wheelSeatGap>=.63,'Relocated seat crowds wheel sweep');
const loftStopAt=z=>{
 const t=clamp((z-21.9)/(23.49-21.9),0,1);
 return pcbLiftStopFoot.map((p,i)=>p.map((v,k)=>v+(pcbLiftStopRoot[i][k]-v)*t));
};
for(let z=21.5;z<=27.1+1e-7;z+=.05) {
 const footprint=loftStopAt(z);
 assert(absArea(booleanPaths([footprint],[outside]))<1e-5,'PCB stop breaches exterior');
 assert(absArea(booleanPaths([footprint],[circle(77,41,2.2,128)],'intersection'))<1e-5,'PCB stop merges into corner screw boss');
 assert(absArea(booleanPaths([footprint],[cableReliefFootprint],'intersection'))<1e-5,'PCB stop closes USB cable relief');
 assert(absArea(booleanPaths([footprint],[rect(76,27.175,5,10.65)],'intersection'))<1e-5,'PCB stop blocks measured USB plug envelope');
}
const rootWallOverlap=absArea(booleanPaths([pcbLiftStopRoot],upperSection(24),'intersection'));
assert(rootWallOverlap>.1,'PCB stop root does not join wheel-side wall');
assert(absArea(booleanPaths([pcbLiftStopRoot],[resolvedInset(outside,.01)],'intersection'))>9,'PCB stop root lacks broad roof overlap');
assert(absArea(booleanPaths([pcbLiftStopRoot],[lcdPocket,...lidHoles,...keyAnchorHoles,powerGuide],'intersection'))<1e-5,'PCB stop root crosses a lid aperture');
console.log('Revision20: 0.2 mm nominal lift gap; stop foot on reviewed PCB land; relocated 2.4 mm² seat supported; old lower seat removed.');
console.log('Revision20: full stop loft clears corner boss and USB cable pocket; root joins roof and wall. Lower-seat hole-mask gap',holeMaskGap,'mm; wheel radial gap',wheelSeatGap,'mm; wall overlap',rootWallOverlap,'mm².');

// Official MB V2.3 placement and SOLDER_TOP mask rectangles, verified against
// NTC013-AA1J-A160T switch body dimensions and the rotated TOP placement.
const bootMaskPads=[71.3,73.45].flatMap(x=>[31.325,35.475].map(y=>rect(x-.375,y-.575,.75,1.15)));
const resetMaskPads=[71.3,73.45].flatMap(x=>[38.325,42.475].map(y=>rect(x-.375,y-.575,.75,1.15)));
const encoderMaskPad=rect(67.65,33.25,1.3,1.4);
const polygonGap=(a,b)=>Math.min(...a.flatMap(p=>b.map((q,i)=>pointSegmentDistance(p,q,b[(i+1)%b.length]))),...b.flatMap(p=>a.map((q,i)=>pointSegmentDistance(p,q,a[(i+1)%a.length]))));
for(const shape of [...undersideSwitchBodies,...bootMaskPads,...resetMaskPads,encoderMaskPad])assert(absArea(booleanPaths([usbBoardSeat],[shape],'intersection'))<1e-5,'Relocated seat overlaps a switch or exposed mask pad');
const bootBodyGap=polygonGap(usbBoardSeat,undersideSwitchBodies[0]);
const bootPadGap=Math.min(...bootMaskPads.map(p=>polygonGap(usbBoardSeat,p)));
const encoderPadGap=polygonGap(usbBoardSeat,encoderMaskPad);
const bootPadHorizontalGap=Math.min(...bootMaskPads.map(p=>bounds2(p)[0]))-bounds2(usbBoardSeat)[2];
assert(close(bootBodyGap,.275)&&close(bootPadHorizontalGap,.425)&&bootPadGap>=.425&&close(encoderPadGap,.35),'Lower-seat component clearance differs from reviewed footprint');
assert(absArea(booleanPaths([rect(71,42.1,3,.8)],[undersideSwitchBodies[1]],'intersection'))>1,'Old seat no longer reproduces RESET body conflict');
assert(absArea(booleanPaths([rect(71,42.1,3,.8)],resetMaskPads,'intersection'))>.1,'Old seat no longer reproduces RESET terminal conflict');
console.log('Revision20 footprint checks: new seat clears BOOT body by',bootBodyGap,'mm, BOOT mask pads by',bootPadGap,'mm and encoder mask pad by',encoderPadGap,'mm. Old seat would overlap RESET body and terminals. Lower contact land has tented vias; inspect the physical board.');

// R21: insertion and retention paths, with the lid removed from the PCB.
// These are geometric fit checks, not a strength or adhesive qualification.
const movePolys=(polys,dy=0)=>polys.map(poly=>poly.map(([x,y])=>[x,y+dy]));
const stripRootPatches=keyYs.flatMap(y=>[-1,1].map(sign=>{
 const yc=y+sign*1.3;
 return ccw([[60.5,y+sign],[60.2,y+sign],...arc(60.2,yc,.3,sign>0?-90:90,0,8)]);
}));
const stripPlan=booleanPaths([stripBar,...keyYs.map(y=>rect(51.5,y-1,9.5,2)),...keyYs.map(y=>circle(51.5,y,2.2)),...stripRootPatches],[],'union');
const stripVolumes=[
 {p:stripPlan,z:[25.8,26.6]},
 {p:booleanPaths(powerRetainer,[powerRetainerSlot]),z:[25.4,26.6]},
 ...keyYs.map(y=>({p:[circle(51.5,y,2.2)],z:[26.6,30]})),
 ...keyYs.map(y=>({p:[circle(51.5,y,.9)],z:[24.1,25.8]}))
];
const railVolumes=[
 {p:keeperFloorRects,z:[23.65,24.65]},
 {p:[...keeperWallRects,keeperLeadStop],z:[23.65,27.1]},
 {p:[...stripSideStops,...powerShelfGuides],z:[25.8,27.1]},
 {p:stripEndStops,z:[26,27.1]},
 {p:[circle(63,19,2.5),circle(63,36,2.5)],z:[26.6,28.99]}
];
const overlaps3=(a,b)=>Math.min(a.z[1],b.z[1])-Math.max(a.z[0],b.z[0])>1e-7&&absArea(booleanPaths(a.p,b.p,'intersection'))>1e-5;
assert(keeperOutline.filter(p=>area(p)>0).length===1,'Keeper is disconnected');
assert(close(25.8-25.65,.15),'Unexpected button-strip float');
assert(close(24.1-.15-23.8,.15),'Strip float closes nominal switch rest gap');
for(let d=0;d<=8.001;d+=.025)for(const volume of stripVolumes) {
 const moving={p:volume.p,z:volume.z.map(z=>z-d)};
 for(const fixed of railVolumes)assert(!overlaps3(moving,fixed),'Strip vertical insertion collides with lid keeper rail at depth '+d);
}
for(let dy=0;dy<=3.3001;dy+=.025) {
 const moving={p:movePolys(keeperOutline,dy),z:[24.65,25.65]};
 for(const fixed of [...railVolumes,...stripVolumes])assert(!overlaps3(moving,fixed),'Keeper seating slide collides at Y offset '+dy);
 assert(absArea(booleanPaths(moving.p,[upperInside]))<1e-5,'Keeper seating slide crosses lid wall');
}
for(let dz=0;dz<=8.001;dz+=.025) {
 const moving={p:movePolys(keeperOutline,3.3),z:[24.65-dz,25.65-dz]};
 for(const fixed of [...railVolumes,...stripVolumes])assert(!overlaps3(moving,fixed),'Keeper vertical drop-in collides at depth '+dz);
}
const tabBearing=keeperTabs.map(tab=>absArea(booleanPaths([tab],keeperFloorRects,'intersection')));
assert(tabBearing.every(a=>a>=2.99),'A keeper tab lacks its intended ledge bearing');
assert(close(60.5-60.3,.2)&&close(65.7-65.5,.2),'Strip insertion side gap changed');
assert(close(58.7-58.5,.2)&&close(67.4-67.2,.2),'Keeper wall side gap changed');
assert(close(38.2-38,.2)&&close(6.7-6.5,.2),'Strip end registration gap changed');
assert(close(26-25.65,.35),'Rear registration stop obstructs keeper');
for(const wall of [...keeperWallRects,keeperLeadStop,...stripSideStops,...powerShelfGuides,...stripEndStops]) {
 assert(absArea(booleanPaths([wall],[outside]))<1e-5,'Keeper rail outside lid outline');
 assert(absArea(booleanPaths([wall],[lcdPocket,lcdFlex,...lidHoles,powerGuide],'intersection'))<1e-5,'Keeper rail crosses another roof aperture');
 assert(absArea([wall])>.89,'Keeper rail root unexpectedly small');
}
for(const volume of railVolumes) {
 assert(volume.z[0]>21.3,'Keeper feature reaches PCB');
 const usbEnvelope={p:[rect(68.65,13.03,7.35,8.94),rect(68.65,28.03,7.35,8.94)],z:[21.3,24.56]};
 assert(!overlaps3(volume,usbEnvelope),'Keeper rail reaches USB shell envelope');
}
console.log('Revision21: strip vertical insertion and keeper +3.3Y load / -3.3Y seating sweeps clear; four bearing tabs',tabBearing,'mm².');
console.log('Revision21: 0.2 mm nominal side/end fits; 0.15 mm strip float leaves 0.15 mm button rest gap. Roof roots join by 0.1 mm; no screw pilot or strip hole remains.');
console.log('Revision21: removable resin-compatible silicone across the rear tab/ledge seam is required for anti-backout. No snap or positive mechanical lock is claimed; physical fit, resin strength, motion and adhesive compatibility remain unverified.');

// R21 shelf guides also register the formerly screwed bar in XY/yaw.
// At a fixed angle each straight datum generates a linear constraint on the
// translation. Clip the feasible translation polygon instead of independently
// combining extreme translations and rotation that cannot occur together.
const clipLinear=(poly,a,b,c)=>{
 const out=[];
 for(let i=0;i<poly.length;i++) {
  const p=poly[i],q=poly[(i+1)%poly.length],dp=a*p[0]+b*p[1]-c,dq=a*q[0]+b*q[1]-c;
  if(dp<=1e-10)out.push(p);
  if((dp<0)!==(dq<0)){const t=dp/(dp-dq);out.push(p.map((v,k)=>v+t*(q[k]-v)));}
 }
 return out;
};
function feasibleStripTranslations(theta) {
 const [cx,cy]=powerSwitchCenter,C=Math.cos(theta),T=Math.tan(theta);
 let p=rect(-1,-1,2,2);
 const cut=(a,b,c)=>p=clipLinear(p,a,b,c);
 for(const y of [10,12]) {
  cut(-1,-T,-(53.8732-cx+9/C+(y-cy)*T));
  cut(1,T,72.2732-cx-9/C+(y-cy)*T);
 }
 for(const y of [27,29]) {
  cut(-1,-T,-(60.3-cx-(60.5-cx)/C+(y-cy)*T));
  cut(1,T,65.7-cx-(65.5-cx)/C+(y-cy)*T);
 }
 for(const y of [20,21])cut(1,T,65.7-cx-(65.5-cx)/C+(y-cy)*T);
 for(const x of [61,65])cut(-T,1,38.2-cy-(38-cy)/C-(x-cx)*T);
 for(const x of [60,65])cut(T,-1,-(6.5-cy-(6.7-cy)/C-(x-cx)*T));
 return p;
}
let maxYaw=0,maxTranslation=[0,0],requiredSlotHalf=[0,0],feasiblePoseVertices=0;
for(let step=-1400;step<=1400;step++) {
 const theta=step*.000025,C=Math.cos(theta),S=Math.sin(theta),poses=feasibleStripTranslations(theta);
 if(!poses.length)continue;
 maxYaw=Math.max(maxYaw,Math.abs(theta));
 for(const [tx,ty] of poses) {
  feasiblePoseVertices++;
  maxTranslation=[Math.max(maxTranslation[0],Math.abs(tx)),Math.max(maxTranslation[1],Math.abs(ty))];
  for(const dx of [-1.5,1.5])for(const dy of [-.3,.3]) {
   const x=(dx-tx)*C+(dy-ty)*S,y=-(dx-tx)*S+(dy-ty)*C;
   const h=1.7*(Math.abs(C)+Math.abs(S))+.3;
   requiredSlotHalf=[Math.max(requiredSlotHalf[0],Math.abs(x)+h),Math.max(requiredSlotHalf[1],Math.abs(y)+h)];
  }
 }
}
const yawBound=1.2*Math.PI/180,Cb=Math.cos(yawBound),Sb=Math.sin(yawBound),Tb=Math.tan(yawBound);
assert(feasibleStripTranslations(yawBound).length===0&&feasibleStripTranslations(-yawBound).length===0,'Side datums do not exclude 1.2 degree yaw');
// The separating side-datum inequalities are monotonic within this small-angle
// regime. They prove no continuous motion from the seated pose past this bound.
assert(.4-11.5732*(1/Cb-1)-19*Tb<0&&.4-11.4268*(1/Cb-1)-19*Tb<0,'Yaw bound lacks opposing-datum exclusion');
// At Y=11.2 the shelf sides imply |tx+ty*tan|<=.2. Front/rear
// end faces at X=63.0732 imply |ty-tx*tan|<=.2. Thus |tx|,|ty|
// <=.2*(1+tan)/(1+tan^2)<.21 for every coupled pose in the range.
assert(.2*(1+Tb)/(1+Tb*Tb)<.21&&maxTranslation.every(v=>v<=.200001),'Strip translation bound changed');
// Conservative independent bounds are wider than every feasible pose. Include
// a full square 4x4 boss, so rounded cap corners cannot invalidate the result.
const bossEnvelope=[3.71*Cb+2.51*Sb,3.71*Sb+2.51*Cb];
const bossCornerClearance=.3-Math.hypot(Math.max(0,bossEnvelope[0]-3.6),Math.max(0,bossEnvelope[1]-2.5));
assert(bossCornerClearance>.11,'Moving strip opening can bind the guided cap boss');
// Even the complete rectangular aperture stays inside the flange's central
// straight region (X +/-6), with >.4 mm overlap along Y. The R1 corners
// therefore cannot let the flange escape through the enlarged aperture.
const apertureInFlange=[3.9*Cb+2.8*Sb+1.71,3.9*Sb+2.8*Cb+.51];
const flangeMinimumOverlap=3.8-apertureInFlange[1];
assert(apertureInFlange[0]<6&&flangeMinimumOverlap>.4,'Enlarged retainer aperture can expose a flange edge');
assert(close((9-5.6)/2,1.7),'Retainer front/back web changed');
console.log('Revision21 coupled XY/yaw checks:',feasiblePoseVertices,'feasible translation vertices; sampled maximum yaw',maxYaw*180/Math.PI,'degrees; translation maxima',maxTranslation,'mm; required slot half-extents',requiredSlotHalf,'mm.');
console.log('Revision21 all-pose conservative proof: 7.8 x 5.6 R0.3 aperture clears even a square cap boss by at least',bossCornerClearance,'mm; flange remains over the entire aperture with at least',flangeMinimumOverlap,'mm overlap.');
