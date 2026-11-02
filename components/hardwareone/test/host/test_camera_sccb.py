#!/usr/bin/env python3
"""Exercise the actual SCCB factory bridge at Wire/manager/SDK boundaries."""
import argparse,re,subprocess,tempfile,sys
from pathlib import Path
from test_web_batch_handlers import extract_block
HERE=Path(__file__).resolve().parent
COMPONENT=HERE.parents[1]
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--sanitize',action='store_true')
 p.add_argument('--sccb-component',type=Path,
                default=COMPONENT.parents[1]/'managed_components/espressif__esp_sccb_intf')
 a=p.parse_args()
 interface_path=a.sccb_component/'interface/esp_sccb_io_interface.h'
 if not interface_path.is_file():
  print('SKIP: resolve a P4 camera build first, or pass --sccb-component for the pinned SDK interface')
  return 77
 source=re.sub(r'^#include.*\n','',(COMPONENT/'HAL_Camera_SCCB.cpp').read_text(),flags=re.M)
 header=interface_path.read_text()
 interface=extract_block(header[header.index('struct esp_sccb_io_t {'):],'struct esp_sccb_io_t')+';'
 unit=(HERE/'camera_sccb_harness.cpp').read_text().replace('// INSERT_INTERFACE',interface).replace('// INSERT_PRODUCTION',source)
 with tempfile.TemporaryDirectory(prefix='hw1-camera-sccb-') as t:
  cpp=Path(t)/'test.cpp';exe=Path(t)/'test';cpp.write_text(unit)
  flags=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-variable','-Wno-unneeded-internal-declaration',str(cpp),'-o',str(exe)]
  if a.sanitize:flags[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
  for shared in (0,1):
   subprocess.run(flags+[f'-DCAMERA_SCCB_SHARED={shared}'],check=True);subprocess.run([str(exe)],check=True)
if __name__=='__main__':sys.exit(main())
