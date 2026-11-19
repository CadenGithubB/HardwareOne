// Host harness for test_espnow_start_order.py.
//
// The runner pastes the production Settings::MeshIdentity record into the
// Settings double, and meshIdRefreshAll,
// initPrimaryMeshFromLegacySettings and (when the tree has it) the
// PrimaryMeshBootstrap guard at the production marker. ENABLE_BONDED_MODE comes
// from the command line (compiled once per value). A "start" here is what
// initEspNow does with slot 0: construct the guard, let a command run while the
// start is under way (boot autostart runs in setup beside cmd_exec), then keep
// the guard on success or drop it on failure. On a tree without the guard the
// start runs the bootstrap the way initEspNow did before the fix (and never
// undoes it). requestSettingsPersist counts the settings writes.
#include <Arduino.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef ENABLE_BONDED_MODE
#error "ENABLE_BONDED_MODE must be defined by the runner"
#endif
#ifndef HW1_HAVE_BOOTSTRAP
#error "HW1_HAVE_BOOTSTRAP must be defined by the runner"
#endif

void webBatchTestStringAppend() {}

static int gChecks = 0;
static int gFailures = 0;
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

// ---- Settings double: the production mesh record inside a small Settings.
struct Settings {
  static constexpr uint8_t N_MESHES = 4;
  // INSERT_MESH_IDENTITY
  MeshIdentity meshes[N_MESHES];
#if ENABLE_BONDED_MODE
  bool bondModeEnabledMesh[N_MESHES] = {};
  uint8_t bondRoleMesh[N_MESHES] = {};
  String bondPeerMacMesh[N_MESHES];
  bool bondModeEnabled = false;
  uint8_t bondRole = 0;
  String bondPeerMac;
#endif
};
static Settings gSettings;

// Settings writes requested (the production call writes the whole file).
static int gPersists = 0;
static bool requestSettingsPersist() {
  ++gPersists;
  return true;
}
static void sodium_memzero(void* p, size_t n) { std::memset(p, 0, n); }

// The mesh id (System_ESPNow_MeshIdCore.h) comes from the mesh key, which a
// start never derives: the ids stand as stored (crypto is "not ready" here, so
// meshIdRefreshAll leaves them), and a mesh without a passphrase has none (0).
static bool espnowCryptoReady() { return false; }
static uint8_t meshKeysRefreshIds() { return 0; }
// The clash pass that follows the refresh (espnow_mesh_slots runs the real one).
static void meshIdResolveClashes() {}

// INSERT_PRODUCTION

static void resetSettings() {
  gSettings = Settings();
  gPersists = 0;
}

static bool same(const String& a, const char* b) { return std::strcmp(a.c_str(), b) == 0; }


// What a start may do to slot 0 between the bootstrap and its end: stretch
// the passphrase under the label slot 0 has then.
static void stretchSlot0() {
  gSettings.meshes[0].passphraseStretchedKey[0] = 0xAB;
  gSettings.meshes[0].passphraseStretchedKeyValid = true;
}

// One start attempt as initEspNow runs it for slot 0; `during` stands for a
// command that runs on cmd_exec while the start is under way.
using During = void (*)();
#if HW1_HAVE_BOOTSTRAP
static void startAttempt(bool succeeds, bool stretch = false, During during = nullptr) {
  PrimaryMeshBootstrap bootstrap;
  if (stretch) stretchSlot0();
  if (during) during();
  if (succeeds) bootstrap.keep();
}
#else
static void startAttempt(bool, bool stretch = false, During during = nullptr) {
  initPrimaryMeshFromLegacySettings();
  meshIdRefreshAll();
  if (stretch) stretchSlot0();
  if (during) during();
}
#endif

// Commands a user can run during the start (what each does to the settings).
static void renamePrimaryToHome() {
  gSettings.meshes[0].label = "home";  // a rename of a mesh without a passphrase keeps its id (none)
  gSettings.meshes[0].passphraseStretchedKeyValid = false;
}
static void setPrimaryPassphrase() {
  gSettings.meshes[0].passphrase = "pw-during";
  gSettings.meshes[0].passphraseStretchedKey[0] = 0x77;
  gSettings.meshes[0].passphraseStretchedKeyValid = true;
}
// A passphrase whose key stretch failed: the key stays zeroed and invalid, as
// the bootstrap left it, so only the passphrase text differs.
static void setPrimaryPassphraseStretchFailed() {
  gSettings.meshes[0].passphrase = "pw-nokey";
}
static void addLabInSlot1() {
  gSettings.meshes[1].label = "lab";
  gSettings.meshes[1].fingerprint = 0;  // added without a passphrase: no mesh id
  gSettings.meshes[1].enabled = true;
}
static void disablePrimary() {
  gSettings.meshes[0].enabled = false;
  gSettings.meshes[0].isDefault = false;
}
#if ENABLE_BONDED_MODE
static void bondOnPrimary() {
  gSettings.bondModeEnabledMesh[0] = true;
  gSettings.bondRoleMesh[0] = 2;
}
#endif
static bool primaryAsMade() {
  const Settings::MeshIdentity& m0 = gSettings.meshes[0];
  return same(m0.label, "primary") && m0.enabled && m0.isDefault && m0.fingerprint == 0;  // no passphrase: no id
}

static bool slot0Empty() {
  const Settings::MeshIdentity& m0 = gSettings.meshes[0];
  return m0.label.length() == 0 && m0.fingerprint == 0 && !m0.enabled && !m0.isDefault;
}

int main() {
  // S1: a new device whose start fails keeps no mesh.
  resetSettings();
  startAttempt(false);
  CHECK(slot0Empty(), "S1 a failed start left slot 0 as '%s' (enabled=%d default=%d)",
        gSettings.meshes[0].label.c_str(), (int)gSettings.meshes[0].enabled, (int)gSettings.meshes[0].isDefault);
  for (uint8_t i = 1; i < Settings::N_MESHES; ++i) {
    CHECK(gSettings.meshes[i].label.length() == 0 && !gSettings.meshes[i].enabled, "S1 slot %u changed", (unsigned)i);
  }

  // S2: a new device whose start succeeds gets the enabled default "primary".
  resetSettings();
  startAttempt(true);
  CHECK(same(gSettings.meshes[0].label, "primary") && gSettings.meshes[0].enabled && gSettings.meshes[0].isDefault,
        "S2 slot 0 after a good start: '%s'", gSettings.meshes[0].label.c_str());
  CHECK(gSettings.meshes[0].fingerprint == 0, "S2 a mesh without a passphrase got mesh id 0x%04X (FAILS ON BASE: CRC of the label)",
        gSettings.meshes[0].fingerprint);

  // S3: failed starts, then a good one: "primary" appears once, as the default.
  resetSettings();
  startAttempt(false);
  startAttempt(false);
  CHECK(slot0Empty(), "S3 two failed starts left slot 0 as '%s'", gSettings.meshes[0].label.c_str());
  startAttempt(true);
  CHECK(same(gSettings.meshes[0].label, "primary") && gSettings.meshes[0].isDefault,
        "S3 the good start after failures did not create primary");

  // S4: a configured slot 0 is left alone by a failed start (its stored mesh
  // id stays; only its key could change it).
  resetSettings();
  gSettings.meshes[0].label = "work";
  gSettings.meshes[0].fingerprint = 0x1234;
  gSettings.meshes[0].passphrase = "pw-work";
  gSettings.meshes[0].enabled = true;
  gSettings.meshes[0].isDefault = true;
  startAttempt(false);
  CHECK(same(gSettings.meshes[0].label, "work") && same(gSettings.meshes[0].passphrase, "pw-work") &&
        gSettings.meshes[0].enabled && gSettings.meshes[0].isDefault, "S4 a configured slot 0 was changed");
  CHECK(gSettings.meshes[0].fingerprint == 0x1234, "S4 mesh id 0x%04X", gSettings.meshes[0].fingerprint);

  // S5: slot 0 emptied with another mesh in use: no start creates "primary".
  resetSettings();
  gSettings.meshes[2].label = "bench";
  gSettings.meshes[2].fingerprint = 0x2345;
  gSettings.meshes[2].enabled = true;
  gSettings.meshes[2].isDefault = true;
  startAttempt(false);
  CHECK(slot0Empty(), "S5 failed start: slot 0 is '%s'", gSettings.meshes[0].label.c_str());
  startAttempt(true);
  CHECK(slot0Empty(), "S5 good start: slot 0 is '%s'", gSettings.meshes[0].label.c_str());
  CHECK(same(gSettings.meshes[2].label, "bench") && gSettings.meshes[2].isDefault &&
        gSettings.meshes[2].fingerprint == 0x2345, "S5 slot 2 changed");

  // S6: an unlabelled slot 0 that already holds a passphrase keeps it, and a
  // failed start puts its flags back.
  resetSettings();
  gSettings.meshes[0].passphrase = "pw-early";
  startAttempt(false);
  CHECK(slot0Empty() && same(gSettings.meshes[0].passphrase, "pw-early"),
        "S6 failed start: slot 0 '%s' passphrase kept=%d", gSettings.meshes[0].label.c_str(),
        (int)same(gSettings.meshes[0].passphrase, "pw-early"));
  startAttempt(true);
  CHECK(same(gSettings.meshes[0].label, "primary") && same(gSettings.meshes[0].passphrase, "pw-early"),
        "S6 good start: slot 0 '%s'", gSettings.meshes[0].label.c_str());

  // S8: a key stretched during the start for slot 0's passphrase can only come
  // from a command (the start stretches nothing), so slot 0 then stays as it is.
  // A key that was there before the start is kept by the undo.
  resetSettings();
  gSettings.meshes[0].passphrase = "pw-early";
  startAttempt(false, true);
  CHECK(same(gSettings.meshes[0].label, "primary") && gSettings.meshes[0].passphraseStretchedKeyValid &&
            gSettings.meshes[0].passphraseStretchedKey[0] == 0xAB,
        "S8 a key stretched by a command during the start was undone");
  resetSettings();
  gSettings.meshes[0].passphrase = "pw-early";
  gSettings.meshes[0].passphraseStretchedKey[0] = 0xCD;
  gSettings.meshes[0].passphraseStretchedKeyValid = true;
  startAttempt(false);
  CHECK(slot0Empty() && gSettings.meshes[0].passphraseStretchedKeyValid &&
            gSettings.meshes[0].passphraseStretchedKey[0] == 0xCD,
        "S8 failed start dropped a key it did not make, or kept primary");
  resetSettings();
  gSettings.meshes[0].passphrase = "pw-early";
  startAttempt(true, true);
  CHECK(gSettings.meshes[0].passphraseStretchedKeyValid && gSettings.meshes[0].passphraseStretchedKey[0] == 0xAB,
        "S8 good start dropped its key");

  // S9: an undo writes the settings, so a file another task wrote during the
  // start (with "primary" in it) does not bring "primary" back at the next boot.
  // Nothing is written when there is nothing to undo.
  resetSettings();
  startAttempt(false);
  CHECK(slot0Empty() && gPersists == 1, "S9 a failed start undid slot 0 without writing the settings (%d writes)",
        gPersists);
  resetSettings();
  startAttempt(true);
  CHECK(gPersists == 0, "S9 a good start wrote the settings (%d writes)", gPersists);
  resetSettings();
  gSettings.meshes[0].label = "work";
  gSettings.meshes[0].enabled = true;
  gSettings.meshes[0].isDefault = true;
  startAttempt(false);
  CHECK(gPersists == 0, "S9 a failed start with a configured slot 0 wrote the settings (%d writes)", gPersists);

  // S10: a command that changed the meshes during a failed start is kept; the
  // undo leaves slot 0 alone and writes nothing.
  resetSettings();
  startAttempt(false, false, renamePrimaryToHome);
  CHECK(same(gSettings.meshes[0].label, "home") && gSettings.meshes[0].enabled && gSettings.meshes[0].isDefault &&
            gSettings.meshes[0].fingerprint == 0 && gPersists == 0,
        "S10 a rename during the start was undone: slot 0 '%s' enabled=%d default=%d",
        gSettings.meshes[0].label.c_str(), (int)gSettings.meshes[0].enabled, (int)gSettings.meshes[0].isDefault);
  resetSettings();
  startAttempt(false, false, setPrimaryPassphrase);
  CHECK(primaryAsMade() && same(gSettings.meshes[0].passphrase, "pw-during") &&
            gSettings.meshes[0].passphraseStretchedKeyValid && gPersists == 0,
        "S10 a passphrase set during the start was left on an unlabelled slot 0 ('%s')",
        gSettings.meshes[0].label.c_str());
  resetSettings();
  startAttempt(false, false, setPrimaryPassphraseStretchFailed);
  CHECK(primaryAsMade() && same(gSettings.meshes[0].passphrase, "pw-nokey") &&
            !gSettings.meshes[0].passphraseStretchedKeyValid && gPersists == 0,
        "S10 a passphrase set during the start (key stretch failed) was left on an unlabelled slot 0 ('%s')",
        gSettings.meshes[0].label.c_str());
  resetSettings();
  startAttempt(false, false, addLabInSlot1);
  CHECK(primaryAsMade() && same(gSettings.meshes[1].label, "lab") && gPersists == 0,
        "S10 a mesh added during the start was left without a default (slot 0 '%s')",
        gSettings.meshes[0].label.c_str());
  resetSettings();
  startAttempt(false, false, disablePrimary);
  CHECK(same(gSettings.meshes[0].label, "primary") && !gSettings.meshes[0].enabled &&
            !gSettings.meshes[0].isDefault && gPersists == 0,
        "S10 'remove primary' during the start was undone");
#if ENABLE_BONDED_MODE
  resetSettings();
  startAttempt(false, false, bondOnPrimary);
  CHECK(primaryAsMade() && gSettings.bondModeEnabledMesh[0] && gSettings.bondRoleMesh[0] == 2 && gPersists == 0,
        "S10 a bond change on slot 0 during the start was undone");
#endif

#if ENABLE_BONDED_MODE
  // S7: the bootstrap copies the legacy bond fields into slot 0; a failed start
  // puts slot 0's bond fields back.
  resetSettings();
  gSettings.bondModeEnabled = true;
  gSettings.bondRole = 1;
  gSettings.bondPeerMac = "peer-text";
  startAttempt(false);
  CHECK(!gSettings.bondModeEnabledMesh[0] && gSettings.bondRoleMesh[0] == 0 &&
        gSettings.bondPeerMacMesh[0].length() == 0, "S7 failed start left slot 0's bond fields set");
  CHECK(gSettings.bondModeEnabled && gSettings.bondRole == 1 && same(gSettings.bondPeerMac, "peer-text"),
        "S7 the legacy bond fields changed");
  startAttempt(true);
  CHECK(gSettings.bondModeEnabledMesh[0] && gSettings.bondRoleMesh[0] == 1 &&
        same(gSettings.bondPeerMacMesh[0], "peer-text"), "S7 good start did not copy the bond fields");
#endif

  std::printf("espnow_start_order (ENABLE_BONDED_MODE=%d): %d checks, %d failed\n",
              ENABLE_BONDED_MODE, gChecks, gFailures);
  return gFailures ? 1 : 0;
}
