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

const cut=ccw([[75.8,36.8],[81,36.8],[81,38.6],[76,38.6],...arc(76,38.4,.2,90,180,12)]);
const post=circle(77,41,2.2,256),tip=circle(77,41,1.2,256);
const absArea=paths=>paths.reduce((n,p)=>n+Math.abs(area(p)),0);
const pseg=(p,a,b)=>{const dx=b[0]-a[0],dy=b[1]-a[1],t=Math.max(0,Math.min(1,((p[0]-a[0])*dx+(p[1]-a[1])*dy)/(dx*dx+dy*dy)));return Math.hypot(p[0]-a[0]-t*dx,p[1]-a[1]-t*dy);};
function sampledGap(boundary,others){let min=Infinity,at;for(const poly of boundary)for(let i=0;i<poly.length;i++){const a=poly[i],b=poly[(i+1)%poly.length],n=Math.ceil(Math.hypot(b[0]-a[0],b[1]-a[1])/.002);for(let j=0;j<=n;j++){const p=[a[0]+(b[0]-a[0])*j/n,a[1]+(b[1]-a[1])*j/n];for(const q of others)for(let k=0;k<q.length;k++){const d=pseg(p,q[k],q[(k+1)%q.length]);if(d<min){min=d;at=p;}}}}return{min,at};}
for(const [band,cavity] of [['hardware',upperInside],['reinforced',upperReinforcedInside]]){
 const voids=booleanPaths([cavity],[post]);
 const cutFootprint=booleanPaths([cut],[outside],'intersection');
 const gap=sampledGap(cutFootprint,voids);
 const overlap=absArea(booleanPaths([cut],voids,'intersection'));
 if(gap.min<.75||overlap>1e-7)throw Error('Cavity web or intersection failed');
 console.log(JSON.stringify({band,minCavityWeb:gap.min,at:gap.at,cutIntoCavityArea:overlap}));
 const shell=booleanPaths([outside],[cavity]);
 const before=booleanPaths([...shell,post],[],'union');
 const after=booleanPaths(before,[cut]);
 const plug=rect(76,27.375,10,10.25);
 const plugArea=absArea(booleanPaths(after,[plug],'intersection'));
 if(plugArea>1e-7)throw Error('Measured plug XY collision');
 console.log(JSON.stringify({band,plugXYIntersectionArea:plugArea,nominalRearGap:38.6-37.625}));
}
const postTrim=absArea(booleanPaths([cut],[post],'intersection'));
const tipTrim=absArea(booleanPaths([cut],[tip],'intersection'));
if(postTrim>1e-7||tipTrim>1e-7)throw Error('Hardware geometry trimmed');
console.log(JSON.stringify({postTrimArea:postTrim,tipTrimArea:tipTrim,tipBoreMinimumWeb:41-1.2-38.6,upperPostGap:41-2.2-38.6}));
console.log(JSON.stringify({plugZ:[19.93,25.93],fullDepthCutZ:[19.7,26.3],verticalClearance:[19.93-19.7,26.3-25.93],roundedCutZ:[19.2,26.8],roofUncutFromZ:26.8,tipReliefRoofZ:22.5}));
const centers=[[3.6,3.6,2.6],[76.4,3.6,2.6],[3.6,52.4,2.6],[77,41,2.2]];
const countSolids=paths=>paths.filter(p=>area(p)>1e-7).length;
for(const z of [19.3,19.7,19.93,21.59,21.61,22.49,22.51,23.02,23.04,24.9,25.04,25.93,26.3,26.7]){
 const cavity=z<21.6?upperInside:upperReinforcedInside;
 let model=booleanPaths([...booleanPaths([outside],[cavity]),...centers.map(([x,y,r])=>circle(x,y,r,128))],[],'union');
 const openings=[];
 if(z<23.03)openings.push(rect(74.7,11,1.3,27));
 else if(z<25.03){const half=z<=24.53?4.9:4.4+Math.sqrt(Math.max(0,.25-(z-24.53)**2));for(const y of [17.5,32.5])openings.push(rect(74.3,y-half,2,2*half));}
 if(z<22.5)openings.push(...centers.map(([x,y])=>circle(x,y,1.2,128)));
 model=booleanPaths(model,openings);
 const d=z<19.7?19.7-z:z>26.3?z-26.3:0,inset=.5-Math.sqrt(Math.max(0,.25-d*d));
 const slice=booleanPaths([cut],[rect(75.8,36.8+inset,5.2,1.8-2*inset)],'intersection');
 const after=booleanPaths(model,slice);
 if(countSolids(after)!==countSolids(model))throw Error('Unexpected material connectivity change atZ'+z);
}
console.log('All14 sampled pocket/USB/roof height sections retain material connectivity.');
