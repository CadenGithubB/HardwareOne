#!/usr/bin/env python3
"""A guest's GET /api/settings and MQTT page leave out the private settings.

Owner, guest privacy 4 (2026-11-22): a guest gets a reduced settings view, with
no saved Wi-Fi networks (their SSIDs) and no joined network name, no MQTT broker host or user name, and no
LLM gateway address; users and administrators are unchanged.

The production guestview::reduceSettingsForGuest (WebServer_GuestSettingsView.h)
is compiled with the vendored ArduinoJson over a settings object laid out as
buildSettingsJsonDoc writes it, and checked:

- network.wifi.networks and wifiPrimarySSID are gone; the MQTT host and user name and the LLM
  gateway URL read "(not shown to guests)"; every other value (port, topics,
  the gateway's model, the current network flags, LLM tuning) is unchanged;
- an empty value stays empty (nothing to hide), a build without a section
  (no LLM HTTP source, no MQTT) is fine, and a second pass changes nothing.

Source contract: the table's paths are the settings modules' own (the Wi-Fi
section constants, the MQTT module's section and broker group, the LLM
module's section and http group); handleSettingsGet applies the view for a
guest only; the MQTT page sends the host and user name through the guest-aware
cell, HTML-escaped, with the page handler saying whether the viewer is a guest.

--component points the suite at another copy of the production sources.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_web_batch_handlers import extract_block, require  # noqa: E402

DRIVER = r'''
#include "WebServer_GuestSettingsView.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static unsigned checks = 0;
static void checked(bool ok, const char* what) {
  ++checks;
  if (ok) return;
  std::fprintf(stderr, "check failed: %s\n", what);
  std::exit(1);
}
static bool textIs(JsonVariantConst v, const char* want) {
  const char* got = v.as<const char*>();
  return got && std::strcmp(got, want) == 0;
}

static const char kFull[] = R"({
  "firmwareVersion": "1.0",
  "wifiPrimarySSID": "HomeNet",
  "network": {
    "wifi": {"wifiAutoStart": true, "wifiAutoReconnect": false,
             "networks": [{"ssid": "HomeNet", "priority": 1, "hidden": false},
                          {"ssid": "WorkNet", "priority": 2, "hidden": true}]},
    "mqtt": {"broker": {"mqttAutoStart": false, "mqttHost": "broker.lan", "mqttPort": 8883,
                        "mqttUser": "hauser", "mqttTLSMode": 1},
             "topics": {"mqttBaseTopic": "hw1", "mqttDiscoveryPrefix": "homeassistant"}},
    "espnow": {"espnowAutoStart": true}
  },
  "apps": {"llm": {"http": {"enabled": true, "url": "https://gw.lan:8443/v1",
                            "model": "openclaw/default", "allowPlaintext": false},
                   "temperature": 0.5, "defaultModel": "model.bin"}}
})";

int main() {
  const char* hidden = guestview::kGuestHiddenText;
  checked(std::strcmp(hidden, "(not shown to guests)") == 0, "the guest sentence");

  {
    JsonDocument doc;
    checked(!deserializeJson(doc, kFull), "parse the full settings");
    JsonObject s = doc.as<JsonObject>();
    checked(guestview::reduceSettingsForGuest(s) == 5, "five values hidden");
    checked(s["network"]["wifi"]["networks"].isNull(), "saved networks dropped");
    checked(s["network"]["wifi"]["wifiAutoStart"] == true, "wifi flags kept");
    checked(textIs(s["network"]["mqtt"]["broker"]["mqttHost"], hidden), "MQTT host hidden");
    checked(textIs(s["network"]["mqtt"]["broker"]["mqttUser"], hidden), "MQTT user hidden");
    checked(s["network"]["mqtt"]["broker"]["mqttPort"] == 8883, "MQTT port kept");
    checked(s["network"]["mqtt"]["broker"]["mqttTLSMode"] == 1, "MQTT TLS mode kept");
    checked(textIs(s["network"]["mqtt"]["topics"]["mqttBaseTopic"], "hw1"), "MQTT topic kept");
    checked(textIs(s["apps"]["llm"]["http"]["url"], hidden), "LLM gateway URL hidden");
    checked(textIs(s["apps"]["llm"]["http"]["model"], "openclaw/default"), "LLM model kept");
    checked(s["apps"]["llm"]["http"]["enabled"] == true, "LLM enabled kept");
    checked(s["apps"]["llm"]["temperature"] == 0.5, "LLM tuning kept");
    checked(s["wifiPrimarySSID"].isNull(), "the joined network name dropped");
    checked(s["network"]["espnow"]["espnowAutoStart"] == true, "other sections kept");
    // A second pass finds nothing new to drop; the masks are already the sentence.
    checked(guestview::reduceSettingsForGuest(s) == 3, "a second pass re-masks only");
    checked(textIs(s["network"]["mqtt"]["broker"]["mqttHost"], hidden), "still hidden");
    std::string out;
    serializeJson(doc, out);
    checked(out.find("broker.lan") == std::string::npos && out.find("HomeNet") == std::string::npos && out.find("hauser") == std::string::npos &&
                out.find("WorkNet") == std::string::npos && out.find("gw.lan") == std::string::npos,
            "no hidden text left in the serialized view");
  }
  {
    // Empty values stay empty; absent sections (a build without them) are fine.
    JsonDocument doc;
    checked(!deserializeJson(doc, R"({"network":{"wifi":{"networks":[]},
        "mqtt":{"broker":{"mqttHost":"","mqttPort":1883}}},"apps":{"llm":{"temperature":0.5}}})"),
            "parse the sparse settings");
    JsonObject s = doc.as<JsonObject>();
    checked(guestview::reduceSettingsForGuest(s) == 1, "only the (empty) network list goes");
    checked(textIs(s["network"]["mqtt"]["broker"]["mqttHost"], ""), "an empty host stays empty");
    checked(s["network"]["mqtt"]["broker"]["mqttUser"].isNull(), "an absent user stays absent");
    checked(s["apps"]["llm"]["http"].isNull(), "no http group is made");
    checked(s["network"]["wifi"]["networks"].isNull(), "the empty list is dropped too");
  }
  {
    JsonDocument doc;
    doc["firmwareVersion"] = "1.0";
    checked(guestview::reduceSettingsForGuest(doc.as<JsonObject>()) == 0, "no sections");
    checked(guestview::reduceSettingsForGuest(JsonObject()) == 0, "a null object");
  }
  std::printf("%u checks\n", checks);
  return 0;
}
'''


def check_sources(component: Path) -> None:
    view = (component / "WebServer_GuestSettingsView.h").read_text()
    paths = re.findall(r'\{"([a-zA-Z.]+)", GuestHide::(Mask|Drop)\}', view)
    require(dict(paths) == {
        "wifiPrimarySSID": "Drop",
        "network.wifi.networks": "Drop",
        "network.mqtt.broker.mqttHost": "Mask",
        "network.mqtt.broker.mqttUser": "Mask",
        "apps.llm.http.url": "Mask",
    }, f"the hidden-settings table: {paths}")

    wifi = (component / "network" / "wifi" / "System_WiFiNetworks.h").read_text()
    for name, value in (("kParent", "network"), ("kSection", "wifi"), ("kNetworks", "networks")):
        require(f'inline constexpr char {name}[] = "{value}";' in wifi,
                f"the saved networks live at network.wifi.networks ({name})")
    settings = (component / "System_Settings.cpp").read_text()
    require('doc["wifiPrimarySSID"] = cur;' in settings,
            "the joined network name is the settings root's wifiPrimarySSID")

    mqtt = (component / "System_MQTT.cpp").read_text()
    require('"mqtt",\n  "network.mqtt",' in mqtt, "the MQTT module's section is network.mqtt")
    for key in ("mqttHost", "mqttUser"):
        row = re.search(r'\{ "' + key + r'", SETTING_STRING,[^\n]*\}', mqtt)
        require(bool(row) and '"broker"' in row.group(0), f"{key} is in the broker group")

    llm = (component / "System_LLMCommands.cpp").read_text()
    require('"llm", "apps.llm", llmSettingEntries' in llm, "the LLM module's section is apps.llm")
    row = re.search(r'\{ "url", SETTING_STRING,[^\n]*\}', llm)
    require(bool(row) and '"http"' in row.group(0), "the gateway url is in the http group")

    server = (component / "WebServer_Server.cpp").read_text()
    handler = extract_block(server, "esp_err_t handleSettingsGet(")
    require("if (ctxIsGuest) guestview::reduceSettingsForGuest(settings);" in handler,
            "handleSettingsGet applies the view for a guest")
    require(handler.count("reduceSettingsForGuest") == 1, "and nowhere else in it")
    require("const bool ctxIsGuest = (ctx.user.length() > 0 && ctxRank == kRoleRankGuest);"
            in handler, "the guest test is the role rank the reply reports")

    page = (component / "WebPage_MQTT.cpp").read_text()
    inner = extract_block(page, "void streamMqttInner(")
    require("streamMqttConfigText(req, gSettings.mqttHost, guestView," in inner and
            "streamMqttConfigText(req, gSettings.mqttUser, guestView," in inner,
            "the MQTT page sends the host and user through the guest-aware cell")
    require(not re.search(r"httpd_resp_send_chunk\([^;]*gSettings\.mqtt(Host|User)", inner),
            "the MQTT page sends no host or user name directly")
    cell = extract_block(page, "static void streamMqttConfigText(")
    require("guestview::kGuestHiddenText" in cell and
            "streamHtmlEscaped(req, value.c_str()" in cell,
            "the cell shows the guest sentence, or the value HTML-escaped")
    require("streamMqttInner(req, isGuestUser(ctx.user));" in page,
            "the MQTT page handler says whether the viewer is a guest")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("clang++")
                        or shutil.which("c++"))
    parser.add_argument("--component", type=Path, default=HERE.parents[1])
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    json_include = args.component.parent / "hardwareone_libs" / "ArduinoJson" / "src"
    require(json_include.is_dir(), f"vendored ArduinoJson at {json_include}")
    check_sources(args.component)
    with tempfile.TemporaryDirectory(prefix="hw1-web-guest-settings-") as directory:
        unit = Path(directory) / "guest_settings_view.cpp"
        binary = Path(directory) / "guest_settings_view"
        unit.write_text(DRIVER)
        compiled = subprocess.run(
            [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(args.component), "-isystem", str(json_include), str(unit), "-o", str(binary)],
            capture_output=True, text=True, timeout=240)
        if compiled.returncode != 0:
            print(compiled.stderr[-4000:], file=sys.stderr)
            raise SystemExit("web_guest_settings_view: compile failed")
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
        if run.returncode != 0:
            print(run.stdout[-2000:], (run.stderr or "").strip(), file=sys.stderr)
            raise SystemExit("web_guest_settings_view: FAILED")
    print(f"web_guest_settings_view: {run.stdout.strip()}; a guest's settings and MQTT page "
          "leave out the saved networks, the MQTT host and user, and the LLM gateway")


if __name__ == "__main__":
    main()
