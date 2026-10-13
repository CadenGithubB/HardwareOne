#!/usr/bin/env python3
"""Edge Impulse inference lock and continuous-task lifecycle.

Compiles the whole of System_EdgeImpulse.cpp and its header (with their
#include lines stripped) after edge_impulse_host_fakes.h: the continuous task
runs on a std::thread, FreeRTOS semaphores are std::mutex/condition variables,
and the fake interpreter can hold one Invoke open until the scenario releases
it. Each scenario runs in its own process:

  stop_start                `eicontinuous 0` then `1` inside one interval
                            leaves exactly one task; stop joins the task;
  enable_off_during_invoke  `eienable 0` while the continuous task is inside
                            Invoke waits for the frame before freeing;
  load_during_invoke        a model load during a continuous frame waits for
                            it, then continuous mode resumes on the new model;
  detect_during_continuous  camera, CLI, file and web detect report busy while
                            a frame runs and run no Invoke; between frames a
                            detect runs;
  unload_during_detect      an unload waits for a single detect's Invoke.

Not modelled: FreeRTOS priorities, preemption or core affinity, real TFLite
kernels, the camera driver.
"""
import argparse
import shutil
import sys
import tempfile
from pathlib import Path

from test_edge_impulse_jpeg import HERE, FIXTURES, Report, compile_unit, module_text, run_binary

SCENARIOS = ('stop_start', 'enable_off_during_invoke', 'load_during_invoke',
             'detect_during_continuous', 'unload_during_detect')


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cxx', default=shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    report = Report()
    harness = (HERE / 'edge_impulse_lifecycle_harness.cpp').read_text()
    module = harness.replace('// INSERT_EDGE_IMPULSE_MODULE', module_text())
    with tempfile.TemporaryDirectory(prefix='hw1-edge-life-') as temporary:
        binary, output = compile_unit(args, Path(temporary), 'lifecycle', module,
                                      ['-DENABLE_EDGE_IMPULSE=1', '-DENABLE_HTTP_SERVER=1'])
        report.check('lifecycle: build', binary is not None, output)
        for scenario in SCENARIOS:
            if binary:
                ok, output = run_binary([binary, FIXTURES, scenario], timeout=60)
            else:
                ok, output = False, 'harness did not build'
            report.check(f'scenario: {scenario}', ok, output)
    report.finish('EDGE_IMPULSE_LIFECYCLE')


if __name__ == '__main__':
    main()
