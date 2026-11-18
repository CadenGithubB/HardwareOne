#!/usr/bin/env python3
"""Run the bond agreement: BOND_REQUEST / BOND_NOTICE on the production code.

A bond is a setting on each device. Before these two frames a device bonded to
a peer that was not bonded back opened the session, called the partner online,
asked for its capabilities, timed out after 15 s, cleared the session and
started over about every 20 s, and a device whose partner ran bonddisconnect or
unpaired it only saw silence. Neither user was told why.

Builds on the doubles of espnow_bond_partner_change_harness.cpp (everything
above its "Cases" line, assembled by test_espnow_bond_partner_change.py with
the production code pasted in: System_ESPNow_BondAgreeCore.h as is, the wire
payloads and reason codes, the agreement glue with v4h_bond_request and
v4h_bond_notice, v4h_bond_heartbeat, bondNotifySessionEstablished, the bond
parts of the espnow_task loop and the bond commands) and appends
espnow_bond_agreement_cases.cpp. Checks:

- mutually bonded: the partner is asked when its session comes up, its request
  is answered ACCEPTED and agrees the bond, and nothing more is asked;
- a request from a peer that is not the partner is answered NOT_BONDED or
  BONDED_ELSEWHERE (never naming the partner, and without this device's role),
  at most once per 5 s however often it asks, reported to the user within the
  line limits, and changes no bond setting;
- a notice from a peer that is not the partner, an answer to an older request
  and an unknown reason are not used;
- ENDED (and NOT_BONDED) from the partner keep this device's bond setting but
  stop the heartbeat, the capability requests and the 15 s timeout that
  cleared the session, and clear the sync state; one request a minute, with
  its acknowledgement watched, is all that is sent; a session alone or a late
  heartbeat does not bring the partner online, but every session gets its
  auth-log record; without a session a handshake is started once a minute,
  also while a request is waiting; the bond comes back when the partner bonds
  again;
- with no answer at all (a partner without these frames) the bond behaves as
  before, and such a partner's bond heartbeat stands for its agreement;
- ROLE_CONFLICT is answered, learnt and reported on both paths, and seen in
  the heartbeat after three in a row;
- the limits: one notice a minute to a peer whose bond frames are ignored,
  also with more such peers than the limit table has entries, a retry after a
  full send queue, no request without a session, and a second's wait on the
  side that answered the handshake;
- a partner change resets the agreement, sends nothing while bond frames are
  held, and answers a request received during the hold from the new settings;
- a decline handled on espnow_task while cmd_exec is inside
  bondNotifySessionEstablished wins at every point it can land on (before the
  agreement is read, before the online event, during the auth-log write): the
  partner is offline, no capability request is due, and no offline event
  comes before its online event;
- the partner's request from before its ENDED (it went through the send queue
  and arrives after the ENDED, which names it) does not agree the bond again;
  a newer request does, and so does request 1 of a restarted partner on its
  new session; this device's ENDED names its last request and no request is
  made between the ENDED and the settings change;
- bondconnect to the partner already set says how the bond stands when it is
  not agreed and asks again at once, at most every 5 s;
- the agreement text the JSON answers carry;
- the role (owner decision 2026-11-18): bondconnect keeps a role chosen with
  bondrole, the request and the notice say "chosen", a partner whose role is
  automatic is not reported as a conflict, this device's automatic role yields
  to a partner's choice from its request or its notice (answered with the new
  role, asked again), both chose the same role is ROLE_CONFLICT, a peer's
  earlier request decides bondconnect, the role words of bondstatus.

Source rules (System_ESPNow.cpp is not host-compiled as a whole): bondrole
master|worker sets the persisted "chosen" flag and tells the partner, bondrole
auto clears it, both go through bondChangeRole, the flag's setting row is in
the "bond" group and read-only, and bondstatus json says roleChosen and
roleSource.

The pure rules are also run on two modelled devices by
test_espnow_bond_agree_core.cpp; the receive table rows by
test_espnow_dispatch_policy.py.
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import test_espnow_bond_partner_change as base  # noqa: E402


def check_role_sources(component):
    from test_web_batch_handlers import extract_block, require
    source = (component / 'System_ESPNow.cpp').read_text()
    role = extract_block(source, 'const char* cmd_bond_role(const String& argsInput)')
    auto_at = role.index('if (args == "auto") {')
    auto = role[auto_at:role.index('return getDebugBuffer();', auto_at)]
    chosen = role[role.index('uint8_t newRole = 255;'):]
    require('setSetting(gSettings.bondRoleChosen, false);' in auto and 'bondAgreementTellRole();' in auto,
            "'bondrole auto' gives the choice up and tells the partner")
    require('setSetting(gSettings.bondRoleChosen, true);' in chosen and 'bondChangeRole(newRole);' in chosen and
            'bondAgreementTellRole();' in chosen and
            chosen.index('setSetting(gSettings.bondRoleChosen, true);') < chosen.index('bondChangeRole(newRole);'),
            "'bondrole master|worker' is a choice (set before the change) and the partner is told")
    require('{ "bondRoleChosen", SETTING_BOOL, &gSettings.bondRoleChosen, false, 0, nullptr, 0, 1, "Bond Role Chosen", '
            'nullptr, false, "bond", nullptr, true },' in source,
            "the flag is persisted, in the 'bond' group (not remotely editable), shown and not edited")
    connect = extract_block(source, 'const char* cmd_bond_connect(const String& argsInput)')
    require('bondRoleForConnect(peerMac)' in connect and 'gSettings.bondRole = role.role;' in connect and
            'cmp > 0' not in connect, "bondconnect takes the role from roleForBond, not from the addresses alone")
    status = extract_block(source, 'const char* cmd_bond_status(const String& argsInput)')
    require('doc["roleChosen"] = gSettings.bondRoleChosen;' in status and
            'doc["roleSource"] = bondagree::roleSourceKey(bondRoleSourceNow());' in status and
            status.count('bondRoleWords(roleWords, sizeof(roleWords));') == 2,
            "bondstatus and bondstatus json say whether the role was chosen and how it was decided")
    request = source[source.index('V4PayloadBondRequest request = {};'):]
    require('(gSettings.bondRoleChosen ? BOND_REQUEST_FLAG_ROLE_CHOSEN : 0)' in request[:400],
            'the bond request carries the chosen flag')
    settings = (component / 'System_Settings.h').read_text()
    require('bool bondRoleChosen;' in settings and 'bondRoleChosen(false),' in settings, "the flag's field, false by default")


def main():
    a = base.parse_args(__doc__)
    harness, have, read = base.assemble(a)
    if not have['AGREE']:
        sys.exit('FAIL espnow_bond_agreement: the source has no bond agreement (BOND_REQUEST / BOND_NOTICE)')
    if harness.count(base.CASES_LINE) != 1:
        sys.exit(f'FAIL: the harness needs exactly one {base.CASES_LINE!r} line')
    unit = harness[:harness.index(base.CASES_LINE)] + (HERE / 'espnow_bond_agreement_cases.cpp').read_text()
    if not base.build_and_run(a, unit, have, read, 'espnow_bond_agreement'):
        print('FAIL espnow_bond_agreement: checks reported failures', file=sys.stderr)
        sys.exit(1)
    check_role_sources(a.component)
    print('PASS espnow_bond_agreement')


if __name__ == '__main__':
    main()
