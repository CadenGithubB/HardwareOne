#!/usr/bin/env python3
"""Guard saved-network integration above the executable owner/JSON tests.

These checks cover the adapters' load-readiness, failure and cancellation
boundaries. The owner tests execute list and serialization behavior separately.
"""

from pathlib import Path
import re

from test_web_batch_handlers import extract_block


COMPONENT = Path(__file__).resolve().parents[2]


def source(name: str) -> str:
    return (COMPONENT / name).read_text(encoding="utf-8")


settings = source("System_Settings.cpp")
builder = extract_block(settings, "bool buildSettingsJsonDoc(")
ready = extract_block(builder, "if (gSettingsLoadedOk)")
assert "if (!wifiNetworksWriteJson(doc, excludePasswords)) return false;" in ready
loader = extract_block(settings, "bool readSettingsJson(")
assert loader.index("selectDeviceKeyEpoch(doc)") < loader.index("wifiNetworksReadJson(doc)")
assert "gSecretLoadFailures += wifiNetworksReadJson(doc);" in loader
assert loader.index("wifiNetworksReadJson(doc)") < loader.index("gSettingsLoadedOk = true")

writer = extract_block(settings, "bool writeSettingsJson(")
failure = extract_block(writer, "if (!buildSettingsJsonDoc(")
assert "SYSEVT_SETTINGS_SAVE_FAILED" in failure
assert "pollResume();" in failure and "return false;" in failure
assert writer.index("if (!buildSettingsJsonDoc(") < writer.index("writeSettingsSnapshot(")

web = extract_block(source("WebServer_Server.cpp"), "esp_err_t handleSettingsGet(")
failure = extract_block(web, "if (!buildSettingsJsonDoc(")
assert "httpd_resp_send_500(req);" in failure and "return ESP_FAIL;" in failure
g2 = extract_block(source("G2_Page_Settings.cpp"), "static bool showModuleJsonViaTextWidget(")
assert "return false;" in extract_block(g2, "if (!buildSettingsJsonDoc(")
espnow = source("System_ESPNow.cpp")
generate = extract_block(espnow, "static String generateDeviceSettings() {")
assert "return String();" in extract_block(generate, "if (!buildSettingsJsonDoc(")
send = extract_block(espnow, "static void sendBondSettings(")
failure = extract_block(send, "if (settingsLen == 0)")
assert "sSettingsTransferInProgress = false;" in failure and "return;" in failure
assert send.index("if (settingsLen == 0)") < send.index('VFS::openGuarded(tempPath, "w"')

legacy = extract_block(source("System_SetupWizard.cpp"), "SetupWizardResult runAndApplyFeatureWizard(")
mode = extract_block(source("System_SetupWizardMode.cpp"), "static void wizardMode_onExit(")
assert legacy.index("if (!result.completed)") < legacy.index("wizardSaveAndApplySettings(")
assert "wizardSaveAndApplySettings(" in extract_block(mode, "if (sWizard.result.completed)")
for adapter in (legacy, mode):
    assert adapter.count("wizardSaveAndApplySettings(") == 1
    assert "saveSetupWiFiNetwork(" not in adapter
apply = extract_block(source("System_SetupWizard.cpp"), "bool wizardSaveAndApplySettings(")
assert apply.index("!result.completed") < apply.index("saveSetupWiFiNetwork(")
assert apply.count("saveSetupWiFiNetwork(") == 1
assert "wifiNetworkSaveResultMessage(wifiSaved)" in apply
accepted = extract_block(apply, "if (wifiSaved == WifiNetworkSaveResult::Accepted)")
assert 'broadcastOutput("WiFi network: "' in accepted
for consumer in (apply, legacy, mode):
    assert "wifiAutoStart" not in consumer
    for obsolete in ("upsertWiFiNetwork(", "sortWiFiByPriority(", "saveWiFiNetworks("):
        assert obsolete not in consumer

restore = extract_block(source("System_FirstTimeSetup.cpp"), "void firstTimeSetupIfNeeded(")
failure = extract_block(restore, "if (wifiSaved != WifiNetworkSaveResult::Accepted)")
assert "continue;" in failure and "setupWiFi();" not in failure
assert restore.index("saveSetupWiFiNetwork(") < restore.index("setupWiFi();")
assert restore.index(failure) < restore.index("setupWiFi();")

for name in ("System_Settings.cpp", "System_SetupWizard.cpp",
             "System_SetupWizardMode.cpp", "System_FirstTimeSetup.cpp"):
    assert "gWifiNetworks" not in source(name), name
    assert "gWifiNetworkCount" not in source(name), name

# Screens submit the ordinary authorized commands. Both removal forms require
# authentication; the stable-ID selector must reject a missing record before
# the handler reaches a connection attempt (the owner exercises stale IDs).
wifi = source("System_WiFi.cpp")
for command, handler in (("wifirm", "cmd_wifirm"), ("wifirmid", "cmd_wifirmid"),
                         ("openwifi", "cmd_wificonnect")):
    assert re.search(r'\{\s*"' + command + r'"\s*,\s*"[^"\n]*"\s*,\s*true\s*,\s*' + handler + r'\s*,', wifi)
connect = extract_block(wifi, "const char* cmd_wificonnect(")
saved_id = extract_block(connect, 'else if (firstArg == "--saved-id")')
assert "parseSavedNetworkId(a.arg(1))" in saved_id
assert 'if (a.count() != 2 || !id) return "Usage:' in saved_id
assert 'if (!wifiNetworkConnectionById(id, selected)) return "Error:' in saved_id
assert connect.index(saved_id) < connect.index("connected = connectToBestWiFiNetwork()")
assert "useSavedId ? connectWiFiTarget(selected, 35000, false)" in connect

print("saved WiFi consumer contracts: OK")
