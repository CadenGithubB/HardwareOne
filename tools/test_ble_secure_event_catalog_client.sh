#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: tools/test_ble_secure_event_catalog_client.sh [--offline|--physical] [client options]

  --offline   Install the reviewed hash-pinned tool environment and run only
              repository-local vectors/catalog checks. This is the default.
  --physical  Run the same offline checks, then explicitly scan/connect using
              the interactive physical client. Secrets remain prompt-only.

Physical mode never changes the ESP32 BLE role. Prepare/restore phone-server
mode through the authenticated UART checklist, not through this runner.

Optional environment:
  HW1_BLE_SECURE_PYTHON  CPython used to create the venv (default: python3)
  HW1_BLE_SECURE_VENV    reusable isolated venv path
EOF
}

mode="offline"
if [[ $# -gt 0 ]]; then
  case "$1" in
    --offline)
      mode="offline"
      shift
      ;;
    --physical)
      mode="physical"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Error: first argument must be --offline or --physical" >&2
      usage >&2
      exit 2
      ;;
  esac
fi

if [[ "${mode}" == "offline" && $# -ne 0 ]]; then
  echo "Error: client options are accepted only with --physical" >&2
  exit 2
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
requirements_path="${script_dir}/ble_secure/requirements.txt"
protocol_test_path="${script_dir}/ble_secure/test_secure_channel_v1.py"
client_path="${script_dir}/ble_secure_event_catalog_client.py"
runner_python="${HW1_BLE_SECURE_PYTHON:-python3}"
venv_path="${HW1_BLE_SECURE_VENV:-${TMPDIR:-/tmp}/hardwareone-ble-secure-venv}"

preflight_python() {
  "$1" -c 'import platform, sys, sysconfig
ok_version = (3, 12) <= sys.version_info[:2] < (3, 15)
ok_platform = platform.system() in {"Darwin", "Linux"}
ok_machine = platform.machine().lower() in {"x86_64", "amd64", "arm64", "aarch64"}
ok_gil = sysconfig.get_config_var("Py_GIL_DISABLED") not in {1, "1"}
if not (ok_version and ok_platform and ok_machine and ok_gil):
    print("Secure BLE tools require standard CPython 3.12-3.14 on Darwin/Linux x86_64 or arm64/aarch64", file=sys.stderr)
    raise SystemExit(1)'
}

preflight_python "${runner_python}"

if [[ ! -x "${venv_path}/bin/python" ]]; then
  "${runner_python}" -m venv "${venv_path}"
fi
venv_python="${venv_path}/bin/python"
preflight_python "${venv_python}"

"${venv_python}" -m pip install \
  --disable-pip-version-check \
  --require-hashes \
  --only-binary=:all: \
  -r "${requirements_path}"

"${venv_python}" -c 'import importlib.metadata as metadata
expected = {"bleak": "3.0.2", "PyNaCl": "1.6.2", "cffi": "2.0.0", "pycparser": "2.23"}
for package, wanted in expected.items():
    actual = metadata.version(package)
    if actual != wanted:
        raise SystemExit(f"{package} version mismatch: expected {wanted}, got {actual}")
import bleak, nacl  # noqa: F401'

"${venv_python}" "${protocol_test_path}" -v
"${venv_python}" "${client_path}" --offline

if [[ "${mode}" == "physical" ]]; then
  exec "${venv_python}" "${client_path}" --physical "$@"
fi
