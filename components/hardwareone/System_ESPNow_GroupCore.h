#pragma once
// Which room and tags of a paired peer this device uses for group commands
// (espnowroomcmd, espnowtagcmd), without the radio, the registry or the console.
//
// A peer announces its own room and tags in its metadata frame. A group
// command carries an account name and password to every member of the group,
// so an announcement alone must not make a peer a member: any paired device
// could name the room it wants the password of. This device therefore keeps,
// per peer, what its admin ACCEPTED, next to what the peer ANNOUNCES:
//   * Growing needs a confirm. A room or tag the peer announces that is not in
//     its accepted set is PENDING. It is shown, and it is not used.
//   * Shrinking does not. A room or tag the peer stops announcing leaves the
//     accepted set at once: a peer may always leave a group. Announcing it
//     again later is growing again.
//   * Accepting records what is announced at that moment, nothing else.
// So a peer that moves from Garage to Kitchen is out of Garage with that
// frame and pending for Kitchen until the admin accepts it.
//
// Rooms and tags compare without regard to ASCII case, as the group commands
// have always matched them. A tag list is comma-separated; spaces around a tag
// do not count and an empty entry is no tag.
//
// No OS dependencies: the caller copies a record in and out and prints what
// this returns, so the host test runs this file as is (espnow_group_core_tests).
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace espnowgroup {

constexpr size_t kRoomCap = 32;  // a room with its NUL, as the device registry stores it
constexpr size_t kTagsCap = 64;  // a tag list with its NUL, as the device registry stores it

// The commands a reply or a notice names.
constexpr char kListCommand[] = "espnowgroups";
constexpr char kAcceptCommand[] = "espnowgroupaccept";
constexpr char kDropCommand[] = "espnowgroupdrop";

// One peer: what it announces and what was accepted on this device.
struct Membership {
  char room[kRoomCap];          // announced, as sent
  char tags[kTagsCap];          // announced, as sent
  char acceptedRoom[kRoomCap];  // empty: no room accepted
  char acceptedTags[kTagsCap];  // each accepted tag once, comma-separated
};

// What one announcement changed.
struct Change {
  bool stored;                    // the record differs from before and has to be saved
  char leftRoom[kRoomCap];        // the accepted room the peer no longer announces
  char leftTags[kTagsCap];        // the accepted tags the peer no longer announces
  bool newPendingRoom;            // the announced room waits now and did not wait before
  char newPendingTags[kTagsCap];  // the tags that wait now and did not wait before
  bool notice() const { return newPendingRoom || newPendingTags[0] != '\0'; }
};

inline char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

inline bool sameText(const char* a, size_t aLen, const char* b, size_t bLen) {
  if (aLen != bLen) return false;
  for (size_t i = 0; i < aLen; i++) {
    if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
  }
  return true;
}

// Bounded copy; a text that does not fit is cut at the capacity.
inline void copyText(char* out, size_t cap, const char* text) {
  if (!out || cap == 0) return;
  size_t i = 0;
  if (text) {
    for (; i + 1 < cap && text[i] != '\0'; i++) out[i] = text[i];
  }
  out[i] = '\0';
}

// Two names of the same room. No room is not a room: empty never matches.
inline bool sameRoom(const char* a, const char* b) {
  if (!a || !b || !a[0] || !b[0]) return false;
  return sameText(a, std::strlen(a), b, std::strlen(b));
}

inline bool tagSpace(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

// The next tag of a comma-separated list, without the spaces around it. Empty
// entries are skipped. Returns false at the end of the list.
inline bool nextTag(const char*& cursor, const char*& tag, size_t& len) {
  while (cursor && *cursor) {
    const char* start = cursor;
    const char* end = start;
    while (*end && *end != ',') ++end;
    cursor = *end ? end + 1 : end;
    while (start < end && tagSpace(*start)) ++start;
    while (end > start && tagSpace(end[-1])) --end;
    if (end > start) {
      tag = start;
      len = static_cast<size_t>(end - start);
      return true;
    }
  }
  return false;
}

inline bool tagsHave(const char* list, const char* tag, size_t len) {
  if (!tag || len == 0) return false;
  const char* cursor = list;
  const char* have;
  size_t haveLen;
  while (nextTag(cursor, have, haveLen)) {
    if (sameText(have, haveLen, tag, len)) return true;
  }
  return false;
}

// The one tag a text names, without the spaces around it. False when the text
// names none or more than one: a tag command goes to one tag, and "light,door"
// must not quietly mean "light".
inline bool singleTag(const char* text, const char*& tag, size_t& len) {
  if (!text || std::strchr(text, ',')) return false;
  const char* cursor = text;
  return nextTag(cursor, tag, len);
}

inline bool tagsHave(const char* list, const char* tag) {
  const char* one;
  size_t len;
  return singleTag(tag, one, len) && tagsHave(list, one, len);
}

// Appends one tag to a comma-separated list. False when it does not fit; the
// list is then left as it was.
inline bool tagsAppend(char* out, size_t cap, const char* tag, size_t len) {
  const size_t used = std::strlen(out);
  const size_t need = used + (used ? 1 : 0) + len + 1;
  if (need > cap) return false;
  size_t pos = used;
  if (used) out[pos++] = ',';
  std::memcpy(out + pos, tag, len);
  out[pos + len] = '\0';
  return true;
}

// out = the tags of `list`, each once and in its order, that `other` has
// (inOther) or does not have (!inOther).
inline void tagsSelect(const char* list, const char* other, bool inOther, char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';
  const char* cursor = list;
  const char* tag;
  size_t len;
  while (nextTag(cursor, tag, len)) {
    if (tagsHave(other, tag, len) != inOther) continue;
    if (tagsHave(out, tag, len)) continue;
    (void)tagsAppend(out, cap, tag, len);
  }
}

inline int tagCount(const char* list) {
  int n = 0;
  const char* cursor = list;
  const char* tag;
  size_t len;
  while (nextTag(cursor, tag, len)) n++;
  return n;
}

// ---- What a record says ------------------------------------------------------

// The announced room waits for acceptance.
inline bool roomPending(const Membership& m) { return m.room[0] != '\0' && !sameRoom(m.acceptedRoom, m.room); }

// The announced tags that wait for acceptance.
inline void pendingTags(const Membership& m, char* out, size_t cap) {
  tagsSelect(m.tags, m.acceptedTags, false, out, cap);
}

inline bool anyPending(const Membership& m) {
  char tags[kTagsCap];
  pendingTags(m, tags, sizeof(tags));
  return roomPending(m) || tags[0] != '\0';
}

// Member of the room / holder of the tag for a group command: accepted here.
inline bool inRoom(const Membership& m, const char* room) { return sameRoom(m.acceptedRoom, room); }
inline bool hasTag(const Membership& m, const char* tag) { return tagsHave(m.acceptedTags, tag); }

// Says it is in the room / has the tag, and that was not accepted here: the
// peers a group command leaves out and counts.
inline bool claimsRoom(const Membership& m, const char* room) {
  return sameRoom(m.room, room) && !inRoom(m, room);
}
inline bool claimsTag(const Membership& m, const char* tag) {
  return tagsHave(m.tags, tag) && !hasTag(m, tag);
}

// ---- What changes a record ---------------------------------------------------

// The peer announced `room` and `tags`. Records them as sent, takes out of the
// accepted set what the peer no longer announces, and reports what newly
// waits. A repeat of the same announcement changes and reports nothing.
inline Change announce(Membership& m, const char* room, const char* tags) {
  Change change{};
  const Membership before = m;
  const bool roomWaited = roomPending(before);
  char tagsWaited[kTagsCap];
  pendingTags(before, tagsWaited, sizeof(tagsWaited));

  copyText(m.room, sizeof(m.room), room);
  copyText(m.tags, sizeof(m.tags), tags);

  if (before.acceptedRoom[0] != '\0') {
    if (sameRoom(before.acceptedRoom, m.room)) {
      // The same room in another spelling stays accepted, spelled as announced.
      copyText(m.acceptedRoom, sizeof(m.acceptedRoom), m.room);
    } else {
      copyText(change.leftRoom, sizeof(change.leftRoom), before.acceptedRoom);
      m.acceptedRoom[0] = '\0';
    }
  }
  tagsSelect(before.acceptedTags, m.tags, false, change.leftTags, sizeof(change.leftTags));
  tagsSelect(m.tags, before.acceptedTags, true, m.acceptedTags, sizeof(m.acceptedTags));

  change.newPendingRoom = roomPending(m) && !(roomWaited && sameRoom(before.room, m.room));
  char tagsWaiting[kTagsCap];
  pendingTags(m, tagsWaiting, sizeof(tagsWaiting));
  tagsSelect(tagsWaiting, tagsWaited, false, change.newPendingTags, sizeof(change.newPendingTags));

  change.stored = std::strcmp(before.room, m.room) != 0 || std::strcmp(before.tags, m.tags) != 0 ||
                  std::strcmp(before.acceptedRoom, m.acceptedRoom) != 0 ||
                  std::strcmp(before.acceptedTags, m.acceptedTags) != 0;
  return change;
}

// A record as it was stored: nothing counts as accepted that the stored
// announcement does not name. Returns whether the record changed.
inline bool reconcile(Membership& m) {
  const Membership stored = m;
  return announce(m, stored.room, stored.tags).stored;
}

// The admin accepts what the peer announces right now. Returns whether the
// record changed; the caller reports m.acceptedRoom and m.acceptedTags.
inline bool acceptAnnounced(Membership& m) {
  const Membership before = m;
  copyText(m.acceptedRoom, sizeof(m.acceptedRoom), m.room);
  tagsSelect(m.tags, m.tags, true, m.acceptedTags, sizeof(m.acceptedTags));
  return std::strcmp(before.acceptedRoom, m.acceptedRoom) != 0 ||
         std::strcmp(before.acceptedTags, m.acceptedTags) != 0;
}

// Nothing of this peer is accepted any more. Returns whether anything was.
inline bool dropAccepted(Membership& m) {
  const bool had = m.acceptedRoom[0] != '\0' || m.acceptedTags[0] != '\0';
  m.acceptedRoom[0] = '\0';
  m.acceptedTags[0] = '\0';
  return had;
}

// ---- How often a peer's changes are reported -----------------------------------

// A peer decides how often it changes what it announces, and every change that
// leaves something waiting is worth a line in the event log. So the lines one
// peer can cause are limited: one report, then nothing for kNoticeGapMs. The
// changes inside that time are counted, and when it is over one more report
// says how many there were and what waits now.
constexpr uint32_t kNoticeGapMs = 60000;

struct NoticeGate {
  bool said;        // a report was printed at saidMs
  uint32_t saidMs;
  uint16_t held;    // changes since then that printed nothing
};

// A change left something waiting at `nowMs`. True: report it now. False: it
// was counted, and noticeHeldDue() says when to report.
inline bool noticeNow(NoticeGate& gate, uint32_t nowMs) {
  if (gate.said && static_cast<uint32_t>(nowMs - gate.saidMs) < kNoticeGapMs) {
    if (gate.held < UINT16_MAX) gate.held++;
    return false;
  }
  gate.said = true;
  gate.saidMs = nowMs;
  gate.held = 0;
  return true;
}

// The number of counted changes that are due to be reported now, or 0. A
// number is returned once; the next gap starts with it.
inline uint16_t noticeHeldDue(NoticeGate& gate, uint32_t nowMs) {
  if (!gate.said || gate.held == 0 || static_cast<uint32_t>(nowMs - gate.saidMs) < kNoticeGapMs) return 0;
  const uint16_t held = gate.held;
  gate.held = 0;
  gate.saidMs = nowMs;
  return held;
}

// Everything that waits in `m`, in the form of a change: for the report that
// follows held-back changes.
inline Change waitingNow(const Membership& m) {
  Change change{};
  change.newPendingRoom = roomPending(m);
  pendingTags(m, change.newPendingTags, sizeof(change.newPendingTags));
  return change;
}

// ---- Text ---------------------------------------------------------------------

constexpr size_t kDescribeCap = 120;  // the longest room and tag list in describe()

// A room and a tag list in words: "room 'Kitchen', tags 'light,dimmer'", one of
// the two alone, or "nothing".
inline void describe(char* out, size_t cap, const char* room, const char* tags) {
  if (!out || cap == 0) return;
  const bool hasRoom = room && room[0] != '\0';
  const bool hasTags = tags && tags[0] != '\0';
  const char* tagWord = (hasTags && tagCount(tags) == 1) ? "tag" : "tags";
  if (hasRoom && hasTags) {
    std::snprintf(out, cap, "room '%s', %s '%s'", room, tagWord, tags);
  } else if (hasRoom) {
    std::snprintf(out, cap, "room '%s'", room);
  } else if (hasTags) {
    std::snprintf(out, cap, "%s '%s'", tagWord, tags);
  } else {
    std::snprintf(out, cap, "nothing");
  }
}

// The lines for an announcement that left something waiting: `m` after the
// announcement and the `change` it returned. `who` is the peer's name on this
// device and `target` what the accept command takes for it. A room and tags
// get a line each, so the longest names still leave the command readable in
// one log line. Each is empty when nothing new of its kind waits.
inline void formatRoomNotice(char* out, size_t cap, const char* who, const Membership& m, const Change& change,
                             const char* target) {
  if (!out || cap == 0) return;
  out[0] = '\0';
  if (!change.newPendingRoom) return;
  std::snprintf(out, cap, "%s says it is in room '%s'; not used for room commands until accepted here: %s %s",
                (who && who[0]) ? who : "A peer", m.room, kAcceptCommand, target ? target : "");
}

inline void formatTagNotice(char* out, size_t cap, const char* who, const Change& change, const char* target) {
  if (!out || cap == 0) return;
  out[0] = '\0';
  if (change.newPendingTags[0] == '\0') return;
  std::snprintf(out, cap, "%s says it has %s '%s'; not used for tag commands until accepted here: %s %s",
                (who && who[0]) ? who : "A peer", tagCount(change.newPendingTags) == 1 ? "tag" : "tags",
                change.newPendingTags, kAcceptCommand, target ? target : "");
}

// The line before the report of what waits after held-back changes.
inline void formatHeldNotice(char* out, size_t cap, const char* who, unsigned held, bool anythingWaits) {
  if (!out || cap == 0) return;
  std::snprintf(out, cap, "%s changed its room or tags %u more time(s) within a minute; %s",
                (who && who[0]) ? who : "A peer", held,
                anythingWaits ? "what waits for acceptance now follows" : "nothing of it waits for acceptance now");
}

}  // namespace espnowgroup
