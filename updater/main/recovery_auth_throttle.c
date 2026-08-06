#include "recovery_auth_throttle.h"

#include <string.h>

bool auth_constant_time_equals(const char *left, const char *right)
{
    size_t left_len = left == NULL ? 0 : strlen(left);
    size_t right_len = right == NULL ? 0 : strlen(right);
    size_t max_len = left_len > right_len ? left_len : right_len;
    unsigned difference = (unsigned)(left_len ^ right_len);
    size_t i;
    for (i = 0; i < max_len; ++i) {
        unsigned l = i < left_len ? (unsigned char)left[i] : 0;
        unsigned r = i < right_len ? (unsigned char)right[i] : 0;
        difference |= l ^ r;
    }
    return difference == 0;
}

static bool peer_is_known_good(const auth_peer_t *peer, int64_t now)
{
    return peer->last_success_us != 0 &&
           now - peer->last_success_us <= AUTH_KNOWN_GOOD_US;
}

/* Decay: forgive one accumulated failure per AUTH_LEAK_PERIOD_US.  Failures are
 * never cleared outright, which is what makes a slow guesser eventually trip. */
static void peer_leak(auth_peer_t *peer, int64_t now)
{
    if (peer->last_leak_us == 0) {
        peer->last_leak_us = now;
        return;
    }
    while (peer->failures > 0 &&
           now - peer->last_leak_us >= AUTH_LEAK_PERIOD_US) {
        --peer->failures;
        peer->last_leak_us += AUTH_LEAK_PERIOD_US;
    }
    if (peer->failures == 0) {
        peer->last_leak_us = now;
    }
}

/* Returns NULL only when every slot holds something we refuse to evict, in
 * which case the caller denies.  The two eviction rules exist to stop an
 * attacker from escaping their own block or displacing the operator:
 *   - never evict a peer that is currently blocked
 *   - never evict the single most recent known-good peer */
static auth_peer_t *peer_slot(auth_throttle_t *throttle, uint32_t addr,
                              int64_t now)
{
    auth_peer_t *victim = NULL;
    const auth_peer_t *keep_good = NULL;
    size_t i;
    for (i = 0; i < AUTH_PEER_SLOTS; ++i) {
        if (throttle->peers[i].in_use && throttle->peers[i].addr == addr) {
            throttle->peers[i].last_seen_us = now;
            return &throttle->peers[i];
        }
    }
    for (i = 0; i < AUTH_PEER_SLOTS; ++i) {
        if (!throttle->peers[i].in_use) {
            auth_peer_t *fresh = &throttle->peers[i];
            memset(fresh, 0, sizeof(*fresh));
            fresh->in_use = true;
            fresh->addr = addr;
            fresh->last_seen_us = now;
            fresh->last_leak_us = now;
            return fresh;
        }
    }
    for (i = 0; i < AUTH_PEER_SLOTS; ++i) {
        const auth_peer_t *candidate = &throttle->peers[i];
        if (!candidate->in_use || !peer_is_known_good(candidate, now)) {
            continue;
        }
        if (keep_good == NULL ||
            candidate->last_success_us > keep_good->last_success_us) {
            keep_good = candidate;
        }
    }
    for (i = 0; i < AUTH_PEER_SLOTS; ++i) {
        auth_peer_t *candidate = &throttle->peers[i];
        if (now < candidate->blocked_until_us || candidate == keep_good) {
            continue;
        }
        if (victim == NULL || candidate->last_seen_us < victim->last_seen_us) {
            victim = candidate;
        }
    }
    if (victim == NULL) {
        return NULL;
    }
    memset(victim, 0, sizeof(*victim));
    victim->in_use = true;
    victim->addr = addr;
    victim->last_seen_us = now;
    victim->last_leak_us = now;
    return victim;
}

static auth_peer_t *peer_find(auth_throttle_t *throttle, uint32_t addr)
{
    size_t i;
    for (i = 0; i < AUTH_PEER_SLOTS; ++i) {
        if (throttle->peers[i].in_use && throttle->peers[i].addr == addr) {
            return &throttle->peers[i];
        }
    }
    return NULL;
}

void auth_throttle_reset(auth_throttle_t *throttle)
{
    if (throttle != NULL) {
        memset(throttle, 0, sizeof(*throttle));
    }
}

auth_decision_t auth_throttle_begin(auth_throttle_t *throttle, uint32_t addr,
                                    int64_t now, int64_t *retry_after_us)
{
    auth_peer_t *peer;
    if (retry_after_us != NULL) {
        *retry_after_us = AUTH_BLOCK_BASE_US;
    }
    if (throttle == NULL) {
        return AUTH_DECISION_BLOCKED;
    }
    peer = peer_slot(throttle, addr, now);
    if (peer == NULL) {
        /* Every slot is blocked or reserved for the operator.  Refuse rather
         * than evict something we promised to keep. */
        return AUTH_DECISION_BLOCKED;
    }
    peer_leak(peer, now);
    if (now < peer->blocked_until_us) {
        if (retry_after_us != NULL) {
            *retry_after_us = peer->blocked_until_us - now;
        }
        return AUTH_DECISION_BLOCKED;
    }
    return AUTH_DECISION_EVALUATE;
}

void auth_throttle_record_failure(auth_throttle_t *throttle, uint32_t addr,
                                  int64_t now)
{
    auth_peer_t *peer;
    if (throttle == NULL) {
        return;
    }
    ++throttle->failures_total;
    if (throttle->global_window_us == 0 ||
        now - throttle->global_window_us > AUTH_GLOBAL_WINDOW_US) {
        throttle->global_window_us = now;
        throttle->global_failures = 0;
    }
    ++throttle->global_failures;
    peer = peer_find(throttle, addr);
    if (peer == NULL) {
        return;
    }
    ++peer->failures;
    if (peer->failures >= AUTH_FAIL_LIMIT ||
        (throttle->global_failures >= AUTH_GLOBAL_LIMIT &&
         !peer_is_known_good(peer, now))) {
        int64_t block = AUTH_BLOCK_BASE_US;
        uint32_t strike;
        for (strike = 0; strike < peer->strikes && block < AUTH_BLOCK_MAX_US;
             ++strike) {
            block *= 2;
        }
        if (block > AUTH_BLOCK_MAX_US) {
            block = AUTH_BLOCK_MAX_US;
        }
        peer->blocked_until_us = now + block;
        peer->failures = 0;
        peer->last_leak_us = now;
        if (peer->strikes < 16u) {
            ++peer->strikes;
        }
    }
}

void auth_throttle_record_success(auth_throttle_t *throttle, uint32_t addr,
                                  int64_t now)
{
    auth_peer_t *peer;
    if (throttle == NULL) {
        return;
    }
    peer = peer_find(throttle, addr);
    if (peer == NULL) {
        return;
    }
    peer->last_success_us = now;
    peer->strikes = 0;
}

void auth_throttle_stats(const auth_throttle_t *throttle, int64_t now,
                         uint32_t *blocked_peers, uint32_t *failures_total)
{
    uint32_t blocked = 0;
    size_t i;
    if (throttle != NULL) {
        for (i = 0; i < AUTH_PEER_SLOTS; ++i) {
            if (throttle->peers[i].in_use &&
                now < throttle->peers[i].blocked_until_us) {
                ++blocked;
            }
        }
    }
    if (blocked_peers != NULL) {
        *blocked_peers = blocked;
    }
    if (failures_total != NULL) {
        *failures_total = throttle != NULL ? throttle->failures_total : 0;
    }
}
