#!/usr/bin/env python3
"""
Accessory-presence policy: host-side model and simulator.

This is the DESIGN of the state machine proposed in
docs/G2_CASE_PRESENCE_POWER_PLAN.md, written as plain Python so it can be
argued about, stepped through and unit-tested before any firmware is touched.
It takes no OS or BLE dependencies, the same discipline as G2_RepairCore.h,
so the firmware version can later be host-tested the same way.

Inputs are the signals the firmware already has or that the plan adds:
  * glasses: link up/down per temple, the renderer heartbeat worn/not-worn
    edge (SYSEVT_G2_PLUGIN_ALIVE / SYSEVT_G2_PLUGIN_SILENT), settings chargingStatus,
    sid 0x81 case info (inCaseStatus once the polarity is known)
  * ring: link up/down, wear 1=notWear 2=wear (R1_SUB_WEAR_STATUS); its
    "in case" is the charger-dock report
  * user intent: an explicit open/close always wins

Outputs are ACTIONS the firmware would take, never radio calls:
  * HOLD            keep the link as it is (glasses' own idle machine runs)
  * QUIET           slow our own periodic traffic to that peer: the EvenCore
                    heartbeat drops from 5 s to a slow cadence (60 s proposed)
                    but NEVER stops, because its strict ACK is the only worn
                    edge we have; base heartbeat, status pushes and
                    notification renders stop
  * PARK            drop the link on purpose and reseek on a slow schedule.
                    ONLY from IN_CASE (user decision 2026-10-29): glasses
                    that are merely set down, or charging on a cable, keep
                    their link and are never parked
  * RESEEK_NOW      the parked peer probably came back: scan now
  * RECONNECT_FAST  the link dropped while the peer was NOT marked in case:
                    the policy steps aside and the registry's existing
                    aggressive auto-reconnect / one-temple repair runs as
                    it does today. The slow back-off applies only after an
                    in-case park.

Run:  python3 tools/g2/g2_presence_policy_sim.py          # scenario traces
      python3 tools/g2/g2_presence_policy_sim.py --test   # unit tests
"""
from __future__ import annotations

import argparse
import sys
import unittest
from dataclasses import dataclass, field
from enum import Enum, auto
from typing import List, Optional, Tuple


class Presence(Enum):
    UNKNOWN = auto()     # link up, no evidence yet
    WORN = auto()        # on head / on finger
    SET_DOWN = auto()    # off body, not in the case (desk, nightstand, cable)
    IN_CASE = auto()     # seated in the case / docked on the charger
    PARKED = auto()      # we dropped the link on purpose (only from IN_CASE)
    USER_CLOSED = auto() # user said close; policy stays out of it


class Action(Enum):
    HOLD = auto()
    QUIET = auto()
    PARK = auto()
    RESEEK_NOW = auto()
    RECONNECT_FAST = auto()


@dataclass
class Policy:
    """Tunables. Defaults are the plan's proposals, not measurements."""
    set_down_quiet_after_s: int = 120      # SET_DOWN this long -> QUIET (charging: at once)
    in_case_park_after_s: int = 5 * 60     # IN_CASE this long -> PARK (0 = never)
    reseek_period_s: int = 10 * 60         # first scan after PARK
    reseek_max_period_s: int = 60 * 60     # back-off ceiling: period doubles to this
    park_min_battery_pct: int = 0          # never park below (0 = ignore)
    allow_park: bool = True                # master switch (setting)


@dataclass
class PeerState:
    name: str
    presence: Presence = Presence.UNKNOWN
    since_s: int = 0
    link_up: bool = False
    quiet: bool = False
    charging: bool = False
    next_reseek_s: Optional[int] = None
    reseek_period_s: int = 0
    battery_pct: Optional[int] = None
    log: List[Tuple[int, str]] = field(default_factory=list)

    def _enter(self, now: int, p: Presence, why: str) -> None:
        if p != self.presence:
            self.log.append((now, f"{self.presence.name} -> {p.name} ({why})"))
            self.presence = p
            self.since_s = now

    def elapsed(self, now: int) -> int:
        return now - self.since_s


class PresencePolicy:
    def __init__(self, policy: Policy) -> None:
        self.policy = policy

    # ---- evidence -----------------------------------------------------
    def on_link(self, s: PeerState, now: int, up: bool) -> List[Action]:
        s.link_up = up
        if up:
            s.quiet = False
            s.next_reseek_s = None
            if s.presence in (Presence.PARKED, Presence.USER_CLOSED):
                s._enter(now, Presence.UNKNOWN, "link up")
            return [Action.HOLD]
        if s.presence == Presence.IN_CASE:
            # The glasses dropped the link while seated in the case (lid
            # closed?). We were about to park anyway: take the slow reseek,
            # not the registry's fast auto-reconnect, all night.
            self._park(s, now, "link lost while in case")
            return [Action.PARK]
        if s.presence in (Presence.PARKED, Presence.USER_CLOSED):
            return []
        # Lost while worn, set down or unknown: NOT in the case, so stay
        # aggressive. The registry's auto-reconnect and the one-temple repair
        # run exactly as today; the policy arms no slow schedule.
        s._enter(now, Presence.UNKNOWN, "link lost")
        s.quiet = False
        s.next_reseek_s = None
        return [Action.RECONNECT_FAST]

    def on_user_close(self, s: PeerState, now: int) -> List[Action]:
        s._enter(now, Presence.USER_CLOSED, "user")
        s.link_up = False
        s.quiet = False
        s.next_reseek_s = None
        return []

    def on_user_open(self, s: PeerState, now: int) -> List[Action]:
        s._enter(now, Presence.UNKNOWN, "user open")
        s.quiet = False
        return [] if s.link_up else [Action.RESEEK_NOW]

    def on_worn(self, s: PeerState, now: int, worn: bool) -> List[Action]:
        if s.presence == Presence.USER_CLOSED:
            return []
        if worn:
            if s.presence == Presence.PARKED:
                # A worn edge cannot arrive on a parked link; it comes from a
                # sibling (the other temple, or the ring while glasses parked).
                s.reseek_period_s = self.policy.reseek_period_s
                s.next_reseek_s = now + s.reseek_period_s
                return [Action.RESEEK_NOW]
            s._enter(now, Presence.WORN, "worn")
            s.quiet = False
            return [Action.HOLD]
        if s.presence in (Presence.WORN, Presence.UNKNOWN):
            s._enter(now, Presence.SET_DOWN, "not worn")
        return []

    def on_in_case(self, s: PeerState, now: int, in_case: bool) -> List[Action]:
        """sid 0x81 inCaseStatus (glasses) or the ring's dock report, with the
        polarity settled by the phase 0 capture. The ONLY evidence that may
        lead to PARK."""
        if s.presence in (Presence.USER_CLOSED, Presence.PARKED):
            return []
        if in_case:
            # In-case is definitive and immediate, unlike the heartbeat latch
            # (~15 s) and unlike charging (a cable while worn is possible), so
            # it outranks WORN. A later worn edge flips it straight back.
            s._enter(now, Presence.IN_CASE, "in case")
            return []
        if s.presence == Presence.IN_CASE:
            s._enter(now, Presence.SET_DOWN, "out of case")
        return []

    def on_charging(self, s: PeerState, now: int, charging: bool) -> List[Action]:
        """Settings field 13 / ring battery frame. Evidence that the device is
        off the body (so QUIET sooner), never on its own a reason to PARK:
        a cable is not the case."""
        s.charging = charging
        if s.presence in (Presence.USER_CLOSED, Presence.PARKED, Presence.IN_CASE):
            return []
        if charging:
            if s.presence == Presence.WORN:
                return []  # worn outranks charging (glasses used on a cable)
            s._enter(now, Presence.SET_DOWN, "charging")
        return []

    def on_battery(self, s: PeerState, pct: int) -> None:
        s.battery_pct = pct

    # ---- time ---------------------------------------------------------
    def tick(self, s: PeerState, now: int) -> List[Action]:
        p = self.policy
        acts: List[Action] = []
        if s.presence == Presence.PARKED:
            if s.next_reseek_s is not None and now >= s.next_reseek_s:
                # Each failed reseek doubles the wait, up to the ceiling. A
                # fixed 10-minute scan all night (54 scans) can cost more than
                # the idle links it replaced; the first simulator run showed
                # exactly that, so the cadence backs off.
                s.reseek_period_s = min(max(s.reseek_period_s * 2, p.reseek_period_s), p.reseek_max_period_s)
                s.next_reseek_s = now + s.reseek_period_s
                acts.append(Action.RESEEK_NOW)
            return acts
        if not s.link_up or s.presence in (Presence.UNKNOWN, Presence.WORN, Presence.USER_CLOSED):
            return acts
        if s.presence == Presence.SET_DOWN:
            # Off the body but not in the case: slow our traffic, keep the
            # link. Never park: the user may pick them up at any moment and
            # a set-down peer that drops its link gets the fast reconnect.
            if not s.quiet and (s.charging or s.elapsed(now) >= p.set_down_quiet_after_s):
                s.quiet = True
                acts.append(Action.QUIET)
        elif s.presence == Presence.IN_CASE:
            if not s.quiet:
                s.quiet = True
                acts.append(Action.QUIET)
            if self._may_park(s) and p.in_case_park_after_s and s.elapsed(now) >= p.in_case_park_after_s:
                acts.append(self._park(s, now, "in case"))
        return acts

    def _may_park(self, s: PeerState) -> bool:
        if not self.policy.allow_park:
            return False
        if self.policy.park_min_battery_pct:
            # Unknown battery counts as too low: never park an accessory we
            # cannot vouch for (boot, or right after a link bounce).
            if s.battery_pct is None or s.battery_pct < self.policy.park_min_battery_pct:
                return False
        return True

    def _park(self, s: PeerState, now: int, why: str) -> Action:
        s._enter(now, Presence.PARKED, why)
        s.link_up = False
        s.reseek_period_s = self.policy.reseek_period_s
        s.next_reseek_s = now + s.reseek_period_s
        return Action.PARK


# ---------------------------------------------------------------------------
# Scenario traces: a night in the case, a night on the desk
# ---------------------------------------------------------------------------

def _run(title: str, events, peers) -> None:
    print(f"===== {title}")
    print(f"{'time':>8} {'peer':<5} {'presence':<11} actions")
    t = 0
    for ev_t, label, fn in events:
        while t < ev_t:
            t += 60
            for s in peers:
                acts = pol_tick(s, t)
                if acts:
                    print(f"{t/3600:8.2f} {s.name:<5} {s.presence.name:<11} {[a.name for a in acts]}")
        print(f"--- {label}")
        acts = fn()
        if acts:
            print(f"{ev_t/3600:8.2f} {'both':<5} {'':<11} {[a.name for a in acts]}")
    for s in peers:
        print(f"\n{s.name} transitions:")
        for when, line in s.log:
            print(f"  {when/3600:7.2f}h {line}")
    print()


_POL = PresencePolicy(Policy())


def pol_tick(s: PeerState, t: int) -> List[Action]:
    return _POL.tick(s, t)


def scenario() -> None:
    pol = _POL
    # Night 1: glasses into the case at 22:00, ring worn all night, up at 07:00.
    g2 = PeerState("g2")
    ring = PeerState("ring")
    pol.on_link(g2, 0, True); pol.on_worn(g2, 0, True)
    pol.on_link(ring, 0, True); pol.on_worn(ring, 0, True)
    _run("night in the case", [
        (22 * 3600, "glasses off, into case",
         lambda: pol.on_worn(g2, 22 * 3600, False) + pol.on_in_case(g2, 22 * 3600 + 5, True)),
        (23 * 3600, "ring stays worn all night (no g2 action)", lambda: []),
        (31 * 3600, "lid opens, glasses on head",
         lambda: pol.on_worn(ring, 31 * 3600, True) + pol.on_worn(g2, 31 * 3600, True)),
    ], (g2, ring))

    # Night 2: glasses set down on the desk, never in the case. QUIET only;
    # a link drop at 03:00 gets the fast reconnect, not the back-off.
    g2 = PeerState("g2")
    pol.on_link(g2, 0, True); pol.on_worn(g2, 0, True)
    _run("night on the desk (no case)", [
        (22 * 3600, "glasses off, on the desk", lambda: pol.on_worn(g2, 22 * 3600, False)),
        (27 * 3600, "link drops (glasses idle-disconnect)", lambda: pol.on_link(g2, 27 * 3600, False)),
        (27 * 3600 + 30, "registry reconnects", lambda: pol.on_link(g2, 27 * 3600 + 30, True)),
        (31 * 3600, "glasses on head", lambda: pol.on_worn(g2, 31 * 3600, True)),
    ], (g2,))


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

class PolicyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.p = Policy()
        self.pol = PresencePolicy(self.p)
        self.s = PeerState("g2")
        self.pol.on_link(self.s, 0, True)

    def run_until(self, end: int, step: int = 30) -> List[Action]:
        acts: List[Action] = []
        t = self.s.since_s
        while t < end:
            t += step
            acts += self.pol.tick(self.s, t)
        return acts

    def park_in_case(self) -> None:
        self.pol.on_worn(self.s, 0, False)
        self.pol.on_in_case(self.s, 0, True)
        self.run_until(self.p.in_case_park_after_s + 60)
        self.assertEqual(self.s.presence, Presence.PARKED)

    def test_worn_holds_forever(self) -> None:
        self.pol.on_worn(self.s, 0, True)
        self.assertEqual(self.run_until(24 * 3600, 600), [])
        self.assertEqual(self.s.presence, Presence.WORN)

    def test_set_down_quiets_but_never_parks(self) -> None:
        self.pol.on_worn(self.s, 0, True)
        self.pol.on_worn(self.s, 100, False)
        acts = self.run_until(100 + 10 * 3600, 60)
        self.assertEqual(acts, [Action.QUIET])
        self.assertEqual(self.s.presence, Presence.SET_DOWN)
        self.assertTrue(self.s.link_up)

    def test_in_case_quiets_at_once_then_parks(self) -> None:
        self.pol.on_worn(self.s, 0, False)
        self.pol.on_in_case(self.s, 10, True)
        acts = self.run_until(10 + self.p.in_case_park_after_s + 60)
        self.assertEqual(acts.count(Action.QUIET), 1)
        self.assertEqual(acts.count(Action.PARK), 1)
        self.assertLess(acts.index(Action.QUIET), acts.index(Action.PARK))
        self.assertEqual(self.s.presence, Presence.PARKED)
        self.assertFalse(self.s.link_up)

    def test_charging_on_a_cable_only_quiets(self) -> None:
        # Charging without the in-case bit: QUIET immediately, never PARK.
        self.pol.on_worn(self.s, 0, False)
        self.pol.on_charging(self.s, 10, True)
        acts = self.run_until(10 + 10 * 3600, 60)
        self.assertEqual(acts, [Action.QUIET])
        self.assertEqual(self.s.presence, Presence.SET_DOWN)
        self.assertTrue(self.s.link_up)

    def test_parked_reseeks_with_backoff(self) -> None:
        self.park_in_case()
        start = self.s.since_s
        when = []
        t = start
        while t < start + 8 * 3600:
            t += 30
            if Action.RESEEK_NOW in self.pol.tick(self.s, t):
                when.append(t - start)
        # first scan 10 min after PARK, then the wait doubles: 20, 40, 60 (cap)
        gaps = [b - a for a, b in zip(when, when[1:])]
        self.assertEqual(when[0], self.p.reseek_period_s)
        self.assertEqual(gaps[:3], [1200, 2400, 3600])
        self.assertTrue(all(g == 3600 for g in gaps[3:]))
        self.assertLess(len(when), 12)  # an 8 h night costs under a dozen scans

    def test_sibling_worn_edge_reseeks_parked_peer_now(self) -> None:
        self.park_in_case()
        self.assertEqual(self.pol.on_worn(self.s, self.s.since_s + 5, True), [Action.RESEEK_NOW])

    def test_user_close_is_sacred(self) -> None:
        self.pol.on_user_close(self.s, 0)
        self.assertEqual(self.pol.on_worn(self.s, 1, True), [])
        self.assertEqual(self.pol.on_in_case(self.s, 2, True), [])
        self.assertEqual(self.pol.on_charging(self.s, 3, True), [])
        self.assertEqual(self.run_until(3600), [])
        self.assertEqual(self.s.presence, Presence.USER_CLOSED)

    def test_user_open_reseeks_now(self) -> None:
        self.pol.on_user_close(self.s, 0)
        self.assertEqual(self.pol.on_user_open(self.s, 5), [Action.RESEEK_NOW])

    def test_park_disabled_by_setting(self) -> None:
        self.p.allow_park = False
        self.pol.on_worn(self.s, 0, False)
        self.pol.on_in_case(self.s, 0, True)
        acts = self.run_until(2 * self.p.in_case_park_after_s)
        self.assertEqual(acts, [Action.QUIET])
        self.assertTrue(self.s.link_up)

    def test_link_lost_while_worn_stays_aggressive(self) -> None:
        self.pol.on_worn(self.s, 0, True)
        self.assertEqual(self.pol.on_link(self.s, 10, False), [Action.RECONNECT_FAST])
        self.assertEqual(self.s.presence, Presence.UNKNOWN)
        self.assertIsNone(self.s.next_reseek_s)
        self.assertEqual(self.run_until(3600), [])

    def test_link_lost_while_set_down_stays_aggressive(self) -> None:
        # The user's rule: back-off only after the in-case bit; a set-down
        # peer that drops its link gets the registry's fast reconnect.
        self.pol.on_worn(self.s, 0, False)
        self.run_until(self.p.set_down_quiet_after_s + 60)
        self.assertTrue(self.s.quiet)
        self.assertEqual(self.pol.on_link(self.s, self.s.since_s + 3600, False), [Action.RECONNECT_FAST])
        self.assertEqual(self.s.presence, Presence.UNKNOWN)
        self.assertFalse(self.s.quiet)
        self.assertIsNone(self.s.next_reseek_s)
        self.assertEqual(self.run_until(self.s.since_s + 3600), [])

    def test_link_lost_while_charging_on_cable_stays_aggressive(self) -> None:
        self.pol.on_worn(self.s, 0, False)
        self.pol.on_charging(self.s, 5, True)
        self.assertEqual(self.pol.on_link(self.s, 30, False), [Action.RECONNECT_FAST])
        self.assertIsNone(self.s.next_reseek_s)

    def test_link_lost_while_in_case_parks_with_backoff(self) -> None:
        self.pol.on_worn(self.s, 0, False)
        self.pol.on_in_case(self.s, 5, True)
        self.assertEqual(self.pol.on_link(self.s, 30, False), [Action.PARK])
        self.assertEqual(self.s.presence, Presence.PARKED)
        self.assertEqual(self.s.next_reseek_s, 30 + self.p.reseek_period_s)

    def test_link_up_clears_park(self) -> None:
        self.park_in_case()
        self.assertEqual(self.pol.on_link(self.s, self.s.since_s + 1, True), [Action.HOLD])
        self.assertEqual(self.s.presence, Presence.UNKNOWN)
        self.assertIsNone(self.s.next_reseek_s)

    def test_charging_while_worn_stays_worn(self) -> None:
        # Glasses used on a cable: WORN outranks CHARGING, nothing parks.
        self.pol.on_worn(self.s, 0, True)
        self.assertEqual(self.pol.on_charging(self.s, 10, True), [])
        self.assertEqual(self.s.presence, Presence.WORN)
        self.assertEqual(self.run_until(2 * self.p.in_case_park_after_s), [])

    def test_in_case_outranks_stale_worn_latch(self) -> None:
        # The heartbeat latch lags ~15 s; the 0x81 push is immediate.
        self.pol.on_worn(self.s, 0, True)
        self.pol.on_in_case(self.s, 1, True)
        self.assertEqual(self.s.presence, Presence.IN_CASE)
        # ...and a real worn edge wins straight back.
        self.assertEqual(self.pol.on_worn(self.s, 2, True), [Action.HOLD])
        self.assertEqual(self.s.presence, Presence.WORN)

    def test_out_of_case_returns_to_set_down(self) -> None:
        self.pol.on_worn(self.s, 0, False)
        self.pol.on_in_case(self.s, 1, True)
        self.pol.on_in_case(self.s, 60, False)
        self.assertEqual(self.s.presence, Presence.SET_DOWN)
        acts = self.run_until(60 + 10 * 3600, 60)
        self.assertNotIn(Action.PARK, acts)

    def test_unknown_battery_blocks_park_when_floor_set(self) -> None:
        self.p.park_min_battery_pct = 20
        self.pol.on_worn(self.s, 0, False)
        self.pol.on_in_case(self.s, 0, True)
        acts = self.run_until(2 * self.p.in_case_park_after_s)
        self.assertNotIn(Action.PARK, acts)

    def test_worn_edge_still_arrives_while_quiet(self) -> None:
        # QUIET keeps a slow heartbeat, so a strict ACK can still flip us WORN.
        self.pol.on_worn(self.s, 0, False)
        acts = self.run_until(self.p.set_down_quiet_after_s + 60)
        self.assertIn(Action.QUIET, acts)
        self.assertEqual(self.pol.on_worn(self.s, self.s.since_s + 300, True), [Action.HOLD])
        self.assertFalse(self.s.quiet)

    def test_user_open_on_live_link_does_not_reseek(self) -> None:
        self.pol.on_worn(self.s, 0, True)
        self.assertEqual(self.pol.on_user_open(self.s, 5), [])

    def test_low_battery_blocks_park(self) -> None:
        self.p.park_min_battery_pct = 20
        self.pol.on_battery(self.s, 10)
        self.pol.on_worn(self.s, 0, False)
        self.pol.on_in_case(self.s, 0, True)
        acts = self.run_until(2 * self.p.in_case_park_after_s)
        self.assertNotIn(Action.PARK, acts)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--test", action="store_true")
    args = ap.parse_args()
    if args.test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(PolicyTests)
        return 0 if unittest.TextTestRunner(verbosity=1).run(suite).wasSuccessful() else 1
    scenario()
    return 0


if __name__ == "__main__":
    sys.exit(main())
