#pragma once

/* Per-peer authentication throttle for the recovery HTTP server.
 *
 * Deliberately free of ESP-IDF dependencies: `now` is passed in rather than
 * read from esp_timer, and there is no logging or socket type here.  That keeps
 * the whole state machine host-testable, which matters because its interesting
 * cases - a guesser pacing itself under the leak rate, block escalation, and
 * eviction under contention - take minutes to reproduce on hardware and
 * microseconds to reproduce on a workstation.
 *
 * The design it replaces was a single global counter, which had three defects
 * that all matter on a two-station AP: one attacker locked out the operator,
 * every SUCCESS cleared the counter (and the served page polls /status every
 * 3 s, laundering another station's failures ~20x a minute), and the sliding
 * window restarted whenever it elapsed, so one guess per 61 s never
 * accumulated.  A leaky bucket keyed per peer fixes all three.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUTH_PEER_SLOTS 4u
#define AUTH_FAIL_LIMIT 5u
/* Forgive one failure per 10 minutes.  This is what eventually catches the slow
 * guesser that a restarting window never accumulated. */
#define AUTH_LEAK_PERIOD_US (600LL * 1000000LL)
#define AUTH_BLOCK_BASE_US (30LL * 1000000LL)
#define AUTH_BLOCK_MAX_US (1800LL * 1000000LL)
/* A peer that authenticated recently is "known good": exempt from the global
 * backstop and holding a reserved slot, so an attacker rotating source
 * addresses can neither evict the operator nor lock them out. */
#define AUTH_KNOWN_GOOD_US (600LL * 1000000LL)
#define AUTH_GLOBAL_WINDOW_US (60LL * 1000000LL)
#define AUTH_GLOBAL_LIMIT 20u

typedef struct {
    /* in_use, not addr != 0, marks occupancy: a failed peer-address lookup
     * yields 0, and that must land in its own bucket rather than aliasing
     * every free slot. */
    bool in_use;
    uint32_t addr;
    int64_t last_seen_us;
    int64_t last_success_us;
    int64_t last_leak_us;
    int64_t blocked_until_us;
    uint32_t failures;
    uint32_t strikes;
} auth_peer_t;

typedef struct {
    auth_peer_t peers[AUTH_PEER_SLOTS];
    uint32_t failures_total;
    uint32_t global_failures;
    int64_t global_window_us;
} auth_throttle_t;

typedef enum {
    /* Caller should evaluate the credential. */
    AUTH_DECISION_EVALUATE = 0,
    /* Caller must answer 429 without looking at the credential at all. */
    AUTH_DECISION_BLOCKED,
} auth_decision_t;

/* Length-and-content comparison whose timing does not depend on how many
 * leading bytes matched. Shared so the HTTP handler and the serial console
 * cannot drift into having one careful comparison and one naive one. */
bool auth_constant_time_equals(const char *left, const char *right);

void auth_throttle_reset(auth_throttle_t *throttle);

/* Claims (or creates) this peer's slot and applies leak decay.  On
 * AUTH_DECISION_BLOCKED, *retry_after_us receives a positive backoff hint. */
auth_decision_t auth_throttle_begin(auth_throttle_t *throttle, uint32_t addr,
                                    int64_t now, int64_t *retry_after_us);

/* Record a credential that was PRESENT and WRONG.  A missing credential must
 * never be reported here: every browser's first request to every path is
 * header-less by design, and counting that locks out the operator's own
 * browser. */
void auth_throttle_record_failure(auth_throttle_t *throttle, uint32_t addr,
                                  int64_t now);

/* Marks the peer known-good.  Deliberately does NOT clear its accumulated
 * failures - they decay on the leak schedule instead - so a valid session
 * cannot launder a concurrent guessing attempt. */
void auth_throttle_record_success(auth_throttle_t *throttle, uint32_t addr,
                                  int64_t now);

void auth_throttle_stats(const auth_throttle_t *throttle, int64_t now,
                         uint32_t *blocked_peers, uint32_t *failures_total);

#ifdef __cplusplus
}
#endif
