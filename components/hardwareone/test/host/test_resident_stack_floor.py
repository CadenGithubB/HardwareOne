#!/usr/bin/env python3
"""Pin the three resident stacks raised for 1.0 (readiness P1-8) to a floor.

cmd_exec, log_out and g2_owner ran with 312 to 700 B free on the P4X soaks of
2026-11-04/05. Their sizes now leave at least 1.5 KB plus a margin over the
worst recorded use. This test fails if a size shrinks below that floor, if a
creation site or a stack reporter stops using the shared constant, or if one of
the stacks moves to a caps-based (PSRAM-capable) create: all three can reach a
flash operation, which needs an internal stack.
"""
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)


def read(name):
    return (COMPONENT / name).read_text()


def one_constant(text, name, where):
    found = re.findall(r'constexpr uint32_t ' + name + r' = (\d+);', text)
    check(len(found) == 1, f'{where}: expected one literal definition of {name}, found {found}')
    return int(found[0]) if found else 0


header = read('System_TaskUtils.h')
g2 = read('G2_Glasses.cpp')
debug = read('System_Debug.cpp')
main = read('HardwareOne.cpp')
monitor = read('System_MemoryMonitor.cpp')
taskutils = read('System_TaskUtils.cpp')
utils = read('System_Utils.cpp')
registry = read('System_FeatureRegistry.cpp')

sizes = {
    'cmd_exec': one_constant(header, 'CMD_EXEC_STACK_WORDS', 'System_TaskUtils.h'),
    'log_out': one_constant(header, 'DEBUG_OUT_STACK_WORDS', 'System_TaskUtils.h'),
    'g2_owner': one_constant(g2, 'G2_CONTROL_STACK_BYTES', 'G2_Glasses.cpp'),
}

# Worst recorded high-water marks (P4X, 2026-11-04/05): (size then, bytes free).
RECORDED = {'cmd_exec': (8192, 700), 'log_out': (4096, 524), 'g2_owner': (6144, 312)}
FLOOR_FREE = 1536     # readiness target: at least 1.5 KB free
MARGIN = 768          # smallest margin over the floor that any of the three keeps
PINNED = {'cmd_exec': 10240, 'log_out': 6144, 'g2_owner': 8192}

for task, (old_size, worst_free) in RECORDED.items():
    used = old_size - worst_free
    left = sizes[task] - used
    check(sizes[task] >= PINNED[task],
          f'{task}: stack {sizes[task]} B is below the pinned floor {PINNED[task]} B')
    check(left >= FLOOR_FREE + MARGIN,
          f'{task}: {left} B free at the recorded peak; need {FLOOR_FREE} + {MARGIN} B')

# No per-board variation: the constants are plain literals outside any #if.
for name, text in (('CMD_EXEC_STACK_WORDS', header), ('DEBUG_OUT_STACK_WORDS', header),
                   ('G2_CONTROL_STACK_BYTES', g2)):
    check(len(re.findall(r'\b' + name + r'\s*=', text)) == 1, f'{name} is assigned more than once')

# Creation sites pass the constant through plain internal-RAM creates, and the
# size registry (taskstats json "stackSize") gets the same value.
check(re.search(r'const uint32_t cmdExecStackWords = CMD_EXEC_STACK_WORDS;', main),
      'cmd_exec: creation size is not CMD_EXEC_STACK_WORDS')
check(re.search(r'xTaskCreateLogged\(commandExecTask, "cmd_exec", cmdExecStackWords,', main),
      'cmd_exec: not created through xTaskCreateLogged with cmdExecStackWords')
check('constexpr uint32_t stackBytes = CMD_EXEC_STACK_WORDS;' in main,
      'cmd_exec: its own periodic stack check does not use CMD_EXEC_STACK_WORDS')

check('taskStackRecord("log_out", DEBUG_OUT_STACK_WORDS);' in debug,
      'log_out: size registry entry does not use DEBUG_OUT_STACK_WORDS')
check(re.search(r'xTaskCreatePinnedToCore\(\s*debugOutputTask,\s*"log_out",\s*DEBUG_OUT_STACK_WORDS,', debug),
      'log_out: not created with xTaskCreatePinnedToCore and DEBUG_OUT_STACK_WORDS')

check(re.search(r'xTaskCreateLogged\(heartbeatWorkerTask, "g2_owner",\s*/\*stack bytes\*/ G2_CONTROL_STACK_BYTES,', g2),
      'g2_owner: not created through xTaskCreateLogged with G2_CONTROL_STACK_BYTES')
check('const uint32_t needApprox = G2_CONTROL_STACK_BYTES + 1200u;' in g2,
      'g2_owner: start gate does not follow G2_CONTROL_STACK_BYTES')
check('? G2_CONTROL_STACK_BYTES - freeNow : 0;' in g2,
      'g2_owner: stack-peak probe does not use G2_CONTROL_STACK_BYTES')

for text, where in ((main, 'HardwareOne.cpp'), (debug, 'System_Debug.cpp'), (g2, 'G2_Glasses.cpp')):
    for task in ('cmd_exec', 'log_out', 'g2_owner'):
        check(not re.search(r'xTaskCreate(WithCaps|Static|StaticPinnedToCore)\w*\([^;]*"' + task + '"', text),
              f'{where}: {task} created with a caps/static helper; it needs an internal stack')

# xTaskCreateLogged records the size before it creates; taskstats json reads it.
check(re.search(r'taskStackRecord\(pcName, usStackDepth\);\s*\n\s*\n\s*BaseType_t res = xTaskCreatePinnedToCore\(', taskutils),
      'xTaskCreateLogged no longer records the stack size before creating')
check('uint32_t stackTotal = taskStackLookup(taskArray[i].pcTaskName);' in utils
      and 't["stackSize"] = (unsigned)stackTotal;' in utils,
      'taskstats json no longer reports stackSize from the size registry')

# Known-task tables: every row names its size by constant, never a literal, and
# the cmd_exec rows use CMD_EXEC_STACK_WORDS.
for text, where in ((monitor, 'System_MemoryMonitor.cpp'), (taskutils, 'System_TaskUtils.cpp')):
    rows = re.findall(r'\{\s*("[a-z_]+"|INPUT_TASK_NAME),\s*\w+,\s*(\w+)\s*\}', text)
    check(rows, f'{where}: known-task table not found')
    for name, size in rows:
        check(not size.isdigit(), f'{where}: table row {name} uses a literal stack size {size}')
    cmd = [size for name, size in rows if name == '"cmd_exec"']
    check(cmd == ['CMD_EXEC_STACK_WORDS'] * len(cmd) and cmd,
          f'{where}: cmd_exec row(s) {cmd} do not use CMD_EXEC_STACK_WORDS')
    for name, size in rows:
        if name == '"log_out"':
            check(size == 'DEBUG_OUT_STACK_WORDS', f'{where}: log_out row uses {size}')

# The feature registry's G2 heap estimate covers the owner stack plus ~2 KB.
m = re.search(r'\{ "g2", "G2 Glasses", FEATURE_CAT_DISPLAY, (\d+),', registry)
check(m and int(m.group(1)) * 1024 >= sizes['g2_owner'] + 2048,
      'feature registry: G2 heap estimate is below the g2_owner stack + 2 KB')

if FAILURES:
    for f in FAILURES:
        print('FAIL:', f)
    sys.exit(1)
print('resident stack floors: ' + ', '.join(f'{k}={v} B' for k, v in sizes.items()) + ' -- ok')
