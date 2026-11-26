/* Host harness for btm_ble_conn_complete's failure branch (ESP-IDF 5.5.5,
 * components/bt/host/bluedroid/stack/btm/btm_ble.c).
 *
 * test_idf_bt_local_patches.py inserts the branch unchanged at PRODUCTION:
 * from `BOOLEAN bg_con = btm_ble_update_mode_operation(...)` to the end of the
 * function, taken from the pristine file and from the file with
 * idf-bt-failed-conn-handle.patch applied. Only the link and security-record
 * lookups, the two clean-up calls and the trace are fakes.
 *
 * P4X-EYE 2026-11 (G2 glasses + R1 ring): the ring's direct connect reached
 * Bluedroid's 30 s establishment timeout, the C6 controller answered the
 * cancel with a failed LE Connection Complete (status 0x02, handle 0x0000),
 * and the branch released the G2 LEFT arm, which held handle 0, without an
 * HCI_Disconnect.
 *
 * Prints one line per scenario: "<name> disc=<handle> sec=<handle> warn=<n>"
 * (-1 = not called). */
#include <stdio.h>
#include <string.h>

typedef unsigned char UINT8;
typedef unsigned short UINT16;
typedef unsigned int UINT32;
typedef UINT8 BOOLEAN;
typedef UINT8 BD_ADDR[6];

#define TRUE 1
#define FALSE 0
#define HCI_SUCCESS 0x00
#define HCI_INVALID_HANDLE 0xFFFF
#define SMP_INCLUDED TRUE

typedef struct {
    BOOLEAN in_use;
    UINT16 handle;
} tL2C_LCB;

typedef struct {
    BOOLEAN in_use;
    UINT16 ble_hci_handle;
} tBTM_SEC_DEV_REC;

static tL2C_LCB lcbs[4];
static tBTM_SEC_DEV_REC recs[4];
static BOOLEAN bg_con_result;
static int disc_handle, sec_handle, warnings;

static tL2C_LCB *l2cu_find_lcb_by_handle(UINT16 handle)
{
    for (int i = 0; i < 4; i++) {
        if (lcbs[i].in_use && lcbs[i].handle == handle) {
            return &lcbs[i];
        }
    }
    return NULL;
}

static tBTM_SEC_DEV_REC *btm_find_dev_by_handle(UINT16 handle)
{
    for (int i = 0; i < 4; i++) {
        if (recs[i].in_use && recs[i].ble_hci_handle == handle) {
            return &recs[i];
        }
    }
    return NULL;
}

static BOOLEAN btm_ble_update_mode_operation(UINT8 role, UINT8 *bda, UINT8 status)
{
    (void)role;
    (void)bda;
    (void)status;
    return bg_con_result;
}

static BOOLEAN l2c_link_hci_disc_comp(UINT16 handle, UINT8 reason)
{
    (void)reason;
    disc_handle = handle;
    return l2cu_find_lcb_by_handle(handle) != NULL;
}

static BOOLEAN btm_sec_disconnected(UINT16 handle, UINT8 reason)
{
    (void)reason;
    sec_handle = handle;
    return btm_find_dev_by_handle(handle) != NULL;
}

#define BTM_TRACE_WARNING(...) (warnings++)

static void failure_branch(UINT8 status, UINT16 handle)
{
    UINT8 role = 0;
    BD_ADDR bda = {0};
/* PRODUCTION */
}

static void reset(void)
{
    memset(lcbs, 0, sizeof(lcbs));
    memset(recs, 0, sizeof(recs));
    bg_con_result = FALSE;
    disc_handle = sec_handle = -1;
    warnings = 0;
}

static void report(const char *name)
{
    printf("%s disc=%d sec=%d warn=%d\n", name, disc_handle, sec_handle, warnings);
}

int main(void)
{
    /* The bench case: LEFT arm live on handle 0 (LCB + security record), the
     * ring's LCB still connecting on HCI_INVALID_HANDLE; cancel answer. */
    reset();
    lcbs[0] = (tL2C_LCB){TRUE, 0x0000};
    lcbs[1] = (tL2C_LCB){TRUE, HCI_INVALID_HANDLE};
    recs[0] = (tBTM_SEC_DEV_REC){TRUE, 0x0000};
    failure_branch(0x02, 0x0000);
    report("live_handle0");

    /* Only a security record still holds the handle. */
    reset();
    recs[0] = (tBTM_SEC_DEV_REC){TRUE, 0x0003};
    failure_branch(0x02, 0x0003);
    report("sec_only");

    /* No link holds the handle: stock behaviour. */
    reset();
    lcbs[0] = (tL2C_LCB){TRUE, 0x0001};
    failure_branch(0x02, 0x0000);
    report("no_live");

    /* A controller that reports HCI_INVALID_HANDLE: the connecting LCB is
     * still found by it, as before. */
    reset();
    lcbs[0] = (tL2C_LCB){TRUE, HCI_INVALID_HANDLE};
    failure_branch(0x3E, HCI_INVALID_HANDLE);
    report("invalid_handle");

    /* A background connection absorbs the failure: no clean-up calls. */
    reset();
    lcbs[0] = (tL2C_LCB){TRUE, 0x0000};
    bg_con_result = TRUE;
    failure_branch(0x02, 0x0000);
    report("bg_con");

    /* Success never enters the branch. */
    reset();
    lcbs[0] = (tL2C_LCB){TRUE, 0x0000};
    failure_branch(HCI_SUCCESS, 0x0000);
    report("success");
    return 0;
}
