// Room and tag membership rules (System_ESPNow_GroupCore.h, compiled as is).
//
// A peer announces its own room and tags; a room or tag command carries an
// account password to every member. These cases check that only what was
// accepted on this device makes a peer a member:
//   * a newly announced room or tag is pending and not a membership;
//   * accepting records what is announced at that moment;
//   * a room or tag the peer stops announcing leaves the accepted set with
//     that announcement, and announcing it again is pending again;
//   * a change of room is out of the old one at once and pending for the new;
//   * tags are judged one by one;
//   * a repeated announcement reports nothing new, a changed one reports only
//     what newly waits;
//   * a stored record never keeps accepted what it does not announce;
//   * letter case, spaces, empty entries and repeats in a tag list;
//   * a tag command names one tag: a list of tags matches nobody;
//   * a peer that keeps changing is reported once a minute, with the number of
//     changes in between and what waits at the end.
#include "System_ESPNow_GroupCore.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static unsigned gChecks = 0;
#define CHECK(c)                                                                 \
  do {                                                                           \
    ++gChecks;                                                                   \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
      std::exit(1);                                                              \
    }                                                                            \
  } while (0)

using namespace espnowgroup;

static bool is(const char* a, const char* b) { return std::strcmp(a, b) == 0; }

static std::string pendingTagsOf(const Membership& m) {
  char out[kTagsCap];
  pendingTags(m, out, sizeof(out));
  return out;
}

static void testTagLists() {
  CHECK(tagsHave("light,dimmer", "light"));
  CHECK(tagsHave("light, Dimmer ,x", "dimmer"));
  CHECK(tagsHave("light", " LIGHT "));
  CHECK(!tagsHave("lights", "light"));
  CHECK(!tagsHave("light", "lights"));
  CHECK(!tagsHave("", "light"));
  CHECK(!tagsHave("light", ""));
  CHECK(!tagsHave("light", ","));
  CHECK(!tagsHave(nullptr, "light"));
  CHECK(!tagsHave("light", nullptr));
  CHECK(tagCount("a,,b, ,c") == 3);
  CHECK(tagCount("") == 0);

  // The target of a tag command is one tag. A list is neither "the first of
  // them" nor "any of them": it matches nothing.
  const char* one = nullptr;
  size_t len = 0;
  CHECK(singleTag(" light ", one, len) && len == 5 && std::strncmp(one, "light", 5) == 0);
  CHECK(!singleTag("light,door", one, len) && !singleTag(",light", one, len) && !singleTag("light,", one, len));
  CHECK(!singleTag("", one, len) && !singleTag("  ", one, len) && !singleTag(nullptr, one, len));
  CHECK(!tagsHave("light,door", "light,door") && !tagsHave("light,door", "door,light"));
  CHECK(!tagsHave("light", ",light") && !tagsHave("light", "light,"));
  Membership holder{};
  copyText(holder.tags, sizeof(holder.tags), "light,door");
  copyText(holder.acceptedTags, sizeof(holder.acceptedTags), "light");
  CHECK(hasTag(holder, "light") && !hasTag(holder, "light,door") && !hasTag(holder, "light,admin"));
  CHECK(claimsTag(holder, "door") && !claimsTag(holder, "door,light") && !claimsTag(holder, "light"));

  char out[kTagsCap];
  tagsSelect(" a , B,a,,c ", "b", false, out, sizeof(out));
  CHECK(is(out, "a,c"));  // trimmed, each once, in order, without the one `other` has
  tagsSelect("a,B,c", "b,C", true, out, sizeof(out));
  CHECK(is(out, "B,c"));  // spelled as in the first list
  tagsSelect("a,b", "", true, out, sizeof(out));
  CHECK(is(out, ""));

  // A list that does not fit keeps whole tags only.
  char small[6];
  tagsSelect("abc,de,f", "", false, small, sizeof(small));
  CHECK(is(small, "abc,f"));

  CHECK(sameRoom("Kitchen", "kitchen"));
  CHECK(!sameRoom("Kitchen", "Kitchen 2"));
  CHECK(!sameRoom("", ""));  // no room is no group
  CHECK(!sameRoom("Kitchen", nullptr));
}

static void testNewClaimIsPending() {
  Membership m{};
  Change c = announce(m, "Kitchen", "light,dimmer");
  CHECK(c.stored && c.newPendingRoom && is(c.newPendingTags, "light,dimmer") && c.notice());
  CHECK(is(m.room, "Kitchen") && is(m.tags, "light,dimmer"));
  CHECK(is(m.acceptedRoom, "") && is(m.acceptedTags, ""));
  CHECK(roomPending(m) && pendingTagsOf(m) == "light,dimmer" && anyPending(m));
  // Announced, not accepted: no member, and counted as a claim.
  CHECK(!inRoom(m, "Kitchen") && claimsRoom(m, "kitchen"));
  CHECK(!hasTag(m, "light") && claimsTag(m, "LIGHT"));
  CHECK(!claimsRoom(m, "Garage") && !claimsTag(m, "heater"));

  // The same announcement again: nothing to save, nothing to say.
  c = announce(m, "Kitchen", "light,dimmer");
  CHECK(!c.stored && !c.notice());
}

static void testAcceptRecordsWhatIsAnnounced() {
  Membership m{};
  announce(m, "Kitchen", " light , Dimmer,light");
  CHECK(acceptAnnounced(m));
  CHECK(is(m.acceptedRoom, "Kitchen") && is(m.acceptedTags, "light,Dimmer"));
  CHECK(!anyPending(m));
  CHECK(inRoom(m, "kitchen") && !claimsRoom(m, "Kitchen"));
  CHECK(hasTag(m, "dimmer") && hasTag(m, "Light") && !claimsTag(m, "light"));
  CHECK(!acceptAnnounced(m));  // accepting again changes nothing

  // Accepting takes what is announced now, not what was listed earlier.
  Membership n{};
  announce(n, "Garage", "tools");
  announce(n, "Kitchen", "light");
  CHECK(acceptAnnounced(n));
  CHECK(is(n.acceptedRoom, "Kitchen") && is(n.acceptedTags, "light"));
  CHECK(!inRoom(n, "Garage") && !hasTag(n, "tools"));

  // A peer that announces nothing has nothing to accept.
  Membership empty{};
  CHECK(!acceptAnnounced(empty) && is(empty.acceptedRoom, "") && is(empty.acceptedTags, ""));
}

static void testRoomChange() {
  Membership m{};
  announce(m, "Garage", "");
  acceptAnnounced(m);
  CHECK(inRoom(m, "Garage"));

  Change c = announce(m, "Kitchen", "");
  CHECK(c.stored && is(c.leftRoom, "Garage") && c.newPendingRoom);
  CHECK(!inRoom(m, "Garage") && !inRoom(m, "Kitchen"));  // out of the old at once, not in the new
  CHECK(claimsRoom(m, "Kitchen") && !claimsRoom(m, "Garage"));
  CHECK(is(m.acceptedRoom, ""));

  // Going back is growing again: it waits, and it is said once.
  c = announce(m, "Garage", "");
  CHECK(c.newPendingRoom && !inRoom(m, "Garage"));
  c = announce(m, "Garage", "");
  CHECK(!c.notice() && !c.stored);

  // Clearing the room leaves it, without a line.
  acceptAnnounced(m);
  CHECK(inRoom(m, "Garage"));
  c = announce(m, "", "");
  CHECK(c.stored && is(c.leftRoom, "Garage") && !c.notice());
  CHECK(!inRoom(m, "Garage") && !roomPending(m) && !anyPending(m));

  // The same room in another spelling is the same room.
  Membership n{};
  announce(n, "Kitchen", "");
  acceptAnnounced(n);
  c = announce(n, "KITCHEN", "");
  CHECK(c.stored && !c.notice() && is(c.leftRoom, ""));
  CHECK(inRoom(n, "kitchen") && is(n.acceptedRoom, "KITCHEN"));
}

static void testTagsOneByOne() {
  Membership m{};
  announce(m, "", "light,dimmer");
  acceptAnnounced(m);

  // One more tag: the accepted ones stay, the new one waits.
  Change c = announce(m, "", "light,dimmer,heater");
  CHECK(c.stored && !c.newPendingRoom && is(c.newPendingTags, "heater") && is(c.leftTags, ""));
  CHECK(hasTag(m, "light") && hasTag(m, "dimmer") && !hasTag(m, "heater") && claimsTag(m, "heater"));
  CHECK(pendingTagsOf(m) == "heater");

  // One accepted tag dropped: it leaves at once; the pending one still waits
  // and is not said again.
  c = announce(m, "", "light,heater");
  CHECK(c.stored && is(c.leftTags, "dimmer") && !c.notice());
  CHECK(hasTag(m, "light") && !hasTag(m, "dimmer") && !hasTag(m, "heater"));
  CHECK(is(m.acceptedTags, "light"));

  // A second new tag is said alone.
  c = announce(m, "", "light,heater,fan");
  CHECK(is(c.newPendingTags, "fan"));
  CHECK(pendingTagsOf(m) == "heater,fan");

  // The dropped tag comes back: pending, not accepted from memory.
  c = announce(m, "", "light,heater,fan,dimmer");
  CHECK(is(c.newPendingTags, "dimmer") && !hasTag(m, "dimmer"));

  acceptAnnounced(m);
  CHECK(is(m.acceptedTags, "light,heater,fan,dimmer") && !anyPending(m));

  // Order and spacing alone change nothing that matters.
  c = announce(m, "", "dimmer , fan,heater,LIGHT");
  CHECK(!c.notice() && is(c.leftTags, ""));
  CHECK(hasTag(m, "light") && hasTag(m, "fan") && is(m.acceptedTags, "dimmer,fan,heater,LIGHT"));

  // All tags cleared.
  c = announce(m, "", "");
  CHECK(is(c.leftTags, "dimmer,fan,heater,LIGHT") && is(m.acceptedTags, "") && !c.notice());
}

static void testDropAndStoredRecord() {
  Membership m{};
  announce(m, "Kitchen", "light");
  acceptAnnounced(m);
  CHECK(dropAccepted(m));
  CHECK(!inRoom(m, "Kitchen") && !hasTag(m, "light") && claimsRoom(m, "Kitchen") && claimsTag(m, "light"));
  CHECK(!dropAccepted(m));
  // Still announced, so a repeat of the announcement says nothing: the admin dropped it.
  Change c = announce(m, "Kitchen", "light");
  CHECK(!c.stored && !c.notice());

  // A record written before the accepted fields existed: nothing accepted.
  Membership old{};
  copyText(old.room, sizeof(old.room), "Kitchen");
  copyText(old.tags, sizeof(old.tags), "light");
  CHECK(!reconcile(old));
  CHECK(!inRoom(old, "Kitchen") && !hasTag(old, "light") && roomPending(old));

  // A stored record cannot keep accepted what it does not announce.
  Membership edited{};
  copyText(edited.room, sizeof(edited.room), "Garage");
  copyText(edited.tags, sizeof(edited.tags), "light");
  copyText(edited.acceptedRoom, sizeof(edited.acceptedRoom), "Kitchen");
  copyText(edited.acceptedTags, sizeof(edited.acceptedTags), "light,admin");
  CHECK(reconcile(edited));
  CHECK(is(edited.acceptedRoom, "") && is(edited.acceptedTags, "light"));
  CHECK(!inRoom(edited, "Kitchen") && !hasTag(edited, "admin") && hasTag(edited, "light"));

  // A consistent record is left as it is.
  Membership good{};
  announce(good, "Kitchen", "light,dimmer");
  acceptAnnounced(good);
  const Membership before = good;
  CHECK(!reconcile(good) && std::memcmp(&before, &good, sizeof(good)) == 0);
}

static void testText() {
  Membership m{};
  char line[224];

  Change c = announce(m, "Kitchen", "");
  formatRoomNotice(line, sizeof(line), "Fridge", m, c, "Fridge");
  CHECK(is(line, "Fridge says it is in room 'Kitchen'; not used for room commands until accepted here: "
                 "espnowgroupaccept Fridge"));
  formatTagNotice(line, sizeof(line), "Fridge", c, "Fridge");
  CHECK(is(line, ""));

  c = announce(m, "Kitchen", "light");
  formatRoomNotice(line, sizeof(line), "Fridge", m, c, "Fridge");
  CHECK(is(line, ""));  // the room was said with the frame before
  formatTagNotice(line, sizeof(line), "Fridge", c, "AA:BB:CC:DD:EE:02");
  CHECK(is(line, "Fridge says it has tag 'light'; not used for tag commands until accepted here: "
                 "espnowgroupaccept AA:BB:CC:DD:EE:02"));

  c = announce(m, "Kitchen", "light,a,b");
  formatTagNotice(line, sizeof(line), "", c, "x");
  CHECK(is(line, "A peer says it has tags 'a,b'; not used for tag commands until accepted here: espnowgroupaccept x"));

  // The longest names still leave the command whole in a log line (255 bytes
  // with its "[EVENT][ESPNOW] " lead).
  Membership big{};
  const std::string room(kRoomCap - 1, 'R');
  const std::string tags = std::string(30, 't') + "," + std::string(32, 'u');
  const std::string who(31, 'N');
  c = announce(big, room.c_str(), tags.c_str());
  formatRoomNotice(line, sizeof(line), who.c_str(), big, c, who.c_str());
  CHECK(std::strlen(line) + 16 < 255 && std::strstr(line, ("espnowgroupaccept " + who).c_str()) != nullptr);
  formatTagNotice(line, sizeof(line), who.c_str(), c, who.c_str());
  CHECK(std::strlen(line) + 16 < 255 && std::strstr(line, ("espnowgroupaccept " + who).c_str()) != nullptr);

  char text[kDescribeCap];
  describe(text, sizeof(text), "Kitchen", "a,b");
  CHECK(is(text, "room 'Kitchen', tags 'a,b'"));
  describe(text, sizeof(text), "Kitchen", "");
  CHECK(is(text, "room 'Kitchen'"));
  describe(text, sizeof(text), "", "a");
  CHECK(is(text, "tag 'a'"));
  describe(text, sizeof(text), "", "");
  CHECK(is(text, "nothing"));
  describe(text, sizeof(text), room.c_str(), tags.c_str());
  CHECK(std::strlen(text) < kDescribeCap - 1);

  CHECK(is(kAcceptCommand, "espnowgroupaccept") && is(kDropCommand, "espnowgroupdrop") &&
        is(kListCommand, "espnowgroups"));
}

// One report per peer and minute; the changes in between are counted and
// reported together.
static void testNoticeGate() {
  NoticeGate gate{};
  uint32_t now = 5000;
  CHECK(noticeHeldDue(gate, now) == 0);  // nothing was ever said
  CHECK(noticeNow(gate, now));           // the first change is reported at once
  CHECK(noticeHeldDue(gate, now) == 0);
  for (int i = 1; i <= 7; ++i) CHECK(!noticeNow(gate, now + (uint32_t)i * 1000));
  CHECK(gate.held == 7);
  CHECK(noticeHeldDue(gate, now + kNoticeGapMs - 1) == 0);
  CHECK(noticeHeldDue(gate, now + kNoticeGapMs) == 7);  // due once, with the count
  CHECK(noticeHeldDue(gate, now + kNoticeGapMs) == 0);
  // That report starts the next minute.
  CHECK(!noticeNow(gate, now + kNoticeGapMs + 10));
  CHECK(noticeHeldDue(gate, now + 2 * kNoticeGapMs - 1) == 0);
  CHECK(noticeHeldDue(gate, now + 2 * kNoticeGapMs) == 1);
  // A quiet peer is reported at once again.
  CHECK(noticeNow(gate, now + 4 * kNoticeGapMs) && gate.held == 0);
  // The clock running over does not hold a report back for long, and the
  // counter does not run over.
  NoticeGate late{};
  CHECK(noticeNow(late, 0xFFFFFF00u));
  CHECK(!noticeNow(late, 0x00000010u));
  CHECK(noticeHeldDue(late, 0xFFFFFF00u + kNoticeGapMs) == 1);
  NoticeGate busy{};
  CHECK(noticeNow(busy, 1));
  busy.held = UINT16_MAX;
  CHECK(!noticeNow(busy, 2) && busy.held == UINT16_MAX);

  // What waits now, as the report after held-back changes gives it.
  Membership m{};
  announce(m, "Kitchen", "light,dimmer");
  acceptAnnounced(m);
  Change none = waitingNow(m);
  CHECK(!none.notice());
  announce(m, "Garage", "light,dimmer,heater");
  Change waits = waitingNow(m);
  CHECK(waits.newPendingRoom && is(waits.newPendingTags, "heater") && waits.notice());
  char line[224];
  formatHeldNotice(line, sizeof(line), "Lamp", 49, true);
  CHECK(is(line, "Lamp changed its room or tags 49 more time(s) within a minute; what waits for acceptance now "
                 "follows"));
  formatHeldNotice(line, sizeof(line), "", 1, false);
  CHECK(is(line, "A peer changed its room or tags 1 more time(s) within a minute; nothing of it waits for acceptance "
                 "now"));
  const std::string who(31, 'n');
  formatHeldNotice(line, sizeof(line), who.c_str(), 65535, true);
  CHECK(std::strlen(line) + 16 < 255);
}

// Text longer than a field is cut, never written past it.
static void testBounds() {
  Membership m{};
  const std::string longRoom(200, 'r');
  const std::string longTags = std::string(40, 'a') + "," + std::string(40, 'b') + "," + std::string(40, 'c');
  Change c = announce(m, longRoom.c_str(), longTags.c_str());
  CHECK(std::strlen(m.room) == kRoomCap - 1 && std::strlen(m.tags) == kTagsCap - 1);
  CHECK(c.newPendingRoom && std::strlen(c.newPendingTags) < kTagsCap);
  acceptAnnounced(m);
  CHECK(std::strlen(m.acceptedRoom) == kRoomCap - 1 && std::strlen(m.acceptedTags) < kTagsCap);
  CHECK(!anyPending(m));
  announce(m, nullptr, nullptr);
  CHECK(is(m.room, "") && is(m.tags, "") && is(m.acceptedRoom, "") && is(m.acceptedTags, ""));
}

int main() {
  testTagLists();
  testNewClaimIsPending();
  testAcceptRecordsWhatIsAnnounced();
  testRoomChange();
  testTagsOneByOne();
  testDropAndStoredRecord();
  testText();
  testNoticeGate();
  testBounds();
  std::printf("espnow_group_core_tests: %u checks passed\n", gChecks);
  return 0;
}
