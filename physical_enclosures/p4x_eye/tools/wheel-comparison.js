// Render a measured before/after figure directly from the viewer's geometry.
const fs=require('fs');
const {createCanvas}=require('@napi-rs/canvas');
const ClipperLib=require('./clipper-6.4.2.js');
const input='~/.codex/visualizations/2026/09/27/01a0e470-16f8-7de2-afc9-6933b2a473e2/p4x-enclosure-unified-body.html';
const html=fs.readFileSync(input,'utf8');
let code=[...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].at(-1)[1];
code=code.slice(code.indexOf('      const faces = [];'),code.indexOf('      const lift='));
const build=source=>new Function('ClipperLib',source+'\nreturn faces;')(ClipperLib);
let beforeCode=[...fs.readFileSync(__dirname+'/revision13/viewer.html','utf8').matchAll(/<script>([\s\S]*?)<\/script>/g)].at(-1)[1];
beforeCode=beforeCode.slice(beforeCode.indexOf('      const faces = [];'),beforeCode.indexOf('      const lift='));
const after=build(code),before=build(beforeCode);
const W=660,H=390,yaw=2.88,pitch=.78,scale=18;
const c=Math.cos(yaw),s=Math.sin(yaw),cp=Math.cos(pitch),sp=Math.sin(pitch);
const xyz=p=>{const x=p[0]-65,y=p[1]-44.8,z=p[2]-18;const u=c*x-s*y,v=s*x+c*y;return[u,-v*sp-z*cp,-v*cp+z*sp];};
const normal=n=>{const u=c*n[0]-s*n[1],v=s*n[0]+c*n[1];return[u,-v*sp-n[2]*cp,-v*cp+n[2]*sp];};
const palette={base:[112,116,120],lid:[119,123,127],midframe:[107,183,131],midBore:[49,54,51],mount:[107,183,131],mountBore:[49,54,51],pcb:[104,170,211],battery:[200,143,83],wheel:[237,150,197],wheelHub:[159,92,133],buttons:[165,137,206],switch:[100,102,105],lcd:[60,65,70],screen:[22,27,30],usb:[175,180,185],usbBore:[28,31,33],baseBore:[45,49,53],lidBore:[45,49,53],insert:[142,206,188],frameInsert:[142,206,188],screw:[202,205,208],screwSlot:[62,64,66],frameScrew:[202,205,208],frameScrewSlot:[62,64,66]};
function render(faces){
 const canvas=createCanvas(W,H),ctx=canvas.getContext('2d'),pixels=ctx.createImageData(W,H),buf=pixels.data;
 for(let i=0;i<W*H;i++){buf[i*4]=25;buf[i*4+1]=28;buf[i*4+2]=31;buf[i*4+3]=255;}
 const depth=new Float64Array(W*H);depth.fill(-Infinity);
 const ox=W/2,oy=H/2;
 const list=faces.filter(f=>!f.part.startsWith('cut')).map(f=>({...f,n:normal(f.normal),loops:f.loops.map(l=>l.map(xyz))})).filter(f=>f.n[2]>.00001);
 for(const f of list){
  const loops=f.loops.map(l=>l.map(p=>[ox+p[0]*scale,oy+p[1]*scale]));
  let low=H,high=0;for(const l of loops)for(const p of l){low=Math.min(low,p[1]);high=Math.max(high,p[1]);}
  const lo=Math.max(0,Math.ceil(low-.5)),hi=Math.min(H,Math.ceil(high-.5));
  const p0=f.loops[0][0],nx=f.n[0]/f.n[2],ny=f.n[1]/f.n[2],dzdx=-nx/scale;
  const light=Math.max(.48,Math.min(1,.73-f.n[0]*.1-f.n[1]*.16+f.n[2]*.14));
  const fill=palette[f.part].map(v=>Math.round(v*light));
  for(let y=lo;y<hi;y++){
   const scan=y+.5,hits=[];
   for(const l of loops)for(let i=0;i<l.length;i++){const a=l[i],b=l[(i+1)%l.length];if((a[1]<=scan&&b[1]>scan)||(b[1]<=scan&&a[1]>scan))hits.push(a[0]+(scan-a[1])*(b[0]-a[0])/(b[1]-a[1]));}
   hits.sort((a,b)=>a-b);
   for(let j=0;j+1<hits.length;j+=2){
    const x0=Math.max(0,Math.ceil(hits[j]-.5)),x1=Math.min(W,Math.ceil(hits[j+1]-.5));
    let z=p0[2]-nx*((x0+.5-ox)/scale-p0[0])-ny*((scan-oy)/scale-p0[1]);
    for(let x=x0;x<x1;x++,z+=dzdx){const k=y*W+x;if(z<depth[k]-.0001)continue;depth[k]=z;const o=k*4;buf[o]=fill[0];buf[o+1]=fill[1];buf[o+2]=fill[2];}
   }
  }
 }
 ctx.putImageData(pixels,0,0);return canvas;
}
const page=createCanvas(1424,588),ctx=page.getContext('2d');
ctx.fillStyle='#191c1f';ctx.fillRect(0,0,page.width,page.height);
ctx.fillStyle='#f0f1f2';ctx.font='500 28px Arial';ctx.fillText('Rotary wheel — before / after',36,43);
ctx.font='18px Arial';ctx.fillStyle='#bec4c9';ctx.fillText('Same angle and scale · another 0.5 mm of wheel access · corner mount aligned',36,76);
const panels=[{x:36,title:'BEFORE · revision 13',detail:'1.6 mm rim exposure · raised corner',faces:before},{x:728,title:'AFTER · revision 14',detail:'2.1 mm rim exposure · continuous rear edge',faces:after}];
for(const p of panels){
 ctx.fillStyle='#f0f1f2';ctx.font='500 21px Arial';ctx.fillText(p.title,p.x,119);
 ctx.drawImage(render(p.faces),p.x,140);
 ctx.font='20px Arial';ctx.fillStyle='#e2e5e8';ctx.fillText(p.detail,p.x,564);
}
const output='physical_enclosures/p4x_eye/wheel-before-after.png';
fs.writeFileSync(output,page.toBuffer('image/png'));
console.log(output);
