#!/usr/bin/env python3
"""Draw the R26 supplier assembly/finish PDF from the released STL meshes."""
from pathlib import Path
import hashlib
import json
import sys

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
from reportlab.pdfgen import canvas
from reportlab.lib.colors import HexColor, white
from reportlab.lib.pagesizes import A4
from reportlab.lib.utils import simpleSplit

from check_wall_sections import load_meshes

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'tmp/pdfs/r26'
WORK.mkdir(parents=True, exist_ok=True)
MANIFEST = ROOT / 'stl/export-manifest-r26.json'
manifest, raw_meshes, hashes = load_meshes(MANIFEST, 56, 29)
source_hash = hashlib.sha256((ROOT/'enclosure.scad').read_bytes()).hexdigest()
assert manifest['source_sha256'] == source_hash
meshes = {k: np.array(v) for k, v in raw_meshes.items()}
COLORS = {'base': '#388b96', 'midframe': '#83aba0', 'lid': '#547b99',
          'button_strip': '#da983e', 'button_keeper': '#a87845', 'power_slider': '#8f78af'}
NAMES = {'base': '1  Base', 'midframe': '2  Midframe', 'button_keeper': '3  Keeper',
         'button_strip': '4  Button strip', 'power_slider': '5  Power slider', 'lid': '6  Lid'}
# PCBWay order number for each part, as shown on the page 1 table. Midframe
# and keeper numbers come from PCBWay's 2026-09-29 review-status and 2026-10-01
# payment emails; the engineering thread only named the other four.
ORDERS = {'base': 'C-1K3W1162240A', 'midframe': 'C-1K7W1162240A', 'button_keeper': 'C-1K4W1162240A',
          'button_strip': 'C-1K5W1162240A', 'power_slider': 'C-1K8W1162240A', 'lid': 'C-1K6W1162240A'}
ISSUE_DATE = '2026-10-12'
PAGES = 2


def render_parts(filename, offsets, view, labels=True, figsize=(7, 9)):
    view = np.array(view, dtype=float); view /= np.linalg.norm(view)
    right = np.cross([0,0,1], view); right /= np.linalg.norm(right)
    up = np.cross(view, right)
    transform = np.array([right, up, view]).T
    triangles, colors, anchors = [], [], {}
    light = np.array([0.4,-0.3,0.86]); light /= np.linalg.norm(light)
    from matplotlib.colors import to_rgb
    for name, dz in offsets.items():
        mesh = meshes[name].copy(); mesh[:,:,2] += dz
        if name == 'power_slider':
            mesh[:,:,0] += 1.0  # Source reference switch handle is at +1 mm travel.
        normal = np.cross(mesh[:,1]-mesh[:,0], mesh[:,2]-mesh[:,0])
        lengths = np.linalg.norm(normal, axis=1)
        normal = normal/np.maximum(lengths[:,None],1e-15)
        shade = .68+.32*np.maximum(0, normal @ light)
        projected = mesh @ transform
        visible = normal @ view > 1e-9
        colors.extend(np.array(to_rgb(COLORS[name]))[None,:]*shade[visible,None])
        triangles.extend(projected[visible])
        anchors[name] = projected.reshape(-1,3).mean(axis=0)
    triangles, colors = np.array(triangles), np.array(colors)
    lo = triangles[:,:,:2].min(axis=(0,1)); hi = triangles[:,:,:2].max(axis=(0,1))
    # Per-pixel depth testing keeps hidden internal ribs off the outer faces.
    # Sorting triangles by centroid alone is insufficient for this enclosure.
    width = 1400
    height = int(np.ceil(width*(hi[1]-lo[1])/(hi[0]-lo[0])))
    raster = np.full((height,width,3),255,dtype=np.uint8)
    depth = np.full((height,width),-np.inf)
    pixel = triangles.copy()
    pixel[:,:,0] = (pixel[:,:,0]-lo[0])/(hi[0]-lo[0])*(width-1)
    pixel[:,:,1] = (hi[1]-pixel[:,:,1])/(hi[1]-lo[1])*(height-1)
    for t, color in zip(pixel,colors):
        x0=max(0,int(np.floor(t[:,0].min()))); x1=min(width-1,int(np.ceil(t[:,0].max())))
        y0=max(0,int(np.floor(t[:,1].min()))); y1=min(height-1,int(np.ceil(t[:,1].max())))
        a,b,d=t
        denom=(b[1]-d[1])*(a[0]-d[0])+(d[0]-b[0])*(a[1]-d[1])
        if abs(denom)<1e-12: continue
        xx,yy=np.meshgrid(np.arange(x0,x1+1)+.5,np.arange(y0,y1+1)+.5)
        u=((b[1]-d[1])*(xx-d[0])+(d[0]-b[0])*(yy-d[1]))/denom
        v=((d[1]-a[1])*(xx-d[0])+(a[0]-d[0])*(yy-d[1]))/denom
        w=1-u-v; z=u*a[2]+v*b[2]+w*d[2]
        region=depth[y0:y1+1,x0:x1+1]
        visible=(u>=-1e-8)&(v>=-1e-8)&(w>=-1e-8)&(z>region)
        region[visible]=z[visible]
        raster[y0:y1+1,x0:x1+1][visible]=np.round(color*255).astype(np.uint8)
    fig, ax = plt.subplots(figsize=figsize)
    ax.imshow(raster,extent=(lo[0],hi[0],lo[1],hi[1]),origin='upper')
    if labels:
        for name, p in anchors.items():
            ax.annotate(NAMES[name], xy=p[:2], xytext=(hi[0]+5,p[1]),
                        fontsize=10, va='center', color='#18364b',
                        arrowprops={'arrowstyle':'-', 'color':'#718b97', 'lw':.8})
    ax.set(xlim=(lo[0]-3,hi[0]+(49 if labels else 3)), ylim=(lo[1]-3,hi[1]+3))
    ax.set_aspect('equal'); ax.axis('off')
    fig.savefig(WORK/filename, dpi=220, bbox_inches='tight', pad_inches=.04, facecolor='white')
    plt.close(fig)


render_parts('exploded.png', {'base':0,'midframe':24,'button_keeper':39,
             'button_strip':54,'power_slider':70,'lid':99}, [1,-1,.8])
render_parts('inside-lid.png', {'lid':0,'button_strip':0,'button_keeper':0,'power_slider':0},
             [1,-1,-1.8], labels=False, figsize=(7,4))

OUT = ROOT/'output/pdf/p4x-eye-r26-pcbway-assembly.pdf'
OUT.parent.mkdir(parents=True,exist_ok=True)
W,H = A4
c = canvas.Canvas(str(OUT),pagesize=A4)
c.setTitle('P4X-EYE R26 - Assembly and finish')
c.setAuthor('P4X-EYE enclosure project')
INK=HexColor('#18364b'); BLUE=HexColor('#14687c'); MUTED=HexColor('#56707c')
PALE=HexColor('#e9f3f4'); LINE=HexColor('#d0dce1')


def text(x,y,s,size=9,bold=False,color=INK):
    c.setFillColor(color); c.setFont('Helvetica-Bold' if bold else 'Helvetica',size); c.drawString(x,y,s)


def para(x,y,s,width=W-68,size=9,leading=12.5):
    for line in simpleSplit(s,'Helvetica',size,width):
        text(x,y,line,size); y-=leading
    return y


def heading(page,subtitle):
    c.setFillColor(BLUE); c.rect(0,H-8,W,8,stroke=0,fill=1)
    text(34,H-43,'P4X-EYE / R26',21,True)
    text(34,H-63,subtitle,10,color=MUTED)
    text(34,23,f'{ISSUE_DATE} | Units: mm | Do not scale | R26 STL files govern geometry',7.5,color=MUTED)
    text(W-64,23,f'{page} / {PAGES}',8,color=MUTED)


def image(name,x,y,w,h):
    c.drawImage(str(WORK/name),x,y,width=w,height=h,preserveAspectRatio=True,anchor='c',mask='auto')


heading(1,'Exploded assembly - six matching printed parts')
image('exploded.png',34,276,W-68,460)
text(34,262,'Parts share the actual assembly axes; vertical gaps are enlarged for clarity.',8.5,color=MUTED)
text(34,249,'Electronics and fasteners omitted. Assemble by the sequence on page 2.',8.5,color=MUTED)
rows=[('1 Base',ORDERS['base'],'p4x-eye-base-r26.stl'),
      ('2 Midframe',ORDERS['midframe'],'p4x-eye-midframe-r26.stl'),
      ('3 Keeper',ORDERS['button_keeper'],'p4x-eye-button-keeper-r26.stl'),
      ('4 Button strip',ORDERS['button_strip'],'p4x-eye-button-strip-r26.stl'),
      ('5 Power slider',ORDERS['power_slider'],'p4x-eye-power-slider-r26.stl'),
      ('6 Lid',ORDERS['lid'],'p4x-eye-lid-r26.stl')]
y=228
for i,row in enumerate([('PART','PCBWAY ORDER','FILE')]+rows):
    if i%2==0:
        c.setFillColor(PALE);c.rect(34,y-14,W-68,23,stroke=0,fill=1)
    for x,s in zip([42,160,328],row):text(x,y-6,s,8.2,i==0)
    y-=23
y=para(34,57,'All six parts are one matched R26 set. Do not mix them with files from earlier revisions.',size=8,leading=10)
para(34,y-1,'Changes from the files you reviewed: base and lid edge steps replaced by a 1 mm chamfer; button strip steps increased to 0.5 and 0.7 mm. Screw-well walls are unchanged at 1 mm.',size=8,leading=10)
c.showPage()

heading(2,'Assembly sequence, finish and handling')
image('inside-lid.png',34,536,W-68,213)
text(34,523,'Lid viewed from inside: orange strip, brown keeper and purple slider in assembly position.',8,color=MUTED)
text(34,500,'ASSEMBLY',10,True,color=BLUE)
y=482
for n,s in enumerate([
    'With the lid inverted, insert the power slider from inside so its nub passes through the slot. Place the button strip over it, aligning the three keys with their holes.',
    'Lower the keeper into its entry windows at +3.30 Y, then slide it -3.30 Y onto the ledges. A removable, resin-compatible silicone anti-backout bead is required; keep it clear of moving surfaces and check varnish compatibility.',
    'Fit the nominal 65 x 36 x 10 battery with its 0.50 pad allowance. Install the midframe and PCB in the base; seat the display in the lid and connect its flex. Route leads through the open cable bay.',
    'Align the slider socket with the switch handle while lowering the lid over the frame keys and USB rail. Four M2 x 8 case screws enter through the base into the lid inserts. Check free movement; do not pull a binding lid closed with the screws.'
],1):
    text(34,y,str(n)+'.',9,True);y=para(49,y,s,width=W-83,size=8.6,leading=11.5)-7
text(34,y-1,'FINISH AND HANDLING',10,True,color=BLUE);y-=19
for s in [
    'Material: UTR-8100 transparent resin. Finish: clear varnish on all surfaces of each separate printed part, including internal surfaces, guides and recesses. No masking.',
    'Ship the six parts separately, unassembled. The customer assembles them after delivery using the sequence above.',
    'During finishing and packing, support the broad faces. Do not load the button arms, slider nub, USB tab or locating keys.'
]: y=para(34,y,s,size=8.5,leading=11.2)-7
c.save()
record={'source_sha256':source_hash,'mesh_sha256':hashes,'pdf':str(OUT.relative_to(ROOT)),
        'pdf_sha256':hashlib.sha256(OUT.read_bytes()).hexdigest(),
        'slider_reference_x_offset_mm':1.0,
        'views':'Exploded and inside-lid views from the released STL meshes',
        'visual_review_completed':False}
(ROOT/'validation/drawing-source-correspondence-r26.json').write_text(json.dumps(record,indent=2)+'\n')
print(OUT)
