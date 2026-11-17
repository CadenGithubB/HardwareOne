import subprocess, sys, tempfile, shutil
from pathlib import Path
SP = Path(sys.argv[1]); PROTO = SP / "proto"
sys.path.insert(0, str(PROTO))
from make_proto import proto_header, proto_methods, COMPONENT  # noqa: E402
with tempfile.TemporaryDirectory(prefix="hw1-probe-proto-", dir=SP) as d:
    d = Path(d)
    (d / "OLED_ConsoleBuffer.h").write_text(proto_header())
    (d / "proto_methods.inc").write_text(proto_methods())
    shutil.copy(PROTO / "proto_probe.cpp", d / "proto_probe.cpp")
    binary = d / "proto"
    # The prototype header sits beside the unit, so "OLED_ConsoleBuffer.h" resolves to it first.
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-fsanitize=address,undefined", "-g", "-pthread", "-I", str(COMPONENT),
                    str(d / "proto_probe.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    # Keep the derived prototype sources as evidence.
    (PROTO / "derived_OLED_ConsoleBuffer.h").write_text(proto_header())
    (PROTO / "derived_proto_methods.inc").write_text(proto_methods())
