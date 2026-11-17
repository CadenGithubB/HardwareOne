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
    methods=source[source.index('static constexpr unsigned kToastQueueCapacity'):source.index('// Event policy remains')]
    methods+='\n'+extract_block(source, 'void oledEventBannerShow(')+'\n'+extract_block(source, 'void oledNotificationBannerShow(')+'\n'+extract_block(source, 'void oledUiRender()')
    types=extract_block(header,'enum class OledUiIcon')+';\n'+extract_block(header,'struct OledToast')+';'
    harness=(HERE/'oled_toast_harness.cpp').read_text().replace('// INSERT_TYPES',types).replace('// INSERT_METHODS',methods)
    assert 'oledToastActive()' in extract_block((COMPONENT/'OLED_Utils.cpp').read_text(),'bool oledIsDirty()')
    utils=(COMPONENT/'OLED_Utils.cpp').read_text()
    assert 'oledFeedbackSetPaused(oledConfirmIsActive())' in extract_block(utils,'bool oledIsDirty()')
    assert 'oledToastClear()' in extract_block(utils,'static void oledResetLocalDisplaySessionTransients()')
    for name in ('OLED_Mode_Auth.cpp','OLED_Mode_ChangePassword.cpp','OLED_SettingsEditor.cpp'):
        assert 'errorDisplayUntil' not in (COMPONENT/name).read_text()

    with tempfile.TemporaryDirectory(prefix='hw1-oled-toast-') as tmp:
        unit=Path(tmp)/'checks.cpp';unit.write_text(harness)
        for height in (64,88,128):
            binary=Path(tmp)/f'checks-{height}'
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                            '-I',str(COMPONENT),f'-DSCREEN_HEIGHT={height}',str(unit),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
    print('Toast: 64/88/128px wrapping, bounds, error style, expiry, queue overflow/coalescing, confirmation pause, session clearing and clock wrap passed')
if __name__=='__main__':main()
