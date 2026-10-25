#!/usr/bin/env python3
"""Run production list drawing/navigation and map packing at both canvas heights.

The optional preview uses a graphics test double and the shipping bitmap font;
it is a layout illustration, not a device capture. --baseline compares OLED
framebuffer bytes with saved pre-change sources.
"""
import argparse
import re
from pathlib import Path
import subprocess
import tempfile
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

def run(component, output, baseline=False, sanitize=False):
    source = (component / 'OLED_Utils.cpp').read_text()
    header = (component / 'OLED_Utils.h').read_text()
    harness = (HERE / 'oled_layout_harness.cpp').read_text()
    types = '\n'.join(extract_block(header, 'struct '+name)+';' for name in ('OLEDScrollItem','OLEDScrollState'))
    functions = '\n'.join(extract_block(source, signature) for signature in (
        'void oledScrollInit(', 'bool oledScrollAddItem(', 'void oledScrollClampSelection(',
        'void oledScrollUp(', 'void oledScrollDown(', 'void oledScrollPageUp(',
        'void oledScrollPageDown(', 'void oledScrollSetSplitPane(',
        'void oledScrollRender(', 'void oledScrollRenderSimple('))
    harness = harness.replace('// INSERT_TYPES', types).replace('// INSERT_FUNCTIONS', functions)
    harness = harness.replace('// INSERT_MENU_INIT', extract_block((component/'OLED_Mode_Menu.cpp').read_text(), 'static void mainMenuEnsureInit('))
    # The old packer only handles 64px; compare its OLED output, and test the
    # current packer at both heights in either run.
    harness = harness.replace('// INSERT_MAP_PACK', extract_block((COMPONENT/'OLED_Mode_Map.cpp').read_text(), 'static void packShadeBufTo1Bit('))
    perf = (component/'OLED_Mode_System.cpp').read_text()
    perf_code = '\n'.join(re.findall(r'(?m)^#define PERF_(?:MAX_TASKS|VISIBLE_ROWS|SAMPLE_MS) .+$', perf))
    perf_code += '\n' + extract_block(perf, 'struct PerfRenderData') + ';\n'
    perf_code += 'static PerfRenderData perfRenderData{}; static uint8_t sPerfPage=0; static int sCpuScroll=0,sStkScroll=0;\n'
    perf_code += '\n'.join(extract_block(perf, name) for name in (
        'static void displayPerfStatsRendered(', 'static bool perfStatsInputHandler('))
    harness = harness.replace('// INSERT_PERF', perf_code)
    with tempfile.TemporaryDirectory(prefix='hw1-display-layout-') as tmp:
        unit=Path(tmp)/'layout.cpp';exe=Path(tmp)/'layout'
        unit.write_text(harness)
        cmd=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
             '-I',str(COMPONENT),'-I',str(COMPONENT.parent/'hardwareone_libs/Adafruit_GFX_Library'),str(unit),'-o',str(exe)]
        if baseline: cmd += ['-DBASELINE_LAYOUT']
        if sanitize: cmd += ['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
        subprocess.run(cmd,check=True)
        subprocess.run([str(exe),str(output)] if output else [str(exe)],check=True)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sanitize',action='store_true')
    p.add_argument('--preview-dir',type=Path)
    p.add_argument('--baseline',type=Path)
    args=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='hw1-display-compare-') as tmp:
        current=args.preview_dir or Path(tmp)/'current';current.mkdir(parents=True,exist_ok=True)
        run(COMPONENT,current,sanitize=args.sanitize)
        if args.baseline:
            before=Path(tmp)/'before';before.mkdir()
            run(args.baseline,before,baseline=True,sanitize=args.sanitize)
            for name in ('menu-64.pgm','simple-64.pgm','perf-cpu-64.pgm','perf-stack-64.pgm'):
                assert (current/name).read_bytes()==(before/name).read_bytes(), name+' OLED changed'
            print('OLED menu, simple-list and CPU/stack monitor framebuffers match the pre-change renderers')
if __name__=='__main__': main()
