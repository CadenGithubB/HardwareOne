#!/usr/bin/env python3
"""Pin the three resident stacks raised on 2026-11-08 to a floor.

cmd_exec, log_out and g2_owner ran with 312 to 700 B free on the P4X soaks of
2026-11-04/05. Their sizes now leave at least 1.5 KB plus a margin over the
worst recorded use. This test fails if a size shrinks below that floor, if a
creation site or a stack reporter stops using the shared constant, or if one of
the stacks moves to a caps-based (PSRAM-capable) create or gains a second,
board-specific create: all three can reach a flash operation, which needs an
internal stack.
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
FLOOR_FREE = 1536     # target: at least 1.5 KB free
MARGIN = 768          # smallest margin over the floor that any of the three keeps
PINNED = {'cmd_exec': 10240, 'log_out': 6144, 'g2_owner': 8192}

for task, (old_size, worst_free) in RECORDED.items():
    used = old_size - worst_free
    left = sizes[task] - used
    check(sizes[task] >= PINNED[task],
          f'{task}: stack {sizes[task]} B is below the pinned floor {PINNED[task]} B')
    check(left >= FLOOR_FREE + MARGIN,
          f'{task}: {left} B free at the recorded peak; need {FLOOR_FREE} + {MARGIN} B')

# Every firmware source in the component (tests and third-party code excluded),
# so a second definition, a macro override or a second create anywhere is seen.
SKIP = {'test', 'third_party'}
SOURCES = {
    p.relative_to(COMPONENT).as_posix(): p.read_text(errors='replace')
    for p in sorted(COMPONENT.rglob('*'))
    if p.is_file() and p.suffix in ('.c', '.cpp', '.h', '.inc')
    and not SKIP.intersection(p.relative_to(COMPONENT).parts)
}

# No per-board variation: each constant has exactly one literal definition in
# the whole component and is never redefined or #undef'd by a macro, and the
# cmd_exec creation variable is assigned once.
for name, home in (('CMD_EXEC_STACK_WORDS', 'System_TaskUtils.h'),
                   ('DEBUG_OUT_STACK_WORDS', 'System_TaskUtils.h'),
                   ('G2_CONTROL_STACK_BYTES', 'G2_Glasses.cpp')):
    defs = [f for f, text in SOURCES.items() for _ in re.findall(r'\b' + name + r'\s*=', text)]
    check(defs == [home], f'{name}: expected one definition in {home}, found {defs}')
    for f, text in SOURCES.items():
        check(not re.search(r'#\s*(define|undef)\s+' + name + r'\b', text),
              f'{f}: {name} is overridden by a macro; the size must not vary per board')
assigns = [f for f, text in SOURCES.items() for _ in re.findall(r'\bcmdExecStackWords\s*=', text)]
check(assigns == ['HardwareOne.cpp'], f'cmdExecStackWords assigned in {assigns}; expected once')

# Creation sites pass the constant through plain internal-RAM creates, and the
# size registry (taskstats json "stackSize") gets the same value.
check(re.search(r'const uint32_t cmdExecStackWords = CMD_EXEC_STACK_WORDS;', main),
      'cmd_exec: creation size is not CMD_EXEC_STACK_WORDS')
check('constexpr uint32_t stackBytes = CMD_EXEC_STACK_WORDS;' in main,
      'cmd_exec: its own periodic stack check does not use CMD_EXEC_STACK_WORDS')
check('const uint32_t needApprox = G2_CONTROL_STACK_BYTES + 1200u;' in g2,
      'g2_owner: start gate does not follow G2_CONTROL_STACK_BYTES')
check('? G2_CONTROL_STACK_BYTES - freeNow : 0;' in g2,
      'g2_owner: stack-peak probe does not use G2_CONTROL_STACK_BYTES')

# Each task name may appear only at its known places, each exactly once: the
# one plain create with the shared constant, its size-registry entry and the
# known-task table rows. Any other "name" literal -- a second create under a
# board #if, a caps-based (PSRAM-stack) create, a record with another size --
# fails here. All three tasks can reach a flash operation, which needs an
# internal stack.
ROW = r'\{\s*"cmd_exec",\s*\w+,\s*CMD_EXEC_STACK_WORDS\s*\}'
ALLOWED = {
    'cmd_exec': [
        ('HardwareOne.cpp', r'xTaskCreateLogged\(commandExecTask, "cmd_exec", cmdExecStackWords,',
         'created through xTaskCreateLogged with cmdExecStackWords'),
        ('System_MemoryMonitor.cpp', ROW, 'known-task row uses CMD_EXEC_STACK_WORDS'),
        ('System_TaskUtils.cpp', ROW, 'known-task row uses CMD_EXEC_STACK_WORDS'),
    ],
    'log_out': [
        ('System_Debug.cpp', r'taskStackRecord\("log_out", DEBUG_OUT_STACK_WORDS\);',
         'size registry entry uses DEBUG_OUT_STACK_WORDS'),
        ('System_Debug.cpp', r'xTaskCreatePinnedToCore\(\s*debugOutputTask,\s*"log_out",\s*DEBUG_OUT_STACK_WORDS,',
         'created with xTaskCreatePinnedToCore and DEBUG_OUT_STACK_WORDS'),
    ],
    'g2_owner': [
        ('G2_Glasses.cpp', r'xTaskCreateLogged\(heartbeatWorkerTask, "g2_owner",\s*/\*stack bytes\*/ G2_CONTROL_STACK_BYTES,',
         'created through xTaskCreateLogged with G2_CONTROL_STACK_BYTES'),
    ],
}
for task, places in ALLOWED.items():
    rest = dict(SOURCES)
    for f, pattern, what in places:
        n = len(re.findall(pattern, rest.get(f, '')))
        check(n == 1, f'{task}: expected exactly one match in {f} ({what}), found {n}')
        rest[f] = re.sub(pattern, '', rest.get(f, ''), count=1)
    for f, text in rest.items():
        check('"' + task + '"' not in text,
              f'{f}: "{task}" appears outside its one create, size record and table rows '
              '(a second or caps-based create?)')

# Belt and braces: no caps or static create of the three, under any spelling
# (xTaskCreateWithCaps, xTaskCreatePinnedToCoreWithCaps, xTaskCreateStatic...).
for f, text in SOURCES.items():
    for task in ('cmd_exec', 'log_out', 'g2_owner'):
        check(not re.search(r'xTaskCreate\w*(WithCaps|Static)\w*\s*\([^;]*"' + task + '"', text),
              f'{f}: {task} created with a caps/static helper; it needs an internal stack')

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

# Classic ESP32 post-admission term: g2_owner starts after the "g2client" admission, so
# the term's "left after start" grows with that stack (20 KB at the old 6 KB).
# The lifecycle harness pins the same figure.
def classic_left_after(text, pattern, where):
    m = re.search(pattern, text)
    check(m, f'{where}: classic g2client "left after" term not found')
    return sum(int(x) for x in re.findall(r'\d+', m.group(1))) if m else 0

prod_left = classic_left_after(monitor, r'\{ "g2client", ([\d\s+]+), 10240 \+ 16384 \}', 'System_MemoryMonitor.cpp')
harness = (HERE / 'g2_client_lifecycle_harness.cpp').read_text()
pin_left = classic_left_after(harness, r'constexpr size_t kClassicLeftAfter = ([\d\s+]+), kClassicLargest = 10240 \+ 16384;',
                              'g2_client_lifecycle_harness.cpp')
check(prod_left >= 20480 + (sizes['g2_owner'] - 6144),
      f'classic post-admission term leaves {prod_left} B after start; need 20480 + the g2_owner growth over 6144')
check(prod_left == pin_left, f'classic post-admission term {prod_left} B differs from the harness pin {pin_left} B')

if FAILURES:
    for f in FAILURES:
        print('FAIL:', f)
    sys.exit(1)
print('resident stack floors: ' + ', '.join(f'{k}={v} B' for k, v in sizes.items()) + ' -- ok')
