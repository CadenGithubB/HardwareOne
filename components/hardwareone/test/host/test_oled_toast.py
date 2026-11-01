#!/usr/bin/env python3
"""Execute shipping toast lifetime/rendering with controlled graphics and time."""
from pathlib import Path
import subprocess,tempfile
from test_web_batch_handlers import extract_block
HERE=Path(__file__).resolve().parent
COMPONENT=HERE.parents[1]
def main():
    source=(COMPONENT/'OLED_UI.cpp').read_text()
    header=(COMPONENT/'OLED_UI.h').read_text()
    methods='\n'.join(extract_block(source,m) for m in ('void oledToastShow(', 'void oledToastClear()', 'bool oledToastActive()', 'void oledToastRender()'))
    types=extract_block(header,'enum class OledUIIcon')+';\n'+extract_block(header,'struct OledToast')+';'
    harness=(HERE/'oled_toast_harness.cpp').read_text().replace('// INSERT_TYPES',types).replace('// INSERT_METHODS',methods)
    assert 'oledToastActive()' in extract_block((COMPONENT/'OLED_Utils.cpp').read_text(),'bool oledIsDirty()')
    with tempfile.TemporaryDirectory(prefix='hw1-oled-toast-') as tmp:
        unit=Path(tmp)/'checks.cpp';unit.write_text(harness)
        for height in (64,88,128):
            binary=Path(tmp)/f'checks-{height}'
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                            '-I',str(COMPONENT),f'-DSCREEN_HEIGHT={height}',str(unit),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
    print('Toast: 64/88/128px wrapping, bounds, error style, expiry, clock wrap and snapshot lifetime passed')
if __name__=='__main__':main()
