// Host harness for test_espnow_start_order.py.
//
// The runner pastes the production Settings::MeshIdentity record into the
// Settings double, and meshFingerprintForLabel, recomputeAllMeshFingerprints,
// initPrimaryMeshFromLegacySettings and (when the tree has it) the
// PrimaryMeshBootstrap guard at the production marker. ENABLE_BONDED_MODE comes
// from the command line (compiled once per value). A "start" here is what
// initEspNow does with slot 0: construct the guard, then keep it on success or
// drop it on failure. On a tree without the guard the start runs the bootstrap
// the way initEspNow did before the fix (and never undoes it).
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

// CRC16-CCITT (0xFFFF start), as System_ESPNow_Wire.
static uint16_t v4_crc16_ccitt(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; ++b) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

// INSERT_PRODUCTION

static void resetSettings() { gSettings = Settings(); }

static bool same(const String& a, const char* b) { return std::strcmp(a.c_str(), b) == 0; }

static uint16_t fp(const char* label) {
  return v4_crc16_ccitt(reinterpret_cast<const uint8_t*>(label), std::strlen(label));
}

// What a start may do to slot 0 between the bootstrap and its end: stretch
// the passphrase under the label slot 0 has then.
static void stretchSlot0() {
  gSettings.meshes[0].passphraseStretchedKey[0] = 0xAB;
  gSettings.meshes[0].passphraseStretchedKeyValid = true;
}

// One start attempt as initEspNow runs it for slot 0.
#if HW1_HAVE_BOOTSTRAP
static void startAttempt(bool succeeds, bool stretch = false) {
  PrimaryMeshBootstrap bootstrap;
  if (stretch) stretchSlot0();
  if (succeeds) bootstrap.keep();
}
#else
static void startAttempt(bool, bool stretch = false) {
  initPrimaryMeshFromLegacySettings();
  recomputeAllMeshFingerprints();
  if (stretch) stretchSlot0();
}
#endif

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
  CHECK(gSettings.meshes[0].fingerprint == fp("primary"), "S2 fingerprint 0x%04X", gSettings.meshes[0].fingerprint);

  // S3: failed starts, then a good one: "primary" appears once, as the default.
  resetSettings();
  startAttempt(false);
  startAttempt(false);
  CHECK(slot0Empty(), "S3 two failed starts left slot 0 as '%s'", gSettings.meshes[0].label.c_str());
  startAttempt(true);
  CHECK(same(gSettings.meshes[0].label, "primary") && gSettings.meshes[0].isDefault,
        "S3 the good start after failures did not create primary");

  // S4: a configured slot 0 is left alone by a failed start (its fingerprint is
  // only recomputed from its label).
  resetSettings();
  gSettings.meshes[0].label = "work";
  gSettings.meshes[0].passphrase = "pw-work";
  gSettings.meshes[0].enabled = true;
  gSettings.meshes[0].isDefault = true;
  startAttempt(false);
  CHECK(same(gSettings.meshes[0].label, "work") && same(gSettings.meshes[0].passphrase, "pw-work") &&
        gSettings.meshes[0].enabled && gSettings.meshes[0].isDefault, "S4 a configured slot 0 was changed");
  CHECK(gSettings.meshes[0].fingerprint == fp("work"), "S4 fingerprint 0x%04X", gSettings.meshes[0].fingerprint);

  // S5: slot 0 emptied with another mesh in use: no start creates "primary".
  resetSettings();
  gSettings.meshes[2].label = "bench";
  gSettings.meshes[2].enabled = true;
  gSettings.meshes[2].isDefault = true;
  startAttempt(false);
  CHECK(slot0Empty(), "S5 failed start: slot 0 is '%s'", gSettings.meshes[0].label.c_str());
  startAttempt(true);
  CHECK(slot0Empty(), "S5 good start: slot 0 is '%s'", gSettings.meshes[0].label.c_str());
  CHECK(same(gSettings.meshes[2].label, "bench") && gSettings.meshes[2].isDefault &&
        gSettings.meshes[2].fingerprint == fp("bench"), "S5 slot 2 changed");

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

  // S8: a key stretched during a failed start (salted with "primary") is
  // dropped; one that was there before the start is kept, and a good start
  // keeps the one it stretched.
  resetSettings();
  gSettings.meshes[0].passphrase = "pw-early";
  startAttempt(false, true);
  CHECK(slot0Empty() && !gSettings.meshes[0].passphraseStretchedKeyValid &&
        gSettings.meshes[0].passphraseStretchedKey[0] == 0, "S8 failed start kept a key salted with primary");
  gSettings.meshes[0].passphraseStretchedKey[0] = 0xCD;
  gSettings.meshes[0].passphraseStretchedKeyValid = true;
  startAttempt(false);
  CHECK(gSettings.meshes[0].passphraseStretchedKeyValid && gSettings.meshes[0].passphraseStretchedKey[0] == 0xCD,
        "S8 failed start dropped a key it did not make");
  resetSettings();
  gSettings.meshes[0].passphrase = "pw-early";
  startAttempt(true, true);
  CHECK(gSettings.meshes[0].passphraseStretchedKeyValid && gSettings.meshes[0].passphraseStretchedKey[0] == 0xAB,
        "S8 good start dropped its key");

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
