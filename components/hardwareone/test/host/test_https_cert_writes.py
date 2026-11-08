#!/usr/bin/env python3
"""certgen stages the HTTPS key and certificate before replacing either.

Source contract on cmd_certgen (System_WiFi.cpp, ENABLE_HTTPS build): the old
code opened /system/certs/https_server.crt and .key with "w" in place and
ignored print()'s result, so a full filesystem truncated the live files and
still reported success. HTTPS start only checks that both files are non-empty
and httpd_ssl_start does not check that key and certificate match, so a new
key beside the old certificate starts a server whose every handshake fails,
with no HTTP fallback. Now:

- no File is opened for writing inside cmd_certgen, and neither file is
  promoted on its own through writeTextAtomic;
- both PEMs are written to .tmp siblings through writeText (checked byte count
  and size; see test_checked_text_writes.py) in one condition, before the
  first rename, and a staging failure removes both temps;
- each rename's result decides success;
- no message claims HTTPS falls back to HTTP or will not start on a mismatch;
- the String copy and the PSRAM buffer of the private key PEM are wiped.

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
    if re.search(r'writeTextAtomic\(\s*"/system/certs/', body):
        failures.append('a certificate file is promoted on its own before the other is staged')

    tmps = {}
    for var, leaf in (('keyTmp', 'https_server.key'), ('certTmp', 'https_server.crt')):
        if not re.search(r'\b' + var + r'\s*=\s*"/system/certs/' + re.escape(leaf) + r'\.tmp"', body):
            failures.append(f'{var} is not the {leaf}.tmp staging path')
        tmps[var] = [m.start() for m in re.finditer(r'writeText\(\s*' + var + r'\.c_str\(\)', body)]
        if len(tmps[var]) != 1:
            failures.append(f'{leaf} is not staged exactly once through writeText')
    staged = re.search(r'const bool staged\s*=(.*?);', body, re.S)
    if not staged or any(f'writeText({v}.c_str()' not in staged.group(1) for v in tmps):
        failures.append('the two staging writes are not joined in one checked condition')
    elif '||' in staged.group(1):
        failures.append('a staging write failure can still count as staged')

    renames = {}
    for tmp, dest in (('keyTmp', 'keyPath'), ('certTmp', 'certPath')):
        found = [m.start() for m in re.finditer(r'!VFS::renameGuarded\(\s*' + tmp + r'\s*,\s*' + dest + r'\s*,', body)]
        if len(found) != 1:
            failures.append(f'the rename of {tmp} to {dest} is missing or its result is ignored')
        else:
            renames[tmp] = found[0]
    if len(renames) == 2 and all(len(v) == 1 for v in tmps.values()):
        last_stage = max(v[0] for v in tmps.values())
        if min(renames.values()) < last_stage:
            failures.append('a file is renamed into place before both are staged')
    fail_branch = re.search(r'if \(!staged \|\| !VFS::renameGuarded\(keyTmp.*?goto cleanup;', body, re.S)
    if not fail_branch or 'removeGuarded(keyTmp' not in fail_branch.group(0) \
            or 'removeGuarded(certTmp' not in fail_branch.group(0):
        failures.append('a staging failure does not remove both temps')

    lowered = body.lower()
    for claim in ('falls back to http', 'fall back to http', 'will not start'):
        if claim in lowered:
            failures.append(f'cmd_certgen still claims "{claim}" for a mismatched pair')

    if not re.search(r'mbedtls_platform_zeroize\(\s*keyText\.begin\(\)', body):
        failures.append('the String copy of the private key PEM is not wiped')
    if not re.search(r'mbedtls_platform_zeroize\(\s*keyPem\s*,\s*4096\s*\)[^;]*;\s*heap_caps_free\(keyPem\)', body):
        failures.append('the PSRAM key PEM buffer is freed without being wiped')

    if failures:
        for f in failures:
            print(f'https_cert_writes: {f}', file=sys.stderr)
        return 1
    print('https_cert_writes: OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
