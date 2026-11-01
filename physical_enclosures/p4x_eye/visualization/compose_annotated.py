"""Encode the continuous studio take; no cuts, fades between shots or background swaps."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
from PIL import Image

OUT=Path(__file__).resolve().parent/'output'
p=argparse.ArgumentParser()
p.add_argument('--output-dir',type=Path)
p.add_argument('--crf',type=int,default=18)
p.add_argument('--variant',choices=['black','frosted-clear'],default='black')
args=p.parse_args()
if args.variant!='black': OUT=OUT/args.variant
if args.output_dir: OUT=args.output_dir.resolve()
FRAMES=OUT/'continuous-frames'
ffmpeg,ffprobe=shutil.which('ffmpeg'),shutil.which('ffprobe')
assert ffmpeg and ffprobe
assert all((FRAMES/f'{i:04d}.png').exists() for i in range(1,721))

if args.variant=='frosted-clear':
    # Keep the traced resin/background, but render typography separately so
    # low-sample denoising cannot soften the small native labels.
    passes=OUT/'label-passes'
    assert (passes/'opacity.json').exists(),'Run clear_labels.py on the clear native scene first'
    opacity=json.loads((passes/'opacity.json').read_text())
    layers=[Image.open(passes/f'{i:02d}.png').convert('RGBA') for i in range(9)]
    background=Image.open(FRAMES/'0432.png').convert('RGBA')
    for frame in range(433,577):
        result=background.copy()
        for layer,value in zip(layers,opacity[str(frame)]):
            value=max(0,min(1,value))
            if not value:continue
            overlay=layer.copy()
            overlay.putalpha(layer.getchannel('A').point(lambda a:round(a*value)))
            result.alpha_composite(overlay)
        result.convert('RGB').save(FRAMES/f'{frame:04d}.png')
    shutil.copy2(FRAMES/'0504.png',OUT/'07-exploded-labeled.png')

def encode(name,clean=False):
    if args.variant!='black':name=name.replace('-black','-'+args.variant)
    movie=OUT/name
    command=[ffmpeg,'-hide_banner','-loglevel','error','-y','-framerate','24','-i',str(FRAMES/'%04d.png')]
    if clean:
        # Remove only the stationary label hold. The camera and product pose on
        # either side are identical, so the clean version is the same take.
        command+=['-vf',"select='lte(n,431)+gte(n,576)',setpts=N/(24*TB)"]
    command+=['-an','-c:v','libx264','-preset','slow','-crf',str(args.crf),'-pix_fmt','yuv420p','-movflags','+faststart',str(movie)]
    subprocess.run(command,check=True)
    probe=json.loads(subprocess.check_output([ffprobe,'-v','error','-count_frames','-show_entries',
        'stream=codec_name,width,height,r_frame_rate,nb_read_frames:format=duration','-of','json',str(movie)]))
    frames=576 if clean else 720
    stream=probe['streams'][0]
    assert stream['codec_name']=='h264' and stream['r_frame_rate']=='24/1'
    assert (stream['width'],stream['height'])==Image.open(FRAMES/'0001.png').size
    assert int(stream['nb_read_frames'])==frames,probe
    assert abs(float(probe['format']['duration'])-frames/24)<.01,probe
    subprocess.run([ffmpeg,'-v','error','-i',str(movie),'-f','null','-'],check=True)
    return {'video':name,'sha256':hashlib.sha256(movie.read_bytes()).hexdigest(),'probe':probe,
            'full_decode_passed':True,'single_continuous_camera':True,'background':'gray studio','intro_orbit_degrees':360,'intro_orbit_seconds':12,'pre_explosion_hold_seconds':1,'overhead_horizontal_ending':True,'final_hold_seconds':2}

annotated=encode('p4x-eye-black-annotated-film.mp4')
annotated.update({'reading_pause_seconds':6,'callout_groups':9,'white_text_and_arrows':True,
    'native_editable_scene':'03 Annotated • continuous studio'})
if (OUT/'07-exploded-labeled.png').exists():
    annotated.update({'still':'07-exploded-labeled.png','still_size':list(Image.open(OUT/'07-exploded-labeled.png').size)})
(OUT/'annotation-validation.json').write_text(json.dumps(annotated,indent=2)+'\n')
clean=encode('p4x-eye-black-product-film.mp4',True)
(OUT/'video-validation.json').write_text(json.dumps(clean,indent=2)+'\n')
print(json.dumps({'annotated':annotated,'clean':clean},indent=2))
