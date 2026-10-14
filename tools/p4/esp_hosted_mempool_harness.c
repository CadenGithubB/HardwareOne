/* Host harness: the functions under test are included unchanged from production. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "mempool.h"
#include "esp_hosted_transport.h"

#define HOSTED_MEM_ALIGNMENT_64 64
#define H_TRANSPORT_QUEUE_SIZE 20
#define MEMPOOL_PADDING 5
#define BLOCKS (H_TRANSPORT_QUEUE_SIZE + MEMPOOL_PADDING)
void esp_heap_adjust_alignment_to_hw(size_t *, size_t *, uint32_t *);
void *hosted_malloc_align(size_t, size_t);
struct test_functions {
    void *(*_h_malloc_align)(size_t, size_t);
    void *(*_h_malloc)(size_t);
    void *(*_h_calloc)(size_t, size_t);
    void *(*_h_memset)(void *, int, size_t);
    void (*_h_free)(void *);
};
static struct test_functions funcs = {hosted_malloc_align, malloc, calloc, memset, free};
static struct {struct test_functions *funcs;} g_h = {&funcs};
static size_t internal_cache = 64, external_cache = 128;
static bool force_internal, encrypted, allocated_external;
static void *pool_base;
static size_t pool_size;

esp_err_t esp_cache_get_alignment(uint32_t caps, size_t *result) {
    *result = caps & MALLOC_CAP_SPIRAM ? external_cache : internal_cache;
    return ESP_OK;
}
bool efuse_hal_flash_encryption_enabled(void) { return encrypted; }

/* Stand-in for heap allocation. The hardware-adjustment hook is actual IDF code. */
void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps) {
    if (force_internal && (caps & MALLOC_CAP_SPIRAM)) return NULL;
    esp_heap_adjust_alignment_to_hw(&alignment, &size, &caps);
    if (caps & MALLOC_CAP_INVALID) return NULL;
    void *ptr = NULL;
    if (posix_memalign(&ptr, alignment, size)) return NULL;
    allocated_external = (caps & MALLOC_CAP_SPIRAM) != 0;
    pool_base = ptr;
    pool_size = size;
    return ptr;
}

#include "allocation.inc"
#if TEST_FIXED
#include "fixed.inc"
#else
#include "original.inc"
#endif

static bool esp_ptr_external_ram(const void *ptr) {
    return allocated_external && (uintptr_t)ptr >= (uintptr_t)pool_base &&
        (uintptr_t)ptr < (uintptr_t)pool_base + pool_size;
}
typedef struct {
    struct { bool (*check_buffer_alignment)(int, const void *, size_t); int slot; } host;
} sdmmc_card_t;
#define SD_ARG_CMD53_WRITE 1
#define SD_ARG_CMD53_INCREMENT 2
#define SD_ARG_CMD53_BLOCK_MODE 4
#define SDMMC_IO_FIXED_ADDR 0x80000000u
#define unlikely(x) (x)
static unsigned actual_transfers;
static esp_err_t sdmmc_io_rw_extended(sdmmc_card_t *card, uint32_t fn, uint32_t addr,
                                      uint32_t arg, void *ptr, size_t size) {
    ++actual_transfers;
    return ESP_OK;
}
#include "sdmmc.inc"

static void run_case(size_t ext, size_t internal, bool fallback, bool encryption) {
    external_cache = ext;
    internal_cache = internal;
    force_internal = fallback;
    encrypted = encryption;
    hosted_mempool_t *pool = transport_drv_common_mempool_create();
    assert(pool);
    assert(transport_drv_common_mempool_create() == pool);
    assert(allocated_external == (CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM && !fallback));
    sdmmc_card_t card = {{sdmmc_host_check_buffer_alignment, 1}};
    void *blocks[BLOCKS];
    unsigned rejected = 0;
    actual_transfers = 0;
    for (unsigned i = 0; i < BLOCKS; ++i) {
        blocks[i] = hosted_mempool_alloc(pool, ESP_TRANSPORT_MAX_BUF_SIZE, true);
        assert(blocks[i]);
        assert((uintptr_t)blocks[i] >= (uintptr_t)pool_base);
        assert((uintptr_t)blocks[i] + ESP_TRANSPORT_MAX_BUF_SIZE <= (uintptr_t)pool_base + pool_size);
        for (unsigned j = 0; j < i; ++j) assert(blocks[i] != blocks[j]);
        /* Exercise the real rejecting SDMMC path, including its early return. */
        esp_err_t err = sdmmc_io_write_blocks(&card, 1, 0, blocks[i], 512);
        assert(err == ESP_OK || err == ESP_ERR_INVALID_ARG);
        rejected += err != ESP_OK;
        memset(blocks[i], (int)i, ESP_TRANSPORT_MAX_BUF_SIZE);
    }
    assert(!hosted_mempool_alloc(pool, ESP_TRANSPORT_MAX_BUF_SIZE, false));
    assert(actual_transfers + rejected == BLOCKS);
    const size_t actual_cache = allocated_external ? ext : internal;
    if (TEST_FIXED || actual_cache <= 64) {
        assert(rejected == 0);
    } else {
        assert(rejected > 0); /* Historical 1600-byte stride reproduces 258. */
        if (actual_cache == 128) assert(rejected == BLOCKS / 2);
    }
    if (TEST_FIXED) {
        size_t required = actual_cache;
        if (encryption && required < 256) required = 256;
        if (required < 64) required = 64;
        for (unsigned i = 0; i < BLOCKS; ++i)
            assert((uintptr_t)blocks[i] % required == 0);
    }
    for (unsigned i = 0; i < BLOCKS; ++i)
        assert(hosted_mempool_free(pool, blocks[i]) == 0);
    /* A second allocation cycle catches freelist/stride regressions. */
    for (unsigned i = 0; i < BLOCKS; ++i) {
        void *ptr = hosted_mempool_alloc(pool, ESP_TRANSPORT_MAX_BUF_SIZE, false);
        assert(ptr == blocks[BLOCKS - 1 - i]);
    }
    transport_drv_common_mempool_deinit();
    transport_drv_common_mempool_deinit();
    pool_base = NULL;
}

int main(void) {
    run_case(128, 64, false, false); /* Qualified P4 configuration. */
    run_case(64, 64, false, false);  /* Old cache configuration remains valid. */
    run_case(256, 64, false, false); /* No hardcoded 128-byte dependency. */
    run_case(128, 64, true, false);  /* PSRAM allocation failure uses internal. */
    run_case(128, 128, true, false); /* Derive internal cache too. */
    run_case(0, 0, false, false);    /* No cache retains Hosted's 64-byte minimum. */
    run_case(64, 64, false, true);   /* Allocator's encryption alignment too. */
    printf("PASS: %s pool, prefer SPIRAM=%d, seven allocation scenarios\n",
           TEST_FIXED ? "fixed" : "original", CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM);
    return 0;
}
