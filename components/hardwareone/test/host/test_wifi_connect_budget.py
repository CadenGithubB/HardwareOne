#!/usr/bin/env python3
"""Run the shipping Wi-Fi connect hunt against a scripted driver and clock.

connectToBestWifiNetwork, connectWifiId, connectWifiTarget and the attempt
function come from System_WiFi.cpp unchanged, with the hunt's bounds helpers
and the boot budget derivation when the tree has them. The driver (status
sequence per saved network), millis()/delay() (a virtual clock), the serial
console and the command context are doubles. On a tree without the bounded
hunt the runner compiles the old entry points behind the same adapters, so the
budget, grace and console-origin checks fail behaviourally rather than at
compile time. The radio-ownership cases run the connect with ESP-NOW running
and with a timed-out ESP-NOW close still pending (System_ESPNow's
espnowOwnsRadio): neither may stop the driver or drop AP_STA beneath it.
--component replays the suite against another tree.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

FORWARD_DECLARATIONS = re.compile(
    r"static bool connectWifi(?:TargetWithin|Target|Id)\([^;{]*\);")


def generate(component: Path = COMPONENT) -> str:
    source = (component / "System_WiFi.cpp").read_text()
    header = (component / "System_WiFi.h").read_text()
    bounded = "WifiConnectOrigin" in header and "struct WifiConnectBounds" in source
    # The attempt helpers sit after the global-flag definitions; slicing there
    # keeps the forward declarations near the top from matching their markers.
    tail = source[source.index("volatile bool gWifiUserCancelled = false;"):]

    support = []
    if bounded:
        support.append(extract_block(header, "enum class WifiConnectOrigin") + ";")
        support.append(re.search(r"^constexpr uint32_t kWifiConnectHuntBudgetMs = \d+;$",
                                 header, re.MULTILINE).group())
        support.append(extract_block(source, "struct WifiConnectBounds") + ";")
        support.extend(re.findall(r"^static constexpr uint32_t kWifi\w+Ms = \d+;$", source, re.MULTILINE))
        support.append(extract_block(source, "static bool wifiConsoleCancelApplies("))
        support.append(extract_block(source, "static WifiConnectBounds wifiConnectBounds("))
        # Only Ctrl-C/Esc cancel; other console bytes are kept for the CLI.
        cancel_keys = "struct WifiConsoleTypeahead" in source
        if cancel_keys:
            support.append(extract_block(source, "struct WifiConsoleTypeahead") + ";")
            support.append("static WifiConsoleTypeahead sWifiConsoleTypeahead;")
            support.append(extract_block(source, "bool wifiConsoleTypeaheadPop("))
            support.append(extract_block(source, "static bool wifiConsoleCancelKeyPressed("))
        else:
            support.append("bool wifiConsoleTypeaheadPop(char&) { return false; }")
        # The escape-sequence checks call the cancel-key reader directly.
        support.append(f"#define HW1_WIFI_CANCEL_KEYS_API {1 if cancel_keys else 0}")
        if cancel_keys and "kWifiConsoleEscapeWaitMs" not in source:
            support.append("static constexpr uint32_t kWifiConsoleEscapeWaitMs = 50;")
        support.append(extract_block(source, "uint32_t wifiBootConnectBudgetMs("))
        support.append("#define HW1_WIFI_CONNECT_BUDGET_API 1")
        # The superseding radio command's side of a BACKGROUND boot connect.
        supersede = "static bool wifiConnectSuperseded(" in source
        if supersede:
            support.extend(re.findall(r"^static std::atomic<bool> sBackgroundConnect\w+\{false\};$",
                                      source, re.MULTILINE))
            support.append(extract_block(source, "static bool wifiConnectSuperseded("))
            support.append(extract_block(source, "static bool wifiConnectWait("))
            support.append(extract_block(source, "void wifiBackgroundConnectReserve("))
            support.append(extract_block(source, "bool wifiBackgroundConnectActive("))
            support.append(extract_block(source, "bool wifiBackgroundConnectSuperseded("))
            support.append(extract_block(source, "bool wifiSupersedeBackgroundConnect("))
        support.append(f"#define HW1_WIFI_SUPERSEDE_API {1 if supersede else 0}")
        support.append(f"#define HW1_WIFI_BOOT_JOIN_API {1 if 'sBackgroundConnectIsBoot' in source else 0}")
        support.append("bool connectToBestWifiNetwork(uint32_t budgetMs, WifiConnectOrigin origin);")
        support.append("static bool huntBest(uint32_t budgetMs, WifiConnectOrigin origin) "
                       "{ return connectToBestWifiNetwork(budgetMs, origin); }")
    else:
        support.append("bool wifiConsoleTypeaheadPop(char&) { return false; }")
        support.append("enum class WifiConnectOrigin : uint8_t { COMMAND = 0, BOOT };")
        support.append("constexpr uint32_t kWifiConnectHuntBudgetMs = 150000;")
        support.append("#define HW1_WIFI_CONNECT_BUDGET_API 0")
        support.append("#define HW1_WIFI_CANCEL_KEYS_API 0")
        support.append("#define HW1_WIFI_BOOT_JOIN_API 0")
        support.append("#define HW1_WIFI_SUPERSEDE_API 0")
        support.append("bool connectToBestWifiNetwork();")
        support.append("static bool huntBest(uint32_t, WifiConnectOrigin) "
                       "{ return connectToBestWifiNetwork(); }")
    # The coexistence mirror counts every attempt as "connecting" while it runs.
    # Only the records-on branch: the records-off branch keeps its own depth counter.
    coex_start = source.index("#if ENABLE_BLE_COEX_RECORDS\nnamespace {\n")
    coex_state = source[coex_start:source.index("#else", coex_start)]
    support.extend(re.findall(r"^std::atomic<\w+> sCoex\w+\{.*;$", coex_state, re.MULTILINE))
    support.append(extract_block(source, "struct CoexConnectScope") + ";")
    declarations = FORWARD_DECLARATIONS.findall(source)
    require(len(declarations) >= 2, "System_WiFi.cpp forward-declares the connect helpers")
    support.extend(declarations)

    hunt = extract_block(source, "bool connectToBestWifiNetwork(")
    attempt = [extract_block(tail, "static const char* wifiStatusToString("),
               extract_block(tail, "static void wifiEnsureIdle("),
               extract_block(tail, "static bool connectWifiId("),
               extract_block(tail, "static bool connectWifiTarget(const WifiConnectionTarget& nw")]
    if bounded:
        attempt.append(extract_block(tail, "static bool connectWifiTargetWithin(const WifiConnectionTarget& nw"))
    if bounded and "static bool wifiConnectSuperseded(" in source:
        # The boot connect's entry point owns the background connect's lifetime.
        attempt.append(extract_block(source, "void setupWifi(uint32_t budgetMs, WifiConnectOrigin origin)"))

    harness = (HERE / "wifi_connect_budget_harness.cpp").read_text()
    for marker in ("// INSERT_PRODUCTION_SUPPORT", "// INSERT_PRODUCTION_HUNT", "// INSERT_PRODUCTION_ATTEMPT"):
        require(harness.count(marker) == 1, f"one {marker} marker")
    return (harness.replace("// INSERT_PRODUCTION_SUPPORT", "\n\n".join(support))
                   .replace("// INSERT_PRODUCTION_HUNT", hunt)
                   .replace("// INSERT_PRODUCTION_ATTEMPT", "\n\n".join(attempt)))


def check_boot_callers(component: Path = COMPONENT) -> None:
    """Connects that run on the boot task pass the BOOT origin, so a console
    keystroke can still skip them; command-context resolution would find no
    context there and leave the console unread. The boot connect that runs on
    its own task passes BACKGROUND instead, and the radio commands that can
    arrive while it hunts supersede it first."""
    boot = (component / "HardwareOne.cpp").read_text()
    require("setupWifi(wifiBootConnectBudgetMs(), WifiConnectOrigin::BOOT);" in boot,
            "the boot connect passes the trial-aware budget and the BOOT origin")
    wifi = (component / "System_WiFi.cpp").read_text()
    if "bool wifiSupersedeBackgroundConnect(" in wifi:
        require("setupWifi(wifiBootConnectBudgetMs(), WifiConnectOrigin::BACKGROUND);" in boot,
                "the background boot connect passes the trial-aware budget and the BACKGROUND origin")
        for handler in ("const char* cmd_wificonnect(", "const char* cmd_closewifi(",
                        "const char* cmd_wifidisconnect(", "const char* cmd_radiopower("):
            body = extract_block(wifi, handler)
            name = handler.split('(')[0].split()[-1]
            require("if (!wifiSupersedeBackgroundConnect())" in body,
                    f"{name} supersedes a background boot connect and reports a timeout")
        # Ordering the plain presence check above cannot see.
        power = extract_block(wifi, "const char* cmd_radiopower(")
        require(power.index("wifiSupersedeBackgroundConnect()") < power.index('"Error: The radio is already off. Turn it on with \'radiopower on\'."'),
                "radiopower off stops the boot connect before deciding the radio is already off")
        connect = extract_block(wifi, "const char* cmd_wificonnect(")
        require(connect.index('"Usage: openwifi [--best') < connect.index("wifiSupersedeBackgroundConnect()"),
                "openwifi checks its arguments before cancelling the boot connect")
        companion = (component / "System_RadioCompanion.cpp").read_text()
        require("stopBootWifiConnect();" in extract_block(companion, "void stopUsers(")
                and "stopBootWifiConnect();" in extract_block(companion, "void updateProc("),
                "the radio companion stops the boot connect before it stops or reuses the transport")
        espnow = (component / "System_ESPNow.cpp").read_text()
        require("wifiBackgroundConnectActive()" in extract_block(espnow, "const char* cmd_espnow_init("),
                "openespnow waits for the boot connect instead of starting beneath it")
        require("wifiBackgroundConnectReserve(true);" in boot,
                "the boot task's connect is reserved before the task is created")
    setup = (component / "System_FirstTimeSetup.cpp").read_text()
    require(setup.count("WifiConnectOrigin::BOOT") == 2,
            "both first-time-setup connects (restore and final) pass the BOOT origin")
    require("    setupWifi();\n" not in setup and "    connectToBestWifiNetwork();  //" not in setup,
            "no first-time-setup connect falls back to the command origin")


def check_rehunt_and_services(component: Path = COMPONENT) -> None:
    """A failed setupWifi connect arms the rehunt (only the plain failure, not a
    supersede or a console cancel), and every join that boot does not own runs
    the network-services job on cmd_exec (NTP, HTTP autostart)."""
    wifi = (component / "System_WiFi.cpp").read_text()
    setup = extract_block(wifi, "void setupWifi(uint32_t budgetMs, WifiConnectOrigin origin)")
    superseded_at = setup.index("if (!connected && origin == WifiConnectOrigin::BACKGROUND &&")
    cancelled_at = setup.index("} else if (!connected && gWifiUserCancelled) {", superseded_at)
    failed_at = setup.index("} else if (!connected) {", cancelled_at)
    joined_at = setup.index("} else {", failed_at)
    note = "wifiRehuntNoteConnectFailed();"
    require(note not in setup[:superseded_at], "setupWifi arms the rehunt only after the connect")
    require(note not in setup[superseded_at:cancelled_at], "a superseded boot connect does not arm the rehunt")
    require(note not in setup[cancelled_at:failed_at], "a console-cancelled connect does not arm the rehunt")
    failed = setup[failed_at:joined_at]
    require(failed.count(note) == 1, "setupWifi's failure branch arms the rehunt")
    require("if (savedCount > 0 && gSettings.wifiEnabled && gSettings.wifiAutoReconnect)" in failed,
            "only with saved networks, Wi-Fi enabled and auto-reconnect on")
    require(note not in setup[joined_at:], "a join does not arm the rehunt")
    noter = extract_block(wifi, "static void wifiRehuntNoteConnectFailed(")
    require("portENTER_CRITICAL(&sRehuntMux);" in noter and "sRehunt.noteConnectFailed(millis());" in noter,
            "the connect-failed note goes to the policy under the rehunt lock")

    logger = extract_block(wifi, "static void wifiEventLogger(")
    got_ip = logger[:logger.index("ARDUINO_EVENT_WIFI_STA_DISCONNECTED")]
    require("sBackgroundConnectIsBoot.load(" in got_ip and "sWifiConnByBoot = byBoot;" in got_ip,
            "the GOT_IP edge records whether the boot connect made the join")
    drain = extract_block(wifi, "void wifiEventLogDrain()")
    conn = extract_block(drain, "if (conn) {")
    require("if (!byBoot && !submitDeferredToCmdExec(wifiReconcileNetworkServicesJob, nullptr))" in conn,
            "the GOT_IP drain submits the network-services job for every join boot does not own")
    job = extract_block(wifi, "static void wifiReconcileNetworkServicesJob(")
    require(job.index("if (!WiFi.isConnected()) return;") < job.index("setupNtp();"),
            "the job does nothing once the link is gone")
    require("if (!getNtpRuntimeStatus().clientArmed) setupNtp();" in job, "the job arms NTP when nothing has")
    for needle, what in (("in.httpEnabled = gSettings.httpEnabled;", "httpenabled"),
                         ("in.autostart = ramFlushResolve(RF_HTTP, gSettings.httpAutoStart);", "autostart as boot resolves it"),
                         ("in.running = isHttpServerRunning();", "a running server"),
                         ("in.userClosed = sHttpUserClosed;", "closehttp"),
                         ("in.bootConnectActive = sBackgroundConnectIsBoot.load(", "the boot connect"),
                         ("if (!wifiServicesShouldStartHttp(in)) return;", "the services policy")):
        require(needle in job, f"the HTTP decision reads {what}")
    require(job.index("if (!wifiServicesShouldStartHttp(in)) return;") < job.index("startHttpServer();"),
            "the server starts only on the policy's yes")
    require("HTTP server started. Try: http://" in job, "the job says where the server is")
    require("sHttpUserClosed = false;" in extract_block(wifi, "const char* cmd_httpstart("),
            "openhttp clears the user-closed latch")
    require("sHttpUserClosed = true;" in extract_block(wifi, "const char* cmd_httpstop("),
            "closehttp sets the user-closed latch")
    require("sHttpUserClosed" not in extract_block(wifi, "const char* cmd_closewifi("),
            "closewifi stopping the server is not a user close")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--keep", type=Path, help="write the generated unit here as well")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="components/hardwareone directory to read production sources from")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    check_boot_callers(args.component)
    check_rehunt_and_services(args.component)
    unit_text = generate(args.component)
    if args.keep:
        args.keep.write_text(unit_text)
    with tempfile.TemporaryDirectory(prefix="hw1-wifi-connect-budget-") as temporary:
        unit, binary = Path(temporary) / "test.cpp", Path(temporary) / "test"
        unit.write_text(unit_text)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-I", str(HERE / "web_batch_stubs"), str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=120)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
        require(result.returncode == 0, f"wifi connect budget failed:\n{result.stdout}\n{result.stderr}")
        count = re.search(r"(?m)^checks=(\d+)$", result.stderr)
        require(count is not None, "missing completed check count")
        print(f"{result.stdout.strip()} ({count[1]} checks)", flush=True)


if __name__ == "__main__":
    main()
