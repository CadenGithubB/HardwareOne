import subprocess, sys, tempfile
from pathlib import Path
HOST = Path.home() / "esp/hardwareone-idf/components/hardwareone/test/host"
sys.path.insert(0, str(HOST))
from test_oled_console_buffer import production_methods, COMPONENT  # noqa: E402
SP = Path(sys.argv[1])
src = (SP / "probe" / "repro_current.cpp").read_text().replace("// INSERT_PRODUCTION_METHODS", production_methods())
with tempfile.TemporaryDirectory(prefix="hw1-probe-repro-", dir=SP) as d:
    unit = Path(d) / "repro.cpp"; binary = Path(d) / "repro"
    unit.write_text(src)
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-fsanitize=address,undefined", "-g", "-pthread", "-I", str(COMPONENT), str(unit), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
