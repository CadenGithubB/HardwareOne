// The device-name rule (System_DeviceName.h) and the mesh-id helpers
// (System_ESPNow_MeshIdCore.h), both production dependency-free headers.
// Each check names the rule it pins; each fails on the code before the 1.0
// name change (19-byte names with '_', 20-byte fields cut without a NUL, CRC16
// of the label as the mesh id).
#include "System_DeviceName.h"
#include "System_ESPNow_MeshIdCore.h"

#include <cstdio>
#include <cstring>
#include <string>

static int gFailures = 0;
#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);    \
      std::printf(__VA_ARGS__);                           \
      std::printf("\n");                                  \
      gFailures++;                                        \
    }                                                     \
  } while (0)

using devicename::Fault;

static Fault chk(const std::string& s) { return devicename::check(s.c_str(), s.size()); }

static void testCheck() {
  CHECK(chk("a") == Fault::None, "R1 one letter is a name");
  CHECK(chk(std::string(31, 'k')) == Fault::None, "R1 31 bytes are allowed (the old rule stopped at 19)");
  CHECK(chk("Kitchen-Cam-2") == Fault::None, "R1 letters, digits and '-'");
  CHECK(chk("") == Fault::Empty, "R1 empty");
  CHECK(chk(std::string(32, 'k')) == Fault::TooLong, "R1 32 bytes refused");
  CHECK(chk("a_b") == Fault::BadChar, "R1 '_' is refused (the old rule allowed it)");
  CHECK(chk("a b") == Fault::BadChar, "R1 space refused");
  CHECK(chk("caf\xC3\xA9") == Fault::BadChar, "R1 non-ASCII refused");
  CHECK(chk("a.b") == Fault::BadChar, "R1 '.' refused");
  CHECK(chk("-a") == Fault::LeadingHyphen, "R1 leading '-'");
  CHECK(chk("a-") == Fault::TrailingHyphen, "D1 trailing '-'");
  CHECK(chk("clear") == Fault::Reserved && chk("ALL") == Fault::Reserved && chk("Json") == Fault::Reserved &&
            chk("none") == Fault::Reserved,
        "D1 reserved command words, any case");
  CHECK(chk("clearly") == Fault::None, "D1 a reserved word inside a longer name is fine");
  CHECK(chk("AA-BB-CC-DD-EE-FF") == Fault::ReadsAsAddress, "D1 MAC with '-'");
  CHECK(chk("DEADBEEFCAFE") == Fault::ReadsAsAddress, "D1 twelve bare hex digits (macParse accepts it)");
  CHECK(chk("dead-beef-cafe") == Fault::ReadsAsAddress, "D1 twelve hex digits with separators");
  CHECK(chk("a-b-c-d-e-f") == Fault::ReadsAsAddress, "D1 hex groups with five separators");
  CHECK(chk("aa:bb:cc:dd:ee:ff") != Fault::None, "D1 MAC with ':' refused");
  CHECK(chk("DEADBEEFCAF") == Fault::None, "D1 eleven hex digits are a name");
  CHECK(chk("cafe-2") == Fault::None, "D1 a short hex word is a name");
  CHECK(devicename::check(nullptr) == Fault::Empty, "R2 null reads as empty");
  CHECK(std::strlen(devicename::faultText(Fault::BadChar)) > 0 && devicename::faultText(Fault::None)[0] == '\0',
        "R2 one wording per fault");
}

static void testFields() {
  char field[devicename::kFieldLen];
  std::memset(field, 0xAA, sizeof(field));
  CHECK(devicename::putField(field, "bench"), "R3 a valid name is written");
  bool tailZero = true;
  for (size_t i = 5; i < sizeof(field); i++) tailZero = tailZero && field[i] == 0;
  CHECK(std::memcmp(field, "bench", 5) == 0 && tailZero, "R3 every byte after the name is 0");

  const std::string n31(31, 'z');
  CHECK(devicename::putField(field, n31.c_str()) && field[31] == '\0', "R3 a 31-byte name keeps its NUL");
  char out[devicename::kFieldLen];
  CHECK(devicename::getField(field, sizeof(field), out) && n31 == out, "R3 a 31-byte name is read whole");

  std::memset(field, 0x55, sizeof(field));
  CHECK(!devicename::putField(field, "bad name") && field[0] == 0 && field[31] == 0,
        "R3 an invalid name leaves the field empty");

  char noNul[devicename::kFieldLen];
  std::memset(noNul, 'q', sizeof(noNul));
  CHECK(!devicename::getField(noNul, sizeof(noNul), out) && out[0] == '\0',
        "R3 32 bytes without a NUL are no name (the old receivers cut them to 19)");

  char junk[devicename::kFieldLen];
  std::memset(junk, 0x7F, sizeof(junk));
  std::memcpy(junk, "lab\0", 4);
  CHECK(devicename::getField(junk, sizeof(junk), out) && std::string(out) == "lab",
        "R3 bytes after the NUL are ignored");

  char bad[devicename::kFieldLen] = {};
  std::memcpy(bad, "bad name!", 9);
  CHECK(!devicename::getField(bad, sizeof(bad), out) && out[0] == '\0',
        "R3 an invalid received name is absent, never cleaned ('bad_name_')");

  char under[devicename::kFieldLen] = {};
  std::memcpy(under, "Living_Room", 11);
  CHECK(!devicename::getField(under, sizeof(under), out), "R3 an old-rule name with '_' is absent");

  CHECK(!devicename::getField("ok", 2, out), "R3 a field cut before its NUL is no name");
}

static void testHelpers() {
  const char* names[] = {"Kitchen", nullptr, "", "Lab-2"};
  CHECK(devicename::clashIndex("kitchen", names, 4) == 0, "R6 clash ignores case");
  CHECK(devicename::clashIndex("LAB-2", names, 4) == 3, "R6 clash finds a later entry");
  CHECK(devicename::clashIndex("garage", names, 4) == -1, "R6 no clash");
  CHECK(devicename::clashIndex("", names, 4) == -1, "R6 empty never clashes");
  CHECK(devicename::sameIgnoringCase("ABC", "abc") && !devicename::sameIgnoringCase("abc", "abcd"),
        "sameIgnoringCase");

  char adv[40];
  bool complete = false;
  const std::string n29(29, 'b'), n31(31, 'c');
  CHECK(devicename::advertisedName(n29.c_str(), adv, sizeof(adv), &complete) == 29 && complete,
        "R4 a 29-byte name is advertised whole");
  CHECK(devicename::advertisedName(n31.c_str(), adv, sizeof(adv), &complete) == 29 && !complete &&
            std::string(adv) == n31.substr(0, 29),
        "R4 a 31-byte name is advertised shortened to 29");

  char fb[devicename::kFieldLen];
  const uint8_t mac[6] = {0x10, 0x20, 0x30, 0xA1, 0xB2, 0xC3};
  devicename::formatWithMacSuffix(devicename::kFallbackPrefix, mac, fb);
  CHECK(std::string(fb) == "HardwareOne-B2C3" && devicename::valid(fb),
        "D3 fallback name is valid and carries only the last two address bytes: %s", fb);
  devicename::formatWithMacSuffix(devicename::kPeerPrefix, mac, fb);
  CHECK(std::string(fb) == "peer-B2C3" && devicename::valid(fb),
        "R3 peer fallback uses '-', the last two address bytes, and is valid: %s", fb);
  devicename::formatWithMacSuffix(devicename::kFallbackPrefix, nullptr, fb);
  CHECK(std::string(fb) == "HardwareOne-0000" && devicename::valid(fb), "D3 no address gives the 0000 suffix: %s", fb);
  const uint8_t allHex[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  devicename::formatWithMacSuffix(devicename::kFallbackPrefix, allHex, fb);
  CHECK(std::string(fb) == "HardwareOne-EEFF" && devicename::valid(fb), "D3 an all-letter suffix is still valid: %s", fb);
  CHECK(chk("B2C3") == Fault::None && chk("HardwareOne-B2C3") == Fault::None,
        "D3 a name that is only a short suffix is a name, not an address");

  // What a person reads: the fallback carries " (default)", a set name does not.
  char shown[devicename::kDisplayCap];
  CHECK(devicename::displayText("HardwareOne-B2C3", false, shown, sizeof(shown)) == 26 &&
            std::string(shown) == "HardwareOne-B2C3 (default)",
        "D5 the fallback reads 'HardwareOne-B2C3 (default)' (got '%s')", shown);
  CHECK(devicename::displayText("Kitchen-Cam", true, shown, sizeof(shown)) == 11 &&
            std::string(shown) == "Kitchen-Cam",
        "D5 a set name reads as it is (got '%s')", shown);
  const std::string n31name(31, 'n');
  CHECK(devicename::displayText(n31name.c_str(), false, shown, sizeof(shown)) == 41 &&
            std::string(shown) == n31name + " (default)",
        "D5 kDisplayCap holds the longest name and the mark");
  char small[8];
  CHECK(devicename::displayText("HardwareOne-B2C3", false, small, sizeof(small)) == 7 &&
            std::string(small) == "Hardwar",
        "D5 a small buffer is cut, never overrun (got '%s')", small);
  CHECK(devicename::displayText(nullptr, true, shown, sizeof(shown)) == 0 && shown[0] == '\0',
        "D5 a null name reads as empty");

  char sug[devicename::kFieldLen];
  devicename::suggestFromLabel("My Kitchen", sug);
  CHECK(std::string(sug) == "My-Kitchen", "wizard: label 'My Kitchen' suggests 'My-Kitchen' (got '%s')", sug);
  devicename::suggestFromLabel("  Living_Room  (north) ", sug);
  CHECK(std::string(sug) == "Living-Room-north", "wizard: spaces/_ to '-', others dropped (got '%s')", sug);
  devicename::suggestFromLabel("\xC3\xA9\xC3\xA9", sug);
  CHECK(sug[0] == '\0', "wizard: nothing valid left gives no suggestion");
  devicename::suggestFromLabel("all", sug);
  CHECK(sug[0] == '\0', "wizard: a reserved word is not suggested");
  const std::string longLabel(40, 'x');
  devicename::suggestFromLabel(longLabel.c_str(), sug);
  CHECK(std::strlen(sug) == 31, "wizard: suggestion cut to 31");
}

static void testMeshId() {
  uint8_t sub[32] = {};
  CHECK(meshid::pick(sub) == 0, "R7 all-zero subkey gives no id");
  sub[0] = 0x00; sub[1] = 0x00; sub[2] = 0x34; sub[3] = 0x12;
  CHECK(meshid::pick(sub) == 0x1234, "R7 a zero first pair is skipped (a raw u16 read would give 0)");
  uint8_t sub2[32] = {};
  sub2[0] = 0x01; sub2[1] = 0x80;
  CHECK(meshid::pick(sub2) == 0x8001, "R7 pairs are little-endian");
  uint8_t sub3[32] = {};
  sub3[31] = 0x01;
  CHECK(meshid::pick(sub3) == 0x0100, "R7 the last pair counts");

  const uint16_t ids[4] = {0x1111, 0, 0x2222, 0x1111};
  CHECK(meshid::clashSlot(ids, 4, 2, 0x1111) == 0, "R7 clash with another slot");
  CHECK(meshid::clashSlot(ids, 4, 0, 0x1111) == 3, "R7 self is ignored");
  CHECK(meshid::clashSlot(ids, 4, 2, 0) == -1, "R7 0 never clashes");
  CHECK(meshid::clashSlot(ids, 4, 1, 0x3333) == -1, "R7 a new id is free");
  CHECK(std::strlen(meshid::kKdfContext) == 8, "R7 libsodium KDF contexts are 8 bytes");
}

int main() {
  testCheck();
  testFields();
  testHelpers();
  testMeshId();
  if (gFailures) {
    std::printf("%d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("device_name_core: all checks passed\n");
  return 0;
}
