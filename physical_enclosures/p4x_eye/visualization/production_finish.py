"""Wait for a production render, encode both films, and write review evidence.

Keeps render intermediates until visual review; safe to rerun after interruption.
"""
import argparse,hashlib,io,json,os,shutil,subprocess,sys,time
from pathlib import Path
from PIL import Image,ImageDraw
p=argparse.ArgumentParser();p.add_argument('--variant',choices=['black','frosted-clear'],required=True);p.add_argument('--log',type=Path,required=True);a=p.parse_args()
HERE=Path(__file__).resolve().parent;OUT=HERE/'output'/'1080p'/a.variant;FRAMES=OUT/'continuous-frames'
print(f'FINISH_WORKER_PID={os.getpid()}',flush=True)
try:
 while True:
  log=a.log.read_text() if a.log.exists() else ''
  if 'Blender quit' in log:break
  time.sleep(10)
 assert all((FRAMES/f'{i:04d}.png').exists() for i in range(1,721)),'Render incomplete; intermediates retained'
 subprocess.run([sys.executable,str(HERE/'compose_annotated.py'),'--variant',a.variant,'--output-dir',str(OUT),'--crf','16'],check=True)
 prefix=f'p4x-eye-{a.variant}'
 def frame(movie,t):
  return Image.open(io.BytesIO(subprocess.check_output(['ffmpeg','-v','error','-ss',str(t),'-i',str(OUT/movie),'-frames:v','1','-f','image2pipe','-vcodec','png','-']))).convert('RGB')
 for movie,name,times in [(prefix+'-annotated-film.mp4','annotated-video-contact-sheet.jpg',[0,3,6,9,12.5,14,20,24,25,26.5,28,29.5]),(prefix+'-product-film.mp4','video-contact-sheet.jpg',[0,3,6,9,12.5,14,17.9,18.1,19,20.5,22,23.5])]:
  canvas=Image.new('RGB',(1280,4*264),(30,33,36));draw=ImageDraw.Draw(canvas)
  for i,t in enumerate(times):
   im=frame(movie,t);im.thumbnail((426,240));x=i%3*426;y=i//3*264;canvas.paste(im,(x,y));draw.text((x+8,y+243),f'{t:g} seconds',fill='white')
  canvas.save(OUT/name,quality=94)
 frame(prefix+'-annotated-film.mp4',21).save(OUT/'annotated-video-frame.png')
 shutil.copy2(FRAMES/'0720.png',OUT/'08-overhead-1080p.png')
 for name in ['annotation-layout.json','model-validation.json']:
  source=HERE/'output'/name
  if source.exists():shutil.copy2(source,OUT/name)
 for first,last in [(289,313),(468,556),(672,720)]:
  data=(FRAMES/f'{first:04d}.png').read_bytes()
  assert all((FRAMES/f'{i:04d}.png').read_bytes()==data for i in range(first+1,last+1)),(first,last)
 for name in ['annotation-validation.json','video-validation.json']:
  file=OUT/name;data=json.loads(file.read_text());data['production_settings']=json.loads((OUT/'production-settings.json').read_text());data['byte_identical_hold_frames']=[[289,313],[468,556],[672,720]];data['native_label_passes_composited']=a.variant=='frosted-clear' and name.startswith('annotation');file.write_text(json.dumps(data,indent=2)+'\n')
 report={'status':'ready_for_visual_review','variant':a.variant,'resolution':[1920,1080],'movies':[prefix+'-annotated-film.mp4',prefix+'-product-film.mp4'],'full_decode_passed':True,'temporary_frames_retained_for_review':True}
 (OUT/'production-status.json').write_text(json.dumps(report,indent=2)+'\n')
 print(json.dumps(report),flush=True)
except Exception as error:
 (OUT/'production-status.json').write_text(json.dumps({'status':'failed','error':str(error),'intermediates_retained':True},indent=2)+'\n')
 raise
