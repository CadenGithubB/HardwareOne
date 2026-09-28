#!/usr/bin/env python3
"""Fault-inject the actual copied IDF JPEG lifetime functions on the host.

No device/build access. Uses the prepared component's source, with thin RTOS,
heap and IRQ stubs. Reverse-applying the reviewed patch supplies a negative
control: the original SDK must fail the failure/retry assertions.
"""
from __future__ import annotations
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent
COMPONENT = Path(os.environ.get("HW1_JPEG_COMPONENT", ROOT / "private/idf-components/esp_driver_jpeg"))
STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#ifndef SLIST_REMOVE_AFTER
#define SLIST_REMOVE_AFTER(elm, field) ((elm)->field.sle_next = (elm)->field.sle_next->field.sle_next)
#endif
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
#define JPEG_MEM_ALLOC_CAPS 0
#define ESP_PM_CPU_FREQ_MAX 0
#define ETS_JPEG_INTR_SOURCE 0
#define JPEG_LL_DECODER_EVENT_INTR 1
#define JPEG_LL_ENCODER_EVENT_INTR 2
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_RETURN_ON_FALSE(cond, error, ...) do { if (!(cond)) return (error); } while (0)
#define ESP_RETURN_ON_ERROR(expr, ...) do { int e = (expr); if (e) return e; } while (0)
#define ESP_GOTO_ON_FALSE(cond, error, label, ...) do { if (!(cond)) { ret = (error); goto label; } } while (0)
#define ESP_GOTO_ON_ERROR(expr, label, ...) do { int e = (expr); if (e) { ret = e; goto label; } } while (0)
#define PERIPH_RCC_ATOMIC() for (int once = 1; once; once = 0)
#define unlikely(x) (x)
typedef int esp_err_t;
typedef int _lock_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
typedef void (*intr_handler_t)(void*);
typedef struct jpeg_isr_handler {
    intr_handler_t handler;
    void *handler_arg;
    uint32_t mask, flags;
    SLIST_ENTRY(jpeg_isr_handler) next;
} jpeg_isr_handler_t;
typedef struct { void *dev; } jpeg_hal_context_t;
typedef struct {
    int intr_priority;
    portMUX_TYPE spinlock;
    void *codec_mutex, *pm_lock, *intr_handle;
    jpeg_hal_context_t hal;
    SLIST_HEAD(, jpeg_isr_handler) jpeg_isr_handler_list;
} jpeg_codec_t;
typedef jpeg_codec_t *jpeg_codec_handle_t;
static int live, irq_live, irq_allocations;
static bool fail_calloc, fail_sem, fail_pm, fail_irq, clock_on;
static void *tracked_calloc(size_t n, size_t size) {
    void *p = calloc(n, size); assert(p); ++live; return p;
}
static void tracked_free(void *p) { if (p) { --live; assert(live >= 0); free(p); } }
static void *heap_caps_calloc(size_t n, size_t size, int caps) {
    if (fail_calloc) { fail_calloc = false; return NULL; }
    return tracked_calloc(n, size);
}
#define free tracked_free
static void _lock_acquire(_lock_t *lock) { assert(*lock == 0); *lock = 1; }
static void _lock_release(_lock_t *lock) { assert(*lock == 1); *lock = 0; }
static void *xSemaphoreCreateBinaryWithCaps(int caps) {
    if (fail_sem) { fail_sem = false; return NULL; }
    return tracked_calloc(1, 1);
}
static void vSemaphoreDeleteWithCaps(void *s) { tracked_free(s); }
static void xSemaphoreGive(void *s) { assert(s); }
static int esp_pm_lock_create(int type, int arg, const char *name, void **out) {
    if (fail_pm) { fail_pm = false; return ESP_ERR_NO_MEM; }
    *out = tracked_calloc(1, 1); return ESP_OK;
}
static int esp_pm_lock_delete(void *lock) { tracked_free(lock); return ESP_OK; }
static void jpeg_ll_enable_bus_clock(bool enable) { clock_on = enable; }
static void jpeg_ll_reset_module_register(void) { }
static void jpeg_hal_init(jpeg_hal_context_t *hal) { hal->dev = (void*)1; }
static uintptr_t jpeg_ll_get_interrupt_status_reg(void *dev) { return 0; }
static void jpeg_isr(void *arg) { }
static int esp_intr_alloc_intrstatus(int source, int flags, uint32_t reg, uint32_t mask,
                                    intr_handler_t fn, void *arg, void **out) {
    if (fail_irq) { fail_irq = false; return ESP_ERR_NO_MEM; }
    *out = tracked_calloc(1, 1); ++irq_live; ++irq_allocations; return ESP_OK;
}
static int esp_intr_free(void *handle) { assert(irq_live > 0); --irq_live; tracked_free(handle); return ESP_OK; }
'''
CHECKS = r'''
static void clean(void) {
    assert(s_jpeg_platform.mutex == 0);
    assert(s_jpeg_platform.jpeg_codec == NULL);
    assert(s_jpeg_platform.count == 0);
    assert(live == 0 && irq_live == 0 && !clock_on);
}
static jpeg_codec_handle_t acquire(void) {
    jpeg_codec_handle_t c = NULL;
    assert(jpeg_acquire_codec_handle(&c) == ESP_OK);
    assert(c && clock_on && s_jpeg_platform.count >= 1);
    return c;
}
static void retry(void) { jpeg_codec_handle_t c = acquire(); assert(jpeg_release_codec_handle(c) == ESP_OK); clean(); }
int main(int argc, char **argv) {
    assert(argc == 2);
    int scenario = atoi(argv[1]);
    jpeg_codec_handle_t c = NULL, other = NULL;
    jpeg_isr_handler_t *first = NULL, *second = NULL;
    if (scenario <= 2) {
        if (scenario == 0) fail_calloc = true;
        if (scenario == 1) fail_sem = true;
        if (scenario == 2) fail_pm = true;
        assert(jpeg_acquire_codec_handle(&c) == ESP_ERR_NO_MEM);
        assert(c == NULL);
        // Match jpeg_new_decoder_engine's unconditional deletion path.
        assert(jpeg_release_codec_handle(c) == ESP_OK);
        clean(); retry();
    } else if (scenario == 3) {
        c = acquire(); other = acquire();
        assert(c == other && s_jpeg_platform.count == 2);
        assert(jpeg_release_codec_handle(c) == ESP_OK);
        assert(s_jpeg_platform.count == 1 && clock_on);
        assert(jpeg_release_codec_handle(other) == ESP_OK);
        clean(); retry();
    } else if (scenario == 4) {
        c = acquire();
        assert(jpeg_release_codec_handle(NULL) == ESP_OK);
        assert(s_jpeg_platform.count == 1 && s_jpeg_platform.jpeg_codec == c);
        assert(jpeg_release_codec_handle(c) == ESP_OK); clean();
    } else {
        c = acquire(); int base_live = live;
        if (scenario == 5) fail_calloc = true;
        if (scenario == 6) fail_irq = true;
        if (scenario == 5 || scenario == 6) {
            assert(jpeg_isr_register(c, jpeg_isr, c, 1, 0, &first) == ESP_ERR_NO_MEM);
            assert(first == NULL && c->intr_handle == NULL && irq_live == 0);
            assert(live == base_live);
            assert(jpeg_isr_register(c, jpeg_isr, c, 1, 0, &first) == ESP_OK);
            assert(jpeg_isr_deregister(c, first) == ESP_OK);
        } else if (scenario == 7) {
            for (int i = 0; i < 5; ++i) {
                assert(jpeg_isr_register(c, jpeg_isr, c, 1, 0, &first) == ESP_OK);
                assert(irq_live == 1 && irq_allocations == i + 1);
                assert(jpeg_isr_deregister(c, first) == ESP_OK);
                assert(c->intr_handle == NULL && irq_live == 0 && live == base_live);
            }
        } else if (scenario == 8) {
            assert(jpeg_isr_register(c, jpeg_isr, c, 1, 0, &first) == ESP_OK);
            int registered_live = live;
            fail_calloc = true;
            assert(jpeg_isr_register(c, jpeg_isr, c, 2, 0, &second) == ESP_ERR_NO_MEM);
            assert(live == registered_live && irq_live == 1 && irq_allocations == 1);
            assert(SLIST_FIRST(&c->jpeg_isr_handler_list) == first);
            assert(jpeg_isr_register(c, jpeg_isr, c, 2, 0, &second) == ESP_OK);
            assert(irq_live == 1 && irq_allocations == 1);
            assert(jpeg_isr_deregister(c, first) == ESP_OK);
            assert(irq_live == 1);
            assert(jpeg_isr_deregister(c, second) == ESP_OK);
            assert(irq_live == 0 && c->intr_handle == NULL);
        }
        assert(jpeg_release_codec_handle(c) == ESP_OK); clean(); retry();
    }
    return 0;
}
'''


def extract(source: str) -> str:
    first = source.index("typedef struct jpeg_platform_t")
    end = source.index("/*---------------------------------------------------------------", first)
    irq_start = source.index("esp_err_t jpeg_isr_register(")
    irq_end = source.index("esp_err_t jpeg_check_intr_priority(", irq_start)
    return source[first:end] + source[irq_start:irq_end]


class DriverCleanupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not compiler or not (COMPONENT / "jpeg_common.c").is_file():
            raise unittest.SkipTest("prepare the private JPEG override and install a C compiler")
        spec = importlib.util.spec_from_file_location("prepare_idf_jpeg", ROOT / "prepare_idf_jpeg.py")
        prepare = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(prepare)
        manifest = json.loads((COMPONENT / prepare.MANIFEST).read_text())
        prepare.verify(COMPONENT, manifest["files"])
        if manifest["files"]["jpeg_common.c"] != prepare.PATCHED_COMMON_SHA256:
            raise AssertionError("prepared JPEG source differs from reviewed hash")
        patch = (ROOT / "idf-jpeg.patch").read_bytes()
        if prepare.digest(patch) != prepare.PATCH_SHA256:
            raise AssertionError("reviewed patch hash changed")
        cls.temp = tempfile.TemporaryDirectory(prefix="hw1-jpeg-cleanup-")
        cls.addClassCleanup(cls.temp.cleanup)
        temporary = Path(cls.temp.name)
        actual = (COMPONENT / "jpeg_common.c").read_text()
        original_dir = temporary / "original"
        original_dir.mkdir()
        (original_dir / "jpeg_common.c").write_text(actual)
        reverted = subprocess.run(["patch", "-R", "-f", "-F", "0", "-p", "1"],
                                  input=patch, cwd=original_dir, capture_output=True)
        if reverted.returncode:
            raise AssertionError(reverted.stdout.decode() + reverted.stderr.decode())
        original = (original_dir / "jpeg_common.c").read_text()
        cls.executables = {}
        for version, source in (("patched", actual), ("original", original)):
            for pm in (0, 1):
                cfile = temporary / f"{version}-{pm}.c"
                executable = temporary / f"{version}-{pm}"
                cfile.write_text(STUBS + extract(source) + CHECKS)
                built = subprocess.run([compiler, "-std=c11", f"-DCONFIG_PM_ENABLE={pm}",
                                        str(cfile), "-o", str(executable)], capture_output=True, text=True)
                if built.returncode:
                    raise AssertionError(built.stdout + built.stderr)
                cls.executables[version, pm] = executable

    def run_case(self, scenario, pm=1, version="patched", succeeds=True):
        result = subprocess.run([str(self.executables[version, pm]), str(scenario)],
                                capture_output=True, text=True, timeout=3)
        if succeeds:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, "negative control unexpectedly passed")

    def test_codec_allocation_failure_retry(self):
        for pm in (0, 1): self.run_case(0, pm)

    def test_semaphore_allocation_failure_retry(self):
        for pm in (0, 1): self.run_case(1, pm)

    def test_pm_allocation_failure_retry(self):
        self.run_case(2)

    def test_two_references_release_exactly_once(self):
        for pm in (0, 1): self.run_case(3, pm)

    def test_null_release_preserves_an_existing_owner(self):
        for pm in (0, 1): self.run_case(4, pm)

    def test_handler_oom_does_not_allocate_irq(self):
        for pm in (0, 1): self.run_case(5, pm)

    def test_irq_oom_releases_handler_then_retry(self):
        for pm in (0, 1): self.run_case(6, pm)

    def test_last_handler_removal_allows_irq_recreation(self):
        for pm in (0, 1): self.run_case(7, pm)

    def test_handler_oom_keeps_existing_irq_and_handlers(self):
        for pm in (0, 1): self.run_case(8, pm)

    def test_original_sdk_negative_controls(self):
        for scenario in (1, 2, 4, 5, 7):
            with self.subTest(scenario=scenario):
                self.run_case(scenario, version="original", succeeds=False)


if __name__ == "__main__":
    unittest.main(verbosity=2)
