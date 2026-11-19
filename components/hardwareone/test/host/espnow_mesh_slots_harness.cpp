// Host harness for test_espnow_mesh_slots.py.
//
// The runner pastes the production Settings::MeshIdentity record into the
// Settings double and the production mesh-slot and device-name code at the
// production marker: the mesh-id helpers (refresh, snapshot, clash slot, the
// boot clash resolution, fingerprintForPeer, staging and commit), the
// meshes settings writer and reader, setEspNowPassphrase,
// initPrimaryMeshFromLegacySettings, the start-passphrase and hint helpers, the
// espnowmeshes subcommands (add, remove, delete, enable, setdefault,
// setpassphrase, listjson) with the delete guard's counters, the helpers that
// keep the running instance on the passphrase a start would load, the device
// name's ESP-NOW side and the one setter cmd_devicename (System_DeviceName.cpp). A stop whose cleanup is still pending is modelled by
// clearing gState.initialized with the passphrase left in place, as
// deinitEspNow leaves it when it returns early.
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
#include <map>
#include <string>
#include <vector>

#include "System_CommandReply.h"
#include "System_DeviceName.h"         // the device-name rule (the setter and the alias checks)
#include "System_ESPNow_MeshIdCore.h"  // the mesh id: pick, clashSlot
#include "System_MacText.h"            // macToDisplay (espnowNameInUseBy)

void webBatchTestStringAppend() {}

static int gFailures = 0;
static int gChecks = 0;
#define CHECK(cond, ...)                                              \
  do {                                                                \
    ++gChecks;                                                        \
    if (!(cond)) {                                                    \
      ++gFailures;                                                    \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                \
      std::printf(__VA_ARGS__);                                       \
      std::printf("\n");                                              \
    }                                                                 \
  } while (0)

#define ENABLE_BONDED_MODE 1
#define ENABLE_ESPNOW 1
static int gWarnLines = 0;
#define WARN_ESPNOWF(...) (++gWarnLines)
#define ERROR_ESPNOWF(...) ((void)0)
#define PSRAM_JSON_DOC(name) JsonDocument name
#define EXT_RAM_BSS_ATTR

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
  String deviceName;  // written only by trees from before the field was dropped (2026-11-17)
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
static uint16_t meshKeysIdFor(const uint8_t stretched[32]);  // below, with the mesh-id doubles
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
  // As production: keys and the mesh id come from the stretched key together.
  if (!gSettings.meshes[i].passphraseStretchedKeyValid) { gDerivedValid[i] = false; return false; }
  gSettings.meshes[i].fingerprint = meshKeysIdFor(gSettings.meshes[i].passphraseStretchedKey);
  gDerivedValid[i] = true;
  return true;
}
struct MeshDerivedKeys {};
static const MeshDerivedKeys* meshKeysGet(uint8_t i) {
  static const MeshDerivedKeys keys;
  return gDerivedValid[i] ? &keys : nullptr;
}
static void sodium_memzero(void* p, size_t n) { std::memset(p, 0, n); }

// ---- The mesh id behind the key doubles (System_ESPNow_MeshIdCore.h has the
// rule; test_espnow_mesh_id_kat.py pins the real derivation). The toy stretch
// puts a label+passphrase's id in its first two bytes, so meshid::pick reads it
// back; a case can force the id a label+passphrase gives, to make a clash.
static std::map<std::string, uint16_t> gForcedIds;
static bool gCryptoReady = true;
static int gStretches = 0;
static bool espnowCryptoReady() { return gCryptoReady; }
static bool meshKeysStretchInto(const String& label, const String& passphrase, uint8_t out[32]) {
  std::memset(out, 0, 32);
  if (!gCryptoReady || passphrase.length() == 0) return false;
  ++gStretches;
  const std::string key = std::string(label.c_str()) + "/" + passphrase.c_str();
  uint16_t id;
  const auto forced = gForcedIds.find(key);
  if (forced != gForcedIds.end()) {
    id = forced->second;
  } else {
    uint32_t h = 2166136261u;
    for (unsigned char c : key) h = (h ^ c) * 16777619u;
    id = (uint16_t)(h ^ (h >> 16));
    if (id == 0) id = 1;
  }
  std::memset(out, 0x5A, 32);
  out[0] = (uint8_t)(id & 0xFF);
  out[1] = (uint8_t)(id >> 8);
  return true;
}
static uint16_t meshKeysIdFor(const uint8_t stretched[32]) { return meshid::pick(stretched); }
static uint8_t meshKeysRefreshIds() {
  uint8_t changed = 0;
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    Settings::MeshIdentity& m = gSettings.meshes[i];
    if (m.label.length() == 0 || !m.passphraseStretchedKeyValid) continue;
    const uint16_t id = meshKeysIdFor(m.passphraseStretchedKey);
    if (id != m.fingerprint) { m.fingerprint = id; ++changed; }
  }
  return changed;
}

// ---- The device name's other users (System_SelfDevice.cpp) and the metadata
// cache espnowNameInUseBy reads.
static int gHostnameApplies = 0;
// Console lines (BROADCAST_PRINTF), for the pair commands' shared-name note.
static std::vector<std::string> gBroadcasts;
#define BROADCAST_PRINTF(...)                                       \
  do {                                                              \
    char _line[256];                                                \
    std::snprintf(_line, sizeof _line, __VA_ARGS__);                \
    gBroadcasts.emplace_back(_line);                                \
  } while (0)
namespace SelfDevice {
static String deviceName() {
  return devicename::valid(gSettings.espnowDeviceName.c_str()) ? gSettings.espnowDeviceName
                                                                : String("HardwareOne-A1B2C3");
}
static void applyHostname() { ++gHostnameApplies; }
}  // namespace SelfDevice
struct MeshPeerMeta { uint8_t mac[6]; char name[32]; bool isActive; };
static MeshPeerMeta gMetaSlots[4] = {};
static MeshPeerMeta* gMeshPeerMeta = gMetaSlots;
static int gMeshPeerSlots = 4;


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
static bool sEspNowDevicesFileBehind = false;  // set by a registry save that did not reach the file
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

#if HW1_HAVE_LIVE_SLOT
// Production keeps this beside setEspNowPassphrase: the slot the live passphrase came from.
static int8_t gEspNowLivePassphraseSlot = -1;
#endif

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
  sEspNowDevicesFileBehind = false;
#if HW1_HAVE_LIVE_SLOT
  gEspNowLivePassphraseSlot = -1;
#endif
  gRegistryFile = RegistryFile::Absent;
  gRegistryFileText.clear();
  gRegistryFileReads = 0;
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    gDerivedValid[i] = false;
    gInvalidations[i] = 0;
  }
  gForcedIds.clear();
  gCryptoReady = true;
  gStretches = 0;
  gWarnLines = 0;
  gHostnameApplies = 0;
  std::memset(gMetaSlots, 0, sizeof(gMetaSlots));
}
// What a start does to the tables these commands read: the instance exists, the
// registry is in RAM with this device's own entry, slot 0 is bootstrapped.
static void startEspNow() {
  gEspNow = &gState;
  if (sEspNowDevicesLoad == ConfigLoad::Status::NotLoaded) sEspNowDevicesLoad = ConfigLoad::Status::AbsentOk;
  initPrimaryMeshFromLegacySettings();
#if HW1_HAVE_LIVE_LOADER
  // initEspNow marks the instance initialized, then loads (pinned there by the runner).
  gState.initialized = true;
  espnowLoadLivePassphrase();
#elif HW1_HAVE_LIVE_SLOT
  {
    // The passphrase a start loads, as initEspNow does it (pinned there by the runner).
    const uint8_t slot = espnowStartPassphraseSlot();
    const String pw = gSettings.meshes[slot].passphrase;
    gEspNowLivePassphraseSlot = pw.length() > 0 ? (int8_t)slot : -1;
    gState.passphrase = pw;
    deriveKeyFromPassphrase(pw, gState.derivedKey);
  }
#endif
  gState.initialized = true;
  gLastSelfKnown = true;
  bool haveSelf = false;
  for (int i = 0; i < gState.deviceCount; i++) haveSelf = haveSelf || std::memcmp(gState.devices[i].mac, kSelfMac, 6) == 0;
  if (!haveSelf) addEspNowDevice(kSelfMac, String("self"), false, nullptr);
}
static void stopEspNow() {
  gState.initialized = false;
#if HW1_HAVE_LIVE_SLOT
  // A stop clears the live passphrase and key, as the production stop does.
  gState.passphrase = "";
  std::memset(gState.derivedKey, 0, sizeof(gState.derivedKey));
  gState.encryptionEnabled = false;
  gEspNowLivePassphraseSlot = -1;
#endif
}
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
// Older firmware's clear of slot 0's passphrase: the mesh disabled, its default
// flag left. A clear does not do that any more, but a settings file written by
// that firmware still holds the state, so the cases that need it build it here.
static void oldFirmwareClearSlot0() {
  Settings::MeshIdentity& m0 = gSettings.meshes[0];
  m0.passphrase = "";
  std::memset(m0.passphraseStretchedKey, 0, sizeof(m0.passphraseStretchedKey));
  m0.passphraseStretchedKeyValid = false;
  m0.enabled = false;
}
// The device as the bench boards were left: 'add bench' before any start put the
// mesh in slot 0, it was keyed, then older firmware cleared its passphrase
// (disabled, default flag kept). ESP-NOW is stopped.
static void makeBenchLeftover() {
  resetNeverStarted();
  CHECK(!isError(meshesCmd_add(String("bench"))) && slotOf("bench") == 0, "setup: add bench");
  startEspNow();
  CHECK(!isError(meshesCmd_setpassphrase(String("bench"), String("correct-horse"))), "setup: set passphrase");
  oldFirmwareClearSlot0();
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
            gSettings.meshes[0].fingerprint == 0,  // no passphrase: no mesh id
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
  // A save that did not reach the file (full filesystem, refused) leaves an
  // unpaired device in it, to come back at the next start: the file counts again.
  resetAll();
  CHECK(!isError(meshesCmd_add(String("lab"))), "D2d setup");
  sEspNowDevicesLoad = ConfigLoad::Status::Ok;
  sEspNowDevicesFileBehind = true;
  gRegistryFile = RegistryFile::Present;
  gRegistryFileText = "{\"devices\":[{\"meshId\":1}]}";
  r = meshesCmd_delete(String("lab"));
  CHECK(isError(r) && has(r, "1 in the device list") && slotOf("lab") == 1,
        "D2d a device still in the file after a failed save was missed: %s", r);
  sEspNowDevicesFileBehind = false;
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
  gSettings.meshes[0].fingerprint = 0;  // never keyed: no mesh id
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
  // ...and stopped: a stopped instance holds no passphrase (the next start loads
  // one), so neither the set nor the delete puts one there.
  resetNeverStarted();
  CHECK(!isError(meshesCmd_add(String("bench"))), "D4 setup");
  startEspNow();
  stopEspNow();
  CHECK(!isError(meshesCmd_setpassphrase(String("bench"), String("correct-horse"))), "D4 setup passphrase while stopped");
  CHECK(gState.passphrase.length() == 0 && !gState.encryptionEnabled, "D4 a passphrase set while stopped went into the stopped instance");
  CHECK(!isError(meshesCmd_delete(String("bench"))), "D4 delete while stopped");
  CHECK(gState.passphrase.length() == 0 && gState.derivedKey[0] == 0 && !gState.encryptionEnabled,
        "D4 the deleted mesh's passphrase is still in the stopped instance");

  // D4b: deleting a mesh whose passphrase the instance does not hold leaves the live one alone.
  resetAll();
  startEspNow();
  CHECK(!isError(meshesCmd_setpassphrase(String("primary"), String("prim-passphrase"))), "D4b setup slot 0");
  CHECK(!isError(meshesCmd_add(String("temp"))) && !isError(meshesCmd_setpassphrase(String("temp"), String("temp-passphrase"))),
        "D4b setup temp");
  CHECK(gState.passphrase == String("prim-passphrase"), "D4b setup: live passphrase is slot 0's");
  CHECK(!isError(meshesCmd_delete(String("temp"))), "D4b delete temp");
  CHECK(gState.passphrase == String("prim-passphrase"), "D4b deleting another mesh replaced the live passphrase");
  // The same when the other mesh happens to have the very same passphrase text.
  CHECK(!isError(meshesCmd_add(String("temp"))) && !isError(meshesCmd_setpassphrase(String("temp"), String("prim-passphrase"))),
        "D4b setup: temp with primary's passphrase text");
  CHECK(!isError(meshesCmd_delete(String("temp"))), "D4b delete temp again");
  CHECK(gState.passphrase == String("prim-passphrase") && gState.derivedKey[0] == 0xA5,
        "D4b a mesh with the same passphrase text was taken for the live one");

  // D4c: the instance holds what a start would load. Changing that mesh's
  // passphrase after the start reaches the instance, and deleting the mesh
  // drops what the instance holds.
  resetAll();
  CHECK(!isError(meshesCmd_add(String("work"))) && !isError(meshesCmd_setpassphrase(String("work"), String("work-passphrase"))) &&
            !isError(meshesCmd_setdefault(String("work"))) && !isError(meshesCmd_delete(String("primary"))),
        "D4c setup: work is the only mesh, keyed and default");
  startEspNow();
  CHECK(gState.passphrase == String("work-passphrase"), "D4c setup: the start loaded the default mesh's passphrase");
  CHECK(!isError(meshesCmd_setpassphrase(String("work"), String("changed-passphrase"))), "D4c setup: change it after the start");
#if HW1_HAVE_LIVE_SLOT
  CHECK(gState.passphrase == String("changed-passphrase") && gEspNowLivePassphraseSlot == slotOf("work"),
        "D4c the running instance did not take the new passphrase of the mesh in use");
#endif
  CHECK(!isError(meshesCmd_delete(String("work"))), "D4c delete work");
  CHECK(gState.passphrase.length() == 0 && gState.derivedKey[0] == 0 && !gState.encryptionEnabled,
        "D4c the instance still holds the deleted mesh's start-time passphrase");

  // D5: a failed save is reported; an unknown label is an error.
  makeBenchLeftover();
  gPersistOk = false;
  CHECK(isError(meshesCmd_delete(String("bench"))), "D5 a failed save was reported as success");
  gPersistOk = true;
  CHECK(isError(meshesCmd_delete(String("nosuch"))), "D5 unknown label");
  // 'add' and 'setdefault' write the settings file once and report a failed save.
  resetAll();
  startEspNow();
  int persistsAtD5 = gPersists;
  CHECK(!isError(meshesCmd_add(String("lab"))) && gPersists == persistsAtD5 + 1, "D5 'add' did not save once");
  gPersistOk = false;
  CHECK(isError(meshesCmd_add(String("work"))), "D5 a failed save of 'add' was reported as success");
  CHECK(isError(meshesCmd_setdefault(String("lab"))), "D5 a failed save of 'setdefault' was reported as success");
  gPersistOk = true;
  persistsAtD5 = gPersists;
  CHECK(!isError(meshesCmd_setdefault(String("lab"))) && gPersists == persistsAtD5 + 1,
        "D5 a repeated 'setdefault' did not save after the failed save");

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
  CHECK(isError(meshesCmd_rename(String(""), String("foo"))) && slotOf("foo") < 0, "D6 rename \"\" labelled an empty slot");
  CHECK(meshSlotByLabel(String("")) == -1 && meshSlotByLabel(String("lab")) == 1, "D6 meshSlotByLabel took \"\" for a mesh");
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
            gSettings.meshes[2].enabled && gSettings.meshes[2].fingerprint != 0 &&
            gSettings.meshes[2].fingerprint == meshKeysIdFor(gSettings.meshes[2].passphraseStretchedKey),
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
  makeBenchLeftover();  // slot 0 disabled, flag left behind by an older firmware's passphrase clear
  CHECK(!isError(meshesCmd_add(String("work"))), "E1 add");
  CHECK(gSettings.meshes[slotOf("work")].isDefault, "E1 the first enabled mesh did not become the default");
  // A mesh that was disabled at start-up has a passphrase and no derived keys.
  // Enabling it derives them, so no restart is needed before it signs frames.
  const bool benchKeyed = gSettings.meshes[0].passphrase.length() > 0;
  gDerivedValid[0] = false;
  CHECK(!isError(meshesCmd_enable(String("bench"))), "E1 enable");
  CHECK(enabledDefaults() == 1 && gSettings.meshes[slotOf("work")].isDefault, "E1 two enabled defaults, or the default moved");
  CHECK(gDerivedValid[0] == benchKeyed, "E1 enable did not derive the keys of a mesh with a passphrase (keyed=%d)", (int)benchKeyed);

  // E1b: the same with a passphrase in place, as after a start-up that skipped
  // the disabled mesh: enable derives the keys.
  resetAll();
  CHECK(!isError(meshesCmd_setpassphrase(String("primary"), String("correct-horse"))), "E1b set passphrase");
  CHECK(!isError(meshesCmd_remove(String("primary"))), "E1b remove");
  gDerivedValid[0] = false;
  CHECK(!isError(meshesCmd_enable(String("primary"))), "E1b enable");
  CHECK(gSettings.meshes[0].passphrase.length() > 0 && gDerivedValid[0],
        "E1b enable left a mesh with a passphrase without derived keys");

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
  CHECK(!isError(meshesCmd_setpassphrase(String("primary"), String("prim-passphrase"))), "P2 setup: slot 0 keyed");
  oldFirmwareClearSlot0();  // cleared by older firmware: disabled, flag kept
  CHECK(gSettings.meshes[1].enabled && enabledDefaults() == 0, "P2 setup: an enabled mesh, no enabled default");
  formatNoPassphraseWarning(w, sizeof(w));
  CHECK(has(w, "espnowmeshes setdefault") && !has(w, "espnowsetpassphrase work <pw>"), "P2 enabled mesh that is not the default: %s", w);
#if HW1_HAVE_PASSPHRASE_HINT
  // P3: the passphrase a start loads comes from an enabled default mesh in slots
  // 1..3 that has one, else from slot 0 while its mesh is enabled and has one, else
  // none (-1).
  resetAll();
  CHECK((int)espnowStartPassphraseSlot() == -1, "P3 new device: a slot without a passphrase was named");
  CHECK(!isError(meshesCmd_add(String("work"))) && !isError(meshesCmd_setdefault(String("work"))), "P3 setup");
  CHECK((int)espnowStartPassphraseSlot() == -1, "P3 a default mesh without a passphrase was chosen");
  CHECK(!isError(meshesCmd_setpassphrase(String("primary"), String("prim-passphrase"))), "P3 set slot 0");
  CHECK((int)espnowStartPassphraseSlot() == 0, "P3 an enabled keyed slot 0 was not chosen");
  CHECK(!isError(meshesCmd_setpassphrase(String("work"), String("work-passphrase"))), "P3 set");
  CHECK((int)espnowStartPassphraseSlot() == slotOf("work"), "P3 the keyed default mesh was not chosen");
#endif
}

static void testNameCanBeCleared() {
  // N1: 'clear' removes the name while ESP-NOW is stopped.
  resetAll();
  CHECK(has(cmd_devicename(String("benchp4")), "benchp4"), "N1 setup");
  CHECK(gSettings.espnowFirstTimeSetup, "N1 setup flag");
  gState.devices[0].name = "benchp4";  // the own entry
  gEvents.clear();
  gMetadataChanged = false;
  const char* r = cmd_devicename(String("clear"));
  CHECK(!isError(r) && has(r, "cleared"), "N1 clear refused: %s", r);
  CHECK(gSettings.espnowDeviceName.length() == 0, "N1 the name is still '%s'", gSettings.espnowDeviceName.c_str());
  CHECK(!gSettings.espnowFirstTimeSetup, "N1 the setup-done flag stayed set with no name");
  CHECK(gState.devices[0].name.length() == 0, "N1 the stopped instance kept the name");
  CHECK(gMetadataChanged, "N1 metadata not marked changed");
  CHECK(gEvents.size() == 1 && gEvents[0].kind == SYSEVT_SETTING_CHANGED && gEvents[0].subject == "deviceName",
        "N1 no setting-changed event");
  CHECK(has(cmd_devicename(String("")), "(not set)"), "N1 the query still shows a name");

  // N2: refused while ESP-NOW runs or is still shutting down; the name stays.
  resetAll();
  CHECK(has(cmd_devicename(String("benchp4")), "benchp4"), "N2 setup");
  gState.initialized = true;
  r = cmd_devicename(String("clear"));
  CHECK(isError(r) && has(r, "closeespnow"), "N2 cleared under a running ESP-NOW: %s", r);
  CHECK(gSettings.espnowDeviceName == String("benchp4"), "N2 a refused clear changed the name");
  gState.initialized = false;
  gShutdownPending = true;
  CHECK(isError(cmd_devicename(String("clear"))), "N2 cleared during a pending shutdown");
  gShutdownPending = false;

  // N3: clearing an unset name is a plain answer, with no write and no event.
  resetAll();
  const int persistsBefore = gPersists;
  r = cmd_devicename(String("clear"));
  CHECK(!isError(r) && has(r, "(not set)") && gPersists == persistsBefore && gEvents.empty(), "N3 unset name: %s", r);

  // N4: ordinary names still work and validate-only still answers without acting.
  resetAll();
  CHECK(isError(cmd_devicename(String("has.dot"))), "N4 a name with '.' was accepted");
  gCliValidateOnly = true;
  CHECK(has(cmd_devicename(String("clear")), "VALID"), "N4 validate-only");
  gCliValidateOnly = false;

  // N5: with the radio off the own address may not be readable; the clear still happens.
  resetAll();
  CHECK(has(cmd_devicename(String("benchp4")), "benchp4"), "N5 setup");
  gSelfMacReadable = false;
  r = cmd_devicename(String("clear"));
  CHECK(!isError(r) && gSettings.espnowDeviceName.length() == 0, "N5 clear with no readable address: %s", r);

  // N6: a device that has not started ESP-NOW since boot (no instance).
  resetNeverStarted();
  CHECK(has(cmd_devicename(String("benchp4")), "benchp4"), "N6 setup");
  r = cmd_devicename(String("clear"));
  CHECK(!isError(r) && gSettings.espnowDeviceName.length() == 0, "N6 clear with no instance: %s", r);
}

// The slot of the passphrase the instance holds, as the instance shows it: the
// marker byte of the legacy key double says whether it holds one at all.
static bool instanceHolds(const char* passphrase) {
  if (!passphrase) return gState.passphrase.length() == 0 && gState.derivedKey[0] == 0 && !gState.encryptionEnabled;
  return gState.passphrase == String(passphrase) && gState.derivedKey[0] == 0xA5 && gState.encryptionEnabled;
}
// What 'espnowmeshes listjson' says about the passphrase in use: {running, passphraseSlot}.
static bool listjsonSays(bool running, int slot) {
  JsonDocument doc;
  if (deserializeJson(doc, meshesCmd_listjson())) return false;
  return doc["running"].is<bool>() && doc["running"].as<bool>() == running &&
         doc["passphraseSlot"].is<int>() && doc["passphraseSlot"].as<int>() == slot;
}

static void testPassphraseInUse() {
  // L1: a passphrase set on the default mesh in slot 1 while ESP-NOW runs is in
  // use at once, and the reply says so instead of "not used for current encryption".
  resetAll();
  startEspNow();
  CHECK(!isError(meshesCmd_add(String("work"))) && !isError(meshesCmd_setdefault(String("work"))), "L1 setup");
  CHECK(instanceHolds(nullptr), "L1 setup: no passphrase anywhere");
  const char* r = meshesCmd_setpassphrase(String("work"), String("work-passphrase"));
  CHECK(!isError(r), "L1 set: %s", r);
  CHECK(instanceHolds("work-passphrase"), "L1 the running instance did not take the default mesh's new passphrase");
  CHECK(!has(r, "Not used for current encryption") && !has(r, "will need to re-pair"), "L1 the reply still says it is not used: %s", r);
  CHECK(has(r, "applies now") && has(r, "do not need to re-pair") && has(r, "need the same passphrase") &&
            has(r, "uses the passphrase of mesh 'work'"),
        "L1 the reply does not say what applies now and what peers need: %s", r);
  CHECK(listjsonSays(true, 1), "L1 listjson does not name slot 1 as the passphrase in use");

  // L2: slot 0's passphrase set beside it does not replace the default mesh's.
  r = meshesCmd_setpassphrase(String("primary"), String("prim-passphrase"));
  CHECK(!isError(r) && instanceHolds("work-passphrase"), "L2 slot 0's passphrase replaced the default mesh's: %s", r);
  CHECK(has(r, "uses the passphrase of mesh 'work'") && has(r, "applies now"), "L2 the reply does not name the mesh in use: %s", r);
#if HW1_HAVE_PASSPHRASE_HINT
  CHECK((int)espnowStartPassphraseSlot() == 1, "L2 a start would load another slot");
#endif

  // L3: setdefault moves the passphrase in use at once, and says which.
  r = meshesCmd_setdefault(String("primary"));
  CHECK(!isError(r) && instanceHolds("prim-passphrase"), "L3 setdefault primary did not bring its passphrase into use: %s", r);
  CHECK(has(r, "uses the passphrase of mesh 'primary'"), "L3 reply: %s", r);
  r = meshesCmd_setdefault(String("work"));
  CHECK(!isError(r) && instanceHolds("work-passphrase") && has(r, "mesh 'work'"), "L3 setdefault work: %s", r);
  CHECK(listjsonSays(true, 1), "L3 listjson");

  // L4: a default mesh without a passphrase leaves slot 0's in use, as a start does.
  CHECK(!isError(meshesCmd_add(String("lab"))), "L4 setup");
  r = meshesCmd_setdefault(String("lab"));
  CHECK(!isError(r) && instanceHolds("prim-passphrase") && has(r, "uses the passphrase of mesh 'primary'"),
        "L4 setdefault to a mesh without a passphrase: %s", r);
  CHECK(listjsonSays(true, 0), "L4 listjson");

  // L5: clearing the passphrase in use falls back to what a start would load.
  r = meshesCmd_setdefault(String("work"));
  CHECK(instanceHolds("work-passphrase"), "L5 setup: %s", r);
  r = meshesCmd_setpassphrase(String("work"), String(""));
  CHECK(!isError(r) && instanceHolds("prim-passphrase") && has(r, "uses the passphrase of mesh 'primary'"),
        "L5 cleared default mesh: %s", r);
  CHECK(has(r, "keep their keys and open sessions"), "L5 the clear does not say what happens to paired devices: %s", r);
  // Without the group key the mesh's broadcasts go out unsigned and keyed peers drop them.
  CHECK(has(r, "heartbeats and time sync now go out unsigned") && has(r, "their signed ones are dropped here"),
        "L5 the clear does not say that the mesh's heartbeats stop getting through: %s", r);
  r = meshesCmd_setpassphrase(String("primary"), String(""));
  CHECK(!isError(r) && instanceHolds(nullptr), "L5 the cleared passphrase is still in use: %s", r);
  // Slot 0's clear also disables its mesh, whose frames are then dropped: the reply says so.
  CHECK(!gSettings.meshes[0].enabled && has(r, "The mesh is disabled") &&
            has(r, "cannot exchange frames with this device") && has(r, "'espnowmeshes enable primary'"),
        "L5 slot 0's clear does not say its paired devices cannot exchange frames: %s", r);
  CHECK(has(r, "no passphrase in use") && has(r, "until mesh 'work' has one"), "L5 the reply does not say none is in use: %s", r);
  CHECK(listjsonSays(true, -1), "L5 listjson");

  // L6: enable brings a keyed mesh into use when it becomes the default; disabling
  // it leaves none in use while slot 0's mesh is disabled too (a disabled mesh keys
  // nothing), and enabling slot 0 brings its passphrase back. Each reply says so.
  resetAll();
  startEspNow();
  CHECK(!isError(meshesCmd_setpassphrase(String("primary"), String("prim-passphrase"))) &&
            !isError(meshesCmd_add(String("work"))) &&
            !isError(meshesCmd_setpassphrase(String("work"), String("work-passphrase"))) &&
            !isError(meshesCmd_remove(String("work"))) && !isError(meshesCmd_remove(String("primary"))),
        "L6 setup: both keyed, both disabled");
  CHECK(enabledDefaults() == 0 && instanceHolds(nullptr), "L6 setup: a disabled slot 0's passphrase is in use");
  CHECK(listjsonSays(true, -1), "L6 listjson names a disabled mesh's passphrase");
  r = meshesCmd_enable(String("work"));
  CHECK(!isError(r) && gSettings.meshes[1].isDefault && instanceHolds("work-passphrase"),
        "L6 enable did not bring the new default's passphrase into use: %s", r);
  CHECK(has(r, "uses the passphrase of mesh 'work'"), "L6 the enable reply does not say: %s", r);
  r = meshesCmd_remove(String("work"));
  CHECK(!isError(r) && instanceHolds(nullptr) && has(r, "no passphrase in use") && !has(r, "mesh 'primary'"),
        "L6 disabling the default mesh handed the passphrase to the disabled slot 0: %s", r);
  r = meshesCmd_enable(String("primary"));
  CHECK(!isError(r) && instanceHolds("prim-passphrase") && has(r, "uses the passphrase of mesh 'primary'"),
        "L6 enabling slot 0 did not bring its passphrase into use: %s", r);
  // A command that does not move it does not mention it.
  r = meshesCmd_enable(String("work"));
  CHECK(!isError(r) && instanceHolds("prim-passphrase") && !has(r, "passphrase of mesh"), "L6 enable work: %s", r);

  // L7: delete names the passphrase in use when it moved.
  resetAll();
  startEspNow();
  CHECK(!isError(meshesCmd_setpassphrase(String("primary"), String("prim-passphrase"))) &&
            !isError(meshesCmd_add(String("work"))) && !isError(meshesCmd_setdefault(String("work"))),
        "L7 setup");
  r = meshesCmd_delete(String("primary"));
  CHECK(!isError(r) && instanceHolds(nullptr) && has(r, "no passphrase in use"), "L7 delete of the mesh in use: %s", r);

  // L8: rename keeps the passphrase in use.
  resetAll();
  startEspNow();
  CHECK(!isError(meshesCmd_setpassphrase(String("primary"), String("prim-passphrase"))), "L8 setup");
  CHECK(!isError(meshesCmd_rename(String("primary"), String("home"))) && instanceHolds("prim-passphrase"), "L8 rename");

  // L9: while ESP-NOW is stopped the instance holds none; the reply says what the
  // next start uses, and the start loads it.
  resetAll();
  CHECK(!isError(meshesCmd_add(String("work"))) && !isError(meshesCmd_setdefault(String("work"))), "L9 setup");
  r = meshesCmd_setpassphrase(String("work"), String("work-passphrase"));
  CHECK(!isError(r) && instanceHolds(nullptr), "L9 the stopped instance took a passphrase: %s", r);
  CHECK(has(r, "stopped") && has(r, "next start uses the passphrase of mesh 'work'") &&
            has(r, "applies from the next ESP-NOW start") && !has(r, "applies now"),
        "L9 reply while stopped: %s", r);
  r = meshesCmd_setpassphrase(String("primary"), String("prim-passphrase"));
  CHECK(!isError(r) && instanceHolds(nullptr) && has(r, "next start uses the passphrase of mesh 'work'"),
        "L9 slot 0 while stopped: %s", r);
  CHECK(listjsonSays(false, 1), "L9 listjson while stopped");
  startEspNow();
  CHECK(instanceHolds("work-passphrase"), "L9 the start did not load the default mesh's passphrase");

  // L10: before the first start of a boot (no instance) slot 0's passphrase is
  // still stored, and the first start uses it.
  resetNeverStarted();
  CHECK(!isError(meshesCmd_add(String("bench"))) && slotOf("bench") == 0, "L10 setup");
  r = meshesCmd_setpassphrase(String("bench"), String("correct-horse"));
  CHECK(!isError(r) && gSettings.meshes[0].passphrase == String("correct-horse") && gSettings.meshes[0].passphraseStretchedKeyValid,
        "L10 slot 0's passphrase was not stored without an instance: %s", r);
  CHECK(has(r, "next start uses the passphrase of mesh 'bench'"), "L10 reply: %s", r);
  CHECK(listjsonSays(false, 0), "L10 listjson");
  startEspNow();
  CHECK(instanceHolds("correct-horse"), "L10 the first start did not use it");

  // L11: a new passphrase on a disabled mesh (slot 0 after a clear, or one turned
  // off with 'remove') does not claim to apply: the mesh's frames are dropped until
  // it is enabled again, so the reply says that and how.
  resetAll();
  startEspNow();
  CHECK(!isError(meshesCmd_setpassphrase(String("primary"), String("prim-passphrase"))) &&
            !isError(meshesCmd_setpassphrase(String("primary"), String(""))) && !gSettings.meshes[0].enabled,
        "L11 setup: slot 0 cleared and disabled");
  r = meshesCmd_setpassphrase(String("primary"), String("prim-passphrase"));
  CHECK(!isError(r) && !gSettings.meshes[0].enabled && gSettings.meshes[0].passphrase == String("prim-passphrase"),
        "L11 set on the disabled slot 0: %s", r);
  CHECK(has(r, "the mesh is disabled") && has(r, "'espnowmeshes enable primary'") && !has(r, "applies now") &&
            has(r, "do not need to re-pair"),
        "L11 the reply for a disabled slot 0 mesh does not say it stays off: %s", r);
  // A disabled mesh keys nothing, so the note does not claim the device uses it.
  CHECK(instanceHolds(nullptr) && !has(r, "uses the passphrase of mesh 'primary'") && has(r, "no passphrase in use"),
        "L11 the passphrase of the disabled slot 0 mesh is in use: %s", r);
  CHECK(!isError(meshesCmd_add(String("work"))) && !isError(meshesCmd_remove(String("work"))) &&
            !gSettings.meshes[1].enabled,
        "L11 setup: 'work' disabled");
  r = meshesCmd_setpassphrase(String("work"), String("work-passphrase"));
  CHECK(!isError(r) && has(r, "the mesh is disabled") && has(r, "'espnowmeshes enable work'") && !has(r, "applies now"),
        "L11 the reply for a disabled mesh in slot 1 does not say it stays off: %s", r);
}

// X6: clearing slot 0's passphrase disables its mesh, so it follows the rule of
// 'espnowmeshes remove': refused with the same reply while slot 0 is the default
// and another enabled mesh exists; otherwise the mesh is disabled and is no
// longer the default.
static void testSlot0ClearFollowsRemove() {
  // C1: slot 0 is the default and 'work' is enabled: refused, nothing changes.
  resetAll();
  startEspNow();
  CHECK(!isError(meshesCmd_setpassphrase(String("primary"), String("prim-passphrase"))) &&
            !isError(meshesCmd_add(String("work"))) &&
            !isError(meshesCmd_setpassphrase(String("work"), String("work-passphrase"))),
        "C1 setup");
  const int persistsBefore = gPersists;
  const int invalidationsBefore = gInvalidations[0];
  gEvents.clear();
  const char* r = meshesCmd_setpassphrase(String("primary"), String(""));
  CHECK(isError(r) && std::strcmp(r, meshDefaultInUseReply()) == 0,
        "C1 clearing the default slot 0 beside an enabled mesh was not refused like remove: %s", r);
  CHECK(std::strcmp(r, meshesCmd_remove(String("primary"))) == 0, "C1 the clear and remove replies differ: %s", r);
  CHECK(gSettings.meshes[0].enabled && gSettings.meshes[0].isDefault &&
            gSettings.meshes[0].passphrase == String("prim-passphrase") && gSettings.meshes[0].passphraseStretchedKeyValid &&
            gInvalidations[0] == invalidationsBefore,
        "C1 the refused clear changed slot 0");
  CHECK(gPersists == persistsBefore && gEvents.empty() && instanceHolds("prim-passphrase"),
        "C1 the refused clear persisted, posted an event or changed the passphrase in use");
  // C2: with the default moved first, the clear goes through: disabled, not the default.
  CHECK(!isError(meshesCmd_setdefault(String("work"))), "C2 setup: default to work");
  r = meshesCmd_setpassphrase(String("primary"), String(""));
  CHECK(!isError(r) && !gSettings.meshes[0].enabled && !gSettings.meshes[0].isDefault &&
            enabledDefaults() == 1 && gSettings.meshes[1].isDefault && instanceHolds("work-passphrase"),
        "C2 the clear after setdefault: %s", r);

  // C3: the only mesh: the clear goes through and the mesh loses the default
  // flag as 'remove' does; enabling it again makes it the default again.
  resetAll();
  startEspNow();
  CHECK(!isError(meshesCmd_setpassphrase(String("primary"), String("prim-passphrase"))), "C3 setup");
  r = meshesCmd_setpassphrase(String("primary"), String(""));
  CHECK(!isError(r) && !gSettings.meshes[0].enabled && !gSettings.meshes[0].isDefault && enabledDefaults() == 0,
        "C3 the cleared only mesh kept its default flag: %s", r);
  CHECK(!isError(meshesCmd_enable(String("primary"))) && gSettings.meshes[0].isDefault && enabledDefaults() == 1,
        "C3 enabling the cleared mesh did not make it the default again");

  // C4: an old-firmware leftover (disabled, flagged default) beside an enabled
  // mesh: the mesh is not enabled, so the clear is allowed and drops the stale flag.
  resetAll();
  startEspNow();
  CHECK(!isError(meshesCmd_setpassphrase(String("primary"), String("prim-passphrase"))) &&
            !isError(meshesCmd_add(String("work"))),
        "C4 setup");
  gSettings.meshes[0].enabled = false;  // disabled by older firmware, flag kept
  r = meshesCmd_setpassphrase(String("primary"), String(""));
  CHECK(!isError(r) && !gSettings.meshes[0].isDefault && gSettings.meshes[0].passphrase.length() == 0,
        "C4 the clear of a disabled slot 0 was refused or kept the stale default flag: %s", r);
}

// X7: a disabled slot 0 keys nothing. The passphrase in use is an enabled default
// mesh's in slots 1..3, else slot 0's while its mesh is enabled, else none; the
// loader, the replies, listjson and the start-up warning agree.
static void testDisabledMeshKeysNothing() {
  // K1: the keyed only mesh is disabled: the device holds no passphrase now.
  resetAll();
  startEspNow();
  CHECK(!isError(meshesCmd_setpassphrase(String("primary"), String("prim-passphrase"))), "K1 setup");
  const char* r = meshesCmd_remove(String("primary"));
  CHECK(!isError(r) && instanceHolds(nullptr) && listjsonSays(true, -1),
        "K1 the disabled slot 0's passphrase is still in use: %s", r);
  CHECK(has(r, "no passphrase in use"), "K1 the remove reply does not say no passphrase is in use now: %s", r);
#if HW1_HAVE_LIVE_SLOT
  CHECK(gEspNowLivePassphraseSlot == -1, "K1 the live slot still names slot 0");
#endif

  // K2: a new default mesh without a passphrase beside it: still none, and the
  // start-up warning sends the user to that mesh and says what is refused.
  r = meshesCmd_add(String("lab"));
  CHECK(!isError(r) && gSettings.meshes[slotOf("lab")].isDefault && instanceHolds(nullptr) && listjsonSays(true, -1),
        "K2 add lab: %s", r);
  CHECK((int)espnowStartPassphraseSlot() == -1, "K2 a start would load the disabled slot 0's passphrase");
  char w[224];
  formatNoPassphraseWarning(w, sizeof(w));
  CHECK(has(w, "espnowsetpassphrase lab <pw>") && has(w, "sending and pairing are refused") &&
            !has(w, "fail to decrypt"),
        "K2 the start-up warning: %s", w);

  // K3: stopped, the next start finds none either.
  stopEspNow();
  CHECK(listjsonSays(false, -1), "K3 listjson while stopped names the disabled slot 0");
  r = meshesCmd_setdefault(String("lab"));
  CHECK(has(r, "next start finds no passphrase"), "K3 the stopped reply: %s", r);
  startEspNow();
  CHECK(instanceHolds(nullptr), "K3 the start loaded the disabled slot 0's passphrase");

  // K4: enabling slot 0 again brings its passphrase into use (lab stays the default).
  r = meshesCmd_enable(String("primary"));
  CHECK(!isError(r) && gSettings.meshes[slotOf("lab")].isDefault && instanceHolds("prim-passphrase") &&
            has(r, "uses the passphrase of mesh 'primary'") && listjsonSays(true, 0),
        "K4 enable primary: %s", r);

  // K5: the start-up warning with no mesh enabled says enabling may be enough.
  resetAll();
  CHECK(!isError(meshesCmd_setpassphrase(String("primary"), String("prim-passphrase"))) &&
            !isError(meshesCmd_remove(String("primary"))),
        "K5 setup");
  formatNoPassphraseWarning(w, sizeof(w));
  CHECK(has(w, "no mesh is enabled") && has(w, "espnowmeshes enable <label>") && has(w, "if it has none"),
        "K5 the warning with only a disabled keyed mesh: %s", w);
}

// X3: an instance whose stop is still pending (initialized is false, the stop has
// not reached the point where it clears the passphrase) must not keep a passphrase
// the settings no longer have. Any mesh command that reloads leaves it holding none.
static void testPendingStopHoldsNoPassphrase() {
  // T1: 'work' (slot 1) keyed and the default, then a stop that stayed pending.
  resetAll();
  startEspNow();
  CHECK(!isError(meshesCmd_add(String("work"))) &&
            !isError(meshesCmd_setpassphrase(String("work"), String("work-passphrase"))) &&
            !isError(meshesCmd_setdefault(String("work"))) && instanceHolds("work-passphrase"),
        "T1 setup");
  gState.initialized = false;  // deinitEspNow stopped early: the passphrase is still there
  gShutdownPending = true;
  const char* r = meshesCmd_setdefault(String("primary"));
  CHECK(!isError(r) && instanceHolds(nullptr), "T1 the pending-stop instance kept a passphrase: %s", r);
#if HW1_HAVE_LIVE_SLOT
  CHECK(gEspNowLivePassphraseSlot == -1, "T1 the live slot still names slot %d", (int)gEspNowLivePassphraseSlot);
#endif
  CHECK(has(r, "ESP-NOW is stopped") && has(r, "next start finds no passphrase"), "T1 the reply: %s", r);
  r = meshesCmd_delete(String("work"));
  CHECK(!isError(r) && instanceHolds(nullptr) && listjsonSays(false, -1), "T1 delete of the mesh it held: %s", r);

  // T2: a command that moves nothing still leaves a pending-stop instance empty.
  resetAll();
  startEspNow();
  CHECK(!isError(meshesCmd_setpassphrase(String("primary"), String("prim-passphrase"))) &&
            !isError(meshesCmd_add(String("lab"))) && instanceHolds("prim-passphrase"),
        "T2 setup");
  gState.initialized = false;
  gShutdownPending = true;
  r = meshesCmd_setpassphrase(String("lab"), String("lab-passphrase"));
  CHECK(!isError(r) && instanceHolds(nullptr) && has(r, "next start uses the passphrase of mesh 'primary'"),
        "T2 setpassphrase on another mesh: %s", r);
  gShutdownPending = false;
}

// The mesh id comes from the mesh key (System_ESPNow_MeshIdCore.h): none (0)
// until a passphrase is set, never 0 on the air, unique among this device's
// meshes (a command that would clash is refused and changes nothing), kept when
// the passphrase is cleared, and a clash found at boot keeps the lower slot.
static void testMeshIds() {
  static const uint8_t kBcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  // I1: 'add fjqo' (CRC16 of the label is 0) then a passphrase: a nonzero id.
  resetAll();
  const char* r = meshesCmd_add(String("fjqo"));
  CHECK(!isError(r) && has(r, "mesh id: none until a passphrase is set"), "I1 add: %s", r);
  CHECK(gSettings.meshes[slotOf("fjqo")].fingerprint == 0, "I1 a mesh without a passphrase has an id");
  CHECK(!isError(meshesCmd_setpassphrase(String("fjqo"), String("fjqo-passphrase"))), "I1 setpassphrase");
  const uint16_t fjqoId = gSettings.meshes[slotOf("fjqo")].fingerprint;
  CHECK(fjqoId != 0, "I1 the keyed mesh has no id (FAILS ON BASE: CRC16('fjqo') == 0)");

  // I2: the default 'primary' has no passphrase: frames take the keyed mesh's
  // id, never 0; with no keyed mesh nothing has an id (nothing is sent).
  CHECK(fingerprintForPeer(kBcast) == fjqoId, "I2 a broadcast took the keyless default mesh's id 0 (got 0x%04X)",
        (unsigned)fingerprintForPeer(kBcast));
  resetAll();
  CHECK(fingerprintForPeer(kBcast) == 0, "I2 a device with only a keyless mesh has an id to send");

  // I3: a passphrase whose id another mesh here has is refused; nothing changes.
  resetAll();
  CHECK(!isError(meshesCmd_add(String("lab"))) && !isError(meshesCmd_setpassphrase(String("lab"), String("lab-passphrase"))),
        "I3 setup lab");
  const uint16_t labId = gSettings.meshes[slotOf("lab")].fingerprint;
  CHECK(!isError(meshesCmd_add(String("work"))), "I3 add work");
  const int work = slotOf("work");
  gForcedIds["work/work-passphrase"] = labId;
  int persists = gPersists;
  r = meshesCmd_setpassphrase(String("work"), String("work-passphrase"));
  CHECK(isError(r) && has(r, "would get mesh id") && has(r, "'lab'"), "I3 a clashing passphrase was taken: %s", r);
  CHECK(gSettings.meshes[work].passphrase.length() == 0 && !gSettings.meshes[work].passphraseStretchedKeyValid &&
            gSettings.meshes[work].fingerprint == 0 && gPersists == persists,
        "I3 the refused passphrase changed the mesh");

  // I4: the same for a rename (the label is part of the key): the label stays.
  CHECK(!isError(meshesCmd_setpassphrase(String("work"), String("other-passphrase"))), "I4 setup");
  const uint16_t workId = gSettings.meshes[work].fingerprint;
  CHECK(workId != 0 && workId != labId, "I4 setup ids");
  gForcedIds["garage/other-passphrase"] = labId;
  r = meshesCmd_rename(String("work"), String("garage"));
  CHECK(isError(r) && has(r, "would get mesh id"), "I4 a clashing rename was taken: %s", r);
  CHECK(gSettings.meshes[work].label == String("work") && gSettings.meshes[work].fingerprint == workId &&
            gSettings.meshes[work].passphrase == String("other-passphrase"),
        "I4 the refused rename changed the mesh");
  r = meshesCmd_rename(String("work"), String("shed"));
  CHECK(!isError(r) && has(r, "new mesh id") && gSettings.meshes[work].fingerprint != workId,
        "I4 a rename does not give a new id: %s", r);

  // I5: enable stages the key of a mesh whose stretched key is not cached; a
  // clash refuses the enable and the mesh stays disabled.
  CHECK(!isError(meshesCmd_remove(String("shed"))) && !gSettings.meshes[work].enabled, "I5 setup remove");
  gSettings.meshes[work].passphraseStretchedKeyValid = false;
  gForcedIds["shed/other-passphrase"] = labId;
  r = meshesCmd_enable(String("shed"));
  CHECK(isError(r) && has(r, "would get mesh id") && !gSettings.meshes[work].enabled, "I5 a clashing enable: %s", r);
  gForcedIds.erase("shed/other-passphrase");
  r = meshesCmd_enable(String("shed"));
  CHECK(!isError(r) && gSettings.meshes[work].enabled && gSettings.meshes[work].fingerprint != 0 &&
            gSettings.meshes[work].fingerprint != labId && has(r, "mesh id 0x"),
        "I5 enable: %s", r);

  // I6: a cleared passphrase keeps the id (paired devices keep their traffic),
  // and the id survives a save and load without a key to derive it from.
  CHECK(!isError(meshesCmd_setpassphrase(String("lab"), String(""))), "I6 clear");
  CHECK(gSettings.meshes[slotOf("lab")].fingerprint == labId, "I6 the cleared mesh lost its id");
  saveAndReload();
  CHECK(gSettings.meshes[slotOf("lab")].fingerprint == labId, "I6 the stored id did not survive a restart");

  // I7: a passphrase change gives a new id and says so.
  resetAll();
  CHECK(!isError(meshesCmd_add(String("lab"))) && !isError(meshesCmd_setpassphrase(String("lab"), String("first-passphrase"))),
        "I7 setup");
  startEspNow();
  r = meshesCmd_setpassphrase(String("lab"), String("second-passphrase"));
  CHECK(!isError(r) && has(r, "mesh id is now 0x") && has(r, "all traffic with them stops"),
        "I7 the reply does not say the mesh id changed: %s", r);

  // I8: two meshes with one id at boot (an edited settings file): the lower slot
  // keeps it, the higher one is off the air for this boot, nothing is written.
  resetAll();
  CHECK(!isError(meshesCmd_add(String("lab"))) && !isError(meshesCmd_setpassphrase(String("lab"), String("lab-passphrase"))) &&
            !isError(meshesCmd_add(String("work"))) && !isError(meshesCmd_setpassphrase(String("work"), String("work-passphrase"))),
        "I8 setup");
  const int labSlot = slotOf("lab"), workSlot = slotOf("work");
  std::memcpy(gSettings.meshes[workSlot].passphraseStretchedKey, gSettings.meshes[labSlot].passphraseStretchedKey, 32);
  gEvents.clear();
  gWarnLines = 0;
  persists = gPersists;
  espnowMeshIdsAfterKeyInit();
  CHECK(gSettings.meshes[labSlot].fingerprint != 0 && gSettings.meshes[workSlot].fingerprint == 0 &&
            gSettings.meshes[workSlot].enabled && gPersists == persists,
        "I8 the boot clash was not resolved toward the lower slot (lab 0x%04X, work 0x%04X)",
        (unsigned)gSettings.meshes[labSlot].fingerprint, (unsigned)gSettings.meshes[workSlot].fingerprint);
  CHECK(gWarnLines == 1 && gEvents.size() == 1, "I8 the clash was not reported once (%d WARN, %zu events)",
        gWarnLines, gEvents.size());
  // I8b: the next ESP-NOW start (PrimaryMeshBootstrap) and a settings reload
  // refresh every keyed mesh's id from its key. The loser keeps no id and no
  // keys, and is not reported again.
  meshIdRefreshAll();
  meshIdRefreshAll();
  CHECK(gSettings.meshes[workSlot].fingerprint == 0 && !gDerivedValid[workSlot] &&
            gSettings.meshes[labSlot].fingerprint != 0,
        "I8b the clash loser regained id 0x%04X after the start's refresh (lab 0x%04X)",
        (unsigned)gSettings.meshes[workSlot].fingerprint, (unsigned)gSettings.meshes[labSlot].fingerprint);
  CHECK(gWarnLines == 1 && gEvents.size() == 1, "I8b the clash was reported again at a refresh (%d WARN, %zu events)",
        gWarnLines, gEvents.size());
  // I8c: once the clash is gone (another key for 'work'), a refresh gives it
  // its own id back, and a later clash is reported again.
  gSettings.meshes[workSlot].passphraseStretchedKey[0] ^= 0x01;
  meshIdRefreshAll();
  CHECK(gSettings.meshes[workSlot].fingerprint != 0 &&
            gSettings.meshes[workSlot].fingerprint != gSettings.meshes[labSlot].fingerprint,
        "I8c 'work' got no id back once its key differs (0x%04X)", (unsigned)gSettings.meshes[workSlot].fingerprint);
  std::memcpy(gSettings.meshes[workSlot].passphraseStretchedKey, gSettings.meshes[labSlot].passphraseStretchedKey, 32);
  meshIdRefreshAll();
  CHECK(gSettings.meshes[workSlot].fingerprint == 0 && gWarnLines == 2,
        "I8c a new clash was not resolved and reported (work 0x%04X, %d WARN)",
        (unsigned)gSettings.meshes[workSlot].fingerprint, gWarnLines);
  // I8d: the clash loser, disabled, with its stretched key cached: enabling it
  // is refused and nothing changes (the enable's stretched-key branch).
  gSettings.meshes[workSlot].enabled = false;
  gSettings.meshes[workSlot].isDefault = false;
  {
    const uint16_t labIdNow = gSettings.meshes[labSlot].fingerprint;
    const char* r8 = meshesCmd_enable(String("work"));
    CHECK(isError(r8) && has(r8, "mesh id") && !gSettings.meshes[workSlot].enabled &&
              gSettings.meshes[workSlot].fingerprint == 0,
          "I8d enabling the clash loser (cached key) was not refused: %s", r8);
    // I8e: no passphrase, a stored id another mesh has (an edited file): the
    // enable's stored-id branch refuses it too.
    gSettings.meshes[workSlot].passphrase = String("");
    gSettings.meshes[workSlot].passphraseStretchedKeyValid = false;
    gSettings.meshes[workSlot].fingerprint = labIdNow;
    r8 = meshesCmd_enable(String("work"));
    CHECK(isError(r8) && has(r8, "mesh id") && !gSettings.meshes[workSlot].enabled,
          "I8e enabling a mesh whose stored id another mesh has was not refused: %s", r8);
  }
}

// The one device name ('devicename', System_DeviceName.cpp): the rule, the
// warning when another device has the name, and the hostname.
static void testDeviceNameSetter() {
  resetAll();
  const std::string n31(31, 'k');
  const char* r = cmd_devicename(String(n31.c_str()));
  CHECK(!isError(r) && gSettings.espnowDeviceName == String(n31.c_str()),
        "G1 a 31-byte name was refused (FAILS ON BASE: 19-byte limit): %s", r);
  CHECK(gHostnameApplies == 1, "G1 the hostname was not handed on");
  for (const char* bad : {"a_b", "-lead", "trail-", "clear-", "all", "JSON", "DEADBEEFCAFE", "aa-bb-cc-dd-ee-ff"}) {
    r = cmd_devicename(String(bad));
    CHECK(isError(r) && gSettings.espnowDeviceName == String(n31.c_str()), "G2 '%s' was taken: %s", bad, r);
  }
  r = cmd_devicename(String(std::string(32, 'k').c_str()));
  CHECK(isError(r) && has(r, "31"), "G2 a 32-byte name: %s", r);
  // Another device here has the name (ignoring case): set, with a warning that names it.
  startEspNow();
  addRegistryDevice(5, 0);  // "peer"
  r = cmd_devicename(String("PEER"));
  CHECK(!isError(r) && gSettings.espnowDeviceName == String("PEER") && has(r, "Warning:") &&
            has(r, "peer (02:00:00:00:00:05)") && has(r, "need the MAC"),
        "G3 a name a paired device has: %s", r);
  const uint8_t metaMac[6] = {0x02, 0, 0, 0, 0, 0x33};
  std::memcpy(gMetaSlots[0].mac, metaMac, 6);
  std::snprintf(gMetaSlots[0].name, sizeof(gMetaSlots[0].name), "kitchen");
  gMetaSlots[0].isActive = true;
  r = cmd_devicename(String("Kitchen"));
  CHECK(!isError(r) && has(r, "Warning:") && has(r, "kitchen (02:00:00:00:00:33)"), "G3 a name a peer announces: %s", r);
  r = cmd_devicename(String("garage"));
  CHECK(!isError(r) && !has(r, "Warning"), "G3 a free name warned: %s", r);
}

// A peer's name given to the pair commands (espnowPeerAliasRefusal): the
// device-name rule refuses it with one wording; a name another device here has
// is taken with a note.
static void testPeerAliasRule() {
  resetAll();
  for (const char* bad : {"a_b", "-lead", "all", "JSON", "DEADBEEFCAFE", "two words"}) {
    const char* r = espnowPeerAliasRefusal(String(bad));
    CHECK(r && has(r, "Error: Peer name:"), "P1 the peer name '%s' was not refused: %s", bad, r ? r : "(taken)");
  }
  const char* r = espnowPeerAliasRefusal(String(std::string(32, 'p').c_str()));
  CHECK(r && has(r, "31"), "P1 a 32-byte peer name was not refused: %s", r ? r : "(taken)");
  gBroadcasts.clear();
  CHECK(espnowPeerAliasRefusal(String("Shed-2")) == nullptr && gBroadcasts.empty(), "P2 a free valid peer name");
  startEspNow();
  addRegistryDevice(5, 0);  // "peer"
  const uint8_t other[6] = {0x02, 0, 0, 0, 0, 0x44};
  CHECK(espnowPeerAliasRefusal(String("Peer"), other) == nullptr && gBroadcasts.size() == 1 &&
            has(gBroadcasts[0].c_str(), "also the name of peer (02:00:00:00:00:05)"),
        "P3 a peer name another device has is taken with a note (%zu lines)", gBroadcasts.size());
}

int main() {
  testMeshIds();
  testPeerAliasRule();
  testDeviceNameSetter();
  testOnlyMeshCanBeRemovedAndDeleted();
  testDeleteFreesTheSlot();
  testSlotsSurviveARestart();
  testPairingSlot();
  testOneDefault();
  testMessagesNameTheMesh();
  testNameCanBeCleared();
  testPassphraseInUse();
  testSlot0ClearFollowsRemove();
  testDisabledMeshKeysNothing();
  testPendingStopHoldsNoPassphrase();
  if (gFailures) {
    std::printf("%d of %d check(s) failed\n", gFailures, gChecks);
    return 1;
  }
  std::printf("all %d mesh slot and device name checks passed\n", gChecks);
  return 0;
}
