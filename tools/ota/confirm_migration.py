#!/usr/bin/env python3
"""Interactive guard for the one-time HardwareOne OTA repartition flash."""

from __future__ import annotations

import argparse
import os
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--board", choices=("feathers3", "feathers3_fe"), required=True
    )
    args = parser.parse_args()
    expected = f"MIGRATE {args.board}"

    print("\nONE-TIME OTA LAYOUT MIGRATION")
    print("  - LittleFS moves and will not mount until recovery runs 'formatfs confirm'.")
    print("  - Back up and verify device files before continuing.")
    print("  - NVS is intentionally preserved; do not add erase-flash to this procedure.")
    if args.board == "feathers3_fe":
        print("  - This target must be invoked as encrypted-migration-flash.")

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
    try:
        answer = input(f"Type {expected!r} to continue: ")
    except (EOFError, KeyboardInterrupt):
        print("\nMigration cancelled.", file=sys.stderr)
        return 2
    if answer != expected:
        print("Migration cancelled: confirmation did not match.", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
