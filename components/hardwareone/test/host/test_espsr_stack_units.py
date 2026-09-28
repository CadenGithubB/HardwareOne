#!/usr/bin/env python3
"""Verify SR status and CLI report ESP-IDF byte units, not vanilla FreeRTOS words."""
from pathlib import Path
import re
import subprocess
import tempfile
from test_espsr_runtime import function

HERE = Path(__file__).resolve().parent
source = (HERE.parents[1]/'System_ESPSR.cpp').read_text()
config = (HERE.parents[1]/'System_TaskUtils.h').read_text()
stack_bytes = int(re.search(r'constexpr uint32_t SR_STACK_WORDS = (\d+)', config).group(1))
a = source.index('  // ESP-IDF task stack depths and high-water marks are bytes')
b = source.index('  serializeJson(doc, output);', a)
status = 'void stackStatus(std::map<std::string, Field>& doc) {\n' + source[a:b] + '}\n'
cli = function(source, 'static const char* cmd_sr_stack(')
unit = r"""
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <type_traits>
using String=std::string;
using UBaseType_t=uint32_t;
#define RETURN_VALID_IF_VALIDATE_CSTR() ((void)0)
static void* gSRTaskHandle=reinterpret_cast<void*>(1);
static bool gESPSRRunning=true;
static std::atomic<uint32_t> gSRStackHwm{6144};
struct Field {
 uint32_t number=0;
 template<class T, std::enable_if_t<std::is_arithmetic<T>::value, int> = 0>
 Field& operator=(T n) {number=static_cast<uint32_t>(n);return *this;}
 Field& operator=(const char*) {return *this;}
};
""" + f'constexpr uint32_t SR_STACK_WORDS={stack_bytes};\n' + status + cli + r"""
int main() {
 std::map<std::string, Field> doc;
 stackStatus(doc);
 assert(doc["srTaskStackAllocBytes"].number==8192);
 assert(doc["srTaskStackAllocWords"].number==2048);
 assert(doc["srTaskStackHwmBytes"].number==6144);
 assert(doc["srTaskStackHwmWords"].number==1536);
 assert(doc["srTaskStackPeakUsedBytesEst"].number==2048);
 String result=cmd_sr_stack("");
 assert(result.find("alloc=8192B")!=String::npos);
 assert(result.find("est_peak_used=2048B")!=String::npos);
 assert(result.find("hwm_free=6144B")!=String::npos);
 gSRStackHwm=9000;stackStatus(doc);assert(doc["srTaskStackPeakUsedBytesEst"].number==0);
 gESPSRRunning=false;stackStatus(doc);assert(doc["srTaskStackHwmBytes"].number==0);
 assert(String(cmd_sr_stack("")).find("not running")!=String::npos);
 puts("ESP-SR status/CLI stack byte-unit tests passed");
}
"""
with tempfile.TemporaryDirectory(prefix='hw1-sr-stack-') as tmp:
    cpp=Path(tmp)/'test.cpp';binary=Path(tmp)/'test';cpp.write_text(unit)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
