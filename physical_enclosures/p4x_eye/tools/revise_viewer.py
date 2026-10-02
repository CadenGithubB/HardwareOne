from pathlib import Path
folder = Path(__import__('os').path.expanduser('~/.codex/visualizations/2026/09/27/01a0e470-16f8-7de2-afc9-6933b2a473e2'))
p = folder / 'p4x-enclosure-unified-body.html'
s = (folder / 'p4x-enclosure-rounded-edges.html').read_text()
s = s.replace('p4x-rounded-edges', 'p4x-unified-body').replace('p4xRoundedEdges', 'p4xUnifiedBody')
s = s.replace('Raised keys · rounded edges', 'Unified enclosure · R2 edges')
s = s.replace('value="usb" selected', 'value="usb"').replace('value="edge">', 'value="edge" selected>')
s = s.replace("yaw:-1.35, pitch:.45", "yaw:2.82, pitch:.64").replace("view:'usb'", "view:'edge'")
s = s.replace('Battery · moved left 0.75 mm', 'Battery · moved forward 6 mm')
start = s.index(' aria-label="Four M2')
end = s.index('>3D preview', start)
s = s[:start] + ' aria-label="Interactive enclosure preview with matching upper and lower wheel contours and 2 millimeter outer face-edge radii. Battery moved 6 millimeters toward the front. Two independent midframe screws, at front-left and rear, hold a flat battery cover. A blind screw boss on the midframe supports the ESP32 mounting tab. USB-C faces project 0.2 millimeters, with lower saddles belonging to the base. Four enclosed bottom wells accept M2 by 8 millimeter screws into blind lid inserts, leaving the top closed. Display is flush and three keys rise 1 millimeter. Component, printed hardware and cable fit remain provisional. Drag to rotate and scroll to zoom."' + s[end:]
s = s.replace('Two independent midframe screws, at front-left and rear, hold a flat battery cover.', 'Two M2 by 8 millimeter nylon screws pass through the midframe into 3 millimeter diameter, 3.2 millimeter long inserts in the front-left and rear base posts. Blind pockets leave room for the screw tips. The rear support rib ends inside the wall.')
s = s.replace('Battery moved 6 millimeters toward the front.', 'The outer wheel area is recessed by 0.5 millimeters with smooth transitions, exposing 1.6 millimeters of the rim. The inner cavity is unchanged. Battery moved 6 millimeters toward the front.')

def section(a, b, replacement):
    global s
    begin, end = s.index(a), s.index(b, s.index(a))
    s = s[:begin] + replacement + s[end:]

section('      function cap(', '      function sides(', '''      const scaleInt=100000;
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
''')
section('      function solid(', '      function inPoly(', '''      function regionSolid(polys,z0,z1,part) {
        for(const p of polys)sides(p,z0,z1,part);
        capPaths(polys,z0,false,part);capPaths(polys,z1,true,part);
      }
      function solid(poly,z0,z1,part,holes=[]) {
        regionSolid(booleanPaths([ccw(poly)],holes),z0,z1,part);
      }
''')
section('      function differenceSolid(', '      const usbWindows=', '''      function differenceSolid(poly,z0,z1,part,holes=[]) {solid(poly,z0,z1,part,holes);}
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
      const wheelRelief=x=>.5*Math.min(ease((x-53.65)/2),ease((72.15-x)/2));
      const exteriorSamples=nominal.flatMap((p,i)=>{
        const q=nominal[(i+1)%nominal.length];
        const n=p[1]>=45.5-1e-8&&q[1]>=45.5-1e-8?Math.max(1,Math.ceil(Math.hypot(q[0]-p[0],q[1]-p[1])/.25)):1;
        return Array.from({length:n},(_,j)=>[p[0]+(q[0]-p[0])*j/n,p[1]+(q[1]-p[1])*j/n]);
      });
      const common=exteriorSamples.map(([x,y])=>[x,y-(y>=45.5-1e-8?wheelRelief(x):0)]);
      const nominalOutside=ccw(largest(booleanPaths([nominal],[],'union')));
      const outside=ccw(largest(booleanPaths([common],[],'union'))),upperOutside=outside;
      function interior(general,local) {
        const extra=booleanPaths(offsetPaths([nominalOutside],-local),[rect(70,7,10,33.8)],'intersection');
        return ccw(largest(booleanPaths([...offsetPaths([nominalOutside],-general),...extra],[],'union')));
      }
      const inside=interior(2,1.6),upperInside=interior(1.6,1);
''')
section('      // Convex clipping keeps', '      function taperedSides(', '''      // General polygon intersection supports the common concave housing.
      function convexClip(poly,clip) {return ccw(largest(booleanPaths([ccw(poly)],[clip],'intersection')));}
''')
s=s.replace('[77,41.6,2.2]', '[77,41.5,2.2]')
s=s.replace("sides(outside,1,14.5,'base')", "sides(outside,2,14.5,'base')")
s=s.replace("resolvedInset(outside,1-Math.cos(a1)),1-Math.sin(a1),1-Math.sin(a0)", "resolvedInset(outside,2*(1-Math.cos(a1))),2*(1-Math.sin(a1)),2*(1-Math.sin(a0))")
begin=s.index('      const rect=(x,y,w,h)=>',s.index('      function taperedSides'))
end=s.index('      for(const x of [10,72.5])',begin)
s=s[:begin]+'''      const mount=[21.1,8.7],frontMount=[5,8.7],rearMount=[44,48.5];
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
      const ledge=booleanPaths(offsetPaths([inside],.01),[...offsetPaths([inside],-1.4),rect(8.3,6.3,65.4,36.4),...shaftHoles,circle(...frontMount,1.55),circle(...rearMount,1.55)]);
      regionSolid(ledge,11.5,13,'base');
      const packKeepout=rect(7.8,5.8,66.4,37.4);
      for(const [x,y,r] of bossCenters) differenceSolid(convexClip(circle(x,y,r+.7),outside),11.5,13,'base',[packKeepout,circle(x,y,1.15)]);
      const midOutline=resolvedInset(inside,.3);
      const frameReliefs=bossCenters.map(([x,y])=>circle(x,y,2.9));
      differenceSolid(midOutline,13,14.5,'midframe',[circle(...frontMount,1.15),circle(...rearMount,1.15),...frameReliefs]);
      // Base-owned open-top USB saddle; the complete board/frame assembly drops in.
      alongX(rect(11.2,14.5,26.6,8.33),74.8,75.8,'base',usbWindows);
      face([[[74.2,11.2,14.5],[74.8,11.2,14.5],[74.8,11.2,17.5]]],[0,-1,0],'base');
      face([[[74.2,37.8,14.5],[74.8,37.8,17.5],[74.8,37.8,14.5]]],[0,1,0],'base');
      face([[[74.2,11.2,14.5],[74.8,11.2,17.5],[74.8,37.8,17.5],[74.2,37.8,14.5]]],[-.98058,0,.19612],'base');
      solid(circle(...mount,3),14.5,14.7,'midframe');
      solid(circle(...mount,3),14.7,19.7,'midframe',[circle(...mount,.6)]);
      sides(circle(...mount,.6),14.7,19.7,'midBore',true);cap(circle(...mount,.6),14.7,true,'midBore');
''' + s[end:]
section("      solid(upperOutside,27,27.1", '      for(const y of [14,36])', '''      // R2 quarter-circle face edge, native-offset slices matching the CAD source.
      for(let i=0;i<64;i++) {
        const a0=Math.PI*i/128,a1=Math.PI*(i+1)/128,z0=27+2*Math.sin(a0),z1=27+2*Math.sin(a1),rim=resolvedInset(upperOutside,2*(1-Math.cos(a1)));
        for(const [lo,hi,opening,anchors] of [[27,27.1,seatRearOpening,true],[27.1,27.5,pocketFlexOpening,true],[27.5,28.5,lcdPocket,true],[28.5,29,lcdPocket,false]]) {
          const low=Math.max(z0,lo),high=Math.min(z1,hi);
          if(high>low+1e-9)solid(rim,low,high,'lid',[...lidHoles,...(anchors?keyAnchorHoles:[]),opening]);
        }
      }
''')
s=s.replace('rounded(8.5,12.5,65,36,1)', 'rounded(8.5,6.5,65,36,1)')
s=s.replace('rect(x-1.5,42.7,3,1.2)', 'rect(x-1.5,42.7,3,x<50?1.2:.8)')
s=s.replace('midframe:42,pcb:65', 'midframe:42,midBore:42,frameScrew:42,frameScrewSlot:42,frameInsert:0,pcb:65')
s=s.replace('mountBore:mix(bg,fg,0.12),pcb:', 'mountBore:mix(bg,fg,0.12),midBore:mix(bg,fg,.12),pcb:')
s=s.replace('baseBore:mix(bg,fg,.14),lidBore:', 'frameInsert:palette.insert,frameScrew:palette.screw,frameScrewSlot:mix(bg,fg,.22),baseBore:mix(bg,fg,.14),lidBore:')
s=s.replace("['insert','screw','screwSlot','cutInsert','cutScrew']", "['insert','screw','screwSlot','frameInsert','frameScrew','frameScrewSlot','cutInsert','cutScrew']")
s=s.replace("return state.midframe||f.part!=='midframe';", "return state.midframe||!['midframe','midBore','frameScrew','frameScrewSlot'].includes(f.part);")
s=s.replace("'Head seat · 10.2 mm deep';", "state.view==='frame'?'Two frame screws · blind PCB mount':'Outer face edges · R2 mm';")
s=s.replace("'Insert · Ø3 × 3.2 mm';", "state.view==='frame'?'M2 × 8 mm · Ø3 × 3.2 mm inserts':state.view==='edge'?'Wheel exposure · 1.6 mm':'Matching upper + lower contour';")
s=s.replace('revision:10,', 'revision:13,').replace('outerEdgeRadiusMm:1,','outerEdgeRadiusMm:2,')
s=s.replace('wheelRimExposureMm:1.1', 'wheelRimExposureMm:1.6,wheelOutsideReliefMm:.5,wheelLocalWallMm:{upper:1.1,lower:1.5}')
s=s.replace('batteryCaseXYmm:[8.5,12.5],batteryShiftRightMm:1,batteryMoveFromRev8Mm:[-.75,0]', 'batteryCaseXYmm:[8.5,6.5],batteryMoveFromRev10Mm:[0,-6]')
s=s.replace('midframeScrewsMm:[[58.9,8.7],[5,46]]', 'midframeScrewsMm:[[5,8.7],[44,48.5]]')
s=s.replace('midframeZmm:[13,14.5]', 'midframeFasteners:{thread:"M2",screwLengthMm:8,insertMm:[3,3.2],plateHoleMm:2.3,pilotBoreMm:2.7,pilotDepthMm:3.4,headSeatZmm:14.5,insertZmm:[9.8,13],screwTipZmm:6.5,tipPocketBottomZmm:6,headDimensionsProvisional:true},midframeZmm:[13,14.5]')
s=s.replace('sharedLeftBoardScrew:true', 'sharedLeftBoardScrew:false,boardBossOwnedByMidframe:true,boardPilotFloorMm:1.7')
s=s.replace('lowerCaseFullShape:true', 'commonCaseContour:true,usbSaddleOwnedByBase:true')
# Revision 14: move the pack and corner hardware, keeping a uniform wheel edge.
section('      const wheelRelief=', '      const usbWindows=', '''      const wheelRelief=x=>ease((x-53.65)/2);
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
''')
section('      function upperSides(', '      const bossCenters=', '')
s=s.replace('[77,41.5,2.2]', '[77,41,2.2]')
s=s.replace('      upperSides(upperOutside);upperSides(upperInside,true);', '''      // Slice the shell at cavity changes and rounded USB aperture corners.
      // Each section is a resolved polygon, so the relocated corner remains closed.
      const upperLevels=[14.5,17.7,18.1,21.6,23.03,24.53,...Array.from({length:12},(_,i)=>24.53+.5*Math.sin((i+1)*Math.PI/24)),27];
      function upperSection(z) {
        const cavity=z<17.7||z>=21.6?upperReinforcedInside:upperInside,holes=[cavity];
        if(z<18.1)holes.push(rect(56.4,42.5,13,11.5));
        if(z<23.03)holes.push(rect(74.7,11,1.3,27));
        if(z>=23.03&&z<25.03){
          const half=z<=24.53?4.9:4.4+Math.sqrt(Math.max(0,.25-(z-24.53)**2));
          for(const y of [17.5,32.5])holes.push(rect(74.3,y-half,2,2*half));
        }
        return booleanPaths([upperOutside],holes);
      }
      for(let i=0;i<upperLevels.length-1;i++){
        const z0=upperLevels[i],z1=upperLevels[i+1];
        if(z1-z0>1e-8)regionSolid(upperSection((z0+z1)/2),z0,z1,'lid');
      }''')
section('      // Open-bottom wheel notch', '      // General polygon intersection', '''      function topY(poly,x) {
        const ys=[];
        for(let i=0;i<poly.length;i++){const a=poly[i],b=poly[(i+1)%poly.length];if((a[0]<=x&&b[0]>x)||(b[0]<=x&&a[0]>x))ys.push(a[1]+(x-a[0])*(b[1]-a[1])/(b[0]-a[0]));}
        return Math.max(...ys);
      }
''')
s=s.replace('rect(8.3,6.3,65.4,36.4)', 'rect(8.3,5.8,65.4,36.4)')
s=s.replace('rect(7.8,5.8,66.4,37.4)', 'rect(7.8,5.3,66.4,37.4)')
s=s.replace('rect(x-1.5,42.7,3,x<50?1.2:.8)', 'rect(x-1.5,x<50?42.7:42.1,3,x<50?1.2:.8)')
s=s.replace('rounded(8.5,6.5,65,36,1)', 'rounded(8.5,6,65,36,1)')
s=s.replace('Battery · moved forward 6 mm', 'Battery · moved forward 6.5 mm')
s=s.replace('The outer wheel area is recessed by 0.5 millimeters with smooth transitions, exposing 1.6 millimeters of the rim. The inner cavity is unchanged. Battery moved 6 millimeters toward the front.', 'The outer wheel area is recessed by 1 millimeter, exposing 2.1 millimeters of the rim. The relocated corner screw follows that edge. The lower wall is reinforced to 1.5 millimeters; the upper wall is 1.1 millimeters except for a 0.6 millimeter band beside the encoder and PCB. Battery moved 6.5 millimeters toward the front.')
s=s.replace('Wheel exposure · 1.6 mm', 'Wheel exposure · 2.1 mm')
s=s.replace('revision:13,', 'revision:14,')
s=s.replace('wheelRimExposureMm:1.6,wheelOutsideReliefMm:.5,wheelLocalWallMm:{upper:1.1,lower:1.5}', 'wheelRimExposureMm:2.1,wheelOutsideReliefMm:1,wheelLocalWallMm:{upperReinforced:1.1,encoderPcbBand:.6,lower:1.5},wheelThinBandZmm:[17.7,21.6],wheelApertureWidthMm:13,relocatedCaseScrewXYmm:[77,41]')
s=s.replace('batteryCaseXYmm:[8.5,6.5],batteryMoveFromRev10Mm:[0,-6]', 'batteryCaseXYmm:[8.5,6],batteryMoveFromRev10Mm:[0,-6.5]')
# Revision 15: local cable pocket beside the rear USB port; case screw stays put.
s=s.replace('      const upperLevels=[', '''      const cableReliefFootprint=[[75.8,36.8],[81,36.8],[81,38.6],[76,38.6],...arc(76,38.4,.2,90,180,12)];
      function cableReliefAt(z) {
        if(z<19.2||z>26.8)return [];
        const dz=z<19.7?19.7-z:z>26.3?z-26.3:0;
        const spread=Math.sqrt(Math.max(0,.25-dz*dz));
        return booleanPaths([cableReliefFootprint],[rect(75.8,37.3-spread,5.2,.8+2*spread)],'intersection');
      }
      const reliefLevels=[19.2,19.7,26.3,26.8,...Array.from({length:11},(_,i)=>19.7-.5*Math.cos((i+1)*Math.PI/24)),...Array.from({length:11},(_,i)=>26.3+.5*Math.sin((i+1)*Math.PI/24))];
      const upperLevels=[...reliefLevels,''')
s=s.replace(')),27];\n      function upperSection', ')),27].sort((a,b)=>a-b);\n      function upperSection')
s=s.replace('return booleanPaths([upperOutside],holes);', 'return booleanPaths([upperOutside],[...holes,...cableReliefAt(z)]);')
s=s.replace('revision:14,', 'revision:15,')
s=s.replace('relocatedCaseScrewXYmm:[77,41],', 'relocatedCaseScrewXYmm:[77,41],rearUsbCable:{measuredHousingMm:[10.25,6],assumedHousingFrontXmm:76,sideGapBeforeMm:.375,sideGapAfterMm:.975,localReliefMaxYmm:38.6,localReliefZmm:[19.2,26.8],fullDepthZmm:[19.7,26.3],screwPostUnchanged:true,fitVerified:false},')
s=s.replace('USB-C faces project 0.2 millimeters, with lower saddles belonging to the base.', 'USB-C faces project 0.2 millimeters, with lower saddles belonging to the base. A rounded local pocket beside the rear USB port adds 0.6 millimeters of cable clearance, retaining the complete corner screw post. The measured 10.25 by 6 millimeter plug housing has 0.975 millimeters of nominal side clearance, assuming its front is at X76.')
s=s.replace("state.view==='edge'?'Wheel exposure · 2.1 mm':'Matching upper + lower contour'", "state.view==='edge'?'Wheel exposure · 2.1 mm':state.view==='usb'?'Cable side clearance · 0.98 mm':'Matching upper + lower contour'")
# Revision 16: plug opening near J25 and a connected route to the lead corner.
s=s.replace("      const frameReliefs=bossCenters", "      const batteryCableCuts=[rounded(65.5,20.65,7,8,1),rounded(69.5,4,3,18,.75),rounded(69,4,9,7,1)];\n      const frameReliefs=bossCenters")
s=s.replace("...frameReliefs]);", "...frameReliefs,...batteryCableCuts]);")
s=s.replace('rect(69,6.1,3,1.4)', 'rect(65,6.1,3,1.4)').replace('rect(69,5,3,1.1)', 'rect(65,5,3,1.1)')
s=s.replace('revision:15,', 'revision:16,batteryCablePassage:{plugWidthMm:5,openingXYmm:[65.5,20.65],openingSizeMm:[7,8],cornerRadiusMm:1,wireChannelXYmm:[69.5,4],wireChannelSizeMm:[3,18],leadEntryXYmm:[69,4],leadEntrySizeMm:[9,7],leadCornerCaseXYmm:[73.5,6],connectorCaseXYmm:[71.375,24.65],physicalFitVerified:false},')
s=s.replace('A blind screw boss on the midframe supports', 'A 7 by 8 millimeter rounded plug opening near J25 joins a 3 millimeter wire channel to the front battery corner. A blind screw boss on the midframe supports')
s=s.replace("state.view==='frame'?'Two frame screws · blind PCB mount'", "state.view==='frame'?'Battery plug opening · 7 × 8 mm'")
s=s.replace("state.view==='frame'?'M2 × 8 mm · Ø3 × 3.2 mm inserts'", "state.view==='frame'?'Wire channel · 3 mm wide'")
# Revision 17: keyed lid registration and a larger LCD flex relief.
section('      const seatRearOpening=', '      const keyYs=', '''      const lcdFlex=rounded(18,5.9,24,6.35,.6);
      const seatRearOpening=ccw(largest(booleanPaths([rounded(15.24,9.45,29.52,31.72,.3),lcdFlex],[],'union')));
      const pocketFlexOpening=ccw(largest(booleanPaths([lcdPocket,lcdFlex],[],'union')));
''')
s=s.replace('[27.1,27.5,pocketFlexOpening,true],[27.5,28.5,lcdPocket,true]', '[27.1,28,pocketFlexOpening,true],[28,28.5,lcdPocket,true]')
s=s.replace('      // Front-loading seat.', '''      // Midframe tongues enter blind lid pockets without thinning the shell.
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
      // Front-loading seat.''')
s=s.replace('revision:16,', 'revision:17,lidAlignment:{owner:"midframe",keysXYWHmm:[[2.3,20,1.5,8],[40,2.3,8,1.5],[24,52.2,8,1.5]],engagementMm:2.5,sideClearanceMm:.2,topGapMm:.3,verticalRemoval:true,fitVerified:false},lcdFlexRelief:{sizeMm:[24,6.35],xyMm:[18,5.9],zMm:[25.8,28],radiusMm:.6},')
s=s.replace('Display is flush and three keys rise 1 millimeter.', 'Display is flush and three buttons rise 1 millimeter. Three chamfered midframe keys enter blind lid pockets with 2.5 millimeters of engagement and 0.2 millimeters of side clearance. The LCD flex relief is 24 by 6.35 millimeters, with an extra 0.5 millimeters of height.')
s=s.replace('<option value="frame">Midframe detail</option>', '<option value="frame">Midframe detail</option><option value="interlock">Lid alignment keys</option>')
s=s.replace('frame:[-.2,.75],seat:', 'frame:[-.2,.75],interlock:[-.38,.6],seat:')
s=s.replace("        if(state.view==='frame') Object.assign", "        if(state.view==='interlock') Object.assign(state,{explode:.12,clear:true,lid:true,pcb:false,display:false,buttons:false,hardware:false,midframe:true});\n        if(state.view==='frame') Object.assign")
s=s.replace("['angle','frame','posts'", "['angle','frame','interlock','posts'")
s=s.replace("['seat','keys','fastener'].includes(state.view)", "['seat','keys','fastener','interlock'].includes(state.view)")
s=s.replace("textContent=state.view==='keys'?", "textContent=state.view==='interlock'?'3 locating keys · 2.5 mm engagement':state.view==='keys'?",1)
s=s.replace("state.view==='seat'?'Pocket clearance · 0.2 mm per side'", "state.view==='interlock'?'Pocket clearance · 0.2 mm per side':state.view==='seat'?'Flex relief · 24 × 6.35 mm'")
# Revision 18: open cable bay and direct, chamfered access to the SW7 slider.
s=s.replace('const batteryCableCuts=[rounded(65.5,20.65,7,8,1),rounded(69.5,4,3,18,.75),rounded(69,4,9,7,1)];',
            'const batteryCableCuts=[rounded(64.5,-1,16.5,29.65,1)];')
s=s.replace('rect(65,6.1,3,1.4)', 'rect(60,6.1,3,1.4)').replace('rect(65,5,3,1.1)', 'rect(60,5,3,1.1)')
s=s.replace('const keyAnchorHoles=[14,36]', 'const keyAnchorHoles=[19,36]')
s=s.replace('for(const y of [14,36]) solid(circle(63,y,2.5)', 'for(const y of [19,36]) solid(circle(63,y,2.5)')
s=s.replace("solid(rect(60.5,12,5,26),25.8,26.6,'buttons',[circle(63,14,.9),circle(63,36,.9)]);",
            "solid(rect(60.5,16.5,5,21.5),25.8,26.6,'buttons',[circle(63,19,.9),circle(63,36,.9)]);")
s=s.replace('      // R2 quarter-circle face edge, native-offset slices matching the CAD source.', '''      const powerSwitchCenter=[63.0732,11.2];
      const powerSwitchBody=rect(powerSwitchCenter[0]-4.5,powerSwitchCenter[1]-1.75,9,3.5);
      // SW7 model is a reference envelope. Slider height remains a fit-check item.
      solid(powerSwitchBody,21.3,24.8,'powerSwitch');
      const powerLeverOffset=1;
      solid(rect(powerSwitchCenter[0]+powerLeverOffset-.75,powerSwitchCenter[1]-.75,1.5,1.5),24.8,26.8,'powerLever');
      function powerOpeningAt(z) {
        const d=clamp((z-28.2)/.8,0,1);
        return rounded(powerSwitchCenter[0]-6-d,powerSwitchCenter[1]-2.5-d,12+2*d,5+2*d,1+d);
      }
      function roofWithPowerOpening(rim,z0,z1,holes) {
        const otherPolys=booleanPaths([rim],holes),lower=powerOpeningAt(z0),upper=powerOpeningAt(z1);
        for(const poly of otherPolys)sides(poly,z0,z1,'lid');
        capPaths(booleanPaths(otherPolys,[lower]),z0,false,'lid');
        capPaths(booleanPaths(otherPolys,[upper]),z1,true,'lid');
        loftSides(lower,upper,z0,z1,'lid',true);
      }
      // R2 quarter-circle face edge, native-offset slices matching the CAD source.''')
s=s.replace('[28,28.5,lcdPocket,true]', '[28,28.2,lcdPocket,true],[28.2,28.5,lcdPocket,true]')
s=s.replace("solid(rim,low,high,'lid',[...lidHoles,...(anchors?keyAnchorHoles:[]),opening]);",
            "roofWithPowerOpening(rim,low,high,[...lidHoles,...(anchors?keyAnchorHoles:[]),opening]);")
s=s.replace('buttons:90,switch:65,', 'buttons:90,switch:65,powerSwitch:65,powerLever:65,')
s=s.replace('switch:mix(bg,fg,.55),', 'switch:mix(bg,fg,.55),powerSwitch:mix(bg,fg,.55),powerLever:color(\'--viz-series-5\'),')
s=s.replace("['pcb','wheel','wheelHub','usb','usbBore','switch']", "['pcb','wheel','wheelHub','usb','usbBore','switch','powerSwitch','powerLever']")
s=s.replace('<option value="interlock">Lid alignment keys</option>', '<option value="interlock">Lid alignment keys</option><option value="power">Power switch</option>')
s=s.replace('interlock:[-.38,.6],seat:', 'interlock:[-.38,.6],power:[-.4,1.1],seat:')
s=s.replace("        if(state.view==='interlock') Object.assign", "        if(state.view==='power') Object.assign(state,{explode:0,zoom:1.1,clear:false,lid:true,pcb:true,display:true,buttons:true,hardware:false,midframe:true});\n        if(state.view==='interlock') Object.assign")
s=s.replace("['angle','frame','interlock','posts'", "['angle','frame','interlock','power','posts'")
s=s.replace("['seat','keys','fastener','interlock'].includes(state.view)", "['seat','keys','fastener','interlock','power'].includes(state.view)")
s=s.replace("textContent=state.view==='interlock'?", "textContent=state.view==='power'?'SW7 access · 12 × 5 mm':state.view==='interlock'?",1)
s=s.replace("state.view==='keys'?'Switch rest gap · 0.3 mm'", "state.view==='power'?'Beveled mouth · 14 × 7 mm':state.view==='keys'?'Switch rest gap · 0.3 mm'")
s=s.replace('Battery plug opening · 7 × 8 mm', 'Open battery-cable bay')
s=s.replace('Wire channel · 3 mm wide', 'Thin edge strips removed')
s=s.replace('A 7 by 8 millimeter rounded plug opening near J25 joins a 3 millimeter wire channel to the front battery corner.',
            'A single open battery-cable bay near J25 removes the narrow edge strips and connects directly to the front and right edges of the midframe.')
s=s.replace('Display is flush and three buttons rise 1 millimeter.',
            'Display is flush and three buttons rise 1 millimeter. The SW7 power slider is accessible from above through a 12 by 5 millimeter opening with a beveled 14 by 7 millimeter mouth. The button-strip anchor is shifted clear of the switch. The slider envelope and finger access require physical verification.')
s=s.replace('revision:17,', 'revision:18,powerSwitch:{reference:"SW7",caseCenterXYmm:[63.0732,11.2],bodyXYmm:[9,3.5],bodyZmm:[21.3,24.8],sliderXYmm:[1.5,1.5],sliderNominalTopZmm:26.8,alternativeSliderTopZmm:27.3,travelMm:2,travelAxis:"X",shownTravelOffsetMm:1,openingMm:[12,5],openingRadiusMm:1,mouthMm:[14,7],mouthRadiusMm:2,bevelZmm:[28.2,29],physicalFitVerified:false},buttonStripAnchorsXYmm:[[63,19],[63,36]],')
s=s.replace('batteryCablePassage:{plugWidthMm:5,openingXYmm:[65.5,20.65],openingSizeMm:[7,8],cornerRadiusMm:1,wireChannelXYmm:[69.5,4],wireChannelSizeMm:[3,18],leadEntryXYmm:[69,4],leadEntrySizeMm:[9,7],leadCornerCaseXYmm:[73.5,6],connectorCaseXYmm:[71.375,24.65],physicalFitVerified:false}',
            'batteryCablePassage:{plugWidthMm:5,openBayXYmm:[64.5,-1],openBaySizeMm:[16.5,29.65],cornerRadiusMm:1,openEdges:["front","right"],thinEdgeStripsRemoved:true,frontSeatXRangeMm:[60,63],leadCornerCaseXYmm:[73.5,6],connectorCaseXYmm:[71.375,24.65],physicalFitVerified:false}')
# Revision 19: a captive cap drives the original switch beneath a small slot.
section('      function powerOpeningAt(', '      // R2 quarter-circle face edge', '''      const powerSlot=rounded(powerSwitchCenter[0]-4.5,powerSwitchCenter[1]-2.2,9,4.4,1);
      const powerGuide=rounded(powerSwitchCenter[0]-8.5,powerSwitchCenter[1]-4.1,17,8.2,1);
      const powerRetainerSlot=rounded(powerSwitchCenter[0]-3.5,powerSwitchCenter[1]-2.3,7,4.6,.3);
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
''')
s=s.replace('[27.1,28,pocketFlexOpening,true],[28,28.2,lcdPocket,true],[28.2,28.5,lcdPocket,true]', '[27.1,27.8,pocketFlexOpening,true],[27.8,28,pocketFlexOpening,true],[28,28.5,lcdPocket,true]')
s=s.replace('powerSwitch:65,powerLever:65,', 'powerSwitch:65,powerLever:65,powerCap:90,')
s=s.replace("powerLever:color('--viz-series-5'),", "powerLever:color('--viz-series-5'),powerCap:color('--viz-series-4'),")
s=s.replace("if(!state.buttons&&f.part==='buttons')", "if(!state.buttons&&['buttons','powerCap'].includes(f.part))")
s=s.replace("['buttons','switch'].includes(f.part)", "['buttons','switch','powerCap','powerSwitch','powerLever'].includes(f.part)")
s=s.replace('Show buttons</span>', 'Show buttons + slider</span>')
s=s.replace('Button strip</span>', 'Button strip</span><span><i class="swatch slider-swatch" aria-hidden="true"></i>Power slider</span>')
s=s.replace('    #p4x-unified-body .button-swatch', '    #p4x-unified-body .slider-swatch { background: var(--viz-series-4); }\n    #p4x-unified-body .button-swatch')
s=s.replace('value="edge" selected', 'value="edge"').replace('value="power">', 'value="power" selected>')
s=s.replace("yaw:2.82, pitch:.64, explode:0, zoom:1,", "yaw:-.4, pitch:1.1, explode:0, zoom:1.1,")
s=s.replace("view:'edge'", "view:'power'")
s=s.replace('SW7 access · 12 × 5 mm', 'Captive power slider · raised 1 mm')
s=s.replace('Beveled mouth · 14 × 7 mm', 'Existing strip screws retain cap')
s=s.replace('The SW7 power slider is accessible from above through a 12 by 5 millimeter opening with a beveled 14 by 7 millimeter mouth. The button-strip anchor is shifted clear of the switch. The slider envelope and finger access require physical verification.',
            'A separate captive power cap drives the original SW7 slider through a 9 by 4.4 millimeter running slot. A hidden flange overlaps the slot and slides inside a blind underside pocket. The enlarged button strip retains the cap using its two existing screws. The cap rises 1 millimeter above the case. The overlap is a dust cover, not a water seal; fit and motion need physical verification.')
s=s.replace('revision:18,', 'revision:19,')
s=s.replace('openingMm:[12,5],openingRadiusMm:1,mouthMm:[14,7],mouthRadiusMm:2,bevelZmm:[28.2,29],',
            'openingMm:[9,4.4],openingRadiusMm:1,guideMm:[17,8.2],guideTopZmm:27.8,captiveCap:{separatePrintedPart:true,flangeMm:[14,7.6,.8],flangeZmm:[26.8,27.6],nubMm:[6,3.8],topZmm:30,socketMm:[2,2],socketZmm:[25.4,27.9],socketLeadInMm:2.4,retainerOwnedByButtonStrip:true,retainerMm:[18,9,1.2],retainerZmm:[25.4,26.6],retainerSlotMm:[7,4.6],capTravelMm:3,nominalSocketBacklashMm:.5,nominalSidePlayMm:.3,nominalVerticalPlayMm:.4,minimumProjectedOverlapMm:1,waterproof:false},')
# Revision 20: stop upward PCB travel and relocate its underside USB-side seat.
s=s.replace('for(const x of [10,72.5]) {', 'for(const x of [10]) {')
s=s.replace("      solid(rect(60,6.1,3,1.4),14.5,19.7,'midframe');", "      const usbBoardSeat=rect(69.3,32.75,1.2,2);\n      solid(usbBoardSeat,14.49,19.7,'usbBoardSeat');\n      solid(rect(60,6.1,3,1.4),14.5,19.7,'midframe');")
s=s.replace('      // Front-loading seat. The central opening', '''      const pcbLiftStopCenter=[72.5,42.3];
      const pcbLiftStopFoot=rounded(71,41.7,3,1.2,.2),pcbLiftStopRoot=rounded(70.5,41.1,4,2.4,.4);
      sides(pcbLiftStopFoot,21.5,21.9,'pcbStop');cap(pcbLiftStopFoot,21.5,false,'pcbStop');
      loftSides(pcbLiftStopFoot,pcbLiftStopRoot,21.9,23.49,'pcbStop');
      sides(pcbLiftStopRoot,23.49,27.1,'pcbStop');cap(pcbLiftStopRoot,27.1,true,'pcbStop');
      // Front-loading seat. The central opening''')
s=s.replace('      // View-only separation. At zero the positions match enclosure.scad.', '''      // Cropped portions of the actual components for the travel-stop detail.
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
      // View-only separation. At zero the positions match enclosure.scad.''')
s=s.replace('midframe:42,midBore:42,', 'midframe:42,usbBoardSeat:42,pcbStop:90,detailPCB:65,detailFrame:42,detailWheel:65,lowerSwitch:65,midBore:42,')
s=s.replace('frameInsert:palette.insert,', 'usbBoardSeat:palette.midframe,pcbStop:palette.lid,detailPCB:palette.pcb,detailFrame:palette.midframe,detailWheel:palette.wheel,lowerSwitch:palette.switch,frameInsert:palette.insert,')
s=s.replace("if((state.view==='fastener')!==f.part.startsWith('cut'))return false;", "if((state.view==='fastener')!==f.part.startsWith('cut'))return false;\n          if(state.view!=='stop'&&f.part.startsWith('detail'))return false;\n          if(state.view==='stop'&&!['pcbStop','usbBoardSeat','detailPCB','detailFrame','detailWheel','lowerSwitch'].includes(f.part))return false;")
s=s.replace("['lid','lidBore','lcd','screen','insert','cutLid','cutInsert']", "['lid','lidBore','lcd','screen','insert','cutLid','cutInsert','pcbStop']")
s=s.replace("'powerSwitch','powerLever']", "'powerSwitch','powerLever','detailPCB','detailWheel','lowerSwitch']")
s=s.replace("['midframe','midBore','frameScrew','frameScrewSlot']", "['midframe','usbBoardSeat','detailFrame','midBore','frameScrew','frameScrewSlot']")
s=s.replace('value="power" selected', 'value="power"')
s=s.replace('<option value="power">Power switch</option>', '<option value="power">Power switch</option><option value="stop" selected>PCB travel stop</option>')
s=s.replace('power:[-.4,1.1],seat:', 'power:[-.4,1.1],stop:[2.7,.28],seat:')
s=s.replace("        if(state.view==='power') Object.assign", "        if(state.view==='stop') Object.assign(state,{explode:0,zoom:1,clear:false,lid:true,pcb:true,display:false,buttons:false,hardware:false,midframe:true});\n        if(state.view==='power') Object.assign")
s=s.replace("['angle','frame','interlock','power','posts'", "['angle','frame','interlock','power','stop','posts'")
s=s.replace("['seat','keys','fastener','interlock','power'].includes(state.view)", "['seat','keys','fastener','interlock','power','stop'].includes(state.view)")
s=s.replace("textContent=state.view==='power'?", "textContent=state.view==='stop'?'PCB travel stop · 0.2 mm nominal gap':state.view==='power'?",1)
s=s.replace("textContent=state.view==='power'?'Existing strip screws retain cap'", "textContent=state.view==='stop'?'Case cutaway · lower support relocated':state.view==='power'?'Existing strip screws retain cap'")
s=s.replace("yaw:-.4, pitch:1.1, explode:0, zoom:1.1,", "yaw:2.7, pitch:.28, explode:0, zoom:1,")
s=s.replace("view:'power'", "view:'stop'")
s=s.replace('The overlap is a dust cover, not a water seal; fit and motion need physical verification.', 'The overlap is a dust cover, not a water seal; fit and motion need physical verification. A lid-owned travel stop has a nominal 0.2 millimeter gap above a clear area of the display-facing PCB. The lower USB-side board support has moved clear of the RESET switch and terminal pads. The PCB travel-stop view is a cropped case cutaway of the same components.')
s=s.replace('revision:19,', 'revision:20,pcbTravelStop:{owner:"lid",caseCenterXYmm:[72.5,42.3],footMm:[3,1.2],footRadiusMm:.2,rootMm:[4,2.4],rootRadiusMm:.4,footZmm:[21.5,21.9],rootStartZmm:23.49,topZmm:27.1,nominalBoardGapMm:.2,contactLand:"component-free display-side PCB; no drill centers in reviewed land",clamp:false,physicalFitVerified:false},usbSideBoardSeat:{owner:"midframe",xyMm:[69.3,32.75],sizeMm:[1.2,2],zMm:[14.49,19.7],areaMm2:2.4,previousSeatRemoved:true,contactLandContainsTentedVias:true,nominalComponentClearancesMm:{bootBody:.275,bootMaskPad:.425,encoderMaskPad:.35,usbHoleMask:.29,wheel:.6316},physicalFitVerified:false},')
# Revision 21: the fixed button bar is captured by a separate sliding keeper.
s=s.replace('powerSwitchCenter[0]-3.5,powerSwitchCenter[1]-2.3,7,4.6,.3', 'powerSwitchCenter[0]-3.9,powerSwitchCenter[1]-2.8,7.8,5.6,.3')
s=s.replace('retainerSlotMm:[7,4.6]', 'retainerSlotMm:[7.8,5.6]')
s=s.replace('const keyAnchorHoles=[19,36].map(y=>circle(63,y,.6));', 'const keyAnchorHoles=[];')
s=s.replace("for(const y of [19,36]) solid(circle(63,y,2.5),26.6,27,'lid',[circle(63,y,.6)]);", "for(const y of [19,36]) solid(circle(63,y,2.5),26.6,28.99,'anchorPads');")
s=s.replace("solid(rect(60.5,16.5,5,21.5),25.8,26.6,'buttons',[circle(63,19,.9),circle(63,36,.9)]);", "solid(rect(60.5,16.5,5,21.5),25.8,26.6,'buttons');")
s=s.replace('      // Replaceable actuator strip;', '''      const keeperMain=rect(60.7,17.5,4.6,20.5);
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
      // Replaceable actuator strip;''')
s=s.replace('midframe:42,', 'keeper:82,keeperRails:90,keeperRoof:90,anchorPads:90,midframe:42,',1)
s=s.replace('usbBoardSeat:palette.midframe,', "keeper:color('--viz-series-6'),keeperRails:palette.lid,keeperRoof:palette.lid,anchorPads:palette.lid,usbBoardSeat:palette.midframe,")
s=s.replace("['lid','lidBore','lcd','screen','insert','cutLid','cutInsert','pcbStop']", "['lid','lidBore','lcd','screen','insert','cutLid','cutInsert','pcbStop','keeperRails','keeperRoof','anchorPads']")
s=s.replace("if(!state.buttons&&['buttons','powerCap'].includes(f.part))", "if(!state.buttons&&['buttons','powerCap','keeper'].includes(f.part))")
s=s.replace("if(state.view!=='stop'&&f.part.startsWith('detail'))return false;", "if(state.view!=='stop'&&f.part.startsWith('detail'))return false;\n          if(state.view!=='keeper'&&f.part==='keeperRoof')return false;\n          if(state.view==='keeper'&&!['buttons','keeper','keeperRails','anchorPads','keeperRoof'].includes(f.part))return false;")
s=s.replace("['buttons','switch','powerCap','powerSwitch','powerLever','detailPCB','detailWheel','lowerSwitch']", "['buttons','keeper','switch','powerCap','powerSwitch','powerLever','detailPCB','detailWheel','lowerSwitch']")
s=s.replace('value="stop" selected', 'value="stop"')
s=s.replace('<option value="stop">PCB travel stop</option>', '<option value="stop">PCB travel stop</option><option value="keeper" selected>Button keeper</option>')
s=s.replace('stop:[2.7,.28],seat:', 'stop:[2.7,.28],keeper:[-.45,-.75],seat:')
s=s.replace("        if(state.view==='stop') Object.assign", "        if(state.view==='keeper') Object.assign(state,{explode:.85,zoom:1,clear:false,lid:true,pcb:false,display:false,buttons:true,hardware:false,midframe:false});\n        if(state.view==='stop') Object.assign")
s=s.replace("['angle','frame','interlock','power','stop','posts'", "['angle','frame','interlock','power','stop','keeper','posts'")
s=s.replace("['seat','keys','fastener','interlock','power','stop'].includes(state.view)", "['seat','keys','fastener','interlock','power','stop','keeper'].includes(state.view)")
s=s.replace("let state = {yaw:2.7, pitch:.28, explode:0, zoom:1, clear:false, lid:true, pcb:true, display:true, buttons:true, hardware:true, midframe:true, view:'stop'};", "let state = {yaw:-.45, pitch:-.75, explode:.85, zoom:1, clear:false, lid:true, pcb:false, display:false, buttons:true, hardware:false, midframe:false, view:'keeper'};")
s=s.replace('const x=p[0]-40,y=p[1]-28,z=p[2]+lift[part]*state.explode;', '''const keeperDetail=state.view==='keeper';
        const keeperDy=keeperDetail&&part==='keeper'?3.3*Math.min(1,state.explode*2):0;
        const keeperDz=keeperDetail&&part==='keeper'?-10*Math.max(0,state.explode-.5):0;
        const x=p[0]-40,y=p[1]-28+keeperDy,z=p[2]+(keeperDetail?keeperDz:lift[part]*state.explode);''')
s=s.replace("textContent=state.view==='stop'?'PCB travel stop", "textContent=state.view==='keeper'?(state.explode>.5?'Load vertically at +3.3 mm Y':state.explode>0?'Slide toward −Y to seat':'Button keeper · seated under four ledges'):state.view==='stop'?'PCB travel stop",1)
s=s.replace("textContent=state.view==='stop'?'Case cutaway", "textContent=state.view==='keeper'?'0.15 mm strip float · silicone anti-backout required':state.view==='stop'?'Case cutaway",1)
s=s.replace('Existing strip screws retain cap', 'Keeper-secured strip retains cap')
s=s.replace('The enlarged button strip retains the cap using its two existing screws.', 'The button strip retains the cap and is itself captured by a separate sliding keeper beneath four lid ledges. The two old screw holes and pilot holes are removed. The keeper loads from below at 3.3 millimeters toward the rear, then slides forward into its seat. A removable resin-compatible silicone bead across the rear tab and ledge seam is required to prevent reverse sliding. This is adhesive anti-backout, not a snap lock. Nominal strip float is 0.15 millimeters.')
s=s.replace('Button strip</span><span>', 'Button strip</span><span><i class="swatch keeper-swatch" aria-hidden="true"></i>Keeper</span><span>')
s=s.replace('    #p4x-unified-body .slider-swatch', '    #p4x-unified-body .keeper-swatch { background: var(--viz-series-6); }\n    #p4x-unified-body .slider-swatch')
s=s.replace('Show buttons + slider</span>', 'Show buttons + keeper + slider</span>')
s=s.replace('revision:20,', 'revision:21,buttonKeeper:{separatePrintedPart:true,ledgeCount:4,zMm:[24.65,25.65],nominalStripFloatMm:.15,insertionYOffsetMm:3.3,powerShelfGuidesXYWHmm:[[52.8732,10,1,2],[72.2732,10,1,2]],retainerOpeningMm:[7.8,5.6],conservativeNominalTranslationBoundMm:.21,conservativeNominalYawBoundDegrees:1.2,assembly:"load from below at +3.3 Y, then slide -3.3 Y",requiredRetention:"removable resin-compatible silicone bead across rear tab/ledge seam",positiveMechanicalLock:false,physicalFitVerified:false},')
s=s.replace('nominalVerticalPlayMm:.4,', 'nominalVerticalPlayMm:.4,worstCaseVerticalPlayIncludingStripMm:.55,')
# Keep the insertion path label adjacent to the existing separation control.
s=s.replace('<span>Explode</span>', '<span id="p4x-unified-body-separation-label">Explode</span>')
s=s.replace("        control.clear.checked=state.clear;", "        root.querySelector('#p4x-unified-body-separation-label').textContent=state.view==='keeper'?'Insert / remove':'Explode';\n        control.explode.setAttribute('aria-label',state.view==='keeper'?'Keeper insertion path; zero is seated, 50 percent is shifted 3.3 millimeters toward rear, 100 percent is lowered for loading':'Explode the enclosure layers; zero is assembled');\n        control.clear.checked=state.clear;")
# Old saved views should not hide the revised attachment on first review.
s=s.replace("if(s&&typeof s==='object') {", "if(s&&typeof s==='object'&&snapshot?.modelContent?.revision===21) {")
library=Path('work/p4x-enclosure/clipper-6.4.2.js').read_text()
licenses='\n'.join(Path('work/p4x-enclosure/'+name).read_text() for name in ['clipper-boost-license.txt','clipper-jsbn-license.txt'])
library='\n'.join('// '+line for line in licenses.splitlines())+'\n'+library
assert '</script' not in library.lower()
s=s.replace('  <script>','  <script>\n'+library+'\n  </script>\n  <script>',1)
p.write_text(s)
print(p, p.stat().st_size)
