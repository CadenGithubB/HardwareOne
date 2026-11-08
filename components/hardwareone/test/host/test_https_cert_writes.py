#!/usr/bin/env python3
"""certgen saves the HTTPS key and certificate through checked atomic writes.

Source contract on cmd_certgen (System_WiFi.cpp, ENABLE_HTTPS build): the old
code opened /system/certs/https_server.crt and .key with "w" in place and
ignored print()'s result, so a full filesystem truncated the live files and
still reported success. Now:

- no File is opened for writing inside cmd_certgen;
- the key and then the certificate go through writeTextAtomic (temp, checked
  byte count and size, rename; see test_checked_text_writes.py for the
  helper's own short-write cases), and each result decides success;
- the Arduino String copy of the private key PEM is wiped before it is freed.

--source-dir <component-directory> reads System_WiFi.cpp from another tree
(for example the pre-fix base); the base tree must fail.
"""
import argparse
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--source-dir', type=Path)
    a = p.parse_args()
    sys.path.insert(0, str(HERE))
    from test_web_batch_handlers import extract_block

    root = a.source_dir or a.component
    source = (root / 'System_WiFi.cpp').read_text()
    # The first definition is the ENABLE_HTTPS one; the second is the
    # one-line stub for builds without HTTPS.
    body = extract_block(source, 'const char* cmd_certgen(const String& argsInput)')
    if 'mbedtls_x509write_crt_pem' not in body:
        raise SystemExit('System_WiFi.cpp: the HTTPS cmd_certgen was not found first')
    failures = []

    if re.search(r'openGuarded\([^;]*"w"', body):
        failures.append('cmd_certgen opens a certificate file with "w" in place')
    if re.search(r'\.print\(\s*\(const char\*\)\s*(certPem|keyPem)', body):
        failures.append('cmd_certgen prints a PEM into an open File (result unchecked)')
    calls = {}
    for leaf in ('https_server.key', 'https_server.crt'):
        found = [m for m in re.finditer(r'writeTextAtomic\(\s*"/system/certs/' + re.escape(leaf) + '"', body)]
        if len(found) != 1:
            failures.append(f'{leaf} is not written exactly once through writeTextAtomic')
            continue
        start = found[0].start()
        calls[leaf] = start
        line_start = body.rfind('\n', 0, start) + 1
        prefix = body[line_start:start].strip()
        if prefix == '':
            # A continuation line: the previous line must join it into a condition.
            prev = body[:line_start].rstrip()
            if not prev.endswith(('&&', '||', '(', '!', '=')):
                failures.append(f'the writeTextAtomic result for {leaf} is ignored')
        elif not prefix.endswith(('&&', '||', '(', '!', '=')):
            failures.append(f'the writeTextAtomic result for {leaf} is ignored')
    if len(calls) == 2 and calls['https_server.key'] > calls['https_server.crt']:
        failures.append('the certificate is promoted before its key')
    if not re.search(r'mbedtls_platform_zeroize\(\s*keyText\.begin\(\)', body):
        failures.append('the String copy of the private key PEM is not wiped')

    if failures:
        for f in failures:
            print(f'https_cert_writes: {f}', file=sys.stderr)
        return 1
    print('https_cert_writes: OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
