#pragma once

// The settings.json keys that hold the phone link's credential, one owner for
// the code that writes them and for factory reset, which must remove every
// one (2026-11-14 sweep, census-duplicates-13).
//
// Pure (no includes) and outside ENABLE_BLUETOOTH: factory reset clears these
// keys from the merged settings file even when Bluetooth is compiled out, so a
// later firmware cannot recover a pipe credential an earlier one saved.
//
// Char arrays, so ArduinoJson takes each key as the string literal it
// replaces. Renaming a key strands the value devices in the field already
// hold: keep the old name in the list (as the legacy keys below are) so
// factory reset still removes it.

namespace hw1pipekeys {

// network.bluetooth: the pipe secret and whether the device made it
// (bluetoothSettingsEntries in BLE_Server.cpp).
inline constexpr char kPhonePipeSecret[] = "phonePipeSecret";
inline constexpr char kPhonePipeSecretGenerated[] = "phonePipeSecretGenerated";

// network.bluetooth.peers.phone: the app-channel enrolment marker that
// blePeersWriteJson writes and blePeersReadJson checks (BLE_Peers.cpp).
inline constexpr char kPipeChannelVersion[] = "pipeChannelVersion";
inline constexpr char kPipeEnrolled[] = "pipeEnrolled";
// Enrolment keys earlier firmware wrote there; never written now, still removed.
inline constexpr char kPipeBondVersion[] = "pipeBondVersion";
inline constexpr char kPipeIdentity[] = "pipeIdentity";
inline constexpr char kPipeIdentityType[] = "pipeIdentityType";

// Every enrolment key a firmware has written under peers.phone. The writer
// drops all of them before it writes the current marker, and factory reset
// removes all of them: a key the writer adds belongs in this list.
inline constexpr const char* const kPhonePipeEnrollmentKeys[] = {
    kPipeChannelVersion, kPipeEnrolled, kPipeBondVersion, kPipeIdentity, kPipeIdentityType,
};

// The network.bluetooth keys factory reset clears with the enrolment.
inline constexpr const char* const kPhonePipeSecretKeys[] = {
    kPhonePipeSecret, kPhonePipeSecretGenerated,
};

constexpr bool isPhonePipeEnrollmentKey(const char* key) {
  for (const char* listed : kPhonePipeEnrollmentKeys) {
    if (listed == key) return true;
  }
  return false;
}
static_assert(isPhonePipeEnrollmentKey(kPipeChannelVersion) && isPhonePipeEnrollmentKey(kPipeEnrolled),
              "the keys blePeersWriteJson writes are ones factory reset removes");

}  // namespace hw1pipekeys
