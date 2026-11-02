#!/usr/bin/env python3
"""Run the production monochrome renderer and OLED preview lifecycle."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from test_web_batch_handlers import extract_block

HERE=Path(__file__).resolve().parent
COMPONENT=HERE.parents[1]
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize',action='store_true')
    args=parser.parse_args()
    flags=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function',
           '-Wno-unused-variable','-Wno-missing-field-initializers','-DHW_CAMERA_HAL_HOST_TEST=1','-I',str(COMPONENT),
           '-I',str(COMPONENT.parent/'hardwareone_libs/ArduinoJson/src')]
    if args.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    source=re.sub(r'^#include.*\n','',(COMPONENT/'OLED_Mode_Camera.cpp').read_text(),flags=re.M)
    oled=(COMPONENT/'OLED_Display.h').read_text();camera=(COMPONENT/'System_Camera_DVP.h').read_text()
    unit=(HERE/'oled_camera_harness.cpp').read_text()
    for marker,value in [('// INSERT_CAMERA_SNAPSHOT',extract_block(camera,'struct CameraStateSnapshot')+';'),
                         ('// INSERT_OLED_ENUM',extract_block(oled,'enum OLEDMode')+';'),
                         ('// INSERT_OLED_ENTRY',extract_block(oled,'struct OLEDModeEntry')+';'),
                         ('// INSERT_PRODUCTION',source)]:
        assert unit.count(marker)==1,marker
        unit=unit.replace(marker,value)
    with tempfile.TemporaryDirectory(prefix='hw1-oled-camera-') as directory:
        work=Path(directory);generated=work/'test.cpp';generated.write_text(unit)
        exe=work/'test'
        subprocess.run([*flags,str(HERE/'test_camera_monochrome.cpp'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
        for height in [64,88,128]:
            subprocess.run([*flags,f'-DTEST_HEIGHT={height}',str(generated),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
        header=re.sub(r'^#(?:include|pragma).*\n','',(COMPONENT/'OLED_Mode_Camera.h').read_text(),flags=re.M)
        for display,camera in [(0,1),(1,0),(0,0)]:
            disabled='#include <string>\n#include <cassert>\nusing String=std::string;\n'
            disabled+=f'#define ENABLE_OLED_DISPLAY {display}\n#define ENABLE_CAMERA_SENSOR {camera}\n'
            disabled+=header+'\n'+source+'\nint main(){prepareCameraPreviewData();resetOLEDCameraPreview();assert(String(cmd_oledcameraread(\"\")).find(\"not built\")!=String::npos);}\n'
            generated.write_text(disabled)
            subprocess.run([*flags,str(generated),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
if __name__=='__main__':main()
