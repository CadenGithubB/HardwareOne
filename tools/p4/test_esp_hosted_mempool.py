#!/usr/bin/env python3
"""Exercise the actual Hosted pool and IDF DMA alignment/write checks on the host.

Only heap/cache/RTOS boundaries are mocked. The test compiles the production
common-pool constructor, hosted_malloc_align, transport allocation functions,
both complete mempool sources, IDF's complete heap alignment hook, and its
SDMMC alignment and block-write functions. All temporary files are removed.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

import prepare_esp_hosted as prepare


def function(source: str, name: str) -> str:
    """Extract one definition unchanged, rather than duplicating its logic."""
    match = re.search(r"^[\w *]+\b" + re.escape(name) + r"\([^;{}]*\)\s*\{", source, re.M)
    if not match:
        raise ValueError(f"production function missing: {name}")
    end = match.end()
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


def write(root: Path, name: str, text: str) -> None:
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, help="pristine Hosted 2.12.13 package")
    parser.add_argument("--idf", type=Path, default=os.environ.get("IDF_PATH"))
    parser.add_argument("--cc", default=shutil.which("clang") or shutil.which("cc"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if not args.idf or not args.cc:
        parser.error("export IDF_PATH (5.5.5) or pass --idf; a host C compiler is required")
    idf = Path(args.idf)
    source = prepare.find_source(args.source)
    source_digest = prepare.tree_digest(source)
    with tempfile.TemporaryDirectory(prefix="hw1-hosted-pool-") as tmp:
        work = Path(tmp)
        patched = prepare.prepare(work / "patched", source)
        before = (patched / prepare.MANIFEST).stat().st_mtime_ns
        prepare.prepare(patched, source, check=True)
        assert before == (patched / prepare.MANIFEST).stat().st_mtime_ns

        # Verify that a changed existing override is rejected, never replaced.
        modified = patched / "host/drivers/transport/transport_drv.c"
        saved = modified.read_bytes()
        modified.write_bytes(saved + b"\n/* unexpected local edit */\n")
        try:
            prepare.prepare(patched, source)
        except ValueError:
            assert modified.read_bytes() != saved
        else:
            raise AssertionError("modified existing override was accepted")
        modified.write_bytes(saved)
        prepare.verify(patched)

        stubs = work / "stubs"
        caps_header = (idf / "components/heap/include/esp_heap_caps.h").read_text()
        caps = "\n".join(line for line in caps_header.splitlines()
                         if line.startswith("#define MALLOC_CAP_"))
        # IDF spells its high-bit sentinel (1<<31); retain the same bits without
        # triggering host UBSan's signed-shift check in this SDK header fixture.
        caps = caps.replace("(1<<31)", "(1u<<31)")
        write(stubs, "esp_heap_caps.h", "#pragma once\n#include <stdint.h>\n#include <stddef.h>\n" +
              caps + "\nvoid *heap_caps_aligned_alloc(size_t,size_t,uint32_t);\n")
        write(stubs, "sdkconfig.h", "#pragma once\n#define CONFIG_ESP_HOSTED_USE_MEMPOOL 1\n"
              "#define CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH 1\n")
        write(stubs, "esp_private/esp_cache_private.h", "#pragma once\n"
              "#include <stdint.h>\n#include <stddef.h>\n"
              "typedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n"
              "#define ESP_ERR_INVALID_ARG 258\n"
              "esp_err_t esp_cache_get_alignment(uint32_t,size_t *);\n")
        write(stubs, "esp_private/gdma.h", "#pragma once\n")
        write(stubs, "soc/soc_caps.h", "#pragma once\n#define SOC_GDMA_SUPPORTED 1\n"
              "#define SOC_AXI_GDMA_SUPPORTED 1\n#define SOC_PSRAM_DMA_CAPABLE 1\n"
              "#define SOC_MEMSPI_ENCRYPTION_ALIGNMENT 256\n")
        write(stubs, "hal/gdma_ll.h", "#pragma once\n#define GDMA_LL_AXI_DESC_ALIGNMENT 8\n")
        write(stubs, "hal/efuse_hal.h", "#pragma once\n#include <stdbool.h>\n"
              "bool efuse_hal_flash_encryption_enabled(void);\n")
        write(stubs, "esp_log.h", "#pragma once\n#include <assert.h>\n"
              "#define ESP_LOGE(...) ((void)0)\n#define ESP_LOGI(...) ((void)0)\n")
        write(stubs, "freertos/FreeRTOS.h", "#pragma once\n#include <assert.h>\n#include <stdint.h>\n"
              "#include <stddef.h>\n#include \"sdkconfig.h\"\n"
              "typedef void *SemaphoreHandle_t;\n#define portMAX_DELAY 0\n")
        for name in ("portmacro", "task", "portable"):
            write(stubs, f"freertos/{name}.h", "#pragma once\n")
        write(stubs, "freertos/semphr.h", "#pragma once\n"
              "#define xSemaphoreCreateMutex() ((void *)1)\n"
              "#define vSemaphoreDelete(x) ((void)0)\n"
              "#define xSemaphoreTake(x,t) ((void)0)\n"
              "#define xSemaphoreGive(x) ((void)0)\n")

        port = source / "host/port/esp/freertos/src/port_esp_hosted_host_os.c"
        allocation = function(port.read_text(), "hosted_malloc_align")
        util = (source / "host/drivers/transport/transport_util.c").read_text()
        allocation += function(util, "transport_util_malloc")
        allocation += function(util, "transport_util_calloc")
        sdmmc = function((idf / "components/esp_driver_sdmmc/src/sdmmc_host.c").read_text(),
                         "sdmmc_host_check_buffer_alignment")
        sdmmc += function((idf / "components/sdmmc/sdmmc_io.c").read_text(), "sdmmc_io_write_blocks")
        (work / "allocation.inc").write_text(allocation)
        (work / "sdmmc.inc").write_text(sdmmc)

        for name, package in (("original", source), ("fixed", patched)):
            pool = (package / "host/drivers/transport/transport_drv.c").read_text()
            actual = "static hosted_mempool_t *mempool_common;\n"
            actual += function(pool, "transport_drv_common_mempool_create")
            actual += function(pool, "transport_drv_common_mempool_deinit")
            (work / f"{name}.inc").write_text(actual)

        harness = Path(__file__).with_name("esp_hosted_mempool_harness.c")
        for prefer in (0, 1):
            for fixed in (0, 1):
                exe = work / f"pool-{prefer}-{fixed}"
                package = patched if fixed else source
                command = [args.cc, "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra",
                           "-Wno-unused-variable", "-Wno-unused-parameter", "-Wno-missing-field-initializers",
                           "-Wno-missing-declarations", f"-DCONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM={prefer}",
                           f"-DTEST_FIXED={fixed}", "-I", str(stubs), "-I", str(work),
                           "-I", str(package / "common/mempool/include"),
                           "-I", str(package / "common/mempool"),
                           "-I", str(package / "common/transport"),
                           str(harness), str(package / "common/mempool/mempool.c"),
                           str(package / "common/mempool/mempool_ll.c"),
                           str(idf / "components/esp_mm/heap_align_hw.c"), "-o", str(exe)]
                if args.sanitize:
                    command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                   "-fno-omit-frame-pointer"]
                subprocess.run(command, check=True)
                subprocess.run([str(exe)], check=True)
        assert source_digest == prepare.tree_digest(source), "pristine package was changed"
    print("PASS: original reproduces misalignment; fixed production pool/allocator/SDMMC pass; provenance checks pass")


if __name__ == "__main__":
    main()
