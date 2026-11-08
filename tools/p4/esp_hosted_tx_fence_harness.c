/* Host harness for HardwareOne's restart TX fence in ESP-Hosted 2.12.13.
 *
 * test_esp_hosted_tx_fence.py applies the Hosted patch to the pristine package
 * and inserts these definitions unchanged at PRODUCTION: the fence and its
 * helpers plus the STA/AP/serial wrappers (transport_drv.c), the SDIO
 * esp_hosted_tx (sdio_drv.c) and Bluedroid's VHCI send (vhci_drv.c), with the
 * interface buffer type and H_FREE_PTR_WITH_FUNC from transport_drv.h. Only
 * the OS abstraction (allocation, queue, semaphore), the transport state, the
 * channels and the log are fakes.
 *
 * Bench 2026-11-08 (P4X-EYE, G2 glasses and R1 ring connected): 3 of 15
 * reboots panicked with a stack protection fault in Bluedroid's hciT task.
 * After the fence, each HCI packet hciT sent went down esp_hosted_tx's
 * refusal branch, whose ESP_LOGE ("tx fail ... transport_up(0)") ran vfprintf
 * on hciT's 2.5 KB stack, then hosted_hci_bluedroid_send logged "Tx failed".
 * A refusal at the fence must be silent on the producer's task (counted
 * instead); a refusal for any other reason keeps Hosted's own log.
 */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_ESP_NETIF_TX_FAILED 0x5007
#define unlikely(x) (x)
#define ESP_PKT_STATS 0

enum { ESP_STA_IF, ESP_AP_IF, ESP_SERIAL_IF, ESP_HCI_IF, ESP_PRIV_IF, ESP_TEST_IF, ESP_MAX_IF };
enum { PRIO_Q_SERIAL, PRIO_Q_BT, PRIO_Q_OTHERS, MAX_PRIORITY_QUEUES };
#define MAX_TRANSPORT_BUFFER_SIZE 1600
#define H_ESP_PAYLOAD_HEADER_OFFSET 12
#define MAX_PAYLOAD_SIZE (MAX_TRANSPORT_BUFFER_SIZE - H_ESP_PAYLOAD_HEADER_OFFSET)
#define H_BUFF_NO_ZEROCOPY 0
#define H_BUFF_ZEROCOPY 1
#define HOSTED_MEM_ALIGNMENT_64 64
#define HOSTED_BLOCK_MAX 0xFFFFFFFF

/* --- the log: every line a producer's task would print ---------------------- */
static int g_logs;
static char g_last_log[160];
static void fake_log(const char *level, const char *tag, const char *fmt, ...)
{
	++g_logs;
	snprintf(g_last_log, sizeof(g_last_log), "%s %s: %s", level, tag, fmt);
	fprintf(stderr, "  [log] %s\n", g_last_log);
}
#define ESP_LOGE(tag, fmt, ...) fake_log("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fake_log("W", tag, fmt, ##__VA_ARGS__)

/* --- allocation (Hosted's OS abstraction and the channel mempools) ---------- */
static int g_live;     /* buffers not yet freed */
static int g_mallocs;  /* _h_malloc_align calls */
static void (*g_on_malloc)(void);  /* one-shot: another task runs inside the call */
static void *fake_malloc_align(size_t size, size_t align)
{
	(void)align;
	++g_mallocs;
	if (g_on_malloc) {
		void (*hook)(void) = g_on_malloc;
		g_on_malloc = NULL;
		hook();
	}
	++g_live;
	return calloc(1, size + 1);
}
static void fake_free(void *ptr)
{
	if (ptr) {
		--g_live;
		free(ptr);
	}
}
static void *fake_memcpy(void *dst, const void *src, uint32_t size) { return memcpy(dst, src, size); }
static void *mempool_alloc(void *mempool, size_t size, unsigned need_memset)
{
	(void)mempool;
	(void)need_memset;
	++g_live;
	return calloc(1, size);
}
static void transport_sta_free_cb(void *buf) { fake_free(buf); }
static void transport_ap_free_cb(void *buf) { fake_free(buf); }
static void transport_serial_free_cb(void *buf) { fake_free(buf); }

/*@TYPES@*/

/* --- the to_slave queues the SDIO write task drains -------------------------- */
static interface_buffer_handle_t g_queue[16];
static int g_queued;
static void *to_slave_queue[MAX_PRIORITY_QUEUES];
static void *sem_to_slave_queue;
static int fake_queue_item(void *queue, void *item, int timeout)
{
	(void)queue;
	(void)timeout;
	assert(g_queued < (int)(sizeof(g_queue) / sizeof(g_queue[0])));
	memcpy(&g_queue[g_queued++], item, sizeof(interface_buffer_handle_t));
	return 0;
}
static int fake_post_semaphore(void *sem) { (void)sem; return 0; }

struct fake_funcs {
	void *(*_h_memcpy)(void *dest, const void *src, uint32_t size);
	void *(*_h_malloc_align)(size_t size, size_t align);
	void (*_h_free)(void *ptr);
	int (*_h_queue_item)(void *queue_handle, void *item, int timeout);
	int (*_h_post_semaphore)(void *semaphore_handle);
};
static struct fake_funcs g_funcs = {fake_memcpy, fake_malloc_align, fake_free, fake_queue_item,
				     fake_post_semaphore};
static struct { struct fake_funcs *funcs; } g_h = {&g_funcs};
#define H_DEFLT_FREE_FUNC g_h.funcs->_h_free

/* --- transport state and channels ------------------------------------------- */
static uint8_t g_transport_up = 1;
uint8_t is_transport_tx_ready(void) { return g_transport_up; }
struct fake_chan { void *api_chan; void *memp; };
static int g_api_chan[3];
static struct fake_chan g_chans[3] = {{&g_api_chan[0], NULL}, {&g_api_chan[1], NULL}, {&g_api_chan[2], NULL}};
static struct fake_chan *chan_arr[ESP_MAX_IF] = {&g_chans[0], &g_chans[1], &g_chans[2]};
static uint8_t wifi_tx_throttling;

int esp_hosted_tx(uint8_t iface_type, uint8_t iface_num, uint8_t *payload_buf, uint16_t payload_len,
		  uint8_t buff_zcopy, uint8_t *buffer_to_free, void (*free_buf_func)(void *ptr), uint8_t flags);

/*@PRODUCTION@*/

/* --- scenarios -------------------------------------------------------------- */
static uint8_t g_hci[31] = {0x02};

static void drain(void)
{
	for (int i = 0; i < g_queued; ++i)
		g_queue[i].free_buf_handle(g_queue[i].priv_buffer_handle);
	g_queued = 0;
}

static void begin(const char *what, uint8_t up)
{
	drain();
	assert(g_live == 0);
	g_logs = g_mallocs = 0;
	g_transport_up = up;
	fprintf(stderr, "%s\n", what);
}

static void all_producers(esp_err_t want)
{
	uint8_t *raw = fake_malloc_align(64, 64);
	assert(esp_hosted_tx(ESP_PRIV_IF, 0, raw, 64, H_BUFF_NO_ZEROCOPY, raw, fake_free, 0) == want);
	uint8_t frame[100] = {0};
	const esp_err_t netif = want == ESP_OK ? ESP_OK : ESP_ERR_ESP_NETIF_TX_FAILED;
	assert(transport_drv_sta_tx(&g_api_chan[ESP_STA_IF], frame, sizeof(frame)) == netif);
	assert(transport_drv_ap_tx(&g_api_chan[ESP_AP_IF], frame, sizeof(frame)) == netif);
	uint8_t *rpc = mempool_alloc(NULL, 48, 0);
	assert(transport_drv_serial_tx(&g_api_chan[ESP_SERIAL_IF], rpc, 48) == netif);
	if (netif != ESP_OK)
		fake_free(rpc);  /* the serial wrapper leaves a refused buffer to its caller */
}

int main(void)
{
	begin("TX open: every producer queues, nothing logs", 1);
	hosted_hci_bluedroid_send(g_hci, sizeof(g_hci));
	all_producers(ESP_OK);
	assert(g_queued == 5 && g_mallocs == 2 && g_logs == 0);

	begin("transport down, no fence: Hosted's own refusal logs stay", 0);
	hosted_hci_bluedroid_send(g_hci, sizeof(g_hci));
	assert(g_logs == 2 && strstr(g_last_log, "vhci_drv") && strstr(g_last_log, "Tx failed"));
	g_logs = 0;
	all_producers(ESP_FAIL);
	assert(g_logs == 4 && g_queued == 0);
	begin("TX open, invalid length: still logged", 1);
	uint8_t *big = fake_malloc_align(8, 64);
	assert(esp_hosted_tx(ESP_PRIV_IF, 0, big, MAX_PAYLOAD_SIZE + 1, H_BUFF_NO_ZEROCOPY, big, fake_free, 0) == ESP_FAIL);
	assert(g_logs == 1 && strstr(g_last_log, "tx fail"));
#ifdef HAVE_FENCE_DROPS
	assert(esp_hosted_hw1_tx_fence_drops() == 0);
#endif

	/* The restart sets the fence while hciT is inside hosted_hci_bluedroid_send:
	 * past its own check, before esp_hosted_tx. The packet is dropped in
	 * esp_hosted_tx, and neither function logs on hciT. */
	begin("fence closes inside a VHCI send: dropped, freed, no log", 1);
	g_on_malloc = esp_hosted_hw1_fence_tx;
	hosted_hci_bluedroid_send(g_hci, sizeof(g_hci));
	assert(!g_on_malloc && g_queued == 0);
	assert(g_logs == 0);

	begin("fenced: a VHCI send drops before any allocation, no log", 1);
	for (int i = 0; i < 50; ++i)  /* GATT discovery keeps hciT sending */
		hosted_hci_bluedroid_send(g_hci, sizeof(g_hci));
	assert(g_queued == 0 && g_logs == 0);
	assert(g_mallocs == 0);

	begin("fenced: every other producer is refused without a log", 1);
	all_producers(ESP_FAIL);
	assert(g_queued == 0 && g_logs == 0);
	begin("fenced with the transport also down: still no log", 0);
	all_producers(ESP_FAIL);
	hosted_hci_bluedroid_send(g_hci, sizeof(g_hci));
	assert(g_queued == 0 && g_logs == 0);
	begin("fenced: the open check stays closed", 1);
	assert(!esp_hosted_hw1_tx_open());
	drain();
	assert(g_live == 0);
#ifdef HAVE_FENCE_DROPS
	/* 1 inside the racing send, 50 VHCI sends, 4 producers twice, 1 more send */
	assert(esp_hosted_hw1_tx_fence_drops() == 1 + 50 + 4 + 4 + 1);
#endif
	fprintf(stderr, "PASS\n");
	return 0;
}
