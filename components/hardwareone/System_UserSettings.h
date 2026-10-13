#ifndef SYSTEM_USERSETTINGS_H
#define SYSTEM_USERSETTINGS_H

#include <Arduino.h>
#include <ArduinoJson.h>

enum class UserSettingsTxnStatus : uint8_t {
  Committed = 0,
  InvalidArgument,
  FilesystemUnavailable,
  LockUnavailable,
  LoadFailed,
  MutationRejected,
  SaveFailed,
};

// Runs synchronously while the per-user settings transaction owns the
// filesystem lock. The callback must not retain `doc` or `context` after it
// returns. Returning false rejects the mutation and performs no save.
using UserSettingsMutator = bool (*)(JsonDocument& doc, void* context);

bool loadUserSettings(uint32_t userId, JsonDocument& doc);
bool saveUserSettings(uint32_t userId, const JsonDocument& doc);
UserSettingsTxnStatus runUserSettingsTransaction(uint32_t userId,
                                                 UserSettingsMutator mutator,
                                                 void* context);
// Merges a preference patch into the user's file. Only the keys on the
// user-editable list are accepted; a patch naming any other key (including
// password, gamepad_password and mustChangePassword, whatever the value, even
// a JSON null) is refused whole and nothing is written.
bool mergeAndSaveUserSettings(uint32_t userId, const JsonDocument& patch);
// The first key of `patch` that mergeAndSaveUserSettings refuses, "" when the
// patch is not an object, or nullptr when every key is user-editable. The
// pointer refers into `patch`.
const char* userSettingsPatchRefusedKey(const JsonDocument& patch);
String getUserSettingsPath(uint32_t userId);

#endif
