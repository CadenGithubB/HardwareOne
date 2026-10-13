#!/usr/bin/env python3
"""Prepare a private relay directory; never edit OpenClaw or start a service."""
from __future__ import annotations

import argparse
import getpass
import ipaddress
import json
import os
from pathlib import Path
import plistlib
import secrets
import shutil
import subprocess
import sys
import tempfile

from relay import ConfigError, _private_file, load_config


def private_write(path, data):
    with open(path, "x", encoding="utf-8") as output:
        os.chmod(path, 0o600)
        output.write(data)


def openssl(program, *arguments):
    result = subprocess.run([program, *arguments], stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, timeout=60)
    if result.returncode:
        raise ValueError("Certificate generation failed; no installation created")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", required=True, help="Studio's explicit test-VLAN IPv4")
    parser.add_argument("--port", type=int, default=18790)
    parser.add_argument("--upstream-port", type=int, default=18789)
    parser.add_argument("--device-id", default="xiao-studio")
    parser.add_argument("--output", type=Path,
                        default=Path.home() / ".openclaw" / "hardwareone-relay")
    parser.add_argument("--gateway-token-file", type=Path,
                        help="Existing owner-only token file; otherwise prompt with echo disabled")
    args = parser.parse_args(argv)
    destination = args.output.expanduser().absolute()
    if destination.exists() or destination.is_symlink():
        parser.error("Output already exists; existing relay files will not be overwritten")
    if not destination.parent.is_dir():
        parser.error("Output's parent directory must already exist")
    if destination.parent.resolve() != destination.parent:
        parser.error("Output's parent directory must not contain symlinks")
    try:
        address = ipaddress.IPv4Address(args.bind)
        if address.is_unspecified or address.is_multicast or address.is_reserved:
            raise ValueError()
    except ValueError:
        parser.error("--bind must be a specific interface IPv4 address")
    program = shutil.which("openssl")
    if not program:
        parser.error("openssl is required to create the local CA and server certificate")
    if args.gateway_token_file:
        credential = _private_file(str(args.gateway_token_file.expanduser()), 1026).rstrip(b"\r\n").decode("ascii")
    else:
        if not sys.stdin.isatty():
            parser.error("Run interactively for masked token entry, or supply --gateway-token-file")
        credential = getpass.getpass("Existing OpenClaw gateway token (hidden): ")
    if not 1 <= len(credential) <= 1024 or any(ord(c) < 33 or ord(c) > 126 for c in credential):
        parser.error("Gateway token must be nonempty printable ASCII without spaces")

    previous_mask = os.umask(0o077)
    try:
        with tempfile.TemporaryDirectory(prefix=".hw1-relay-", dir=destination.parent) as temporary:
            staging = Path(temporary)
            private_write(staging / "gateway-token.txt", credential + "\n")
            credential = None
            private_write(staging / "device-token.txt", secrets.token_urlsafe(32) + "\n")
            private_write(staging / "ca.cnf", """[req]
prompt = no
distinguished_name = dn
x509_extensions = ca
[dn]
CN = Hardware One Relay CA
[ca]
basicConstraints = critical, CA:true, pathlen:0
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash
""")
            private_write(staging / "leaf.cnf", """basicConstraints = critical, CA:false
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid,issuer
subjectAltName = IP:%s
""" % address)
            openssl(program, "req", "-new", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-sha256", "-days", "3650", "-config", str(staging / "ca.cnf"),
                    "-keyout", str(staging / "ca-key.pem"), "-out", str(staging / "ca-cert.pem"))
            openssl(program, "req", "-new", "-newkey", "rsa:2048", "-nodes", "-sha256",
                    "-subj", "/CN=Hardware One Relay", "-keyout", str(staging / "relay-key.pem"),
                    "-out", str(staging / "relay.csr"))
            openssl(program, "x509", "-req", "-in", str(staging / "relay.csr"),
                    "-CA", str(staging / "ca-cert.pem"), "-CAkey", str(staging / "ca-key.pem"),
                    "-CAcreateserial", "-days", "365", "-sha256", "-extfile", str(staging / "leaf.cnf"),
                    "-out", str(staging / "relay-cert.pem"))
            for name in ("ca-key.pem", "relay-key.pem"):
                os.chmod(staging / name, 0o600)
            config = dict(bind_host=str(address), bind_port=args.port,
                          device_identity=args.device_id, upstream_port=args.upstream_port,
                          tls_cert_file=str(staging / "relay-cert.pem"),
                          tls_key_file=str(staging / "relay-key.pem"),
                          device_token_file=str(staging / "device-token.txt"),
                          upstream_token_file=str(staging / "gateway-token.txt"))
            private_write(staging / "config.json", json.dumps(config, indent=2) + "\n")
            load_config(staging / "config.json")
            for key in ("tls_cert_file", "tls_key_file", "device_token_file", "upstream_token_file"):
                config[key] = str(destination / Path(config[key]).name)
            (staging / "config.json").write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
            shutil.copyfile(Path(__file__).with_name("relay.py"), staging / "relay.py")
            os.chmod(staging / "relay.py", 0o600)
            launch = dict(Label="ai.openclaw.hardwareone-relay",
                          ProgramArguments=[sys.executable, str(destination / "relay.py"),
                                            "--config", str(destination / "config.json")],
                          RunAtLoad=True, KeepAlive=True, ThrottleInterval=30,
                          ProcessType="Background",
                          StandardOutPath=str(destination / "relay.log"),
                          StandardErrorPath=str(destination / "relay.log"),
                          EnvironmentVariables={"PYTHONUNBUFFERED": "1"})
            with open(staging / "launchagent.plist", "xb") as output:
                plistlib.dump(launch, output)
            for name in ("ca.cnf", "leaf.cnf", "relay.csr", "ca-cert.srl"):
                (staging / name).unlink(missing_ok=True)
            # Refuse to replace a directory that appeared while preparing files.
            # mkdir reserves the destination; it is removed only if our rename fails.
            destination.mkdir(mode=0o700)
            try:
                os.rename(staging, destination)
            except BaseException:
                destination.rmdir()
                raise
    finally:
        os.umask(previous_mask)
    print("Prepared relay files in " + str(destination))
    print("Relay base URL: https://%s:%d/v1" % (address, args.port))
    print("Public CA certificate: " + str(destination / "ca-cert.pem"))
    print("Device token is in device-token.txt; keep both token files and private keys private.")
    print("Server certificate expires in 365 days; preserve ca-key.pem privately for renewal.")
    print("OpenClaw settings are unchanged. No service has been started or installed.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ConfigError, ValueError, OSError, UnicodeError, subprocess.TimeoutExpired):
        print("Relay setup failed; inspect the non-secret arguments and private-file permissions.", file=sys.stderr)
        sys.exit(1)
