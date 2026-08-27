#!/usr/bin/env python3
"""Source-contract checks for settings serialization and cache fencing."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : pos + 1]
    raise AssertionError(f"unterminated function: {signature}")


settings = (ROOT / "System_Settings.cpp").read_text()
user = (ROOT / "System_User.cpp").read_text()
notifications = (ROOT / "System_Notifications.cpp").read_text()

assert '#include "System_UserSettings.h"' in settings

transaction = function_body(settings, "UserSettingsTxnStatus runUserSettingsTransaction(")
outer_lock = transaction.index("FsLockGuard transactionGuard")
load = transaction.index("if (!loadUserSettings(")
mutate = transaction.index("mutator(base, context)")
save = transaction.index("if (!saveUserSettings(")
assert outer_lock < load < mutate < save
assert "isFsLockedByCurrentTask()" in transaction

load_settings = function_body(settings, "bool loadUserSettings(")
assert 'String("/littlefs") + path' in load_settings
assert "::stat(physicalPath.c_str(), &fileInfo)" in load_settings
assert "errno != ENOENT" in load_settings
assert load_settings.index("errno != ENOENT") < load_settings.index(
    "doc.to<JsonObject>()"
)
assert "if (!doc.is<JsonObject>())" in load_settings

save_body = function_body(settings, "bool saveUserSettings(")
assert save_body.count("measureJson(doc)") == 1
assert "UserPrefsInvalidateOnExit" in save_body
assert "written != expected || observed != expected" in save_body
assert "directWritten == expected && directObserved == expected" in save_body
assert save_body.index("written != expected || observed != expected") < save_body.index("renameGuarded")
assert "written > 0" not in save_body

merge = function_body(settings, "bool mergeAndSaveUserSettings(")
assert "runUserSettingsTransaction(" in merge
assert "loadUserSettings(" not in merge and "saveUserSettings(" not in merge

merge_mutator = function_body(settings, "bool mergeUserSettingsMutation(")
assert "if (!base.is<JsonObject>()) return false;" in merge_mutator
assert "base.to<JsonObject>()" not in merge_mutator

for signature in ("bool setUserPassword(", "bool setUserGamepadPassword("):
    body = function_body(user, signature)
    assert "runUserSettingsTransaction(" in body
    assert "loadUserSettings(" not in body and "saveUserSettings(" not in body
    assert body.index("hashed.length() == 0") < body.index("runUserSettingsTransaction(")
    assert "UserSettingsTxnStatus::LoadFailed" in body
    assert "SYSEVT_CONFIG_FILE_CORRUPT" in body

for signature in (
    "bool applyUserPasswordMutation(",
    "bool applyUserGamepadPasswordMutation(",
):
    body = function_body(user, signature)
    assert "if (!settings.is<JsonObject>()) return false;" in body
    assert "settings.to<JsonObject>()" not in body

invalidate = function_body(notifications, "void notifUserPrefsInvalidate(")
assert "++gUserPrefsCacheGeneration" in invalidate
resolve = function_body(notifications, "void notifViewerResolve(")
generation_capture = resolve.index("loadGeneration = gUserPrefsCacheGeneration")
flash_load = resolve.index("loadUserNotifPrefs(", generation_capture)
generation_check = resolve.index("loadGeneration != gUserPrefsCacheGeneration")
assert flash_load < generation_check
assert "continue;" in resolve[generation_check:]
assert resolve.rfind("xSemaphoreGive", 0, flash_load) != -1
assert resolve.find("xSemaphoreTake", flash_load, generation_check) != -1

print("user-settings transaction source guards passed")
