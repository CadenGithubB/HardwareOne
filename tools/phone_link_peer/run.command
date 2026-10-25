#!/bin/zsh
# Start the Mac test phone in Terminal.app: double-click this file in Finder, or run
#   open -a Terminal tools/phone_link_peer/run.command
# CoreBluetooth needs the Bluetooth permission, which macOS gives to Terminal (allow it once in
# System Settings > Privacy & Security > Bluetooth). Extra arguments go to the tool (--relay, ...).
cd "${0:A:h}" || exit 1
if [[ ! -x .venv/bin/python ]]; then
  echo "No .venv in ${PWD}: see README.md (Install)."
  exit 1
fi
exec ./.venv/bin/python -m hw1peer "$@"
