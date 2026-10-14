#!/usr/bin/env python3
"""Connect an installed local Terminal host to the existing relay; start nothing."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hmac
import json
import os
from pathlib import Path
import secrets
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "relay"))
from relay import ConfigError, _json, _private_file, _token, load_config


def atomic_write(path: Path, data: bytes):
    fd, temporary = tempfile.mkstemp(prefix=".hw1-terminal-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--relay-config", type=Path,
                        default=Path.home()/".openclaw/hardwareone-relay/config.json")
    parser.add_argument("--terminal-directory", type=Path,
                        default=Path.home()/".local/share/hardwareone-even-terminal")
    parser.add_argument("--check", action="store_true", help="Validate without saving any files")
    args = parser.parse_args(argv)
    os.umask(0o077)
    config_path = args.relay_config.expanduser().absolute()
    terminal = args.terminal_directory.expanduser().absolute()
    try:
        if config_path.parent.resolve() != config_path.parent:
            raise ValueError("Relay directory must not contain symlinks")
        old = _private_file(str(config_path), 16384)
        relay_config = _json(old)
        # Validate the existing TLS and credentials before preparing an update.
        existing = load_config(config_path)
        host = _json(_private_file(str(terminal/"hardwareone.json"), 16384))
        if not isinstance(host, dict) or type(host.get("port")) is not int or not 1024 <= host["port"] <= 65535:
            raise ValueError("Invalid Terminal host configuration")
        if host.get("gatewayPort") != existing.upstream_port:
            raise ValueError("Terminal and relay must use the same local Gateway port")
        if not hmac.compare_digest(_token(host.get("gatewayTokenFile"), 1),
                                   _token(existing.upstream_token_file, 1)):
            raise ValueError("Terminal and relay must use the same Gateway credential")
        bridge_path = host.get("bridgeTokenFile")
        if not isinstance(bridge_path, str) or not Path(bridge_path).is_absolute():
            raise ValueError("Terminal bridge token path must be absolute")
        relay_config.update(terminal_port=host["port"], terminal_token_file=bridge_path)
        updated = (json.dumps(relay_config, indent=2) + "\n").encode()
        # Use the exact new relay's complete validator, including distinct-token
        # checks. This temporary file contains paths, never inline credentials.
        with tempfile.TemporaryDirectory(prefix=".hw1-terminal-check-", dir=config_path.parent) as directory:
            staged = Path(directory)/"config.json"
            staged.write_bytes(updated)
            load_config(staged)
        source = Path(__file__).resolve().parent.parent/"relay/relay.py"
        new_code = source.read_bytes()
        compile(new_code, str(source), "exec")
        installed_code = config_path.with_name("relay.py")
        previous_code = _private_file(str(installed_code), 256 * 1024)
        if args.check:
            print("Terminal and relay configuration validated; no files changed or connections made.")
            return 0
        if old == updated and previous_code == new_code:
            print("Terminal is already configured. Restart the relay if it is still running the older code.")
            return 0
        suffix = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + secrets.token_hex(3)
        for path, data in ((config_path, old), (installed_code, previous_code)):
            backup = path.with_name(path.name + ".before-terminal-" + suffix)
            with open(backup, "xb") as output:
                output.write(data)
            print("Backup: " + str(backup))
        # New relay accepts the old configuration too: publish code first so a
        # failure before the config rename still leaves a usable chat relay.
        atomic_write(installed_code, new_code)
        atomic_write(config_path, updated)
    except (ConfigError, ValueError, TypeError, OSError, KeyError):
        parser.exit(1, "Setup failed. Check private file permissions, matching Gateway paths/port, and the installed Terminal configuration. No service was started.\n")
    print("Enabled Terminal through the existing relay. Device token, CA and Wi-Fi settings are unchanged.")
    print("Restart the relay to load the update; keep the local Terminal host running in its other window.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
