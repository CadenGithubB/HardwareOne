// Host harness for test_espnow_mesh_slots.py.
//
// The runner pastes the production Settings::MeshIdentity record into the
// Settings double and the production mesh-slot and device-name code at the
// production marker: meshFingerprintForLabel, setEspNowPassphrase,
// initPrimaryMeshFromLegacySettings, the start-passphrase and hint helpers, the
// espnowmeshes subcommands (add, remove, delete, enable, setdefault,
// setpassphrase) and cmd_espnow_setname. Everything else here is a small double:
// settings writes, the key cache, stored identities, events and the ESP-NOW
// state. No radio or flash is touched.
#include <Arduino.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

void webBatchTestStringAppend() {}

static int gFailures = 0;
#define CHECK(cond, ...)                                              \
  do {                                                                \
    if (!(cond)) {                                                    \
      ++gFailures;                                                    \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                \
      std::printf(__VA_ARGS__);                                       \
      std::printf("\n");                                              \
    }                                                                 \
  } while (0)

#define ENABLE_BONDED_MODE 1

// ---- Settings double: the production mesh record inside a small Settings.
struct Settings {
  static constexpr uint8_t N_MESHES = 4;
  // INSERT_MESH_IDENTITY
  MeshIdentity meshes[N_MESHES];
  String espnowDeviceName;
  bool espnowFirstTimeSetup = false;
  bool bondModeEnabledMesh[N_MESHES] = {};
  uint8_t bondRoleMesh[N_MESHES] = {};
  String bondPeerMacMesh[N_MESHES];
  bool bondModeEnabled = false;
  uint8_t bondRole = 0;
  String bondPeerMac;
};
static Settings gSettings;

static int gPersists = 0;
static bool gPersistOk = true;
static bool requestSettingsPersist() {
  ++gPersists;
  return gPersistOk;
}
static void setSetting(String& field, const String& value) {
  field = value;
  ++gPersists;
}
static void setSetting(bool& field, bool value) {
  field = value;
  ++gPersists;
}

// ---- ESP-NOW state double.
struct EspNowDeviceDouble {
  uint8_t mac[6];
  String name;
};
struct EspNowState {
  bool initialized = false;
  String passphrase;
  uint8_t derivedKey[16] = {};
  bool encryptionEnabled = false;
  String deviceName;
  EspNowDeviceDouble devices[4];
  int deviceCount = 0;
};
static EspNowState gState;
static EspNowState* gEspNow = &gState;
static bool gShutdownPending = false;
static bool espnowShutdownPending() { return gShutdownPending; }
static const uint8_t kSelfMac[6] = {0x02, 0, 0, 0, 0, 0x01};
enum { ESP_OK = 0, ESP_FAIL = -1 };
static bool gSelfMacReadable = true;
static int espnowSelfStaMac(uint8_t mac[6]) {
  if (!gSelfMacReadable) return ESP_FAIL;
  std::memcpy(mac, kSelfMac, 6);
  return ESP_OK;
}
static void addEspNowDevice(const uint8_t* mac, const String& name, bool, const uint8_t*) {
  if (gState.deviceCount >= 4) return;
  std::memcpy(gState.devices[gState.deviceCount].mac, mac, 6);
  gState.devices[gState.deviceCount].name = name;
  ++gState.deviceCount;
}

// Legacy key: a marker byte stands for "derived from a non-empty passphrase".
void deriveKeyFromPassphrase(const String& passphrase, uint8_t* key) {
  std::memset(key, passphrase.length() ? 0xA5 : 0x00, 16);
  gState.encryptionEnabled = passphrase.length() > 0;
}

// ---- Mesh key cache doubles.
static bool gDerivedValid[Settings::N_MESHES] = {};
static int gInvalidations[Settings::N_MESHES] = {};
static void meshKeysInvalidate(uint8_t i) {
  gDerivedValid[i] = false;
  ++gInvalidations[i];
}
static bool meshKeysStretchPassphrase(uint8_t i) {
  std::memset(gSettings.meshes[i].passphraseStretchedKey, 0x5A, 32);
  gSettings.meshes[i].passphraseStretchedKeyValid = true;
  return true;
}
static bool meshKeysDerive(uint8_t i) {
  gDerivedValid[i] = true;
  return true;
}
static void sodium_memzero(void* p, size_t n) { std::memset(p, 0, n); }

// CRC16-CCITT (0x1021, init 0xFFFF), as the wire code computes label fingerprints.
static uint16_t v4_crc16_ccitt(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

// ---- Stored identities double (slot i is returned only while valid, as in production).
struct PeerIdentity {
  uint8_t mac[6];
  uint8_t meshId;
  bool valid;
};
static PeerIdentity gPeers[16] = {};
static uint8_t peerIdentitySlotCount() { return 16; }
static const PeerIdentity* peerIdentityAt(uint8_t i) {
  if (i >= 16) return nullptr;
  return gPeers[i].valid ? &gPeers[i] : nullptr;
}

// ---- Command plumbing doubles.
static char gDebugBuffer[1024];
static bool ensureDebugBuffer() { return true; }
static char* getDebugBuffer() { return gDebugBuffer; }
static bool gMetadataChanged = false;
static bool gCliValidateOnly = false;
#define RETURN_VALID_IF_VALIDATE_CSTR() \
  do {                                  \
    if (gCliValidateOnly) return "VALID"; \
  } while (0)
enum SystemEventKind : uint8_t { SYSEVT_SETTING_CHANGED = 7, SYSEVT_MESH_PASSPHRASE_CHANGED = 9 };
struct Event {
  uint8_t kind;
  std::string subject, detail;
};
static std::vector<Event> gEvents;
static void systemEventPost(uint8_t kind, const char* subject = nullptr, const char* detail = nullptr) {
  gEvents.push_back({kind, subject ? subject : "", detail ? detail : ""});
}
// Whitespace-split arguments, enough for the name command.
class CommandArgs {
public:
  explicit CommandArgs(const String& input) {
    std::string s = input.c_str();
    size_t i = 0;
    while (i < s.size()) {
      while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
      size_t j = i;
      while (j < s.size() && !std::isspace((unsigned char)s[j])) ++j;
      if (j > i) args_.push_back(String(s.substr(i, j - i).c_str()));
      i = j;
    }
  }
  int count() const { return (int)args_.size(); }
  const String& arg(int i) const { return (i >= 0 && i < count()) ? args_[(size_t)i] : empty_; }
  bool hasMinArgs(int n) const { return count() >= n; }

private:
  std::vector<String> args_;
  String empty_;
};

// INSERT_PRODUCTION

#if !HW1_HAVE_MESH_DELETE
// Pre-fix trees have no delete subcommand; the checks that need it then fail by name.
static const char* meshesCmd_delete(const String&) { return "Error: no delete subcommand"; }
#endif
#if !HW1_HAVE_PASSPHRASE_HINT
static void formatNoPassphraseWarning(char* out, size_t outLen) {
  std::snprintf(out, outLen, "%s", "Run 'espnowsetpassphrase primary <pw>'.");
}
#endif

// ---- Test helpers.
static bool has(const char* reply, const char* needle) { return reply && std::strstr(reply, needle) != nullptr; }
static bool isError(const char* reply) { return reply && std::strncmp(reply, "Error", 5) == 0; }
static void resetAll() {
  gSettings = Settings();
  gState = EspNowState();
  gShutdownPending = false;
  gSelfMacReadable = true;
  gPersists = 0;
  gPersistOk = true;
  gEvents.clear();
  gMetadataChanged = false;
  std::memset(gPeers, 0, sizeof(gPeers));
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    gDerivedValid[i] = false;
    gInvalidations[i] = 0;
  }
  initPrimaryMeshFromLegacySettings();  // what the first ESP-NOW start does on a new device
}
static int enabledDefaults() {
  int n = 0;
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    if (gSettings.meshes[i].enabled && gSettings.meshes[i].isDefault) ++n;
  }
  return n;
}
static int slotOf(const char* label) {
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    if (gSettings.meshes[i].label == String(label)) return i;
  }
  return -1;
}
static bool slotIsEmpty(uint8_t i) {
  const Settings::MeshIdentity& m = gSettings.meshes[i];
  for (uint8_t b : m.passphraseStretchedKey) {
    if (b) return false;
  }
  return m.label.length() == 0 && m.passphrase.length() == 0 && !m.enabled && !m.isDefault &&
         m.fingerprint == 0 && !m.passphraseStretchedKeyValid;
}
// The device as the bench boards were left: slot 0 renamed, keyed, then cleared.
static void makeBenchLeftover() {
  resetAll();
  gSettings.meshes[0].label = "bench";
  gSettings.meshes[0].fingerprint = meshFingerprintForLabel(String("bench"));
  CHECK(!isError(meshesCmd_setpassphrase(String("bench"), String("correct-horse"))), "setup: set passphrase");
  CHECK(!isError(meshesCmd_setpassphrase(String("bench"), String(""))), "setup: clear passphrase");
}

static void testOnlyMeshCanBeRemovedAndDeleted() {
  // M1: a new device's single mesh can be disabled; nothing else could be the default.
  resetAll();
  const char* r = meshesCmd_remove(String("primary"));
  CHECK(!isError(r), "M1 the only mesh could not be disabled: %s", r);
  CHECK(!gSettings.meshes[0].enabled, "M1 mesh still enabled after remove");
  CHECK(enabledDefaults() == 0 && !gSettings.meshes[0].isDefault, "M1 a disabled mesh kept the default flag");
  CHECK(gSettings.meshes[0].label == String("primary"), "M1 remove must keep the label for 'enable'");

  // M2: the leftover the bench boards had (a mesh in slot 0 whose passphrase was
  // cleared, so disabled and still flagged default) can be deleted. With ESP-NOW
  // stopped that leaves no mesh at all, as on a new device, and 'add' takes slot 0.
  makeBenchLeftover();
  r = meshesCmd_delete(String("bench"));
  CHECK(!isError(r), "M2 the leftover mesh could not be deleted: %s", r);
  CHECK(slotOf("bench") < 0, "M2 label 'bench' is still stored");
  CHECK(slotIsEmpty(0), "M2 slot 0 is not empty after deleting the only mesh of a stopped device");
  CHECK(gPersists > 0, "M2 nothing was persisted");
  CHECK(!isError(meshesCmd_add(String("home"))) && slotOf("home") == 0 && gSettings.meshes[0].isDefault,
        "M2 'add' after the delete did not behave as on a new device");

  // M2b: a running ESP-NOW always has a mesh in slot 0, so it gets the
  // unconfigured 'primary' mesh back at once.
  makeBenchLeftover();
  gState.initialized = true;
  r = meshesCmd_delete(String("bench"));
  CHECK(!isError(r) && has(r, "'primary'"), "M2b delete under a running ESP-NOW: %s", r);
  CHECK(gSettings.meshes[0].label == String("primary") && gSettings.meshes[0].enabled &&
            gSettings.meshes[0].isDefault && gSettings.meshes[0].passphrase.length() == 0 &&
            !gSettings.meshes[0].passphraseStretchedKeyValid,
        "M2b slot 0 is not back to the unconfigured 'primary' mesh");
  CHECK(gSettings.meshes[0].fingerprint == meshFingerprintForLabel(String("primary")), "M2b fingerprint not recomputed");

  // M3: with a second enabled mesh the default still cannot go until another is chosen.
  resetAll();
  CHECK(!isError(meshesCmd_add(String("work"))), "M3 setup add");
  r = meshesCmd_remove(String("primary"));
  CHECK(isError(r) && has(r, "setdefault"), "M3 default removed while another mesh could take over: %s", r);
  r = meshesCmd_delete(String("primary"));
  CHECK(isError(r) && has(r, "setdefault"), "M3 default deleted while another mesh could take over: %s", r);
  CHECK(gSettings.meshes[0].enabled && gSettings.meshes[0].isDefault, "M3 refused command changed the mesh");
}

static void testDeleteFreesTheSlot() {
  // D1: a removed mesh keeps its slot; delete frees it and 'add' uses it again.
  resetAll();
  CHECK(!isError(meshesCmd_add(String("a"))) && !isError(meshesCmd_add(String("b"))) &&
            !isError(meshesCmd_add(String("c"))),
        "D1 setup: three adds");
  CHECK(isError(meshesCmd_add(String("d"))), "D1 setup: a fifth mesh fits in four slots");
  CHECK(!isError(meshesCmd_setpassphrase(String("b"), String("correct-horse"))), "D1 setup: passphrase");
  const int slotB = slotOf("b");
  gSettings.bondModeEnabledMesh[slotB] = true;
  gSettings.bondRoleMesh[slotB] = 1;
  gSettings.bondPeerMacMesh[slotB] = "02:00:00:00:00:09";
  CHECK(!isError(meshesCmd_remove(String("b"))), "D1 remove b");
  CHECK(isError(meshesCmd_add(String("d"))), "D1 a disabled mesh's slot was reused without delete");
  gEvents.clear();
  const char* r = meshesCmd_delete(String("b"));
  CHECK(!isError(r), "D1 delete b: %s", r);
  CHECK(slotIsEmpty((uint8_t)slotB), "D1 slot not empty after delete (label, passphrase, key or flags left)");
  CHECK(!gDerivedValid[slotB] && gInvalidations[slotB] > 0, "D1 derived keys not dropped");
  CHECK(!gSettings.bondModeEnabledMesh[slotB] && gSettings.bondRoleMesh[slotB] == 0 &&
            gSettings.bondPeerMacMesh[slotB].length() == 0,
        "D1 the slot's bond settings stayed for the next mesh");
  CHECK(gEvents.size() == 1 && gEvents[0].kind == SYSEVT_MESH_PASSPHRASE_CHANGED && gEvents[0].detail == "cleared",
        "D1 no 'passphrase cleared' event for a deleted keyed mesh");
  CHECK(!isError(meshesCmd_add(String("d"))) && slotOf("d") == slotB, "D1 'add' did not reuse the freed slot");
  CHECK(gSettings.meshes[slotB].passphrase.length() == 0 && !gSettings.meshes[slotB].passphraseStretchedKeyValid,
        "D1 the new mesh inherited the old passphrase or key");

  // D2: refused while paired devices still name the slot; nothing changes.
  resetAll();
  CHECK(!isError(meshesCmd_add(String("work"))), "D2 setup");
  const int slotW = slotOf("work");
  gPeers[3] = PeerIdentity{{2, 0, 0, 0, 0, 7}, (uint8_t)slotW, true};
  gPeers[5] = PeerIdentity{{2, 0, 0, 0, 0, 8}, (uint8_t)slotW, true};
  gPeers[6] = PeerIdentity{{2, 0, 0, 0, 0, 9}, 0, true};  // another mesh: not counted
  const int persistsBefore = gPersists;
  r = meshesCmd_delete(String("work"));
  CHECK(isError(r) && has(r, "2 paired devices") && has(r, "espnowunpair"), "D2 delete with paired devices: %s", r);
  CHECK(slotOf("work") == slotW && gSettings.meshes[slotW].enabled && gPersists == persistsBefore,
        "D2 a refused delete changed or persisted something");
  gPeers[5].valid = false;
  r = meshesCmd_delete(String("work"));
  CHECK(isError(r) && has(r, "1 paired device still belongs"), "D2 singular wording: %s", r);
  gPeers[3].valid = false;
  CHECK(!isError(meshesCmd_delete(String("work"))), "D2 delete after unpairing");

  // D3: deleting slot 0 beside a mesh in use leaves the placeholder switched off
  // and does not take the default or the live passphrase from the other mesh.
  resetAll();
  CHECK(!isError(meshesCmd_add(String("work"))), "D3 setup add");
  CHECK(!isError(meshesCmd_setpassphrase(String("work"), String("work-passphrase"))), "D3 setup passphrase");
  CHECK(!isError(meshesCmd_setdefault(String("work"))), "D3 setup default");
  gState.initialized = true;
  gState.passphrase = "work-passphrase";  // what a start loads: the enabled default mesh's
  deriveKeyFromPassphrase(gState.passphrase, gState.derivedKey);
  r = meshesCmd_delete(String("primary"));
  CHECK(!isError(r), "D3 delete slot 0: %s", r);
  CHECK(gSettings.meshes[0].label == String("primary") && !gSettings.meshes[0].enabled && !gSettings.meshes[0].isDefault,
        "D3 the placeholder joined in beside the mesh in use");
  CHECK(enabledDefaults() == 1 && gSettings.meshes[slotOf("work")].isDefault, "D3 the default moved");
  CHECK(gState.passphrase == String("work-passphrase") && gState.derivedKey[0] == 0xA5,
        "D3 the running instance lost the default mesh's passphrase");

  // D3b: the same with ESP-NOW stopped: the placeholder is written now, so the
  // next start cannot bootstrap an enabled default mesh beside the one in use.
  resetAll();
  CHECK(!isError(meshesCmd_add(String("work"))) && !isError(meshesCmd_setdefault(String("work"))), "D3b setup");
  CHECK(!isError(meshesCmd_delete(String("primary"))), "D3b delete");
  initPrimaryMeshFromLegacySettings();  // the next ESP-NOW start
  CHECK(gSettings.meshes[0].label == String("primary") && !gSettings.meshes[0].enabled && enabledDefaults() == 1,
        "D3b the next start put an enabled default mesh beside the one in use");

  // D4: a running instance that started with the deleted mesh's passphrase drops it.
  resetAll();
  gSettings.meshes[0].label = "bench";
  CHECK(!isError(meshesCmd_setpassphrase(String("bench"), String("correct-horse"))), "D4 setup");
  gState.initialized = true;
  CHECK(gState.passphrase == String("correct-horse"), "D4 setup: live passphrase");
  CHECK(!isError(meshesCmd_delete(String("bench"))), "D4 delete");
  CHECK(gState.passphrase.length() == 0 && gState.derivedKey[0] == 0 && !gState.encryptionEnabled,
        "D4 the deleted mesh's passphrase is still in use");

  // D5: a failed save is reported.
  makeBenchLeftover();
  gPersistOk = false;
  CHECK(isError(meshesCmd_delete(String("bench"))), "D5 a failed save was reported as success");
  CHECK(isError(meshesCmd_delete(String("nosuch"))), "D5 unknown label");
}

static void testOneDefault() {
  // E1: re-enabling a mesh that was disabled while it was the default must not
  // leave two enabled defaults once another mesh took the flag.
  makeBenchLeftover();  // slot 0 disabled, flag left behind by the passphrase clear
  CHECK(!isError(meshesCmd_add(String("work"))), "E1 add");
  CHECK(gSettings.meshes[slotOf("work")].isDefault, "E1 the first enabled mesh did not become the default");
  CHECK(!isError(meshesCmd_enable(String("bench"))), "E1 enable");
  CHECK(enabledDefaults() == 1 && gSettings.meshes[slotOf("work")].isDefault, "E1 two enabled defaults, or the default moved");

  // E2: enabling the only mesh makes it the default again.
  resetAll();
  CHECK(!isError(meshesCmd_remove(String("primary"))), "E2 remove");
  CHECK(!isError(meshesCmd_enable(String("primary"))), "E2 enable");
  CHECK(enabledDefaults() == 1 && gSettings.meshes[0].isDefault, "E2 no default after re-enabling the only mesh");

  // E3: removing a mesh twice explains the next step instead of repeating itself.
  resetAll();
  CHECK(!isError(meshesCmd_remove(String("primary"))), "E3 remove");
  const char* r = meshesCmd_remove(String("primary"));
  CHECK(!isError(r) && has(r, "already disabled") && has(r, "espnowmeshes delete primary"), "E3 second remove: %s", r);
}

static void testMessagesNameTheMesh() {
  // P1: clearing the passphrase of a renamed slot 0 names that mesh and says what happened to it.
  resetAll();
  gSettings.meshes[0].label = "bench";
  CHECK(!isError(meshesCmd_setpassphrase(String("bench"), String("correct-horse"))), "P1 setup");
  const char* r = meshesCmd_setpassphrase(String("bench"), String(""));
  CHECK(!has(r, "primary"), "P1 the reply calls mesh 'bench' primary: %s", r);
  CHECK(has(r, "'bench'") && !gSettings.meshes[0].enabled && has(r, "disabled") && has(r, "espnowmeshes enable bench"),
        "P1 the reply does not say the mesh is now disabled: %s", r);
  // A mesh in another slot stays enabled, and the reply does not claim otherwise.
  resetAll();
  CHECK(!isError(meshesCmd_add(String("work"))), "P1 add");
  CHECK(!isError(meshesCmd_setpassphrase(String("work"), String("work-passphrase"))), "P1 set");
  r = meshesCmd_setpassphrase(String("work"), String(""));
  CHECK(gSettings.meshes[slotOf("work")].enabled && !has(r, "disabled") && !has(r, "primary"), "P1 other slot: %s", r);

  // P2: the start-up warning names the mesh a passphrase should go on.
  char w[224];
  resetAll();
  gSettings.meshes[0].label = "bench";
  formatNoPassphraseWarning(w, sizeof(w));
  CHECK(has(w, "espnowsetpassphrase bench <pw>") && !has(w, "primary"), "P2 renamed slot 0: %s", w);
  resetAll();
  formatNoPassphraseWarning(w, sizeof(w));
  CHECK(has(w, "espnowsetpassphrase primary <pw>"), "P2 unrenamed slot 0: %s", w);
  resetAll();
  CHECK(!isError(meshesCmd_add(String("work"))) && !isError(meshesCmd_setdefault(String("work"))), "P2 setup");
  formatNoPassphraseWarning(w, sizeof(w));
  CHECK(has(w, "espnowsetpassphrase work <pw>"), "P2 the default mesh is not the one named: %s", w);
  resetAll();
  CHECK(!isError(meshesCmd_remove(String("primary"))), "P2 setup remove");
  formatNoPassphraseWarning(w, sizeof(w));
  CHECK(!has(w, "espnowsetpassphrase primary") && has(w, "espnowmeshes"), "P2 no enabled mesh: %s", w);
#if HW1_HAVE_PASSPHRASE_HINT
  // P3: the passphrase a start loads comes from slot 0 unless an enabled default
  // mesh in another slot has one.
  resetAll();
  CHECK(espnowStartPassphraseSlot() == 0, "P3 new device");
  CHECK(!isError(meshesCmd_add(String("work"))) && !isError(meshesCmd_setdefault(String("work"))), "P3 setup");
  CHECK(espnowStartPassphraseSlot() == 0, "P3 a default mesh without a passphrase was chosen");
  CHECK(!isError(meshesCmd_setpassphrase(String("work"), String("work-passphrase"))), "P3 set");
  CHECK(espnowStartPassphraseSlot() == slotOf("work"), "P3 the keyed default mesh was not chosen");
#endif
}

static void testNameCanBeCleared() {
  // N1: 'clear' removes the name while ESP-NOW is stopped.
  resetAll();
  CHECK(has(cmd_espnow_setname(String("benchp4")), "benchp4"), "N1 setup");
  CHECK(gSettings.espnowFirstTimeSetup, "N1 setup flag");
  gState.deviceName = "benchp4";
  addEspNowDevice(kSelfMac, String("benchp4"), false, nullptr);
  gEvents.clear();
  gMetadataChanged = false;
  const char* r = cmd_espnow_setname(String("clear"));
  CHECK(!isError(r) && has(r, "cleared"), "N1 clear refused: %s", r);
  CHECK(gSettings.espnowDeviceName.length() == 0, "N1 the name is still '%s'", gSettings.espnowDeviceName.c_str());
  CHECK(!gSettings.espnowFirstTimeSetup, "N1 the setup-done flag stayed set with no name");
  CHECK(gState.deviceName.length() == 0 && gState.devices[0].name.length() == 0, "N1 the stopped instance kept the name");
  CHECK(gMetadataChanged, "N1 metadata not marked changed");
  CHECK(gEvents.size() == 1 && gEvents[0].kind == SYSEVT_SETTING_CHANGED && gEvents[0].subject == "espnowDeviceName",
        "N1 no setting-changed event");
  CHECK(has(cmd_espnow_setname(String("")), "(not set)"), "N1 the query still shows a name");

  // N2: refused while ESP-NOW runs or is still shutting down; the name stays.
  resetAll();
  CHECK(has(cmd_espnow_setname(String("benchp4")), "benchp4"), "N2 setup");
  gState.initialized = true;
  r = cmd_espnow_setname(String("clear"));
  CHECK(isError(r) && has(r, "closeespnow"), "N2 cleared under a running ESP-NOW: %s", r);
  CHECK(gSettings.espnowDeviceName == String("benchp4"), "N2 a refused clear changed the name");
  gState.initialized = false;
  gShutdownPending = true;
  CHECK(isError(cmd_espnow_setname(String("clear"))), "N2 cleared during a pending shutdown");
  gShutdownPending = false;

  // N3: clearing an unset name is a plain answer, with no write and no event.
  resetAll();
  const int persistsBefore = gPersists;
  r = cmd_espnow_setname(String("clear"));
  CHECK(!isError(r) && has(r, "(not set)") && gPersists == persistsBefore && gEvents.empty(), "N3 unset name: %s", r);

  // N5: with the radio off the own address may not be readable; the clear still happens.
  resetAll();
  CHECK(has(cmd_espnow_setname(String("benchp4")), "benchp4"), "N5 setup");
  gSelfMacReadable = false;
  r = cmd_espnow_setname(String("clear"));
  CHECK(!isError(r) && gSettings.espnowDeviceName.length() == 0, "N5 clear with no readable address: %s", r);

  // N4: ordinary names still work and validate-only still answers without acting.
  resetAll();
  CHECK(isError(cmd_espnow_setname(String("has.dot"))), "N4 a name with '.' was accepted");
  gCliValidateOnly = true;
  CHECK(has(cmd_espnow_setname(String("clear")), "VALID"), "N4 validate-only");
  gCliValidateOnly = false;
}

int main() {
  testOnlyMeshCanBeRemovedAndDeleted();
  testDeleteFreesTheSlot();
  testOneDefault();
  testMessagesNameTheMesh();
  testNameCanBeCleared();
  if (gFailures) {
    std::printf("%d check(s) failed\n", gFailures);
    return 1;
  }
  std::printf("all mesh slot and device name checks passed\n");
  return 0;
}
