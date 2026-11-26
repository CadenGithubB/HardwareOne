/* Host harness for HardwareOne's unknown-handle ACL safety net in Bluedroid's
 * L2CAP (ESP-IDF 5.5.5 components/bt/host/bluedroid/stack/l2cap/l2c_main.c
 * with idf-bt-unknown-handle-acl.patch applied).
 *
 * test_idf_bt_local_patches.py inserts the patch's state, constants and its
 * helper (hw1_l2c_unknown_handle_acl) unchanged at PRODUCTION. Only the clock,
 * BTM's ACL lookup, the HCI command and the trace are fakes.
 *
 * A link the host forgot while the controller kept it (the failed connection
 * complete on a live handle, idf-bt-failed-conn-handle.patch) floods "rcvd ACL
 * for unknown handle" forever in stock Bluedroid; no API can disconnect it.
 * Prints "ok" or the first failed expectation. */
#include <stdio.h>
#include <string.h>

typedef unsigned char UINT8;
typedef unsigned short UINT16;
typedef unsigned int UINT32;
typedef UINT8 BOOLEAN;

#define TRUE 1
#define FALSE 0
#define HCI_ERR_PEER_USER 0x13

typedef struct {
    int unused;
} tACL_CONN;

static UINT32 now_ms;
static UINT32 osi_time_get_os_boottime_ms(void) { return now_ms; }

static tACL_CONN known_acl;
static int acl_known_handle = -1;
static tACL_CONN *btm_handle_to_acl(UINT16 handle)
{
    return handle == acl_known_handle ? &known_acl : NULL;
}

static int disconnects, last_disc_handle = -1, last_disc_reason = -1, warnings;
static BOOLEAN btsnd_hcic_disconnect(UINT16 handle, UINT8 reason)
{
    disconnects++;
    last_disc_handle = handle;
    last_disc_reason = reason;
    return TRUE;
}

#define L2CAP_TRACE_WARNING(...) (warnings++)

/* PRODUCTION */

static int failures;
#define EXPECT(cond, what) do { if (!(cond)) { printf("FAIL %s (line %d)\n", what, __LINE__); failures++; } } while (0)

static void reset(void)
{
    memset(hw1_ghost_handles, 0, sizeof(hw1_ghost_handles));
    acl_known_handle = -1;
    disconnects = warnings = 0;
    last_disc_handle = last_disc_reason = -1;
    now_ms = 100000;
}

int main(void)
{
    UINT32 logged;

    /* Burst: the third packet within 1 s disconnects, once. */
    reset();
    EXPECT(hw1_l2c_unknown_handle_acl(0) == 1, "first packet is logged with its count");
    now_ms += 100;
    EXPECT(hw1_l2c_unknown_handle_acl(0) == 0, "second packet is not logged");
    EXPECT(disconnects == 0, "two packets do not disconnect");
    now_ms += 100;
    hw1_l2c_unknown_handle_acl(0);
    EXPECT(disconnects == 1 && last_disc_handle == 0 && last_disc_reason == HCI_ERR_PEER_USER,
           "third packet within 1 s sends HCI_Disconnect(0, 0x13)");
    EXPECT(warnings == 1, "one warning per disconnect");
    for (int i = 0; i < 40; i++) {      /* 4.8 s more of flooding */
        now_ms += 120;
        hw1_l2c_unknown_handle_acl(0);
    }
    EXPECT(disconnects == 1, "no second disconnect within 5 s");
    for (int i = 0; i < 3; i++) {       /* 5.1 s on: a fresh burst */
        now_ms += 100;
        hw1_l2c_unknown_handle_acl(0);
    }
    EXPECT(disconnects == 2, "disconnect repeats after 5 s while the flood goes on");

    /* Log rate limit: first and every 100th packet per handle. */
    reset();
    logged = 0;
    for (int i = 1; i <= 250; i++) {
        now_ms += 2000;
        if (hw1_l2c_unknown_handle_acl(7)) {
            logged++;
        }
    }
    EXPECT(logged == 3, "logged at packets 1, 100 and 200");
    /* 500 s of one packet every 2 s: one disconnect per 5 s gap at most. */
    EXPECT(disconnects >= 1 && disconnects <= 500 / 5 + 1, "a slow ghost is disconnected, rate-limited");

    /* The bench ghost: one LE Connection Parameter Update Request every
     * ~39 s on handle 0. The second sighting disconnects. */
    reset();
    EXPECT(hw1_l2c_unknown_handle_acl(0) == 1, "first sighting is logged");
    EXPECT(disconnects == 0, "a single packet never disconnects");
    now_ms += 39000;
    hw1_l2c_unknown_handle_acl(0);
    EXPECT(disconnects == 1 && last_disc_handle == 0 && last_disc_reason == HCI_ERR_PEER_USER,
           "the second packet 39 s later sends HCI_Disconnect(0, 0x13)");
    now_ms += 39000;
    hw1_l2c_unknown_handle_acl(0);
    EXPECT(disconnects == 2, "still there 39 s later: disconnect again");

    /* A short trickle under 2 s (a late notification or two after a real
     * disconnect) never disconnects. */
    reset();
    for (int i = 0; i < 2; i++) {
        hw1_l2c_unknown_handle_acl(1);
        now_ms += 900;
    }
    EXPECT(disconnects == 0, "two packets 900 ms apart do not disconnect");
    hw1_l2c_unknown_handle_acl(1);      /* 1.8 s after the first */
    EXPECT(disconnects == 0, "three packets over 1.8 s, under 3 in any 1 s window, do not disconnect");
    now_ms += 300;
    hw1_l2c_unknown_handle_acl(1);      /* 2.1 s after the first */
    EXPECT(disconnects == 1 && last_disc_handle == 1, "still sending 2 s on: disconnect");

    /* BTM still knows the handle: leave it to BTM. */
    reset();
    acl_known_handle = 4;
    for (int i = 0; i < 5; i++) {
        hw1_l2c_unknown_handle_acl(4);
        now_ms += 50;
    }
    EXPECT(disconnects == 0, "no disconnect for a handle BTM has an ACL entry for");

    /* More handles than slots: the least recently seen slot is reused and
     * the tracked handles keep their state. */
    reset();
    for (UINT16 h = 10; h < 14; h++) {
        hw1_l2c_unknown_handle_acl(h);
        now_ms += 10;
    }
    hw1_l2c_unknown_handle_acl(11);     /* 10 is now the oldest */
    now_ms += 10;
    EXPECT(hw1_l2c_unknown_handle_acl(20) == 1, "a fifth handle gets a slot");
    EXPECT(hw1_l2c_unknown_handle_acl(11) == 0, "handle 11 kept its count");
    EXPECT(hw1_l2c_unknown_handle_acl(10) == 1, "handle 10 was evicted and starts again");

    /* A handle quiet for 60 s starts a new episode (logged again). */
    reset();
    hw1_l2c_unknown_handle_acl(5);
    now_ms += 61000;
    EXPECT(hw1_l2c_unknown_handle_acl(5) == 1, "logged again after 60 s of quiet");

    if (failures == 0) {
        printf("ok\n");
    }
    return failures ? 1 : 0;
}
