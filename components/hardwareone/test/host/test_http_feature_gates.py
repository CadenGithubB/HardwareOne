#!/usr/bin/env python3
"""Preprocess the actual feature header across HTTP/network/web combinations.

Uses the existing deployment override hook, never edits the live configuration.
This pins dependent web flags (including Power) when HTTP is disabled. It does
not claim that every unrelated hardware feature combination links or boots.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("c++"))
    args = parser.parse_args()
    assert args.cxx, "a C++ preprocessor is required"
    header = Path(__file__).resolve().parents[2] / "System_BuildConfig.h"
    source = header.read_text()
    web = source[source.index("// DERIVED WEB FLAGS"):
                 source.index("// CROSS-FEATURE DEPENDENCY OVERRIDES")]
    flags = set(re.findall(r"#define\s+(ENABLE_WEB_\w+)\s+", web))
    assert "ENABLE_WEB_POWER" in flags
    checks = 0
    with tempfile.TemporaryDirectory(prefix="hw1-http-gates-") as temp:
        override = Path(temp) / "feature_override.h"
        for network in range(5):
            for web_level in range(5):
                for wifi in range(2):
                    for http in range(2):
                        values = {
                            "NETWORK_FEATURE_LEVEL": network,
                            "WEB_FEATURE_LEVEL": web_level,
                            "CUSTOM_ENABLE_NET_WIFI": wifi,
                            "CUSTOM_ENABLE_NET_HTTP": http,
                            "CUSTOM_ENABLE_WEB_POWER": 1,
                        }
                        override.write_text("\n".join(
                            f"#undef {key}\n#define {key} {value}"
                            for key, value in values.items()))
                        result = subprocess.run([
                            args.cxx, "-E", "-dM", "-x", "c++",
                            "-DARDUINO_UM_FEATHERS3_DEV=1",
                            f'-DHW1_DEPLOYMENT_CONFIG_HEADER="{override}"',
                            "-include", str(header), "-",
                        ], input="", text=True, capture_output=True)
                        assert result.returncode == 0, f"{values}: {result.stderr}"
                        macros = dict(re.findall(r"^#define\s+(\w+)\s+(.+)$", result.stdout, re.M))
                        enabled = web_level > 0 and (
                            (network in (2, 3)) or (network == 4 and wifi and http))
                        label = f"network={network},web={web_level},wifi={wifi},http={http}"
                        assert macros["ENABLE_HTTP_SERVER"] == str(int(bool(enabled))), label
                        if not enabled:
                            for flag in flags:
                                assert macros[flag] == "0", f"{label}: {flag} remains enabled"
                        elif web_level in (2, 3):
                            assert macros["ENABLE_WEB_POWER"] == "1", label
                        elif web_level == 4:
                            assert macros["ENABLE_WEB_POWER"] == "CUSTOM_ENABLE_WEB_POWER", label
                        checks += 1
    print(f"HTTP dependency gates: {checks} preprocessor profiles passed")


if __name__ == "__main__":
    main()
