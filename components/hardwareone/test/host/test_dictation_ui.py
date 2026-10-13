#!/usr/bin/env python3
"""Compile actual shared OLED/G2 dictation delivery and G2 service functions.

A second unit (oled_keyboard_mask_harness.cpp) compiles the shipping OLED
virtual keyboard (init, reset, preview, oledKeyboardDisplay) and the Set Pattern
openers against a recording display: a masked (secret) field never shows its
text on any keyboard page, and a plain field keeps its preview. A source
contract checks that every secret field opens the keyboard masked and that
first-time setup no longer rewrites the shared buffer around its render.
`--component` points the suite at another tree.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from test_espsr_runtime import function
from test_web_batch_handlers import extract_block
HERE=Path(__file__).resolve().parent
SOURCE=HERE.parents[1]

# Secret keyboard fields opened inside larger handlers: (file, enclosing
# function, the `masked` argument it must pass). The Set Pattern openers are
# compiled and rendered by the mask unit instead.
SECRET_FIELDS=(
    ('OLED_Mode_Auth.cpp','static bool handleLoginModeInput(','currentField == FIELD_PASSWORD'),
    ('OLED_Mode_ChangePassword.cpp','static bool handleChangePasswordModeInput(','true'),
    ('OLED_Mode_Network.cpp','static void startWifiAddPass(','true'),
    ('OLED_Mode_Network.cpp','static void startWifiAddFromScan(','true'),
    ('OLED_ESPNow.cpp','bool oledEspNowHandleRemoteFormInput(','gOledEspNowState.remoteFormField == 1'),
    ('OLED_ESPNow.cpp','bool oledEspNowHandleSettingsInput(','gOledEspNowState.settingsEditField == 6'),
    ('OLED_FirstTimeSetup.cpp','String getOLEDTextInput(','isPassword'),
)
PLAIN_FIELDS=(('OLED_Mode_Network.cpp','static void startWifiAddSSID('),)

def keyboard_init_args(body):
    """Argument lists of every oledKeyboardInit( call in `body`, comments removed."""
    calls=[]
    for match in re.finditer(r'\boledKeyboardInit\(',body):
        pos=match.end();depth=1;start=pos
        while depth:
            depth+={'(':1,')':-1}.get(body[pos],0);pos+=1
        text=re.sub(r'/\*.*?\*/|//[^\n]*','',body[start:pos-1],flags=re.S)
        args,depth,current=[],0,''
        for char in text:
            if char==',' and depth==0:args.append(current);current='';continue
            depth+={'(':1,')':-1}.get(char,0);current+=char
        args.append(current)
        calls.append([' '.join(arg.split()) for arg in args])
    return calls

def secret_field_failures(component):
    failures=[]
    for name,signature,expected in SECRET_FIELDS:
        calls=keyboard_init_args(extract_block((component/name).read_text(),signature))
        if len(calls)!=1:failures.append(f'{name} {signature}: expected one oledKeyboardInit call, found {len(calls)}');continue
        args=calls[0]
        if len(args)<5:failures.append(f'{name} {signature}: the secret field opens the keyboard unmasked (no masked argument: {", ".join(args)})')
        elif args[4]!=expected:failures.append(f'{name} {signature}: masked argument is `{args[4]}`, expected `{expected}`')
    for name,signature in PLAIN_FIELDS:
        for args in keyboard_init_args(extract_block((component/name).read_text(),signature)):
            if len(args)>4 and args[4] not in ('false',):failures.append(f'{name} {signature}: a plain field passes masked `{args[4]}`')
    fts=extract_block((component/'OLED_FirstTimeSetup.cpp').read_text(),'String getOLEDTextInput(')
    # Debug traces must not print a masked field's text or the character just typed.
    for line in (component/'OLED_Utils.cpp').read_text().splitlines():
        if 'DEBUG_DISPLAYF' in line and re.search(r'gOledKeyboardState\.text(?!\w)',line):
            failures.append('OLED_Utils.cpp: a keyboard debug trace prints gOledKeyboardState.text directly (use oledKeyboardTraceText, which hides a masked field)')
    if re.search(r"gOledKeyboardState\.text\[[^\]]*\]\s*=\s*'\*'",fts):
        failures.append("OLED_FirstTimeSetup.cpp getOLEDTextInput: writes '*' into the shared keyboard buffer around its render (masking belongs to the keyboard)")
    return failures

def mask_unit(component):
    header=(component/'OLED_Utils.h').read_text();utils=(component/'OLED_Utils.cpp').read_text()
    dictation=(component/'System_Dictation.h').read_text();transcript=(component/'System_Transcript.h').read_text()
    pattern=(component/'OLED_Mode_SetPattern.cpp').read_text()
    types='\n'.join([extract_block(transcript,'struct TranscriptStatus')+';',
        extract_block(dictation,'enum class DictationState')+';',extract_block(dictation,'struct DictationSnapshot')+';',
        '\n'.join(re.findall(r'(?m)^#define OLED_KEYBOARD_\w+ .*$',header)),
        extract_block(header,'enum OLEDKeyboardMode')+';',extract_block(header,'enum class OLEDKeyboardDictationPolicy')+';',
        re.search(r'(?m)^typedef int \(\*OLEDKeyboardAutocompleteFunc\)[^;]*;',header)[0],
        extract_block(header,'struct OLEDKeyboardState')+';','extern OLEDKeyboardState gOledKeyboardState;',
        re.search(r'void oledKeyboardInit\([^;]*\);',header)[0],'void oledKeyboardReset();','void oledKeyboardDisplay(Adafruit_SSD1306* display);'])
    start=utils.index('const char OLED_KEYBOARD_CHARS_UPPER[OLED_KEYBOARD_ROWS][OLED_KEYBOARD_COLS] =')
    keyboard=utils[start:utils.index('bool oledKeyboardHandleInput(',start)]
    opener='\n'.join(['static bool sKeyboardActive = false;']+[extract_block(pattern,s) for s in ('static void startPatternKeyboard(','static void startTextKeyboard(')])
    unit=(HERE/'oled_keyboard_mask_harness.cpp').read_text()
    for marker,text in (('// INSERT_TYPES',types),('// INSERT_KEYBOARD',keyboard),('// INSERT_SET_PATTERN',opener)):
        assert unit.count(marker)==1,marker
        unit=unit.replace(marker,text)
    return unit

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--sanitize',action='store_true')
    p.add_argument('--cxx',default=os.environ.get('CXX') or 'clang++')
    p.add_argument('--component',type=Path,default=HERE.parents[1],help='production component tree (default: this checkout)')
    a=p.parse_args();SOURCE=a.component.resolve()
    g2=(SOURCE/'G2_Glasses.cpp').read_text();te=(SOURCE/'G2_Page_TextEntry.cpp').read_text();oled=(SOURCE/'OLED_Utils.cpp').read_text();header=(SOURCE/'System_Dictation.h').read_text()
    unit=(HERE/'dictation_ui_harness.cpp').read_text()
    transcript=(SOURCE/'System_Transcript.h').read_text()
    types=extract_block(transcript,'struct TranscriptStatus')+';\n'+'\n'.join(extract_block(header,s)+';' for s in ('enum class DictationState','struct DictationSnapshot','struct DictationTextReceipt'))+'\n'+extract_block(g2,'struct KbdPadState')+';'
    unit=unit.replace('// INSERT_TYPES',types)
    unit=unit.replace('// INSERT_FIELD','\n'.join(function(te,s) for s in ('size_t g2TextEntryPadRemaining(', 'size_t g2TextEntryPadAppendText(')))
    unit=unit.replace('// INSERT_G2','\n'.join(function(g2,s) for s in ('static bool kbdPadConsumeDictationText(', 'static void kbdPadDictationServiceOnTap(')))
    unit=unit.replace('// INSERT_OLED','\n'.join(function(oled,s) for s in ('static bool oledKeyboardConsumeDictationText(', 'void oledKeyboardDictationTick(')))
    for signature in ('static bool kbdPadConsumeDictationText(', 'static void kbdPadDictationServiceOnTap('):
        assert 'ENABLE_LOCAL_STT' not in function(g2,signature)
    assert 'ENABLE_LOCAL_STT' not in function(oled,'void oledKeyboardDictationTick(')
    # Keyboard fields drain only the keyboard mailbox, which never matches an
    # App lease: neither a Transcription nor a QuickNote can type into a field.
    for text,signature in ((g2,'static bool kbdPadConsumeDictationText('),(g2,'static void kbdPadDictationServiceOnTap('),
                           (oled,'static bool oledKeyboardConsumeDictationText('),(oled,'void oledKeyboardDictationTick(')):
        assert 'dictationApp' not in function(text,signature)
    with tempfile.TemporaryDirectory(prefix='hw1-dictation-ui-') as d:
        cpp=Path(d)/'test.cpp';exe=Path(d)/'test';cpp.write_text(unit)
        cmd=[a.cxx,'-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-variable','-Wno-missing-field-initializers',str(cpp),'-o',str(exe)]
        if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(cmd,check=True);subprocess.run([str(exe)],check=True)
        mask_cpp=Path(d)/'mask.cpp';mask_exe=Path(d)/'mask';mask_cpp.write_text(mask_unit(SOURCE))
        cmd=[a.cxx,'-std=c++17','-Wall','-Wextra','-Werror','-pedantic',str(mask_cpp),'-o',str(mask_exe)]
        if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer']
        subprocess.run(cmd,check=True);masked=subprocess.run([str(mask_exe)])
    failures=secret_field_failures(SOURCE)
    for failure in failures:print('FAIL: '+failure,file=sys.stderr)
    if failures or masked.returncode:
        sys.exit(f'OLED keyboard masking failed ({len(failures)} secret-field contract failure(s), mask unit exit {masked.returncode})')
    print(f'Secret keyboard fields: {len(SECRET_FIELDS)} masked call sites and {len(PLAIN_FIELDS)} plain one checked')
if __name__=='__main__':main()
