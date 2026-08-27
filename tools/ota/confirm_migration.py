#!/usr/bin/env python3
"""Interactive guard for the one-time HardwareOne OTA repartition flash."""

from __future__ import annotations

import argparse
import os
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import make_manifest  # noqa: E402  (path set above)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--board", choices=sorted(make_manifest.BOARD_LAYOUTS), required=True
    )
    args = parser.parse_args()
    expected = f"MIGRATE {args.board}"

    print("\nONE-TIME OTA LAYOUT MIGRATION")
    print("  - LittleFS is REPLACED with an empty filesystem. Its contents are destroyed.")
    print("  - Back up and verify device files before continuing.")
    print("  - To update only the recovery updater instead, use 'factory-flash'.")
    print("  - NVS is intentionally preserved; do not add erase-flash to this procedure.")
    if args.board == "feathers3_fe":
        print("  - This target must be invoked as encrypted-migration-flash.")

    sys.stdout.flush()

    if os.environ.get("HW1_OTA_MIGRATION_CONFIRM") == expected:
        print("Migration confirmation accepted from HW1_OTA_MIGRATION_CONFIRM.")
        return 0
    if not sys.stdin.isatty():
        print(
            f"Refusing non-interactive migration. Set "
            f"HW1_OTA_MIGRATION_CONFIRM={expected!r} only after backup verification.",
            file=sys.stderr,
        )
        return 2
    # The prompt goes to stderr, and stdout is flushed first.
    #
    # This runs as a ninja subcommand, so stdout is a pipe, not a terminal.
    # Python block-buffers a piped stdout, and input()'s prompt goes to
    # stdout -- so the prompt sat in the buffer while the process blocked on
    # a read the operator could not see they were being asked for. The
    # observed result was a migration that appeared to hang, then printed
    # "Migration cancelled" followed by the question it had never shown.
    # stderr is unbuffered, so the prompt is visible before the read blocks.
    sys.stdout.flush()
    sys.stderr.write(f"Type {expected!r} to continue: ")
    sys.stderr.flush()
    try:
        answer = input()
    except (EOFError, KeyboardInterrupt):
        print("\nMigration cancelled.", file=sys.stderr)
        return 2
    if answer != expected:
        print("Migration cancelled: confirmation did not match.", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
