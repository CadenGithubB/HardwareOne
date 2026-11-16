#!/usr/bin/env python3
"""Run the shipping health layout, graph navigation and owned-series snapshot."""
from pathlib import Path
import subprocess, tempfile, re
from test_web_batch_handlers import extract_block
from test_oled_text_field import text_field_implementation
HERE=Path(__file__).resolve().parent
COMPONENT=HERE.parents[1]
def generate():
    read=lambda name:(COMPONENT/name).read_text()
    header=read('G2_Health.h'); model=read('G2_Health.cpp'); utils=read('OLED_Utils.cpp')
    # Reuse the existing framebuffer/font double; this is a render preview,
    # not a photograph or a second implementation of the layout.
    canvas=(HERE/'oled_layout_harness.cpp').read_text().split('// INSERT_TYPES')[0]
    canvas=canvas.replace('static int canvasHeight = 64;', 'static int canvasHeight = 64, canvasHeader = 10, canvasFooter = 10;').replace('#define OLED_CONTENT_START_Y 11', '#define OLED_CONTENT_START_Y (canvasHeader+1)').replace('#define OLED_CONTENT_HEIGHT (canvasHeight - 21)', '#define OLED_CONTENT_HEIGHT (canvasHeight-canvasHeader-canvasFooter-1)')
    canvas=canvas.replace('if(x+6>128){x=0;y+=8;}', 'if(x+6>128){x=0;y+=8;} assert(y>=OLED_CONTENT_START_Y && y+8<=OLED_CONTENT_START_Y+OLED_CONTENT_HEIGHT);')
    canvas=canvas.replace('  void drawFastVLine(', '''  void drawPixel(int px,int py,int value) {
    assert(px>=0 && px<128 && py>=OLED_CONTENT_START_Y && py<OLED_CONTENT_START_Y+OLED_CONTENT_HEIGHT);
    pixel(px,py,value);
  }
  void drawLine(int x0,int y0,int x1,int y1,int value) {
    const int dx=abs(x1-x0), sx=x0<x1?1:-1, dy=-abs(y1-y0), sy=y0<y1?1:-1;
    int error=dx+dy;
    for(;;){drawPixel(x0,y0,value);if(x0==x1 && y0==y1)break;int e=error*2;if(e>=dy){error+=dy;x0+=sx;}if(e<=dx){error+=dx;y0+=sy;}}
  }
  void drawFastVLine(''')
    types='\n'.join(extract_block(header,x)+';' for x in ('struct G2HealthGraphPoint','struct G2HealthGraphSnapshot','enum G2HealthMetric'))
    types+='\n'+extract_block(read('G2_Ring.h'),'struct G2RingTelemetry')+';'
    types+='\n'+extract_block(model,'struct TrendMeta')+';'
    for x in ('enum G2RingDesiredState','enum G2RingObservedState'):
        types+='\n'+extract_block(read('G2_Ring.h'),x)+';'
    for x in ('struct OLEDScrollItem','struct OLEDScrollState'):
        types+='\n'+extract_block(read('OLED_Utils.h'),x)+';'
    state='''
#include "G2_HealthGraphCore.h"
using HealthSample=hw1_g2_health::Sample;
using HealthSeries=hw1_g2_health::Series<HEALTH_HIST_CAP>;
static HealthSeries sHr{},sHrv{},sSpo2{},sTemp{},sBat{},sTrendHr{},sTrendHrv{},sTrendSpo2{};
static TrendMeta sTrendMetaHr{},sTrendMetaHrv{},sTrendMetaSpo2{};
static uint32_t now=100000;
uint32_t millis(){return now;}
static bool allowLock=true,locked=false;
struct SeriesLock { SeriesLock(){assert(!::locked);::locked=allowLock;} ~SeriesLock(){::locked=false;} bool locked() const {return allowLock;} };
static constexpr uint32_t kAgePlausibleMs=30u*24u*3600u*1000u;
'''
    series='\n'.join(extract_block(model,x) for x in ('static HealthSeries* seriesFor(', 'static HealthSeries* trendSeriesFor(', 'static TrendMeta* trendMetaFor(', 'static size_t seriesOrderedStart(', 'static inline const HealthSample& seriesAt(', 'static inline uint32_t sampleAgeMs(', 'bool g2HealthGraphSnapshot('))
    textfield='#include "oled_text_field_gfx_double.h"\nstatic int currentOLEDMode=0;\n'+text_field_implementation()+'''
struct CanvasGfx : Adafruit_GFX { Adafruit_SSD1306& c; explicit CanvasGfx(Adafruit_SSD1306& d):Adafruit_GFX(128,canvasHeight),c(d){} void drawPixel(int16_t px,int16_t py,uint16_t v) override { c.pixel(px,py,v); } };
void oledTextFieldDraw(Adafruit_SSD1306* d,int x,int y,int w,const char* t,uint16_t fg,uint16_t bg,bool a,const void* o){ if(!d) return; if(t && *t) d->textRows.push_back(y); CanvasGfx g(*d); oledTextFieldDraw(static_cast<Adafruit_GFX*>(&g),x,y,w,t,fg,bg,a,o); }
'''
    lists='''
void oledScrollInit(OLEDScrollState*,const char* =nullptr,int=4);
bool oledScrollAddItem(OLEDScrollState*,const char*,const char* =nullptr,bool=true,void* =nullptr);
void oledScrollRenderSimple(Adafruit_SSD1306*,OLEDScrollState*,bool=true,int=OLED_CONTENT_START_Y);
'''
    lists+='\n'.join(extract_block(utils,x) for x in ('void oledScrollInit(', 'bool oledScrollAddItem(', 'void oledScrollClearKeepSelection(', 'void oledScrollClampSelection(', 'void oledScrollUp(', 'void oledScrollDown(', 'void oledScrollRenderSimple('))
    page=read('OLED_Mode_R1_Health.cpp'); page=page[page.index('enum HealthAction'):page.index('static const OLEDModeEntry')]
    harness=(HERE/'oled_health_harness.cpp').read_text()
    stub='namespace disabled {\n'+extract_block(header,'inline bool g2HealthGraphSnapshot(')+'\n}\n'
    return "\n".join([canvas, re.search(r"static constexpr size_t HEALTH_HIST_CAP = \d+;",header)[0], types, state, series, textfield, lists, harness.replace('// INSERT_PAGE',page), stub])+'''\nint main(int argc,char** argv){runChecks(argc>1?argv[1]:nullptr);G2HealthGraphSnapshot disabledData{};disabledData.count=5;assert(!disabled::g2HealthGraphSnapshot(HEALTH_METRIC_HR,false,disabledData)&&!disabledData.count);}\n'''
def main():
    import argparse
    p=argparse.ArgumentParser();p.add_argument('--preview-dir',type=Path);args=p.parse_args()
    if args.preview_dir:args.preview_dir.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='hw1-oled-health-') as directory:
        unit=Path(directory)/'health.cpp';binary=Path(directory)/'health';unit.write_text(generate())
        subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-I',str(COMPONENT),'-I',str(HERE),'-I',str(COMPONENT.parent/'hardwareone_libs/Adafruit_GFX_Library'),str(unit),'-o',str(binary)],check=True)
        subprocess.run([str(binary)]+([str(args.preview_dir)] if args.preview_dir else []),check=True)
if __name__=='__main__':main()
