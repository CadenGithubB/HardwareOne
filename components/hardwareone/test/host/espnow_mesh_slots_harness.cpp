// Host harness for test_espnow_mesh_slots.py.
//
// The runner pastes the production Settings::MeshIdentity record into the
// Settings double and the production mesh-slot and device-name code at the
// production marker: meshFingerprintForLabel, recomputeAllMeshFingerprints, the
// meshes settings writer and reader, setEspNowPassphrase,
// initPrimaryMeshFromLegacySettings, the start-passphrase and hint helpers, the
// espnowmeshes subcommands (add, remove, delete, enable, setdefault,
// setpassphrase) with the delete guard's counters, and cmd_espnow_setname.
// ArduinoJson is the vendored library. Everything else here is a small double:
// settings writes, the key cache, stored identities, the device registry and
// its file, events and the ESP-NOW state. No radio or flash is touched.
#include <Arduino.h>
#include <ArduinoJson.h>

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
#define PSRAM_JSON_DOC(name) JsonDocument name

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

// Secrets are stored behind a marker, so a check can tell a field that went
// through putSecret from one written in the clear.
static void putSecret(JsonObject obj, const char* key, const String& plaintext) {
  obj[key] = std::string("enc:") + plaintext.c_str();
}
static String getSecret(JsonObjectConst obj, const char* key) {
  const char* v = obj[key] | "";
  return String(std::strncmp(v, "enc:", 4) == 0 ? v + 4 : "");
}

// ---- ESP-NOW state double. gEspNow is null until the first start of a boot.
struct EspNowDevice {
  uint8_t mac[6];
  String name;
  uint8_t meshId = 0;
};
struct EspNowState {
  bool initialized = false;
  String passphrase;
  uint8_t derivedKey[16] = {};
  bool encryptionEnabled = false;
  String deviceName;
  EspNowDevice devices[6];
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
// isSelfMac answers only while the instance runs; the last-known address also after a stop.
static bool gLastSelfKnown = false;
static bool isSelfMac(const uint8_t* mac) { return gState.initialized && std::memcmp(mac, kSelfMac, 6) == 0; }
static bool isLastKnownSelfMac(const uint8_t* mac) { return gLastSelfKnown && std::memcmp(mac, kSelfMac, 6) == 0; }
static void addEspNowDevice(const uint8_t* mac, const String& name, bool, const uint8_t*) {
  if (gState.deviceCount >= 6) return;
  std::memcpy(gState.devices[gState.deviceCount].mac, mac, 6);
  gState.devices[gState.deviceCount].name = name;
  gState.devices[gState.deviceCount].meshId = 0;
  ++gState.deviceCount;
}

// Legacy key: a marker byte stands for "derived from a non-empty passphrase".
void deriveKeyFromPassphrase(const String& passphrase, uint8_t* key) {
  if (!gEspNow) return;
  std::memset(key, passphrase.length() ? 0xA5 : 0x00, 16);
  gEspNow->encryptionEnabled = passphrase.length() > 0;
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
static bool gIdentitiesLoaded = true;
static bool peerIdentitiesLoaded() { return gIdentitiesLoaded; }
static uint8_t peerIdentitySlotCount() { return 16; }
static const PeerIdentity* peerIdentityAt(uint8_t i) {
  if (i >= 16) return nullptr;
  return gPeers[i].valid ? &gPeers[i] : nullptr;
}

// ---- Device registry doubles: the load status and the saved file.
namespace ConfigLoad {
enum class Status : uint8_t { NotLoaded = 0, Ok, AbsentOk, NotMounted, OpenFailed, EmptyFile, Truncated, Corrupt, NoMemory, Partial };
inline bool savable(Status s) { return s == Status::Ok || s == Status::AbsentOk; }
}  // namespace ConfigLoad
static ConfigLoad::Status sEspNowDevicesLoad = ConfigLoad::Status::NotLoaded;
enum class RegistryFile { Absent, Unreadable, Present };
static RegistryFile gRegistryFile = RegistryFile::Absent;
static std::string gRegistryFileText;
static int gRegistryFileReads = 0;
static int meshRegistryCountInJson(const String& content, uint8_t slot);
// The production reader opens ESPNOW_DEVICES_FILE under the filesystem lock and
// hands its text to meshRegistryCountInJson; this double does the same from memory.
static int meshRegistryCountInFile(uint8_t slot) {
  ++gRegistryFileReads;
  if (gRegistryFile == RegistryFile::Absent) return 0;
  if (gRegistryFile == RegistryFile::Unreadable) return -1;
  return meshRegistryCountInJson(String(gRegistryFileText.c_str()), slot);
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
#if !HW1_HAVE_REGISTRY_GUARD
// Trees whose delete does not read the device registry have no parser for it.
static int meshRegistryCountInJson(const String&, uint8_t) { return -1; }
#endif
#if !HW1_HAVE_PAIRING_SLOT
// Trees before the pairing helper put a pairing with no mesh argument into slot 0.
static uint8_t meshSlotForPairing(const String& arg) {
  const uint8_t slot = parseMeshArgOrDefault(arg);
  return slot == 0xFE ? 0 : slot;
}
#endif
#if !HW1_HAVE_PASSPHRASE_HINT
static void formatNoPassphraseWarning(char* out, size_t outLen) {
  std::snprintf(out, outLen, "%s", "Run 'espnowsetpassphrase primary <pw>'.");
}
#endif

// ---- Test helpers.
static bool has(const char* reply, const char* needle) { return reply && std::strstr(reply, needle) != nullptr; }
static bool isError(const char* reply) { return reply && std::strncmp(reply, "Error", 5) == 0; }
// A device that has not started ESP-NOW since boot: no instance, no registry in RAM, no mesh.
static void resetNeverStarted() {
  gSettings = Settings();
  gState = EspNowState();
  gEspNow = nullptr;
  gShutdownPending = false;
  gSelfMacReadable = true;
  gLastSelfKnown = false;
  gPersists = 0;
  gPersistOk = true;
  gEvents.clear();
  gMetadataChanged = false;
  std::memset(gPeers, 0, sizeof(gPeers));
  gIdentitiesLoaded = true;
  sEspNowDevicesLoad = ConfigLoad::Status::NotLoaded;
  gRegistryFile = RegistryFile::Absent;
  gRegistryFileText.clear();
  gRegistryFileReads = 0;
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    gDerivedValid[i] = false;
    gInvalidations[i] = 0;
  }
}
// What a start does to the tables these commands read: the instance exists, the
// registry is in RAM with this device's own entry, slot 0 is bootstrapped.
static void startEspNow() {
  gEspNow = &gState;
  if (sEspNowDevicesLoad == ConfigLoad::Status::NotLoaded) sEspNowDevicesLoad = ConfigLoad::Status::AbsentOk;
  initPrimaryMeshFromLegacySettings();
  gState.initialized = true;
  gLastSelfKnown = true;
  bool haveSelf = false;
  for (int i = 0; i < gState.deviceCount; i++) haveSelf = haveSelf || std::memcmp(gState.devices[i].mac, kSelfMac, 6) == 0;
  if (!haveSelf) addEspNowDevice(kSelfMac, String("self"), false, nullptr);
}
static void stopEspNow() { gState.initialized = false; }
// The usual starting point: ESP-NOW was started once and stopped again.
static void resetAll() {
  resetNeverStarted();
  startEspNow();
  stopEspNow();
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
static int labelCount(const char* label) {
  int n = 0;
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    if (gSettings.meshes[i].label == String(label)) ++n;
  }
  return n;
}
static bool slotIsEmpty(uint8_t i) {
  const Settings::MeshIdentity& m = gSettings.meshes[i];
  for (uint8_t b : m.passphraseStretchedKey) {
    if (b) return false;
  }
  return m.label.length() == 0 && m.passphrase.length() == 0 && !m.enabled && !m.isDefault &&
         m.fingerprint == 0 && !m.passphraseStretchedKeyValid;
}
static void addRegistryDevice(uint8_t lastByte, uint8_t meshId) {
  const uint8_t mac[6] = {0x02, 0, 0, 0, 0, lastByte};
  addEspNowDevice(mac, String("peer"), false, nullptr);
  gState.devices[gState.deviceCount - 1].meshId = meshId;
}
// The device as the bench boards were left: 'add bench' before any start put the
// mesh in slot 0, it was keyed, then its passphrase was cleared (which disables
// slot 0 and leaves its default flag). ESP-NOW is stopped.
static void makeBenchLeftover() {
  resetNeverStarted();
  CHECK(!isError(meshesCmd_add(String("bench"))) && slotOf("bench") == 0, "setup: add bench");
  startEspNow();
  CHECK(!isError(meshesCmd_setpassphrase(String("bench"), String("correct-horse"))), "setup: set passphrase");
  CHECK(!isError(meshesCmd_setpassphrase(String("bench"), String(""))), "setup: clear passphrase");
  stopEspNow();
  CHECK(!gSettings.meshes[0].enabled && gSettings.meshes[0].isDefault, "setup: the leftover is disabled and still flagged default");
}

static void testOnlyMeshCanBeRemovedAndDeleted() {
  // M1: a device's single mesh can be disabled; nothing else could be the default.
  resetAll();
  const char* r = meshesCmd_remove(String("primary"));
  CHECK(!isError(r), "M1 the only mesh could not be disabled: %s", r);
  CHECK(!gSettings.meshes[0].enabled, "M1 mesh still enabled after remove");
  CHECK(enabledDefaults() == 0 && !gSettings.meshes[0].isDefault, "M1 a disabled mesh kept the default flag");
  CHECK(gSettings.meshes[0].label == String("primary"), "M1 remove must keep the label for 'enable'");

  // M2: the bench leftover can be deleted. That leaves no mesh at all, as on a
  // new device: 'add' takes slot 0 again, or the next start sets up "primary".
  makeBenchLeftover();
  int persistsBefore = gPersists;
  r = meshesCmd_delete(String("bench"));
  CHECK(!isError(r) && has(r, "Slot 0 is free"), "M2 the leftover mesh could not be deleted: %s", r);
  CHECK(slotOf("bench") < 0 && slotIsEmpty(0), "M2 slot 0 is not empty after deleting the only mesh");
  CHECK(gPersists > persistsBefore, "M2 the delete was not persisted");
  startEspNow();
  CHECK(gSettings.meshes[0].label == String("primary") && gSettings.meshes[0].enabled && gSettings.meshes[0].isDefault &&
            gSettings.meshes[0].passphrase.length() == 0 &&
            gSettings.meshes[0].fingerprint == meshFingerprintForLabel(String("primary")),
        "M2 the next start did not set up the unconfigured 'primary' mesh");
  makeBenchLeftover();
  CHECK(!isError(meshesCmd_delete(String("bench"))), "M2 delete");
  CHECK(!isError(meshesCmd_add(String("home"))) && slotOf("home") == 0 && gSettings.meshes[0].isDefault,
        "M2 'add' after the delete did not behave as on a new device");

  // M2b: the same under a running ESP-NOW. The slot is emptied there too; the
  // instance runs without a mesh until 'add' or the next start.
  makeBenchLeftover();
  startEspNow();
  r = meshesCmd_delete(String("bench"));
  CHECK(!isError(r) && slotIsEmpty(0) && labelCount("primary") == 0, "M2b delete under a running ESP-NOW: %s", r);

  // M2c: a device that has not started ESP-NOW since boot (no instance at all).
  resetNeverStarted();
  CHECK(!isError(meshesCmd_add(String("bench"))), "M2c setup");
  r = meshesCmd_delete(String("bench"));
  CHECK(!isError(r) && slotIsEmpty(0), "M2c delete with no instance: %s", r);
  CHECK(gRegistryFileReads == 1, "M2c the saved registry was not consulted (%d reads)", gRegistryFileReads);

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
  const char* r = meshesCmd_add(String("d"));
  CHECK(isError(r) && has(r, "espnowmeshes delete"), "D1 a fifth mesh in four slots, or the reply does not name delete: %s", r);
  CHECK(!isError(meshesCmd_setpassphrase(String("b"), String("correct-horse"))), "D1 setup: passphrase");
  const int slotB = slotOf("b");
  gSettings.bondModeEnabledMesh[slotB] = true;
  gSettings.bondRoleMesh[slotB] = 1;
  gSettings.bondPeerMacMesh[slotB] = "02:00:00:00:00:09";
  CHECK(!isError(meshesCmd_remove(String("b"))), "D1 remove b");
  CHECK(isError(meshesCmd_add(String("d"))), "D1 a disabled mesh's slot was reused without delete");
  gEvents.clear();
  r = meshesCmd_delete(String("b"));
  CHECK(!isError(r), "D1 delete b: %s", r);
  CHECK(slotIsEmpty((uint8_t)slotB), "D1 slot not empty after delete (label, passphrase, key or flags left)");
  CHECK(!gDerivedValid[slotB] && gInvalidations[slotB] > 0, "D1 derived keys not dropped");
  CHECK(!gSettings.bondModeEnabledMesh[slotB] && gSettings.bondRoleMesh[slotB] == 0 &&
            gSettings.bondPeerMacMesh[slotB].length() == 0,
        "D1 the slot's bond settings stayed for the next mesh");
  CHECK(gEvents.size() == 1 && gEvents[0].kind == SYSEVT_MESH_PASSPHRASE_CHANGED && gEvents[0].detail == "cleared",
        "D1 no 'passphrase cleared' event for a deleted keyed mesh");
  CHECK(slotOf("a") == 1 && slotOf("c") == 3, "D1 the other meshes moved");
  CHECK(!isError(meshesCmd_add(String("d"))) && slotOf("d") == slotB, "D1 'add' did not reuse the freed slot");
  CHECK(gSettings.meshes[slotB].passphrase.length() == 0 && !gSettings.meshes[slotB].passphraseStretchedKeyValid,
        "D1 the new mesh inherited the old passphrase or key");

  // D2: refused while stored identities still name the slot; nothing changes.
  resetAll();
  CHECK(!isError(meshesCmd_add(String("work"))), "D2 setup");
  const int slotW = slotOf("work");
  gPeers[3] = PeerIdentity{{2, 0, 0, 0, 0, 7}, (uint8_t)slotW, true};
  gPeers[5] = PeerIdentity{{2, 0, 0, 0, 0, 8}, (uint8_t)slotW, true};
  gPeers[6] = PeerIdentity{{2, 0, 0, 0, 0, 9}, 0, true};  // another mesh: not counted
  int persistsBefore = gPersists;
  r = meshesCmd_delete(String("work"));
  CHECK(isError(r) && has(r, "2 with stored keys") && has(r, "espnowunpair") && has(r, "espnowforget"),
        "D2 delete with stored identities: %s", r);
  CHECK(slotOf("work") == slotW && gSettings.meshes[slotW].enabled && gPersists == persistsBefore,
        "D2 a refused delete changed or persisted something");
  gPeers[5].valid = false;
  gPeers[3].valid = false;
  CHECK(!isError(meshesCmd_delete(String("work"))), "D2 delete after unpairing");

  // D2b: a registry device with no stored identity (plain 'espnowpair', a key
  // exchange that never finished, or 'espnowforget') also holds the slot.
  resetAll();
  CHECK(!isError(meshesCmd_add(String("lab"))), "D2b setup");
  addRegistryDevice(0x21, (uint8_t)slotOf("lab"));
  r = meshesCmd_delete(String("lab"));
  CHECK(isError(r) && has(r, "1 in the device list") && has(r, "0 with stored keys"), "D2b registry-only device: %s", r);
  CHECK(slotOf("lab") == 1, "D2b a refused delete removed the mesh");
  gState.deviceCount--;  // unpaired
  CHECK(!isError(meshesCmd_delete(String("lab"))), "D2b delete after unpairing");

  // D2c: this device's own registry entry (slot 0) does not hold slot 0, running or stopped.
  resetAll();  // started once: the own entry exists
  CHECK(gState.deviceCount == 1 && gState.devices[0].meshId == 0, "D2c setup: own entry");
  CHECK(!isError(meshesCmd_delete(String("primary"))), "D2c own entry blocked the delete while stopped");
  resetAll();
  startEspNow();
  CHECK(!isError(meshesCmd_delete(String("primary"))), "D2c own entry blocked the delete while running");
  // A real peer in slot 0 still does.
  resetAll();
  addRegistryDevice(0x22, 0);
  CHECK(isError(meshesCmd_delete(String("primary"))), "D2c a peer in slot 0 did not block the delete");

  // D2d: before the first start of a boot the registry is only in its file.
  resetNeverStarted();
  CHECK(!isError(meshesCmd_add(String("bench"))) && !isError(meshesCmd_add(String("lab"))), "D2d setup");
  gRegistryFile = RegistryFile::Present;
  gRegistryFileText = "{\"devices\":[{\"mac\":\"x\",\"name\":\"s1\",\"meshId\":1},{\"mac\":\"y\",\"name\":\"s2\"}]}";
  r = meshesCmd_delete(String("lab"));
  CHECK(isError(r) && has(r, "1 in the device list"), "D2d a saved device in slot 1 did not block the delete: %s", r);
  CHECK(!isError(meshesCmd_setdefault(String("lab"))), "D2d setup: default to lab");
  r = meshesCmd_delete(String("bench"));
  CHECK(isError(r) && has(r, "1 in the device list"), "D2d a saved device without meshId is in slot 0: %s", r);
  CHECK(!isError(meshesCmd_setdefault(String("bench"))), "D2d setup: default back to bench");
  gRegistryFileText = "{\"devices\":[]}";
  CHECK(!isError(meshesCmd_delete(String("lab"))), "D2d empty saved registry");
  CHECK(!isError(meshesCmd_add(String("lab"))), "D2d setup again");
  gRegistryFile = RegistryFile::Unreadable;
  r = meshesCmd_delete(String("lab"));
  CHECK(isError(r) && has(r, "could not be read") && slotOf("lab") == 1, "D2d unreadable registry: %s", r);
  gRegistryFile = RegistryFile::Present;
  gRegistryFileText = "{\"devices\":[{\"meshId\":1}";  // cut short
  r = meshesCmd_delete(String("lab"));
  CHECK(isError(r) && slotOf("lab") == 1, "D2d a damaged registry file was read as 'no devices': %s", r);
  CHECK(meshRegistryCountInJson(String("[1,2]"), 0) == -1 && meshRegistryCountInJson(String("{}"), 0) == 0,
        "D2d registry text that is not an object must not count as empty");
  // A registry that failed to load into RAM is not trusted either: the file is asked...
  resetAll();
  CHECK(!isError(meshesCmd_add(String("lab"))), "D2d setup");
  sEspNowDevicesLoad = ConfigLoad::Status::OpenFailed;
  gRegistryFile = RegistryFile::Unreadable;
  CHECK(isError(meshesCmd_delete(String("lab"))), "D2d a registry that never loaded was treated as empty");
  // ...and a device paired into the table since then counts although the file does not have it.
  gRegistryFile = RegistryFile::Absent;
  addRegistryDevice(0x23, 1);
  r = meshesCmd_delete(String("lab"));
  CHECK(isError(r) && has(r, "1 in the device list"), "D2d a device paired after a failed registry load was missed: %s", r);
  gState.deviceCount--;
  gRegistryFile = RegistryFile::Present;
  gRegistryFileText = "{\"devices\":[{\"meshId\":1},{\"meshId\":1}]}";
  addRegistryDevice(0x24, 1);
  r = meshesCmd_delete(String("lab"));
  CHECK(isError(r) && has(r, "3 in the device list"), "D2d table and file are not both counted after a failed load: %s", r);
  // A partial load is rewritten from the table, and a clean one equals it: the table alone counts.
  for (ConfigLoad::Status st : {ConfigLoad::Status::Partial, ConfigLoad::Status::Ok}) {
    resetAll();
    CHECK(!isError(meshesCmd_add(String("lab"))), "D2d setup");
    sEspNowDevicesLoad = st;
    gRegistryFile = RegistryFile::Present;
    gRegistryFileText = "{\"devices\":[{\"meshId\":1}]}";  // what the file still says
    addRegistryDevice(0x25, 1);
    r = meshesCmd_delete(String("lab"));
    CHECK(isError(r) && has(r, "1 in the device list") && gRegistryFileReads == 0,
          "D2d status %d: the table in RAM was not the source (%d file reads): %s", (int)st, gRegistryFileReads, r);
    gState.deviceCount--;
    r = meshesCmd_delete(String("lab"));
    CHECK(!isError(r) && gRegistryFileReads == 0, "D2d status %d: the file was asked although the table holds it: %s", (int)st, r);
  }

  // D2e: stored identities that were not read at this start say nothing.
  resetAll();
  CHECK(!isError(meshesCmd_add(String("lab"))), "D2e setup");
  gIdentitiesLoaded = false;
  r = meshesCmd_delete(String("lab"));
  CHECK(isError(r) && has(r, "Restart") && slotOf("lab") == 1, "D2e delete without the stored identities: %s", r);

  // D3: deleting a keyed slot 0 beside the keyed default mesh. The slot is left
  // empty (no placeholder joins in), the default stays, and the running
  // instance now holds the default mesh's passphrase, as a start would load.
  resetAll();
  startEspNow();
  CHECK(!isError(meshesCmd_setpassphrase(String("primary"), String("prim-passphrase"))), "D3 setup slot 0 passphrase");
  CHECK(gState.passphrase == String("prim-passphrase"), "D3 setup: live passphrase is slot 0's");
  CHECK(!isError(meshesCmd_add(String("work"))), "D3 setup add");
  CHECK(!isError(meshesCmd_setpassphrase(String("work"), String("work-passphrase"))), "D3 setup passphrase");
  CHECK(!isError(meshesCmd_setdefault(String("work"))), "D3 setup default");
  r = meshesCmd_delete(String("primary"));
  CHECK(!isError(r), "D3 delete slot 0: %s", r);
  CHECK(slotIsEmpty(0) && enabledDefaults() == 1 && gSettings.meshes[slotOf("work")].isDefault && slotOf("work") == 1,
        "D3 slot 0 not empty, or the default mesh moved");
  CHECK(gState.passphrase == String("work-passphrase") && gState.derivedKey[0] == 0xA5 && gState.encryptionEnabled,
        "D3 the running instance does not hold the default mesh's passphrase");
  stopEspNow();
  startEspNow();
  CHECK(slotIsEmpty(0) && enabledDefaults() == 1, "D3 the next start put a mesh into slot 0 beside the one in use");

  // D3b: slot 0 deleted while the only other mesh is disabled, that mesh enabled
  // afterwards: the next start must not create an enabled default "primary".
  resetAll();
  CHECK(!isError(meshesCmd_add(String("work"))), "D3b setup add");
  CHECK(!isError(meshesCmd_remove(String("work"))), "D3b setup disable work");
  CHECK(!isError(meshesCmd_delete(String("primary"))), "D3b delete the only enabled mesh");
  CHECK(!isError(meshesCmd_enable(String("work"))) && gSettings.meshes[1].isDefault, "D3b enable work");
  startEspNow();
  CHECK(slotIsEmpty(0) && enabledDefaults() == 1 && labelCount("primary") == 0,
        "D3b the start created a second enabled default mesh");
  // With the other mesh still disabled the start creates nothing either; the warning says no mesh is enabled.
  resetAll();
  CHECK(!isError(meshesCmd_add(String("work"))) && !isError(meshesCmd_remove(String("work"))), "D3b setup");
  CHECK(!isError(meshesCmd_delete(String("primary"))), "D3b delete");
  startEspNow();
  CHECK(slotIsEmpty(0) && enabledDefaults() == 0, "D3b a disabled mesh elsewhere did not keep slot 0 empty");

  // D3c: a mesh labelled "primary" in another slot never gets a twin in slot 0.
  resetAll();
  gSettings.meshes[0].label = "home";
  gSettings.meshes[0].fingerprint = meshFingerprintForLabel(String("home"));
  CHECK(!isError(meshesCmd_add(String("primary"))) && !isError(meshesCmd_setdefault(String("primary"))), "D3c setup");
  startEspNow();
  CHECK(!isError(meshesCmd_delete(String("home"))), "D3c delete home");
  stopEspNow();
  startEspNow();
  CHECK(labelCount("primary") == 1 && slotOf("primary") == 1 && slotIsEmpty(0), "D3c two meshes are labelled 'primary'");

  // D4: an instance that holds the deleted mesh's passphrase drops it: running...
  resetNeverStarted();
  CHECK(!isError(meshesCmd_add(String("bench"))), "D4 setup");
  startEspNow();
  CHECK(!isError(meshesCmd_setpassphrase(String("bench"), String("correct-horse"))), "D4 setup passphrase");
  CHECK(gState.passphrase == String("correct-horse"), "D4 setup: live passphrase");
  CHECK(!isError(meshesCmd_delete(String("bench"))), "D4 delete");
  CHECK(gState.passphrase.length() == 0 && gState.derivedKey[0] == 0 && !gState.encryptionEnabled,
        "D4 the deleted mesh's passphrase is still in use (running)");
  // ...and stopped, where a passphrase set after the stop is still in the instance.
  resetNeverStarted();
  CHECK(!isError(meshesCmd_add(String("bench"))), "D4 setup");
  startEspNow();
  stopEspNow();
  CHECK(!isError(meshesCmd_setpassphrase(String("bench"), String("correct-horse"))), "D4 setup passphrase while stopped");
  CHECK(gState.passphrase == String("correct-horse"), "D4 setup: live passphrase while stopped");
  CHECK(!isError(meshesCmd_delete(String("bench"))), "D4 delete while stopped");
  CHECK(gState.passphrase.length() == 0 && gState.derivedKey[0] == 0 && !gState.encryptionEnabled,
        "D4 the deleted mesh's passphrase is still in the stopped instance");

  // D4b: deleting a mesh whose passphrase the instance does not hold leaves the live one alone.
  resetAll();
  startEspNow();
  CHECK(!isError(meshesCmd_setpassphrase(String("primary"), String("prim-passphrase"))), "D4b setup slot 0");
  CHECK(!isError(meshesCmd_add(String("work"))) && !isError(meshesCmd_setpassphrase(String("work"), String("work-passphrase"))) &&
            !isError(meshesCmd_setdefault(String("work"))),
        "D4b setup work: keyed and default, not live until the next start");
  CHECK(!isError(meshesCmd_add(String("temp"))) && !isError(meshesCmd_setpassphrase(String("temp"), String("temp-passphrase"))),
        "D4b setup temp");
  CHECK(gState.passphrase == String("prim-passphrase"), "D4b setup: live passphrase is slot 0's");
  CHECK(!isError(meshesCmd_delete(String("temp"))), "D4b delete temp");
  CHECK(gState.passphrase == String("prim-passphrase"), "D4b deleting another mesh replaced the live passphrase");

  // D5: a failed save is reported; an unknown label is an error.
  makeBenchLeftover();
  gPersistOk = false;
  CHECK(isError(meshesCmd_delete(String("bench"))), "D5 a failed save was reported as success");
  gPersistOk = true;
  CHECK(isError(meshesCmd_delete(String("nosuch"))), "D5 unknown label");

  // D6: an empty label (a quoted "" on the command line) matches no mesh, also
  // not an emptied slot: nothing is created, deleted or changed.
  resetAll();
  startEspNow();
  CHECK(!isError(meshesCmd_add(String("lab"))) && !isError(meshesCmd_setpassphrase(String("lab"), String("lab-passphrase"))) &&
            !isError(meshesCmd_setdefault(String("lab"))) && !isError(meshesCmd_delete(String("primary"))),
        "D6 setup: slot 0 emptied, lab keyed and default");
  const int persistsAtD6 = gPersists;
  const String liveAtD6 = gState.passphrase;
  CHECK(isError(meshesCmd_setpassphrase(String(""), String("some-passphrase"))), "D6 setpassphrase \"\" was accepted");
  CHECK(slotIsEmpty(0) && enabledDefaults() == 1 && labelCount("primary") == 0 && gState.passphrase == liveAtD6,
        "D6 setpassphrase \"\" created a mesh in slot 0 or replaced the live passphrase");
  CHECK(isError(meshesCmd_delete(String(""))) && isError(meshesCmd_remove(String(""))) && isError(meshesCmd_enable(String(""))) &&
            isError(meshesCmd_setdefault(String(""))),
        "D6 an empty label was taken for a mesh");
  CHECK(gPersists == persistsAtD6 && slotIsEmpty(0) && !gSettings.meshes[0].enabled, "D6 an empty label changed something");
}

// The settings file keeps slot numbers, so a hole left by delete survives a restart.
static void saveAndReload() {
  JsonDocument doc;
  espnowMeshesWriteJson(doc, false);
  std::string text;
  serializeJson(doc, text);
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    gSettings.meshes[i] = Settings::MeshIdentity();
    gSettings.bondModeEnabledMesh[i] = false;
    gSettings.bondRoleMesh[i] = 0;
    gSettings.bondPeerMacMesh[i] = "";
  }
  JsonDocument back;
  CHECK(!deserializeJson(back, text.c_str()), "the written settings did not parse");
  espnowMeshesReadJson(back);
}
static void testSlotsSurviveARestart() {
  // S1: delete the middle one of three; after a restart the third is still in slot 2.
  resetAll();
  CHECK(!isError(meshesCmd_add(String("lab"))) && !isError(meshesCmd_add(String("work"))), "S1 setup");
  CHECK(!isError(meshesCmd_setpassphrase(String("work"), String("work-passphrase"))), "S1 setup passphrase");
  gSettings.bondModeEnabledMesh[2] = true;
  gSettings.bondPeerMacMesh[2] = "02:00:00:00:00:09";
  CHECK(!isError(meshesCmd_remove(String("lab"))) && !isError(meshesCmd_delete(String("lab"))), "S1 delete lab");
  saveAndReload();
  CHECK(slotOf("primary") == 0 && slotIsEmpty(1) && slotOf("work") == 2,
        "S1 the meshes changed slot across a restart (primary %d, work %d)", slotOf("primary"), slotOf("work"));
  CHECK(gSettings.meshes[2].passphrase == String("work-passphrase") && gSettings.meshes[2].passphraseStretchedKeyValid &&
            gSettings.meshes[2].enabled && gSettings.meshes[2].fingerprint == meshFingerprintForLabel(String("work")),
        "S1 the mesh in slot 2 lost its passphrase, key, state or fingerprint");
  CHECK(gSettings.bondModeEnabledMesh[2] && gSettings.bondPeerMacMesh[2] == String("02:00:00:00:00:09"),
        "S1 the bond settings are no longer beside their mesh");
  CHECK(enabledDefaults() == 1 && gSettings.meshes[0].isDefault, "S1 default flag");
  // A device paired in "work" (slot 2) still holds its slot after the restart.
  gPeers[0] = PeerIdentity{{2, 0, 0, 0, 0, 7}, 2, true};
  CHECK(isError(meshesCmd_delete(String("work"))), "S1 the paired device no longer protects its mesh");
  gPeers[0].valid = false;

  // S2: an emptied slot 0 stays empty across a restart; the mesh in slot 1 does not move into it.
  resetAll();
  CHECK(!isError(meshesCmd_add(String("work"))) && !isError(meshesCmd_setdefault(String("work"))), "S2 setup");
  CHECK(!isError(meshesCmd_delete(String("primary"))), "S2 delete slot 0");
  saveAndReload();
  CHECK(slotIsEmpty(0) && slotOf("work") == 1 && gSettings.meshes[1].isDefault, "S2 the mesh moved into slot 0");

  // S3: a file written before slot numbers existed lists the meshes in slot order.
  resetAll();
  {
    JsonDocument old;
    deserializeJson(old, "{\"espnow\":{\"meshes\":[{\"label\":\"primary\",\"enabled\":true,\"isDefault\":true},"
                         "{\"label\":\"lab\",\"enabled\":false,\"isDefault\":false},"
                         "{\"label\":\"work\",\"passphrase\":\"enc:old-passphrase\",\"enabled\":true,\"isDefault\":false}]}}");
    espnowMeshesReadJson(old);
  }
  CHECK(slotOf("primary") == 0 && slotOf("lab") == 1 && slotOf("work") == 2 && slotIsEmpty(3) &&
            gSettings.meshes[2].passphrase == String("old-passphrase"),
        "S3 an older settings file was not read in slot order");

  // S4: slot numbers that cannot be used (out of range, taken) fall back to the lowest free slot.
  {
    JsonDocument odd;
    deserializeJson(odd, "{\"espnow\":{\"meshes\":[{\"slot\":2,\"label\":\"a\",\"enabled\":true},"
                         "{\"slot\":2,\"label\":\"b\",\"enabled\":true},{\"slot\":9,\"label\":\"c\",\"enabled\":true},"
                         "{\"slot\":-1,\"label\":\"d\",\"enabled\":true},{\"slot\":3,\"label\":\"e\",\"enabled\":true}]}}");
    espnowMeshesReadJson(odd);
  }
  CHECK(slotOf("a") == 2 && slotOf("b") == 0 && slotOf("c") == 1 && slotOf("d") == 3 && slotOf("e") < 0,
        "S4 unusable slot numbers: a=%d b=%d c=%d d=%d e=%d", slotOf("a"), slotOf("b"), slotOf("c"), slotOf("d"), slotOf("e"));

  // S5: the passphrase and the stretched key go through putSecret; the copy for
  // other readers (excludePasswords) carries neither, and both carry the slot.
  resetAll();
  CHECK(!isError(meshesCmd_add(String("work"))) && !isError(meshesCmd_setpassphrase(String("work"), String("work-passphrase"))),
        "S5 setup");
  JsonDocument full, shared;
  espnowMeshesWriteJson(full, false);
  espnowMeshesWriteJson(shared, true);
  std::string fullText, sharedText;
  serializeJson(full, fullText);
  serializeJson(shared, sharedText);
  CHECK(fullText.find("enc:work-passphrase") != std::string::npos && fullText.find("\"work-passphrase\"") == std::string::npos,
        "S5 the passphrase is not stored through putSecret");
  CHECK(sharedText.find("work-passphrase") == std::string::npos && sharedText.find("passphraseStretchedKey") == std::string::npos,
        "S5 the shared copy carries key material");
  CHECK(fullText.find("\"slot\":1") != std::string::npos && sharedText.find("\"slot\":1") != std::string::npos,
        "S5 the slot number is not written");
}

// A new pairing never lands in a slot without a mesh.
static void testPairingSlot() {
  const uint8_t none = Settings::N_MESHES;
  // Q1: no mesh argument: the default mesh (slot 0 on an ordinary device).
  resetAll();
  CHECK(meshSlotForPairing(String("")) == 0, "Q1 default on a device with only 'primary'");
  CHECK(!isError(meshesCmd_add(String("work"))) && !isError(meshesCmd_setdefault(String("work"))), "Q1 setup");
  CHECK(meshSlotForPairing(String("")) == 1, "Q1 no argument did not go to the default mesh (slot %u)",
        (unsigned)meshSlotForPairing(String("")));
  // Q2: slot 0 emptied by delete: no argument still means the default mesh, never the empty slot.
  CHECK(!isError(meshesCmd_delete(String("primary"))), "Q2 setup: delete slot 0");
  CHECK(slotIsEmpty(0) && meshSlotForPairing(String("")) == 1, "Q2 a pairing with no mesh argument went to the emptied slot %u",
        (unsigned)meshSlotForPairing(String("")));
  // A slot number that holds no mesh is refused, a label or number of a real one is taken.
  CHECK(meshSlotForPairing(String("0")) == none && meshSlotForPairing(String("2")) == none, "Q2 an empty slot was accepted by number");
  CHECK(meshSlotForPairing(String("1")) == 1 && meshSlotForPairing(String("work")) == 1, "Q2 the mesh by number or label");
  CHECK(meshSlotForPairing(String("7")) == none && meshSlotForPairing(String("nosuch")) == none, "Q2 unknown mesh");
  // Q3: no enabled default: the first enabled mesh; no enabled mesh: the mesh in slot 0, if there is one.
  resetAll();
  CHECK(!isError(meshesCmd_add(String("work"))), "Q3 setup");
  gSettings.meshes[0].isDefault = false;
  CHECK(meshSlotForPairing(String("")) == 0, "Q3 first enabled mesh");
  CHECK(!isError(meshesCmd_remove(String("work"))) && !isError(meshesCmd_remove(String("primary"))), "Q3 setup: all disabled");
  CHECK(meshSlotForPairing(String("")) == 0, "Q3 no enabled mesh: the mesh in slot 0 as before");
  CHECK(meshSlotForPairing(String("work")) == 1, "Q3 a disabled mesh named by its label, as before");
  // Q4: no mesh at all: nothing to pair into.
  resetNeverStarted();
  CHECK(meshSlotForPairing(String("")) == none && meshSlotForPairing(String("0")) == none, "Q4 a device with no mesh");
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

  // E4: a flow that worked before still ends with one enabled default.
  resetAll();
  CHECK(!isError(meshesCmd_add(String("work"))) && !isError(meshesCmd_setdefault(String("work"))), "E4 setup");
  CHECK(!isError(meshesCmd_remove(String("primary"))) && !isError(meshesCmd_enable(String("primary"))), "E4 remove and enable");
  CHECK(enabledDefaults() == 1 && gSettings.meshes[slotOf("work")].isDefault, "E4 the default moved or doubled");
}

static void testMessagesNameTheMesh() {
  // P1: clearing the passphrase of a mesh in slot 0 names that mesh and says what happened to it.
  resetNeverStarted();
  CHECK(!isError(meshesCmd_add(String("bench"))), "P1 setup add");
  startEspNow();
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

  // P2: the start-up warning names the mesh whose passphrase a start loads.
  char w[224];
  resetNeverStarted();
  CHECK(!isError(meshesCmd_add(String("bench"))), "P2 setup");
  formatNoPassphraseWarning(w, sizeof(w));
  CHECK(has(w, "espnowsetpassphrase bench <pw>") && !has(w, "primary"), "P2 mesh 'bench' in slot 0: %s", w);
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
  CHECK(!has(w, "espnowsetpassphrase primary") && has(w, "no mesh is enabled") && has(w, "espnowmeshes enable"),
        "P2 no enabled mesh: %s", w);
  // An enabled mesh that is not the default: its passphrase would not be loaded,
  // so the warning must not send the user to set one there.
  resetAll();
  startEspNow();
  CHECK(!isError(meshesCmd_add(String("work"))), "P2 setup add");
  CHECK(!isError(meshesCmd_setpassphrase(String("primary"), String("prim-passphrase"))) &&
            !isError(meshesCmd_setpassphrase(String("primary"), String(""))),
        "P2 setup: slot 0 keyed and cleared (disabled, flag kept)");
  CHECK(gSettings.meshes[1].enabled && enabledDefaults() == 0, "P2 setup: an enabled mesh, no enabled default");
  formatNoPassphraseWarning(w, sizeof(w));
  CHECK(has(w, "espnowmeshes setdefault") && !has(w, "espnowsetpassphrase work <pw>"), "P2 enabled mesh that is not the default: %s", w);
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
  gState.devices[0].name = "benchp4";  // the own entry
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

  // N4: ordinary names still work and validate-only still answers without acting.
  resetAll();
  CHECK(isError(cmd_espnow_setname(String("has.dot"))), "N4 a name with '.' was accepted");
  gCliValidateOnly = true;
  CHECK(has(cmd_espnow_setname(String("clear")), "VALID"), "N4 validate-only");
  gCliValidateOnly = false;

  // N5: with the radio off the own address may not be readable; the clear still happens.
  resetAll();
  CHECK(has(cmd_espnow_setname(String("benchp4")), "benchp4"), "N5 setup");
  gSelfMacReadable = false;
  r = cmd_espnow_setname(String("clear"));
  CHECK(!isError(r) && gSettings.espnowDeviceName.length() == 0, "N5 clear with no readable address: %s", r);

  // N6: a device that has not started ESP-NOW since boot (no instance).
  resetNeverStarted();
  CHECK(has(cmd_espnow_setname(String("benchp4")), "benchp4"), "N6 setup");
  r = cmd_espnow_setname(String("clear"));
  CHECK(!isError(r) && gSettings.espnowDeviceName.length() == 0, "N6 clear with no instance: %s", r);
}

int main() {
  testOnlyMeshCanBeRemovedAndDeleted();
  testDeleteFreesTheSlot();
  testSlotsSurviveARestart();
  testPairingSlot();
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
