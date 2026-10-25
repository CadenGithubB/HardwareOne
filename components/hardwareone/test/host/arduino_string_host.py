"""Host build inputs for the Arduino core String, so tests execute production String code."""
from pathlib import Path
import subprocess

CORE = Path(__file__).resolve().parents[3] / 'arduino/cores/esp32'


def arduino_string(directory, flags=()):
    """Write shims into `directory`; return (compiler include args, extra link inputs)."""
    d = Path(directory)
    (d / 'Arduino.h').write_text('#pragma once\n#include <WString.h>\n')
    (d / 'esp_heap_caps.h').write_text('#pragma once\n#include <cstddef>\n#define MALLOC_CAP_INTERNAL 1\n'
                                       '#define MALLOC_CAP_8BIT 2\nvoid* heap_caps_malloc(size_t,unsigned);\n')
    string = (CORE / 'WString.cpp').read_text().replace('#include "Arduino.h"', '#include <cstdio>').replace(
        '#include "esp32-hal-log.h"', '#define log_e(...) ((void)0)\n#define log_w(...) ((void)0)')
    (d / 'WString.cpp').write_text(string)
    conversions = (CORE / 'stdlib_noniso.c').read_text().replace('#include "esp_system.h"', '') + (
        '\nchar *itoa(int v,char*s,int b){return ltoa(v,s,b);}\n'
        'char *utoa(unsigned int v,char*s,int b){return ultoa(v,s,b);}\n')
    (d / 'convert.c').write_text(conversions)
    subprocess.run(['cc', '-std=c11', *flags, '-I', str(CORE), '-c', str(d / 'convert.c'),
                    '-o', str(d / 'convert.o')], check=True)
    return ['-I', str(d), '-I', str(CORE)], [str(d / 'WString.cpp'), str(d / 'convert.o')]
