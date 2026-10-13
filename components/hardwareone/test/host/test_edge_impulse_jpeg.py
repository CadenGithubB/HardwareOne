#!/usr/bin/env python3
"""Edge Impulse input geometry, labels, shared pipeline and JPEG boundary.

Compiles the whole of System_EdgeImpulse.cpp and its header (with their
#include lines stripped) after edge_impulse_host_fakes.h, which supplies a fake
TFLite Micro interpreter (exact-size tensors with guard bytes), FreeRTOS on
std::thread, in-memory files, the camera capture call and httpd. The real
HAL_JPEG.cpp front end is linked; only the codec backend is faked.

Checks, each reported separately:
  static          one inference pipeline and one Invoke call site, decoding
                  through hwjpeg::decode, the seven unused exports gone, no
                  shared label buffer, no eiinputsize setting;
  decode unit     decodeEdgeImpulseJpeg keeps the VGA cap and rejects bad
                  input before the codec;
  link (HTTP=0)   an ENABLE_EDGE_IMPULSE=1, ENABLE_HTTP_SERVER=0 build links
                  every command-table handler;
  scenarios       one process each: gray96, changed_setting, non_square and
                  fill_types (the fill writes exactly the input tensor),
                  twelve_labels (label table sized by the model, labels owned
                  by each detection), parity (camera and file give the same
                  tensor and results and apply the same checks), web_json
                  (web detect uses the shared JSON builders).
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
JSON_INCLUDE = COMPONENT.parent / 'hardwareone_libs' / 'ArduinoJson' / 'src'
FIXTURES = HERE / 'jpeg_fixtures'

SCENARIOS = ('gray96', 'changed_setting', 'non_square', 'fill_types',
             'twelve_labels', 'parity', 'web_json')
UNUSED_EXPORTS = ('getLastDetectionResults', 'setStateChangeCallback',
                  'getTrackedObjectCount', 'getTrackedObject(', 'buildStateChangeJson',
                  'isModelLoaded', 'getLoadedModelPath')
REGISTER = 'void registerEdgeImpulseHandlers(httpd_handle_t server)'


def strip_includes(text):
    return '\n'.join('' if line.lstrip().startswith('#include') else line
                     for line in text.splitlines()) + '\n'


def module_text(component=COMPONENT):
    """The module header and source, ready to follow edge_impulse_host_fakes.h."""
    header = (component / 'System_EdgeImpulse.h').read_text()
    source = (component / 'System_EdgeImpulse.cpp').read_text()
    # Registration uses C++20 designated initializers for httpd_uri_t; the
    # host build only needs both handlers referenced.
    source = source.replace(extract_block(source, REGISTER), REGISTER + ' {\n'
                            '  (void)server;\n  (void)&handleEIOrganize;\n'
                            '  (void)&handleEdgeImpulseDetect;\n}')
    return strip_includes(header) + strip_includes(source)


def compile_unit(args, workdir, name, text, defines):
    """Compile one harness unit; returns (binary or None, compiler output)."""
    unit, binary = workdir / f'{name}.cpp', workdir / name
    unit.write_text(text)
    command = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
               # Firmware style, not test concerns: CLI handlers ignore their
               # argument string, command rows omit the trailing flag, and
               # "%lu" matches uint32_t on the Xtensa toolchain only.
               '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-Wno-format',
               '-pthread', *defines,
               '-I', str(HERE / 'edge_impulse_stubs'), '-I', str(HERE),
               '-I', str(COMPONENT), '-I', str(JSON_INCLUDE),
               str(unit), str(COMPONENT / 'HAL_JPEG.cpp'), '-o', str(binary)]
    if args.sanitize:
        command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
    result = subprocess.run(command, capture_output=True, text=True)
    return (binary if result.returncode == 0 else None), result.stdout + result.stderr


def run_binary(command, timeout=120):
    try:
        result = subprocess.run([str(c) for c in command], capture_output=True, text=True,
                                timeout=timeout)
    except subprocess.TimeoutExpired:
        return False, f'timed out after {timeout} s'
    return result.returncode == 0, (result.stdout + result.stderr).strip()


def tail(text, lines=12):
    return '\n'.join(text.splitlines()[-lines:])


class Report:
    def __init__(self):
        self.failures = []

    def check(self, name, ok, detail=''):
        print(f'{"PASS" if ok else "FAIL"}: {name}')
        if not ok:
            self.failures.append(name)
            if detail:
                print('    ' + tail(detail).replace('\n', '\n    '))

    def finish(self, suite):
        if self.failures:
            print(f'{suite}: {len(self.failures)} failed: {", ".join(self.failures)}')
            sys.exit(1)
        print(f'{suite} PASS')


def static_checks(report, component=COMPONENT):
    header = (component / 'System_EdgeImpulse.h').read_text()
    source = (component / 'System_EdgeImpulse.cpp').read_text()
    settings = (component / 'System_Settings.cpp').read_text()
    report.check('static: decodes through hwjpeg::decode, not fmt2rgb888',
                 'fmt2rgb888(' not in source and source.count('hwjpeg::decode(') == 1)
    report.check('static: one Invoke call site', source.count('->Invoke()') == 1)
    try:
        bodies = [extract_block(source, marker) for marker in
                  ('static EIResults runCameraInferenceLocked()', 'EIResults runInferenceFromFile(')]
        shared = all(b.count('inferRgb(') == 1 and b.count('decodeEdgeImpulseJpeg(') == 1 and
                     'resizeRgb888(' not in b for b in bodies)
    except ValueError:
        shared = False
    report.check('static: camera and file entry points share inferRgb', shared)
    for name in UNUSED_EXPORTS:
        report.check(f'static: unused export {name.rstrip("(")} removed',
                     name not in header and name not in source)
    report.check('static: no shared static label buffer', 'genericLabel' not in source)
    table = extract_block(source, 'static const SettingEntry edgeImpulseSettingEntries[] =')
    report.check('static: input size is not a setting (no row, no eiinputsize command)',
                 '"inputSize"' not in table and 'eiinputsize' not in settings and
                 'edgeImpulseInputSize' not in source)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cxx', default=shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    report = Report()
    static_checks(report)
    harness = (HERE / 'edge_impulse_jpeg_harness.cpp').read_text()
    source = (COMPONENT / 'System_EdgeImpulse.cpp').read_text()
    with tempfile.TemporaryDirectory(prefix='hw1-edge-jpeg-') as temporary:
        workdir = Path(temporary)

        try:
            decode = (extract_block(source, 'static uint8_t* allocateEdgeImpulseRgb(') + '\n' +
                      extract_block(source, 'static bool decodeEdgeImpulseJpeg('))
        except ValueError as error:
            report.check('decode unit: build', False, f'production boundary missing: {error}')
        else:
            binary, output = compile_unit(args, workdir, 'decode',
                                          harness.replace('// INSERT_EDGE_IMPULSE_DECODE', decode),
                                          ['-DEI_DECODE_UNIT'])
            report.check('decode unit: build', binary is not None, output)
            if binary:
                ok, output = run_binary([binary, FIXTURES])
                report.check('decode unit: VGA cap, rejections before the codec', ok, output)

        module = harness.replace('// INSERT_EDGE_IMPULSE_MODULE', module_text())
        binary, output = compile_unit(args, workdir, 'link', module,
                                      ['-DENABLE_EDGE_IMPULSE=1', '-DENABLE_HTTP_SERVER=0',
                                       '-DEI_LINK_CHECK'])
        report.check('link: EI=1, HTTP=0 build has every command handler', binary is not None, output)
        if binary:
            ok, output = run_binary([binary])
            report.check('link: eistatus runs without the web server', ok, output)

        binary, output = compile_unit(args, workdir, 'module', module,
                                      ['-DENABLE_EDGE_IMPULSE=1', '-DENABLE_HTTP_SERVER=1'])
        report.check('module: build', binary is not None, output)
        for scenario in SCENARIOS:
            if binary:
                ok, output = run_binary([binary, FIXTURES, scenario])
            else:
                ok, output = False, 'module did not build'
            report.check(f'scenario: {scenario}', ok, output)
    report.finish('EDGE_IMPULSE_JPEG')


if __name__ == '__main__':
    main()
