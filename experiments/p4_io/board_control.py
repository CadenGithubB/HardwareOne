#!/usr/bin/env python3
"""Reuse the checked board coordinator, with this experiment's private logs."""
import importlib.util
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
    "ble_roles_board_control", HERE.parent / "p4_ble_roles/board_control.py")
controller = importlib.util.module_from_spec(spec)
spec.loader.exec_module(controller)
controller.HERE = HERE


def jobs(source):
    """Keep display login credentials in private storage and redacted logs."""
    for line in source:
        job = json.loads(line)
        if job.get("action") == "displaylogin":
            credentials = json.loads((HERE / "private/credentials.json").read_text())
            user = controller.command_token(credentials["username"])
            password = controller.command_token(credentials["password"])
            job = {"action": "command", "board": "p4",
                   "command": f"login {user} {password} display"}
        yield json.dumps(job) + "\n"


if __name__ == "__main__":
    sys.stdin = jobs(sys.stdin)
    raise SystemExit(controller.main())
