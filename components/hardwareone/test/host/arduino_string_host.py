"""Host build inputs for the Arduino core String, so tests execute production String code."""
from pathlib import Path
import re
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


def arduino_string_counted(directory, flags=()):
    """arduino_string() with its heap calls counted.

    The copied WString.cpp calls hostStringMalloc/Realloc/Free in place of
    malloc/realloc/free; string_heap_count.h declares the counters a harness
    reads (blocks made, regrowths, frees). Only String's own heap use is
    counted, which is what the heap tests compare.
    """
    d = Path(directory)
    includes, inputs = arduino_string(d, flags)
    (d / 'string_heap_count.h').write_text(
        '#pragma once\n#include <cstddef>\n'
        'struct StringHeapCount { unsigned long made = 0, regrown = 0, freed = 0; };\n'
        'extern StringHeapCount gStringHeap;\n'
        'void* hostStringMalloc(size_t);\nvoid* hostStringRealloc(void*, size_t);\nvoid hostStringFree(void*);\n')
    (d / 'string_heap_count.cpp').write_text(
        '#include "string_heap_count.h"\n#include <cstdlib>\n'
        'StringHeapCount gStringHeap;\n'
        'void* hostStringMalloc(size_t n) { ++gStringHeap.made; return std::malloc(n); }\n'
        'void* hostStringRealloc(void* p, size_t n) {\n'
        '  if (p) ++gStringHeap.regrown; else ++gStringHeap.made;\n'
        '  return std::realloc(p, n);\n}\n'
        'void hostStringFree(void* p) { if (p) ++gStringHeap.freed; std::free(p); }\n')
    source = d / 'WString.cpp'
    text = re.sub(r'\b(malloc|realloc|free)\(',
                  lambda m: 'hostString' + m[1].capitalize() + '(', source.read_text())
    source.write_text('#include "string_heap_count.h"\n' + text)
    return includes, inputs + [str(d / 'string_heap_count.cpp')]
