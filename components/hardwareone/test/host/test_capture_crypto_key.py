#!/usr/bin/env python3
"""Exercise the real capture-key loader against scripted NVS and libsodium fakes.

Only ESP_ERR_NVS_NOT_FOUND (namespace or blob missing) may mint a new key. Any
other read-only open failure, or an unreadable stored blob, must fail closed
without calling nvs_set_blob.

File catch-up chunks (captureCryptoSealChunk/OpenChunk/KeyId, sealing on send):
round trip, a wrong AAD or key or a damaged byte fails and wipes the output, a
random nonce per seal, bounds, no key -> -1 with nothing written, the key id
stable, per key, and not the key. Source rules: the key is read only inside the
AEAD call under the module mutex, with no new copy. libsodium is a toy double
with the real call shapes (not the real cipher).

Line sealing, line opening and the PSRAM scratch are not covered; the NVS fake
models return codes, not flash wear or encryption.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


CHUNK_FUNCTIONS = ("bool captureCryptoPseudonym(", "int captureCryptoSealChunk(",
                   "int captureCryptoOpenChunk(", "bool captureCryptoKeyId(")


def chunk_rules(source: str) -> None:
    """The chunk API reads the key only inside the AEAD call and keeps no copy."""
    seal = extract_block(source, "int captureCryptoSealChunk(")
    open_ = extract_block(source, "int captureCryptoOpenChunk(")
    key_id = extract_block(source, "bool captureCryptoKeyId(")
    for name, body in (("seal", seal), ("open", open_)):
        require(body.count("sKey") == 1, f"{name}: sKey appears once, as the AEAD key argument")
        require("ScratchGuard" in body, f"{name}: the key is read under the module mutex")
        require("base64" not in body and "ps_alloc" not in body, f"{name}: no base64, no scratch")
    require("sKey" not in key_id, "the key id comes from the keyed pseudonym, never the key bytes")
    require(not re.search(r"(uint8_t|unsigned char)\s+\w+\[32\]", seal + open_ + key_id),
            "no new 32-byte key copy")
    require("return -1" in seal and "sodium_memzero(out" in seal and "sodium_memzero(out" in open_,
            "seal and open fail closed and wipe their output")


def generate() -> str:
    source = (COMPONENT / "System_CaptureCrypto.cpp").read_text()
    marker = "bool captureCryptoEnsureKey("
    require(source.count(marker) == 1, "one captureCryptoEnsureKey definition")
    loader = extract_block(source, marker)
    harness = (HERE / "capture_crypto_key_harness.cpp").read_text()
    require(harness.count("// INSERT_PRODUCTION") == 1, "one insertion marker")
    if "int captureCryptoSealChunk(" in source:
        chunk_rules(source)
        header = (COMPONENT / "System_CaptureCrypto.h").read_text()
        overhead = re.search(r"^#define CAPCRYPT_CHUNK_OVERHEAD \d+$", header, re.M)
        require(overhead is not None, "System_CaptureCrypto.h defines CAPCRYPT_CHUNK_OVERHEAD")
        loader = "\n\n".join(["#define HAVE_CATCHUP_CHUNKS 1", overhead.group(0), loader] +
                              [extract_block(source, marker) for marker in CHUNK_FUNCTIONS])
    return harness.replace("// INSERT_PRODUCTION", loader)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-capture-key-") as directory:
        work = Path(directory)
        source, binary = work / "test.cpp", work / "test"
        source.write_text(generate())
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-function", str(source), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
