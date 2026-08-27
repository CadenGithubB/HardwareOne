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
bool mergeAndSaveUserSettings(uint32_t userId, const JsonDocument& patch);
String getUserSettingsPath(uint32_t userId);

#endif
