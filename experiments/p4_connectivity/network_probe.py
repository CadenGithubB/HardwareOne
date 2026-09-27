#!/usr/bin/env python3
"""Explicit local test AP join/restore; no network actions on import.

This run began with en1 off and Ethernet active. Only restore that observed
state; do not use this helper as general network configuration tooling.
"""
import argparse
import json
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('join', 'restore'))
    parser.add_argument('--ssid', default='HW1_P4_TEST')
    args = parser.parse_args()
    def run(*arguments):
        result = subprocess.run(['/usr/sbin/networksetup', *arguments], capture_output=True, text=True)
        # Never print subprocess arguments or credential-bearing errors.
        if result.returncode or 'Error' in result.stdout or 'Failed' in result.stdout:
            raise RuntimeError('networksetup operation failed')
        return result
    if args.action == 'restore':
        run('-setairportpower', 'en1', 'off')
        print('Restored en1 Wi-Fi OFF; Ethernet configuration unchanged.')
        return 0
    credentials = json.loads((HERE/'private/credentials.json').read_text())
    run('-setairportpower', 'en1', 'on')
    run('-setairportnetwork', 'en1', args.ssid, credentials['ap_password'])
    print('Joined local board test AP; Ethernet configuration unchanged.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
