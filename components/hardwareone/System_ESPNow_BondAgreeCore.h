#pragma once
// What the two ends of a bond tell each other about it (BOND_REQUEST /
// BOND_NOTICE), without the radio, the settings or the console.
//
// A bond is a local setting on each device: bond mode, a partner, a role. Before
// these two frames nothing told the other device about it. A device bonded to a
// peer that was not bonded back opened the session, called the partner online,
// asked for its capabilities, timed out after 15 s, cleared the session and
// started over, for good, and neither user was told why. A device that ended
// the bond left its partner with silence.
//
//   * BOND_REQUEST says "you are my bond partner". A bonded device sends it to
//     its partner when the bond is set up, when their session comes up, once a
//     minute while the partner has not agreed, and when the user gives the
//     bond command again for a partner that has not.
//   * BOND_NOTICE says how the sender's bond stands toward the receiver:
//     ACCEPTED (you are my partner too), ROLE_CONFLICT (the same, but both of us
//     have the same role), NOT_BONDED (bond mode is off here), BONDED_ELSEWHERE
//     (my partner is another device), ENDED (I just ended our bond). It answers
//     a request, is sent unasked to the partner a device drops, and goes, at
//     most once a minute, to a paired peer whose bond frames this device ignores.
//     A peer that is not the partner gets at most one answer every 5 s, however
//     often it asks.
//
// Both frames carry a 16-bit number. A request's number comes back in the
// notice that answers it, and an answer counts only when it is to the last
// request sent. ENDED is never an answer, and its number means something else:
// the last request its sender sent. The receiver takes that sender's requests
// up to that number as void. A request goes through the send queue and ENDED
// is sent directly, so a request made just before the bond was ended can
// arrive after the ENDED; without the number it would read as "bonded again".
//
// Neither frame changes the receiver's bond settings, with one exception, the
// role: bonding still takes the command on each device. A request from a peer
// that is not the partner gets an answer and one line for the user. A notice
// counts only when it comes from the configured partner, and then only changes
// what this device knows about the partner's side: the agreement.
//
// The role. A user may choose a device's role (bondrole master|worker); the
// choice is kept, also by bondconnect. A device whose role was not chosen
// (the default, and after 'bondrole auto') takes the other role than a partner
// whose user chose one: the request carries the sender's role and a "chosen"
// flag, an ACCEPTED, ROLE_CONFLICT or ENDED notice the same (roleForBond,
// takeInbound). When neither chose, bondconnect gives the device with the
// higher address the master role, as before. When both chose the same role,
// ROLE_CONFLICT says so, as before.
//
//   Unknown   no answer yet. The bond behaves as it did before these frames,
//             so a partner whose firmware never sends them still bonds.
//   Agreed    the partner has this device as its partner: its ACCEPTED or
//             ROLE_CONFLICT, its own request, or (firmware without these
//             frames) its bond heartbeat, which it only sends to its partner.
//   Declined  the partner said NOT_BONDED, BONDED_ELSEWHERE or ENDED. The owner
//             stops the heartbeat and the sync and does not call the partner
//             online from the session; only the request goes on, once a minute.
//
// No OS dependencies: the owner (System_ESPNow.cpp) holds its own lock around
// every call, passes the clock, reads the settings, and sends the frames and
// prints the lines this returns, so the host tests run this file as is.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "System_Deadline.h"

namespace bondagree {

constexpr uint32_t kRequestRepeatMs = 60000;          // the request again while not agreed
constexpr uint32_t kRequestRetryMs = 5000;            // after a request that could not be queued, and
                                                      // the shortest gap between requests a declined
                                                      // partner's own traffic sets off
constexpr uint32_t kSessionRecheckMs = 1000;          // a due request waits for the session
constexpr uint32_t kSessionSettleMs = 1000;           // the side that answered the handshake waits this
                                                      // long before it asks: the other side opens the
                                                      // session a moment later and drops what comes first
constexpr uint32_t kEndingQuietMs = kRequestRetryMs;  // no request this long after the partner was told
                                                      // ENDED; the settings change that follows ends the
                                                      // wait at once (reset)
constexpr uint32_t kNoticeGapMs = 60000;              // unasked notices to one peer whose bond frames are ignored
constexpr uint32_t kAnswerGapMs = kRequestRetryMs;    // answers to one peer that is not the partner
constexpr uint32_t kAskLineGapMs = 60000;             // "wants to bond" line for a fresh request
constexpr uint32_t kAskLineRepeatMs = 15UL * 60000UL; // ... and for the once-a-minute repeats
constexpr uint8_t kRoleConflictBeats = 3;             // heartbeats with our own role before it is called a conflict
constexpr uint8_t kPeerSlots = 4;                     // peers with limits of their own
constexpr uint8_t kPeerEntries = kPeerSlots + 1;      // ... and one entry the peers beyond them share
constexpr uint8_t kInboxSlots = 4;

// BOND_NOTICE reason codes, as sent (BondNoticeReason in System_ESPNow_Wire.h).
// A receiver ignores a code it does not know.
enum Reason : uint8_t {
  kReasonNone = 0,
  kReasonAccepted = 1,
  kReasonNotBonded = 2,
  kReasonBondedElsewhere = 3,
  kReasonEnded = 4,
  kReasonRoleConflict = 5,
  // Local only, never sent (outside the wire range 1-5): the partner's session
  // capability word lacks the bond bit, so its firmware was built without bond
  // mode and cannot be a bond partner (System_ESPNow_SessionCapsCore.h).
  kReasonNoBondBuild = 0x80,
};

// BOND_REQUEST flags. Fresh: the bond was just set up or the session just came
// up; clear on the once-a-minute repeats. RoleChosen: the sender's user chose
// its role. RoleYields: the sender follows the role rules below (set on every
// request of this firmware): when its role was not chosen and the receiver's
// user chose the same one, it takes the other role. Firmware from before
// never sets it and never yields.
constexpr uint8_t kRequestFlagFresh = 0x01;
constexpr uint8_t kRequestFlagRoleChosen = 0x02;
constexpr uint8_t kRequestFlagRoleYields = 0x04;
// BOND_NOTICE flags (sent with the role only: ACCEPTED, ROLE_CONFLICT, ENDED).
constexpr uint8_t kNoticeFlagRoleChosen = 0x01;

constexpr uint8_t kRoleWorker = 0;
constexpr uint8_t kRoleMaster = 1;

enum class Agreement : uint8_t { Unknown, Agreed, Declined };

// This device's bond settings, read by the owner for each call.
struct Local {
  bool bonded;         // bond mode on and a partner configured
  uint8_t partner[6];  // valid when bonded
  uint8_t role;
  bool roleChosen;     // this device's user chose the role (bondrole)
  // The partner's session capability word lacks the bond bit (its build has
  // no bond mode): no request is sent to it. `partnerNoBondConfirmed`: that
  // word is its current one (SessionCapsCore.h), so the user is told and the
  // agreement says so; otherwise the request only waits.
  bool partnerNoBond;
  bool partnerNoBondConfirmed;
};

// A request or notice a handler received, kept until the owner's tick takes
// it: the answer is worked out then, from settings no partner change is in the
// middle of.
struct Inbound {
  bool inUse;
  bool isRequest;
  uint8_t mac[6];
  uint8_t role;
  uint8_t code;    // request: flags; notice: reason
  uint8_t flags;   // notice: its flags (0 for a request)
  uint16_t seq;
  uint16_t order;  // arrival order
};

// A paired peer this device answered or ignored: the limits on what it is sent
// and on the lines printed about it.
struct PeerNote {
  bool inUse;
  bool noticeDue;         // its bond frames were ignored: tell it why
  bool answerDue;         // it asked again inside kAnswerGapMs: answered when the gap is over
  uint16_t answerSeq;     // ... the number of its newest request
  uint8_t mac[6];
  uint32_t lastNoticeMs;  // 0 = none sent
  uint32_t lastAskLineMs; // 0 = none printed
  uint32_t touchedMs;
  bool roleKnown;         // its last request: its role, and whether its user chose it
  uint8_t role;           // (bondconnect to it uses them)
  bool roleChosen;
};

struct State {
  Agreement agreement;
  uint8_t reason;              // the partner's reason while Declined
  bool roleConflict;           // Agreed, and both devices have the same role
  bool announcedAgreed;        // the "accepted the bond" line was printed for this partner
  uint8_t sameRoleBeats;       // heartbeats in a row carrying our own role
  bool requestDue;             // send a request as soon as the session allows
  uint32_t requestNotBeforeMs; // deadline; 0 = at once
  uint32_t nextRepeatMs;       // deadline of the next once-a-minute request; 0 = none
  uint32_t lastRequestMs;      // 0 = none sent
  uint16_t lastSeq;            // of the last request sent, 0 = none yet; survives a partner change
  uint32_t endingSinceMs;      // 0 = not ending. The partner was told ENDED then: no request for kEndingQuietMs
  bool partnerVoidSet;         // the partner's ENDED named its last request ...
  uint16_t partnerVoidSeq;     // ... its requests up to this number are void
  uint16_t nextOrder;
  uint32_t requestsSent;
  uint32_t requestsVoided;     // the partner's requests from before its ENDED, not used
  uint32_t noticesSent;
  uint32_t noticesIgnored;     // from a peer that is not the partner, or answering an old request
  uint32_t inboundDropped;     // inbox full
  bool partnerRoleKnown;       // the partner's role as its last request or notice carried it ...
  uint8_t partnerRole;
  bool partnerRoleChosen;      // ... and whether its user chose it
  Inbound inbox[kInboxSlots];
  PeerNote peers[kPeerEntries]; // the last entry is the shared one (peerTouch)
};

inline bool macIs(const uint8_t a[6], const uint8_t b[6]) { return memcmp(a, b, 6) == 0; }

inline bool isPartner(const Local& local, const uint8_t mac[6]) {
  return local.bonded && macIs(local.partner, mac);
}

// The words that follow the partner's name: "<name> ended the bond".
inline const char* reasonPhrase(uint8_t reason) {
  switch (reason) {
    case kReasonAccepted:        return "accepted the bond";
    case kReasonNotBonded:       return "has bond mode off";
    case kReasonBondedElsewhere: return "is bonded to another device";
    case kReasonEnded:           return "ended the bond";
    case kReasonRoleConflict:    return "is bonded to this device with the same role";
    case kReasonNoBondBuild:     return "was built without bond mode, so it cannot be a bond partner";
    default:                     return "gave no answer";
  }
}

// Stable keys for JSON.
inline const char* reasonKey(uint8_t reason) {
  switch (reason) {
    case kReasonAccepted:        return "accepted";
    case kReasonNotBonded:       return "not_bonded";
    case kReasonBondedElsewhere: return "bonded_elsewhere";
    case kReasonEnded:           return "ended";
    case kReasonRoleConflict:    return "role_conflict";
    case kReasonNoBondBuild:     return "no_bond_build";
    default:                     return "";
  }
}

inline const char* agreementKey(Agreement a) {
  switch (a) {
    case Agreement::Agreed:   return "agreed";
    case Agreement::Declined: return "declined";
    default:                  return "unknown";
  }
}

// True when request number `a` was given out after `b`. The numbers are 16 bit
// and go round, so "after" is "up to half the range ahead".
inline bool seqNewer(uint16_t a, uint16_t b) {
  const uint16_t ahead = (uint16_t)(a - b);
  return ahead != 0 && ahead < 0x8000u;
}

// The partner or bond mode changed (or the bond was just set up): nothing
// known about the old partner's side applies, and the new one is asked as soon
// as its session allows. The request number goes on, so an answer to a
// request made before the change matches nothing.
inline void reset(State& s) {
  s.agreement = Agreement::Unknown;
  s.reason = kReasonNone;
  s.roleConflict = false;
  s.announcedAgreed = false;
  s.sameRoleBeats = 0;
  s.requestDue = true;
  s.requestNotBeforeMs = 0;
  s.nextRepeatMs = 0;
  s.endingSinceMs = 0;
  s.partnerVoidSet = false;   // the old partner's ENDED says nothing about the new one's requests
  s.partnerRoleKnown = false;
  s.partnerRole = 0;
  s.partnerRoleChosen = false;
}

// ---- The role ---------------------------------------------------------------------
enum class RoleSource : uint8_t {
  Chosen,   // this device's user chose it (bondrole master|worker)
  Partner,  // the partner's user chose the other role
  Address,  // neither chose: the device with the higher address is the master
  Kept,     // neither chose now, and the role is not the one the addresses give: it was
            // set earlier (a partner's choice since given up, 'bondrole auto') and kept
};

struct RoleChoice {
  uint8_t role;
  RoleSource source;
};

// The role this device takes when it bonds (bondconnect). `chosen`, `role`:
// this device's. `partnerKnown`, `partnerChose`, `partnerRole`: what the
// partner's last request or notice said. `selfHigher`: this device's address
// is the higher of the two.
inline RoleChoice roleForBond(bool chosen, uint8_t role, bool partnerKnown, bool partnerChose, uint8_t partnerRole,
                              bool selfHigher) {
  if (chosen) return { role, RoleSource::Chosen };
  if (partnerKnown && partnerChose && partnerRole <= kRoleMaster) {
    return { partnerRole == kRoleMaster ? kRoleWorker : kRoleMaster, RoleSource::Partner };
  }
  return { selfHigher ? kRoleMaster : kRoleWorker, RoleSource::Address };
}

// Whether this device, bonded already, now takes the other role: its role was
// not chosen, and the partner's user chose the same one. False otherwise
// (both chose the same role: a conflict, which ROLE_CONFLICT reports).
inline bool roleYields(bool chosen, uint8_t role, bool partnerChose, uint8_t partnerRole) {
  return !chosen && partnerChose && partnerRole <= kRoleMaster && partnerRole == role;
}

// How this device's role came about now, for bondrole and bondstatus.
// `role`: this device's; `selfHigher`: its address is the higher of the two
// (`partnerKnown` false: no partner configured, the address rule is what
// bondconnect will apply).
inline RoleSource roleSourceNow(const State& s, bool chosen, uint8_t role, bool partnerKnown, bool selfHigher) {
  if (chosen) return RoleSource::Chosen;
  if (s.partnerRoleKnown && s.partnerRoleChosen && s.partnerRole <= kRoleMaster && s.partnerRole != role) {
    return RoleSource::Partner;
  }
  if (!partnerKnown || role == (selfHigher ? kRoleMaster : kRoleWorker)) return RoleSource::Address;
  return RoleSource::Kept;
}

inline const char* roleSourceKey(RoleSource src) {
  switch (src) {
    case RoleSource::Chosen:  return "chosen";
    case RoleSource::Partner: return "partner";
    case RoleSource::Kept:    return "kept";
    default:                  return "address";
  }
}

// Ask the partner (again) now, but not more often than every kRequestRetryMs.
inline void askSoon(State& s, uint32_t nowMs) {
  if (s.requestDue) return;
  s.requestDue = true;
  const bool recent = s.lastRequestMs != 0 && (uint32_t)(nowMs - s.lastRequestMs) < kRequestRetryMs;
  s.requestNotBeforeMs = recent ? deadlineAfter(s.lastRequestMs, kRequestRetryMs) : 0;
}

// A session with the partner came up. It is asked whether it agrees; a partner
// that declined is asked at most every kRequestRetryMs. `answeredHandshake`:
// this device answered the handshake, so its session is up before the other
// side has finished opening its own. A request sent at once would arrive there
// undecryptable and be dropped, so it waits kSessionSettleMs. Returns false
// when the partner declined the bond: the session alone does not make it
// online then.
//
// The numbers an ENDED voided are forgotten here. A request from before that
// ENDED belongs to the session before this one. And a partner that restarted
// counts its requests from 1 again: held against the old number, they would
// all be void, and it can only reach this device over a new session.
//
// `partnerCanBond` false: the session's capability word says the partner's
// build has no bond mode. It is not online and is not asked (planRequest holds
// the request and tells the user). A session whose word has the bond bit ends
// a kReasonNoBondBuild decline: the partner was reflashed, and is asked as new.
inline bool onSessionUp(State& s, uint32_t nowMs, bool answeredHandshake, bool partnerCanBond = true) {
  s.partnerVoidSet = false;
  if (!partnerCanBond) {
    s.requestDue = false;
    s.requestNotBeforeMs = 0;
    return false;
  }
  if (s.agreement == Agreement::Declined && s.reason == kReasonNoBondBuild) {
    s.agreement = Agreement::Unknown;
    s.reason = kReasonNone;
  }
  const bool declined = s.agreement == Agreement::Declined;
  if (declined) {
    askSoon(s, nowMs);
  } else {
    s.requestDue = true;
    s.requestNotBeforeMs = 0;
  }
  if (answeredHandshake) {
    const uint32_t settled = deadlineAfter(nowMs, kSessionSettleMs);
    const bool later = deadlinePending(s.requestNotBeforeMs, nowMs) &&
                       (int32_t)(s.requestNotBeforeMs - settled) > 0;
    if (!later) s.requestNotBeforeMs = settled;
  }
  return !declined;
}

// The partner went silent (heartbeat timeout). What it said before may no
// longer hold; it is asked again when the session is back.
inline void onLinkLost(State& s) {
  if (s.agreement == Agreement::Agreed) s.agreement = Agreement::Unknown;
  s.sameRoleBeats = 0;
}

// ---- Lines for the user ------------------------------------------------------
enum class Line : uint8_t {
  None,
  Accepted,          // "<name> accepted the bond"
  Declined,          // "<name> <reasonPhrase>"
  RoleConflict,      // both devices have the same role
  RoleConflictOver,  // ... no longer
  WantsBond,         // a peer that is not the partner asked
};

// ---- Receive side: handlers record, the tick decides ---------------------------

// Stores a received request or notice. A newer one of the same kind from the
// same peer replaces the older, except that a waiting ENDED is only replaced
// by another ENDED: it alone says which of the sender's requests are void, so
// the notice that follows it gets an entry of its own. False when the inbox is
// full (the frame is dropped: a request is repeated, an ignored bond frame
// brings a notice again).
inline bool pushInbound(State& s, bool isRequest, const uint8_t mac[6], uint8_t role, uint8_t code,
                        uint16_t seq, uint8_t flags = 0) {
  Inbound* slot = nullptr;
  for (auto& e : s.inbox) {
    if (!e.inUse || e.isRequest != isRequest || !macIs(e.mac, mac)) continue;
    if (!isRequest && e.code == kReasonEnded && code != kReasonEnded) continue;
    slot = &e;
    break;
  }
  if (!slot) {
    for (auto& e : s.inbox) {
      if (!e.inUse) { slot = &e; break; }
    }
  }
  if (!slot) {
    s.inboundDropped++;
    return false;
  }
  slot->inUse = true;
  slot->isRequest = isRequest;
  memcpy(slot->mac, mac, 6);
  slot->role = role;
  slot->code = code;
  slot->flags = isRequest ? 0 : flags;
  slot->seq = seq;
  slot->order = s.nextOrder++;
  return true;
}

inline PeerNote* peerFind(State& s, const uint8_t mac[6]) {
  for (auto& p : s.peers) {
    if (p.inUse && macIs(p.mac, mac)) return &p;
  }
  return nullptr;
}

inline uint32_t stamp(uint32_t nowMs) { return nowMs ? nowMs : 1; }

// True while `sinceMs` (0 = never) is less than `gapMs` ago.
inline bool within(uint32_t sinceMs, uint32_t nowMs, uint32_t gapMs) {
  return sinceMs != 0 && (uint32_t)(nowMs - sinceMs) < gapMs;
}

// An entry none of whose limits still holds anything back: giving it to
// another peer lets nobody be told or reported sooner than the limits allow.
inline bool peerSpent(const PeerNote& p, uint32_t nowMs) {
  return !p.noticeDue && !p.answerDue && !within(p.lastNoticeMs, nowMs, kNoticeGapMs) &&
         !within(p.lastAskLineMs, nowMs, kAskLineRepeatMs);
}

// The peer's entry. A new peer gets a free entry, or the spent one touched
// longest ago. While every entry is still inside a limit, the last entry is
// handed over with its limits as they stand: the peers that do not fit share
// one notice a minute and one line between them, so a fifth and sixth peer
// cannot start everybody's limits over by taking turns.
inline PeerNote& peerTouch(State& s, const uint8_t mac[6], uint32_t nowMs) {
  PeerNote* p = peerFind(s, mac);
  if (!p) {
    for (auto& c : s.peers) {
      if (c.inUse && !peerSpent(c, nowMs)) continue;
      if (!c.inUse) { p = &c; break; }
      if (!p || (uint32_t)(nowMs - c.touchedMs) > (uint32_t)(nowMs - p->touchedMs)) p = &c;
    }
    if (p) {
      memset(p, 0, sizeof(*p));
    } else {
      p = &s.peers[kPeerSlots];
      p->answerDue = false;   // owed to the peer that had the entry, not to this one
    }
    memcpy(p->mac, mac, 6);
    p->inUse = true;
  }
  p->touchedMs = nowMs;
  return *p;
}

// A bond frame from a paired peer that is not the partner was ignored. True
// when that peer is due a notice (at most one per kNoticeGapMs); the tick
// sends it (takeIgnoredNotice).
inline bool noteIgnoredFrame(State& s, const uint8_t mac[6], uint32_t nowMs) {
  PeerNote& p = peerTouch(s, mac, nowMs);
  if (p.noticeDue) return false;
  if (within(p.lastNoticeMs, nowMs, kNoticeGapMs)) return false;
  p.noticeDue = true;
  return true;
}

// An answer held back by kAnswerGapMs whose time has come.
inline bool answerReady(const PeerNote& p, uint32_t nowMs) {
  return p.answerDue && !within(p.lastNoticeMs, nowMs, kAnswerGapMs);
}

// The next notice owed to a peer that is not the partner: its bond frames
// were ignored (`seq` 0), or it asked again inside kAnswerGapMs and the gap is
// over (`seq` is its newest request). A peer that has become the partner
// meanwhile is owed none: it is asked, and its own request is answered.
inline bool takeOwedNotice(State& s, const Local& local, uint32_t nowMs, uint8_t to[6], uint8_t* reason,
                           uint16_t* seq) {
  for (auto& p : s.peers) {
    if (!p.inUse || !(p.noticeDue || answerReady(p, nowMs))) continue;
    const uint16_t answers = p.answerDue ? p.answerSeq : 0;
    p.noticeDue = false;
    p.answerDue = false;
    if (isPartner(local, p.mac)) continue;
    p.lastNoticeMs = stamp(nowMs);
    memcpy(to, p.mac, 6);
    *reason = local.bonded ? kReasonBondedElsewhere : kReasonNotBonded;
    *seq = answers;
    s.noticesSent++;
    return true;
  }
  return false;
}

// What the owner does for one received request or notice.
struct Outcome {
  bool handled;          // an inbox entry was taken (call again for the next)
  bool sendNotice;       // to `mac`, with noticeReason and noticeSeq
  uint8_t noticeReason;
  uint16_t noticeSeq;
  Line line;             // about `mac`
  uint8_t lineReason;    // Declined: the partner's reason; WantsBond: what the peer was answered
  bool becameDeclined;   // the partner just declined: stop the bond's periodic work
  bool ignored;          // a notice that did not count
  bool voided;           // the partner's request from before its ENDED: not used, not answered
  uint8_t mac[6];        // the sender
  uint8_t noticeRole;    // the role the notice carries (this device's, after a switch below)
  bool switchRole;       // this device's role was not chosen and the partner chose the same one:
  uint8_t newRole;       // ... it takes this one (the owner changes the setting)
};

// What the partner's request or notice says about its role: kept, and
// whether this device yields (takes the other role) worked out.
inline void notePartnerRole(State& s, const Local& local, uint8_t role, bool chose, Outcome& out) {
  s.partnerRoleKnown = true;
  s.partnerRole = role;
  s.partnerRoleChosen = chose;
  if (roleYields(local.roleChosen, local.role, chose, role)) {
    out.switchRole = true;
    out.newRole = role == kRoleMaster ? kRoleWorker : kRoleMaster;
  }
}

inline void setAgreed(State& s, uint8_t localRole, uint8_t partnerRole, bool partnerSaysConflict,
                      Outcome& out) {
  const bool wasAgreed = s.agreement == Agreement::Agreed;
  const bool conflict = partnerSaysConflict || partnerRole == localRole;
  s.agreement = Agreement::Agreed;
  s.reason = kReasonNone;
  s.nextRepeatMs = 0;   // nothing is repeated while agreed, and a deadline is not kept for weeks
  s.sameRoleBeats = conflict ? kRoleConflictBeats : 0;
  if (conflict) {
    if (!s.roleConflict || !wasAgreed) out.line = Line::RoleConflict;
    s.announcedAgreed = true;
  } else if (s.roleConflict && wasAgreed) {
    out.line = Line::RoleConflictOver;
  } else if (!s.announcedAgreed) {
    out.line = Line::Accepted;
    s.announcedAgreed = true;
  }
  s.roleConflict = conflict;
}

// Takes the oldest received request or notice and decides. Requests: the
// partner's makes the bond agreed and is answered ACCEPTED or ROLE_CONFLICT,
// unless its ENDED voided it; anyone else's is answered NOT_BONDED or
// BONDED_ELSEWHERE (never which device), at most once per kAnswerGapMs, and
// reported to the user within the line limits. Notices: only the partner's
// count, and an answer only when it is to the last request sent. ENDED is not
// an answer: it counts whatever its number, which names the partner's own
// requests that are void.
inline Outcome takeInbound(State& s, const Local& local, uint32_t nowMs) {
  Outcome out = {};
  Inbound* next = nullptr;
  for (auto& e : s.inbox) {
    if (!e.inUse) continue;
    if (!next || (uint16_t)(e.order - next->order) > 0x8000u) next = &e;
  }
  if (!next) return out;
  const Inbound in = *next;
  next->inUse = false;
  out.handled = true;
  memcpy(out.mac, in.mac, 6);
  out.noticeRole = local.role;

  if (in.isRequest) {
    out.noticeSeq = in.seq;
    if (isPartner(local, in.mac)) {
      if (s.partnerVoidSet) {
        if (!seqNewer(in.seq, s.partnerVoidSeq)) {
          // Sent before the partner ended the bond and overtaken by its
          // ENDED. Acting on it would call the bond agreed again and start
          // the heartbeat toward a device that has just left.
          s.requestsVoided++;
          out.voided = true;
          return out;
        }
        s.partnerVoidSet = false;   // a request from after the ENDED: the partner bonded again
      }
      out.sendNotice = true;
      s.noticesSent++;
      // The partner's role and whether its user chose it. A device whose own
      // role was not chosen takes the other role, and answers with it.
      const bool partnerChose = (in.code & kRequestFlagRoleChosen) != 0;
      notePartnerRole(s, local, in.role, partnerChose, out);
      if (out.switchRole) out.noticeRole = out.newRole;
      // This device's user chose the role the partner has, and the partner's
      // role is automatic: the partner takes the other one on this answer
      // (it carries "chosen"), so that is no conflict to report. Only a
      // partner that says it follows the rule (RoleYields) does: older
      // firmware keeps its role, and the conflict is reported as before.
      const bool partnerYields = local.roleChosen && !partnerChose && (in.code & kRequestFlagRoleYields) != 0 &&
                                 in.role == local.role && in.role <= kRoleMaster;
      setAgreed(s, out.noticeRole, partnerYields ? (uint8_t)(kRoleMaster - in.role) : in.role, false, out);
      out.noticeReason = s.roleConflict ? kReasonRoleConflict : kReasonAccepted;
      return out;
    }
    out.noticeReason = local.bonded ? kReasonBondedElsewhere : kReasonNotBonded;
    PeerNote& p = peerTouch(s, in.mac, nowMs);
    // Kept for a bondconnect to it: a device whose role was not chosen takes
    // the other role than one its user chose.
    p.roleKnown = true;
    p.role = in.role;
    p.roleChosen = (in.code & kRequestFlagRoleChosen) != 0;
    if (within(p.lastNoticeMs, nowMs, kAnswerGapMs)) {
      // It was told less than kAnswerGapMs ago. A device asks this often only
      // right after a bond command; anything faster is not answered frame for
      // frame. Its newest request is answered when the gap is over
      // (takeOwedNotice), so the answer still carries a number it accepts.
      p.answerDue = true;
      p.answerSeq = in.seq;
    } else {
      out.sendNotice = true;
      s.noticesSent++;
      p.lastNoticeMs = stamp(nowMs);   // the answer is its notice for this minute
      p.noticeDue = false;
      p.answerDue = false;
    }
    const bool fresh = (in.code & kRequestFlagFresh) != 0;
    const uint32_t gap = fresh ? kAskLineGapMs : kAskLineRepeatMs;
    if (!within(p.lastAskLineMs, nowMs, gap)) {
      p.lastAskLineMs = stamp(nowMs);
      out.line = Line::WantsBond;
      out.lineReason = out.noticeReason;
    }
    return out;
  }

  // A notice. The number of an answer is that of our request; the number of
  // ENDED is the sender's own (see below) and is not compared with ours.
  const bool oldAnswer = in.code != kReasonEnded && in.seq != 0 && in.seq != s.lastSeq;
  if (!isPartner(local, in.mac) || oldAnswer) {
    s.noticesIgnored++;
    out.ignored = true;
    return out;
  }
  if (in.code == kReasonEnded && in.seq != 0 &&
      (!s.partnerVoidSet || seqNewer(in.seq, s.partnerVoidSeq))) {
    // The partner's requests up to this number were made before it ended the
    // bond. One of them may still be on its way here.
    s.partnerVoidSet = true;
    s.partnerVoidSeq = in.seq;
  }
  if (in.code == kReasonAccepted || in.code == kReasonRoleConflict || in.code == kReasonEnded) {
    notePartnerRole(s, local, in.role, (in.flags & kNoticeFlagRoleChosen) != 0, out);
    if (in.code == kReasonEnded) out.switchRole = false;  // the bond ends: nothing to take
  }
  switch (in.code) {
    case kReasonAccepted:
    case kReasonRoleConflict:
      // A conflict the partner reports is about this device's old role when
      // it takes the other one now; it is asked again with the new one.
      setAgreed(s, out.switchRole ? out.newRole : local.role, in.role,
                in.code == kReasonRoleConflict && !out.switchRole, out);
      if (out.switchRole) askSoon(s, nowMs);
      return out;
    case kReasonNotBonded:
    case kReasonBondedElsewhere:
    case kReasonEnded: {
      // A partner that ended the bond answers the requests that follow with
      // NOT_BONDED or BONDED_ELSEWHERE. That is the same news: "ended" stays
      // the reason shown and no second line is printed for it.
      const bool afterEnded = s.agreement == Agreement::Declined && s.reason == kReasonEnded;
      const uint8_t reason = afterEnded ? (uint8_t)kReasonEnded : in.code;
      const bool wasDeclined = s.agreement == Agreement::Declined;
      const bool changed = !wasDeclined || s.reason != reason;
      out.becameDeclined = !wasDeclined;
      s.agreement = Agreement::Declined;
      s.reason = reason;
      s.roleConflict = false;
      s.announcedAgreed = false;
      s.sameRoleBeats = 0;
      s.requestDue = false;
      s.requestNotBeforeMs = 0;
      // Asked again a minute after the last request; a minute from now when
      // that is longer ago (the partner said this unasked). On the first
      // decline this is worked out from the time of that request, not taken
      // from a deadline that may have been standing since the bond was agreed.
      if (!wasDeclined || !deadlinePending(s.nextRepeatMs, nowMs)) {
        const bool recent = within(s.lastRequestMs, nowMs, kRequestRepeatMs);
        s.nextRepeatMs = deadlineAfter(recent ? s.lastRequestMs : nowMs, kRequestRepeatMs);
      }
      if (changed) {
        out.line = Line::Declined;
        out.lineReason = reason;
      }
      return out;
    }
    default:   // a reason from newer firmware
      s.noticesIgnored++;
      out.ignored = true;
      return out;
  }
}

// What `mac` said about its role in its last request or notice: the partner's
// own, or a peer that asked to bond with this device. False: nothing known.
inline bool roleNoteOf(State& s, const Local& local, const uint8_t mac[6], uint8_t* role, bool* chosen) {
  if (isPartner(local, mac)) {
    if (!s.partnerRoleKnown) return false;
    *role = s.partnerRole;
    *chosen = s.partnerRoleChosen;
    return true;
  }
  const PeerNote* p = peerFind(s, mac);
  if (!p || !p->roleKnown) return false;
  *role = p->role;
  *chosen = p->roleChosen;
  return true;
}

// ---- The partner's bond heartbeat ------------------------------------------------
struct Beat {
  bool proceed;  // false: the partner declined; the heartbeat is not acted on
  Line line;
};

// A bond heartbeat from the partner (the receive table lets only the partner's
// through). A device sends it only to its own partner, so with no answer yet it
// stands for agreement: a partner whose firmware has no BOND_REQUEST bonds as
// before. From a partner that declined it is not acted on; the partner is asked
// instead (a heartbeat queued just before it ended the bond can arrive late).
// The role it carries shows a conflict after kRoleConflictBeats in a row, so
// the moment between the two halves of a role swap is not reported.
inline Beat onPartnerHeartbeat(State& s, uint8_t partnerRole, uint8_t localRole, uint32_t nowMs) {
  Beat beat = {};
  if (s.agreement == Agreement::Declined) {
    askSoon(s, nowMs);
    return beat;
  }
  beat.proceed = true;
  if (s.agreement == Agreement::Unknown) {
    s.agreement = Agreement::Agreed;
    s.nextRepeatMs = 0;   // as in setAgreed
    if (!s.announcedAgreed) {
      s.announcedAgreed = true;
      beat.line = Line::Accepted;
    }
  }
  if (partnerRole == localRole) {
    if (s.sameRoleBeats < kRoleConflictBeats) s.sameRoleBeats++;
    if (s.sameRoleBeats >= kRoleConflictBeats && !s.roleConflict) {
      s.roleConflict = true;
      beat.line = Line::RoleConflict;
    }
  } else {
    s.sameRoleBeats = 0;
    if (s.roleConflict) {
      s.roleConflict = false;
      beat.line = Line::RoleConflictOver;
    }
  }
  return beat;
}

// ---- Asking the partner -----------------------------------------------------------

// This device is about to tell its partner ENDED and then change its bond
// settings. Returns the number that goes into the notice: the last request
// sent (0: none), which the partner takes as void together with every older
// one. From here on no request is planned until the settings change resets
// the state: one sent after the notice would carry a newer number, which the
// notice does not void. If the change never comes, asking goes on after
// kEndingQuietMs. The time is kept as a start, not as a deadline: nothing
// looks at it while the bond is agreed, and a deadline left standing for
// weeks would read as still ahead.
inline uint16_t beginEnding(State& s, uint32_t nowMs) {
  s.endingSinceMs = stamp(nowMs);
  return s.lastSeq;
}

struct RequestPlan {
  bool send;           // BOND_REQUEST to the partner with seq and flags
  bool watchAck;       // ... and count a missing acknowledgement against the session
  bool kickHandshake;  // declined partner, no session: one backed-off handshake start
  bool noBondBuildLine; // the partner just became "built without bond mode": tell the user once
  uint16_t seq;
  uint8_t flags;
};

// The partner's session says its build has no bond mode (Local.partnerNoBond).
// Nothing is sent; looked at again in a minute. With a confirmed word the
// agreement becomes Declined with kReasonNoBondBuild; true when that is new,
// so the user is told once, not on every session. reset() clears the reason,
// so a settings change tells the user again.
inline bool holdForNoBondBuild(State& s, uint32_t nowMs, bool confirmed) {
  s.requestDue = false;
  s.requestNotBeforeMs = 0;
  s.nextRepeatMs = deadlineAfter(nowMs, kRequestRepeatMs);
  if (!confirmed) return false;
  if (s.agreement == Agreement::Declined && s.reason == kReasonNoBondBuild) return false;
  s.agreement = Agreement::Declined;
  s.reason = kReasonNoBondBuild;
  s.roleConflict = false;
  s.announcedAgreed = false;
  s.sameRoleBeats = 0;
  return true;
}

// Cheap test for the tick: is there anything to take or send?
inline bool hasWork(const State& s, uint32_t nowMs) {
  for (const auto& e : s.inbox) {
    if (e.inUse) return true;
  }
  for (const auto& p : s.peers) {
    if (p.inUse && (p.noticeDue || answerReady(p, nowMs))) return true;
  }
  if (within(s.endingSinceMs, nowMs, kEndingQuietMs)) return false;   // planRequest sends nothing yet
  if (s.requestDue) return !deadlinePending(s.requestNotBeforeMs, nowMs);
  return s.agreement != Agreement::Agreed && !deadlinePending(s.nextRepeatMs, nowMs);
}

// Whether a request goes out now. Due at once after a reset, a session coming
// up or the partner's own traffic; otherwise every kRequestRepeatMs while the
// partner has not agreed. Only over an ACTIVE session: without one a due
// request waits (the bond heartbeat brings the session up). A declined partner
// gets no heartbeat, so without a session a handshake start is asked for once
// a minute, whatever was due; the session coming up asks again (onSessionUp).
// Nothing is planned between telling the partner ENDED and the settings change
// that follows (beginEnding). The owner reports the send with requestResult.
//
// watchAck: the acknowledgement of the once-a-minute repeat, and of every
// request to a partner that declined, is watched. Nothing else is sent to such
// a partner, so two requests in a row that nobody acknowledges are the only
// sign that it restarted and lost the session. The first request of a bond or
// a session is not watched: one loss right after the handshake would be held
// against a session that is fine.
inline RequestPlan planRequest(State& s, const Local& local, uint32_t nowMs, bool sessionActive) {
  RequestPlan plan = {};
  if (!local.bonded) {
    // Nobody to ask. Looked at again in a minute; setting up a bond resets this.
    s.requestDue = false;
    s.nextRepeatMs = deadlineAfter(nowMs, kRequestRepeatMs);
    return plan;
  }
  if (within(s.endingSinceMs, nowMs, kEndingQuietMs)) return plan;
  s.endingSinceMs = 0;
  const bool asked = s.requestDue && !deadlinePending(s.requestNotBeforeMs, nowMs);
  const bool repeat = !s.requestDue && s.agreement != Agreement::Agreed &&
                      !deadlinePending(s.nextRepeatMs, nowMs);
  if (!asked && !repeat) return plan;
  // The owner's rule: no BOND_REQUEST to a partner whose session says its build
  // has no bond mode. Checked here, at send time, so neither a turn of the tick
  // between the session going ACTIVE and onSessionUp nor a reset() can send one.
  if (sessionActive && local.partnerNoBond) {
    plan.noBondBuildLine = holdForNoBondBuild(s, nowMs, local.partnerNoBondConfirmed);
    return plan;
  }
  if (!sessionActive) {
    if (s.agreement == Agreement::Declined && s.reason == kReasonNoBondBuild) {
      // No handshake is started for a partner without bond mode: a new
      // session (the partner's own traffic, or after a reflash) asks again.
      s.requestDue = false;
      s.requestNotBeforeMs = 0;
      s.nextRepeatMs = deadlineAfter(nowMs, kRequestRepeatMs);
      return plan;
    }
    if (s.agreement == Agreement::Declined) {
      s.requestDue = false;
      s.requestNotBeforeMs = 0;
      if (!deadlinePending(s.nextRepeatMs, nowMs)) {
        s.nextRepeatMs = deadlineAfter(nowMs, kRequestRepeatMs);
        plan.kickHandshake = true;
      }
    } else if (repeat) {
      s.nextRepeatMs = deadlineAfter(nowMs, kRequestRepeatMs);
    } else {
      s.requestNotBeforeMs = deadlineAfter(nowMs, kSessionRecheckMs);
    }
    return plan;
  }
  plan.send = true;
  plan.flags = asked ? kRequestFlagFresh : 0;
  plan.watchAck = !asked || s.agreement == Agreement::Declined;
  if (++s.lastSeq == 0) s.lastSeq = 1;
  plan.seq = s.lastSeq;
  return plan;
}

// The request planned by planRequest was queued (or not: it is tried again in
// kRequestRetryMs).
inline void requestResult(State& s, uint32_t nowMs, bool queued) {
  if (!queued) {
    s.requestDue = true;
    s.requestNotBeforeMs = deadlineAfter(nowMs, kRequestRetryMs);
    return;
  }
  s.requestDue = false;
  s.requestNotBeforeMs = 0;
  s.lastRequestMs = stamp(nowMs);
  s.nextRepeatMs = deadlineAfter(nowMs, kRequestRepeatMs);
  s.requestsSent++;
}

}  // namespace bondagree
