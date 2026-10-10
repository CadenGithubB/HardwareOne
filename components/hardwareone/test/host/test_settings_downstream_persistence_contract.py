#!/usr/bin/env python3
"""Source-level guards for settings mutations that cross async/module seams."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def source(name: str) -> str:
    return (ROOT / name).read_text(encoding="utf-8")


def between(text: str, start: str, end: str) -> str:
    begin = text.find(start)
    assert begin >= 0, f"missing start marker: {start}"
    finish = text.find(end, begin + len(start))
    assert finish >= 0, f"missing end marker after {start}: {end}"
    return text[begin:finish]


camera = source("G2_Page_CameraSettings.cpp")
assert "static void writeSetting(" not in camera
assert "g2SubmitHijackCommand(line, cookie, onCameraSettingDone" in camera
top_taps = between(camera, "static void handleTopTap(", "static void handleSubTap(")
assert "gSettings.cameraStreamFps =" not in top_taps
sub_taps = between(camera, "static void handleSubTap(", "static void handleResolutionPickerTap(")
assert "s.apply(next)" in sub_taps
stream_taps = between(camera, "static void handleStreamPickerTap(", "void g2CameraSettingsHandleTap(")
assert "gSettings.g2StreamWidth  =" not in stream_taps
assert "gSettings.g2StreamHeight =" not in stream_taps
assert "onCameraSettingDone" in stream_taps

espnow = source("System_ESPNow.cpp")
primary_setter = between(espnow, "static void setEspNowPassphrase(",
                         "// ============================================================================\n// BOND MODE")
assert "setSetting(" not in primary_setter
passphrase = between(espnow, "static const char* meshesCmd_setpassphrase(",
                     "static const char* meshesCmd_rename(")
assert "setSetting(gSettings.meshes[i].passphrase" not in passphrase
assert passphrase.rfind("requestSettingsPersist()") > passphrase.rfind("meshKeysInvalidate(i)")
rename = between(espnow, "static const char* meshesCmd_rename(",
                 "const char* cmd_espnow_meshes(")
assert "setSetting(gSettings.meshes[i].label" not in rename
assert rename.rfind("requestSettingsPersist()") > rename.rfind("meshKeysInvalidate(i)")
enable = between(espnow, "static const char* meshesCmd_enable(",
                 "static const char* meshesCmd_setdefault(")
assert "setSetting(gSettings.meshes[i].enabled" not in enable
assert enable.rfind("requestSettingsPersist()") > enable.rfind("fingerprint =")

legacy = source("System_SetupWizard.cpp")
legacy_apply = between(legacy, "SetupWizardResult runAndApplyFeatureWizard(", "\n}")
assert legacy_apply.count("wizardSaveAndApplySettings(") == 1
assert legacy_apply.find("if (!result.completed)") < legacy_apply.find("wizardSaveAndApplySettings(")

cli_mode = source("System_SetupWizardMode.cpp")
mode_exit = between(cli_mode, "static void wizardMode_onExit(",
                    "static void wizardMode_onTick(")
assert "if (sWizard.result.completed)" in mode_exit
assert mode_exit.count("wizardSaveAndApplySettings(") == 1
assert mode_exit.find("if (sWizard.result.completed)") < mode_exit.find("wizardSaveAndApplySettings(")
for adapter in (legacy_apply, mode_exit):
    assert "writeSettingsJson()" not in adapter
    assert "writeDebugJson()" not in adapter
completion = between(legacy, "void wizardSaveAndApplySettings(", "\n}")
assert completion.count("writeSettingsJson()") == 1
assert completion.count("writeDebugJson()") == 1
assert completion.find("if (!result.completed)") < completion.find("writeSettingsJson()")

ble = source("BLE_Peers.cpp")
owner_rollback = between(ble, "if (!ownerAvailable) {", "if (!config.applied) {")
assert "const PeerConfigApplyResult rollback = peerConfigApply(" in owner_rollback
assert "rollback.applied && rollback.policyChanged" in owner_rollback
assert "requestSettingsPersist()" in owner_rollback

sensor = source("System_SensorLogging.cpp")
autostart = between(sensor, "void sensorLogAutoStart()", "\n}")
assert "setSetting(gSettings.sensorLogPath, String(CAPTURE_HEALTHLOG_DEFAULT))" in autostart

print("settings downstream persistence contract: OK")
