"""Six supplier drawings of the existing, hash-verified R21 STL geometry."""
from pathlib import Path
import ast, json, hashlib, math, sys

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
from pypdf import PdfReader

PARTS = ROOT / 'mechanical/p4x-eye-enclosure'
WORK = ROOT / 'work/p4x-enclosure/part-drawings-r21'
OUT = PARTS / 'pcbway-upload-r21/drawings'
WORK.mkdir(parents=True, exist_ok=True)
OUT.mkdir(parents=True, exist_ok=True)
M = json.loads((PARTS / 'stl/export-manifest.json').read_text())
P = {p['part']: p for p in M['parts']}
assert M['revision'] == 21
assert hashlib.sha256((PARTS/'enclosure.scad').read_bytes()).hexdigest() == M['source_sha256']
pdfmetrics.registerFont(TTFont('DrawFont', '/System/Library/Fonts/Supplemental/Arial.ttf'))
pdfmetrics.registerFont(TTFont('DrawFont-Bold', '/System/Library/Fonts/Supplemental/Arial Bold.ttf'))
BLACK=HexColor('#202832'); GRAY=HexColor('#626c77'); LIGHT=HexColor('#dce2e8')
BLUE=HexColor('#174e75'); PALE=HexColor('#edf3f7')

# Reuse only drawing primitives; do not execute the combined-PDF builder.
src = ast.parse((ROOT/'work/p4x-enclosure/make_pcbway_drawing.py').read_text())
helpers = ast.Module(body=[n for n in src.body if isinstance(n, ast.FunctionDef) and n.name != 'page'], type_ignores=[])
exec(compile(helpers, '<drawing primitives>', 'exec'))
IMAGES={name:render_top(name) for name in P}

def render_front(name):
    p=P[name]; mesh=trimesh.load_mesh(PARTS/'stl'/p['file'],process=False)
    tri=np.asarray(mesh.vertices)[mesh.faces]; w,h,z=p['size_mm']; ppm=24
    im=Image.new('RGB',(round(w*ppm)+1,round(z*ppm)+1),'white'); draw=ImageDraw.Draw(im)
    # Orthographic view from -Y; X horizontal and Z vertical.
    normals=np.cross(tri[:,1]-tri[:,0],tri[:,2]-tri[:,0])
    for i in np.argsort(tri[:,:,1].mean(axis=1))[::-1]:
        if normals[i,1] >= -1e-10: continue
        level=int(190+45*tri[i,:,1].mean()/h)
        draw.polygon([(float(v[0]*ppm),float((z-v[2])*ppm)) for v in tri[i]], fill=(level,level+2,min(255,level+4)))
    path=WORK/(name+'-front.png'); im.save(path); return path

FRONTS={name:render_front(name) for name in P}
titles={'base':'Lower case','lid':'Upper lid','midframe':'Midframe','button_strip':'Button strip','button_keeper':'Button keeper','power_slider':'Power slider'}

def header(name):
    p=P[name]
    c.setFillColor(white); c.rect(0,0,297*mm,210*mm,fill=1,stroke=0)
    c.setStrokeColor(BLACK); c.setLineWidth(.22*mm); c.rect(8*mm,8*mm,281*mm,194*mm,fill=0,stroke=1)
    text(13,193,titles[name]+' | R21 technical drawing',14,True)
    text(13,185,'FILE: '+p['file'],9,True)
    text(13,178,'Quantity: 1   |   Units: mm   |   Existing STL geometry; dimensions nominal',8,color=GRAY)
    line(8,173,289,173)
    line(8,27,289,27)
    text(13,21,'P4XE-R21-'+name.upper().replace('_','-'),9,True)
    text(283,21,'2026-09-29  |  SHEET 1 / 1',8,True,align='right')
    text(13,14,'STL governs geometry. Material / finish: selected PCBWay order. Do not scale drawing.',7.5)
    text(283,14,'Quotation issue. Physical fit verified by customer.',7.5,align='right')
    text(13,31,'STL SHA-256: '+p['sha256'],6.6,color=GRAY)

def front(name,x,y,s):
    w,h,z=P[name]['size_mm']
    c.drawImage(str(FRONTS[name]),x*mm,y*mm,w*s*mm,z*s*mm,mask='auto')
    dim_v(y,y+z*s,x-8,x,f'{z:.2f}')
    text(x,y-6,'FRONT VIEW: looking along +Y; +Z upward',7.3,color=GRAY)

def insert_part(name):
    is_lid=name=='lid'
    text(25,164,'INTERIOR VIEW | Looking along -Z | Scale 1.55:1',7.6,color=GRAY)
    xy=top_view(name,27,68,1.55)
    if is_lid:
        rows=[['L1','3.60','52.40','14.50'],['L2','76.40','52.40','14.50'],['L3','3.60','3.60','14.50'],['L4','77.00','15.00','14.50']]
        tags=[('L1',3.6,52.4,25,158),('L2',76.4,52.4,150,158),('L3',3.6,3.6,44,78),('L4',77,15,157,85)]
    else:
        rows=[['B1','5.00','8.70','13.00'],['B2','44.00','48.50','13.00']]
        tags=[('B1',5,8.7,21,91),('B2',44,48.5,97,158)]
    for label,X,Y,tx,ty in tags: tag(*xy(X,Y),label,tx,ty)
    w,h,z=P[name]['size_mm']
    text(27,50,f'Envelope: {w:.2f} x {h:.2f} x {z:.2f}',8,True)
    text(27,44,'O / coordinates refer to the exported STL bounding box.',7.3)
    text(167,163,f'{len(rows)}x M2 x 0.4 BONDED INSERTS',10,True,BLUE)
    y=table(167,157,115,['ID','X','Y','Entry Z'],rows,[19,28,28,40])
    text(167,y-6,'Insert from interior along -Z; seat flush.',8)
    y-=16
    text(167,y,'EXISTING PILOT GEOMETRY',8.8,True); y-=6
    y=para(167,y,'DIA 2.70 x 3.40 deep; lead-in DIA 3.10 x 0.25. DIA 2.40 tip relief to '+('8.00' if is_lid else '7.00')+' total depth from entry face.',115,8,3.9)-6
    text(167,y,'INSERT FIT TO BE CONFIRMED',8.8,True); y-=6
    y=para(167,y,'Reference insert: DIA 3.00 x 3.20 long. Current pilots are not adhesive-fit pockets for this insert. Confirm insert, adhesive and revised pocket dimensions before printing. Do not heat-set into cured resin.',115,8,3.9)-5
    y=para(167,y,'Fasteners: M2 x 8 nylon, measured under head. Keep thread and screw-tip space free of adhesive.',115,8,3.9)-5
    if is_lid:
        para(167,y,'Keep exterior blind. Button retention ledges require no inserts. Local wheel wall 0.60; receiver web 0.80; keeper ledges 1.00. Maintain guide and LCD-seat dimensions after finish.',115,8,3.9)
    else:
        para(167,y,'B1 boss DIA 5.50; B2 DIA 6.00. Four other case fixings: DIA 2.30 clearance, underside DIA 4.50 x 10.20 head wells. Closed well rim 0.75; screw-tip gap 0.50. Preserve closed floor.',115,8,3.9)

def other_part(name):
    w,h,z=P[name]['size_mm']
    scale=min(1.7 if name=='midframe' else 3.2,116/w,86/h)
    topy=73
    text(27,164,'TOP VIEW | Looking along -Z',8,True)
    text(27,159,f'Scale {scale:.2f}:1',7.5,color=GRAY)
    top_view(name,30,topy,scale)
    front(name,30,43,scale)
    text(169,162,'PART SPECIFICATION',10,True,BLUE)
    y=table(169,156,112,['Dimension','Nominal'],[['Overall X',f'{w:.2f}'],['Overall Y',f'{h:.2f}'],['Overall Z',f'{z:.2f}'],['Quantity','1'],['Installed inserts','None']],[62,50],size=8.3)
    notes={
      'midframe':[
        'Two DIA 2.30 clearance holes fasten to inserts in the base. Hole centers in exported XY: (2.70, 6.40), (41.70, 46.20).',
        'Flat battery-facing plate: 1.50 thick. Three locating keys: 1.50 thick with 2.50 engagement into the lid.',
        'Preserve the PCB locator, support lands and open battery-cable bay as modelled. No insert is installed in this part.',
        'Keep mating keys, PCB seats and screw seats free of finish buildup. Nominal lid key clearance: 0.20 per side, 0.30 at tip.'
      ],
      'button_strip':[
        'Three keys: DIA 4.40 at 6.00 pitch. Three actuator stems: DIA 1.80. Flexible arms: 2.00 wide x 0.80 thick.',
        'Solid mounting bar and integrated power-slider retaining shelf. No screw holes or inserts.',
        'Return arms must flex and recover repeatedly. Confirm selected resin suitability for this function.',
        'Preserve moving surfaces and small clearances after finishing. Retained by the separate keeper; do not bond moving keys or the slider.'
      ],
      'button_keeper':[
        'Rigid keeper: uniform 1.00 thickness. Four lateral retaining tabs. Print geometry as supplied.',
        'No holes, threads or inserts. Fits lid ledges beneath the button-strip mounting bar.',
        'Assembly translation: 3.30 along the lid guide. Keep sliding edges free of coating buildup.',
        'Customer applies a removable resin-compatible silicone bead at the keeper / lid seam to prevent backing out. Do not bond moving controls.'
      ],
      'power_slider':[
        'Flange: 14.00 x 7.60 x 0.80. Finger nub footprint: 6.00 x 3.80; overall height 4.60.',
        'Switch-handle socket: 2.00 x 2.00, with 2.40 square entry lead-in. Preserve socket and sliding surfaces.',
        'No holes for screws, threads or inserts. Retained inside the lid by the button-strip shelf.',
        'Guide clearance: 0.30 lateral and nominal 0.20 above / below flange in assembly. Keep finish off functional sliding faces.'
      ]
    }
    end=note_block(169,y-10,'FUNCTIONAL FEATURES',notes[name],112)
    assert end>33, (name,end)

outputs=[]
for name in ['base','lid','midframe','button_strip','button_keeper','power_slider']:
    path=OUT/(P[name]['file'].replace('.stl','-drawing.pdf'))
    c=canvas.Canvas(str(path),pagesize=(297*mm,210*mm),pageCompression=1)
    c.setTitle(titles[name]+' R21 - technical drawing')
    c.setAuthor('HardwareOne enclosure project')
    header(name)
    if name in ('base','lid'): insert_part(name)
    else: other_part(name)
    c.save()
    reader=PdfReader(path)
    assert len(reader.pages)==1
    assert P[name]['file'] in reader.pages[0].extract_text()
    outputs.append({'part':name,'pdf':str(path),'stl':P[name]['file'],'stl_sha256':P[name]['sha256']})
(WORK/'manifest.json').write_text(json.dumps(outputs,indent=2)+'\n')
print(json.dumps(outputs,indent=2))
