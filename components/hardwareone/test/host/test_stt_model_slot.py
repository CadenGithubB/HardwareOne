#!/usr/bin/env python3
"""The speech model's boot-lifetime weights block (stt/stt_model_slot.h) and how the firmware uses it.

Compiles stt_model_slot_harness.cpp, which drives the real header against a simulated TLSF-style PSRAM
heap: a control replays the field failure of the old per-session load/free lifecycle ("No contiguous PSRAM
block for speech model" after one or two sessions), and the slot runs the same sessions with one load and
no second block. Then pins the wiring the harness cannot reach (citrinet_runtime.cpp needs ESP-DL): the
cache gives the slot back instead of freeing it, the admission asks for one free block only before the
block exists, the refusal says what to do, boot reserves the block before other features allocate, and
calls longer than the trimmed image's 10 s model are cut into pieces.
It does not exercise ESP-IDF's heap, the SD card, ESP-DL or the device.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]


def function_body(text, signature):
    start = text.index(signature)
    depth = 0
    for i in range(text.index('{', start), len(text)):
        depth += {'{': 1, '}': -1}.get(text[i], 0)
        if depth == 0:
            return text[start:i + 1]
    raise AssertionError(signature)


def check_sources():
    runtime = (SOURCE / 'stt/citrinet_runtime.cpp').read_text()
    reset = function_body(runtime, 'void ModelCache::reset()')
    assert 'giveBack(this)' in reset and 'free' not in reset and 'delete' not in reset, reset
    drop = function_body(runtime, 'void ModelCache::dropResident()')
    assert 'clear_arena' in drop and 'delete' not in drop, drop
    # The weights are never a per-call or per-session allocation any more.
    assert 'Buffer candidate' not in runtime and 'heap_caps_free(raw_)' not in runtime
    frees = re.findall(r'heap_caps_free\((\w+)\)', runtime)
    assert sorted(frees) == ['mem', 'p', 'p'], ('Buffer, slotRelease and the vocabulary failure path only', frees)
    assert 'heap_caps_realloc(cache->raw_' not in runtime and 'gSlot.trim(kSlotHeap)' in runtime
    # Admission: one free block of the model's size only while no block is reserved.
    assert runtime.count('(!newBlock || heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)>=identity::kRawBytes)') == 2
    assert 'No contiguous PSRAM block for speech model' not in runtime
    message = re.search(r'kNoModelBlock\[\]="([^"]+)"', runtime).group(1)
    assert 'Restart the device' in message and len(message) + 1 <= 96, message
    assert runtime.count('kNoModelBlock') >= 3
    # The resident model is built once per boot and kept with the weights.
    assert 'gResident=cache->resident_;' in runtime
    assert 'weights.resident_=gResident' in runtime

    local = (SOURCE / 'System_STTLocal.cpp').read_text()
    assert 'hw1::stt::singleCallSamples()' in local and 'hw1::stt::splitSpeech(' in local
    reserve_model = function_body(local, 'void sttLocalReserveModel() {\n#if HW1_STT_CITRINET')
    # No block is held all boot for a model the card does not carry: the availability check comes first.
    assert reserve_model.index('sttLocalAvailable(') < reserve_model.index('(void)hw1::stt::reserveModel();'), \
        reserve_model
    assert 'reason=model' in reserve_model
    assert local.count('void sttLocalReserveModel()') == 2, 'P4 and stub builds'

    boot = (SOURCE / 'HardwareOne.cpp').read_text()
    setup = boot[boot.index('void hardwareone_setup()'):]
    reserve = setup.index('sttLocalReserveModel();')
    assert setup.index('initFilesystem()') < reserve < setup.index('sttVocabularyInit();'), \
        'reserve right after the filesystem, before anything else of STT allocates'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true', help='AddressSanitizer and UBSan')
    parser.add_argument('--cxx', default='clang++')
    args = parser.parse_args()
    check_sources()
    with tempfile.TemporaryDirectory(prefix='hw1-stt-model-slot-') as tmp:
        exe = Path(tmp) / 'stt_model_slot'
        cmd = [args.cxx, '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', '-pedantic', f'-I{SOURCE}',
               str(HERE / 'stt_model_slot_harness.cpp'), '-o', str(exe)]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True, timeout=180)
        subprocess.run([str(exe)], check=True, timeout=120)


if __name__ == '__main__':
    main()
