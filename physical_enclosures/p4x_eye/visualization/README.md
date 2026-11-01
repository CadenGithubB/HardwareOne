# P4X EYE — Blender product visualization

## Website image set

`output/website/` contains five new dark-studio/editorial compositions, full-size
PNG masters, optimized WebP and JPEG exports, an image ZIP, and an editable
`hardware-one-website.blend`. `design-preview.html` is a responsive layout
concept using these assets and the existing black 1080p film. It was reviewed
on desktop and at 390px mobile width; it does not modify the live website.

Recreate with `website_stills.py` loaded against the approved black native
scene, then run `python3 package_website.py`. The render uses CPU Cycles, four
threads and 96 samples, independent of the clear GPU film. Use `--preview`
for small composition proofs. These images retain the existing illustrative
display UI and reference-based internal assembly. The preview's journal uses
short summaries; the complete original WordPress posts remain on the site.

Editable R26 product assembly, with a satin-black case for this first visual
direction. Manufacturing source and STL releases are unchanged.

## Deliverables

Files are in `output/`:

- `p4x-eye-r26-black.blend`: native scene with separately editable case parts,
  electronics, materials, lighting, cameras, desk props, and animation.
- `01-studio-black.png`: studio hero.
- `02-connectors-black.png`: ports and controls close-up.
- `03-desk-black.png`: oak desk with notebook, pen, and coffee for scale.
- `04-exploded-black.png`: separated enclosure, battery, midframe, PCB, and LCD.
- `05-electronics-underside.png`: processor/radio side of PCB.
- `06-wheel-side-black.png`: wheel-side close-up with the unintended raised
  strip removed and the genuine shell seam retained.
- `08-overhead-black.png`: horizontal overhead ending.
- `07-exploded-labeled.png`: 3840 × 2160 gray-studio diagram with nine
  editable white label/arrow groups identifying the layers and fasteners.
- `p4x-eye-black-annotated-film.mp4`: continuous 30-second take with a six-second
  exploded pause and staggered white label fades, in the same gray studio.
- `p4x-eye-black-product-film.mp4`: continuous silent 24 s / 24 fps / 1280 × 720 H.264 film.
- `animation-contact-sheet.jpg`: animation framing and transition checks.
- `render-contact-sheet.jpg`, `video-contact-sheet.jpg`: final stills gallery
  and frames decoded from the actual encoded MP4.
- `deliverables.json`: retained artifact hashes, sizes, and cleanup record.
- `model-validation.json`, `video-validation.json`: source/assembly and video checks.
- `scene-verification.json`: screen/control surface probes, exploded offsets,
  reassembly states, and camera margins across all 144 hero frames.

## Edit in Blender

Built with Blender 4.4.3. Coordinates are metres; display units are millimetres.
No external textures, add-ons, fonts, or HDRI downloads are required.

The `PRODUCT` collection is shared by two scenes: studio and desk. Six parent
empties organize the stack and three more hold the seven screws. Child mesh
positions are assembled dimensions. Four M2 × 8 screws withdraw below the base,
two M2 × 8 screws withdraw above the midplate, and one provisional M1.6 × 5
reference screw withdraws above the P4X board. Screws have modeled threads and
recessed drives. The midplate axes are (5, 8.7) and (44, 48.5) mm; the PCB axis
is (21.1, 8.7) mm, following the enclosure source.
In the original studio/desk scenes, set frame 1 for the complete enclosure
and frame 196 for the exploded state. The continuous film uses the third scene
and its independent timing below.
Keep the scene frames synchronized when switching scenes; shared parent objects
carry animation. Native imported meshes remain editable, with shading for highlights. These visualization meshes must not be
used as replacement manufacturing exports.

The screen UI is editable Blender text and geometry. It is an illustrative
Hardware One status layout, not a firmware screenshot. It can be replaced by a
real display capture later. No branding or legends have been added to the case.

The third scene, `03 Annotated • continuous studio`, contains a separately
animated copy with linked mesh data, native white text and thin white leaders.
One perspective camera makes the complete move without cuts or background
changes. The full orbit stays at a constant height, holds still for one second,
then pulls back directly as the layers rise; there is no intermediate hero swoop.
After the exploded pause, one quaternion move settles into a straight overhead
view with the long edge horizontal. The final two seconds hold still. Edit labels under
`ANNO • Labels and leaders`. Frame 504 shows the
fully exploded assembly and all nine callouts; frame 1 is assembled.

| Frames | Continuous movement |
| --- | --- |
| 1–289 | Full 360° orbit; assembled device, lower camera reveals encoder wheel |
| 289–313 | One-second still hold |
| 314–432 | Direct pullback as the layers separate |
| 432–576 | Six-second exploded pause with label fades |
| 577–672 | Reassemble and ease directly into a horizontal overhead view |
| 673–720 | Stationary overhead finish |

The clean 24-second version removes only the stationary reading hold. The
camera and assembly match across that join. The annotated version retains
all 720 frames, lasting 30 seconds. Fully visible label frames 469–556 reuse
frame 468 exactly so the frozen view does not shimmer.

The stills use Cycles path tracing. The film uses Eevee for practical local
render times. Both use AgX and the same geometry, materials, cameras, and lights.

## Rebuild and render

Run from the repository root. Factory startup avoids unrelated user add-ons.
On this Mac Blender requires normal macOS graphics access; restricted sandbox
startup crashed before the Python script ran.

```sh
BLENDER=/Applications/Blender.app/Contents/MacOS/Blender
VIS=physical_enclosures/p4x_eye/visualization
"$BLENDER" -b --factory-startup --python-exit-code 1 -P "$VIS/build_scene.py" -- --preview
"$BLENDER" -b --factory-startup "$VIS/output/p4x-eye-r26-black.blend" --python-exit-code 1 -P "$VIS/annotate.py"
"$BLENDER" -b --factory-startup "$VIS/output/p4x-eye-r26-black.blend" --python-exit-code 1 -P "$VIS/render.py" -- stills
"$BLENDER" -b --factory-startup "$VIS/output/p4x-eye-r26-black.blend" --python-exit-code 1 -P "$VIS/verify_scene.py"
"$BLENDER" -b --factory-startup "$VIS/output/p4x-eye-r26-black.blend" --python-exit-code 1 -P "$VIS/verify_annotation.py"
"$BLENDER" -b --factory-startup "$VIS/output/p4x-eye-r26-black.blend" --python-exit-code 1 -P "$VIS/render.py" -- proof --engine BLENDER_EEVEE_NEXT
"$BLENDER" -b --factory-startup "$VIS/output/p4x-eye-r26-black.blend" --python-exit-code 1 -P "$VIS/render.py" -- video --engine BLENDER_EEVEE_NEXT
python3 "$VIS/compose_annotated.py"
```

Video rendering skips existing numbered frames so interrupted jobs can resume.
Delete only this task's `output/continuous-frames/` when changing animation or
materials; otherwise old frames will be mixed into the new movie. `--start`
and `--end` select a subset. Use `--only 07` with `stills` to render only the
4K labeled view. Encoding requires ffmpeg/ffprobe and Pillow and verifies both movies'
frame counts, dimensions, frame rate, duration and full decode.

`verify_annotation.py` checks the evaluated camera path and product framing
across all 720 frames, a fixed exploded pose during the pause, all seven visible
screws, white callouts, fade values and text bounds. Rebuilding the base model
removes the third scene; rerun `annotate.py` afterward. Temporary video and proof
frames may be removed after visual inspection; the native scene retains all
editable geometry, animation and text.

## Geometry and reference fidelity

The six case meshes start from the matched R26 STL set. Each source SHA-256 is checked
against the export manifest before import. Print rotations and translations are
inverted in `build_scene.py`; numeric assembly bounds are recorded and checked.
For the user-requested cosmetic correction, the pictured corner of base and lid
is trimmed to the smooth exterior roof footprint. All four screw corners
now share the lid's outline, including removal of the lower shell's sleeve lips.
The actual seated base/lid parting line remains visible at Z14.5, with a small
0.035 mm presentation edge break on each outer mating perimeter rather than a
concealed or fused joint. Interior seating faces stay at the original datum.
The lid's wheel-side wall is also flush to the roof footprint, removing the
free-standing edge of the former outward band while retaining the wheel opening.
The original SCAD and release STL files are unchanged. The real parting edges and the CAD's 1 mm outer face chamfer are retained.
Shell coplanar faces are dissolved and degenerate edges removed. The extra
shell bevel modifier was removed: its interaction with weighted normals
created invalid shading normals, causing the triangular flash under the
encoder. Both final shell meshes verify finite unit shading normals.
The lid roof is Z29 mm, the lid joint Z14.5, and key tops Z30. The power cap is
at the source's displayed +1 mm travel offset. Case dimensions are approximately
80 × 56 × 29 mm. The source's local X extension to 80.52 mm is among the
protrusions trimmed back to the smooth roof perimeter for this visualization.

The PCB outline, thickness, screen seat, active area, battery, and controls use
repository enclosure references. The 245 actual component placement rows and
official silk artwork come from [Espressif's reference archive](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32p4/_downloads/0857ce9b0e3668c3e56170a8a595db07/ESP32-P4X-EYE-EN.zip).
Silkscreen strokes are clipped inside the physical PCB perimeter and mounting
hole, including stroke width, so artwork cannot float beyond the board or
appear above the encoder through the case. The original reference files are
unchanged. Every evaluated ink vertex is checked against the actual board mesh.
See `assets/provenance.json` for archive identity. Its physical PCB/placement is
MB V2.3, matching the enclosure reference, while its schematic is V2.4.

Package bodies, lead shapes, connector interiors, PCB material appearance,
battery label/folds/wire route, and unseen electronic details are visualization
approximations. Component placement is sourced; detailed manufacturer 3D package
models were not supplied. Small passives and ICs use package-family dimensions;
the processor and RF module are identifiable, with modeled contacts and shield.
The camera is omitted, as in the enclosure design. No extra external antenna is
invented. The battery follows the user's 65 × 36 × 10 mm pack, not the smaller
pack used in Espressif's stock case.

This scene supports product visualization. It does not establish physical fit,
manufacturing tolerance, electrical correctness, or supplier approval.

## Storage

Retain final native scene, stills, movie, compact source references and reports.
Numbered video/proof frames, Blender backup files, and downloaded reference ZIP
are task-created temporary outputs; remove after the final outputs are checked.

## Lightly frosted clear edition

`output/frosted-clear/` contains a separate native model, clear product films,
studio/desk stills, a labeled exploded still and the overhead finish. The shell
and printed midplate use neutral clear resin: 96% transmission, roughness 0.12,
IOR 1.46, and a subtle surface texture. Controls remain charcoal. This is a
visual material approximation, not a measured resin optical profile.

Create it from the verified black model using `make_clear.py`. The camera move
and geometry are shared between both editions. Pass `--variant frosted-clear`
to `render.py` and `compose_annotated.py` to keep outputs separate. Clear films
use Cycles with denoising and Metal GPU refraction; the black film uses Eevee.
Example clear film command after making the clear native model:

```sh
"$BLENDER" -b --factory-startup "$VIS/output/frosted-clear/p4x-eye-r26-frosted-clear.blend" --python-exit-code 1 -P "$VIS/render.py" -- video --variant frosted-clear --gpu --samples 16
"$BLENDER" -b --factory-startup "$VIS/output/frosted-clear/p4x-eye-r26-frosted-clear.blend" --python-exit-code 1 -P "$VIS/clear_labels.py"
python3 "$VIS/compose_annotated.py" --variant frosted-clear
```

Frame 720 is the final overhead pose. Use `stills --only 08` to render it.
Both camera verifiers write evidence alongside the loaded native file.

The clear movie uses separate native label passes during the stationary
exploded hold. `clear_labels.py` exports the nine groups and native fade values;
`compose_annotated.py` overlays them on the identical traced background pose.
This prevents low-sample denoising from softening small type. The editable
labels remain in the Blender file; the temporary passes can be removed after
verification. The clear labeled still is the 1280×720 film frame; the black
labeled still remains 4K.

## 1080p production revision

`output/1080p/black/` and `output/1080p/frosted-clear/` isolate the production
assets from earlier720p review files. Both native files preserve the verified
360°, initial hold, exploded callouts and horizontal overhead ending.
Production uses1920×1080 at24fps, quarter-frame motion blur and sign-compatible
camera quaternions. Clear uses Cycles/Metal with128 maximum samples, minimum32,
adaptive threshold0.025, fixed seed and high-quality accurate denoising with
albedo/normal guides. Clear micro-bump is disabled to avoid subpixel sparkle;
the accepted resin roughness/transmission are unchanged. Black uses Eevee128
antialiasing samples. CRF16 encoding retains fine detail.

`production.py` creates the native settings from the verified review scene.
`render.py --production --output-dir PATH` renders into that edition's resumable
cache. Clear stationary label frames reuse the traced frame432 and separate
native label passes. `production_finish.py --variant VARIANT --log LOG` waits
for the corresponding render, encodes both versions, checks full decode and
actual resolution, and writes preview sheets plus `production-status.json`.
It retains intermediates until visual review and cleanup. The short
`1080p-motion-test.mp4` is a diagnostic ping-pong loop, not the finished film.
