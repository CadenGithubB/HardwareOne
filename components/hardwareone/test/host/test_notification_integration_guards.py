#!/usr/bin/env python3
"""Source-contract checks for production notification mutation adapters."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
notifications = (ROOT / "System_Notifications.cpp").read_text()
oled = (ROOT / "OLED_Utils.cpp").read_text()
settings = (ROOT / "System_Settings.cpp").read_text()

assert '#include "System_NotificationKindListCore.h"' in notifications
assert "notif_kind_core::prepare(" in notifications
assert "notif_kind_core::apply(" in notifications
assert "runUserSettingsTransaction(uid, applyNotifUserKindMutation" in notifications
personal_start = notifications.index("static const char* notifUserKindListCmd(")
personal_end = notifications.index("// Force-OFF:", personal_start)
personal_body = notifications[personal_start:personal_end]
assert "mergeAndSaveUserSettings(" not in personal_body
assert "std::string_view(token.data, token.length)" in notifications
assert "settings.remove(mutation->key)" in notifications
assert "mutation->coreResult.outputCount" in notifications

assert '#include "System_EventKindMask.h"' in notifications
assert "static inline bool maskTest" not in notifications
assert "static inline void maskSet" not in notifications
assert "eventKindMaskTest(gNotifOffMask" in notifications
assert "eventKindMaskSet(gNotifOffMask" in notifications

assert '#include "System_EventKindMask.h"' in oled
assert '#include "System_EventCatalog.h"' in oled
assert 'String("notifyusermute set ")' in oled
assert "ncToggleMute(const SystemEventCatalogKindInfo& kind)" in oled
assert "eventKindMaskTest(sNcViewer.muteMask, kindId)" in oled
assert "+ kind.name" in oled
for provider_call in (
    "systemEventCatalogFamilyCount",
    "systemEventCatalogFamilyAt",
    "systemEventCatalogFamilyKindAt",
):
    assert provider_call in oled
for obsolete in (
    "sNcKindNames",
    "sNcKindIds",
    "sNcKindCount",
    "ncBuildKindList",
):
    assert obsolete not in oled
assert "uint32_t mask[4]" not in oled
assert "ncMuteTest" not in oled
assert "cmd.reserve(2400)" not in oled

for command in ("notifyusermute", "notifyusershow"):
    command_start = settings.index(f'{{ "{command}"')
    command_help = settings[command_start : command_start + 650]
    assert "set <kind> <on|off>" in command_help
    assert "patch <+kind,-kind>" in command_help
    assert "all|none" in command_help

print("notification integration source guards passed")
