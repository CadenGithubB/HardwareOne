"""Revision-matched PCBWay insert-location drawing; no CAD changes."""
from pathlib import Path
import sys, json, hashlib, math

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'work/p4x-enclosure/mesh-tools'))
import numpy as np
import trimesh
from PIL import Image, ImageDraw
from reportlab.pdfgen import canvas
from reportlab.lib.units import mm
from reportlab.lib.colors import HexColor, Color, white
from reportlab.pdfbase.pdfmetrics import stringWidth
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont

pdfmetrics.registerFont(TTFont('DrawFont', '/System/Library/Fonts/Supplemental/Arial.ttf'))
pdfmetrics.registerFont(TTFont('DrawFont-Bold', '/System/Library/Fonts/Supplemental/Arial Bold.ttf'))

PARTS = ROOT / 'mechanical/p4x-eye-enclosure'
WORK = ROOT / 'work/p4x-enclosure/drawing-r21'
WORK.mkdir(exist_ok=True)
OUT = PARTS / 'p4x-eye-pcbway-technical-drawing-r21.pdf'
M = json.loads((PARTS / 'stl/export-manifest.json').read_text())
assert M['revision'] == 21
assert hashlib.sha256((PARTS / 'enclosure.scad').read_bytes()).hexdigest() == M['source_sha256']
P = {p['part']:p for p in M['parts']}
BLACK = HexColor('#202832')
GRAY = HexColor('#626c77')
LIGHT = HexColor('#dce2e8')
BLUE = HexColor('#174e75')
PALE = HexColor('#edf3f7')

def render_top(name):
    p = P[name]
    src = PARTS / 'stl' / p['file']
    assert hashlib.sha256(src.read_bytes()).hexdigest() == p['sha256']
    mesh = trimesh.load_mesh(src, process=False)
    vertices = np.asarray(mesh.vertices)
    tri = vertices[mesh.faces]
    w,h,z = p['size_mm']
    ppm = 22
    im = Image.new('RGB', (round(w*ppm)+1,round(h*ppm)+1), 'white')
    d = ImageDraw.Draw(im)
    # Orthographic +Z view of the actual exported mesh. Height shading reveals
    # recessed bores without inventing hidden geometry or an assembly view.
    normal = np.cross(tri[:,1]-tri[:,0],tri[:,2]-tri[:,0])
    area = normal[:,2]
    zmean = tri[:,:,2].mean(axis=1)
    for idx in np.argsort(zmean):
        if area[idx] <= 1e-10:
            continue
        t = tri[idx]
        level = 195 if name == 'button_keeper' else int(180+60*zmean[idx]/z)
        d.polygon([(float(v[0]*ppm),float((h-v[1])*ppm)) for v in t],
                  fill=(level,level+2,min(255,level+4)))
    path = WORK / (name+'-top.png')
    im.save(path)
    return path

IMAGES = {name:render_top(name) for name in ('lid','base','button_strip','button_keeper')}
c = canvas.Canvas(str(OUT), pagesize=(297*mm,210*mm), pageCompression=1)
c.setTitle('ESP32-P4X-EYE enclosure R21 - PCBWay technical drawing')
c.setAuthor('HardwareOne enclosure project')
c.setSubject('Existing STL geometry and six requested bonded M2 insert locations; quotation review')

def line(x1,y1,x2,y2,color=BLACK,width=.22):
    c.setStrokeColor(color); c.setLineWidth(width*mm)
    c.line(x1*mm,y1*mm,x2*mm,y2*mm)

def text(x,y,s,size=8.5,bold=False,color=BLACK,align='left'):
    c.setFillColor(color); c.setFont('DrawFont-Bold' if bold else 'DrawFont',size)
    fn = {'left':c.drawString,'right':c.drawRightString,'center':c.drawCentredString}[align]
    fn(x*mm,y*mm,s)

def para(x,y,s,width,size=8.5,leading=4.1,color=BLACK):
    words=s.split(); rows=[]; row=''
    for word in words:
        candidate=(row+' '+word).strip()
        if stringWidth(candidate,'DrawFont',size) > width*mm and row:
            rows.append(row); row=word
        else: row=candidate
    if row: rows.append(row)
    for r in rows:
        text(x,y,r,size,color=color); y-=leading
    return y

def arrow(x1,y1,x2,y2,color=BLACK,head=1.7):
    line(x1,y1,x2,y2,color)
    a=math.atan2(y2-y1,x2-x1)
    for delta in [-.42,.42]:
        line(x2,y2,x2-head*math.cos(a+delta),y2-head*math.sin(a+delta),color)

def dim_h(x1,x2,y,edge,label):
    line(x1,edge,x1,y-1.4,GRAY,.13); line(x2,edge,x2,y-1.4,GRAY,.13)
    arrow(x1,y,x2,y); arrow(x2,y,x1,y)
    tw=stringWidth(label,'DrawFont',8)*1/mm
    c.setFillColor(white); c.rect(((x1+x2)/2-tw/2-1)*mm,(y-1)*mm,(tw+2)*mm,4*mm,fill=1,stroke=0)
    text((x1+x2)/2,y-.1,label,8,align='center')

def dim_v(y1,y2,x,edge,label):
    line(edge,y1,x-1.4,y1,GRAY,.13); line(edge,y2,x-1.4,y2,GRAY,.13)
    arrow(x,y1,x,y2); arrow(x,y2,x,y1)
    c.saveState(); c.translate((x-1.5)*mm,((y1+y2)/2)*mm); c.rotate(90)
    c.setFillColor(white); tw=stringWidth(label,'DrawFont',8)
    c.rect(-tw/2-2,-2,tw+4,10,stroke=0,fill=1)
    c.setFont('DrawFont',8); c.setFillColor(BLACK); c.drawCentredString(0,0,label)
    c.restoreState()

def tag(x,y,label,tx,ty):
    r=2.4
    c.setStrokeColor(BLUE); c.setLineWidth(.22*mm)
    c.circle(x*mm,y*mm,r*mm,stroke=1,fill=0)
    arrow(tx,ty,x,y,BLUE,1.3)
    text(tx,ty+1.2,label,9,True,BLUE,align='center')

def table(x,y,width,headers,rows,col_widths=None,rowh=6.5,size=8.3):
    cols=col_widths or [width/len(headers)]*len(headers)
    c.setFillColor(PALE); c.rect(x*mm,(y-rowh)*mm,width*mm,rowh*mm,fill=1,stroke=0)
    for ri,row in enumerate([headers]+rows):
        xx=x
        for j,val in enumerate(row):
            text(xx+2,y-rowh*(ri+1)+2.1,str(val),size,ri==0)
            xx+=cols[j]
        line(x,y-rowh*(ri+1),x+width,y-rowh*(ri+1),LIGHT,.13)
    return y-rowh*(len(rows)+1)

def page(title,num,files):
    c.setFillColor(white); c.rect(0,0,297*mm,210*mm,fill=1,stroke=0)
    c.setStrokeColor(BLACK); c.setLineWidth(.22*mm); c.rect(8*mm,8*mm,281*mm,194*mm,fill=0,stroke=1)
    text(13,193,title,14,True)
    text(13,186,'FOR QUOTATION / INSERT FEASIBILITY REVIEW - NOT RELEASED FOR PRODUCTION',8.5,True,BLUE)
    text(13,180,'Existing R21 mesh geometry. Confirm resin-compatible M2 insert and pocket changes before manufacture.',8,False,GRAY)
    line(8,175,289,175)
    line(8,27,289,27)
    text(13,21,'P4XE-R21-INS',10,True)
    text(13,14,'Units: mm   |   Dimensions nominal; STL governs   |   Do not scale drawing',7.5)
    text(185,21,'R21  /  DOC 2026-09-29',8,True)
    text(283,21,f'SHEET {num} / 3',8,True,align='right')
    text(283,14,'Material / finish: per selected PCBWay order',7.5,align='right')
    for i,f in enumerate(files):
        text(13,168-5*i,'FILE: '+P[f]['file'],8.5,True)

def top_view(name,x,y,s):
    w,h,z=P[name]['size_mm']
    c.drawImage(str(IMAGES[name]),x*mm,y*mm,w*s*mm,h*s*mm,mask='auto')
    dim_h(x,x+w*s,y-8,y,f'{w:.2f}')
    dim_v(y,y+h*s,x-8,x,f'{h:.2f}')
    # Origin belongs to the exported STL bounding box, not a hypothetical sharp corner.
    arrow(x,y,x+9,y,BLUE); arrow(x,y,x,y+9,BLUE)
    text(x+10,y-1,'+X',7,False,BLUE); text(x-1,y+10,'+Y',7,False,BLUE)
    text(x-2,y-3.8,'O',7,True,BLUE)
    return lambda X,Y:(x+X*s,y+Y*s)

def pocket_detail(x,y,total,label):
    # Cavity section, depths measured from entry surface. Supporting outer
    # profile deliberately broken/schematic; no false boss-wall dimensions.
    text(x-7,y+7,label,9,True)
    scale=4.4; w=8; h=total+1.2
    c.setFillColor(HexColor('#e4e8ec')); c.setStrokeColor(GRAY)
    c.rect((x-w/2*scale)*mm,(y-h*scale)*mm,w*scale*mm,h*scale*mm,fill=1,stroke=1)
    pts=[(-1.55,0),(1.55,0),(1.35,.25),(1.35,3.4),(1.2,3.4),(1.2,total),(-1.2,total),(-1.2,3.4),(-1.35,3.4),(-1.35,.25)]
    p=c.beginPath(); p.moveTo((x+pts[0][0]*scale)*mm,(y-pts[0][1]*scale)*mm)
    for xx,yy in pts[1:]:p.lineTo((x+xx*scale)*mm,(y-yy*scale)*mm)
    p.close(); c.setFillColor(white); c.setStrokeColor(BLACK); c.drawPath(p,fill=1,stroke=1)
    c.setDash(3,2); line(x,y+3,x,y-total*scale-3,GRAY,.13); c.setDash()
    arrow(x,y+5,x,y+.5,BLUE); text(x+5,y+2.5,'Insert enters here',7.3,False,BLUE)
    dim_v(y-3.4*scale,y,x+24,x+w/2*scale,'3.40')
    dim_v(y-total*scale,y,x+34,x+w/2*scale,f'{total:.2f}')
    text(x-w/2*scale,y-h*scale-5,'Cavity section only - schematic',7.4,False,GRAY)

def note_block(x,y,title,items,width):
    text(x,y,title,9.5,True); y-=6
    for n,item in enumerate(items,1):
        text(x,y,str(n)+'.',8)
        y=para(x+5,y,item,width-5,8,3.75)-2
    return y

# Sheet 1: lid. The exported lid is flipped from assembly and faces upward here.
page('Upper lid - four requested M2 inserts',1,['lid'])
text(25,158,'INTERIOR VIEW  |  Looking down -Z of exported STL  |  Scale 1.55:1',7.7,False,GRAY)
xy=top_view('lid',27,63,1.55)
for label,X,Y,tx,ty in [('L1',3.6,52.4,25,153),('L2',76.4,52.4,150,153),('L3',3.6,3.6,44,73),('L4',77,15,157,80)]:
    tag(*xy(X,Y),label,tx,ty)
tag(*xy(72.5,13.7),'S1',121,96)
text(27,44,'Envelope: 80.00 x 56.00 x 14.50',8,True)
text(27,38,'Origin O: exported STL X=0, Y=0. Not assembly coordinates.',7.6)
text(167,169,'S1 tip: X72.50 / Y13.70 / Z7.50 (exported)',7.5,False,BLUE)
text(167,165,'3 x 1.2 contact; nominal 0.20 PCB gap in assembly.',7.5,False,BLUE)
text(167,160,'4x M2 x 0.4 BONDED INSERTS REQUESTED',9.2,True,BLUE)
table(167,155,115,['ID','X','Y','Entry Z'],[['L1','3.60','52.40','14.50'],['L2','76.40','52.40','14.50'],['L3','3.60','3.60','14.50'],['L4','77.00','15.00','14.50']], [19,28,28,40])
text(167,117,'Installation: from interior, toward -Z in this STL.',8)
text(167,112,'Target: flush with each entry face; no exterior holes.',8)
pocket_detail(188,96,8,'A - EXISTING PILOT, 4 PLACES')
text(238,93,'Diameter callouts',8,True)
para(238,87,'Pilot: DIA 2.70 to 3.40 deep. Then DIA 2.40 to 8.00 total depth.',44,7.7,3.65)
para(238,66,'Lead-in: nominal DIA 3.10 x 0.25 deep. Geometry per STL.',44,7.7,3.65)
text(167,40,'Button retention uses internal ledges; no extra inserts.',7.6,True)
text(13,31,'STL SHA-256: '+P['lid']['sha256'],6.8,False,GRAY)
c.showPage()

# Sheet 2: base, still in its assembled orientation.
page('Lower case - two requested M2 inserts',2,['base'])
text(25,158,'INTERIOR VIEW  |  Looking down -Z of exported STL  |  Scale 1.55:1',7.7,False,GRAY)
xy=top_view('base',27,63,1.55)
tag(*xy(5,8.7),'B1',21,86)
tag(*xy(44,48.5),'B2',97,153)
text(27,44,'Envelope: 80.00 x 56.00 x 22.83',8,True)
text(27,38,'Origin O: exported STL X=0, Y=0; same XY as assembly.',7.6)
text(167,160,'2x M2 x 0.4 BONDED INSERTS REQUESTED',9.2,True,BLUE)
table(167,155,115,['ID','X','Y','Entry Z'],[['B1','5.00','8.70','13.00'],['B2','44.00','48.50','13.00']], [19,28,28,40])
text(167,130,'Installation: from open case, toward -Z in this STL.',8)
text(167,125,'Flush post tops. B1 boss DIA 5.50; B2 boss DIA 6.00.',8)
pocket_detail(188,108,7,'B - EXISTING PILOT, 2 PLACES')
text(238,105,'Diameter callouts',8,True)
para(238,99,'Pilot: DIA 2.70 to 3.40 deep. Then DIA 2.40 to 7.00 total depth.',44,7.7,3.65)
para(238,78,'Lead-in: nominal DIA 3.10 x 0.25 deep. Geometry per STL.',44,7.7,3.65)
para(167,57,'The four other case fastener locations are clearance holes, not insert locations: DIA 2.30 through to interior with DIA 4.50 x 10.20 deep head wells from the underside.',115,8,3.8)
text(167,38,'Preserve the closed bottom and enclosed screw-well walls.',7.6,True)
text(13,31,'STL SHA-256: '+P['base']['sha256'],6.8,False,GRAY)
c.showPage()

# Sheet 3: screwless button assembly; no standalone midframe/slider sections.
page('Button strip, keeper and supplier requirements',3,['button_strip','button_keeper'])
text(24,153,'BUTTON STRIP',8,True)
text(24,148,'Scale 1.4:1',7.5,False,GRAY)
top_view('button_strip',29,101,1.4)
text(82,153,'KEEPER',8,True)
text(82,148,'Scale 2:1',7.5,False,GRAY)
top_view('button_keeper',88,101,2)
text(24,86,'Strip: 22.77 x 31.30 x 5.90; solid mounting bar.',7.4)
text(24,81,'Keeper: 8.50 x 20.50 x 1.00; print flat.',7.4)
note_block(19,73,'SCREWLESS RETENTION',[
    'Seat the strip vertically against the lid pads. Hold it there while installing the keeper.',
    'Insert keeper 3.3 mm toward +Y from its seated position, then slide -Y to the stop.',
    'Required: removable resin-compatible silicone bead bridging a rear keeper tab and its fixed ledge. Cure before use. Keep arms and moving controls free.'
],112)
note_block(140,166,'SUPPLIER REVIEW / ORDER NOTES',[
    'Quantity: one each of six parts. Six inserts total, only at L1-L4 and B1-B2.',
    'Request M2 x 0.4 brass inserts bonded with resin-compatible adhesive. No thermal insertion into cured resin.',
    'Existing pilots are for heat-setting, not approved glue pockets. Previous hardware envelope DIA 3.00 x 3.20 long is reference only and does not fit DIA 2.70 pilots as a bonded joint.',
    'Before printing, confirm insert part number, OD, length, adhesive and required hole / boss changes. Supply revised matching CAD/drawing for approval; do not silently enlarge these pockets.',
    'M2 x 8 screws measured under head. Base-post screw tip has only 0.50 nominal clearance with existing stack. Keep threads and tip space free of adhesive.',
    'Confirm thin-feature strength and finished fits: wheel wall 0.60; screw-well rim 0.75; button arms 0.80; receiver web 0.80; key fit 0.20 per side; button radial gap 0.40.',
    'Selected online material and finish, and uploaded 3D files, take priority. Select the intended clear resin / clear finish in the order; this drawing does not override those selections.',
    'Dimensions are nominal CAD references; no tighter manufacturing tolerance is imposed. Review dimensional changes from finishing. Physical fit is not verified.',
    'R21: button keeper replaces the two strip screws. Its ledges carry loads; silicone prevents sliding back out. Strip float 0.15; settled button gap 0.15. No strength or physical fit validation.'
 ],143)
text(13,31,'SHA-256 PREFIX  Strip: '+P['button_strip']['sha256'][:24]+'   Keeper: '+P['button_keeper']['sha256'][:24],6.8,False,GRAY)
c.save()
print(OUT)
