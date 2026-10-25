#!/usr/bin/env python3
"""Run the phone link v2 pairing code (BLE_PhonePairCode.h) as shipped.

The harness proves the GF(2^5) field (x^5+x^2+1 primitive), the check symbol,
that every single-symbol substitution and every swap of two different symbols
of 300 generated codes is rejected (exhaustive over positions and values),
entry normalisation (outer whitespace, case, '-' and ' ' anywhere, I/L -> 1,
O -> 0, U and any other byte refused, exactly 16 symbols), generation from
three random words and the display form. This driver recomputes the printed
vectors with its own GF arithmetic, so a wrong modulus, weight or alphabet
fails here even though the error-detection properties would still hold. The
text check keeps the header pure.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
ALPHABET = '0123456789ABCDEFGHJKMNPQRSTVWXYZ'


def gf_mul(a: int, b: int) -> int:
    result = 0
    for bit in range(5):
        if b >> bit & 1:
            result ^= a << bit
    for bit in range(8, 4, -1):  # reduce by x^5 + x^2 + 1
        if result >> bit & 1:
            result ^= 0b100101 << (bit - 5)
    return result


def check_value(values: list[int]) -> int:
    check = 0
    for index, value in enumerate(values):
        weight = 1
        for _ in range(index + 1):
            weight = gf_mul(weight, 2)
        check ^= gf_mul(weight, value)
    return check


def check_sources(component: Path) -> None:
    header = (component / 'BLE_PhonePairCode.h').read_text()
    includes = re.findall(r'^#include\s+(\S+)', header, re.MULTILINE)
    require(includes == ['<cstddef>', '<cstdint>'], f'the code stays pure (no BLE, OS or Arduino): {includes}')
    code = re.sub(r'//[^\n]*', '', header)
    require(not re.search(r'\bnew\b|malloc|std::string|std::vector|esp_|Arduino', code),
            'the code allocates nothing and calls no SDK')
    require(f'kAlphabet[] = "{ALPHABET}"' in header, 'the alphabet')


def check_vectors(output: str) -> int:
    count = 0
    for line in output.splitlines():
        if not line.startswith('VEC '):
            continue
        _, hex_values, canonical, shown = line.split()
        values = list(bytes.fromhex(hex_values))
        require(len(values) == 15 and all(v < 32 for v in values), line)
        expected = ''.join(ALPHABET[v] for v in values) + ALPHABET[check_value(values)]
        require(canonical == expected, f'{line}: expected {expected}')
        require(shown == '-'.join(expected[i:i + 4] for i in range(0, 16, 4)), line)
        count += 1
    require(count == 64, f'64 vectors printed, saw {count}')
    return count


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--component', type=Path, help='a copy of components/hardwareone (mutation runs)')
    args = parser.parse_args()
    component = (args.component or COMPONENT).resolve()
    require(bool(args.cxx), 'a host C++17 compiler is required')
    require(gf_mul(2, 16) == 5 and check_value([1] + [0] * 14) == 2, 'the reference field')
    flags = ['-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', '-pedantic']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-g']
    check_sources(component)
    with tempfile.TemporaryDirectory(prefix='hw1-phone-pair-code-') as directory:
        binary = Path(directory) / 'pair_code'
        subprocess.run([args.cxx, *flags, '-I', str(component), str(HERE / 'phone_pair_code_harness.cpp'),
                        '-o', str(binary)], check=True, timeout=180)
        output = subprocess.check_output([str(binary)], text=True, timeout=300)
    vectors = check_vectors(output)
    print(output.splitlines()[-1])
    print(f'Phone pairing code: GF(32) check symbol, {vectors} vectors recomputed, every single error and '
          'swap rejected, normalisation and generation passed')


if __name__ == '__main__':
    main()
