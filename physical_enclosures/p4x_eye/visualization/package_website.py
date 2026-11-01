"""Make optimized website assets, an image index, and a responsive design preview."""
from pathlib import Path
import hashlib
import json
import zipfile
from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
OUT = HERE / 'output' / 'website'
names = ['01-hero', '02-controls-ports', '03-exploded', '04-on-the-desk', '05-overhead']
titles = ['Hero / video introduction', 'Controls and USB-C detail', 'Exploded assembly / seven screws', 'On a desk / everyday context', 'Overhead / form and interface']
records = []
for name, title in zip(names, titles):
    source = OUT / (name + '.png')
    with Image.open(source) as im:
        im.load()
        assert im.width >= 1600 and im.height >= 1500
        web = im.convert('RGB')
        web.thumbnail((1800, 1800), Image.Resampling.LANCZOS)
        for ext in ('webp', 'jpg'):
            dest = OUT / (name + '.' + ext)
            if ext == 'webp':
                web.save(dest, quality=90, method=6)
            else:
                web.save(dest, quality=93, optimize=True, subsampling=0)
            with Image.open(dest) as check:
                check.load()
            records.append({'file': dest.name, 'width': web.width, 'height': web.height,
                            'bytes': dest.stat().st_size, 'sha256': hashlib.sha256(dest.read_bytes()).hexdigest()})
        records.append({'file': source.name, 'purpose': title, 'width': im.width, 'height': im.height,
                        'bytes': source.stat().st_size, 'sha256': hashlib.sha256(source.read_bytes()).hexdigest()})

font = ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf', 24)
small = ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf', 18)
sheet = Image.new('RGB', (1800, 1230), '#14171d')
d = ImageDraw.Draw(sheet)
d.text((32, 22), 'HARDWARE ONE / WEBSITE IMAGE SET', font=font, fill='#f3f4f2')
for i, (name, title) in enumerate(zip(names, titles)):
    x, y = 24 + (i % 3) * 592, 80 + (i // 3) * 570
    image = Image.open(OUT / (name + '.jpg'))
    image.thumbnail((568, 500), Image.Resampling.LANCZOS)
    sheet.paste(image, (x + (568 - image.width)//2, y + (500-image.height)//2))
    d.text((x, y + 512), title, font=small, fill='#c8d1d2')
d.text((1212, 752), 'MODELED. LIT. RENDERED.', font=font, fill='#8cdbd1')
d.text((1212, 800), 'Black enclosure / source-based internals', font=small, fill='#b5bcc3')
d.text((1212, 834), 'Full-resolution PNG + WebP + JPEG', font=small, fill='#b5bcc3')
d.text((1212, 868), 'Editable Blender cameras and lighting', font=small, fill='#b5bcc3')
sheet.save(OUT / 'website-image-set.jpg', quality=94)

html = '''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Hardware One — design preview</title><style>
:root{color-scheme:dark;--bg:#14151b;--surface:#1d2028;--ink:#f1f3f1;--muted:#aeb5c1;--line:#343940;--accent:#91ded3}
*{box-sizing:border-box}img{max-width:100%;height:auto}html{scroll-behavior:smooth}body{margin:0;background:var(--bg);color:var(--ink);font:17px/1.65 -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}a{color:inherit;text-decoration:none}a:hover{text-decoration:underline}header,main,footer{max-width:1240px;margin:auto;padding:0 40px}header{display:flex;justify-content:space-between;align-items:center;padding-top:28px;padding-bottom:28px;border-bottom:1px solid var(--line)}header b{letter-spacing:-.04em;font-size:22px}nav{display:flex;gap:28px;font-size:14px;color:var(--muted)}section{padding:90px 0;border-bottom:1px solid var(--line)}.eyebrow{font-size:12px;letter-spacing:.16em;text-transform:uppercase;color:var(--accent);margin:0 0 20px}h1,h2,h3{line-height:1.08;font-weight:600;letter-spacing:-.045em}h1{font-size:clamp(48px,5.3vw,76px);margin:0 0 26px}h2{font-size:clamp(32px,3.6vw,49px);margin:0 0 22px}h3{font-size:25px;margin:0 0 14px}p{color:var(--muted);max-width:640px;margin:0 0 22px}.hero{display:grid;grid-template-columns:1.2fr 1fr;gap:64px;align-items:center;padding-top:58px}.hero img{width:100%;border-radius:18px;display:block}.hero .intro{font-size:19px}.links{display:flex;gap:14px;flex-wrap:wrap;margin-top:30px}.button{padding:12px 20px;border:1px solid var(--line);border-radius:8px;font-size:14px;display:inline-block}.button.primary{background:var(--accent);color:#10221f;border-color:var(--accent)}.facts{display:flex;gap:35px;flex-wrap:wrap;margin-top:30px;font-size:12px;color:var(--muted)}.facts strong{display:block;color:var(--ink);font-size:16px;margin-bottom:2px}.section-head{display:flex;justify-content:space-between;align-items:end;gap:70px;margin-bottom:32px}.section-head p{max-width:410px}.film{display:block;width:100%;aspect-ratio:16/9;background:#11151b;border-radius:16px}.caption{font-size:12px;color:#8d96a3;margin-top:14px}.split{display:grid;grid-template-columns:1fr 1fr;gap:74px;align-items:center}.split img{width:100%;border-radius:16px;display:block}.exploded{max-height:850px;object-fit:contain;background:#11151a}.feature-list{padding:0;list-style:none}.feature-list li{padding:18px 0;border-bottom:1px solid var(--line);color:var(--muted);font-size:15px}.feature-list b{display:block;color:var(--ink);font-size:18px;font-weight:500}.tiles{display:grid;grid-template-columns:1fr 1fr;gap:28px}.tiles img{width:100%;border-radius:14px;display:block;aspect-ratio:4/3;object-fit:cover}.tiles h3{margin-top:24px}.tiles p{font-size:15px}.wide-image{width:100%;display:block;border-radius:16px;max-height:650px;object-fit:cover}.specs{display:grid;grid-template-columns:1fr 1.3fr;gap:80px}details{border-bottom:1px solid var(--line);padding:20px 0}summary{cursor:pointer;list-style:none;font-size:21px;letter-spacing:-.02em;display:flex;justify-content:space-between;align-items:center;gap:20px}summary:after{content:'+';color:var(--accent);font-weight:300;font-size:25px}details[open]>summary:after{content:'−'}summary::-webkit-details-marker{display:none}details p,details ul{font-size:15px;color:var(--muted);margin:18px 0 8px}details details{margin-left:24px;padding:15px 0}details details summary{font-size:16px}footer{display:flex;justify-content:space-between;gap:30px;padding-top:35px;padding-bottom:40px;color:var(--muted);font-size:12px}.note{font-size:12px}.section-label{color:var(--muted);font-size:13px;letter-spacing:.06em} @media(max-width:780px){header,main,footer{padding-left:22px;padding-right:22px}nav{gap:14px}.hero,.split,.specs,.tiles{grid-template-columns:1fr;gap:32px}section{padding:54px 0}.hero{padding-top:30px}.section-head{display:block}.section-head p{margin-top:20px}.facts{gap:24px}footer{display:block}footer span{display:block;margin:8px 0}}
</style></head><body>
<header><a href="https://cadenbeck.tech/"><b>Caden Beck</b></a><nav><a href="https://cadenbeck.tech/home/projects/">Projects</a><a href="https://cadenbeck.tech/home/about/">About</a><a href="https://github.com/CadenGithubB/HardwareOne">GitHub ↗</a></nav></header>
<main><section class="hero"><img src="01-hero.webp" width="1800" height="1350" alt="Black Hardware One enclosure viewed from above, showing its display, buttons, and USB-C ports"><div><p class="eyebrow">Hardware One / open source</p><h1>Small device.<br>Room to explore.</h1><p class="intro">A modular ESP32 platform for handheld tools, sensor nodes, connected devices, and wearable companions.</p><p>Choose your hardware. Enable the features you need. Make it your own.</p><div class="links"><a class="button primary" href="#film">Watch the device</a><a class="button" href="https://github.com/CadenGithubB/HardwareOne">Explore the project ↗</a></div><div class="facts"><span><strong>ESP32 / S3 / P4</strong>Processor options</span><span><strong>Modular</strong>Hardware and software</span><span><strong>Open source</strong>Built to be explored</span></div></div></section>
<section id="film"><div class="section-head"><div><p class="eyebrow">A closer look</p><h2>Every side.<br>Every layer.</h2></div><p>A complete rotation, then a look inside the enclosure—down to the board, battery, midplate, and seven assembly screws.</p></div><video class="film" controls playsinline preload="none" poster="film-poster.jpg" aria-label="Hardware One black enclosure product film"><source src="../1080p/black/p4x-eye-black-annotated-film.mp4" type="video/mp4"></video><p class="caption">30-second Blender visualization · 1080p · Play to explore the assembly</p></section>
<section class="split"><div><p class="eyebrow">Built in layers</p><h2>See what<br>holds it together.</h2><p>The enclosure is only part of the story. Inside are the display, populated PCB, support plate, battery, and the hardware that brings the assembly together.</p><ul class="feature-list"><li><b>A real assembly</b>Separate shell parts, a visible parting seam, and modeled internal components.</li><li><b>Seven fasteners</b>Four bottom screws, two midplate screws, and one board screw.</li><li><b>A starting point for your build</b>This enclosure is one Hardware One configuration. The firmware also supports other board and peripheral combinations.</li></ul></div><img class="exploded" src="03-exploded.webp" width="1440" height="1800" loading="lazy" alt="Exploded Hardware One assembly with separated display, top shell, populated board, midplate, battery, base, and seven screws"></section>
<section><p class="eyebrow">Details that matter</p><h2>Hands-on hardware.</h2><div class="tiles"><article><img src="02-controls-ports.webp" width="1800" height="1350" loading="lazy" alt="Close-up of the enclosure’s buttons and two USB-C ports"><h3>Control at your fingertips.</h3><p>A compact display, physical buttons, rotary input, and accessible ports in this handheld configuration.</p></article><article><img src="05-overhead.webp" width="1800" height="1200" loading="lazy" alt="Straight overhead view of the black enclosure and illustrative status screen"><h3>A compact working surface.</h3><p>Use local menus, a browser, or the shared command system. Available interfaces depend on your build.</p></article></div></section>
<section><div class="section-head"><div><p class="eyebrow">Beyond the workbench</p><h2>Ready for your next idea.</h2></div><p>Build a handheld instrument, connect sensors, try offline speech tools, or link devices together.</p></div><img class="wide-image" src="04-on-the-desk.webp" width="1800" height="1200" loading="lazy" alt="Hardware One on an oak desk beside a notebook and pen"><p class="caption">Rendered scene; device shown at its modeled physical scale.</p></section>
<section class="specs"><div><p class="eyebrow">The platform</p><h2>Choose a build.<br>Keep exploring.</h2><p>Hardware One spans multiple processors and device roles. Features depend on your board, silicon revision, memory, peripherals, and build profile.</p><a class="button" href="https://github.com/CadenGithubB/HardwareOne">Setup and documentation ↗</a></div><div><details><summary>Hardware specifications</summary><ul><li>ESP32 and ESP32-S3: up to 240 MHz.</li><li>ESP32-P4 revision 1.3: up to 360 MHz; revision 3.x: up to 400 MHz. Separate firmware builds are required.</li><li>Current documented configurations: 8–16 MB flash, 2–32 MB PSRAM.</li><li>Optional displays, physical controls, cameras, audio, microSD, and I²C sensors.</li><li>P4 wireless connectivity uses a companion radio.</li></ul><p>Current P4 defaults target 3.x; recorded hardware qualification covers revision 3.2.</p></details><details><summary>Software features</summary><ul><li>Shared commands across serial, browser, local menus, and supported remote interfaces.</li><li>ESP-NOW pairing, bonded devices, remote commands, and multi-hop networking.</li><li>Sensor logging, automations, offline maps, and MQTT integration.</li><li>Optional smart-glasses, ring, speech, and assistant features on compatible builds.</li></ul></details></div></section>
<section class="specs"><div><p class="eyebrow">Project journal</p><h2>How it got here.</h2><p>From the first sensor experiments to the public release and enclosure development.</p></div><div><details><summary>Early Development</summary><details><summary>7/10/25 — Initial Posting</summary><p>Initial sensors, gamepad input, connectivity, and command processing.</p></details><details><summary>8/12/25 — Second Update</summary><p>Hardware changes, filesystem improvements, and diagnostics.</p></details><details><summary>10/7/25 — Third Update</summary><p>Continued development of the device and its interfaces.</p></details><details><summary>12/19/25 — Fourth Update</summary><p>Sensor lifecycle improvements, wireless connectivity, and system tools.</p></details></details><details><summary>Release &amp; Beyond</summary><details><summary>2/24/26 — Public GitHub Release</summary><p>Public source, configurable builds, bonded devices, maps, camera, and speech features.</p></details><details><summary>Case STL Release</summary><p>Enclosure development and downloadable mechanical designs.</p></details></details><p class="caption"><a href="https://cadenbeck.tech/home/projects/software-related-projects/hardwareone/">Read the complete original posts ↗</a></p></div></section>
<p class="note" style="padding-top:24px">Product images are Blender visualizations of one enclosure configuration. Screen content is illustrative, rather than a firmware capture. Some component details are approximations.</p></main><footer><span>Hardware One · Caden Beck’s Project Log</span><span>Design preview · November 1, 2026</span></footer></body></html>'''
(OUT / 'design-preview.html').write_text(html)
with Image.open(OUT / 'film-poster.jpg') as poster:
    poster.load()
    assert poster.size == (1920, 1080)
records.append({'file': 'film-poster.jpg', 'width': 1920, 'height': 1080, 'bytes': (OUT / 'film-poster.jpg').stat().st_size})
(OUT / 'asset-manifest.json').write_text(json.dumps(records, indent=2))
(OUT / 'README.md').write_text('''# Hardware One website image set

Five new Cycles renders from the approved black assembly, with dark studio backgrounds and an oak-desk scene. Full-resolution PNG masters, optimized WebP files, and JPEG fallbacks are supplied. `hardware-one-website.blend` retains the editable cameras, lighting, and model.

Alongside the image archive, open `design-preview.html` in the output folder for a responsive page concept using these assets and the completed black 1080p video. It keeps the dark palette and nested update groups, with a larger hero, dedicated film section, internals section, and details gallery. Journal entries in this concept are short summaries; the live WordPress originals remain untouched. The live website has not been redesigned by this packaging script.

The interface shown on the device is illustrative, not a firmware screenshot. Internal component placement follows the existing reference-based scene; some package details are approximations. This imagery represents one enclosure configuration, not every processor or peripheral supported by Hardware One.

## Files and intended use

- 01-hero: opening product image or video poster.
- 02-controls-ports: detail card for inputs and ports.
- 03-exploded: internals section; all seven screws included.
- 04-on-the-desk: wider editorial section showing context and scale.
- 05-overhead: hardware overview or alternate product card.
- website-image-set.jpg: overview of the five new images.
- asset-manifest.json: dimensions, sizes, and SHA256 hashes.

Use WebP on the site, with descriptive alt text and explicit image dimensions. Keep the video click-to-play with controls, inline playback, and a poster. The preview links to the existing local video without duplicating it.
''')
with zipfile.ZipFile(OUT / 'hardware-one-website-images.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    for name in names:
        for ext in ('png', 'webp', 'jpg'):
            z.write(OUT / (name + '.' + ext), name + '.' + ext)
    for filename in ('README.md', 'asset-manifest.json', 'website-image-set.jpg', 'film-poster.jpg'):
        z.write(OUT / filename, filename)
print(json.dumps({'verified_images': len(records), 'archive': str(OUT / 'hardware-one-website-images.zip')}, indent=2))
