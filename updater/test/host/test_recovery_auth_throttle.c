/* Host tests for the recovery HTTP authentication throttle.
 *
 * These cover the cases that are impractical on hardware: a guesser pacing
 * itself under the leak rate (7 real minutes), block escalation across three
 * strikes, and slot eviction under contention.  Time is injected, so the whole
 * suite runs in microseconds.
 */

#include "recovery_auth_throttle.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

#define SEC(n) ((int64_t)(n) * 1000000LL)

static const uint32_t PEER_A = 0x0100A8C0u; /* 192.168.0.1 */
static const uint32_t PEER_B = 0x0200A8C0u;
static const uint32_t PEER_C = 0x0300A8C0u;
static const uint32_t PEER_D = 0x0400A8C0u;
static const uint32_t PEER_E = 0x0500A8C0u;

/* A wrong credential five times must block; the fifth attempt is still
 * evaluated, the sixth is refused outright. */
static void test_blocks_after_limit(void)
{
    auth_throttle_t t;
    int64_t now = SEC(100);
    unsigned i;
    auth_throttle_reset(&t);
    for (i = 0; i < AUTH_FAIL_LIMIT; ++i) {
        CHECK(auth_throttle_begin(&t, PEER_A, now, NULL) ==
              AUTH_DECISION_EVALUATE);
        auth_throttle_record_failure(&t, PEER_A, now);
        now += SEC(1);
    }
    int64_t retry = 0;
    CHECK(auth_throttle_begin(&t, PEER_A, now, &retry) ==
          AUTH_DECISION_BLOCKED);
    CHECK(retry > 0 && retry <= AUTH_BLOCK_BASE_US);
}

/* The regression that would have locked the operator out of their own browser:
 * a header-less request is never reported as a failure, so any number of them
 * must leave the peer free. */
static void test_absent_credential_never_blocks(void)
{
    auth_throttle_t t;
    int64_t now = SEC(100);
    unsigned i;
    auth_throttle_reset(&t);
    for (i = 0; i < 50; ++i) {
        CHECK(auth_throttle_begin(&t, PEER_A, now, NULL) ==
              AUTH_DECISION_EVALUATE);
        /* caller observed AUTH_VALUE_ABSENT and records nothing */
        now += SEC(1);
    }
    CHECK(auth_throttle_begin(&t, PEER_A, now, NULL) ==
          AUTH_DECISION_EVALUATE);
}

/* One peer's block must not affect another station. */
static void test_block_is_per_peer(void)
{
    auth_throttle_t t;
    int64_t now = SEC(100);
    unsigned i;
    auth_throttle_reset(&t);
    for (i = 0; i < AUTH_FAIL_LIMIT; ++i) {
        (void)auth_throttle_begin(&t, PEER_A, now, NULL);
        auth_throttle_record_failure(&t, PEER_A, now);
    }
    CHECK(auth_throttle_begin(&t, PEER_A, now, NULL) == AUTH_DECISION_BLOCKED);
    CHECK(auth_throttle_begin(&t, PEER_B, now, NULL) == AUTH_DECISION_EVALUATE);
}

/* Laundering: a second station holding an authenticated poll open must not
 * reset anyone's failure count, including its own. */
static void test_success_does_not_launder_failures(void)
{
    auth_throttle_t t;
    int64_t now = SEC(100);
    unsigned i;
    auth_throttle_reset(&t);
    for (i = 0; i < AUTH_FAIL_LIMIT - 1; ++i) {
        (void)auth_throttle_begin(&t, PEER_A, now, NULL);
        auth_throttle_record_failure(&t, PEER_A, now);
        /* B authenticates repeatedly in between, as the page's poll does */
        (void)auth_throttle_begin(&t, PEER_B, now, NULL);
        auth_throttle_record_success(&t, PEER_B, now);
        now += SEC(3);
    }
    (void)auth_throttle_begin(&t, PEER_A, now, NULL);
    auth_throttle_record_failure(&t, PEER_A, now);
    CHECK(auth_throttle_begin(&t, PEER_A, now, NULL) == AUTH_DECISION_BLOCKED);
}

/* A peer's own success must not clear its accumulated failures either. */
static void test_own_success_does_not_clear(void)
{
    auth_throttle_t t;
    int64_t now = SEC(100);
    unsigned i;
    auth_throttle_reset(&t);
    for (i = 0; i < AUTH_FAIL_LIMIT - 1; ++i) {
        (void)auth_throttle_begin(&t, PEER_A, now, NULL);
        auth_throttle_record_failure(&t, PEER_A, now);
    }
    (void)auth_throttle_begin(&t, PEER_A, now, NULL);
    auth_throttle_record_success(&t, PEER_A, now);
    (void)auth_throttle_begin(&t, PEER_A, now, NULL);
    auth_throttle_record_failure(&t, PEER_A, now);
    CHECK(auth_throttle_begin(&t, PEER_A, now, NULL) == AUTH_DECISION_BLOCKED);
}

/* The defect the old sliding window had: one guess per 61 s never accumulated.
 * Under the leak schedule it must eventually block. */
static void test_slow_guesser_still_blocks(void)
{
    auth_throttle_t t;
    int64_t now = SEC(100);
    unsigned i;
    auth_throttle_reset(&t);
    for (i = 0; i < AUTH_FAIL_LIMIT; ++i) {
        if (i != 0) {
            now += SEC(61); /* slower than the old 60 s window, which reset */
        }
        CHECK(auth_throttle_begin(&t, PEER_A, now, NULL) ==
              AUTH_DECISION_EVALUATE);
        auth_throttle_record_failure(&t, PEER_A, now);
    }
    /* The whole point: 244 s of elapsed time is well under one leak period, so
     * nothing was forgiven and the fifth failure blocks.  The old sliding
     * window restarted every 60 s and never reached the limit at this pace. */
    CHECK(auth_throttle_begin(&t, PEER_A, now, NULL) == AUTH_DECISION_BLOCKED);
}

/* Follow-on property, discovered while writing the test above: a guesser pacing
 * itself slower than the CURRENT block duration serves that block out between
 * attempts.  Escalation is what actually contains them - once the block exceeds
 * their pacing, they are throttled continuously rather than intermittently. */
static void test_escalation_outruns_a_paced_guesser(void)
{
    auth_throttle_t t;
    int64_t now = SEC(100);
    unsigned round;
    bool blocked_across_gap = false;
    auth_throttle_reset(&t);
    for (round = 0; round < 4 && !blocked_across_gap; ++round) {
        unsigned i;
        for (i = 0; i < AUTH_FAIL_LIMIT; ++i) {
            if (auth_throttle_begin(&t, PEER_A, now, NULL) ==
                AUTH_DECISION_EVALUATE) {
                auth_throttle_record_failure(&t, PEER_A, now);
            }
            now += SEC(61);
        }
        blocked_across_gap =
            auth_throttle_begin(&t, PEER_A, now, NULL) == AUTH_DECISION_BLOCKED;
    }
    CHECK(blocked_across_gap);
}

/* Leak decay must genuinely forgive: pacing under the leak rate stays free. */
static void test_leak_forgives_slow_enough_traffic(void)
{
    auth_throttle_t t;
    int64_t now = SEC(100);
    unsigned i;
    auth_throttle_reset(&t);
    for (i = 0; i < 12; ++i) {
        CHECK(auth_throttle_begin(&t, PEER_A, now, NULL) ==
              AUTH_DECISION_EVALUATE);
        auth_throttle_record_failure(&t, PEER_A, now);
        now += AUTH_LEAK_PERIOD_US + SEC(1);
    }
    CHECK(auth_throttle_begin(&t, PEER_A, now, NULL) ==
          AUTH_DECISION_EVALUATE);
}

/* Blocks escalate 30s -> 60s -> 120s across strikes. */
static void test_block_escalates(void)
{
    auth_throttle_t t;
    int64_t now = SEC(100);
    int64_t expected = AUTH_BLOCK_BASE_US;
    unsigned round;
    auth_throttle_reset(&t);
    for (round = 0; round < 3; ++round) {
        unsigned i;
        for (i = 0; i < AUTH_FAIL_LIMIT; ++i) {
            (void)auth_throttle_begin(&t, PEER_A, now, NULL);
            auth_throttle_record_failure(&t, PEER_A, now);
        }
        int64_t retry = 0;
        CHECK(auth_throttle_begin(&t, PEER_A, now, &retry) ==
              AUTH_DECISION_BLOCKED);
        CHECK(retry == expected);
        now += expected + SEC(1); /* serve out the block */
        CHECK(auth_throttle_begin(&t, PEER_A, now, NULL) ==
              AUTH_DECISION_EVALUATE);
        expected *= 2;
    }
}

/* An attacker rotating source addresses must not be able to evict the
 * operator's known-good slot, nor escape their own block by cycling. */
static void test_eviction_protects_operator_and_blocked(void)
{
    auth_throttle_t t;
    int64_t now = SEC(100);
    unsigned i;
    auth_throttle_reset(&t);

    /* Operator authenticates from A. */
    (void)auth_throttle_begin(&t, PEER_A, now, NULL);
    auth_throttle_record_success(&t, PEER_A, now);

    /* B gets itself blocked. */
    for (i = 0; i < AUTH_FAIL_LIMIT; ++i) {
        (void)auth_throttle_begin(&t, PEER_B, now, NULL);
        auth_throttle_record_failure(&t, PEER_B, now);
    }
    CHECK(auth_throttle_begin(&t, PEER_B, now, NULL) == AUTH_DECISION_BLOCKED);

    /* Fill the rest, then churn new addresses through the table. */
    (void)auth_throttle_begin(&t, PEER_C, now, NULL);
    (void)auth_throttle_begin(&t, PEER_D, now, NULL);
    now += SEC(1);
    for (i = 0; i < 20; ++i) {
        (void)auth_throttle_begin(&t, PEER_E + i, now, NULL);
        now += SEC(1);
    }

    /* B must still be blocked - cycling addresses cannot clear it. */
    CHECK(auth_throttle_begin(&t, PEER_B, now, NULL) == AUTH_DECISION_BLOCKED);
    /* The operator must still be able to authenticate. */
    CHECK(auth_throttle_begin(&t, PEER_A, now, NULL) == AUTH_DECISION_EVALUATE);
}

/* A failed peer-address lookup yields 0; it must occupy its own bucket rather
 * than aliasing every free slot. */
static void test_zero_address_is_its_own_bucket(void)
{
    auth_throttle_t t;
    int64_t now = SEC(100);
    unsigned i;
    uint32_t blocked = 0;
    auth_throttle_reset(&t);
    for (i = 0; i < AUTH_FAIL_LIMIT; ++i) {
        (void)auth_throttle_begin(&t, 0u, now, NULL);
        auth_throttle_record_failure(&t, 0u, now);
    }
    CHECK(auth_throttle_begin(&t, 0u, now, NULL) == AUTH_DECISION_BLOCKED);
    /* A real peer is unaffected by the unknown-address bucket. */
    CHECK(auth_throttle_begin(&t, PEER_A, now, NULL) == AUTH_DECISION_EVALUATE);
    auth_throttle_stats(&t, now, &blocked, NULL);
    CHECK(blocked == 1);
}

/* Stats feed the operator-visible /status fields. */
static void test_stats_report_totals(void)
{
    auth_throttle_t t;
    int64_t now = SEC(100);
    uint32_t blocked = 0;
    uint32_t total = 0;
    unsigned i;
    auth_throttle_reset(&t);
    for (i = 0; i < 3; ++i) {
        (void)auth_throttle_begin(&t, PEER_A, now, NULL);
        auth_throttle_record_failure(&t, PEER_A, now);
    }
    auth_throttle_stats(&t, now, &blocked, &total);
    CHECK(blocked == 0);
    CHECK(total == 3);
}

/* Shared by the HTTP handler and the serial console, so it is worth pinning
 * the boundary cases rather than assuming. */
static void test_constant_time_equals(void)
{
    CHECK(auth_constant_time_equals("hunter2hunter2", "hunter2hunter2"));
    CHECK(!auth_constant_time_equals("hunter2hunter2", "hunter2hunter3"));
    /* length differences must not compare equal on a common prefix */
    CHECK(!auth_constant_time_equals("hunter2", "hunter2hunter2"));
    CHECK(!auth_constant_time_equals("hunter2hunter2", "hunter2"));
    /* empty and NULL are handled without crashing, and are not equal to a
     * real credential - the case that matters when NVS read fails */
    CHECK(!auth_constant_time_equals("", "hunter2hunter2"));
    CHECK(!auth_constant_time_equals(NULL, "hunter2hunter2"));
    CHECK(!auth_constant_time_equals("hunter2hunter2", NULL));
    CHECK(auth_constant_time_equals(NULL, NULL));
    CHECK(auth_constant_time_equals("", ""));
}

int main(void)
{
    test_constant_time_equals();
    test_blocks_after_limit();
    test_absent_credential_never_blocks();
    test_block_is_per_peer();
    test_success_does_not_launder_failures();
    test_own_success_does_not_clear();
    test_slow_guesser_still_blocks();
    test_escalation_outruns_a_paced_guesser();
    test_leak_forgives_slow_enough_traffic();
    test_block_escalates();
    test_eviction_protects_operator_and_blocked();
    test_zero_address_is_its_own_bucket();
    test_stats_report_totals();
    if (g_failures != 0) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all recovery auth throttle tests passed\n");
    return 0;
}
